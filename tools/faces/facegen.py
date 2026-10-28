"""A face of his own for every player: face textures made from the game's own pieces.

The game's generic heads (GHEAD.BIG g<head><haircolour><hairstyle><facialhair>.ssh, 128 x 256, the layout of the
generic head models g001-g045) are composites: 15 bald faces, each with every hair colour x style and every
facial hair painted on. They are taken apart here into layers (alpha and colour per pixel, solved over the 15
faces: composite = base * (1 - a) + layer * a), so hair and beards can go on any face, in any colour, greying.
The faces themselves are new: a base face of the player's skin tone gets the features (eyes, brows, nose, lips,
creases) of two or three of the game's 294 real 2005 faces that share the generic layout, blended with random
weights, so no two players share a face and none is a 2005 player's. Age deepens the creases and greys the hair;
some clean-shaven players get stubble; eye colour varies with skin tone.

    python3 tools/faces/facegen.py prepare ISO WORKDIR      # take the disc's faces apart (once)
    python3 tools/faces/facegen.py sheet WORKDIR OUT.png    # 24 sample faces

make_face(src, params, seed) -> 128 x 256 RGBA (uint8, alpha 0..255) and, with src['hd'], a 512 x 1024 version
composed from 4x upscaled pieces (tools/hd/esrgan.py) so the HD textures carry the same face.
"""
import sys, os
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

# head -> the body skin tone the 2005 database pairs it with (research/notes/findings.md, "Generic heads")
HEAD_TONE = [0, 1, 1, 2, 2, 3, 4, 5, 5, 6, 7, 8, 9, 9, 10]
UP, LOW = (slice(58, 128), slice(18, 110)), (slice(118, 196), slice(30, 98))   # brows-eyes-nose / mouth-chin

def _gauss(x, s):
    import cv2
    return cv2.GaussianBlur(x, (0, 0), s)

def prepare(iso, work):
    import isofile, refpack, ssh
    from bigfile import read_big
    os.makedirs(work, exist_ok=True)
    g = isofile.read_file(iso, 'DATA/GHEAD.BIG'); ge = {n: (o, s) for n, o, s in read_big(g)}
    def tex(big, ents, n):
        o, s = ents[n]; c = big[o:o + s]
        c = refpack.decompress(c) if c[:2] in (b'\x10\xfb', b'\x11\xfb', b'\x90\xfb', b'\x91\xfb') else c
        return ssh.Ssh(c).shapes[0].rgba()
    G = np.zeros((15, 7, 10, 8, 256, 128, 4), np.uint8)
    for h in range(15):
        for c in range(7):
            for s in range(10):
                for f in range(8):
                    G[h, c, s, f] = tex(g, ge, 'g%s%s%s%s.ssh' % (chr(97 + h), chr(97 + c), chr(97 + s), chr(97 + f)))
    B = G[:, 0, 0, 0, :, :, :3].astype(np.float32)
    def layer(T, Bx):
        D = T - Bx; Bm = Bx.mean(0); Dm = D.mean(0); Bc = Bx - Bm; Dc = D - Dm
        a = np.clip(-(Bc * Dc).sum((0, 3)) / np.maximum((Bc * Bc).sum((0, 3)), 1e-3), 0, 1)
        u = Dm + a[..., None] * Bm
        return a, np.clip(np.where(a[..., None] > 0.02, u / np.maximum(a[..., None], 1e-3), 0), 0, 255)
    ha = np.zeros((7, 10, 256, 128), np.float32); hc = np.zeros((7, 10, 256, 128, 3), np.float32)
    ba = np.zeros((7, 8, 256, 128), np.float32); bc = np.zeros((7, 8, 256, 128, 3), np.float32)
    for c in range(7):
        for s in range(1, 10): ha[c, s], hc[c, s] = layer(G[:, c, s, 0, :, :, :3].astype(np.float32), B)
        Bx = G[:, c, 3, 0, :, :, :3].astype(np.float32)
        for f in range(1, 8):
            a, col = layer(G[:, c, 3, f, :, :, :3].astype(np.float32), Bx)
            a[:96] = 0; a *= np.clip(1 - ha[c, 3] * 1.5, 0, 1)          # beards live below the hair
            ba[c, f], bc[c, f] = _gauss(a, 0.6), col
    # the real faces that share the generic layout
    d = isofile.read_file(iso, 'DATA/MODELS.BIG'); de = {n: (o, s) for n, o, s in read_big(d)}
    tmpl = _gauss(B.mean(0).mean(-1), 1.5)
    donors = []
    for n in sorted(x for x in de if x.startswith('c') and x.endswith('.ssh')):
        t = tex(d, de, n)
        if t.shape[:2] != (256, 128): continue
        y = t[60:170, 24:104, :3].astype(np.float32).mean(-1); x = tmpl[60:170, 24:104]
        y = (y - y.mean()) / (y.std() + 1e-6); x = (x - x.mean()) / (x.std() + 1e-6)
        if (x * y).mean() > 0.3: donors.append(t[..., :3])
    D = np.array(donors).astype(np.float32)
    lum = D.mean(-1); cheek = lum[:, 115:135, 30:45].mean((1, 2))
    clean = np.minimum(lum[:, 135:160, 50:78].mean((1, 2)), lum[:, 165:185, 48:80].mean((1, 2))) / cheek
    np.savez_compressed(os.path.join(work, 'faces.npz'), B=B.astype(np.uint8), A=G[:, 0, 0, 0, :, :, 3],
                        ha=ha.astype(np.float16), hc=hc.astype(np.uint8), ba=ba.astype(np.float16), bc=bc.astype(np.uint8),
                        D=D.astype(np.uint8), clean=clean.astype(np.float32))
    print('%d donor faces, layers saved in %s' % (len(D), work))

def load(work, hd=False):
    """the pieces at 128 x 256 (hd=False) or their 4x versions (faces_hd.npz from tools/faces/upscale_sources.py)"""
    k = 1 if not hd else (2 if hd == 2 else 4)
    if k == 2:                                                                # 2x: made once from the 4x pieces
        p2 = os.path.join(work, 'faces_2x.npz')
        if not os.path.exists(p2):
            import cv2
            z = np.load(os.path.join(work, 'faces_hd.npz')); d = {}
            for n in z.files:
                a = z[n]
                if a.ndim >= 3 and a.shape[-3:-1] == (1024, 512) and a.shape[-1] in (3, 4): lead = a.shape[:-3]; flat = a.reshape((-1, 1024, 512, a.shape[-1]))
                elif a.ndim >= 2 and a.shape[-2:] == (1024, 512): lead = a.shape[:-2]; flat = a.reshape((-1, 1024, 512))
                else: d[n] = a; continue
                out = np.stack([cv2.resize(x.astype(np.float32), (256, 512), interpolation=cv2.INTER_AREA).astype(a.dtype) for x in flat])
                d[n] = out.reshape(lead + out.shape[1:])
            np.savez(p2, **d)
        z = np.load(p2)
    else: z = np.load(os.path.join(work, 'faces_hd.npz' if hd else 'faces.npz'))
    src = {kk: z[kk] for kk in z.files}
    src['k'] = k
    src['Bf'] = src['B'].astype(np.float32)
    src['skin'] = src['Bf'][:, 110 * k:140 * k, 28 * k:48 * k].reshape(len(src['Bf']), -1, 3).mean(1)
    yy, xx = np.mgrid[0:256 * k, 0:128 * k] / float(k)
    def ell(cy, cx, ry, rx, soft):
        r = np.sqrt(((yy - cy) / ry) ** 2 + ((xx - cx) / rx) ** 2); return np.clip((1 - r) / soft, 0, 1).astype(np.float32)
    src['m_up'] = ell(92, 64, 38, 29, 0.3)                                 # brows, eyes, nose (not the temples: a donor's sideburns)
    src['m_low'] = ell(152, 64, 34, 30, 0.35) * (yy > 118)                  # mouth and chin
    src['m_brow'] = ell(76, 64, 9, 40, 0.5)
    src['m_face'] = ell(120, 64, 96, 56, 0.25)
    return src

def _det(src, i):
    """donor i's features: the face over its own smooth skin, as a ratio (carries over to any skin tone)"""
    c = src.setdefault('_det', {})
    if i not in c:
        D = src['D'][i].astype(np.float32) + 1
        c[i] = D / _gauss(D, 5.0 * src['k'])
        if len(c) > 64: c.pop(next(iter(c)))
    return c[i]

def make_face(src, p, seed):
    """p: dict(tone 0..10, hair 0..6, style 0..9, facial 0..7, age years, head 0..14 optional)"""
    rng = np.random.default_rng(seed)
    tone = int(p.get('tone', 2)); age = float(p.get('age', 27))
    heads = [h for h, t in enumerate(HEAD_TONE) if t == tone] or [int(np.argmin([abs(t - tone) for t in HEAD_TONE]))]
    h = int(p['head']) if p.get('head') is not None else int(rng.choice(heads))
    base = src['Bf'][h].copy()
    base *= (1 + rng.normal(0, 0.035)) * (1 + rng.normal(0, 0.02, 3))         # a shade of his own
    n = len(src['D'])
    up = rng.choice(n, 2, replace=False); w = rng.dirichlet([1.5, 1.5])
    clean = np.where(src['clean'] > 0.9)[0]
    low = rng.choice(clean if len(clean) > 4 else np.arange(n), 2, replace=False); w2 = rng.dirichlet([1.5, 1.5])
    k = 0.85 + 0.012 * max(0.0, age - 24) + rng.normal(0, 0.05)              # creases deepen with age
    r_up = np.exp(sum(wi * np.log(_det(src, i)) for wi, i in zip(w, up)) * k)
    r_low = np.exp(sum(wi * np.log(_det(src, i)) for wi, i in zip(w2, low)) * k)
    m1, m2 = src['m_up'][..., None], src['m_low'][..., None]
    face = base * (1 + m1 * (r_up - 1)) * (1 + m2 * (r_low - 1))
    # brows follow the hair: lighter for blond / red / grey
    hair = int(p.get('hair', 0))
    if hair in (1, 3, 6):
        mb = src['m_brow'][..., None] * (0.45 if hair == 1 else 0.3)
        face = face * (1 - mb) + base * mb
    # greying with age (hair colour 6 is grey)
    grey = float(np.clip((age - 36) / 18 + rng.normal(0, 0.15), 0, 0.9)) if hair != 6 else 0.0
    def hair_layer(a, c, cg):
        col = c.astype(np.float32)
        if grey > 0:
            g = cg.astype(np.float32).mean(-1, keepdims=True) * np.array([1.0, 0.99, 0.96], np.float32)   # the game's grey is bluish
            col = col * (1 - grey) + g * grey
        return a.astype(np.float32)[..., None], col
    style = int(p.get('style', 4)); facial = int(p.get('facial', 0))
    if style > 0:
        a, col = hair_layer(src['ha'][hair, style], src['hc'][hair, style], src['hc'][6, style])
        face = face * (1 - a) + col * a
    if facial > 0:
        a, col = hair_layer(src['ba'][hair, facial], src['bc'][hair, facial], src['bc'][6, facial])
        a = a * (0.75 + 0.25 * rng.random())
        face = face * (1 - a) + col * a
    elif rng.random() < 0.3:                                                  # stubble
        a = src['ba'][hair, 1].astype(np.float32)[..., None] * (0.12 + 0.12 * rng.random())
        face = face * (1 - a) + src['bc'][hair, 1].astype(np.float32) * a
    K = src['k']
    blue_eyes = tone <= 2 and rng.random() < 0.35; eye_draw = rng.random()   # drawn before anything size-dependent:
    if K > 1:                                                                 # every size of a face is the same face
        g = np.random.default_rng(seed + 7).normal(0, 1, face.shape[:2]).astype(np.float32)   # skin: fine pores the upscaler smoothed away
        pores = _gauss(g, 0.8) - _gauss(g, 2.5)
        face = face * (1 + (pores * 0.045 * src['m_face'])[..., None])
    out = np.zeros((256 * K, 128 * K, 4), np.uint8)
    out[..., :3] = np.clip(face, 0, 255); out[..., 3] = src['A'][h]
    # the eye / teeth atlas below the face stays the base's; eye colour: blue or green now and then for light skin
    if blue_eyes:
        eyes = out[200 * K:256 * K, 0:64 * K, :3].astype(np.float32)
        lumi = eyes.mean(-1, keepdims=True); iris = (lumi < 120) & (lumi > 25)
        tint = np.array([90, 125, 170] if eye_draw < 0.7 else [95, 135, 95], np.float32)
        out[200 * K:256 * K, 0:64 * K, :3] = np.where(iris, np.clip(lumi / 80 * tint, 0, 255), eyes).astype(np.uint8)
    return out, h

def sheet(work, path, n=24, hd=False):
    from PIL import Image
    src = load(work, hd); rng = np.random.default_rng(7); tiles = []
    for i in range(n):
        p = dict(tone=int(rng.integers(0, 11)), hair=int(rng.integers(0, 7)), style=int(rng.choice([3, 4, 5, 6, 7, 8])),
                 facial=int(rng.choice([0, 0, 0, 1, 2, 5, 6, 7])), age=float(rng.integers(21, 41)))
        t, _ = make_face(src, p, 1000 + i); tiles.append(t[..., :3])
    rows = [np.concatenate(tiles[i:i + 8], 1) for i in range(0, n, 8)]
    Image.fromarray(np.concatenate(rows, 0)).save(path)

if __name__ == '__main__':
    if sys.argv[1] == 'prepare': prepare(sys.argv[2], sys.argv[3])
    elif sys.argv[1] == 'sheet': sheet(sys.argv[2], sys.argv[3], hd='--hd' in sys.argv)
