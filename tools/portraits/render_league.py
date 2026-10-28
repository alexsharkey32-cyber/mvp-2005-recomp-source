"""Portraits for a whole league without capturing every player: the game's close-up of each generic head model is
recorded once (tools/faces/templates/f<face>_s<style>: the Edit Player close-up of one player whose face and hair
style were set to every value), and every player is drawn from the template of his head with his own textures put
in: his made face (tools/faces/build_faces.py, at 4x, or a face from a photo), his club's cap and jersey (the
league's own home uniform: era uniforms, custom clubs) and the skin of his arms and neck in his own tone.

    python3 tools/portraits/render_league.py ISO MOD_DIR TEMPLATES FACES_WORK RAW_OUT [--all] [--club CODE] [--uniforms MOD]
    python3 tools/portraits/build.py ISO RAW_OUT MOD_DIR/portraits          (or mods/portraits-2026 for the modern rosters)

Big-leaguers only unless --all. The template club's textures are found among the recorded ones by matching
pixels with that club's home uniform on the disc; the matched ones are swapped for the same pieces of his club.
"""
import sys, os, glob
import numpy as np
from PIL import Image
here = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, here); sys.path.insert(0, os.path.join(here, '..')); sys.path.insert(0, os.path.join(here, '..', 'faces'))
import render, mvpdb, isofile, refpack, ssh, facegen
from bigfile import read_big

TEMPLATE_CLUB = 10                                   # the templates were recorded on a Red Sox player (u010a)
UNIFORM_SHAPES = ('hat ', 'jerf', 'jers', 'jert', 'jerk', 'slvl', 'slvr', 'jrfl', 'jrsl', 'jrtl', 'jrkl', 'chst', 'lega', 'legb', 'legc', 'legd', 'aslv')

def unpack(c): return refpack.decompress(c) if c[1:2] == b'\xfb' else c

class Uniforms:
    """home uniform shapes per club: the league's overlay first (era uniforms, custom clubs), else the disc"""
    def __init__(self, iso, mod):
        self.d = isofile.read_file(iso, 'DATA/MODELS.BIG'); self.e = {n: (o, s) for n, o, s in read_big(self.d)}
        self.dirs = [os.path.join(mod, 'art', 'DATA', 'MODELS.BIG'), os.path.join(mod, 'DATA', 'MODELS.BIG')] + \
                    [os.path.join(x, 'DATA', 'MODELS.BIG') for i, x in enumerate(sys.argv) if sys.argv[i - 1] == '--uniforms']   # e.g. mods/uniforms-2026
        self.cache = {}
    def shapes(self, club, slot='a'):
        k = (club, slot)
        if k not in self.cache:
            name = 'u%03d%s.ssh' % (club, slot); raw = None
            for dd in self.dirs:
                p = os.path.join(dd, name)
                if os.path.exists(p): raw = open(p, 'rb').read(); break
            if raw is None:
                o, s = self.e[name]; raw = self.d[o:o + s]
            out = {}
            for sh in ssh.Ssh(unpack(raw)).shapes:
                if sh.type in (2, 14) and sh.palette() is not None and sh.name not in out: out[sh.name] = sh.rgba()
            self.cache[k] = out
        return self.cache[k]

def match_uniform(tdir, tmpl_shapes):
    """recorded texture hash -> uniform shape name, by comparing pixels (same size, small difference)"""
    out = {}
    for f in glob.glob(os.path.join(tdir, '*.png')):
        h = os.path.basename(f)[:-4]
        if len(h) != 16: continue
        im = np.asarray(Image.open(f).convert('RGBA')).astype(np.float32)
        best = None
        for n, px in tmpl_shapes.items():
            if px.shape[:2] != im.shape[:2]: continue
            d = np.abs(px[..., :3].astype(np.float32) - im[..., :3]).mean()
            if best is None or d < best[0]: best = (d, n)
        if best and best[0] < 6: out[h] = best[1]
    return out

def main():
    a = [x for i, x in enumerate(sys.argv) if i > 0 and not x.startswith('--') and sys.argv[i - 1] not in ('--club', '--uniforms')]
    iso, mod, tmpl_dir, work, raw = a[:5]
    only = sys.argv[sys.argv.index('--club') + 1] if '--club' in sys.argv else None
    os.makedirs(raw, exist_ok=True)
    T, _ = mvpdb.read_database(os.path.join(mod, 'DATA', 'DATABASE', 'DATABASE.BIG'))
    team = {k: v for k, v in T['team.dat'].rows}; tc = T['team.dat'].cols
    org = T['org.dat']; mlb = {'%09x' % (int(v[org.col('org_team_mlb')]) & 0xfffffffff) for k, v in org.rows}
    club_of = {}
    for k, v in T['roster.dat'].rows:
        if v[0] in mlb or '--all' in sys.argv: club_of.setdefault(v[1], v[0])
    params = {}
    for ln in open(os.path.join(mod, 'faces_params.txt')):
        q = ln.split()
        if q and not ln.startswith('#'): params[q[0]] = dict(tone=int(q[1]), hair=int(q[2]), style=int(q[3]), facial=int(q[4]), age=float(q[5]))
    faces = []
    for ln in open(os.path.join(mod, 'faces.txt')):
        q = ln.split()
        if len(q) >= 3 and not ln.startswith('#'): faces.append((q[0], q[1], int(q[2])))
    U = Uniforms(iso, mod)
    hd_cache = {}
    sess = None
    def up4(rgba, k):
        """a uniform piece at 4x (Real-ESRGAN general model), cached"""
        nonlocal sess
        if k in hd_cache: return hd_cache[k]
        import cv2, onnxruntime as ort, hashlib
        cdir = os.path.join(work, 'uniform_hd'); os.makedirs(cdir, exist_ok=True)       # kept across leagues: the same pieces recur
        cf = os.path.join(cdir, hashlib.md5(rgba.tobytes()).hexdigest() + '.png')
        if os.path.exists(cf):
            hd_cache[k] = np.asarray(Image.open(cf).convert('RGBA')).astype(np.float32) / 255; return hd_cache[k]
        if sess is None:
            root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
            cands = [os.environ.get('MVP_ESRGAN', ''), os.path.join(root, 'hd', 'realesr-general-x4v3.onnx'),
                     os.path.join(root, '..', 'hd', 'realesr-general-x4v3.onnx'), '/home/claude/hd/realesr-general-x4v3.onnx']
            m = next(c for c in cands if c and os.path.exists(c))
            sess = ort.InferenceSession(m, providers=['CPUExecutionProvider'])
        inp = sess.get_inputs()[0].name
        rgb = rgba[..., :3].astype(np.float32) / 255
        y = sess.run(None, {inp: rgb.transpose(2, 0, 1)[None]})[0][0].transpose(1, 2, 0)
        a = cv2.resize(rgba[..., 3].astype(np.float32) / 255, (rgba.shape[1] * 4, rgba.shape[0] * 4), interpolation=cv2.INTER_CUBIC)
        hd_cache[k] = np.dstack([np.clip(y, 0, 1), np.clip(a, 0, 1)]).astype(np.float32)
        Image.fromarray((hd_cache[k] * 255 + 0.5).astype(np.uint8), 'RGBA').save(cf)
        return hd_cache[k]
    tmpl_shapes = U.shapes(TEMPLATE_CLUB)
    src4 = facegen.load(work, hd=True)
    tinfo = {}
    def template(head, style):
        s = style if style in (0, 1, 2, 9) else 4
        name = 'f%02d_s%d' % (head + 1, s)
        if name not in tinfo:
            d = os.path.join(tmpl_dir, name)
            if not os.path.exists(os.path.join(d, 'prims.txt')): d = os.path.join(tmpl_dir, 'f%02d_s4' % (head + 1))
            tris = render.load_prims(d)
            cnt = {}
            for t in tris:
                if t['w'] == 128 and t['h'] == 256: cnt[t['hash']] = cnt.get(t['hash'], 0) + 1
            face_h = max(cnt, key=cnt.get)
            uni = match_uniform(d, tmpl_shapes)
            # the game turns part of some textures around when it loads them (the cap: columns 70-126 mirrored, so the
            # logo reads right on the model): which column of the disc's picture each recorded column shows
            cmap = {}
            for h, nm in uni.items():
                rec = np.asarray(Image.open(os.path.join(d, h + '.png')).convert('RGB')).astype(np.float32); ref = tmpl_shapes[nm][..., :3].astype(np.float32)
                m = np.array([int(np.argmin(np.abs(ref - rec[:, x:x + 1]).mean((0, 2)))) for x in range(rec.shape[1])])
                if (m != np.arange(len(m))).sum() > 4: cmap[h] = m
            skin = [t['hash'] for t in tris if t['w'] == 128 and t['h'] == 64 and t['hash'] not in uni]
            skin_h = max(set(skin), key=skin.count) if skin else None
            tinfo[name] = (d, face_h, uni, skin_h, cmap)
        return tinfo[name]
    n = 0
    for key, gname, seed in faces:
        if key not in club_of: continue
        tk = club_of[key]; tr = team.get(tk)
        if tr is None: continue
        if only and tr[tc.index('team_short_name')] != only: continue
        club = int(tr[tc.index('team_artid')]) - 1
        if not 0 <= club < 30: continue
        out = os.path.join(raw, key[-8:] + '_0.png')                         # the runtime's key: the id's low 32 bits
        if os.path.exists(out): continue
        p = dict(params.get(key, {})); head = ord(gname[1]) - 97; p['head'] = head
        d, face_h, uni, skin_h, cmap = template(head, p.get('style', 4))
        ph = os.path.join(mod, 'faces_4x', key + '.png')
        face = np.asarray(Image.open(ph).convert('RGBA')) if os.path.exists(ph) else facegen.make_face(src4, p, seed)[0]
        ov = {face_h: face.astype(np.float32) / 255}
        cs = U.shapes(club)
        for h, nm in uni.items():
            if nm in cs:
                px = cs[nm][:, cmap[h]] if h in cmap else cs[nm]
                ov[h] = up4(np.ascontiguousarray(px), (mod, club, nm, h in cmap)) if '--no-hd' not in sys.argv else px.astype(np.float32) / 255
        if skin_h:                                       # arms and neck in his skin: the template's tinted to his face's cheeks
            sk = np.asarray(Image.open(os.path.join(d, skin_h + '.png')).convert('RGBA')).astype(np.float32) / 255
            want = face[110 * 4:140 * 4, 28 * 4:48 * 4, :3].reshape(-1, 3).astype(np.float32).mean(0) / 255
            have = sk[..., :3].reshape(-1, 3).mean(0)
            sk = sk.copy(); sk[..., :3] = np.clip(sk[..., :3] * (want / np.maximum(have, 1e-3)), 0, 1)
            ov[skin_h] = sk
        img = render.render(d, 4, ov, crop=(0, 448 * 0.17, 640 * 0.343, 448 * 0.75), straight=True)
        Image.fromarray(img, 'RGBA').save(out); n += 1
        if n % 25 == 0: print(n, key, flush=True)
    print('%d portraits rendered -> %s' % (n, raw))

if __name__ == '__main__': main()
