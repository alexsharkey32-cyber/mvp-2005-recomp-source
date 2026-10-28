"""EA shape files (.ssh, "SHPS"): read, edit palettes and pixels, splice shapes between files, write.

    python3 tools/ssh.py file.ssh [out_dir]      list the shapes (and save them as PNG: needs numpy + cv2)

Layout: 'SHPS', file size, count, 'G359', count x (4-char name, offset), filler, then one chunk per shape.
A chunk is a chain of sections: type byte, 24-bit size of the section (0 = last), then 12 more header bytes
(width, height, ...) and the data. The first section holds the pixels:
  type 2  = 8-bit indices, type 5 = RGBA, type 14 = 3 bits per pixel (see decode14), type 1 = 4-bit;
then a palette (type 0x21: 256 x RGBA, PS2 alpha: 0x80 = opaque) and comment sections (0x69, 0x70).
Palette entries are stored in the PS2's "CSM1" order: index i uses entry (i & 0xe7) | (i & 8) << 1 | (i & 16) >> 1.
"""
import struct, sys, os

def csm1(i): return (i & 0xe7) | ((i & 8) << 1) | ((i & 16) >> 1)

class Shape:
    def __init__(self, name, chunk):
        self.name, self.chunk = name, bytearray(chunk)
    @property
    def type(self): return self.chunk[0]
    @property
    def size(self): return struct.unpack_from('<HH', self.chunk, 4)
    def sections(self):
        """[(offset in chunk, type, section size or 0)]"""
        out, p = [], 0
        while p + 16 <= len(self.chunk):
            t, size = self.chunk[p], int.from_bytes(self.chunk[p + 1:p + 4], 'little')
            out.append((p, t, size))
            if size == 0: break
            p += size
        return out
    def pal_offset(self):
        for p, t, size in self.sections():
            if t in (0x21, 0x22, 0x23, 0x24) and p + 16 + 1024 <= len(self.chunk): return p + 16
        return None
    def palette(self):
        """256 x [r, g, b, a] in file order (a: 0x80 = opaque), or None"""
        o = self.pal_offset()
        if o is None: return None
        return [list(self.chunk[o + 4 * i:o + 4 * i + 4]) for i in range(256)]
    def set_palette(self, pal):
        o = self.pal_offset()
        for i, c in enumerate(pal): self.chunk[o + 4 * i:o + 4 * i + 4] = bytes(max(0, min(255, int(round(v)))) for v in c)
    def rename(self, name):
        """the directory name and the copy of it kept in the chunk's name section (type 0x70), which the game looks up"""
        for p, t, size in self.sections():
            if t == 0x70:
                e = self.chunk.index(0, p + 4); old = bytes(self.chunk[p + 4:e])
                if old.strip() == self.name.encode('latin1').strip() and len(name) <= len(old) + (len(self.chunk) - e - 1):
                    self.chunk[p + 4:p + 4 + max(len(old), len(name)) + 1] = name.encode('latin1').ljust(max(len(old), len(name)) + 1, b'\0')
        self.name = name
    def pixel_bytes(self):
        w, h = self.size
        n = {2: w * h, 5: w * h * 4, 14: w * h * 3 // 8, 1: w * h // 2}.get(self.type)
        return None if n is None else bytes(self.chunk[16:16 + n])
    def set_pixel_bytes(self, b):
        assert len(b) == len(self.pixel_bytes()); self.chunk[16:16 + len(b)] = b
    # ---- images (numpy)
    def indices(self):
        """(h, w) uint8 palette indices (types 2 and 14)"""
        import numpy as np
        w, h = self.size
        if self.type == 2: return np.frombuffer(self.pixel_bytes(), np.uint8).reshape(h, w).copy()
        if self.type == 14: return decode14(self.pixel_bytes(), w, h)
        raise ValueError('type %d has no indices' % self.type)
    def rgba(self):
        """(h, w, 4) uint8, alpha 0..255"""
        import numpy as np
        w, h = self.size
        if self.type == 5: im = np.frombuffer(self.pixel_bytes(), np.uint8).reshape(h, w, 4).copy()
        else:
            pal = np.array(self.palette(), np.uint8); lut = pal[[csm1(i) for i in range(256)]]
            im = lut[self.indices()]
        im[..., 3] = np.minimum(255, im[..., 3].astype(np.int32) * 255 // 128)
        return im

class Ssh:
    def __init__(self, data):
        assert data[:4] == b'SHPS', data[:4]
        n = struct.unpack_from('<I', data, 8)[0]
        self.tag = data[12:16]
        ents = [(data[16 + 8 * i:20 + 8 * i].decode('latin1'), struct.unpack_from('<I', data, 20 + 8 * i)[0]) for i in range(n)]
        offs = sorted(set(o for _, o in ents)) + [len(data)]
        self.filler = data[16 + 8 * n:offs[0]]                    # "Buy ERTS" and padding
        self.shapes = [Shape(nm, data[o:offs[offs.index(o) + 1]]) for nm, o in ents]
    def __getitem__(self, name):
        for s in self.shapes:
            if s.name == name: return s
        raise KeyError(name)
    def names(self): return [s.name for s in self.shapes]
    def build(self):
        head = 16 + 8 * len(self.shapes)
        body, dirs, pos = b'', b'', head + len(self.filler)
        for s in self.shapes:
            dirs += s.name.encode('latin1')[:4].ljust(4) + struct.pack('<I', pos + len(body)); body += bytes(s.chunk)
        out = b'SHPS' + struct.pack('<II', pos + len(body), len(self.shapes)) + self.tag + dirs + self.filler + body
        return out

# ---------------------------------------------------------------- type 14 (3 bits per pixel, unpacked by the GS)
# Layer A: W/8 x H/4 32-bit texels (4 base indices each), layer B: W/4 x H/4 32-bit texels read as an 8-bit
# W/2 x H/2 texture of 2-bit offsets. The game draws A into a 32-bit target of W/2 x H/2, adds the offsets and
# uses the target's memory as an 8-bit W x H texture, so both layers go through the GS's block layouts.
kBlock32 = [[0, 1, 4, 5, 16, 17, 20, 21], [2, 3, 6, 7, 18, 19, 22, 23], [8, 9, 12, 13, 24, 25, 28, 29], [10, 11, 14, 15, 26, 27, 30, 31]]
kBlock8 = kBlock32
kCol32 = [[0, 1, 4, 5, 8, 9, 12, 13], [2, 3, 6, 7, 10, 11, 14, 15], [16, 17, 20, 21, 24, 25, 28, 29], [18, 19, 22, 23, 26, 27, 30, 31],
          [32, 33, 36, 37, 40, 41, 44, 45], [34, 35, 38, 39, 42, 43, 46, 47], [48, 49, 52, 53, 56, 57, 60, 61], [50, 51, 54, 55, 58, 59, 62, 63]]
kCol8 = [[0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54], [8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62],
         [33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23], [41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31],
         [96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86], [104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94],
         [65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119], [73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127],
         [128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182], [136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190],
         [161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151], [169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159],
         [224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214], [232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222],
         [193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247], [201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255]]

def _addr32(bw, x, y):
    import numpy as np
    page = (y >> 5) * bw + (x >> 6); block = np.array(kBlock32)[(y >> 3) & 3, (x >> 3) & 7]
    return (page * 32 + block) * 64 + np.array(kCol32)[y & 7, x & 7]            # word index
def _addr8(bw, x, y):
    import numpy as np
    page = (y >> 6) * ((bw + 1) >> 1) + (x >> 7); block = np.array(kBlock8)[(y >> 4) & 3, (x >> 4) & 7]
    return (page * 32 + block) * 256 + np.array(kCol8)[y & 15, x & 15]          # byte index
def _mem_words(w32, h32):
    bw = max(1, (w32 + 63) // 64)
    return bw, (h32 + 31) // 32 * bw * 32 * 64 + 64 * 32 * bw
def ct32_to_t8(words, w32, h32):
    """(h32, w32) uint32 written as a 32-bit image -> the same memory read as an 8-bit image (2*h32, 2*w32)"""
    import numpy as np
    bw, n = _mem_words(w32, h32)
    ys, xs = np.mgrid[0:h32, 0:w32]
    mem = np.zeros((n,), np.uint32); mem[_addr32(bw, xs, ys)] = words
    ys, xs = np.mgrid[0:h32 * 2, 0:w32 * 2]
    return mem.view(np.uint8)[_addr8(bw * 2, xs, ys)]
def t8_to_ct32(img8, w32, h32):
    """inverse of ct32_to_t8: an 8-bit image (2*h32, 2*w32) -> the 32-bit words (h32, w32) that hold it"""
    import numpy as np
    bw, n = _mem_words(w32, h32)
    mem = np.zeros((n * 4,), np.uint8)
    ys, xs = np.mgrid[0:h32 * 2, 0:w32 * 2]
    mem[_addr8(bw * 2, xs, ys)] = img8
    ys, xs = np.mgrid[0:h32, 0:w32]
    return mem.view(np.uint32)[_addr32(bw, xs, ys)]

def decode14(data, W, H):
    """type-14 pixel data -> (H, W) uint8 palette indices"""
    import numpy as np
    aw, ah, bw_, bh_ = W // 8, H // 4, W // 4, H // 4
    A = np.frombuffer(data[:aw * ah * 4], np.uint8).reshape(ah, aw, 4)
    Bw = np.frombuffer(data[aw * ah * 4:aw * ah * 4 + bw_ * bh_ * 4], np.uint8).copy().view(np.uint32).reshape(bh_, bw_)
    B8 = ct32_to_t8(Bw, bw_, bh_).astype(np.int32)                 # (H/2, W/2): four 2-bit offsets per target texel
    fy, fx = np.mgrid[0:H // 2, 0:W // 2]
    a = A[fy // 2, fx // 4].astype(np.int32)                       # the A texel a target texel takes its bases from
    out = np.zeros((H // 2, W // 2, 4), np.uint8)
    for k in range(4):
        s = (B8 >> (2 * k)) & 3
        out[..., k] = np.clip(a[..., k] + s, 0, 255) if k < 3 else (a[..., k] & 0xfc) | ((a[..., k] + s) & 3)
    words = out.reshape(H // 2, W // 2 * 4).copy().view(np.uint32).reshape(H // 2, W // 2)
    return ct32_to_t8(words, W // 2, H // 2)

_layout14_cache = {}
def _layout14(H, W):
    """which pixels share a base (members), and where each pixel's offset bits go (cached per size)"""
    import numpy as np
    if (H, W) in _layout14_cache: return _layout14_cache[(H, W)]
    lab = np.arange((H // 2) * (W // 2) * 4, dtype=np.uint32).reshape(H // 2, W // 2, 4)
    pos = np.zeros((H, W), np.uint32)
    for sh in (0, 8, 16, 24):
        part = ((lab >> sh) & 255).astype(np.uint8)
        words = part.reshape(H // 2, W // 2 * 4).copy().view(np.uint32).reshape(H // 2, W // 2)
        pos |= ct32_to_t8(words, W // 2, H // 2).astype(np.uint32) << sh
    p_ty, p_tx, p_k = (pos // 4) // (W // 2), (pos // 4) % (W // 2), pos % 4
    grp = ((p_ty // 2) * (W // 8) + (p_tx // 4)) * 4 + p_k
    ngrp = (H // 4) * (W // 8) * 4
    order = np.argsort(grp.ravel(), kind='stable')
    _layout14_cache[(H, W)] = (order.reshape(ngrp, 8), p_ty, p_tx, p_k, ngrp)
    return _layout14_cache[(H, W)]
def _write14(H, W, L, best_base, best_off):
    import numpy as np
    members, p_ty, p_tx, p_k, ngrp = L
    A = best_base.reshape(H // 4, W // 8, 4).astype(np.uint8)
    offs_px = np.zeros((H * W,), np.uint8); offs_px[members] = best_off.astype(np.uint8)
    B8 = np.zeros((H // 2, W // 2), np.uint8)
    np.add.at(B8, (p_ty.ravel(), p_tx.ravel()), (offs_px << (2 * p_k.ravel())).astype(np.uint8))
    Bw = t8_to_ct32(B8, W // 4, H // 4)
    data = A.tobytes() + Bw.astype('<u4').tobytes()
    return data, decode14(data, W, H)
def encode14(idx, pal_rgba, want_rgba=None, bases=None):
    """(H, W) palette indices -> type-14 pixel data that decodes as close to them as the format allows.
    Each group of 8 pixels shares a base index and each pixel picks base + 0..3, so a group needs four
    consecutive palette entries that fit; the base is chosen by colour error (pal_rgba: 256 x 4 in index order,
    want_rgba: (H, W, 4) target colours, default = the colours of idx). Returns (data, decoded indices)."""
    import numpy as np
    H, W = idx.shape
    pal = np.asarray(pal_rgba, np.float32)
    want = pal[idx] if want_rgba is None else np.asarray(want_rgba, np.float32)
    L = _layout14(H, W)
    if bases is not None and list(bases) == list(range(0, 4 * (len(list(bases))), 4)):
        # fast path: ramps of four starting at multiples of 4 (then the alpha channel's wrap changes nothing)
        nr = len(list(bases)); members, p_ty, p_tx, p_k, ngrp = L
        wantf = want.reshape(-1, 4)[members]                                               # (ngrp, 8, 4)
        P = pal[:4 * nr]                                                                    # (4nr, 4)
        d = (wantf * wantf).sum(-1)[..., None] - 2 * wantf @ P.T + (P * P).sum(-1)[None, None]   # (ngrp, 8, 4nr)
        d = d.reshape(ngrp, 8, nr, 4)
        off_all = d.argmin(-1); err = d.min(-1).sum(1)                                     # (ngrp, 8, nr), (ngrp, nr)
        r = err.argmin(1)
        best_base = (r * 4).astype(np.int32); best_off = off_all[np.arange(ngrp), :, r]
        return _write14(H, W, L, best_base, best_off)
    # where every pixel lives: target texel (ty, tx), byte k of its word
    ty, tx = np.mgrid[0:H // 2, 0:W // 2]
    probe = np.zeros((H // 2, W // 2, 4), np.uint8)
    lab = np.arange((H // 2) * (W // 2) * 4, dtype=np.uint32).reshape(H // 2, W // 2, 4)   # label of each byte
    # run the same 32 -> 8 bit mapping on labels, 8 bits at a time
    pos = np.zeros((H, W), np.uint32)
    for sh in (0, 8, 16, 24):
        part = ((lab >> sh) & 255).astype(np.uint8)
        words = part.reshape(H // 2, W // 2 * 4).copy().view(np.uint32).reshape(H // 2, W // 2)
        pos |= ct32_to_t8(words, W // 2, H // 2).astype(np.uint32) << sh
    p_ty, p_tx, p_k = (pos // 4) // (W // 2), (pos // 4) % (W // 2), pos % 4               # per final pixel
    grp = ((p_ty // 2) * (W // 8) + (p_tx // 4)) * 4 + p_k                                  # A texel + channel: the base it uses
    ngrp = (H // 4) * (W // 8) * 4
    order = np.argsort(grp.ravel(), kind='stable'); counts = np.bincount(grp.ravel(), minlength=ngrp)
    assert counts.min() == counts.max() == 8
    members = order.reshape(ngrp, 8)                                                       # pixel numbers of each group
    wantf = want.reshape(-1, 4)[members]                                                   # (ngrp, 8, 4)
    # candidates: every base 0..252 (alpha channel k=3: offsets wrap inside the low 2 bits)
    best_err = np.full((ngrp,), np.inf, np.float32); best_base = np.zeros((ngrp,), np.int32); best_off = np.zeros((ngrp, 8), np.int32)
    kch = (np.arange(ngrp) % 4)
    e_all = np.empty((ngrp, 8, 4), np.float32)
    for base in (range(256) if bases is None else bases):
        for s in range(4):
            i_rgb = min(255, base + s); i_a = (base & 0xfc) | ((base + s) & 3)
            ii = np.where(kch == 3, i_a, i_rgb)                                            # index this offset gives, per group
            d = wantf - pal[ii][:, None, :]
            e_all[:, :, s] = (d * d).sum(-1)
        off = e_all.argmin(-1); err = e_all.min(-1).sum(-1)
        better = err < best_err
        best_err[better] = err[better]; best_base[better] = base; best_off[better] = off[better]
    # write the layers
    A = best_base.reshape(H // 4, W // 8, 4).astype(np.uint8)
    offs_px = np.zeros((H * W,), np.uint8); offs_px[members] = best_off.astype(np.uint8)   # per final pixel
    B8 = np.zeros((H // 2, W // 2), np.uint8)                                              # per target texel: 4 x 2 bits
    np.add.at(B8, (p_ty.ravel(), p_tx.ravel()), (offs_px << (2 * p_k.ravel())).astype(np.uint8))
    Bw = t8_to_ct32(B8, W // 4, H // 4)
    data = A.tobytes() + Bw.astype('<u4').tobytes()
    return data, decode14(data, W, H)

def image14(rgba, k=8):
    """(H, W, 4) uint8 image with PS2 alpha (0x80 = opaque) -> (type-14 pixel data, palette in file order).
    The palette is built for the format: the image's k main colours, and for every pair of them a ramp of four
    entries, so an 8-pixel group lying on an edge between two colours finds its four consecutive entries."""
    import numpy as np
    H, W, _ = rgba.shape
    px = rgba.reshape(-1, 4).astype(np.float32)
    # main colours: k-means, seeded with the most frequent colours
    cols, cnt = np.unique(rgba.reshape(-1, 4), axis=0, return_counts=True)
    cent = cols[np.argsort(-cnt)][:1].astype(np.float32)
    while len(cent) < min(k, len(cols)):                      # farthest-point seeding
        d = ((cols[:, None, :].astype(np.float32) - cent[None]) ** 2).sum(-1).min(1) * np.sqrt(cnt)
        cent = np.vstack([cent, cols[np.argmax(d)].astype(np.float32)])
    for _ in range(12):
        lab = ((px[:, None, :] - cent[None]) ** 2).sum(-1).argmin(1)
        for j in range(len(cent)):
            if (lab == j).any(): cent[j] = px[lab == j].mean(0)
    pal = []
    for a in range(len(cent)):
        for b in range(a, len(cent)):
            for t in (0.0, 1 / 3.0, 2 / 3.0, 1.0): pal.append(cent[a] * (1 - t) + cent[b] * t)
            if len(pal) >= 252: break
        if len(pal) >= 252: break
    pal = (pal + [np.zeros(4, np.float32)] * 256)[:256]
    pal = np.clip(np.round(np.array(pal)), 0, 255)
    data, dec = encode14(np.zeros((H, W), np.uint8), pal, want_rgba=rgba.astype(np.float32))
    out = [None] * 256
    for i in range(256): out[csm1(i)] = [int(v) for v in pal[i]]
    return data, out

def image14_photo(rgba, ramps=63, iters=10):
    """(H, W, 4) uint8 photo with PS2 alpha -> (type-14 pixel data, palette in file order), like the game's own
    portraits: every group of 8 pixels that shares a base index gets a ramp of four colours (as in DXT), and the
    63 ramps of the palette are the k-means centres of the groups' own ramps."""
    import numpy as np
    H, W, _ = rgba.shape
    members = groups14(H, W)                                                  # (ngrp, 8) pixel numbers
    px = rgba.reshape(-1, 4).astype(np.float32)[members]                      # (ngrp, 8, 4)
    wt = np.array([1.0, 1.2, 0.8, 1.5], np.float32)                           # error weights r, g, b, alpha
    mean = px.mean(1, keepdims=True); d = (px - mean) * wt
    _, _, vt = np.linalg.svd(d, full_matrices=False)                          # principal direction per group
    ax = vt[:, 0, :]
    t = (d * ax[:, None, :]).sum(-1)
    lo = px[np.arange(len(px)), t.argmin(1)]; hi = px[np.arange(len(px)), t.argmax(1)]
    lum = lambda c: c[..., 0] * 0.3 + c[..., 1] * 0.59 + c[..., 2] * 0.11 + c[..., 3]
    sw = lum(lo) > lum(hi); lo[sw], hi[sw] = hi[sw].copy(), lo[sw].copy()
    ep = np.concatenate([lo, hi], 1)                                           # (ngrp, 8)
    rng = np.random.default_rng(1)
    cent = ep[rng.choice(len(ep), ramps, replace=len(ep) < ramps)].copy()
    for _ in range(iters):
        lab = ((ep[:, None, :] - cent[None]) ** 2).sum(-1).argmin(1)
        cnt = np.bincount(lab, minlength=ramps)
        sums = np.zeros_like(cent); np.add.at(sums, lab, ep)
        empty = cnt == 0
        cent[~empty] = sums[~empty] / cnt[~empty, None]
        if empty.any(): cent[empty] = ep[rng.integers(len(ep), size=int(empty.sum()))]
    pal = []
    for c in cent:
        a, b = c[:4], c[4:]
        for f in (0.0, 1 / 3.0, 2 / 3.0, 1.0): pal.append(a * (1 - f) + b * f)
    pal += [np.zeros(4, np.float32)] * (256 - len(pal))
    pal = np.clip(np.round(np.array(pal)), 0, 255)
    pal[:, 3] = np.clip(pal[:, 3], 0, 128)
    data, dec = encode14(np.zeros((H, W), np.uint8), pal, want_rgba=rgba.astype(np.float32), bases=range(0, 4 * ramps, 4))   # the ramps start at multiples of 4
    out = [None] * 256
    for i in range(256): out[csm1(i)] = [int(v) for v in pal[i]]
    return data, out

_groups14_cache = {}
def groups14(H, W):
    if (H, W) not in _groups14_cache: _groups14_cache[(H, W)] = _groups14(H, W)
    return _groups14_cache[(H, W)]
def _groups14(H, W):
    """the pixels (row-major numbers) of each group of 8 that shares a base index in type 14, (H*W/8, 8)"""
    import numpy as np
    lab = np.arange((H // 2) * (W // 2) * 4, dtype=np.uint32).reshape(H // 2, W // 2, 4)
    pos = np.zeros((H, W), np.uint32)
    for sh in (0, 8, 16, 24):
        part = ((lab >> sh) & 255).astype(np.uint8)
        words = part.reshape(H // 2, W // 2 * 4).copy().view(np.uint32).reshape(H // 2, W // 2)
        pos |= ct32_to_t8(words, W // 2, H // 2).astype(np.uint32) << sh
    p_ty, p_tx, p_k = (pos // 4) // (W // 2), (pos // 4) % (W // 2), pos % 4
    grp = ((p_ty // 2) * (W // 8) + (p_tx // 4)) * 4 + p_k
    return np.argsort(grp.ravel(), kind='stable').reshape(-1, 8)

if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    if d[:2] == b'\x10\xfb':
        sys.path.insert(0, os.path.dirname(__file__)); import refpack; d = refpack.decompress(d)
    f = Ssh(d)
    for s in f.shapes:
        print('%-4s type %2d  %4d x %-4d  %6d bytes' % (s.name, s.type, s.size[0], s.size[1], len(s.chunk)))
        if len(sys.argv) > 2 and s.type in (2, 5, 14):
            import cv2
            os.makedirs(sys.argv[2], exist_ok=True)
            cv2.imwrite(os.path.join(sys.argv[2], s.name.strip() + '.png'), s.rgba()[..., [2, 1, 0, 3]])
