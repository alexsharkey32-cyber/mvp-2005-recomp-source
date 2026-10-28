"""Reads every texture out of the game's archives and names it the way the runtime will (WIDTHxHEIGHT_hash.png),
so an HD pack can be built for the whole game without playing through it with texture dumping on.

    python3 tools/hd/extract_textures.py <MVP Baseball 2005 ISO> <work dir> [mods dir]

Writes <work dir>/src/*.png (the original textures) and <work dir>/index.tsv, one line per texture:
    name  tier  model  tiling  grain  category  source  rect
(rect: WxH of the image inside a texture that is bigger than it, see below; - otherwise)
The runtime hashes a texture's pixels as the graphics chip sees them, so the same is done here: the EA shape
files (.ssh, in the .BIG archives, RefPack-compressed) are decoded - including the 3-bit "type 14" ones the
game unpacks on the graphics chip - their palettes applied, and the hash computed (a few lines of C, compiled
on first use; a slow Python version is the fallback).
Menu artwork and fonts are not power-of-two sized; the game uses them as the next power-of-two size and the runtime
takes the rest of such a texture as empty (gs.cpp, "how much of a texture was really uploaded"), so they are written
here as the image in the top left corner of an empty canvas.
Tiers say how much a texture matters (tools/hd/build_pack.py works through them in order):
  1 players: today's uniforms (mods), every MLB club's home and away set, skin, bats, gloves, helmets
  2 the generic heads today's rosters use; all ballparks
  3 MLB alternates and throwbacks; menu art (logos, uniform and ballpark previews)
  4 minor-league and classic uniforms, the 2005 players' own heads, heads only the 2005 rosters use
  5 everything else that can show up (2005 portraits, heads no roster uses yet: future draft picks)
Jersey names and numbers are put together by the game from letter textures at run time; the same is done here
for every number and every player's name in his club's lettering (tiers 2-5, category "lettering").
"""
import sys, os, re, struct, ctypes, subprocess, tempfile, glob, collections
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import bigfile, ssh, refpack, mvpdb

# ---------------------------------------------------------------- the runtime's texture hash (gs.cpp, tex_get)
_C = r'''
#include <stdint.h>
#include <stddef.h>
static uint64_t mix64(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); return h * 0xff51afd7ed558ccdull; }
uint64_t texhash(const uint32_t* px, uint32_t w, uint32_t h) {
    uint64_t hash = mix64(w, h);
    for (size_t i = 0; i < (size_t)w * h; i++) hash = (hash ^ px[i]) * 0x100000001b3ull;
    return hash;
}
'''
def _load_hash(work):
    so = os.path.join(work, 'texhash.' + ('dylib' if sys.platform == 'darwin' else 'so'))
    try:
        if not os.path.exists(so):
            src = os.path.join(work, 'texhash.c'); open(src, 'w').write(_C)
            subprocess.check_call(['cc', '-O2', '-shared', '-fPIC', '-o', so, src])
        lib = ctypes.CDLL(so); lib.texhash.restype = ctypes.c_uint64
        return lambda px: lib.texhash(px.ctypes.data_as(ctypes.c_void_p), ctypes.c_uint32(px.shape[1]), ctypes.c_uint32(px.shape[0]))
    except Exception as e:
        print('no C compiler (%s): hashing in Python, this will take a long time' % e)
        M = (1 << 64) - 1
        def mix(h, v): h ^= (v + 0x9e3779b97f4a7c15 + (h << 6) + (h >> 2)) & M; return (h * 0xff51afd7ed558ccd) & M
        def slow(px):
            h = mix(px.shape[1], px.shape[0])
            for c in px.view(np.uint32).ravel().tolist(): h = ((h ^ c) * 0x100000001b3) & M
            return h
        return slow

_LUT = [ssh.csm1(i) for i in range(256)]
def raw_rgba(s):
    """the texels as the GS decodes them: RGBA, alpha 0..0x80"""
    w, h = s.size
    if s.type == 5: return np.frombuffer(s.pixel_bytes(), np.uint8).reshape(h, w, 4).copy()
    pal = s.palette()
    if pal is None:
        # A palette with fewer than 256 colours (n x 1 in its header; most field, wall and overlay textures): the
        # game loads it into the start of an otherwise empty 16 x 16 block, read in the usual CSM1 order.
        for p, t, size in s.sections():
            if t == 0x21:
                n = struct.unpack_from('<H', s.chunk, p + 4)[0]
                if not 0 < n < 256 or p + 16 + 4 * n > len(s.chunk): break
                full = np.zeros((256, 4), np.uint8); full[:n] = np.frombuffer(bytes(s.chunk[p + 16:p + 16 + 4 * n]), np.uint8).reshape(n, 4)
                return full[_LUT][s.indices()]
        raise ValueError('no palette')
    return np.array(pal, np.uint8)[_LUT][s.indices()]

# ---------------------------------------------------------------- the disc
def iso_files(path):
    f = open(path, 'rb')
    def rd(lba, n): f.seek(lba * 2048); return f.read(n)
    pvd = rd(16, 2048); root = pvd[156:190]; out = []
    def walk(lba, size, path):
        d = rd(lba, size); o = 0
        while o < size:
            l = d[o]
            if l == 0: o = (o // 2048 + 1) * 2048; continue
            e = d[o:o + l]; lb, sz = struct.unpack_from('<I', e, 2)[0], struct.unpack_from('<I', e, 10)[0]; nm = e[33:33 + e[32]]
            if nm not in (b'\0', b'\1'):
                n = nm.decode().split(';')[0]
                if e[25] & 2: walk(lb, sz, path + n + '/')
                else: out.append((path + n, lb, sz))
            o += l
    walk(struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0], '')
    return f, out

def head_files(db_bytes_or_path):
    """generic head textures (GHEAD.BIG g<head><hair colour><style><facial hair>) and real faces a roster uses"""
    T, _ = mvpdb.read_database(db_bytes_or_path)
    at = T['attrib.dat']; ci = {c: i for i, c in enumerate(at.cols)}
    heads, faces = set(), set()
    for k, v in at.rows:
        f = int(v[ci['playerattrib_face']])
        if 901 <= f <= 915: heads.add('g' + ''.join(chr(97 + x) for x in (f - 901, int(v[ci['playerattrib_haircolour']]), int(v[ci['playerattrib_hairstyle']]), int(v[ci['playerattrib_facialhair']]))) + '.ssh')
        else: faces.add('c%03d.ssh' % f)
    return heads, faces

def classify(path, heads_now, heads_2005, faces_now):
    """-> (tier, model, tiling, grain, category) or None to leave the texture alone"""
    mod = path.startswith('mod:'); q = path.split('/DATA/', 1)[1] if mod else path[5:]
    parts = q.split('/'); top = parts[0]; leaf = parts[1] if len(parts) > 1 else ''
    if top == 'MODELS.BIG':
        m = re.match(r'([a-z])(\d\d\d)(\w?)\.ssh', leaf)
        if not m: return (1, 'best', 0, 1, 'uniform') if leaf in ('umpire.ssh', 'umpirec.ssh', 'uniform.ssh') else None
        kind, team, slot = m.group(1), int(m.group(2)), m.group(3)
        if kind == 'f': return None                                   # letters and digits: composed at run time
        if kind == 'u': return (1 if mod or (team < 30 and slot in 'ab') else 3 if team < 30 else 4, 'best', 0, 1, 'uniform')
        if kind == 'c': return (2 if leaf in faces_now else 4, 'best', 0, 1, 'face')
        return (1, 'best', 0, 1, 'equipment')
    if top == 'GHEAD.BIG': return (2 if leaf in heads_now else 4 if leaf in heads_2005 else 5, 'best' if leaf in heads_now or leaf in heads_2005 else 'photo', 0, 1, 'head')
    if top == 'INITSTAZ.BIG' or top == 'INITPRGZ.BIG': return (1, 'best', 0, 1, 'equipment')
    if top == 'STADIUM': return (2, 'photo', 1, 1, 'ballpark')
    if top == 'FRONTEND':
        if leaf == 'PORTRAIT.BIG': return (5, 'photo', 0, 0, 'portrait')
        return (3, 'auto', 0, 0, 'menu')
    return (3, 'auto', 0, 0, 'menu')

# ---------------------------------------------------------------- jersey names and numbers
# The game builds these at run time from the letter files (MODELS.BIG f<team><slot>.ssh: A-Z 16x32, digits 32x64):
#   number: a 64x64 texture, two digit cells side by side, or one at x = 16 on a field of palette entry 0; the outer
#           row and columns of each cell are overwritten with the cell's corner colour;
#   name:   a 256x32 texture filled with palette entry 0, upper-case letters at a pitch of 16 pixels, centred in the
#           first 224 pixels (14 letters at most; anything that is not a letter leaves its cell empty).
# Left-handers are the right-handers' animations mirrored, so the game mirrors what they wear to keep it readable:
# numbers are flipped whole, names within their 224 pixels, and the badge on a cap (x 70-126, y 16-63) or a helmet
# (x 2-62) is flipped in place.
# So the exact textures - and their names - can be made in advance for every font and every player.
class Font:
    def __init__(self, sf):
        self.glyph = {}
        for sh in sf.shapes:
            try: self.glyph[sh.name] = np.array(sh.palette(), np.uint8)[_LUT][sh.indices()]
            except Exception: pass
        self.pal0 = np.array(sf.shapes[0].palette(), np.uint8)[_LUT][0]
    # Numbers are put together in 32-bit colour on a canvas cleared to (19, 19, 19, alpha 0), and the frame of each
    # digit cell is overwritten with that same value. (Most fonts have it as the digits' own corner colour, which is
    # what an earlier version used; where the corners differ - f025a - or a single digit leaves the sides empty,
    # only the constant gives the texture the game makes.)
    CLEAR = np.array([19, 19, 19, 0], np.uint8)
    def number(self, n):
        s = str(n); cells = []
        for ch in s:
            c = self.glyph['zig' + ch].copy()
            if c.shape[:2] != (64, 32): return None
            bg = self.CLEAR; c[0, :] = bg; c[63, :] = bg; c[:, 0] = bg; c[:, 31] = bg; cells.append(c)
        if len(s) == 2: return np.concatenate(cells, 1)
        out = np.zeros((64, 64, 4), np.uint8); out[:] = self.CLEAR; out[:, 16:48] = cells[0]; return out
    def name(self, text):
        out = np.zeros((32, 256, 4), np.uint8); out[:] = self.pal0
        x0 = (224 - 16 * len(text)) // 2
        for i, ch in enumerate(text):
            g = self.glyph.get(ch + '___')
            if g is not None and g.shape[:2] == (32, 16): out[:, x0 + 16 * i:x0 + 16 * i + 16] = g
        return out

def lettering(read_member, mod_fonts, rosters, add):
    """read_member(name) -> bytes of a MODELS.BIG member or None; mod_fonts {name: bytes}; rosters [(db path, is_current)];
    add(pixels, tier, category, source)"""
    def fonts_of(team, slot):
        out = []
        for raw, tag in ((mod_fonts.get('f%03d%s.ssh' % (team, slot)), 'mod'), (read_member('f%03d%s.ssh' % (team, slot)), 'disc')):
            if raw is None: continue
            if raw[:2] == b'\x10\xfb': raw = refpack.decompress(raw)
            try: out.append((Font(ssh.Ssh(raw)), tag))
            except Exception: pass
        return out
    cache = {}
    def fonts(team, slot):
        if (team, slot) not in cache: cache[(team, slot)] = fonts_of(team, slot)
        return cache[(team, slot)]
    slots = 'abcdghijko'
    for team in range(126):
        for slot in slots:
            for font, tag in fonts(team, slot):
                if 'zig0' not in font.glyph: continue
                tier = 2 if team < 30 and (slot in 'ab' or tag == 'mod') else 3 if team < 30 else 4
                for n in range(100):
                    px = font.number(n)
                    if px is None: continue
                    add(px, tier, 'lettering', 'number %d f%03d%s' % (n, team, slot))
                    add(px[:, ::-1], tier, 'lettering', 'number %d f%03d%s mirrored' % (n, team, slot))
    for db, current in rosters:
        T, _ = mvpdb.read_database(db)
        teams = T['team.dat']; tci = teams.col('team_artid'); lci = teams.col('team_league')
        art = {k: (int(v[tci]) - 1, int(v[lci])) for k, v in teams.rows}
        org = {}
        if 'org.dat' in T:                                            # minor-league clubs -> their MLB club (call-ups)
            for k, v in T['org.dat'].rows:
                for x in v[2:5]:
                    try: org['%09x' % (int(x) & 0xffffffff)] = k
                    except ValueError: pass
        at = T['attrib.dat']; bi, ti = at.col('playerattrib_bats'), at.col('playerattrib_throws')
        names = {k: v[1] for k, v in at.rows}
        lefty = {k for k, v in at.rows if v[bi] != '0' or v[ti] != '0'}       # bats or throws left (or both ways)
        for k, v in T['roster.dat'].rows:
            if v[0] not in art or v[1] not in names: continue
            team, league = art[v[0]]
            text = re.sub('[^A-Z]', ' ', names[v[1]].upper()).rstrip()
            if not text.strip() or len(text) > 14: continue
            mlb = league in (0, 1)
            wear = [(team, s, (2 if mlb and s in 'ab' else 3 if mlb else 4) + (0 if current else 1)) for s in slots]
            if v[0] in org and org[v[0]] in art: wear += [(art[org[v[0]]][0], s, 4) for s in 'ab']
            for t, slot, tier in wear:
                for font, tag in fonts(t, slot):
                    if 'A___' not in font.glyph: continue
                    px = font.name(text)
                    add(px, min(tier, 5), 'lettering', 'name %s f%03d%s' % (text, t, slot))
                    if v[1] in lefty:
                        px = px.copy(); px[:, :224] = px[:, :224][:, ::-1]
                        add(px, min(tier, 5), 'lettering', 'name %s f%03d%s mirrored' % (text, t, slot))

def main():
    iso, work = sys.argv[1], sys.argv[2]; mods = sys.argv[3] if len(sys.argv) > 3 else 'mods'
    os.makedirs(os.path.join(work, 'src'), exist_ok=True)
    texhash = _load_hash(work)
    f, files = iso_files(iso)
    def read(p):
        for n, lb, sz in files:
            if n == p: f.seek(lb * 2048); return f.read(sz)
    tmp = tempfile.NamedTemporaryFile(delete=False, suffix='.BIG'); tmp.write(read('DATA/DATABASE/DATABASE.BIG')); tmp.close()
    heads_2005, faces_2005 = head_files(tmp.name); os.unlink(tmp.name)
    heads_now, faces_now = set(heads_2005), set(faces_2005); roster_dbs = []
    for db in glob.glob(os.path.join(mods, '*', 'DATA', 'DATABASE', 'DATABASE.BIG')):
        h, c = head_files(db)
        if len(h) > 100: heads_now, faces_now = h, c; roster_dbs.append(db)   # a roster mod (the commentary overlay has no players)
    heads_2005 -= heads_now
    index, stats = {}, collections.Counter()
    def add_texture(px, cls, source):
        """px: RGBA as the GS has it (alpha 0..0x80 and a little above)"""
        h0, w0 = px.shape[:2]; rect = '-'
        W, H = 1, 1
        while W < w0: W *= 2
        while H < h0: H *= 2
        if (W, H) != (w0, h0):                                        # used as the next power-of-two size, the rest empty
            canvas = np.zeros((H, W, 4), np.uint8); canvas[:h0, :w0] = px; px = canvas; rect = '%dx%d' % (w0, h0)
        px = np.ascontiguousarray(px); h, w = px.shape[:2]
        name = '%dx%d_%016x.png' % (w, h, texhash(px))
        e = index.get(name)
        if e is None or cls[0] < e[0]:
            index[name] = cls + (source, rect)
            if e is None:
                px = px.copy()                                       # PNG alpha: 255 = the GS's 0x80; textures that go well above
                if px[..., 3].max() <= 0xa0: px[..., 3] = np.minimum(255, px[..., 3].astype(np.int32) * 2)   # it keep the GS value (as the runtime does)
                Image.fromarray(px, 'RGBA').save(os.path.join(work, 'src', name), compress_level=1)
    def add_shapes(raw, path):
        cls = classify(path, heads_now, heads_2005, faces_now)
        if cls is None: return
        try: sf = ssh.Ssh(raw)
        except Exception: stats['unreadable shape files'] += 1; return
        for sh in sf.shapes:
            w, h = sh.size
            if sh.type not in (2, 5, 14) or w < 8 or h < 8 or w > 1024 or h > 1024: continue
            if (w & (w - 1) or h & (h - 1)) and cls[4] != 'menu': continue      # odd sizes: menu artwork only
            if w * h < 256 and not (w & (w - 1) or h & (h - 1)): continue
            try: px = np.ascontiguousarray(raw_rgba(sh))
            except Exception: stats['shapes without a palette'] += 1; continue
            if px.shape[:2] != (h, w): continue
            if not px[..., :3].any() and not px[..., 3].any(): continue
            add_texture(px, cls, path + ':' + sh.name.strip())
            if cls[4] == 'uniform' and (w, h) == (128, 64) and sh.name.strip() in ('hat', 'hlm1', 'hlm2', 'hhlm'):
                m = px.copy()
                if sh.name.strip() == 'hat': m[16:64, 70:127] = px[16:64, 70:127][:, ::-1]
                else: m[:, 2:63] = px[:, 2:63][:, ::-1]
                if not np.array_equal(m, px): add_texture(m, cls, path + ':' + sh.name.strip() + ' mirrored')
    def handle(data, path):
        if data[:2] == b'\x10\xfb':
            try: data = refpack.decompress(data)
            except Exception: return
        if data[:4] in (b'BIGF', b'BIG4'):
            try: ents = bigfile.read_big(data)
            except Exception: return
            for name, o, s in ents: handle(data[o:o + s], path + '/' + name)
        elif data[:4] == b'SHPS': add_shapes(data, path)
    only_lettering = '--lettering' in sys.argv
    if only_lettering:                                                # add names and numbers to an existing index
        for l in open(os.path.join(work, 'index.tsv')):
            p = l.rstrip('\n').split('\t'); index[p[0]] = (int(p[1]), p[2], int(p[3]), int(p[4]), p[5], p[6], p[7] if len(p) > 7 else '-')
    for n, lb, sz in files:
        if only_lettering: break
        if not n.startswith('DATA/') or n.startswith('DATA/AUDIO') or 'MOVIES' in n or not n.upper().endswith(('.BIG', '.SSH')): continue
        before = len(index); f.seek(lb * 2048); handle(f.read(sz), n)
        if len(index) > before: print('%-36s %6d textures' % (n, len(index) - before), flush=True)
    before = len(index)
    for mp in sorted(glob.glob(os.path.join(mods, '*', 'DATA', '**', '*'), recursive=True)):
        if os.path.isfile(mp) and mp.lower().endswith(('.ssh', '.big')): handle(open(mp, 'rb').read(), 'mod:' + os.path.relpath(mp, mods).replace(os.sep, '/'))
    print('%-36s %6d textures' % ('mods', len(index) - before))
    # names and numbers
    before = len(index)
    models = read('DATA/MODELS.BIG'); ments = {n: (o, s_) for n, o, s_ in bigfile.read_big(models)}
    mod_fonts = {os.path.basename(p): open(p, 'rb').read() for p in glob.glob(os.path.join(mods, '*', 'DATA', 'MODELS.BIG', 'f*.ssh'))}
    tmp = tempfile.NamedTemporaryFile(delete=False, suffix='.BIG'); tmp.write(read('DATA/DATABASE/DATABASE.BIG')); tmp.close()
    rosters = [(db, True) for db in roster_dbs] + [(tmp.name, not roster_dbs)]
    lettering(lambda n: models[ments[n][0]:ments[n][0] + ments[n][1]] if n in ments else None, mod_fonts, rosters,
              lambda px, tier, cat, src: add_texture(px, (tier, 'photo', 0, 0, cat), src))
    os.unlink(tmp.name)
    print('%-36s %6d textures' % ('jersey names and numbers', len(index) - before))
    with open(os.path.join(work, 'index.tsv'), 'w') as out:
        for name, e in sorted(index.items(), key=lambda kv: (kv[1][0], kv[1][4], kv[1][5], kv[0])):
            out.write('%s\t%d\t%s\t%d\t%d\t%s\t%s\t%s\n' % ((name,) + e))
    open(os.path.join(work, 'src', '.complete'), 'w').write('%d\n' % len(index))    # finish_pack.sh: nothing left to read
    tiers = collections.Counter(); units = collections.Counter()
    for name, e in index.items():
        w, h = (int(x) for x in name.split('_')[0].split('x')); tiers[(e[0], e[4])] += 1; units[(e[0], e[4])] += w * h / 16384
    for k in sorted(tiers): print('tier %d %-10s %6d textures  (%6d x 128x128)' % (k[0], k[1], tiers[k], units[k]))
    print(dict(stats))

if __name__ == '__main__': main()
