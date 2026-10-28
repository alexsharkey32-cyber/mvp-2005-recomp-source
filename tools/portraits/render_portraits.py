"""Portrait pictures from recorded triangles (tools/portraits/capture_prims.py), drawn by tools/portraits/render.py
with the HD textures (made faces, HD uniforms) wherever there is one for a texture, then framed and packed by
tools/portraits/build.py like the captures.

    python3 tools/portraits/render_portraits.py PRIMS_DIR RAW_OUT [--league MOD_DIR --faces WORKDIR] [--replace DIR ...] [--scale 4]

--league with --faces: the players' made faces (tools/faces/build_faces.py) are drawn at 4x, made again from their
seed and looks (the HD textures in the league are 2x; a face from a photo, faces_4x/<key>.png, is used as it is).
    python3 tools/portraits/build.py ISO RAW_OUT mods/<league>/portraits
"""
import sys, os, glob
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import render

def main():
    a = sys.argv[1:]
    prims, raw = a[0], a[1]
    scale = int(a[a.index('--scale') + 1]) if '--scale' in a else 4
    reps = [a[i + 1] for i, x in enumerate(a) if x == '--replace']
    hd = {}
    for d in reps:                                        # HD replacements by the texture's hash: <W>x<H>_<hash>.png
        for f in glob.glob(os.path.join(d, '*_*.png')):
            hd[os.path.basename(f).split('_')[-1][:16]] = f
    league = a[a.index('--league') + 1] if '--league' in a else None
    work = a[a.index('--faces') + 1] if '--faces' in a else None
    made = {}                                             # texture hash -> player key, for the made faces
    if league and work and os.path.exists(os.path.join(league, 'faces.txt')):
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'faces'))
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
        import facegen, build_faces, ssh, refpack
        params = {}
        for ln in open(os.path.join(league, 'faces_params.txt')):
            q = ln.split()
            if q and not ln.startswith('#'): params[q[0]] = dict(tone=int(q[1]), hair=int(q[2]), style=int(q[3]), facial=int(q[4]), age=float(q[5]))
        for ln in open(os.path.join(league, 'faces.txt')):
            q = ln.split()
            if len(q) < 4 or ln.startswith('#'): continue
            key, name, seed = q[0], q[1], int(q[2])
            sh = ssh.Ssh(refpack.decompress(open(os.path.join(league, 'DATA', 'GHEAD.BIG', name), 'rb').read())).shapes[0]
            h = '%016x' % build_faces.tex_hash(ssh.decode14(sh.pixel_bytes(), 128, 256), sh.palette())
            p = dict(params.get(key, {})); p['head'] = ord(name[1]) - 97
            made[h] = (key, seed, p)
        src4 = facegen.load(work, hd=True)
        cache = os.path.join(raw, '_faces4x'); os.makedirs(cache, exist_ok=True)
        def face4x(h):
            key, seed, p = made[h]
            ph = os.path.join(league, 'faces_4x', key + '.png')
            if os.path.exists(ph): return ph
            f = os.path.join(cache, key + '.png')
            if not os.path.exists(f): Image.fromarray(facegen.make_face(src4, p, seed)[0], 'RGBA').save(f)
            return f
    os.makedirs(raw, exist_ok=True)
    n = 0
    for d in sorted(glob.glob(os.path.join(prims, '*'))):
        key = os.path.basename(d)
        if key.startswith('_') or not os.path.exists(os.path.join(d, 'prims.txt')): continue
        out = os.path.join(raw, key + '_0.png')
        if os.path.exists(out): continue
        hashes = {t['hash'] for t in render.load_prims(d)}
        ov = {h: hd[h] for h in hashes if h in hd}
        for h in hashes:
            if h in made: ov[h] = face4x(h)
        # the area the captures keep: the left panel of the Edit Player close-up
        img = render.render(d, scale, ov, crop=(0, 448 * 0.17, 640 * 0.343, 448 * 0.75), straight=True)
        Image.fromarray(img, 'RGBA').save(out); n += 1
        print('%s (%d HD textures)' % (key, len(ov)), flush=True)
    print('%d portraits rendered' % n)

if __name__ == '__main__': main()
