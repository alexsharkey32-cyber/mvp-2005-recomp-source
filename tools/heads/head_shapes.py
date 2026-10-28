"""More varied head shapes: the 15 generic heads reshaped below the eyes -> mods/heads (DATA/MODELS.BIG/g001-g045.ord).

    python3 tools/heads/head_shapes.py ISO [OUT = mods/heads] [--preview out.png] [--scale 1.0]

The game's 15 heads (each in three near-identical copies: g001-015, g016-030, g031-045) differ mostly in their
textures; their shapes are close. Each head gets its own jaw width, cheekbone width, lower-face length and chin
depth, within a natural range, so players with different heads also differ in outline. Only the face below the
eyes moves: the cranium (the cap's fit) and the neck (the joint with the body) stay as they are, with smooth
blends in between. Vertex count, texture coordinates and file sizes are unchanged (tools/heads/ordmesh.py).
"""
import sys, os
import numpy as np
here = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, here); sys.path.insert(0, os.path.join(here, '..'))
import ordmesh, isofile, refpack
from bigfile import read_big

# per head (1-15): jaw width, cheekbone width, lower-face length, chin forward (fractions)
SHAPES = {
    1: (+0.06, +0.02, -0.03, +0.00), 2: (-0.05, -0.02, +0.05, +0.02), 3: (+0.02, +0.04, +0.02, -0.02),
    4: (-0.03, +0.00, -0.04, +0.03), 5: (+0.08, +0.03, +0.03, +0.02), 6: (-0.04, +0.05, -0.02, -0.01),
    7: (+0.05, -0.02, +0.04, +0.01), 8: (-0.06, -0.03, -0.02, +0.00), 9: (+0.03, +0.05, +0.00, +0.03),
    10: (+0.07, +0.00, +0.05, -0.02), 11: (-0.02, +0.04, +0.03, +0.02), 12: (+0.04, -0.03, -0.04, +0.00),
    13: (-0.05, +0.02, +0.02, -0.02), 14: (+0.09, +0.04, +0.01, +0.03), 15: (+0.00, -0.04, +0.04, +0.01),
}

def smooth(t): t = np.clip(t, 0, 1); return t * t * (3 - 2 * t)

def reshape(P, params, scale=1.0):
    """P: (n, 3) raw positions (x across, y up, z depth); returns new raw positions"""
    jw, cw, fl, cf = [p * scale for p in params]
    x = P[:, 0] - 32768.0; y = P[:, 1].astype(np.float64); z = P[:, 2].astype(np.float64)
    ylo, yhi = np.percentile(y, 1), np.percentile(y, 99)
    t = (y - ylo) / (yhi - ylo)                     # 0 neck .. 1 crown
    eye, mouth, chin, neck = 0.62, 0.36, 0.20, 0.08
    lower = smooth((eye - t) / (eye - mouth)) * smooth((t - neck) / (chin - neck))   # 0 above the eyes and at the neck
    cheek = np.exp(-((t - (eye - 0.06)) / 0.07) ** 2) * smooth((t - neck) / (chin - neck))
    front = smooth((z - np.percentile(z, 40)) / (np.percentile(z, 95) - np.percentile(z, 40)))     # the face, not the back of the head
    nx = x * (1 + jw * lower + cw * cheek)
    ny = y + (t - eye) * (yhi - ylo) * fl * lower                                  # longer / shorter below the eyes
    nz = z + cf * (np.percentile(z, 95) - np.percentile(z, 40)) * lower * smooth((eye - t) / (eye - chin)) * front
    return np.c_[nx + 32768.0, ny, nz]

def main():
    a = [x for i, x in enumerate(sys.argv[1:], 1) if not x.startswith('--') and sys.argv[i - 1] not in ('--preview', '--scale')]
    iso = a[0]; out = a[1] if len(a) > 1 else os.path.join('mods', 'heads')
    scale = float(sys.argv[sys.argv.index('--scale') + 1]) if '--scale' in sys.argv else 1.0
    d = isofile.read_file(iso, 'DATA/MODELS.BIG'); e = {n: (o, s) for n, o, s in read_big(d)}
    od = os.path.join(out, 'DATA', 'MODELS.BIG'); os.makedirs(od, exist_ok=True)
    prev = []
    for g in range(1, 46):
        name = 'g%03d.ord' % g; o, s = e[name]; raw = d[o:o + s]; comp = raw[1:2] == b'\xfb'
        h = ordmesh.Head(refpack.decompress(raw) if comp else raw)
        P0 = h.xyz(); P1 = reshape(P0, SHAPES[(g - 1) % 15 + 1], scale); h.set_xyz(P1)
        open(os.path.join(od, name), 'wb').write(refpack.compress(h.bytes()) if comp else h.bytes())
        if 16 <= g <= 30: prev.append((g, P0, h.xyz()))
    print('%d head models -> %s' % (45, od))
    if '--preview' in sys.argv:
        from PIL import Image, ImageDraw
        W = Image.new('RGB', (150 * 15, 200), 'white'); dr = ImageDraw.Draw(W)
        for k, (g, A, B) in enumerate(prev):
            for P, col in ((A, (200, 200, 200)), (B, (0, 0, 0))):
                X = (P[:, 0] - 32768) / 16384 * 55; Y = (P[:, 1] - 47800) * 16 / 16384 * 55
                for x, y in zip(X, Y): dr.point((k * 150 + 75 + x, 100 - y), fill=col)
            dr.text((k * 150 + 5, 5), 'head %d' % (g - 15), fill='red')
        W.save(sys.argv[sys.argv.index('--preview') + 1])

if __name__ == '__main__': main()
