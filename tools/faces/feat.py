import cv2, numpy as np, glob, os
casc = cv2.CascadeClassifier(cv2.data.haarcascades + 'haarcascade_frontalface_default.xml')
def regions(img):
    g = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    f = casc.detectMultiScale(g, 1.05, 3, minSize=(30, 30))
    if len(f):
        x, y, w, h = max(f, key=lambda r: r[2] * r[3])
    else:
        x, y, w, h = 26, 55, 44, 50
    R = lambda fx0, fy0, fx1, fy1: img[int(y + fy0 * h):int(y + fy1 * h), int(x + fx0 * w):int(x + fx1 * w)]
    return dict(
        cheekL=R(0.15, 0.50, 0.32, 0.68), cheekR=R(0.68, 0.50, 0.85, 0.68),
        nose=R(0.42, 0.45, 0.58, 0.62), forehead=R(0.3, 0.08, 0.7, 0.2),
        lip=R(0.35, 0.72, 0.65, 0.80), chin=R(0.35, 0.92, 0.65, 1.08),
        jawL=R(0.10, 0.80, 0.28, 0.98), jawR=R(0.72, 0.80, 0.90, 0.98),
        sideL=R(-0.12, 0.05, 0.02, 0.40), sideR=R(0.98, 0.05, 1.12, 0.40)), (x, y, w, h), len(f) > 0
def lab(p):
    if p.size == 0: return np.array([50., 0., 0.]), 0.
    l = cv2.cvtColor(p, cv2.COLOR_BGR2LAB).reshape(-1, 3).astype(np.float32)
    return np.median(l, 0), float(l[:, 0].std())
def features(path):
    img = cv2.imread(path)
    if img is None: return None
    r, box, found = regions(img)
    sk = np.median(np.stack([lab(r['cheekL'])[0], lab(r['cheekR'])[0], lab(r['nose'])[0], lab(r['forehead'])[0]]), 0)
    out = list(sk)
    for k in ('lip', 'chin', 'jawL', 'jawR'):
        m, s = lab(r[k]); out += [sk[0] - m[0], s, m[1] - sk[1], m[2] - sk[2]]
    for k in ('sideL', 'sideR'):
        m, s = lab(r[k]); out += list(m) + [s]
    out += [box[2] / 96.0, float(found)]
    return np.array(out, np.float32)
