"""Builds the HD texture pack from the textures tools/hd/extract_textures.py found on the disc.

    python3 tools/hd/build_pack.py <work dir> <models dir> <out dir (textures/replace)> [options]
      --tiers 1-3      which tiers to do (default 1-4; see extract_textures.py)
      --only cat,cat   only these categories (uniform, equipment, lettering, head, face, ballpark, menu, portrait)
      --quick          use the fast model everywhere (about 15x faster than the best one, a little softer)
      --redo cat,cat   also redo textures of these categories that are already in the out dir
      --seconds n      stop after n seconds (run it again to continue: finished textures are skipped)
      --part i/n       every n-th texture starting at i (several processes or machines at once)
      --order file     do the textures named in this file first, in its order (--order-only: nothing else)
      --dumps dir,dir  also do the textures in these dump folders that the archives do not have (fonts, menu pieces)
      --list           only print what would be done
      --repair-only    only bring finished textures up to date (see REV below); no model needed
      --prune          only delete textures made from predictions that the current index no longer contains

Models (.pth, from the Real-ESRGAN project) are converted to ONNX on first use when onnxruntime is installed
(`pip install onnxruntime`), which is 3-4x faster than the NumPy fallback.
Each texture is upscaled 4x and then given back its original colour and grain (tools/hd/hdpack.py).
"""
import sys, os, time, zlib
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hdpack

# Revision of the processing, recorded per texture in done.txt. Textures made by an older revision are brought up
# to date, without the model wherever that is possible:
#   3  colours taken from the original only (hdpack.chroma_transfer): repaired on the finished picture
#   4  wrapped padding only in the directions in which a texture repeats (a wall with a line along its foot got that
#      line along its top as well): ballpark and menu textures that were padded wrongly are made again
#   5  grain for everything that is not menu art (smooth photographs - faces, cloth, mown grass, painted walls -
#      were taken for flat artwork and came out without it): repaired on the finished picture (hdpack.fix_grain)
# Menu art and dumps from before revision 5 are made again (2: letters with shadows, font pages with a filler
# between the cells; 5: brightness held to the original's range - the anime model drew a white line where a wall
# texture had a slightly lighter top row).
REV = 5
REDO_BELOW = {'menu': 5, 'dump': 5}        # menu art and dumps: made again (model output is clamped since 5)
SEAMS = ('ballpark', 'menu', 'dump')                    # categories revision 4 can affect
NO_GRAIN = ('menu', 'dump', 'lettering')                # categories revision 5 does not affect

def main():
    a = sys.argv[1:]
    def opt(name, default=None, flag=False):
        if name in a:
            i = a.index(name); a.pop(i)
            return True if flag else a.pop(i)
        return default
    tiers = opt('--tiers', '1-4'); only = opt('--only'); quick = opt('--quick', flag=True); redo = opt('--redo', '')
    seconds = float(opt('--seconds', '0')); part = opt('--part'); order = opt('--order'); order_only = opt('--order-only', flag=True); dumps = opt('--dumps'); listing = opt('--list', flag=True); repair_only = opt('--repair-only', flag=True); prune = opt('--prune', flag=True); threads = int(opt('--threads', '0'))
    work, models, out = a[0], a[1], a[2]
    lo, _, hi = tiers.partition('-'); lo = int(lo); hi = int(hi or lo)
    only = set(only.split(',')) if only else None; redo = set(redo.split(',')) if redo else set()
    os.makedirs(out, exist_ok=True)
    have = set(os.listdir(out))
    # done.txt: what this tool has written and with which model, so that a --quick pass can be followed by a full one
    # (which redoes only what the best model improves) and an interrupted --redo run continues where it stopped
    made_path = os.path.join(work, 'done.txt'); made = {}; rev = {}
    if os.path.exists(made_path):
        for l in open(made_path):
            p = l.split()
            if p: made[p[0]] = p[1] if len(p) > 1 else 'best'; rev[p[0]] = int(p[2][1:]) if len(p) > 2 else 1
    if prune:                                             # textures this tool made from a prediction that a later version
        known = set(l.split('\t', 1)[0] for l in open(os.path.join(work, 'index.tsv')))     # of extract_textures.py dropped
        stale = [n for n in have if n in made and made[n] != 'auto' and n not in known]    # ('auto' = from a dump: kept)
        for n in stale: os.remove(os.path.join(out, n))
        if stale: print('removed %d textures the game never asks for' % len(stale), flush=True)
        return
    first = {}
    if order:
        for i, l in enumerate(open(order)): first[l.strip()] = i
    jobs = []
    for l in open(os.path.join(work, 'index.tsv')):
        name, tier, model, tiling, grain, cat, src, rect = (l.rstrip('\n').split('\t') + ['-'])[:8]
        if not lo <= int(tier) <= hi or (only and cat not in only): continue
        if order_only and name not in first: continue
        if quick and model == 'best': model = 'photo'
        if name in have:
            if name in made and rev[name] >= REDO_BELOW.get(cat, 1) and not (made[name] == 'auto' and model != 'auto'):
                # (made from a dump before the archives were known to hold it - the field's grass and dirt, which
                # came out smooth without their grain: those are made again as what they are)
                if made[name] == model or made[name] == 'best' or model != 'best':
                    if rev[name] < REV: jobs.append((name, made[name], int(tiling), float(grain), cat, rect, True))
                    continue
            elif name not in made and cat not in redo: continue
        jobs.append((name, model, int(tiling), float(grain), cat, rect, False))
    if first: jobs.sort(key=lambda j: first.get(j[0], len(first)))
    # textures that are not in the archives as such (fonts, pieces of the menus the game assembles): from dumps
    # (Textures > Dump textures in the game writes textures/dump/). Video frames and look-up data are left out.
    srcs = {}
    known = set(l.split('\t', 1)[0] for l in open(os.path.join(work, 'index.tsv')))
    for d in (dumps.split(',') if dumps else []):
        for name in sorted(os.listdir(d)):
            if not name.endswith('.png') or name in known or name in srcs or (name in made and name in have and rev[name] >= REV): continue
            if name in have and name not in made and 'dump' not in redo: continue
            fix = name in made and name in have and rev[name] >= REDO_BELOW['dump']
            try: w, h = (int(x) for x in name.split('_')[0].split('x'))
            except ValueError: continue
            if w < 16 or h < 16 or (w == 16 and h >= 64) or w * h > 512 * 512: continue      # 16 x n: strips of video
            a = np.asarray(Image.open(os.path.join(d, name)).convert('RGBA')).astype(np.float32)
            rgb, al = a[..., :3], a[..., 3]
            if al.max() < 12: continue                                                    # nothing to see
            vis = al > 8
            dd = (np.abs(np.diff(rgb, axis=1)).mean() + np.abs(np.diff(rgb, axis=0)).mean()) / 2
            if dd / (np.abs(rgb - rgb.reshape(-1, 3).mean(0)).mean() + 1e-3) > 0.95 and vis.mean() > 0.9: continue   # noise: data, not a picture
            srcs[name] = os.path.join(d, name); jobs.append((name, 'auto', -1, 0.0, 'dump', '-', fix))
    tag = ''
    if part: i, n = (int(x) for x in part.split('/')); jobs = jobs[i::n]; tag = '[%d] ' % (i + 1)
    if repair_only: jobs = [j for j in jobs if j[6]]
    jobs.sort(key=lambda j: not j[6])                  # repairs first: quick, and they are what is on screen already
    nfix = sum(1 for j in jobs if j[6])
    units = sum(int(j[0].split('_')[0].split('x')[0]) * int(j[0].split('_')[0].split('x')[1]) for j in jobs if not j[6]) / 16384
    print(tag + '%d textures to do (%d x 128x128)%s' % (len(jobs) - nfix, units, ', %d finished ones to bring up to date' % nfix if nfix else ''), flush=True)
    if listing:
        for j in jobs: print(*j[:6], 'repair' if j[6] else '')
        return
    up = hdpack.Upscaler(models, threads=threads, cache_dir=work)
    t0 = time.time(); done = 0; done_units = 0.0; fixed = 0; tfix = 0.0; told = t0; remade = 0; skipped = 0
    for name, model, tiling, grain, cat, rect, fix in jobs:
        if seconds and time.time() - t0 > seconds: break
        was_fix = fix
        if time.time() - told > 60:                       # a sign of life once a minute
            told = time.time()
            if fixed < nfix: print('%sbringing finished textures up to date: %d of %d' % (tag, fixed, nfix), flush=True)
            elif done_units > 0:
                left = (units - done_units) * (told - t0 - tfix) / done_units
                print('%s%d of %d textures, about %s left' % (tag, done, len(jobs) - nfix, '%d min' % (left / 60 + 1) if left < 5400 else '%.1f h' % (left / 3600)), flush=True)
        def mark():
            nonlocal fixed, tfix
            with open(made_path, 'a') as mf: mf.write('%s %s r%d\n' % (name, model, REV))
            fixed += 1
            if fixed == nfix: tfix = time.time() - t0; print(tag + 'brought %d finished textures up to date in %.0f s (%d made again)' % (fixed, tfix, remade), flush=True)
        r0 = rev.get(name, 1)
        todo_seam = cat in SEAMS and r0 < 4; todo_grain = r0 < 5 and grain > 0 and cat not in NO_GRAIN
        if fix and r0 >= 3 and not todo_seam and not todo_grain: mark(); continue     # nothing changed for these
        try:
            img = np.asarray(Image.open(srcs.get(name) or os.path.join(work, 'src', name)).convert('RGBA'))
            full = None
            if rect != '-':                               # an image in the corner of an empty canvas: only the image is upscaled
                rw, rh = (int(x) for x in rect.split('x')); full = img.shape; img = img[:rh, :rw]
            kw = dict(tiling=bool(tiling) if cat == 'ballpark' else None if cat in ('menu', 'dump') else False,
                      crisp_alpha=True if cat == 'lettering' else 'auto' if cat in ('menu', 'dump') else False)
            if fix and todo_seam and hdpack.seam_risk(img, **kw):
                if repair_only: skipped += 1; continue      # needs the model: left for a normal run
                fix = False; remade += 1
            if fix:
                todo_grain = todo_grain and hdpack.lost_grain(img, kw['tiling'])
                if r0 >= 3 and not todo_grain: mark(); continue
                old = np.asarray(Image.open(os.path.join(out, name)).convert('RGBA'))
                if old.shape[0] < img.shape[0] * 4 or old.shape[1] < img.shape[1] * 4: raise ValueError('not a 4x texture')
                hi_img = old[:img.shape[0] * 4, :img.shape[1] * 4]
                if r0 < 3: hi_img = hdpack.fix_colour(img, hi_img, **kw)
                if todo_grain: hi_img = hdpack.fix_grain(img, hi_img, kw['tiling'], grain, zlib.crc32(name.encode()))
            else: hi_img = hdpack.process(up, img, model=model, grain=grain, seed=zlib.crc32(name.encode()), **kw)
            if full is not None: canvas = np.zeros((full[0] * 4, full[1] * 4, 4), np.uint8); canvas[:hi_img.shape[0], :hi_img.shape[1]] = hi_img; hi_img = canvas
            tmp = os.path.join(out, '.tmp_%d_%s' % (os.getpid(), name))
            opaque = bool((hi_img[..., 3] == 255).all())
            Image.fromarray(hi_img[..., :3] if opaque else hi_img, 'RGB' if opaque else 'RGBA').save(tmp, format='PNG', compress_level=4)
            os.replace(tmp, os.path.join(out, name))
            if was_fix: mark()
            else:
                with open(made_path, 'a') as mf: mf.write('%s %s r%d\n' % (name, model, REV))
                done += 1; done_units += max(1, img.shape[0] * img.shape[1]) / 16384
        except Exception as e:
            print(tag + 'failed', name, e, flush=True)
    dt = time.time() - t0 - tfix
    if len(jobs) > nfix: print(tag + 'done %d of %d textures in %.0f s (%.2f s per 128x128), %d left' % (done, len(jobs) - nfix, dt, dt / max(done_units, 1e-9), len(jobs) - nfix - done), flush=True)
    if nfix and fixed < nfix: print(tag + 'brought %d of %d finished textures up to date%s' % (fixed, nfix, ' (%d need the model: left for a normal run)' % skipped if skipped else ''), flush=True)

if __name__ == '__main__': main()
