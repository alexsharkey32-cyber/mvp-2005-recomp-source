#!/usr/bin/env python3
"""The 2x HD pack from the 4x one: every texture in SRC (textures/replace) halved (Lanczos, on premultiplied alpha so
cut-out edges stay clean) into DST (build/hd-2x), same names. Resumable: textures already in DST are skipped.

  python3 tools/release/make_hd_2x.py [SRC] [DST] [--jobs N]
"""
import os, sys, time
from multiprocessing import Pool
from PIL import Image
import numpy as np

def half(args):
    src, dst = args
    try:
        im = Image.open(src); im.load()
        w, h = im.size
        nw, nh = max(1, w // 2), max(1, h // 2)
        if im.mode in ('RGBA', 'LA', 'P'):
            a = np.asarray(im.convert('RGBA'), dtype=np.float32) / 255.0
            pm = a.copy(); pm[..., :3] *= pm[..., 3:4]
            r = np.stack([np.asarray(Image.fromarray(pm[..., c]).resize((nw, nh), Image.LANCZOS)) for c in range(4)], -1)
            r = np.clip(r, 0, 1)
            al = r[..., 3:4]
            rgb = np.where(al > 1e-4, r[..., :3] / np.maximum(al, 1e-4), 0)
            out = np.concatenate([np.clip(rgb, 0, 1), al], -1)
            Image.fromarray((out * 255 + 0.5).astype(np.uint8), 'RGBA').save(dst + '.tmp.png', compress_level=6)
        else:
            im.convert('RGB').resize((nw, nh), Image.LANCZOS).save(dst + '.tmp.png', compress_level=6)
        os.replace(dst + '.tmp.png', dst)
        return 1
    except Exception as e:
        sys.stderr.write('%s: %s\n' % (src, e)); return 0

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    src = args[0] if args else 'textures/replace'
    dst = args[1] if len(args) > 1 else 'build/hd-2x'
    jobs = int(sys.argv[sys.argv.index('--jobs') + 1]) if '--jobs' in sys.argv else max(1, os.cpu_count() - 1)
    os.makedirs(dst, exist_ok=True)
    have = set(os.listdir(dst))
    todo = [(os.path.join(src, f), os.path.join(dst, f)) for f in sorted(os.listdir(src)) if f.endswith('.png') and not f.startswith('.') and f not in have]
    print('%d to do (%d done before), %d processes' % (len(todo), len(have), jobs), flush=True)
    t0 = time.time(); n = 0
    with Pool(jobs) as p:
        for ok in p.imap_unordered(half, todo, chunksize=64):
            n += 1
            if n % 2000 == 0:
                el = time.time() - t0
                print('%d of %d, about %.0f min left' % (n, len(todo), el / n * (len(todo) - n) / 60), flush=True)
    print('done: %d in %.0f min' % (n, (time.time() - t0) / 60), flush=True)

if __name__ == '__main__':
    main()
