import cv2, numpy as np
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
from feat import regions
def bfeat(path):
    img = cv2.imread(path)
    r, box, found = regions(img)
    lab = lambda p: cv2.cvtColor(p, cv2.COLOR_BGR2LAB).reshape(-1, 3).astype(np.float32) if p.size else np.zeros((1, 3), np.float32) + 128
    sk = np.concatenate([lab(r['cheekL']), lab(r['cheekR']), lab(r['nose'])])
    sL = np.median(sk[:, 0]); sC = np.median(np.hypot(sk[:, 1] - 128, sk[:, 2] - 128))
    out = [sL, sC]
    for k in ('lip', 'chin', 'jawL', 'jawR'):
        p = lab(r[k]); L = p[:, 0]; C = np.hypot(p[:, 1] - 128, p[:, 2] - 128)
        dark = np.mean((L < sL - 18) & (C < sC + 2))
        out += [(sL - np.median(L)) / max(sL, 30), sC - np.median(C), dark, L.std() / max(sL, 30)]
    return np.array(out, np.float32)
