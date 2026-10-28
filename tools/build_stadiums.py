"""Builds the "2026 ballparks" overlay: mods/stadiums-2026/

Usage: python3 tools/build_stadiums.py <your MVP Baseball 2005 ISO> [out dir = mods/stadiums-2026] [--only CODE]

Two things, both from the data on your disc:
  - names: the parks' current names in the menus and overlays (strings.txt, applied to the game's string tables
    by the runtime);
  - outfield dimensions: where the fences have moved since 2005 - or the club has moved to a new park - the
    outfield is reshaped to today's distances. A stadium is a set of models whose vertices are plain 32-bit
    floats in inches with home plate at the origin and the foul lines along +X (left) and +Z (right), plus text
    files for the walls, collision, shadows and the crowd. Everything from the outfield grass outwards is moved
    radially by the change in fence distance at that angle (interpolated between the five posted distances:
    left line, left-centre, centre, right-centre, right line), so the fence, the warning track, the stands
    behind it and the collision walls stay together. The infield is untouched.
What this cannot do: build new ballparks. The eight clubs that have moved since 2005 (Athletics, Rangers, Twins,
Cardinals, Braves, Marlins, Nationals, Mets) get today's name and fence distances on the building the game has.
Wall heights and the distance numbers painted on the walls are unchanged.
  - the commentators: they recorded each park's 2005 name ("...at Jacobs Field"). Nobody recorded today's names, so
    the renamed parks are taken out of the commentators' index (SPCH_PBP/STDNMHDR.BIG); the game then leaves the park
    out of the introduction, as it does for the parks it has no recording for.
"""
import sys, os, struct, math
sys.path.insert(0, os.path.dirname(__file__))
import isofile, refpack
from bigfile import read_big

args = [a for a in sys.argv[1:] if not a.startswith('--')]
if __name__ == '__main__':
    ISO = args[0]
    OUT = args[1] if len(args) > 1 else 'mods/stadiums-2026'
    ONLY = sys.argv[sys.argv.index('--only') + 1].upper() if '--only' in sys.argv else None
else: ISO = OUT = ONLY = None                                    # used as a library (tools/leaguepack.py): set ISO first

# ---------------------------------------------------------------- what changed
# code of the stadium files (DATA/STADIUM/<code>DAY|NITE|DOME.BIG): change of the posted distance in feet at the
# left line, left-centre, centre, right-centre and right line (today minus 2005), and the source of today's numbers.
PARKS = {
    'SAFE': ((0, -12, -4, -5, 0),    'T-Mobile Park: fences moved in for 2013 (331-378-401-381-326; was 331-390-405-386-326)'),
    'ARLI': ((-3, -18, 7, -7, 1),    'Globe Life Field, 2020 (329-372-407-374-326; Ameriquest Field was 332-390-400-381-325)'),
    'COME': ((-3, 0, -8, 0, 0),      'Comerica Park: centre field brought in for 2023 (342-370-412-365-330; was 345-370-420-365-330)'),
    'KAUF': ((0, -8, 0, -8, 0),      'Kauffman Stadium: alleys moved in for 2026 (330-379-410-379-330; was 330-387-410-387-330)'),
    'METR': ((-4, -8, -3, 0, 1),     'Target Field, 2010 (339-377-404-367-328; the Metrodome was 343-385-408-367-327)'),
    'PETO': ((0, -16, 0, -20, 0),    'Petco Park: alleys moved in 2013 (left alley 386, right alley 391; were 402 and 411)'),
    'SBCP': ((0, -5, -8, -6, 0),     'Oracle Park: 2020 (left-centre 399, centre 391, right-centre 415; were 404, 399, 421)'),
    'MINU': ((0, 4, -27, -3, 0),     "Daikin Park: Tal's Hill removed for 2017 (315-366-409-370-326; was 315-362-436-373-326)"),
    'BUSH': ((6, 3, -2, 3, 5),       'Busch Stadium III, 2006 (336-375-400-375-335; Busch Memorial was 330-372-402-372-330)'),
    'TURN': ((0, 5, -1, -15, -5),    'Truist Park, 2017 (335-385-400-375-325; Turner Field was 335-380-401-390-330)'),
    'PROP': ((14, 1, -4, 2, -10),    'loanDepot park, 2020 fences (344-386-400-387-335; Pro Player Stadium was 330-385-404-385-345)'),
    'RFKS': ((2, -3, -8, -10, 0),    'Nationals Park, 2008 (337-377-402-370-335; RFK Stadium was 335-380-410-380-335)'),
    'SHEA': ((-3, -13, -2, 4, -8),   'Citi Field, 2015 fences (335-358-408-375-330; Shea Stadium was 338-371-410-371-338)'),
    'NETW': ((0, 0, 3, 0, -5),       'Sutter Health Park (330-403-325; the Coliseum was 330-400-330)'),
}
# names: 2005 string -> today's (replaced wherever they appear in the game's string tables)
NAMES = [
    ('Angel Stadium of Anaheim', 'Angel Stadium'),
    ('Network Associates Coliseum™', 'Sutter Health Park'),
    ('SAFECO Field™', 'T-Mobile Park'),
    ('Ameriquest Field in Arlington™', 'Globe Life Field'),
    ('U.S. Cellular Field™', 'Rate Field'),
    ('Jacobs Field™', 'Progressive Field'),
    ('Hubert H. Humphrey Metrodome', 'Target Field'),
    ('SkyDome™', 'Rogers Centre'),
    ('Bank One Ballpark™', 'Chase Field'),
    ('PETCO Park™', 'Petco Park'),
    ('SBC Park™', 'Oracle Park'),
    ('Minute Maid Park™', 'Daikin Park'),
    ('Miller Park™', 'American Family Field'),
    ('Turner Field™', 'Truist Park'),
    ('Pro Player Stadium™', 'loanDepot park'),
    ('RFK Stadium', 'Nationals Park'),
    ('Shea Stadium', 'Citi Field'),
]

# the commentators' recordings of the 2005 names that no longer fit: STDNMHDR.BIG members <id>a / <id>b. A park's id
# is its club's number (team_artid), except RFK Stadium, which is 44 (28 is still Montreal's Olympic Stadium).
SPOKEN_OLD = (2, 3, 4, 5, 6, 9, 14, 15, 19, 22, 23, 26, 27, 29, 44)

if os.environ.get('MVP_TEST_PARK'):                     # testing: "FENW:30,-30,30,-30,0" reshapes only that park, by those feet
    _c, _d = os.environ['MVP_TEST_PARK'].split(':'); PARKS = {_c: (tuple(float(v) for v in _d.split(',')), 'test')}; NAMES = []

# ---------------------------------------------------------------- the reshaping
ANGLES = (0.0, 22.5, 45.0, 67.5, 90.0)
FADE = 12.0            # degrees past each foul line over which the change dies out
START = 0.62           # the stretch begins at this fraction of the fence distance (the outfield grass)
FAR = 9000.0           # inches: beyond this (sky dome, the sun) nothing moves

def smooth(t): t = max(0.0, min(1.0, t)); return t * t * (3 - 2 * t)
def wall_segments(wall_text):
    """base edges of the wall triangles in wall.dat: [((x1, z1), (x2, z2))]"""
    pts = [tuple(float(v) for v in l.split(',')) for l in wall_text.replace('\r', '').split('\n') if l.count(',') == 2]
    segs = set()
    for i in range(0, len(pts) - 2, 3):
        low = [p for p in pts[i:i + 3] if p[1] < 6.0]
        if len(low) == 2:
            a, b = (low[0][0], low[0][2]), (low[1][0], low[1][2])
            if a != b: segs.add((min(a, b), max(a, b)))
    return sorted(segs)
def ray_hit(segs, th):
    """nearest wall edge past 200 ft along the ray at th degrees: (distance, segment, position 0..1 along it) or None"""
    a = math.radians(th); dx, dz = math.cos(a), math.sin(a); best = None
    for sg in segs:
        (x1, z1), (x2, z2) = sg
        ex, ez = x2 - x1, z2 - z1; den = dx * ez - dz * ex
        if abs(den) < 1e-9: continue
        t = (x1 * ez - z1 * ex) / den; u = (x1 * dz - z1 * dx) / den
        if t > 2400 and -1e-6 <= u <= 1 + 1e-6 and (best is None or t < best[0]): best = (t, sg, min(1.0, max(0.0, u)))
    return best
class Warp:
    """Moves a point by the displacement of the fence at its angle. The fence is the wall polyline of wall.dat;
    each of its corners moves radially by the change in posted distance at its angle, and the fence between two
    corners stays straight, so every point on a wall moves exactly as the collision wall does."""
    def __init__(self, deltas_ft, segs):
        self.d = [v * 12.0 for v in deltas_ft]; self.segs = segs; self.cache = {}
    def delta(self, th):
        d = self.d
        if th <= 0: return d[0] * smooth(1 + th / FADE)
        if th >= 90: return d[4] * smooth(1 - (th - 90) / FADE)
        k = min(3, int(th / 22.5)); t = smooth((th - ANGLES[k]) / 22.5)
        return d[k] + (d[k + 1] - d[k]) * t
    def corner(self, p):
        r = math.hypot(p[0], p[1]); dl = self.delta(math.degrees(math.atan2(p[1], p[0])))
        return (p[0] / r * dl, p[1] / r * dl)
    def field(self, th):
        """(fence distance, displacement x, displacement z) at this angle"""
        key = round(th * 20)
        v = self.cache.get(key)
        if v is None:
            q = key / 20.0; h = ray_hit(self.segs, q) if -FADE < q < 90 + FADE else None
            if h is None:
                dl = self.delta(q); a = math.radians(q); v = (4200.0, math.cos(a) * dl, math.sin(a) * dl)
            else:
                t, (p1, p2), u = h; d1, d2 = self.corner(p1), self.corner(p2)
                v = (t, d1[0] + (d2[0] - d1[0]) * u, d1[1] + (d2[1] - d1[1]) * u)
            self.cache[key] = v
        return v
    def __call__(self, x, z):
        r = math.hypot(x, z)
        if r < 1.0 or r > FAR: return x, z
        th = math.degrees(math.atan2(z, x))
        if th <= -FADE or th >= 90 + FADE: return x, z
        f = th * 20; k0 = math.floor(f); w = f - k0                     # between two cached angles
        a, b = self.field(k0 / 20.0), self.field((k0 + 1) / 20.0)
        if abs(a[0] - b[0]) > 600: a = b = (a if w < 0.5 else b)        # a step in the fence (a corner): no blending across it
        rw = a[0] + (b[0] - a[0]) * w; r0 = START * rw
        s = smooth((r - r0) / (rw - r0))
        if s <= 0: return x, z
        return x + (a[1] + (b[1] - a[1]) * w) * s, z + (a[2] + (b[2] - a[2]) * w) * s

def fence_profile(segs):
    """fence distance (inches) for every degree 0..90"""
    prof = [None] * 91
    for deg in range(91):
        h = ray_hit(segs, deg)
        if h: prof[deg] = h[0]
    known = [(i, v) for i, v in enumerate(prof) if v is not None]
    assert len(known) > 20, 'no outfield fence found in wall.dat'
    for i in range(91):                                              # gaps (a foul pole exactly on the line): nearest known
        if prof[i] is None: prof[i] = min(known, key=lambda kv: abs(kv[0] - i))[1]
    return prof

# ---------------------------------------------------------------- models (.ord: ELF header + .data, symbols in the .orl)
def unpacks(data):
    """VIF UNPACK commands in a model's data: (offset of the payload, vn, vl, count)"""
    out, i, n = [], 0, len(data)
    while i + 4 <= n:
        w = struct.unpack_from('<I', data, i)[0]; cmd = (w >> 24) & 0x7f; num = (w >> 16) & 0xff; addr = w & 0xffff
        if (cmd & 0x60) == 0x60 and (addr & 0xc000) == 0xc000 and (addr & 0x3ff) < 0x200:
            vn, vl = (cmd >> 2) & 3, cmd & 3; cnt = num or 256; size = (cnt * ((32 >> vl) // 8) * (vn + 1) + 3) & ~3
            out.append((i + 4, vn, vl, cnt)); i += 4 + size; continue
        i += 4
    return out
def warp_model(ord_bytes, warp):
    d = bytearray(ord_bytes); base = 64                       # .data starts after the ELF header
    data = bytes(d[base:])
    old, new = [], []
    for off, vn, vl, cnt in unpacks(data):
        if vn != 2 or vl != 0: continue                       # positions: three 32-bit floats
        v = list(struct.unpack_from('<%df' % (3 * cnt), data, off))
        for k in range(0, len(v), 3):
            old.append((v[k], v[k + 1], v[k + 2]))
            v[k], v[k + 2] = warp(v[k], v[k + 2]); new.append((v[k], v[k + 1], v[k + 2]))
        struct.pack_into('<%df' % (3 * cnt), d, base + off, *v)
    if not old: return bytes(d), 0, 0
    moved = sum(1 for a, b in zip(old, new) if abs(a[0] - b[0]) + abs(a[2] - b[2]) > 0.01)
    # bounding volumes kept in the data: min(x,y,z,1) max(x,y,z,1) centre(x,y,z) radius. Each is rebuilt from the
    # moved vertices that were inside it (its own vertices are among them).
    boxes = 0
    skip = set()
    for off, vn, vl, cnt in unpacks(data):
        for k in range(off, off + ((cnt * ((32 >> vl) // 8) * (vn + 1) + 3) & ~3), 4): skip.add(k)
    for i in range(0, len(data) - 48 + 1, 4):
        if i in skip: continue
        f = struct.unpack_from('<12f', data, i)
        if f[3] != 1.0 or f[7] != 1.0: continue
        mn, mx, c, rad = f[0:3], f[4:7], f[8:11], f[11]
        if not all(math.isfinite(v) and abs(v) < 1e6 for v in f): continue
        if any(mn[k] > mx[k] for k in range(3)) or rad <= 0: continue
        if any(abs(c[k] - (mn[k] + mx[k]) / 2) > 0.02 * (abs(mx[k] - mn[k]) + 1) for k in range(3)): continue
        inside = [b for a, b in zip(old, new) if all(mn[k] - 0.5 <= a[k] <= mx[k] + 0.5 for k in range(3))]
        if not inside: continue
        nmn = [min(p[k] for p in inside) for k in range(3)]; nmx = [max(p[k] for p in inside) for k in range(3)]
        nmn = [min(nmn[k], mn[k]) if k == 1 else nmn[k] for k in range(3)]; nmx = [max(nmx[k], mx[k]) if k == 1 else nmx[k] for k in range(3)]
        nc = [(nmn[k] + nmx[k]) / 2 for k in range(3)]
        nr = max(rad, 0.5 * math.sqrt(sum((nmx[k] - nmn[k]) ** 2 for k in range(3))))
        struct.pack_into('<12f', d, base + i, nmn[0], nmn[1], nmn[2], 1.0, nmx[0], nmx[1], nmx[2], 1.0, nc[0], nc[1], nc[2], nr); boxes += 1
    # the model's own box: six floats (min, max) that enclose every vertex
    lo = [min(p[k] for p in old) for k in range(3)]; hi = [max(p[k] for p in old) for k in range(3)]
    for i in range(0, min(len(data), 1 << 20) - 24 + 1, 4):
        if i in skip: continue
        f = struct.unpack_from('<6f', data, i)
        if all(abs(f[k] - lo[k]) < 0.01 * (abs(lo[k]) + 1) and abs(f[3 + k] - hi[k]) < 0.01 * (abs(hi[k]) + 1) for k in range(3)):
            nlo = [min(p[k] for p in new) for k in range(3)]; nhi = [max(p[k] for p in new) for k in range(3)]
            struct.pack_into('<6f', d, base + i, *(nlo + nhi)); boxes += 1
    return bytes(d), moved, boxes

# ---------------------------------------------------------------- text data
def fmt(v): return '%.6f' % v
def warp_xyz_lines(text, warp, first=0):
    """every line that is exactly three numbers (x, y, z) from line `first` on"""
    nl = '\r\n' if '\r\n' in text else '\n'; out = []
    for n, l in enumerate(text.split(nl)):
        p = l.split(',')
        if n >= first and len(p) == 3:
            try: x, y, z = float(p[0]), float(p[1]), float(p[2])
            except ValueError: out.append(l); continue
            x2, z2 = warp(x, z)
            if (x2, z2) != (x, z):
                dec = len(p[0].split('.')[1]) if '.' in p[0] else 6
                l = '%.*f,%s,%.*f' % (dec, x2, p[1], dec, z2)
        out.append(l)
    return nl.join(out)
def warp_fields(text, warp, layouts):
    """layouts: {number of fields: (index of x, index of z)}; for crowd.dat's header lines also the box"""
    nl = '\r\n' if '\r\n' in text else '\n'; out = []
    for l in text.split(nl):
        p = l.split(',')
        if len(p) in layouts:
            try:
                for ix, iz in layouts[len(p)]:
                    x, z = float(p[ix]), float(p[iz]); x2, z2 = warp(x, z)
                    if (x2, z2) != (x, z): p[ix], p[iz] = fmt(x2), fmt(z2)
                l = ','.join(p)
            except ValueError: pass
        out.append(l)
    return nl.join(out)
def warp_crowd(text, warp):
    """crowd.dat: 'count,minx,miny,minz,maxx,maxy,maxz' then count lines of a 3x4 matrix, position, extras"""
    nl = '\r\n' if '\r\n' in text else '\n'; lines = text.split(nl); out = list(lines); i = 1
    while i < len(lines):
        p = lines[i].split(',')
        if len(p) != 7: i += 1; continue
        cnt = int(p[0]); xs, zs = [], []
        for k in range(i + 1, min(len(lines), i + 1 + cnt)):
            q = lines[k].split(',')
            if len(q) < 15: continue
            x, z = float(q[12]), float(q[14]); x2, z2 = warp(x, z); xs.append(x2 - x); zs.append(z2 - z)
            if (x2, z2) != (x, z): q[12], q[14] = fmt(x2), fmt(z2); out[k] = ','.join(q)
        if xs:                                               # the section's box moves by the range its people moved
            b = [float(v) for v in p[1:]]
            b[0] += min(0.0, min(xs)); b[3] += max(0.0, max(xs)); b[2] += min(0.0, min(zs)); b[5] += max(0.0, max(zs))
            x0, z0 = warp(b[0], b[2]); x1, z1 = warp(b[3], b[5])
            b[0] = min(b[0], x0); b[2] = min(b[2], z0); b[3] = max(b[3], x1); b[5] = max(b[5], z1)
            out[i] = p[0] + ',' + ','.join(fmt(v) for v in b)
        i += 1 + cnt
    return nl.join(out)

MODELS = ['mstadium', 'mfield0', 'mfield1', 'mfield2', 'crowd', 'cammen', 'alphablend', 'camaligned', 'jumbo', 'net', 'mshadow',
          'removable', 'colouradd', 'useraligned', 'track', 'background0', 'background1', 'background2',
          'remcrowd0', 'remcrowd1', 'remcrowd2', 'remcrowd3', 'remcrowd4', 'remcrowd5']

def build_park(path, deltas):
    big = isofile.read_file(ISO, path); ents = {n: (o, s) for n, o, s in read_big(big)}
    def get(name):
        o, s = ents[name]; c = big[o:o + s]
        return refpack.decompress(c) if c[:2] in (b'\x10\xfb', b'\x11\xfb', b'\x90\xfb', b'\x91\xfb') else c
    wall = get('wall.dat').decode('latin1')
    segs = wall_segments(wall); prof = fence_profile(segs); warp = Warp(deltas, segs)
    out = {}; moved = 0; boxes = 0
    for m in MODELS:
        if m + '.ord' not in ents: continue
        new, mv, bx = warp_model(get(m + '.ord'), warp)
        if mv: out[m + '.ord'] = new; moved += mv; boxes += bx
    texts = {'wall.dat': lambda t: warp_xyz_lines(t, warp), 'coll.dat': lambda t: warp_xyz_lines(t, warp),
             'shadow.dat': lambda t: warp_xyz_lines(t, warp), 'render.ifo': lambda t: warp_xyz_lines(t, warp),
             'stadium.ifo': lambda t: warp_fields(warp_xyz_lines(t, warp, first=1), warp, {4: [(1, 3)]}),
             'flags.csv': lambda t: warp_fields(t, warp, {8: [(4, 6)]}), 'crowd.dat': lambda t: warp_crowd(t, warp)}
    for name, fn in texts.items():
        if name not in ents: continue
        t = get(name).decode('latin1'); t2 = fn(t)
        if t2 != t: out[name] = t2.encode('latin1')
    after = fence_profile(wall_segments(out['wall.dat'].decode('latin1'))) if 'wall.dat' in out else prof
    return out, prof, after, moved, boxes

def main():
    with open(ISO, 'rb') as f:                                # the stadium files on this disc
        f.seek(16 * 2048); pvd = f.read(2048); root = pvd[156:190]; files = {}
        isofile._walk(f, struct.unpack('<I', root[2:6])[0], struct.unpack('<I', root[10:14])[0], '', files)
    log = []
    for code, (deltas, why) in sorted(PARKS.items()):
        if ONLY and code != ONLY: continue
        for path in sorted(p for p in files if p.startswith('DATA/STADIUM/' + code)):
            out, before, after, moved, boxes = build_park(path, deltas)
            d = os.path.join(OUT, path); os.makedirs(d, exist_ok=True)
            for name, data in out.items(): open(os.path.join(d, name), 'wb').write(refpack.compress(data))
            pick = lambda prof: '-'.join('%d' % round(prof[a] / 12) for a in (1, 22, 45, 68, 89))
            log.append('%-22s %s -> %s ft   (%d vertices moved, %d files)' % (path.split('/')[-1], pick(before), pick(after), moved, len(out)))
            print(log[-1]); sys.stdout.flush()
        log.append('    ' + why)
    if not ONLY:
        os.makedirs(OUT, exist_ok=True)
        with open(os.path.join(OUT, 'strings.txt'), 'w', encoding='utf-8') as f:
            f.write('# 2005 text<TAB>today\'s text: replaced wherever it appears in the game\'s string tables (FEENG.LOC, IGENG.LOC)\n')
            for a, b in NAMES: f.write('%s\t%s\n' % (a, b))
        big = bytearray(isofile.read_file(ISO, 'DATA/AUDIO/SPCH_PBP/STDNMHDR.BIG')); off = 16; hidden = 0
        gone = {'%02d%s.hdr' % (i, ab) for i in SPOKEN_OLD for ab in 'ab'}
        for _ in range(struct.unpack('>I', big[8:12])[0]):        # NNx.hdr -> NNx.hdx: same length, same order, not found
            off += 8; e = big.index(b'\0', off)
            if big[off:e].decode('latin1') in gone: big[e - 1] = ord('x'); hidden += 1
            off = e + 1
        os.makedirs(os.path.join(OUT, 'DATA', 'AUDIO', 'SPCH_PBP'), exist_ok=True)
        open(os.path.join(OUT, 'DATA', 'AUDIO', 'SPCH_PBP', 'STDNMHDR.BIG'), 'wb').write(big)
        log.append('commentary: the 2005 names of %d renamed parks are no longer spoken (%d recordings hidden)' % (len(SPOKEN_OLD), hidden)); print(log[-1])
    open(os.path.join(OUT, 'stadiums_changes.txt' if not ONLY else 'stadiums_changes_%s.txt' % ONLY), 'w').write('\n'.join(log) + '\n')
    print('wrote', OUT)

if __name__ == '__main__': main()
