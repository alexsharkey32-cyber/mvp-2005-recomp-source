"""Every player of a league a face of his own (tools/faces/facegen.py), put into the league's mod.

    python3 tools/faces/build_faces.py ISO MOD_DIR WORKDIR [--hd mlb|all|none] [--season YEAR] [--levels mlb,aaa|mlb|all]
    (in parallel: --part 0/2 and --part 1/2 at the same time, then --merge)

MOD_DIR is a league pack (mods/pack-1977) or the modern rosters (mods/rosters-2026): its DATA/DATABASE/
DATABASE.BIG is read and written. Players with one of the game's real 2005 heads (face < 900) keep it.
Everyone else wears one of the game's generic head textures, GHEAD.BIG g<head><haircolour><hairstyle><facial>
.ssh, chosen by four of his database values. Here every such player gets a combination of those four nobody
else has, and the texture of that name is replaced by his own face, made from his real skin tone, hair colour,
hair style, facial hair and age. The head model (head shape) and the hair style class (shaved / long / short /
braids, which can bring their own geometry) stay his; only the codes that pick nothing but the texture are
moved: hair colour, facial hair and the short styles among themselves. The menus' appearance values then show
those codes, not his looks. Made-up rookies of later seasons get the game's own generic faces.
With HD textures on, the faces are 2x (--hd-scale 4 for 4x; --hd mlb: big-leaguers only, the default; all; none):
MOD_DIR/textures/replace/128x256_<hash>.png, named the way the runtime names the 128 x 256 face texture.
Writes MOD_DIR/faces.txt (player key -> texture, seed).
"""
import sys, os, glob, datetime
import numpy as np
from PIL import Image
here = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, here); sys.path.insert(0, os.path.join(here, '..'))
import facegen, mvpdb, isofile, refpack, ssh
from bigfile import read_big

M = (1 << 64) - 1
def tex_hash(idx, pal_file):
    def mix64(h, v):
        h ^= (v + 0x9e3779b97f4a7c15 + ((h << 6) & M) + (h >> 2)) & M
        return (h * 0xff51afd7ed558ccd) & M
    lut = np.array([pal_file[ssh.csm1(i)] for i in range(256)], np.uint64)
    words = (lut[:, 0] | lut[:, 1] << 8 | lut[:, 2] << 16 | lut[:, 3] << 24)[idx.ravel()]
    h = mix64(idx.shape[1], idx.shape[0]); P = 0x100000001b3
    for w in words.tolist(): h = ((h ^ w) * P) & M
    return h

SHORT = [3, 4, 5, 6, 7, 8]
def style_class(s): return 'short' if s in SHORT else 'x%d' % s

def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    iso, mod, work = a[0], a[1], a[2]
    opt = lambda k, d=None: sys.argv[sys.argv.index(k) + 1] if k in sys.argv else d
    hd_mode = opt('--hd', 'mlb'); hd_scale = int(opt('--hd-scale', 2))
    yt = os.path.join(mod, 'year.txt')
    season = int(opt('--season') or (open(yt).read().split()[0] if os.path.exists(yt) else 2027))
    shift = season - 2005
    dbp = os.path.join(mod, 'DATA', 'DATABASE', 'DATABASE.BIG')
    T, order = mvpdb.read_database(dbp)
    at = T['attrib.dat']; ci = {c: i for i, c in enumerate(at.cols)}
    col = lambda n: ci['playerattrib_' + n]
    # big-leaguers: players on the clubs that are an organisation's major-league club
    org = T['org.dat']; mlb = {'%09x' % (int(v[org.col('org_team_mlb')]) & 0xfffffffff) for k, v in org.rows}
    level1 = {v[1] for k, v in T['roster.dat'].rows if v[0] in mlb}
    aaa = {'%09x' % (int(v[org.col('org_team_aaa')]) & 0xfffffffff) for k, v in org.rows}
    level2 = {v[1] for k, v in T['roster.dat'].rows if v[0] in aaa}
    levels = opt('--levels', 'mlb,aaa')                                       # who gets a face of his own (all: everyone)
    def wanted(key): return levels == 'all' or key in level1 or ('aaa' in levels and key in level2)
    players = []
    for key, v in at.rows:
        face = int(v[col('face')])
        if face < 900 or face > 915 or not wanted(key): continue
        try: bd = datetime.date(1947, 12, 31) + datetime.timedelta(days=int(v[col('birthday')])); age = 2005 - bd.year     # stored shifted
        except Exception: age = 27
        players.append(dict(key=key, row=v, h=face - 901, tone=int(v[col('skin_tone')]), hair=int(v[col('haircolour')]),
                            style=int(v[col('hairstyle')]), facial=int(v[col('facialhair')]), age=max(18, min(45, age)), mlb=key in level1))
    # unique (head, colour, style, facial): his own values where free, else the nearest free combination of his class
    taken = set(); assign = {}
    players.sort(key=lambda p: (not p['mlb'], p['key']))
    for p in players:
        cls = style_class(p['style'])
        styles = SHORT if cls == 'short' else [p['style']]
        heads = [p['h']] + [h for h in range(15) if h != p['h'] and facegen.HEAD_TONE[h] == facegen.HEAD_TONE[p['h']]] + \
                [h for h in range(15) if facegen.HEAD_TONE[h] != facegen.HEAD_TONE[p['h']]]
        want = (p['h'], p['hair'], p['style'], p['facial'])
        if want not in taken: slot = want
        else:
            slot = None
            for h in heads:
                for c in [p['hair']] + [x for x in range(7) if x != p['hair']]:
                    for s in [p['style']] + [x for x in styles if x != p['style']]:
                        for f in [p['facial']] + [x for x in range(8) if x != p['facial']]:
                            if (h, c, s, f) not in taken: slot = (h, c, s, f); break
                        if slot: break
                    if slot: break
                if slot: break
        if not slot: print('no free face for', p['key']); continue
        taken.add(slot); assign[p['key']] = slot
    print('%d players with generic heads, %d big-leaguers; %d faces to make' % (len(players), sum(p['mlb'] for p in players), len(assign)))
    part = opt('--part')                                                      # "k/n": this worker's share; merged by --merge
    gd = os.path.join(mod, 'DATA', 'GHEAD.BIG'); rd = os.path.join(mod, 'textures', 'replace')
    parts = sorted(glob.glob(os.path.join(mod, 'faces_part_*.txt')))
    if '--merge' in sys.argv or not part:
        if not part and not parts:                                             # a fresh run in one go: nothing half-made to keep
            for f in glob.glob(os.path.join(rd, '128x256_*.png')) + glob.glob(os.path.join(gd, '*.ssh')): os.remove(f)
        if not part and '--merge' not in sys.argv:
            run_part(iso, mod, work, players, assign, col, hd_mode, hd_scale, 0, 1)
        done = {}
        for f in glob.glob(os.path.join(mod, 'faces_part_*.txt')):
            for ln in open(f):
                q = ln.split()
                if len(q) == 4: done[q[0]] = ln.strip()
        log = []
        for p in players:
            if p['key'] not in assign or p['key'] not in done: continue
            h, c, s, f = assign[p['key']]; row = p['row']
            row[col('face')] = str(901 + h); row[col('haircolour')] = str(c); row[col('hairstyle')] = str(s); row[col('facialhair')] = str(f)
            log.append(done[p['key']])
        mvpdb.write_database(dbp, T, order)
        with open(os.path.join(mod, 'faces_params.txt'), 'w') as fo:          # his real looks (the codes above were moved)
            fo.write('# player key, skin tone, hair colour, hair style, facial hair, age (for tools/faces/photo_faces.py)\n')
            for p in players:
                if p['key'] in assign and p['key'] in done: fo.write('%s %d %d %d %d %.0f\n' % (p['key'], p['tone'], p['hair'], p['style'], p['facial'], p['age']))
        open(os.path.join(mod, 'faces.txt'), 'w').write('# player key, face texture (GHEAD.BIG), seed, HD texture (tools/faces/build_faces.py)\n' + '\n'.join(log) + '\n')
        for f in glob.glob(os.path.join(mod, 'faces_part_*.txt')): os.remove(f)
        print('%d faces -> %s' % (len(log), mod)); return
    k, n = map(int, part.split('/'))
    run_part(iso, mod, work, players, assign, col, hd_mode, hd_scale, k, n)

def run_part(iso, mod, work, players, assign, col, hd_mode, hd_scale, k, n_parts):
    """makes the faces of every n-th player (from k), appending "key texture seed hd" to faces_part_<k>.txt;
    a restarted worker skips the ones its file already lists"""
    g = isofile.read_file(iso, 'DATA/GHEAD.BIG'); ge = {n: (o, s) for n, o, s in read_big(g)}
    src = facegen.load(work); src_hd = facegen.load(work, hd=(2 if hd_scale == 2 else True)) if hd_mode != 'none' else None
    gd = os.path.join(mod, 'DATA', 'GHEAD.BIG'); os.makedirs(gd, exist_ok=True)
    rd = os.path.join(mod, 'textures', 'replace'); os.makedirs(rd, exist_ok=True)
    pf = os.path.join(mod, 'faces_part_%d.txt' % k)
    have = {ln.split()[0] for ln in open(pf)} if os.path.exists(pf) else set()
    out = open(pf, 'a')
    for n, p in enumerate(players):
        if p['key'] not in assign or n % n_parts != k or p['key'] in have: continue
        h, c, s, f = assign[p['key']]
        seed = int(p['key'], 16) & 0x7fffffff
        params = dict(tone=p['tone'], hair=p['hair'], style=p['style'], facial=p['facial'], age=p['age'], head=h)
        lo, _ = facegen.make_face(src, params, seed)
        name = 'g%s%s%s%s.ssh' % (chr(97 + h), chr(97 + c), chr(97 + s), chr(97 + f))
        o, sz = ge[name]; cdat = g[o:o + sz]
        cdat = refpack.decompress(cdat) if cdat[:2] in (b'\x10\xfb', b'\x11\xfb', b'\x90\xfb', b'\x91\xfb') else cdat
        fsh = ssh.Ssh(cdat); sh = fsh.shapes[0]
        rgba = lo.copy(); rgba[..., 3] = np.round(lo[..., 3].astype(np.float32) * 128 / 255).astype(np.uint8)
        data, pal = ssh.image14_photo(rgba)
        sh.set_pixel_bytes(data); sh.set_palette(pal)
        open(os.path.join(gd, name), 'wb').write(refpack.compress(fsh.build()))
        hd_name = ''
        if src_hd is not None and (hd_mode == 'all' or p['mlb']):
            hi, _ = facegen.make_face(src_hd, params, seed)
            if hi.shape[1] != 128 * hd_scale:                                     # 2x by default (made from 2x pieces): a quarter of the disk space
                import cv2
                hi = cv2.resize(hi, (128 * hd_scale, 256 * hd_scale), interpolation=cv2.INTER_AREA)
            hsh = tex_hash(ssh.decode14(data, 128, 256), pal)
            hd_name = '128x256_%016x.png' % hsh
            Image.fromarray(hi, 'RGBA').save(os.path.join(rd, hd_name), compress_level=4)
        out.write('%s %s %d %s\n' % (p['key'], name, seed, hd_name or '-')); out.flush()
        if n % 200 == 0: print(n, flush=True)
    out.close()

if __name__ == '__main__': main()
