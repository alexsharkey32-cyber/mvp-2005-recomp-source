"""Hair measurements from a 96x144 official headshot (cap on, face centred, plain studio backdrop)."""
import numpy as np, cv2
_casc = cv2.CascadeClassifier(cv2.data.haarcascades + 'haarcascade_frontalface_default.xml')

def facebox(img):
    g = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    f = _casc.detectMultiScale(g, 1.05, 3, minSize=(30, 30))
    if len(f): return tuple(int(v) for v in max(f, key=lambda r: r[2] * r[3])), True
    return (26, 55, 44, 50), False

def hair(img):
    """-> dict: skin (Lab), hair (Lab of the hair beside the face, or None), n (hair pixels), long (share of the
    area beside the jaw that is hair: > 0.1 = hair down to the collar), beard (Lab of the darkest beard pixels or
    None), beard_frac, brow (Lab of the darkest eyebrow pixels or None), mask / bmask for debugging.
    Lab as OpenCV's 8-bit Lab: L 0..255, a and b centred on 128."""
    (x, y, w, h), found = facebox(img)
    H, W = img.shape[:2]; cx = x + w // 2
    lab = cv2.cvtColor(cv2.GaussianBlur(img, (3, 3), 0), cv2.COLOR_BGR2LAB).astype(np.float32)
    L = lab[..., 0]
    def box(fx0, fy0, fx1, fy1):
        x0 = int(np.clip(x + fx0 * w, 0, W)); x1 = int(np.clip(x + fx1 * w, 0, W)); y0 = int(np.clip(y + fy0 * h, 0, H)); y1 = int(np.clip(y + fy1 * h, 0, H))
        return (slice(y0, y1), slice(x0, x1))
    sk = np.concatenate([lab[box(0.22, 0.50, 0.36, 0.66)].reshape(-1, 3), lab[box(0.64, 0.50, 0.78, 0.66)].reshape(-1, 3), lab[box(0.42, 0.45, 0.58, 0.62)].reshape(-1, 3)])
    skin = np.median(sk, 0) if len(sk) else np.array([150., 145., 148.], np.float32)
    dsk = np.linalg.norm((lab - skin) * [0.7, 1.6, 1.6], axis=2)
    # the backdrop, per row and side (it has a gradient), from the image's edge columns
    bgl = np.median(lab[:, 0:3], 1); bgr = np.median(lab[:, W - 3:W], 1)
    bg = np.where((np.arange(W) < cx)[None, :, None], bgl[:, None, :], bgr[:, None, :])
    dbg = np.linalg.norm((lab - bg) * [1.0, 2.2, 2.2], axis=2)
    ear = (lab[..., 1] > skin[1] + 2.5) & (L > skin[0] - 50) & (lab[..., 1] > 138)      # ears and shadowed skin: redder than the cheek
    cand = (dbg > 26) & (dsk > 24) & ~ear & (L < skin[0] + 6)
    side = np.zeros((H, W), bool); low = np.zeros((H, W), bool)
    for b in (box(-0.30, 0.45, 0.00, 1.00), box(1.00, 0.45, 1.30, 1.00)): side[b] = True          # beside the ears and the jaw
    for b in (box(-0.30, 0.72, -0.03, 1.00), box(1.03, 0.72, 1.30, 1.00)): low[b] = True          # below the ear lobes
    side[:, :3] = False; side[:, W - 3:] = False; low[:, :3] = False; low[:, W - 3:] = False
    m = cand & side
    out = dict(skin=skin, box=(x, y, w, h), found=found, mask=m, n=int(m.sum()))
    out['long'] = float((cand & low).sum()) / max(int(low.sum()), 1)
    out['hair'] = np.median(lab[m], 0) if m.sum() >= 30 else None
    bm = np.zeros((H, W), bool)
    for b in (box(0.32, 0.90, 0.68, 1.06), box(0.14, 0.74, 0.30, 0.96), box(0.70, 0.74, 0.86, 0.96)): bm[b] = True   # chin and jaw
    beard = bm & (L < skin[0] - 16) & (dbg > 20)
    out['beard_frac'] = float(beard.sum()) / max(int(bm.sum()), 1); out['bmask'] = beard
    if beard.sum() >= 50:
        px = lab[beard]; px = px[np.argsort(px[:, 0])[:max(20, len(px) // 3)]]      # the darkest third: least skin showing through
        out['beard'] = np.median(px, 0)
    else: out['beard'] = None
    br = np.concatenate([lab[box(0.18, 0.24, 0.44, 0.38)].reshape(-1, 3), lab[box(0.56, 0.24, 0.82, 0.38)].reshape(-1, 3)])
    out['brow'] = np.median(br[np.argsort(br[:, 0])[:max(6, len(br) // 8)]], 0) if len(br) > 20 else None
    return out
