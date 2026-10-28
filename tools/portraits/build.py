"""Portraits from the captures of tools/portraits/capture.py -> mods/portraits-2026/

    python3 tools/portraits/build.py <your MVP Baseball 2005 ISO> RAWDIR [out = mods/portraits-2026] [--gallery DIR]

For every player captured (RAWDIR/<key>_0..2.png, the Edit Player close-up at 4x on a magenta backdrop):
  - the picture where his eyes are open: of the three, the one closest to the other two (a blink is the odd one out);
  - cut out from the backdrop (alpha from the magenta, colours freed of its spill) and framed like the game's
    portraits: cap to collar, the head filling the width;
  - a 128 x 128 portrait in the game's own format (type 14, as in PORTRAIT.BIG), added to PORTRAIT.BIG as
    "<n>.ssh" (n from 6000: numbers the disc does not use);
  - its 512 x 512 version, named the way the runtime names that 128 x 128 texture (textures/replace/128x128_<hash>.png),
    so with HD textures on, the cards show the full-detail picture;
  - portraits.txt: "<player key> <n>", which the runtime reads to show player <key> picture <n>.
--gallery also writes each portrait on a studio gradient (PNG, 512 x 512) to look at.
Numbers already given (an earlier portraits.txt in OUT) are kept, so a rebuild with more players changes nothing else.
"""
import sys, os, glob, struct
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import numpy as np
from PIL import Image
import isofile, refpack, ssh
from bigfile import read_big

args = [a for a in sys.argv[1:] if not a.startswith('--')]
ISO, RAW = args[0], args[1]
OUT = args[2] if len(args) > 2 else 'mods/portraits-2026'
GALLERY = sys.argv[sys.argv.index('--gallery') + 1] if '--gallery' in sys.argv else None
PANEL_X = 0.343            # the Edit Player panel starts here (fraction of the width): nothing right of it is the player
TOP_Y = 0.17               # above: the screen's title

def matte(im):
    """(H, W, 3) float -> alpha 0..1 (magenta backdrop = 0) and the colours without the backdrop's spill"""
    r, g, b = im[..., 0], im[..., 1], im[..., 2]
    m = np.minimum(r, b) - g                                     # magenta 255, skin and cloth well below 60
    a = np.clip((200.0 - m) / 140.0, 0.0, 1.0)
    H, W = a.shape
    if W > 1500: a[:, int(W * PANEL_X):] = 0; a[:int(H * TOP_Y), :] = 0          # (a whole screen; the janitor's cut-outs are already only the player's side)
    key = np.array([255.0, 0.0, 255.0])
    fg = (im - (1 - a[..., None]) * key) / np.maximum(a[..., None], 1e-3)
    fg = np.where(a[..., None] > 0.02, np.clip(fg, 0, 255), 0)
    return a, fg

def frame(a):
    """square (x0, y0, size) around the head, in pixels of the capture"""
    Wf = a.shape[1] if a.shape[1] > 1500 else a.shape[1] / PANEL_X             # width of the whole screen
    rows = np.where((a > 0.5).sum(1) >= max(4, int(Wf) // 400))[0]
    top = rows[0]
    band = a[top:top + int(Wf * 0.122)] > 0.5                    # cap and face, above the shoulders
    cols = np.where(band.any(0))[0]
    x0, x1 = cols[0], cols[-1]
    size = (x1 - x0) / 0.64
    cx = (x0 + x1) / 2 - 0.04 * size                              # the brim points right: the face is a bit left of the middle
    return cx - size / 2, top - 0.05 * size, size

def crop(img, x0, y0, size, out):
    """crop a square (may reach outside: transparent) and resize to out x out, premultiplied for the filter"""
    H, W = img.shape[:2]
    pad = int(size) + 4
    big = np.zeros((H + 2 * pad, W + 2 * pad, img.shape[2]), np.float32); big[pad:pad + H, pad:pad + W] = img
    X0, Y0 = int(round(x0)) + pad, int(round(y0)) + pad; S = int(round(size))
    sq = big[Y0:Y0 + S, X0:X0 + S]
    ch = [Image.fromarray(sq[..., k].astype(np.float32), 'F').resize((out, out), Image.LANCZOS) for k in range(sq.shape[2])]
    return np.stack([np.asarray(c) for c in ch], -1)

def grade(rgb, n):
    """a photographer's finish: a gentle S-curve (more depth than the flat menu lighting), a touch of warmth in the
    mid-tones, and an unsharp mask scaled to the size"""
    from PIL import ImageFilter
    x = rgb / 255.0
    x = np.clip(x + 0.18 * x * (1 - x) * (2 * x - 1) * -1.0 * -1.0, 0, 1)      # S-curve around the middle
    x = np.clip(x * np.array([1.025, 1.0, 0.975]), 0, 1)                    # warmth
    im = Image.fromarray((x * 255).astype(np.uint8))
    im = im.filter(ImageFilter.UnsharpMask(radius=max(1, n // 256), percent=70 if n > 128 else 35, threshold=2))
    return np.asarray(im).astype(np.float32)

def portrait(paths):
    # captures on the magenta backdrop (RGB), or renders with their own alpha (tools/portraits/render_portraits.py)
    raw = [np.asarray(Image.open(p).convert('RGBA')).astype(np.float32) for p in paths]
    rendered = all(r[..., 3].min() < 255 for r in raw)
    shots = [r[..., :3] for r in raw]
    if rendered:
        a = raw[0][..., 3] / 255.0; fg = shots[0]
        x0, y0, S = frame(a)
        pm = np.concatenate([fg * a[..., None], a[..., None]], -1)
        def at_r(n):
            c = crop(pm, x0, y0, S, n); al = np.clip(c[..., 3], 0, 1)
            rgb = np.clip(c[..., :3] / np.maximum(al[..., None], 1e-3), 0, 255) * (al[..., None] > 0.004)
            return grade(rgb, n), al
        return at_r(512), at_r(128), 0
    # eyes open: the shot closest to the others (each shot framed on its own, compared small)
    def small(sh):
        al, _ = matte(sh); x0, y0, S = frame(al)
        return crop(sh, x0, y0, S, 96)
    if len(shots) >= 3:
        sm = [small(s) for s in shots]
        best = int(np.argmin([sum(np.abs(s - t).mean() for t in sm) for s in sm]))
    else: best = 0
    a, fg = matte(shots[best]); x0, y0, S = frame(a)
    pm = np.concatenate([fg * a[..., None], a[..., None]], -1)    # premultiplied
    def at(n):
        c = crop(pm, x0, y0, S, n); al = np.clip(c[..., 3], 0, 1)
        rgb = np.clip(c[..., :3] / np.maximum(al[..., None], 1e-3), 0, 255) * (al[..., None] > 0.004)
        return grade(rgb, n), al
    return at(512), at(128), best

def tex_hash(idx, pal_file):
    """the runtime's name for the 128 x 128 texture: FNV-1a-like over the decoded pixels (gs.cpp, tex_get)"""
    M = (1 << 64) - 1
    def mix64(h, v):
        h ^= (v + 0x9e3779b97f4a7c15 + ((h << 6) & M) + (h >> 2)) & M
        return (h * 0xff51afd7ed558ccd) & M
    lut = [pal_file[ssh.csm1(i)] for i in range(256)]
    words = np.array([c[0] | c[1] << 8 | c[2] << 16 | c[3] << 24 for c in lut], np.uint64)[idx.ravel()]
    h = mix64(idx.shape[1], idx.shape[0])
    P = 0x100000001b3
    for w in words.tolist(): h = ((h ^ w) * P) & M
    return h

def main():
    big = isofile.read_file(ISO, 'DATA/FRONTEND/PORTRAIT.BIG')
    ents = {n.lower(): (o, s) for n, o, s in read_big(big)}
    o, s = ents['0145.ssh']; template = refpack.decompress(big[o:o + s])
    used = {int(n[:-4]) for n in ents if n[:-4].isdigit()}
    os.makedirs(os.path.join(OUT, 'DATA/FRONTEND/PORTRAIT.BIG'), exist_ok=True)
    os.makedirs(os.path.join(OUT, 'textures/replace'), exist_ok=True)
    for f in glob.glob(os.path.join(OUT, 'textures/replace', '*.png')): os.remove(f)     # made again below
    if GALLERY: os.makedirs(GALLERY, exist_ok=True)
    ids = {}
    mp = os.path.join(OUT, 'portraits.txt')
    if os.path.exists(mp):
        for line in open(mp):
            p = line.split()
            if len(p) >= 2 and not line.startswith('#'): ids[p[0]] = int(p[1])
    nxt = max([6000] + [v + 1 for v in ids.values()])
    keys = sorted({os.path.basename(f).split('_')[0] for f in glob.glob(os.path.join(RAW, '*_0.png'))})
    for n, key in enumerate(keys):
        paths = sorted(glob.glob(os.path.join(RAW, key + '_[0-9].png')))
        try: (hd, hda), (lo, loa), best = portrait(paths)
        except Exception as e: print('%s: %s' % (key, e)); continue
        if key not in ids:
            while nxt in used: nxt += 1
            ids[key] = nxt; nxt += 1
        pid = ids[key]
        rgba = np.concatenate([lo, np.round(loa * 128)[..., None]], -1).clip(0, 255).astype(np.uint8)
        data, pal = ssh.image14_photo(rgba)
        f = ssh.Ssh(template); sh = f.shapes[0]
        sh.set_pixel_bytes(data); sh.set_palette(pal); sh.rename('%04d' % pid)
        out = f.build()
        open(os.path.join(OUT, 'DATA/FRONTEND/PORTRAIT.BIG', '%04d.ssh' % pid), 'wb').write(refpack.compress(out))
        h = tex_hash(ssh.decode14(data, 128, 128), pal)
        hdimg = np.concatenate([hd, hda[..., None] * 255], -1).clip(0, 255).astype(np.uint8)
        name = '128x128_%016x.png' % h
        Image.fromarray(hdimg, 'RGBA').save(os.path.join(OUT, 'textures/replace', name))
        if GALLERY:
            yy = np.linspace(0, 1, 512)[:, None, None]
            bg = (np.array([62, 70, 84]) * (1 - yy) + np.array([28, 31, 38]) * yy) * np.ones((512, 512, 3))
            Image.fromarray((hd * hda[..., None] + bg * (1 - hda[..., None])).astype(np.uint8)).save(os.path.join(GALLERY, key + '.png'))
        print('%s -> %04d (shot %d) %s' % (key, pid, best, name), flush=True)
    with open(mp, 'w') as fo:
        fo.write('# player key (hex) -> portrait number (PORTRAIT.BIG <n>.ssh), made by tools/portraits/build.py\n')
        for k in sorted(ids, key=lambda k: ids[k]): fo.write('%s %d\n' % (k, ids[k]))
    print('%d portraits in %s' % (len(ids), OUT))

if __name__ == '__main__': main()
