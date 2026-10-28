"""Uniforms for clubs the game never had: a club's colours, its wordmark, its cap logo -> a complete uniform set in
the game's own format (MODELS.BIG u<team><slot>.ssh, the number/letter font f<team><slot>.ssh and the menu
thumbnail in UNIFORMS.BIG), built on one of the game's uniforms as the pattern.

    python3 tools/uniform_maker.py ISO SPEC.json OUTDIR [--sheet out.png]

SPEC.json: {"team": 12, "slot": "a", "pattern": "u010a.ssh" (optional: the set whose cut, folds and piping to use;
default the slot's own), "cloth": "#f2efe6", "trim": "#10503c", "undershirt": "#10503c", "pants": (cloth),
"wordmark": "path.png", "crown": "#10503c", "brim": (crown), "button": (crown), "cap_logo": "path.png",
"helmet": (crown), "helmet_logo": (cap_logo), "numbers": "#10503c", "numbers_outline": "#ebc85a",
"pinstripes": true/false (keep the pattern's pinstripes, in the trim colour), "jacket": (crown)}

How: the pattern's textures are taken apart - cloth (one colour, shaded by its folds), piping and pinstripes (thin
lines), the wordmark and logos (solid shapes), the undershirt at the collar - the wordmark and logos are painted
out, the cloth is painted in the new colour with the same folds, the lines in the trim colour, and the new
wordmark / logo is put where the old one was, shaded by the cloth under it. The cap and helmets: the crown in the
crown colour, the logo on the front panel. Body-type variants of the jersey (jers, jert) and the mirrored
long-sleeve ones (jr*l) get the same treatment. The low-detail atlas (llod) is made again from the new textures.
"""
import sys, os, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np, cv2
from PIL import Image
import ssh

FRONTS = ['jerf', 'jers', 'jert']; FRONTS_L = ['jrfl', 'jrsl', 'jrtl']; BACKS = ['jerk', 'jrkl']
SLEEVES = ['slvl', 'slvr']; PANTS = ['lega', 'legb', 'legc', 'legd']
LOD = {'front': (0, 0, 64, 64), 'back': (0, 64, 64, 64), 'legs': (64, 0, 32, 64), 'hat': (96, 0, 32, 32),
       'helmets': (96, 64, 32, 32), 'helmet': (96, 96, 32, 16), 'sleeve': (96, 112, 32, 16)}
LOD_SRC = {'front': 'jerf', 'back': 'jerk', 'legs': 'lega', 'hat': 'hat ', 'helmets': 'hhlm', 'helmet': 'hlm1', 'sleeve': 'slvr'}

def col(h, default=None):
    if h is None: return default
    if isinstance(h, (list, tuple)): return np.array(h[:3], np.float32)
    h = h.strip().lstrip('#'); return np.array([int(h[i:i + 2], 16) for i in (0, 2, 4)], np.float32)

# ---------------------------------------------------------------- reading and writing shapes as images
def image_ps2(shape):
    """(h, w, 4) uint8 with the file's own alpha (0x80 = opaque)"""
    if shape.type == 5: return np.frombuffer(shape.pixel_bytes(), np.uint8).reshape(shape.size[1], shape.size[0], 4).copy()
    pal = np.array(shape.palette(), np.uint8); lut = pal[[ssh.csm1(i) for i in range(256)]]
    return lut[shape.indices()]

def quantize(rgba):
    """-> (indices, palette 256 x 4) by k-means in RGBA (exact when <= 256 colours)"""
    h, w, _ = rgba.shape
    flat = rgba.reshape(-1, 4)
    cols, inv = np.unique(flat, axis=0, return_inverse=True)
    if len(cols) <= 256:
        pal = np.zeros((256, 4), np.uint8); pal[:len(cols)] = cols
        return inv.reshape(h, w).astype(np.uint8), pal
    data = flat.astype(np.float32)
    crit = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 20, 0.5)
    _, lab, cent = cv2.kmeans(data, 256, None, crit, 2, cv2.KMEANS_PP_CENTERS)
    pal = np.clip(np.round(cent), 0, 255).astype(np.uint8)
    return lab.reshape(h, w).astype(np.uint8), pal

def write_image(shape, rgba):
    """an image (PS2 alpha) into a shape of type 2 (8-bit) or 14"""
    rgba = np.clip(rgba, 0, 255).astype(np.uint8)
    if shape.type == 14:
        data, pal = ssh.image14_photo(rgba); shape.set_pixel_bytes(data); shape.set_palette(pal)
    elif shape.type == 2:
        idx, pal = quantize(rgba); shape.set_pixel_bytes(idx.tobytes())
        out = [None] * 256
        for i in range(256): out[ssh.csm1(i)] = [int(v) for v in pal[i]]
        shape.set_palette(out)
    elif shape.type == 5: shape.set_pixel_bytes(rgba.tobytes())

# ---------------------------------------------------------------- taking a texture apart
def analyse(rgb, keep=None, cc=None, hem=True):
    """-> dict: cloth chromaticity, reference brightness, masks: deco (not cloth), under (collar), word (solid
    shapes), pip (thin lines). keep: pixels that are not part of the garment (background, transparency)."""
    f = rgb.astype(np.float32); H, W = f.shape[:2]
    L = f.mean(2) + 1
    fb = cv2.GaussianBlur(f, (3, 3), 0); ch = fb / (fb.mean(2) + 1)[..., None]         # chromaticity, a little smoothed
    sel = ~keep if keep is not None else np.ones((H, W), bool)
    q = np.round(ch[sel] * 16)
    if cc is None:
        vals, cnt = np.unique(q, axis=0, return_counts=True); cc = vals[cnt.argmax()] / 16
        near = sel & (np.abs(ch - cc).sum(2) < 0.15)
        if near.sum() > 20: cc = np.median(ch[near], 0)
    ch = f / L[..., None]                                                             # (unsmoothed: thin lines)
    dist = np.abs(ch - cc).sum(2)
    base = (dist < 0.15) & sel
    ref = np.percentile(L[base], 90) if base.any() else L.max()
    thr = max(0.18, 4 * float(np.median(dist[base]))) if base.any() else 0.18
    deco = (dist > thr) | (L < ref * 0.42) | (L > ref * 1.6)
    if keep is not None: deco &= ~keep
    k = np.ones((3, 3), np.uint8)
    thick = (cv2.dilate(cv2.morphologyEx(deco.astype(np.uint8), cv2.MORPH_OPEN, k), k) > 0) & deco
    n, lab, st, _ = cv2.connectedComponentsWithStats(thick.astype(np.uint8), 8)
    under = np.zeros_like(deco); word = np.zeros_like(deco)
    for i in range(1, n):
        x, y, w, h, a = st[i]
        if y < 6 and x > W * 0.15 and x + w < W * 0.85: under |= lab == i          # the collar opening (undershirt)
        elif a >= 6 and y + h < H * 0.8: word |= lab == i
    pip = deco & ~under & ~word
    pip &= ~(np.arange(H)[:, None] >= H - 4)                                       # hems
    n, lab, st, _ = cv2.connectedComponentsWithStats(pip.astype(np.uint8), 8)
    for i in range(1, n):                                                         # lines, not specks
        x, y, w, h, ar = st[i]
        if max(w, h) < 8 or (hem and y + h > H - 14 and h < 20): pip &= lab != i
    return dict(cc=cc, ref=ref, L=L, deco=deco, under=under, word=word, pip=pip)

def paint_out(rgb, mask):
    m = cv2.dilate(mask.astype(np.uint8), np.ones((2, 2), np.uint8))
    return cv2.inpaint(np.ascontiguousarray(rgb[..., :3]), m, 3, cv2.INPAINT_TELEA).astype(np.float32)

def decal(out, shade, img, box, mirror=False, fill=1.0, max_h=1.15):
    """an RGBA image fitted into box (x0, y0, x1, y1) of out, shaded like the cloth under it"""
    if img is None: return
    x0, y0, x1, y1 = box
    im = img.convert('RGBA'); bb = im.getbbox()
    if bb: im = im.crop(bb)
    if mirror: im = im.transpose(Image.FLIP_LEFT_RIGHT)
    bw, bh = max(1, x1 - x0), max(1, y1 - y0)
    k = min(bw * fill / im.width, bh * max_h / im.height)
    nw, nh = max(1, int(round(im.width * k))), max(1, int(round(im.height * k)))
    a = np.asarray(im.resize((nw, nh), Image.LANCZOS)).astype(np.float32) / 255.0
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2; X, Y = cx - nw // 2, cy - nh // 2
    H, W = out.shape[:2]
    sx0, sy0 = max(0, -X), max(0, -Y); X0, Y0 = max(0, X), max(0, Y)
    X1, Y1 = min(W, X + nw), min(H, Y + nh)
    if X1 <= X0 or Y1 <= Y0: return
    a = a[sy0:sy0 + (Y1 - Y0), sx0:sx0 + (X1 - X0)]
    sh = np.clip(shade[Y0:Y1, X0:X1], 0.55, 1.12)[..., None]
    reg = out[Y0:Y1, X0:X1, :3]
    reg[:] = reg * (1 - a[..., 3:]) + np.clip(a[..., :3] * 255 * sh, 0, 255) * a[..., 3:]

def bbox(mask):
    ys, xs = np.where(mask)
    return (xs.min(), ys.min(), xs.max() + 1, ys.max() + 1) if len(ys) else None

# ---------------------------------------------------------------- garments
def garment(rgba, cloth, trim, under=None, wordmark=None, mirror=False, pinstripes=True, keep_border=False, keep_solid=False, hem=True):
    """jersey / pants / sleeve / jacket texture (PS2 alpha kept)"""
    rgb = rgba[..., :3]; alpha = rgba[..., 3]
    keep = alpha < 0x40
    if keep_border:                                   # menu thumbnails: what touches the border and is not cloth is background
        a0 = analyse(rgb, keep)
        n, lab = cv2.connectedComponents(a0['deco'].astype(np.uint8), 8)
        edge = set(np.unique(np.concatenate([lab[0], lab[-1], lab[:, 0], lab[:, -1]]))) - {0}
        keep = keep | (np.isin(lab, list(edge)) & ~a0['under'])
    a = analyse(rgb, keep, hem=hem)
    solid = a['word'] if keep_solid else np.zeros_like(a['word'])
    pip = a['pip'] if pinstripes else a['pip'] & ~vertical_lines(a['pip'])
    remove = a['deco'] & ~a['under'] & ~solid & ~keep
    base = paint_out(rgb, remove)
    shade = base.mean(2) / a['ref']
    out = np.dstack([np.clip(cloth[None, None] * shade[..., None], 0, 255), alpha.astype(np.float32)])
    if under is not None: out[a['under'], :3] = np.clip(under * shade[..., None], 0, 255)[a['under']]
    out[pip, :3] = np.clip(trim * shade[..., None], 0, 255)[pip]
    out[solid, :3] = rgb[solid]                       # catcher's gear drawn on the pants etc.
    out[keep] = rgba[keep]
    if wordmark is not None and a['word'].any():
        decal(out, shade, wordmark, bbox(a['word']), mirror)
    return out, a

def vertical_lines(mask):
    """pinstripes: thin lines that run most of the height"""
    n, lab, st, _ = cv2.connectedComponentsWithStats(mask.astype(np.uint8), 8)
    out = np.zeros_like(mask)
    for i in range(1, n):
        x, y, w, h, ar = st[i]
        if h > mask.shape[0] * 0.25 and w <= 4: out |= lab == i
    return out

def cap(rgba, crown, brim, button, logo, region='hat', cc=None):
    """cap (hat 128x64) or helmet (hlm1 128x64, hhlm 128x128): crown colour by the folds, the logo where the old one was"""
    rgb = rgba[..., :3]; alpha = rgba[..., 3]; H, W = rgb.shape[:2]
    keep = alpha < 0x40
    a = analyse(rgb, keep, cc)
    # logos: blobs of decoration (pieces closer than 3 pixels belong together) of a logo's size, inside the window
    if region == 'hat': win = (W * 0.55, 0, W, H * 0.78)
    elif region == 'hlm1': win = (0, 0, W * 0.6, H * 0.6)
    else: win = (0, 0, W, H)
    dm = a['deco'].copy()
    if region == 'hat': dm[int(H * 0.72):] = False; dm[:int(H * 0.2), :int(W * 0.78)] = False     # brim, button
    blobs_m = cv2.dilate(dm.astype(np.uint8), np.ones((5, 5), np.uint8))
    n, lab, st, _ = cv2.connectedComponentsWithStats(blobs_m, 8)
    logo_m = np.zeros_like(a['deco']); boxes = []
    near_keep = cv2.dilate(keep.astype(np.uint8), np.ones((5, 5), np.uint8)) > 0
    for i in range(1, n):
        x, y, w, h, ar = st[i]
        m = (lab == i) & dm
        if m.sum() < 25 or w * h > W * H * 0.22: continue                       # specks; the face guard
        if not (x >= win[0] - 2 and y >= win[1] - 2 and x + w <= win[2] + 2 and y + h <= win[3] + 2): continue
        if region == 'hhlm' and (m & near_keep).sum() > m.sum() * 0.3: continue  # the mask's bars next to the cut-outs
        if region == 'hat' and y >= H * 0.72: continue
        logo_m |= m; boxes.append((x + 2, y + 2, x + w - 2, y + h - 2))
    base = paint_out(rgb, logo_m)
    shade = base.mean(2) / a['ref']
    other = a['deco'] & ~logo_m                        # the underside of the brim, the button, eyelets
    out = np.dstack([np.clip(crown[None, None] * shade[..., None], 0, 255), alpha.astype(np.float32)])
    if region == 'hat':
        b = np.zeros((H, W), bool); b[int(H * 0.75):, :W // 2] = True          # top of the brim
        out[b, :3] = np.clip(brim * shade[..., None], 0, 255)[b]
        u = np.zeros((H, W), bool); u[int(H * 0.75):, W // 2:] = True         # underside: keep
        out[u] = rgba[u]
        bt = np.zeros((H, W), bool); bt[:int(H * 0.2), int(W * 0.5):int(W * 0.78)] = True
        bt &= other
        out[bt, :3] = np.clip(button * shade[..., None], 0, 255)[bt]
    else:
        out[other] = rgba[other]                       # face guard, ear hole, highlights
    out[keep] = rgba[keep]
    if logo is not None:
        for bx in boxes:
            x0, y0, x1, y1 = bx
            lg = logo
            if (y1 - y0) > 1.5 * (x1 - x0) and logo.width > 1.3 * logo.height: lg = logo.rotate(90, expand=True)
            decal(out, np.clip(shade, 0.8, 1.1), lg, bx, fill=1.0, max_h=1.0)
    return out

def catcher_sheet(rgba, shell, logo):
    """hhlm, the catcher's helmet (128 x 128, the same layout for every club): left half the mask's shell with the
    cap logo on top and logos on the sides, top right the cage, bottom right the back with a logo. The shell takes
    the new colour, the logos are painted out (the side ones, cut by the seam) or replaced (top, back)."""
    rgb = rgba[..., :3].astype(np.float32); alpha = rgba[..., 3]; H, W = rgb.shape[:2]
    keep = alpha < 0x40
    S = lambda v: int(round(v * W / 128))
    top, back = (S(16), 0, S(48), S(38)), (S(72), S(70), S(124), S(116))
    sides = [(0, S(26), S(14), S(100)), (S(50), S(26), S(64), S(100))]
    region = np.zeros((H, W), bool); region[:, :W // 2] = True; region[H // 2:, W // 2:] = True   # shell parts
    L = rgb.mean(2) + 1; ch = rgb / L[..., None]
    ref_px = np.zeros((H, W), bool); ref_px[S(40):S(56), S(2):S(16)] = True                     # plain shell, beside the guard
    cc = np.median(ch[ref_px], 0); ref = np.percentile(L[ref_px], 90)
    shellm = region & ((np.abs(ch - cc).sum(2) < 0.4) | (L < ref * 0.45)) & ~keep
    logos = np.zeros((H, W), bool)
    for x0, y0, x1, y1 in [top, back] + sides:
        r = np.zeros((H, W), bool); r[y0:y1, x0:x1] = True
        logos |= r & ~keep
    guard = np.zeros((H, W), bool); guard[S(54):S(104), S(14):S(52)] = True                    # the face guard stays
    logos &= ~guard
    base = paint_out(rgba, logos)
    shade = base.mean(2) / ref
    blur = cv2.blur(np.where(shellm, shade, 0).astype(np.float32), (15, 15)) / np.maximum(cv2.blur(shellm.astype(np.float32), (15, 15)), 1e-3)
    shade = np.where(logos, blur, shade)                         # painted-out logos: the shell's smooth shading, no ghosts
    out = rgba.astype(np.float32).copy()
    m = (shellm | logos) & ~guard
    out[m, :3] = np.clip(shell * shade[..., None], 0, 255)[m]
    if logo is not None:
        decal(out, np.clip(shade, 0.8, 1.1), logo, top, fill=0.9, max_h=0.9)
        decal(out, np.clip(shade, 0.8, 1.1), logo, back, fill=0.8, max_h=0.8)
    return out

def font_recolour(shapes, fill, outline):
    """number / letter shapes. The game draws the glyphs with their palettes as they are (indices untouched), so
    only palette entries change, all by one rule: opaque entries split by brightness into the fill (the larger
    group, by pixels over all glyphs) and the outline; each keeps its brightness relative to its group."""
    pals, lums, wts = [], [], []
    for sh in shapes:
        pal = np.array(sh.palette(), np.float32)                       # file order
        idx = sh.indices(); cnt = np.bincount(idx.ravel(), minlength=256)
        filecnt = np.zeros(256); filecnt[[ssh.csm1(i) for i in range(256)]] = cnt
        pals.append(pal); op = pal[:, 3] > 0x40
        lums.append(np.where(op, pal[:, :3].mean(1), -1)); wts.append(filecnt * op)
    L = np.concatenate(lums); Wt = np.concatenate(wts)
    if Wt.sum() == 0: return
    v = np.repeat(L[Wt > 0], Wt[Wt > 0].astype(int))
    t = (np.percentile(v, 30) + np.percentile(v, 85)) / 2
    hi, lo = (L >= t) & (Wt > 0), (L < t) & (L >= 0) & (Wt > 0)
    big_is_hi = Wt[hi].sum() >= Wt[lo].sum()
    m_hi = (L[hi] * Wt[hi]).sum() / max(Wt[hi].sum(), 1) + 1; m_lo = (L[lo] * Wt[lo]).sum() / max(Wt[lo].sum(), 1) + 1
    for sh, pal, l in zip(shapes, pals, lums):
        for i in range(256):
            if l[i] < 0: continue
            is_hi = l[i] >= t
            c = (fill if is_hi == big_is_hi else outline)
            if c is None: continue
            pal[i, :3] = np.clip(c * l[i] / (m_hi if is_hi else m_lo), 0, 255)
        sh.set_palette([list(p) for p in pal])

# ---------------------------------------------------------------- a whole set
def make(uniform_bytes, spec, font_bytes=None, preview=None):
    """-> (uniform bytes, font bytes or None); preview (a Shape from UNIFORMS.BIG) is redrawn in place"""
    def img(k):
        p = spec.get(k)
        return Image.open(p) if p else None
    cloth = col(spec.get('cloth'), np.array([240, 238, 232], np.float32))
    trim = col(spec.get('trim'), np.array([30, 40, 70], np.float32))
    under = col(spec.get('undershirt'), trim)
    pants = col(spec.get('pants'), cloth)
    crown = col(spec.get('crown'), trim); brim = col(spec.get('brim'), crown); button = col(spec.get('button'), crown)
    helmet = col(spec.get('helmet'), crown); jacket = col(spec.get('jacket'), crown)
    word, logo = img('wordmark'), img('cap_logo'); hlogo = img('helmet_logo') or logo
    pin = spec.get('pinstripes', True)
    f = ssh.Ssh(uniform_bytes); names = set(f.names())
    done = {}
    orig = {nm: image_ps2(f[nm]) for nm in FRONTS + FRONTS_L if nm in names}
    for nm in FRONTS + FRONTS_L + BACKS:
        if nm in FRONTS_L and nm in names and FRONTS[FRONTS_L.index(nm)] in done:
            src = FRONTS[FRONTS_L.index(nm)]
            if np.abs(orig[src][:, ::-1].astype(np.float32) - orig[nm]).mean() < 8:      # the mirror image of the front
                out = done[src][:, ::-1].copy(); write_image(f[nm], out); done[nm] = out; continue
        if nm in names:
            out, _ = garment(image_ps2(f[nm]), cloth, trim, under, word if nm not in BACKS else None, mirror=nm in FRONTS_L, pinstripes=pin)
            write_image(f[nm], out); done[nm] = out
    for nm in SLEEVES:
        if nm in names: out, _ = garment(image_ps2(f[nm]), cloth, trim, under, None, pinstripes=pin, hem=False); write_image(f[nm], out); done[nm] = out
    for nm in PANTS:
        if nm in names: out, _ = garment(image_ps2(f[nm]), pants, trim, None, None, pinstripes=pin, keep_solid=True); write_image(f[nm], out); done[nm] = out
    if 'merf' in names: out, _ = garment(image_ps2(f['merf']), jacket, jacket, None, word); write_image(f['merf'], out)
    for nm in ('merk', 'mslv'):
        if nm in names: out, _ = garment(image_ps2(f[nm]), jacket, jacket, None, None); write_image(f[nm], out)
    orig_caps = {nm: 1 for nm in ('hlm1',) if nm in names}
    hl1 = image_ps2(f['hlm1']) if 'hlm1' in names else None
    if 'hat ' in names: out = cap(image_ps2(f['hat ']), crown, brim, button, logo, 'hat'); write_image(f['hat '], out); done['hat '] = out
    if 'hlm1' in names: out = cap(image_ps2(f['hlm1']), helmet, helmet, helmet, hlogo, 'hlm1'); write_image(f['hlm1'], out); done['hlm1'] = out
    if 'hhlm' in names:
        out = catcher_sheet(image_ps2(f['hhlm']), helmet, hlogo); write_image(f['hhlm'], out); done['hhlm'] = out
    if 'llod' in names:                                 # the low-detail atlas from the new textures
        lod = image_ps2(f['llod']).astype(np.float32)
        for key, (x, y, w, h) in LOD.items():
            src = done.get(LOD_SRC[key])
            if src is None: continue
            small = np.asarray(Image.fromarray(np.clip(src, 0, 255).astype(np.uint8), 'RGBA').resize((w, h), Image.BOX)).astype(np.float32)
            lod[y:y + h, x:x + w] = small
        write_image(f['llod'], lod)
    font = None
    if font_bytes is not None:
        ff = ssh.Ssh(font_bytes)
        font_recolour([s for s in ff.shapes if s.type in (2, 14) and s.palette() is not None], col(spec.get('numbers'), trim), col(spec.get('numbers_outline'), None))
        font = ff.build()
    if preview is not None:
        out, _ = garment(image_ps2(preview), cloth, trim, under, word, pinstripes=pin, keep_border=True)
        write_image(preview, out)
    return f.build(), font

def sheet(uniform_bytes, path):
    f = ssh.Ssh(uniform_bytes); tiles = []
    for nm in ['jerf', 'jerk', 'jrfl', 'slvr', 'lega', 'hat ', 'hlm1', 'hhlm', 'llod', 'merf']:
        if nm not in f.names(): continue
        r = image_ps2(f[nm]).astype(np.float32); a = np.minimum(r[..., 3:] / 128.0, 1)
        im = (r[..., :3] * a + 60 * (1 - a)).astype(np.uint8)
        t = np.zeros((128, 128, 3), np.uint8) + 30; h, w = im.shape[:2]; s = 128 / max(h, w)
        im = np.asarray(Image.fromarray(im).resize((int(w * s), int(h * s)), Image.NEAREST)); t[:im.shape[0], :im.shape[1]] = im
        tiles.append(t)
    Image.fromarray(np.hstack(tiles)).resize((len(tiles) * 256, 256), Image.NEAREST).save(path)

if __name__ == '__main__':
    import isofile, refpack
    from bigfile import read_big
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    iso, spec_path, out = a[0], a[1], a[2]
    spec = json.load(open(spec_path)); base = os.path.dirname(os.path.abspath(spec_path))
    for k in ('wordmark', 'cap_logo', 'helmet_logo'):
        if spec.get(k): spec[k] = os.path.join(base, spec[k])
    models = isofile.read_file(iso, 'DATA/MODELS.BIG'); ents = {n.lower(): (o, s) for n, o, s in read_big(models)}
    def get(n):
        o, s = ents[n]; c = models[o:o + s]; return refpack.decompress(c) if c[1:2] == b'\xfb' else c
    team, slot = int(spec['team']), spec.get('slot', 'a')
    name = 'u%03d%s.ssh' % (team, slot); pat = spec.get('pattern', name)
    fname = 'f%03d%s.ssh' % (team, slot)
    u, fo = make(get(pat), spec, get(fname) if fname in ents else None)
    d = os.path.join(out, 'DATA', 'MODELS.BIG'); os.makedirs(d, exist_ok=True)
    open(os.path.join(d, name), 'wb').write(refpack.compress(u))
    if fo: open(os.path.join(d, fname), 'wb').write(refpack.compress(fo))
    if '--sheet' in sys.argv: sheet(u, sys.argv[sys.argv.index('--sheet') + 1])
    print('wrote', name)
