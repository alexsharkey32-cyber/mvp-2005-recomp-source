"""Portraits drawn by our own renderer from the game's geometry.

The game draws the player in the Edit Player close-up; the runtime records every textured triangle of one such
frame (".once PRIMS dir": dir/prims.txt with screen position, depth, vertex colour and texture coordinates,
and each texture as dir/<hash>.png). This draws those triangles again, at any resolution, with:
  - other textures in place of the game's (the face from tools/faces/facegen.py at 512 x 1024, HD uniforms);
  - perspective-correct, filtered (bilinear, supersampled) texturing instead of the PS2's;
  - its own light: normals are rebuilt from the triangles' screen position and depth, and a soft key light,
    a fill and a rim light are added to the game's own (baked into the vertex colours);
  - a studio backdrop, then the same framing as the captured portraits (tools/portraits/build.py).

    python3 tools/portraits/render.py PRIMSDIR OUT.png [--scale 4] [--tex HASH=file.png ...] [--crop x0,y0,x1,y1]
"""
import sys, os, numpy as np
from PIL import Image

def load_prims(d):
    tris, cur = [], None
    for ln in open(os.path.join(d, 'prims.txt')):
        p = ln.split()
        if p[0] == 'T':
            kv = dict(x.split('=') for x in p[4:])
            cur = dict(hash=p[1], w=int(p[2]), h=int(p[3]), tfx=int(kv['tfx']), abe=int(kv['abe']), zmsk=int(kv['zmsk']), v=[])
            tris.append(cur)
        elif p[0] == 'V':
            cur['v'].append([float(x) for x in p[1:]])
    return [t for t in tris if len(t['v']) == 3]

def sample(tex, u, v):
    """bilinear, clamped; tex float (h, w, 4); u, v in 0..1"""
    h, w = tex.shape[:2]
    x = np.clip(u * w - 0.5, 0, w - 1.001); y = np.clip(v * h - 0.5, 0, h - 1.001)
    x0 = x.astype(int); y0 = y.astype(int); fx = (x - x0)[..., None]; fy = (y - y0)[..., None]
    a = tex[y0, x0]; b = tex[y0, x0 + 1]; c = tex[y0 + 1, x0]; d = tex[y0 + 1, x0 + 1]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy

_libc = []
def _lib():
    if not _libc:
        import ctypes
        p = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'csrc', 'libraster.so')
        try: _libc.append(ctypes.CDLL(p) if os.path.exists(p) and not os.environ.get('MVP_PY_RASTER') else None)
        except OSError: _libc.append(None)
    return _libc[0]

def render(d, scale=4, override=None, crop=None, light=True, bg=None, straight=False):
    tris = [t for t in load_prims(d) if t['tfx'] != 0 and t['hash'] != '0000000000000000']      # the 3D models only
    texs = {}
    def tex(hsh):
        if hsh not in texs:
            p = (override or {}).get(hsh)
            if isinstance(p, np.ndarray): texs[hsh] = p                              # a picture given as floats 0..1 (RGBA)
            else: texs[hsh] = np.asarray(Image.open(p or os.path.join(d, hsh + '.png')).convert('RGBA')).astype(np.float32) / 255
        return texs[hsh]
    V = np.array([t['v'] for t in tris], np.float32)                   # (n, 3, 10): x y z r g b a s/q t/q q
    x0, y0, x1, y1 = crop or (V[..., 0].min(), V[..., 1].min(), V[..., 0].max() + 1, V[..., 1].max() + 1)
    W, H = int((x1 - x0) * scale), int((y1 - y0) * scale)
    col = np.zeros((H, W, 3), np.float32); zb = np.full((H, W), -1.0, np.float32); cov = np.zeros((H, W), np.float32)
    nrm = np.zeros((H, W, 3), np.float32)
    zs = V[..., 2]; zlo, zhi = np.percentile(zs, 1), np.percentile(zs, 99)
    lib = _lib()
    if lib is not None:                                                   # the same loop in C (csrc/raster.c), ~50x faster
        import ctypes as C
        hs = sorted({t['hash'] for t in tris}); idx = {h: i for i, h in enumerate(hs)}
        arrs = [np.ascontiguousarray(tex(h), np.float32) for h in hs]
        TP = (C.POINTER(C.c_float) * len(arrs))(*[a.ctypes.data_as(C.POINTER(C.c_float)) for a in arrs])
        i32 = lambda x: np.ascontiguousarray(np.array(x, np.int32))
        tid = i32([idx[t['hash']] for t in tris]); tfx = i32([t['tfx'] for t in tris]); abe = i32([t['abe'] for t in tris])
        tw = i32([a.shape[1] for a in arrs]); th = i32([a.shape[0] for a in arrs])
        Vc = np.ascontiguousarray(V, np.float32)
        fp = lambda a: a.ctypes.data_as(C.POINTER(C.c_float)); ip = lambda a: a.ctypes.data_as(C.POINTER(C.c_int))
        lib.raster(len(tris), fp(Vc), ip(tid), ip(tfx), ip(abe), TP, ip(tw), ip(th), W, H, C.c_float(scale), C.c_float(x0), C.c_float(y0),
                   C.c_float(zlo), C.c_float(zhi), fp(col), fp(zb), fp(cov), fp(nrm))
        tris = []                                                         # done
    for t, v in zip(tris, V):
        P = (v[:, :2] - [x0, y0]) * scale
        bx0, by0 = np.floor(P.min(0)).astype(int); bx1, by1 = np.ceil(P.max(0)).astype(int) + 1
        bx0, by0 = max(bx0, 0), max(by0, 0); bx1, by1 = min(bx1, W), min(by1, H)
        if bx1 <= bx0 or by1 <= by0: continue
        yy, xx = np.mgrid[by0:by1, bx0:bx1].astype(np.float32) + 0.5
        (ax, ay), (bxp, byp), (cx, cy) = P
        den = (byp - cy) * (ax - cx) + (cx - bxp) * (ay - cy)
        if abs(den) < 1e-6: continue
        l0 = ((byp - cy) * (xx - cx) + (cx - bxp) * (yy - cy)) / den
        l1 = ((cy - ay) * (xx - cx) + (ax - cx) * (yy - cy)) / den
        l2 = 1 - l0 - l1
        m = (l0 >= -1e-4) & (l1 >= -1e-4) & (l2 >= -1e-4)
        if not m.any(): continue
        L = np.stack([l0, l1, l2], -1)
        z = L @ v[:, 2]
        zold = zb[by0:by1, bx0:bx1]
        m &= z >= zold
        if not m.any(): continue
        q = v[:, 9]; S = v[:, 7] * q; T = v[:, 8] * q
        qq = L @ q; u = (L @ S) / qq; w_ = (L @ T) / qq
        tx = sample(tex(t['hash']), u, w_)
        vc = L @ v[:, 3:7] / 128.0
        rgb = tx[..., :3] * vc[..., :3] * 2.0 * 0.5 + (vc[..., 3:4] * 0.15 if t['tfx'] == 3 else 0)
        a = tx[..., 3] if t['abe'] else np.ones_like(u)
        a = np.where(m, np.clip(a * 1.0, 0, 1), 0)
        c_old = col[by0:by1, bx0:bx1]
        col[by0:by1, bx0:bx1] = c_old * (1 - a[..., None]) + rgb * a[..., None]
        cov[by0:by1, bx0:bx1] = np.maximum(cov[by0:by1, bx0:bx1], a)
        solid = m & (a > 0.5)
        zb[by0:by1, bx0:bx1] = np.where(solid, z, zold)
        # the triangle's normal from its screen position and depth (depth scaled to about the screen's units)
        pz = (v[:, 2] - zlo) / max(zhi - zlo, 1) * (W / scale) * 0.6
        e1 = np.array([P[1, 0] - P[0, 0], P[1, 1] - P[0, 1], (pz[1] - pz[0]) * scale]); e2 = np.array([P[2, 0] - P[0, 0], P[2, 1] - P[0, 1], (pz[2] - pz[0]) * scale])
        n = np.cross(e1, e2); n = n / (np.linalg.norm(n) + 1e-9)
        if n[2] < 0: n = -n
        nrm[by0:by1, bx0:bx1] = np.where(solid[..., None], n, nrm[by0:by1, bx0:bx1])
    if light:
        import cv2
        n = cv2.GaussianBlur(nrm, (0, 0), 2.0 * scale); n /= np.linalg.norm(n, axis=-1, keepdims=True) + 1e-9
        key = np.array([-0.45, -0.55, 0.70]); key /= np.linalg.norm(key)
        rim = np.array([0.85, -0.25, -0.2]); rim /= np.linalg.norm(rim)
        kd = np.clip((n * key).sum(-1), 0, 1); rd = np.clip((n * rim).sum(-1) + 0.35, 0, 1) ** 3
        shade = 0.82 + 0.30 * kd
        col = col * shade[..., None] + rd[..., None] * np.array([0.10, 0.09, 0.08]) * (cov[..., None] > 0)
    if bg is None:
        gy = np.linspace(0, 1, H)[:, None, None]
        bg = (np.array([0.20, 0.23, 0.27]) * (1 - gy) + np.array([0.08, 0.09, 0.11]) * gy) * np.ones((1, W, 1))
    out = col if straight else col * cov[..., None] + bg * (1 - cov[..., None])      # straight: colour with its own alpha
    rgba = np.dstack([np.clip(out, 0, 1), cov])
    return (rgba * 255).astype(np.uint8)

if __name__ == '__main__':
    a = sys.argv[1:]
    d, out = a[0], a[1]
    scale = int(a[a.index('--scale') + 1]) if '--scale' in a else 4
    ov = {}
    for i, x in enumerate(a):
        if x == '--tex': h, f = a[i + 1].split('='); ov[h] = f
    crop = tuple(float(x) for x in a[a.index('--crop') + 1].split(',')) if '--crop' in a else None
    Image.fromarray(render(d, scale, ov, crop)).save(out)
