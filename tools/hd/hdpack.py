"""HD texture processing shared by the pack builders: 4x Real-ESRGAN (onnxruntime when it is installed, else the
NumPy fallback), then the steps a plain upscale misses:
  - colour is pulled back to the original's (a model drifts a little in brightness and hue);
  - grain is rebuilt. The models are denoisers: dirt, grass, cloth and skin come out as smooth plastic, because
    what made them look like dirt was their noise. The noise the model removed is measured (original minus the
    upscale shrunk back), put back where it was not an edge, and continued into the two new octaves of detail;
  - transparent texels get the colour of their neighbours first (no dark halos) and the alpha channel is upscaled
    on its own, kept crisp where it was a cut-out;
  - tiling textures are padded by wrapping (in the directions in which they repeat) and their grain is periodic,
    so they stay seamless.
"""
import os, sys, numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import esrgan

MODELS = {'photo': 'realesr-general-x4v3', 'best': 'RealESRGAN_x4plus', 'art': 'RealESRGAN_x4plus_anime_6B'}

BUCKETS = (64, 112, 184)                   # tile sizes for Core ML (Upscaler.up: tile 160 + 2 x pad 12)
class Upscaler:
    """model files are looked up in models_dir as <name>.onnx (written on first use from <name>.pth) or <name>.pth"""
    def __init__(self, models_dir, threads=0, cache_dir=None):
        self.dir, self.cache, self.threads, self.nets = models_dir, cache_dir or models_dir, threads, {}
        try: import onnxruntime as ort; self.ort = ort
        except Exception: self.ort = None
    def net(self, kind):
        if kind in self.nets: return self.nets[kind]
        name = MODELS[kind]; pth = os.path.join(self.dir, name + '.pth')
        if self.ort is not None:
            onnx = os.path.join(self.dir, name + '.onnx')
            if not os.path.exists(onnx): onnx = os.path.join(self.cache, name + '.onnx')
            if not os.path.exists(onnx):
                import onnx_export; onnx_export.export(pth, onnx)
            so = self.ort.SessionOptions()
            if self.threads: so.intra_op_num_threads = self.threads
            so.log_severity_level = 3
            prov = os.environ.get('MVP_HD_PROVIDER', 'cpu')
            if prov == 'coreml' and 'CoreMLExecutionProvider' in self.ort.get_available_providers():
                # Apple GPU / Neural Engine. Core ML compiles the model for each input shape, so tiles are padded up
                # to a few fixed sizes (BUCKETS) and the result cropped back.
                opts = {'ModelFormat': 'MLProgram', 'MLComputeUnits': os.environ.get('MVP_HD_UNITS', 'ALL'),
                        'RequireStaticInputShapes': '0', 'ModelCacheDirectory': os.path.join(self.cache, 'coreml_cache')}
                os.makedirs(opts['ModelCacheDirectory'], exist_ok=True)
                s = self.ort.InferenceSession(onnx, so, providers=[('CoreMLExecutionProvider', opts), 'CPUExecutionProvider'])
                def f(x, s=s):
                    h, w = x.shape[:2]
                    bh = next(b for b in BUCKETS if b >= h); bw = next(b for b in BUCKETS if b >= w)
                    xp = np.pad(x, ((0, bh - h), (0, bw - w), (0, 0)), mode='edge') if (bh, bw) != (h, w) else x
                    r = s.run(None, {'input': np.ascontiguousarray(xp.transpose(2, 0, 1)[None])})[0][0].transpose(1, 2, 0)
                    return r[:h * 4, :w * 4]
            else:
                s = self.ort.InferenceSession(onnx, so, providers=['CPUExecutionProvider'])
                f = lambda x: s.run(None, {'input': np.ascontiguousarray(x.transpose(2, 0, 1)[None])})[0][0].transpose(1, 2, 0)
        else:
            f = esrgan.load(pth)
        self.nets[kind] = f; return f
    def up(self, rgb, kind, wrap, tile=160, pad=12):
        """HxWx3 uint8 -> 4Hx4Wx3 float32 (0..255), padded by wrapping (tiling textures) or mirroring"""
        p = 8
        x = _pad(rgb, p, p, wrap, 'reflect').astype(np.float32) / 255.0
        net = self.net(kind); H, W, _ = x.shape
        out = np.zeros((H * 4, W * 4, 3), np.float32)
        for y0 in range(0, H, tile):
            for x0 in range(0, W, tile):
                y1, x1 = min(H, y0 + tile), min(W, x0 + tile)
                ya, xa, yb, xb = max(0, y0 - pad), max(0, x0 - pad), min(H, y1 + pad), min(W, x1 + pad)
                r = net(x[ya:yb, xa:xb])
                out[y0 * 4:y1 * 4, x0 * 4:x1 * 4] = r[(y0 - ya) * 4:(y1 - ya) * 4, (x0 - xa) * 4:(x1 - xa) * 4]
        return np.clip(out[p * 4:-p * 4, p * 4:-p * 4], 0, 1) * 255.0

# ---------------------------------------------------------------- small image helpers (float32, HxW[xC])
# "wrap" everywhere: True / False, or (down, across) when a texture repeats in one direction only
def _axes(wrap): return tuple(bool(x) for x in wrap) if isinstance(wrap, (tuple, list)) else (bool(wrap), bool(wrap))
def _pad(a, py, px, wrap, other):
    wy, wx = _axes(wrap); z = [(0, 0)] * (a.ndim - 2)
    if py: a = np.pad(a, [(py, py), (0, 0)] + z, mode='wrap' if wy else other)
    if px: a = np.pad(a, [(0, 0), (px, px)] + z, mode='wrap' if wx else other)
    return a
def tiles(rgb):
    """does the picture continue into itself? -> (down, across). The step from the last row to the first is compared
    with the steps between neighbouring rows: a field of grass or a brick wall passes, a wall with a white line
    along its foot does not (wrapped padding would carry that line to the top edge as a bright seam)."""
    a = rgb.astype(np.float32); out = []
    for ax in (0, 1):
        b = a if ax == 0 else a.transpose(1, 0, 2)
        if b.shape[0] < 4: out.append(False); continue
        inner = np.abs(b[1:] - b[:-1]).mean((1, 2)); seam = float(np.abs(b[0] - b[-1]).mean())
        out.append(seam <= max(1.6 * float(np.percentile(inner, 75)), 4.0))
    return tuple(out)
def _blur(a, sigma, wrap):
    """separable Gaussian"""
    r = max(1, int(sigma * 3 + 0.5)); k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2).astype(np.float32); k /= k.sum()
    for ax in (0, 1):
        p = _pad(a, r if ax == 0 else 0, r if ax == 1 else 0, wrap, 'reflect'); out = np.zeros_like(a)
        for i, w in enumerate(k):
            sl = [slice(None)] * a.ndim; sl[ax] = slice(i, i + a.shape[ax]); out += w * p[tuple(sl)]
        a = out
    return a
def _cubic(x):
    x = np.abs(x); return np.where(x < 1, 1.5 * x ** 3 - 2.5 * x ** 2 + 1, np.where(x < 2, -0.5 * x ** 3 + 2.5 * x ** 2 - 4 * x + 2, 0)).astype(np.float32)
def _up(a, k, wrap):
    """Catmull-Rom upscale by k (pixel centres aligned), wrapping or clamping at the borders"""
    for ax in (0, 1):
        n = a.shape[ax]
        p = _pad(a, 2 if ax == 0 else 0, 2 if ax == 1 else 0, wrap, 'edge')
        shape = list(a.shape); shape[ax] = n * k; out = np.zeros(shape, np.float32)
        for ph in range(k):
            f = (ph + 0.5) / k - 0.5; b = int(np.floor(f)); f -= b
            acc = 0
            for i in range(-1, 3):
                sl = [slice(None)] * a.ndim; sl[ax] = slice(2 + b + i, 2 + b + i + n); acc = acc + _cubic(f - i) * p[tuple(sl)]
            so = [slice(None)] * a.ndim; so[ax] = slice(ph, None, k); out[tuple(so)] = acc
        a = out
    return a
def _down(a, k): h, w = a.shape[0] // k, a.shape[1] // k; return a.reshape((h, k, w, k) + a.shape[2:]).mean((1, 3))
_LUM = np.array([0.299, 0.587, 0.114], np.float32)

def restore(orig, E, wrap, seed=0, grain=1.0):
    """orig: HxWx3 (0..255), E: its 4x upscale. Returns E with the original's colour and grain."""
    orig = orig.astype(np.float32); h, w = orig.shape[:2]
    D = _down(E, 4)
    R = orig - D                                   # what the model changed, at the original's resolution
    low = _blur(R, 2.0, wrap)                      # colour / brightness drift: always undone
    out = E + _up(low, 4, wrap)
    if grain <= 0: return out
    G = R - low                                    # fine detail the model dropped: grain, or stair-steps along edges
    lum = D @ _LUM
    p = _pad(lum, 1, 1, wrap, 'edge')
    edge = np.hypot(p[1:-1, 2:] - p[1:-1, :-2], p[2:, 1:-1] - p[:-2, 1:-1]) * 0.5
    edge = _blur(np.maximum(edge, _blur(edge, 1.0, wrap)), 0.7, wrap)
    wgt = 1.0 / (1.0 + (edge / 7.0) ** 2)          # 1 on flat (grainy) surfaces, 0 along real edges
    gl = G @ _LUM
    gstd = np.sqrt(_blur((gl * wgt) ** 2, 2.5, wrap))          # local grain strength
    gstd = np.minimum(gstd, 14.0)
    wU = np.clip(_up(wgt, 4, wrap), 0, 1)[..., None]; sU = np.maximum(_up(gstd, 4, wrap), 0)[..., None]
    # the grain's own colour: mostly brightness, a little of each channel's own
    cs = np.sqrt((G ** 2).reshape(-1, 3).mean(0)) / (np.sqrt((gl ** 2).mean()) + 1e-3); cs = np.clip(cs, 0.6, 1.4)
    rng = np.random.default_rng(seed)
    n2 = _up(rng.standard_normal((h * 2, w * 2), np.float32), 2, wrap)       # one octave above the original's grain
    n4 = rng.standard_normal((h * 4, w * 4), np.float32)                     # two octaves above
    n4 = n4 - _blur(n4, 1.0, wrap)
    fine = (0.75 * n2 / 0.72 + 0.5 * n4 / 0.8)[..., None] * cs
    chroma = rng.standard_normal((h * 2, w * 2, 3), np.float32); chroma = _up(chroma - chroma.mean(2, keepdims=True), 2, wrap) * 0.25
    out = out + grain * wU * (0.9 * _up(G, 4, wrap) + sU * (fine + chroma))
    return out

def chroma_transfer(orig, out, wrap):
    """Colours only from the original. The models sharpen brightness well but overshoot in colour: the pale
    ring an artist's sharpening left around red lettering comes back as vivid cyan. Every pixel of the enlarged
    picture keeps its brightness and takes its colour from the original's texels around it, preferring those as
    bright as itself, so colour edges stay where the brightness edges are and no colour appears that the
    original did not have next to that spot. orig HxWx3, out 4Hx4Wx3 float."""
    o = orig.astype(np.float32); h, w = o.shape[:2]
    y = o @ _LUM; c = np.dstack([o[..., 2] - y, o[..., 0] - y])
    Y = out @ _LUM
    yp = _pad(y, 1, 1, wrap, 'edge'); cp = _pad(c, 1, 1, wrap, 'edge')
    off = (np.arange(4) + 0.5) / 4 - 0.5                 # where a pixel sits inside its texel
    fy = np.tile(off, h)[:, None]; fx = np.tile(off, w)[None, :]
    acc = np.zeros((h * 4, w * 4, 2), np.float32); wsum = np.zeros((h * 4, w * 4), np.float32)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            yt = np.repeat(np.repeat(yp[1 + dy:1 + dy + h, 1 + dx:1 + dx + w], 4, 0), 4, 1)
            ct = np.repeat(np.repeat(cp[1 + dy:1 + dy + h, 1 + dx:1 + dx + w], 4, 0), 4, 1)
            ws = np.exp(-((dy - fy) ** 2 + (dx - fx) ** 2) / (2 * 0.85 ** 2))
            wt = ws * (np.exp(-((Y - yt) ** 2) / (2 * 18.0 ** 2)) + 1e-6)
            acc += ct * wt[..., None]; wsum += wt
    C = acc / wsum[..., None]
    R = Y + C[..., 1]; B = Y + C[..., 0]
    return np.dstack([R, (Y - 0.299 * R - 0.114 * B) / 0.587, B])

def fill_transparent(rgb, alpha):
    """give fully transparent pixels the colour of the nearest visible pixel (repeated dilation)"""
    rgb = rgb.astype(np.float32); vis = alpha > 8
    if vis.all() or not vis.any(): return rgb.astype(np.uint8)
    out = rgb.copy(); known = vis.copy()
    for _ in range(64):
        if known.all(): break
        acc = np.zeros_like(out); cnt = np.zeros(known.shape, np.float32)
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            k = np.roll(known, (dy, dx), (0, 1)); v = np.roll(out, (dy, dx), (0, 1))
            acc += v * k[..., None]; cnt += k
        new = (~known) & (cnt > 0)
        out[new] = acc[new] / cnt[new][:, None]; known |= new
    return np.clip(out, 0, 255).astype(np.uint8)

def _runs(m, n):
    """pixels of the mask that sit in a horizontal run of at least n"""
    c = np.cumsum(np.pad(m, ((0, 0), (1, 0))), 1, dtype=np.int32)
    full = (c[:, n:] - c[:, :-n]) == n                           # full[y, x]: m[y, x:x+n] all set
    out = np.zeros_like(m)
    for k in range(n): out[:, k:k + full.shape[1]] |= full
    return out

def gutter(img):
    """font pages that separate their glyph cells with a see-through filler colour (never meant to be drawn, but the
    upscaler smears it into the cells and it shows as a faint box around every letter): mask of that filler"""
    h, w = img.shape[:2]; v = img[0, 0]
    if not 0 < v[3] < 255 or h < 128 or w < 128: return None
    g = (img == v).all(2)
    if not 0.08 < g.mean() < 0.75 or not g[0].all(): return None
    fill = _runs(g, 8) | _runs(g.T, 8).T                         # bars and lines; single pixels belong to the glyphs
    if fill.sum() < 0.75 * g.sum() or (img[..., 3][~g] == 0).mean() < 0.25: return None
    thin = _runs(g.T, 8).T & ~_runs(g, 4)                        # the upright lines between the cells
    if (thin.sum(0) >= 8).sum() < 6: return None
    return fill

def _minmax(a, r):
    """running minimum and maximum over a (2r+1)^2 window"""
    lo, hi = a, a
    for ax in (0, 1):
        p = np.pad(lo, [(r, r) if k == ax else (0, 0) for k in (0, 1)], 'edge'); q = np.pad(hi, [(r, r) if k == ax else (0, 0) for k in (0, 1)], 'edge')
        n = a.shape[ax]; sl = lambda i: (slice(i, i + n), slice(None)) if ax == 0 else (slice(None), slice(i, i + n))
        lo = np.minimum.reduce([p[sl(i)] for i in range(2 * r + 1)]); hi = np.maximum.reduce([q[sl(i)] for i in range(2 * r + 1)])
    return lo, hi

def steepen(ua, r=2, k=2.2):
    """sharpen the steps of a smoothly enlarged mask without moving them or changing its levels (small shadowed
    letters: the model invents blotches in shadows this thin, a plain enlargement leaves them blurred)"""
    lo, hi = _minmax(ua, r); d = hi - lo
    t = np.where(d > 1, (ua - lo) / np.maximum(d, 1), 0.5); w = np.clip(d / 48, 0, 1)
    return ua * (1 - w) + (lo + np.clip((t - 0.5) * k + 0.5, 0, 1) * d) * w

def is_flat(rgb, alpha):
    """artwork vs photo: share of pixels equal to their right/bottom neighbour, and colour count"""
    a = rgb.astype(np.int16)
    same = (np.abs(a[:, 1:] - a[:, :-1]).sum(2) <= 6).mean() * 0.5 + (np.abs(a[1:] - a[:-1]).sum(2) <= 6).mean() * 0.5
    cols = len(np.unique(rgb.reshape(-1, 3), axis=0))
    transparent = alpha is not None and (alpha < 250).mean() > 0.05
    return same > 0.45 or cols < 64 or (transparent and same > 0.3)

def luma_clamp(orig, out, wrap):
    """No brighter and no darker than the original was around that spot. The models like to turn a pale one-texel
    highlight into a pure white stroke and to ring next to hard edges; brightness is held to the range of the
    3x3 original texels around each pixel, widened a little (more where the range is large already)."""
    y = orig.astype(np.float32) @ _LUM; h, w = y.shape
    p = _pad(y, 1, 1, wrap, 'edge')
    st = [p[dy:dy + h, dx:dx + w] for dy in range(3) for dx in range(3)]
    lo = np.minimum.reduce(st); hi = np.maximum.reduce(st); m = 6.0 + 0.12 * (hi - lo)
    lo = _up(lo - m, 4, wrap); hi = _up(hi + m, 4, wrap)      # smooth bounds: no texel-sized steps in the limit
    Y = out @ _LUM
    return out + (np.clip(Y, lo, np.maximum(lo, hi)) - Y)[..., None]

def _prepare(img, tiling, crisp_alpha):
    g = gutter(img) if crisp_alpha == 'auto' else None  # menu art only: jersey numbers keep their frame
    if g is not None: img = img.copy(); img[g] = 0
    rgb, alpha = img[..., :3], img[..., 3]
    has_alpha = (alpha > 40).mean() > 0.02 and (alpha < 200).mean() > 0.02
    flat = is_flat(rgb, alpha if has_alpha else None)
    wrap = (not has_alpha and not flat) if tiling is None else (tiling and not has_alpha)
    if wrap: wrap = tiles(rgb)                          # only in the directions in which it really repeats
    src = fill_transparent(rgb, alpha) if has_alpha else rgb
    return img, rgb, alpha, has_alpha, flat, wrap, src

def lost_grain(img, tiling=None, crisp_alpha=False):
    """a texture made before revision 5: was it taken for flat artwork although it has grain? (few colours, or
    grain so fine that neighbouring texels count as equal: mown grass, painted walls. Its grain was not restored.)"""
    if tiling is None: return False
    img, rgb, alpha, has_alpha, flat, wrap, src = _prepare(img, tiling, crisp_alpha)
    if not flat: return False
    y = src.astype(np.float32) @ _LUM
    return float(np.abs(y - _blur(y, 1.0, wrap)).mean()) > 0.6

def fix_grain(img, out, tiling, grain, seed):
    """a finished texture that lost_grain() names: the same repair on the finished picture, no model needed (what the
    model removed is still the difference between the original and the picture, shrunk)"""
    img, rgb, alpha, has_alpha, flat, wrap, src = _prepare(img, tiling, False)
    new = restore(src, out[..., :3].astype(np.float32), wrap, seed, grain)
    return np.dstack([np.clip(new + 0.5, 0, 255).astype(np.uint8), out[..., 3]])

def seam_risk(img, tiling=None, crisp_alpha=False):
    """a texture made before wrapping was decided per direction: was it padded by wrapping where it does not repeat?
    (then its edge carries a line from the opposite edge and it has to be made again)"""
    w = _prepare(img, tiling, crisp_alpha)[5]
    return bool(w) and _axes(w) != (True, True)

def fix_colour(img, out, tiling=None, crisp_alpha=False):
    """a texture made before chroma_transfer existed: the same repair on the finished picture (no model needed).
    img: the original RGBA, out: its 4x RGBA; same tiling / crisp_alpha as it was made with."""
    img, rgb, alpha, has_alpha, flat, wrap, src = _prepare(img, tiling, crisp_alpha)
    new = chroma_transfer(src, out[..., :3].astype(np.float32), wrap)
    return np.dstack([np.clip(new + 0.5, 0, 255).astype(np.uint8), out[..., 3]])

def process(up, img, model='auto', tiling=None, grain=1.0, seed=0, crisp_alpha=False):
    """RGBA uint8 (alpha 0..255) -> 4x RGBA uint8.
    model: 'best' (RealESRGAN_x4plus), 'photo' (realesr-general-x4v3), 'art' (anime) or 'auto' (art for flat
    artwork up to 128x128, photo otherwise). tiling: pad by wrapping (None = opaque non-flat textures).
    crisp_alpha: also run a soft alpha channel through the model (letters) instead of resampling it smoothly;
    'auto' decides from the alpha channel (on/off with thin edges = the model, letters with shadows = a sharpened
    enlargement, glows = a smooth one). The see-through filler between the cells of a font page is removed."""
    img, rgb, alpha, has_alpha, flat, wrap, src = _prepare(img, tiling, crisp_alpha)
    if model == 'auto': model = 'art' if flat and rgb.shape[0] * rgb.shape[1] <= 128 * 128 else 'photo'
    E = up.up(src, model, wrap)
    E = luma_clamp(src, E, wrap)
    E = chroma_transfer(src, E, wrap)
    # grain: what restore() measures is what the model removed, so a texture without grain gets none either way;
    # only menu artwork (tiling None), where the "grain" would be the stair-steps of small lettering, is exempt
    E = restore(src, E, wrap, seed, 0.0 if flat and tiling is None else grain)
    out_rgb = np.clip(E + 0.5, 0, 255).astype(np.uint8)
    if has_alpha:
        vis = float((alpha >= 8).sum())                 # a cut-out: what can be seen at all is nearly all solid
        hard = vis > 0 and (alpha > 247).sum() / vis > 0.93 and (alpha > 247).mean() > 0.02
        if not hard and crisp_alpha == 'auto':         # lettering and line art keep their edges in the alpha channel:
            n = float((alpha > 8).sum())               # few in-between values = edges, some = letters with a shadow,
            mid = ((alpha > 24) & (alpha < 232)).sum() / n if n else 1.0      # many = a glow or a soft shadow
            crisp_alpha = bool(flat and mid < 0.15) or ('steep' if flat and mid < 0.45 else False)
        if hard:                                       # cut-out: run the mask through the model for crisp edges
            ua = up.up(np.repeat(alpha[..., None], 3, 2), 'art' if alpha.size <= 128 * 128 else 'photo', False).mean(2)
            ua = np.clip((ua - 128) * 3 + 128, 0, 255)
        elif crisp_alpha is True:                      # the model, held to the coverage the original had
            ua = up.up(np.repeat(alpha[..., None], 3, 2), model if model in ('art', 'photo') else 'photo', False).mean(2)
            ua = np.clip(ua + _up(alpha.astype(np.float32) - _down(ua, 4), 4, False), 0, 255)
        elif crisp_alpha == 'steep': ua = np.clip(steepen(np.clip(_up(alpha.astype(np.float32), 4, False), 0, 255)), 0, 255)
        else: ua = np.clip(_up(alpha.astype(np.float32), 4, False), 0, 255)
        lo, hi = _minmax(alpha.astype(np.float32), 1)   # nothing where the original had nothing within a texel,
        ua = np.where(np.repeat(np.repeat(hi, 4, 0), 4, 1) == 0, 0, ua)        # solid where it was solid all around
        ua = np.where(np.repeat(np.repeat(lo, 4, 0), 4, 1) >= 255, 255, ua)
        out_a = (ua + 0.5).astype(np.uint8)
    else:
        out_a = np.clip(_up(alpha.astype(np.float32), 4, wrap) + 0.5, 0, 255).astype(np.uint8)
    return np.dstack([out_rgb, out_a])
