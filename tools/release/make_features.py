#!/usr/bin/env python3
"""Packs the launcher's features (tools/release/features.json) into release assets.

  python3 tools/release/make_features.py OUT_DIR [--only id,id] [--part-size 1900] [--base-url URL]
                                          [--upload OWNER/REPO --tag TAG] [--keep]

For every feature it writes OUT_DIR/<id>.partN.tar (plain tar: the data is already compressed, and every OS's own
tar unpacks it), each under --part-size MB so they fit GitHub's 2 GB limit per release asset, and OUT_DIR/manifest.json
with each part's size and SHA-256. The archives hold paths relative to the game folder (mods/rosters-2026/...,
textures/hd/...). previews/ (OUT_DIR/previews/*.jpg) go along as they are.

--upload sends each part to the GitHub release TAG of OWNER/REPO as soon as it is made and deletes it locally
(unless --keep), so the 42 GB HD pack never needs 42 GB of free space twice. It uses the GitHub CLI (gh) or
GITHUB_TOKEN with curl.

Without --upload the folder works as an offline install: copy it next to the game as features/ (or point
MVP_FEATURES_DIR at it) and the launcher installs from it.
"""
import argparse, glob, hashlib, io, json, os, subprocess, sys, tarfile, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))


class HashWriter(io.RawIOBase):
    def __init__(self, f): self.f, self.h, self.n = f, hashlib.sha256(), 0
    def writable(self): return True
    def write(self, b): self.f.write(b); self.h.update(b); self.n += len(b); return len(b)


def walk(src):
    out = []
    for d, dirs, files in os.walk(src):
        dirs.sort()
        for f in sorted(files):
            if f.startswith('.') or f.endswith('.tmp.png'): continue
            p = os.path.join(d, f)
            out.append((p, os.path.getsize(p)))
    return out


def plan_parts(entries, limit):
    """entries: [(src_path, arcname, size)] -> list of lists, each under limit bytes of tar"""
    parts, cur, cur_n = [], [], 0
    for e in entries:
        need = e[2] + 1024 + 512
        if cur and cur_n + need > limit: parts.append(cur); cur, cur_n = [], 0
        cur.append(e); cur_n += need
    if cur: parts.append(cur)
    return parts


def write_tar(path, entries, extra=None):
    with open(path, 'wb') as raw:
        hw = HashWriter(raw)
        with tarfile.open(fileobj=hw, mode='w|', format=tarfile.PAX_FORMAT) as t:
            for src, arc, _ in entries:
                ti = tarfile.TarInfo(arc); ti.size = os.path.getsize(src); ti.mtime = 1700000000; ti.mode = 0o644
                with open(src, 'rb') as f: t.addfile(ti, f)
            for arc, data in (extra or []):
                ti = tarfile.TarInfo(arc); ti.size = len(data); ti.mtime = 1700000000; ti.mode = 0o644
                t.addfile(ti, io.BytesIO(data))
    return hw.n, hw.h.hexdigest()


def upload(repo, tag, path):
    name = os.path.basename(path)
    for attempt in range(60):                                   # up to ~2 hours of retries: rides out a dropped connection
        r = subprocess.run(['gh', 'release', 'upload', tag, path, '--repo', repo, '--clobber'])
        if r.returncode == 0: return
        print('  upload of %s failed (attempt %d), retrying in 2 min' % (name, attempt + 1), flush=True); time.sleep(120)
    sys.exit('could not upload %s' % name)


def expand(spec):
    feats = []
    for f in spec['features']:
        if not f.get('eras'):
            feats.append(f); continue
        dirs = sorted(set(glob.glob(os.path.join(ROOT, f['glob'])) + glob.glob(os.path.join(ROOT, f.get('glob2', f['glob'])))))
        for d in dirs:
            try: pj = json.load(open(os.path.join(d, 'pack.json')))
            except Exception: continue
            if not pj.get('era'): continue
            season = pj.get('season', 0)
            name = pj.get('name') or os.path.basename(d)
            feats.append({'id': 'era-%s' % season, 'name': name, 'group': f['group'], 'default': False,
                          'description': pj.get('blurb') or pj.get('description', ''),
                          'paths': [os.path.relpath(d, ROOT)], 'era': True, 'season': season})
    return feats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--only', default='')
    ap.add_argument('--part-size', type=int, default=1900, help='MB per part')
    ap.add_argument('--base-url', default='')
    ap.add_argument('--upload', default='', help='OWNER/REPO')
    ap.add_argument('--tag', default='features')
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--skip-done', action='store_true', help='resume: parts listed in OUT/uploaded.txt are not made or sent again')
    ap.add_argument('--version', default='1')
    ap.add_argument('--max-files', type=int, default=0, help='testing: at most this many files per feature')
    ap.add_argument('--plan-only', action='store_true', help='write the manifest with estimated sizes, make no archives (UI tests)')
    a = ap.parse_args()
    spec = json.load(open(os.path.join(os.path.dirname(__file__), 'features.json')))
    os.makedirs(a.out, exist_ok=True)
    man_path = os.path.join(a.out, 'manifest.json')
    old = {}
    if os.path.exists(man_path):
        for f in json.load(open(man_path)).get('features', []): old[f['id']] = f
    only = set(x for x in a.only.split(',') if x)
    if a.upload:                                                 # previews first (a release has no folders: previews/x.png -> preview-x.png)
        for p in glob.glob(os.path.join(a.out, 'previews', '*')):
            q = os.path.join(a.out, 'preview-' + os.path.basename(p))
            with open(p, 'rb') as i, open(q, 'wb') as o: o.write(i.read())
            upload(a.upload, a.tag, q); os.remove(q)
    out = []
    feats = expand(spec)
    feats.sort(key=lambda f: f['id'].startswith('hd-'))         # the big HD packs last: everything else is available sooner
    for f in feats:
        fid = f['id']
        if only and fid not in only:
            if fid in old: out.append(old[fid])
            continue
        entries = []
        if 'dir' in f:                                          # a folder packed under another name (textures/replace -> textures/hd)
            src = os.path.join(ROOT, f['dir'])
            if not os.path.isdir(src): print('skip %s: %s missing' % (fid, f['dir'])); continue
            for p, n in walk(src): entries.append((p, f['dest'] + '/' + os.path.relpath(p, src).replace(os.sep, '/'), n))
        else:
            missing = [p for p in f['paths'] if not os.path.exists(os.path.join(ROOT, p))]
            if missing: print('skip %s: %s missing' % (fid, ', '.join(missing))); continue
            for rel in f['paths']:
                src = os.path.join(ROOT, rel)
                if os.path.isfile(src): entries.append((src, rel, os.path.getsize(src)))
                else:
                    for p, n in walk(src): entries.append((p, os.path.relpath(p, ROOT).replace(os.sep, '/'), n))
        if a.max_files: entries = entries[:a.max_files]
        install = sum(e[2] for e in entries)
        parts = plan_parts(entries, a.part_size * 1000000)
        print('%s: %d files, %.2f GB, %d part(s)' % (fid, len(entries), install / 1e9, len(parts)))
        rec = {k: v for k, v in f.items() if k not in ('dir', 'dest', 'marker', 'glob', 'glob2', 'description_from')}
        rec.update({'version': a.version, 'install_size': install, 'files': len(entries), 'parts': []})
        for i, pe in enumerate(parts):
            name = '%s.part%d.tar' % (fid, i + 1) if len(parts) > 1 else '%s.tar' % fid
            path = os.path.join(a.out, name)
            extra = [(f['dest'] + '/' + f['marker'], b'%s\n' % fid.encode())] if f.get('marker') and i == 0 else None
            t0 = time.time()
            if a.plan_only:
                rec['parts'].append({'file': name, 'size': sum(e[2] + 1536 for e in pe), 'sha256': '', 'files': len(pe)}); continue
            done = {}
            upl = os.path.join(a.out, 'uploaded.txt')
            if a.skip_done and os.path.exists(upl):
                for line in open(upl):
                    f2 = line.split()
                    if len(f2) == 3: done[f2[0]] = (int(f2[1]), f2[2])
            if name in done:
                size, sha = done[name]
                print('  %s  already uploaded' % name)
                rec['parts'].append({'file': name, 'size': size, 'sha256': sha, 'files': len(pe)})
                continue
            size, sha = write_tar(path, pe, extra)
            print('  %s  %.0f MB  %.0f s' % (name, size / 1e6, time.time() - t0))
            rec['parts'].append({'file': name, 'size': size, 'sha256': sha, 'files': len(pe)})
            if a.upload:
                upload(a.upload, a.tag, path)
                if not a.keep: os.remove(path)
                with open(os.path.join(a.out, 'uploaded.txt'), 'a') as u: u.write('%s %d %s\n' % (name, size, sha))
        out.append(rec)
        man = {'format': 1, 'name': 'MVP Baseball 2005 Recomp features', 'base_url': a.base_url, 'features': out}
        json.dump(man, open(man_path, 'w'), indent=1)          # after every feature: a stopped run keeps what it made
        if a.upload: upload(a.upload, a.tag, man_path)        # players see each feature as soon as it is complete
    man = {'format': 1, 'name': 'MVP Baseball 2005 Recomp features', 'base_url': a.base_url, 'features': out}
    json.dump(man, open(man_path, 'w'), indent=1)
    if a.upload:
        upload(a.upload, a.tag, man_path)
    print('wrote %s (%d features)' % (man_path, len(out)))


if __name__ == '__main__':
    main()
