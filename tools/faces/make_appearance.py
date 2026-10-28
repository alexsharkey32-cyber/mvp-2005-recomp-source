"""Measures each player's look from his MLB headshot for tools/build_rosters.py.

Usage: python3 tools/faces/make_appearance.py headshots.bin [out=game/rosters/appearance_2026.txt]
headshots.bin: records of (u32 mlbam id, u32 length, JPEG) — 96 px wide official headshots (img.mlbstatic.com).
Per player: face lightness (cheeks/nose, corrected for the photo's exposure using the grey studio background),
and the probability of facial hair / a full beard from a small logistic model trained on 96 hand-labelled
headshots (beard_sample_*.txt: 0 none, 1 stubble, 2 goatee or moustache, 3 full beard).
Hair (hair.py): the colour of the hair that shows beside the face under the cap, how far down it reaches
(long hair), and the colour of the beard and the eyebrows as a second opinion on the hair colour.
"""
import sys, os, struct, tempfile, numpy as np, cv2
sys.path.insert(0, os.path.dirname(__file__))
from feat2 import bfeat
from hair import hair
HERE = os.path.dirname(__file__)
src = sys.argv[1]; out = sys.argv[2] if len(sys.argv) > 2 else 'game/rosters/appearance_2026.txt'
d = open(src, 'rb').read(); imgs = {}; i = 0
while i < len(d):
    pid, n = struct.unpack('<II', d[i:i + 8]); i += 8; imgs[pid] = d[i:i + n]; i += n
tmp = tempfile.mkdtemp()
def path(pid):
    p = os.path.join(tmp, '%d.jpg' % pid)
    if not os.path.exists(p): open(p, 'wb').write(imgs[pid])
    return p
def exposure(p):
    img = cv2.imread(p); lab = cv2.cvtColor(img, cv2.COLOR_BGR2LAB).astype(np.float32)
    bg = np.concatenate([lab[2:14, 2:14].reshape(-1, 3), lab[2:14, -14:-2].reshape(-1, 3)])
    L, a, b = np.median(bg, 0)
    neutral = abs(a - 128) < 6 and abs(b - 128) < 8 and 120 < L < 240
    return L if neutral else None
ids = [int(x) for x in open(os.path.join(HERE, 'beard_sample_ids.txt')).read().split()]
y = np.array([int(x) for x in open(os.path.join(HERE, 'beard_sample_labels.txt')).read().split()])
X = np.stack([bfeat(path(i)) for i in ids])
def fit(X, t, l2=2.0, it=4000, lr=0.1):
    mu = X.mean(0); sd = X.std(0) + 1e-6; Z = np.hstack([(X - mu) / sd, np.ones((len(X), 1))]); w = np.zeros(Z.shape[1])
    for _ in range(it):
        p = 1 / (1 + np.exp(-Z @ w)); w -= lr * (Z.T @ (p - t) / len(t) + l2 * np.r_[w[:-1], 0] / len(t))
    return mu, sd, w
def pred(m, x): mu, sd, w = m; z = np.r_[(x - mu) / sd, 1]; return 1 / (1 + np.exp(-z @ w))
m_any = fit(X, (y >= 2).astype(float)); m_full = fit(X, (y == 3).astype(float))
bgs = {pid: exposure(path(pid)) for pid in imgs}
ref = np.median([v for v in bgs.values() if v is not None])
lines = []
for pid in sorted(imgs):
    b = bfeat(path(pid)); L = b[0]
    if bgs[pid] is not None: L = L * min(1.08, max(0.92, ref / bgs[pid]))   # small exposure correction only
    h = hair(cv2.imread(path(pid)))
    f3 = lambda v: '%.0f %.0f %.0f' % tuple(v) if v is not None else '-1 -1 -1'
    lines.append('%d %.1f %.3f %.3f %.3f %.3f %.3f %d %.3f %s %s %.0f %.0f %.0f' % (pid, L, pred(m_any, b), pred(m_full, b), b[2], b[6], (b[10] + b[14]) / 2,
                 h['n'], h['long'], f3(h['hair']), f3(h['beard']), h['brow'][0] if h['brow'] is not None else -1, h['skin'][1], h['skin'][2]))
open(out, 'w').write('# mlbam_id skin_L p_facial_hair p_full_beard lip_dark chin_dark jaw_dark hair_pixels long_hair hair_L hair_a hair_b beard_L beard_a beard_b brow_L skin_a skin_b'
                     ' (tools/faces/make_appearance.py; colours are OpenCV Lab, -1 = not measured)\n' + '\n'.join(lines) + '\n')
print('%d players -> %s' % (len(lines), out))
