"""Builds the "2026 uniforms" overlay: mods/uniforms-2026/

Usage: python3 tools/build_uniforms.py <your MVP Baseball 2005 ISO> [out dir = mods/uniforms-2026] [--sheets dir]
Needs numpy (and cv2 for --sheets).

What it does, and what it does not. Every team in the game has a wardrobe: home, road and alternate uniforms
plus throwbacks, each a file of textures (MODELS.BIG u<team><slot>.ssh) with a jersey thumbnail for the menus
(UNIFORMS.BIG). This tool only re-uses and recolours that art, read from your disc:
  - where a club's look today is one of the game's throwbacks (Blue Jays, Reds, Astros, Brewers, Twins, Mets,
    the Orioles' cartoon-bird caps, the Royals' powder blue), that set becomes the default home/road uniform;
  - where the colours changed (Rays navy, Padres brown and gold, Diamondbacks red, Marlins blue), the palettes
    are recoloured;
  - where a wordmark no longer fits (Cleveland, the Athletics' and Marlins' road jerseys), the jersey that
    still fits is used for both home and road, on white or grey cloth, and Cleveland's caps are the plain "C".
The logos in the menus and on the in-game cards (LOGOS.BIG) follow: recoloured for the recoloured clubs, and
Cleveland's Chief Wahoo logos become the block "C" of its 1970s cap, cropped from that cap's texture.
Nothing is drawn by hand: no current MLB logo or wordmark is reproduced, so scripts and cap logos are the
2005-or-earlier ones even where a team has redesigned them since. The throwback slots themselves stay as they are.
"""
import sys, os, struct, colorsys
sys.path.insert(0, os.path.dirname(__file__))
import isofile, refpack, ssh
from bigfile import read_big
import numpy as np

args = [a for a in sys.argv[1:] if not a.startswith('--')]
ISO = args[0]
OUT = args[1] if len(args) > 1 else 'mods/uniforms-2026'
SHEETS = sys.argv[sys.argv.index('--sheets') + 1] if '--sheets' in sys.argv else None

class Big:
    def __init__(self, path):
        self.path = path; self.data = isofile.read_file(ISO, path); self.ents = {n: (o, s) for n, o, s in read_big(self.data)}
    def get(self, name):
        o, s = self.ents[name]; c = self.data[o:o + s]
        return refpack.decompress(c) if c[:2] in (b'\x10\xfb', b'\x11\xfb', b'\x90\xfb', b'\x91\xfb') else c
    def has(self, name): return name in self.ents
MODELS = Big('DATA/MODELS.BIG'); PREVIEWS = Big('DATA/FRONTEND/UNIFORMS.BIG'); LOGOS = Big('DATA/FRONTEND/LOGOS.BIG')

# ---------------------------------------------------------------- shape groups and the low-detail atlas
FRONT = ['jerf', 'jers', 'jert', 'jrfl', 'jrsl', 'jrtl']                        # jersey fronts (body types, long sleeves)
CLOTH = FRONT + ['jerk', 'jrkl', 'slvl', 'slvr', 'lega', 'legb', 'legc', 'legd']   # everything made of the jersey's cloth
CAPS = ['hat ', 'hlm1', 'hhlm']                                                  # cap, batting helmet, helmet sheet
LOD = {'front': (0, 0, 64, 64), 'back': (0, 64, 64, 64), 'legs': (64, 0, 32, 64), 'hat': (96, 0, 32, 32),
       'helmets': (96, 64, 32, 32), 'helmet': (96, 96, 32, 16), 'sleeve': (96, 112, 32, 16)}   # x, y, w, h in llod (128 x 128)

def hsv(c): return colorsys.rgb_to_hsv(c[0] / 255.0, c[1] / 255.0, c[2] / 255.0)
def weights(shape):
    """pixels per palette entry, in file (palette) order"""
    cnt = np.bincount(shape.indices().ravel(), minlength=256)
    w = np.zeros(256);
    for i in range(256): w[ssh.csm1(i)] += cnt[i]
    return w

# ---------------------------------------------------------------- palette operations
def recolour(shape, rules):
    """rules: [(hue_lo, hue_hi, min_sat, (r, g, b)[, v_max, sat_sure])]: palette entries with a hue in the window
    take the target's hue; saturation and brightness are scaled so that the average colour of the class becomes
    the target. Pixels that blend into the cloth keep their brightness (no dark fringes). v_max / sat_sure keep
    the faint tint of shaded white cloth out of a class: an entry brighter than v_max only counts when its
    saturation reaches sat_sure."""
    pal = shape.palette()
    if pal is None: return 0
    w = weights(shape) + 1e-3; n = 0
    for rule in rules:
        lo, hi, smin, target = rule[:4]; vmax = rule[4] if len(rule) > 4 else 1.0; sure = rule[5] if len(rule) > 5 else 2.0
        ht, st, vt = hsv(target)
        cls = []
        for i, c in enumerate(pal):
            h, s, v = hsv(c); hd = h * 360
            inside = lo <= hd <= hi if lo <= hi else (hd >= lo or hd <= hi)
            if inside and s >= smin and v > 0.04 and (v <= vmax or s >= sure): cls.append((i, h, s, v))
        if not cls: continue
        ws = np.array([w[i] for i, *_ in cls]); ss = np.array([s for _, _, s, _ in cls]); vs = np.array([v for *_, v in cls])
        core = ss >= np.percentile(ss, 50)                                  # the solid colour, not its soft edges
        s_ref = float((ss[core] * ws[core]).sum() / ws[core].sum()); v_ref = float((vs[core] * ws[core]).sum() / ws[core].sum())
        for i, h, s, v in cls:
            a = min(1.0, s / s_ref)                                         # how much of this entry is the colour
            s2 = min(1.0, s * st / s_ref); v2 = min(1.0, v * (1 + a * (vt / v_ref - 1)))
            r, g, b = colorsys.hsv_to_rgb(ht, s2, v2)
            pal[i] = [r * 255, g * 255, b * 255, pal[i][3]]; n += 1
    shape.set_palette(pal)
    return n

def cloth_curve(src, ref):
    """brightness mapping (256-entry table) that gives src's neutral cloth the brightness distribution of ref's"""
    def cdf(shape):
        pal = shape.palette(); w = weights(shape); hist = np.zeros(256)
        for i, c in enumerate(pal):
            h, s, v = hsv(c)
            if s < 0.16 and v > 0.25: hist[int(round(v * 255))] += w[i]
        return hist
    hs, hr = cdf(src), cdf(ref)
    if hs.sum() < 500 or hr.sum() < 500: return None
    cs, cr = np.cumsum(hs) / hs.sum(), np.cumsum(hr) / hr.sum()
    lut = np.searchsorted(cr, np.minimum(cs, 0.999), side='left').astype(np.float64)   # quantile matching
    lo, hi = int(np.argmax(cs > 0.02)), int(np.argmax(cs > 0.98))           # outside the measured range: continue the ends
    lut[:lo] = np.arange(lo) * (lut[lo] / max(1, lo)); lut[hi:] = np.minimum(255, lut[hi] + (np.arange(hi, 256) - hi) * 0.5)
    return lut
def apply_cloth(shape, lut):
    pal = shape.palette()
    if pal is None or lut is None: return
    for i, c in enumerate(pal):
        h, s, v = hsv(c)
        if s < 0.30 and v > 0.25:
            k = lut[int(round(v * 255))] / 255.0 / max(v, 1e-3)
            k = 1 + (k - 1) * (1 - min(1.0, max(0.0, (s - 0.16) / 0.14)))     # fades out towards coloured trim (no speckles in tinted cloth)
            pal[i] = [min(255, c[0] * k), min(255, c[1] * k), min(255, c[2] * k), c[3]]
    shape.set_palette(pal)

# ---------------------------------------------------------------- writing an 8-bit shape from an image
def quantize(rgba):
    """(h, w, 4) uint8 (PS2 alpha) -> (indices (h, w), palette 256 x 4): exact when there are at most 256 colours"""
    h, w, _ = rgba.shape
    flat = rgba.reshape(-1, 4); cols, inv = np.unique(flat, axis=0, return_inverse=True)
    if len(cols) <= 256:
        pal = np.zeros((256, 4), np.uint8); pal[:len(cols)] = cols
        return inv.reshape(h, w).astype(np.uint8), pal
    # median cut on the distinct colours, weighted by use
    cnt = np.bincount(inv.ravel(), minlength=len(cols)).astype(np.float64)
    boxes = [np.arange(len(cols))]
    while len(boxes) < 256:
        k = max(range(len(boxes)), key=lambda b: (len(boxes[b]) > 1) * (np.ptp(cols[boxes[b]].astype(int), axis=0).max() + 1) * cnt[boxes[b]].sum())
        b = boxes[k]
        if len(b) < 2: break
        ch = int(np.argmax(np.ptp(cols[b].astype(int), axis=0)))
        order = b[np.argsort(cols[b][:, ch], kind='stable')]; cw = np.cumsum(cnt[order]); cut = int(np.searchsorted(cw, cw[-1] / 2)) + 1
        cut = min(max(cut, 1), len(order) - 1)
        boxes[k] = order[:cut]; boxes.append(order[cut:])
    pal = np.zeros((256, 4), np.uint8); remap = np.zeros(len(cols), np.int32)
    for k, b in enumerate(boxes):
        pal[k] = np.round((cols[b] * cnt[b][:, None]).sum(0) / cnt[b].sum()); remap[b] = k
    return remap[inv].reshape(h, w).astype(np.uint8), pal
def set_image8(shape, rgba_ps2):
    assert shape.type == 2
    idx, pal = quantize(rgba_ps2)
    shape.set_pixel_bytes(idx.tobytes())
    filepal = [[0, 0, 0, 0]] * 256
    filepal = [list(pal[i]) for i in range(256)]
    out = [None] * 256
    for i in range(256): out[ssh.csm1(i)] = filepal[i]
    shape.set_palette(out)
def image_ps2(shape):
    """(h, w, 4) with the file's own alpha values"""
    pal = np.array(shape.palette(), np.uint8); lut = pal[[ssh.csm1(i) for i in range(256)]]
    return lut[shape.indices()]

# ---------------------------------------------------------------- recipes
# slot -> dict(base=slot, cloth=slot whose cloth brightness to take, front=slot the jersey fronts come from,
#              caps=slot the cap and helmets come from, sleeves='right' (left sleeve = copy of the right one),
#              recolour=[rules])
NAVY, BROWN, GOLD, RED_AZ, BLUE_MIA = (40, 52, 84), (64, 46, 36), (190, 150, 60), (120, 34, 44), (38, 110, 150)
RAYS = [(140, 200, 0.12, NAVY)]
PADRES = [(205, 250, 0.15, BROWN, 0.48, 0.30)]                  # navy, but not the bluish shade of white cloth
PADRES_TRIM = PADRES + [(35, 60, 0.12, GOLD)]                      # the khaki trim on white and navy becomes gold
DBACKS = [(220, 310, 0.25, RED_AZ)]                             # purple and the blue-violet of the road script
MARLINS = [(150, 200, 0.12, BLUE_MIA)]
GUARD_CAP = [(195, 250, 0.15, (38, 48, 78))]                       # the 1970s cap's blue to navy
TEAMS = {
    1:  ('Athletics',    {'b': dict(base='b', front='a')}),
    5:  ('Cleveland',    {'a': dict(base='j', cloth='a', caps='g', sleeves='right', cap_recolour=GUARD_CAP, preview_copy=[(110, 38, 18, 22, 108, 60)]),
                          'b': dict(base='j', caps='g', sleeves='right', cap_recolour=GUARD_CAP, preview_copy=[(110, 38, 18, 22, 108, 60)]),
                          'c': dict(same='a'), 'd': dict(same='b')}),
    7:  ('Royals',       {'d': dict(base='h')}),
    8:  ('Twins',        {'a': dict(base='i', caps='a')}),
    9:  ('Orioles',      {'a': dict(base='a', caps='g'), 'b': dict(base='b', caps='g')}),
    12: ('Rays',         {s: dict(base=s, recolour=RAYS) for s in 'abcdo'}),
    13: ('Blue Jays',    {'a': dict(base='g', caps='h'), 'b': dict(base='h'), 'c': dict(same='a')}),
    14: ('Diamondbacks', {s: dict(base=s, recolour=DBACKS) for s in 'abcdo'}),
    17: ('Padres',       {'a': dict(base='a', recolour=PADRES_TRIM), 'b': dict(base='b', recolour=PADRES),
                          'c': dict(base='c', recolour=PADRES_TRIM), 'o': dict(base='o', recolour=PADRES_TRIM)}),
    20: ('Reds',         {'a': dict(base='g'), 'b': dict(base='h')}),
    21: ('Astros',       {'a': dict(base='h'), 'b': dict(base='j', caps='h'), 'c': dict(same='a'), 'd': dict(same='b')}),
    22: ('Brewers',      {'a': dict(base='i', caps='g'), 'b': dict(base='j', caps='g'), 'c': dict(base='g')}),
    26: ('Marlins',      {'a': dict(base='a', recolour=MARLINS), 'b': dict(base='a', cloth='b', recolour=MARLINS),
                          'c': dict(same='a'), 'd': dict(base='d', recolour=MARLINS), 'o': dict(base='o', recolour=MARLINS)}),
    28: ('Mets',         {'a': dict(base='g')}),
}

# ---------------------------------------------------------------- the logos in the menus and on the in-game cards
# LOGOS.BIG l<team>.ssh: l = wordmark 128x64, a = logo 128x64, i and g = logos 64x64, j = 32x16, d and e = colour bars.
# Recoloured clubs get the same recolouring (the logos are saturated, so the targets are the full colours);
# Cleveland's Chief Wahoo logos become the block "C" of the 1970s cap, taken from that cap's texture.
LOGO_RULES = {
    12: [(140, 200, 0.12, (20, 52, 110))],
    14: [(220, 310, 0.20, (167, 25, 48))],
    17: [(205, 250, 0.15, (70, 46, 30)), (35, 60, 0.20, (255, 196, 37))],
    26: [(150, 200, 0.12, (0, 163, 224))],
}
def recolour_pixels(shape, rules):
    """the palette recolouring for a 32-bit shape: every pixel is its own palette entry"""
    w, h = shape.size; px = np.frombuffer(shape.pixel_bytes(), np.uint8).reshape(-1, 4).copy()
    for rule in rules:
        lo, hi, smin, target = rule[:4]; ht, st, vt = hsv(target)
        cls = []
        for i, c in enumerate(px):
            hh, ss, vv = hsv(c); hd = hh * 360
            if (lo <= hd <= hi if lo <= hi else (hd >= lo or hd <= hi)) and ss >= smin and vv > 0.04: cls.append((i, ss, vv))
        if not cls: continue
        ss = np.array([c[1] for c in cls]); vs = np.array([c[2] for c in cls]); core = ss >= np.percentile(ss, 50)
        s_ref, v_ref = float(ss[core].mean()), float(vs[core].mean())
        for i, sat, v in cls:
            a = min(1.0, sat / s_ref); r, g, b = colorsys.hsv_to_rgb(ht, min(1.0, sat * st / s_ref), min(1.0, v * (1 + a * (vt / v_ref - 1))))
            px[i, :3] = (round(r * 255), round(g * 255), round(b * 255))
    shape.set_pixel_bytes(px.tobytes())
def cleveland_c(size_w, size_h, c_h):
    """the block C from the 1970s cap (u005g.ssh, shape 'hat '), on a transparent canvas, PS2 alpha"""
    import cv2
    hat = ssh.Ssh(MODELS.get('u005g.ssh'))['hat '].rgba()[21:41, 79:117, :3].astype(np.float32)
    up = cv2.resize(hat, None, fx=8, fy=8, interpolation=cv2.INTER_CUBIC)
    hsv_ = cv2.cvtColor(np.clip(up, 0, 255).astype(np.uint8), cv2.COLOR_RGB2HSV).astype(np.float32)
    hue, sat, val = hsv_[..., 0] * 2, hsv_[..., 1] / 255, hsv_[..., 2] / 255
    blue = ((hue > 180) & (hue < 260) & (sat > 0.18)).astype(np.float32)             # the cap itself
    alpha = cv2.GaussianBlur(1 - blue, (0, 0), 2.0)
    alpha = np.clip((alpha - 0.35) / 0.3, 0, 1)
    red = np.clip((sat - 0.25) / 0.25, 0, 1) * (((hue < 25) | (hue > 335)).astype(np.float32))
    red = cv2.GaussianBlur(red, (0, 0), 1.5)
    col = np.zeros(up.shape, np.float32); col[...] = (244, 244, 244)
    col = col * (1 - red[..., None]) + np.array((214, 30, 52), np.float32) * red[..., None]       # the club's red, white edge
    ys, xs = np.where(alpha > 0.5); y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    col, alpha = col[y0:y1, x0:x1], alpha[y0:y1, x0:x1]
    c_w = int(round(c_h * 0.92))                                                      # the cap's texture is stretched sideways
    col = cv2.resize(col, (c_w, c_h), interpolation=cv2.INTER_AREA); alpha = cv2.resize(alpha, (c_w, c_h), interpolation=cv2.INTER_AREA)
    out = np.zeros((size_h, size_w, 4), np.float32); out[..., :3] = (214, 30, 52)
    oy, ox = (size_h - c_h) // 2, (size_w - c_w) // 2
    out[oy:oy + c_h, ox:ox + c_w, :3] = col; out[oy:oy + c_h, ox:ox + c_w, 3] = alpha * 128
    return np.clip(np.round(out), 0, 255).astype(np.uint8)
def paint_c(img, x0, y0, x1, y1, c_h):
    """the block C centred in the rectangle [x0:x1, y0:y1] of an opaque RGBA image"""
    c = cleveland_c(x1 - x0, y1 - y0, c_h).astype(np.float32); a = c[..., 3:4] / 128.0
    img[y0:y1, x0:x1, :3] = np.clip(np.round(img[y0:y1, x0:x1, :3] * (1 - a) + c[..., :3] * a), 0, 255).astype(np.uint8)
def cleveland_park(cram):
    """Jacobs Field's textures (cram.ssh of JACODAY/JACONITE.BIG) with the block C wherever the 2005 logo is painted:
    the outfield wall (indd, ads2), the scoreboard emblem (inda) and the concourse signs and banners (add4)"""
    f = ssh.Ssh(cram)
    def flat(img, x0, y0, x1, y1, src):                     # fill a rectangle with the median colour of another one
        sx0, sy0, sx1, sy1 = src
        img[y0:y1, x0:x1, :3] = np.median(img[sy0:sy1, sx0:sx1, :3].reshape(-1, 3), axis=0).astype(np.uint8)
    def put14(name, img):                                   # with the texture's own palette: what was not painted stays as it is
        sh = f[name]; pal = np.array(sh.palette(), np.float32)[[ssh.csm1(i) for i in range(256)]]
        want = img.astype(np.float32); want[..., 3] = pal[sh.indices()][..., 3]
        data, _ = ssh.encode14(sh.indices(), pal, want); sh.set_pixel_bytes(data)
    sh = f['inda']; set_image8(sh, cleveland_c(64, 64, 54))
    img = f['indd'].rgba().copy(); img[..., 3] = 128
    flat(img, 2, 2, 126, 62, (2, 2, 20, 62)); paint_c(img, 34, 4, 94, 60, 52); put14('indd', img)
    img = f['ads2'].rgba().copy(); img[..., 3] = 128
    img[:, 64:128] = img[:, 128:192]; paint_c(img, 68, 8, 124, 62, 46); put14('ads2', img)
    img = f['add4'].rgba().copy(); img[..., 3] = 128
    flat(img, 3, 2, 29, 29, (3, 2, 7, 6)); paint_c(img, 3, 2, 29, 29, 22)                 # the sign
    for x in (32, 80):                                                                    # the two banners
        flat(img, x + 1, 1, x + 15, 28, (x + 1, 27, x + 15, 30)); paint_c(img, x + 1, 4, x + 15, 26, 14)
    flat(img, 112, 9, 126, 23, (100, 10, 110, 22)); paint_c(img, 112, 9, 126, 23, 11)     # the small one on the pink wall
    put14('add4', img)
    return f.build()
def build_logos(team):
    name = 'l%03d.ssh' % team
    if not LOGOS.has(name): return None
    f = ssh.Ssh(LOGOS.get(name)); changed = False
    if team in LOGO_RULES:
        for s_ in f.shapes:
            if s_.type in (2, 14): changed |= recolour(s_, LOGO_RULES[team]) > 0
            elif s_.type == 5: recolour_pixels(s_, LOGO_RULES[team]); changed = True
    if team == 5:
        for nm, (w, h, ch) in {'a005': (128, 64, 56), 'i005': (64, 64, 52), 'g005': (64, 64, 44)}.items():
            sh = f[nm]; data, pal = ssh.image14(cleveland_c(w, h, ch)); sh.set_pixel_bytes(data); sh.set_palette(pal)
        f['j005'].set_pixel_bytes(cleveland_c(32, 16, 14).tobytes())
        changed = True
    return f.build() if changed else None

def ufile(team, slot): return 'u%03d%s.ssh' % (team, slot)
def preview_shape(team, slot):
    name = '%03d.ssh' % team
    if not PREVIEWS.has(name): return None
    f = ssh.Ssh(PREVIEWS.get(name))
    for s in f.shapes:
        if s.name == '%03d%s' % (team, slot): return s
    return None

def build_slot(team, slot, r):
    """-> (uniform file bytes, font file bytes or None, preview Shape or None, notes)"""
    base = r['base']; notes = []
    f = ssh.Ssh(MODELS.get(ufile(team, base)))
    prev = preview_shape(team, base)
    have = set(f.names())
    if base != slot: notes.append('the "%s" set' % base)
    # cloth: white <-> grey
    if 'cloth' in r:
        ref = ssh.Ssh(MODELS.get(ufile(team, r['cloth'])))
        lut = cloth_curve(f['jerk'], ref['jerk'])
        for nm in CLOTH:
            if nm in have: apply_cloth(f[nm], lut)
        apply_cloth(f['llod'], lut)
        pr = preview_shape(team, r['cloth'])
        if prev is not None and pr is not None: apply_cloth(prev, cloth_curve(prev, pr))
        notes.append('cloth as "%s"' % r['cloth'])
    lod = image_ps2(f['llod']); lod_changed = False
    def paste(donor_lod, key):
        nonlocal lod_changed
        x, y, w, h = LOD[key]; lod[y:y + h, x:x + w] = donor_lod[y:y + h, x:x + w]; lod_changed = True
    # jersey fronts from another set, on this set's cloth
    if 'front' in r:
        d = ssh.Ssh(MODELS.get(ufile(team, r['front'])))
        lut = cloth_curve(d['jerk'], f['jerk'])
        for nm in FRONT:
            if nm in have and nm in d.names():
                s = d[nm]; apply_cloth(s, lut)
                for k, t in enumerate(f.shapes):
                    if t.name == nm: f.shapes[k] = s
        apply_cloth(d['llod'], lut); paste(image_ps2(d['llod']), 'front')
        dp = preview_shape(team, r['front'])
        if prev is not None and dp is not None: apply_cloth(dp, cloth_curve(dp, prev)); prev = dp
        notes.append('jersey front of "%s"' % r['front'])
    if 'caps' in r:
        d = ssh.Ssh(MODELS.get(ufile(team, r['caps'])))
        if 'cap_recolour' in r:
            for nm in CAPS + ['llod']:
                if nm in d.names(): recolour(d[nm], r['cap_recolour'])
        for nm in CAPS:
            if nm in have and nm in d.names():
                for k, t in enumerate(f.shapes):
                    if t.name == nm: f.shapes[k] = d[nm]
        dl = image_ps2(d['llod'])
        for key in ('hat', 'helmets', 'helmet'): paste(dl, key)
        notes.append('caps of "%s"' % r['caps'])
    if r.get('sleeves') == 'right' and 'slvl' in have and 'slvr' in have:
        for k, t in enumerate(f.shapes):
            if t.name == 'slvl': s = ssh.Shape('slvr', f['slvr'].chunk); s.rename('slvl'); f.shapes[k] = s
        notes.append('plain left sleeve')
    if lod_changed: set_image8(f['llod'], lod)
    if 'recolour' in r:
        n = 0
        for s in f.shapes:
            if s.name not in ('shoe', 'lace', 'bglb', 'bglt') and s.type in (2, 14): n += recolour(s, r['recolour'])
        if prev is not None: recolour(prev, r['recolour'])
        notes.append('recoloured')
    if 'preview_copy' in r and prev is not None and prev.type == 14:      # cover a patch in the thumbnail with the cloth next to it
        idx = prev.indices()
        for sx, sy, w, h, dx, dy in r['preview_copy']: idx[dy:dy + h, dx:dx + w] = idx[sy:sy + h, sx:sx + w].copy()
        pal = np.array(prev.palette(), np.float32)[[ssh.csm1(i) for i in range(256)]]
        data, _ = ssh.encode14(idx, pal); prev.set_pixel_bytes(data)
    font = MODELS.get('f%03d%s.ssh' % (team, base)) if base != slot and MODELS.has('f%03d%s.ssh' % (team, base)) else None
    return f.build(), font, prev, notes

def main():
    members = {}        # (archive, member) -> bytes
    log = []
    for team, (name, slots) in sorted(TEAMS.items()):
        done = {}; pfile = '%03d.ssh' % team
        pv = ssh.Ssh(PREVIEWS.get(pfile)) if PREVIEWS.has(pfile) else None
        for slot, r in slots.items():
            if not MODELS.has(ufile(team, slot)): continue
            if 'same' in r:
                if r['same'] not in done: continue
                data, font, prev, notes = done[r['same']]; notes = ['same as "%s"' % r['same']]
                if font is None and MODELS.has('f%03d%s.ssh' % (team, r['same'])): font = MODELS.get('f%03d%s.ssh' % (team, r['same']))
                if prev is not None: prev = ssh.Shape(prev.name, prev.chunk)
            else:
                data, font, prev, notes = build_slot(team, slot, r); done[slot] = (data, font, prev, notes)
            members[('DATA/MODELS.BIG', ufile(team, slot))] = data
            if font is not None and MODELS.has('f%03d%s.ssh' % (team, slot)): members[('DATA/MODELS.BIG', 'f%03d%s.ssh' % (team, slot))] = font
            if pv is not None and prev is not None:
                for k, t in enumerate(pv.shapes):
                    if t.name == '%03d%s' % (team, slot): p2 = ssh.Shape(prev.name, prev.chunk); p2.rename(t.name); pv.shapes[k] = p2
            log.append('%-13s %s: %s' % (name, slot, ', '.join(notes)))
        if pv is not None: members[('DATA/FRONTEND/UNIFORMS.BIG', pfile)] = pv.build()
        lg = build_logos(team)
        if lg is not None: members[('DATA/FRONTEND/LOGOS.BIG', 'l%03d.ssh' % team)] = lg; log.append('%-13s menu and card logos %s' % (name, 'the block C' if team == 5 else 'recoloured'))
    for arc in ('DATA/STADIUM/JACODAY.BIG', 'DATA/STADIUM/JACONITE.BIG'):          # Cleveland's park: the block C on the walls
        park = Big(arc)
        if park.has('cram.ssh'): members[(arc, 'cram.ssh')] = cleveland_park(park.get('cram.ssh'))
    log.append('Cleveland     the park: the block C on the outfield wall, the scoreboard and the concourse signs')
    for (arc, nm), data in members.items():
        d = os.path.join(OUT, arc); os.makedirs(d, exist_ok=True)
        open(os.path.join(d, nm), 'wb').write(refpack.compress(data))
    open(os.path.join(OUT, 'uniforms_changes.txt'), 'w').write('\n'.join(log) + '\n')
    print('\n'.join(log)); print('wrote %d files to %s' % (len(members), OUT))
    if SHEETS: sheets(members)

def sheets(members):
    import cv2
    os.makedirs(SHEETS, exist_ok=True)
    def tile(data, label):
        f = ssh.Ssh(data)
        def g(nm, w, h):
            if nm not in f.names(): return np.zeros((h, w, 3), np.uint8)
            im = f[nm].rgba(); a = im[..., 3:4] / 255.0
            return cv2.resize((im[..., :3] * a + 90 * (1 - a)).astype(np.uint8)[..., ::-1], (w, h), interpolation=cv2.INTER_AREA)
        t = np.vstack([np.hstack([g('jerf', 128, 128), g('jerk', 128, 128)]), np.hstack([g('hat ', 128, 64), g('hlm1', 128, 64)]),
                       np.hstack([g('slvl', 128, 64), g('lega', 64, 64), g('llod', 64, 64)])])
        cv2.putText(t, label, (2, 12), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (0, 255, 255), 1)
        return t
    rows = []
    for team in sorted(TEAMS):
        key = ('DATA/FRONTEND/LOGOS.BIG', 'l%03d.ssh' % team)
        if key not in members: continue
        for data in (LOGOS.get(key[1]), members[key]):
            f = ssh.Ssh(data); tiles = []
            for s_ in f.shapes:
                im = s_.rgba(); a = im[..., 3:4] / 255.0; t = np.full((64, 128, 3), 110, np.uint8)
                rgb = (im[..., :3] * a + 110 * (1 - a)).astype(np.uint8)[..., ::-1]; t[:min(64, rgb.shape[0]), :rgb.shape[1]] = rgb[:64]
                tiles.append(t)
            rows.append(np.hstack(tiles))
    if rows: cv2.imwrite(os.path.join(SHEETS, 'logos.png'), np.vstack(rows))
    for team, (name, slots) in sorted(TEAMS.items()):
        row0, row1 = [], []
        for slot in slots:
            if ('DATA/MODELS.BIG', ufile(team, slot)) not in members: continue
            row0.append(tile(MODELS.get(ufile(team, slot)), '2005 ' + slot)); row1.append(tile(members[('DATA/MODELS.BIG', ufile(team, slot))], '2026 ' + slot))
        pf = members.get(('DATA/FRONTEND/UNIFORMS.BIG', '%03d.ssh' % team))
        if pf:
            f0, f1 = ssh.Ssh(PREVIEWS.get('%03d.ssh' % team)), ssh.Ssh(pf)
            for src, row in ((f0, row0), (f1, row1)):
                ims = [cv2.resize(src['%03d%s' % (team, s)].rgba()[..., [2, 1, 0]], (64, 64)) for s in slots if '%03d%s' % (team, s) in src.names()][:8]
                t = np.zeros((256, 128, 3), np.uint8)
                for k, im in enumerate(ims): t[(k // 2) * 64:(k // 2) * 64 + 64, (k % 2) * 64:(k % 2) * 64 + 64] = im
                row.append(t)
        if row0: cv2.imwrite(os.path.join(SHEETS, '%03d_%s.png' % (team, name.replace(' ', '_'))), np.vstack([np.hstack(row0), np.hstack(row1)]))

if __name__ == '__main__': main()
