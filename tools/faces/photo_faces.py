"""Faces from photos you supply: a front-facing photo of a player becomes his face texture.

    python3 tools/faces/photo_faces.py PHOTO_DIR MOD_DIR WORKDIR [--strength 0.9]

PHOTO_DIR holds one photo per player, named after him ("Mike Trout.jpg") or by his database key
("0ad3fe283.png"); front-facing, eyes open, no sunglasses, the face at least ~200 pixels wide works best.
MOD_DIR is a league whose faces were made by tools/faces/build_faces.py (its faces.txt says which texture is
whose). For every photo: the face and eyes are found (OpenCV's own detectors, no downloads), the photo is laid
onto the face texture's layout by its eyes and mouth (an affine fit to where they sit in the game's head
texture), its colours are matched to the player's skin, and it is blended into his generated face inside the
face (the hair, ears and neck stay the generated ones). Both the 128 x 256 texture and its 4x HD version are
replaced. Photos where no face or no pair of eyes is found are listed and skipped.

Photos of real people: use ones you have the right to use.
"""
import sys, os, glob, re
import numpy as np, cv2
from PIL import Image
here = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, here); sys.path.insert(0, os.path.join(here, '..'))
import facegen, mvpdb, ssh, refpack, build_faces

# where the eyes and the mouth sit in the 128 x 256 head texture (generic layout, measured on the disc's faces)
UV_EYE_L, UV_EYE_R, UV_MOUTH = (50.0, 87.5), (77.0, 87.5), (63.5, 139.0)

def find_face(img):
    g = cv2.equalizeHist(cv2.cvtColor(img, cv2.COLOR_RGB2GRAY))
    fc = cv2.CascadeClassifier(cv2.data.haarcascades + 'haarcascade_frontalface_default.xml')
    ec = cv2.CascadeClassifier(cv2.data.haarcascades + 'haarcascade_eye.xml')
    faces = fc.detectMultiScale(g, 1.08, 5, minSize=(60, 60))
    if len(faces) == 0: return None
    x, y, w, h = max(faces, key=lambda r: r[2] * r[3])
    roi = g[y:y + h // 2 + h // 10, x:x + w]
    eyes = ec.detectMultiScale(roi, 1.05, 4, minSize=(w // 10, w // 10), maxSize=(w // 3, w // 3))
    if len(eyes) < 2: return None
    cs = sorted([(x + ex + ew / 2.0, y + ey + eh / 2.0, ew * eh) for ex, ey, ew, eh in eyes], key=lambda e: -e[2])
    pair = None
    for i in range(len(cs)):                                  # the biggest two that sit side by side
        for j in range(i + 1, len(cs)):
            a, b = cs[i], cs[j]
            if abs(a[0] - b[0]) > w * 0.2 and abs(a[1] - b[1]) < h * 0.12: pair = (a, b); break
        if pair: break
    if not pair: return None
    l, r = sorted(pair, key=lambda e: e[0])
    ex, ey = (l[0] + r[0]) / 2, (l[1] + r[1]) / 2
    d = r[0] - l[0]
    mouth = (ex - (r[1] - l[1]) * 1.05, ey + d * 1.05)         # the mouth: about one eye distance below the eyes
    return (l[0], l[1]), (r[0], r[1]), mouth

def photo_to_uv(img, pts, k=4):
    src = np.float32(pts); dst = np.float32([UV_EYE_L, UV_EYE_R, UV_MOUTH]) * k
    A = cv2.getAffineTransform(src, dst)
    return cv2.warpAffine(img, A, (128 * k, 256 * k), flags=cv2.INTER_CUBIC, borderMode=cv2.BORDER_REFLECT)

def blend(face, uvphoto, src, strength=0.9):
    """the photo inside the face, its colours matched to the generated skin"""
    k = src['k']
    m = np.clip(src['m_up'] + src['m_low'], 0, 1)
    m = facegen._gauss(m, 2.0 * k)[..., None] * strength
    f = face[..., :3].astype(np.float32); p = uvphoto.astype(np.float32)
    ref = (slice(110 * k, 135 * k), slice(36 * k, 92 * k))                  # cheeks and nose: the skin to match
    gain = f[ref].reshape(-1, 3).mean(0) / np.maximum(p[ref].reshape(-1, 3).mean(0), 1)
    p = p * gain
    out = face.copy(); out[..., :3] = np.clip(f * (1 - m) + p * m, 0, 255).astype(np.uint8)
    return out

def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    photos, mod, work = a[0], a[1], a[2]
    strength = float(sys.argv[sys.argv.index('--strength') + 1]) if '--strength' in sys.argv else 0.9
    T, _ = mvpdb.read_database(os.path.join(mod, 'DATA', 'DATABASE', 'DATABASE.BIG'))
    at = T['attrib.dat']; ci = {c: i for i, c in enumerate(at.cols)}
    by_name = {}; rows = {}
    for key, v in at.rows:
        by_name.setdefault(('%s %s' % (v[0], v[1])).lower(), key); rows[key] = v
    faces = {}
    for ln in open(os.path.join(mod, 'faces.txt')):
        if ln.startswith('#'): continue
        p = ln.split()
        if len(p) >= 4: faces[p[0]] = (p[1], int(p[2]), p[3])
    src = facegen.load(work); src_hd = facegen.load(work, hd=True)
    g = None
    done, skipped = 0, []
    for path in sorted(glob.glob(os.path.join(photos, '*'))):
        stem = os.path.splitext(os.path.basename(path))[0]
        key = stem.lower() if re.fullmatch(r'[0-9a-f]{9}', stem.lower()) else by_name.get(re.sub(r'[_\-]+', ' ', stem).lower())
        if not key or key not in faces: skipped.append('%s: no such player with a made face' % stem); continue
        try: img = np.asarray(Image.open(path).convert('RGB'))
        except Exception as e: skipped.append('%s: %s' % (stem, e)); continue
        pts = find_face(img)
        if not pts: skipped.append('%s: no face with two eyes found' % stem); continue
        name, seed, hd_name = faces[key]
        v = rows[key]; col = lambda n: int(v[ci['playerattrib_' + n]])
        h, c, s, f = ord(name[1]) - 97, ord(name[2]) - 97, ord(name[3]) - 97, ord(name[4]) - 97
        # the face he was given, made again, then the photo over it
        params = dict(tone=col('skin_tone'), head=h, hair=c, style=s, facial=f, age=27)
        if os.path.exists(os.path.join(mod, 'faces_params.txt')):
            for ln in open(os.path.join(mod, 'faces_params.txt')):
                q = ln.split()
                if q and q[0] == key: params.update(tone=int(q[1]), hair=int(q[2]), style=int(q[3]), facial=int(q[4]), age=float(q[5]))
        lo, _ = facegen.make_face(src, params, seed); hi, _ = facegen.make_face(src_hd, params, seed)
        hi = blend(hi, photo_to_uv(img, pts, 4), src_hd, strength)
        lo = blend(lo, photo_to_uv(img, pts, 1), src, strength)
        gd = os.path.join(mod, 'DATA', 'GHEAD.BIG', name)
        fsh = ssh.Ssh(refpack.decompress(open(gd, 'rb').read())); sh = fsh.shapes[0]
        rgba = lo.copy(); rgba[..., 3] = np.round(lo[..., 3].astype(np.float32) * 128 / 255).astype(np.uint8)
        data, pal = ssh.image14_photo(rgba); sh.set_pixel_bytes(data); sh.set_palette(pal)
        open(gd, 'wb').write(refpack.compress(fsh.build()))
        rd = os.path.join(mod, 'textures', 'replace')
        if hd_name != '-' and os.path.exists(os.path.join(rd, hd_name)): os.remove(os.path.join(rd, hd_name))
        new = '128x256_%016x.png' % build_faces.tex_hash(ssh.decode14(data, 128, 256), pal)
        f4 = os.path.join(mod, 'faces_4x'); os.makedirs(f4, exist_ok=True)
        Image.fromarray(hi, 'RGBA').save(os.path.join(f4, key + '.png'))           # for the portrait renderer
        import cv2
        Image.fromarray(cv2.resize(hi, (256, 512), interpolation=cv2.INTER_AREA), 'RGBA').save(os.path.join(rd, new), compress_level=9)
        faces[key] = (name, seed, new); done += 1
        print('%s -> %s' % (stem, name), flush=True)
    with open(os.path.join(mod, 'faces.txt'), 'w') as fo:
        fo.write('# player key, face texture (GHEAD.BIG), seed, HD texture (tools/faces/build_faces.py, photo_faces.py)\n')
        for k_, (n_, s_, h_) in faces.items(): fo.write('%s %s %d %s\n' % (k_, n_, s_, h_))
    print('%d faces from photos; skipped %d' % (done, len(skipped)))
    for x in skipped: print('  ' + x)

if __name__ == '__main__': main()
