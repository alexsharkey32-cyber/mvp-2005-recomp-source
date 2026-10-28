"""Real-ESRGAN inference in plain NumPy (no PyTorch needed).

Loads the official .pth checkpoints (torch zip format, read with a small custom unpickler) and runs
  - RRDBNet (RealESRGAN_x4plus: 23 blocks, RealESRGAN_x4plus_anime_6B: 6 blocks)
  - SRVGGNetCompact (realesr-general-x4v3)
Convolutions are im2col + float32 matrix products, so speed is whatever BLAS NumPy has.
Real-ESRGAN by Xintao Wang et al., BSD-3-Clause: https://github.com/xinntao/Real-ESRGAN
"""
import zipfile, pickle, numpy as np

# ---------------------------------------------------------------- checkpoint loading
class _Storage:
    def __init__(self, key, dtype): self.key, self.dtype = key, dtype
def load_pth(path):
    z = zipfile.ZipFile(path)
    prefix = z.namelist()[0].split('/')[0]
    dtypes = {'FloatStorage': np.float32, 'HalfStorage': np.float16, 'LongStorage': np.int64, 'IntStorage': np.int32}
    class U(pickle.Unpickler):
        def find_class(self, mod, name):
            if name in dtypes: return name
            if name == '_rebuild_tensor_v2':
                def rebuild(storage, offset, size, stride, *a):
                    st = storage
                    buf = np.frombuffer(z.read('%s/data/%s' % (prefix, st.key)), dtype=st.dtype)
                    n = int(np.prod(size)) if size else 1
                    if not size: return buf[offset].copy()
                    # strides are in elements
                    return np.lib.stride_tricks.as_strided(buf[offset:], shape=size, strides=[s * buf.itemsize for s in stride]).copy()
                return rebuild
            if mod == 'collections' and name == 'OrderedDict':
                import collections; return collections.OrderedDict
            return super().find_class(mod, name)
        def persistent_load(self, pid):
            # ('storage', storage_type, key, location, numel)
            return _Storage(pid[2], dtypes[pid[1]] if isinstance(pid[1], str) else np.float32)
    d = U(z.open('%s/data.pkl' % prefix)).load()
    for k in ('params_ema', 'params', 'state_dict'):
        if isinstance(d, dict) and k in d: d = d[k]; break
    return {k: v.astype(np.float32) for k, v in d.items()}

# ---------------------------------------------------------------- layers (HWC float32)
def conv3(x, w, b):
    """3x3 convolution, padding 1. x: HxWxC, w: OxCx3x3 -> HxWxO"""
    H, W, C = x.shape
    p = np.pad(x, ((1, 1), (1, 1), (0, 0)))
    s = p.strides
    cols = np.lib.stride_tricks.as_strided(p, shape=(H, W, 3, 3, C), strides=(s[0], s[1], s[0], s[1], s[2]))
    wm = w.transpose(2, 3, 1, 0).reshape(9 * C, -1)            # (ky,kx,c) x O
    out = cols.reshape(H * W, 9 * C) @ wm
    out += b
    return out.reshape(H, W, -1)
def lrelu(x): return np.maximum(x, 0.2 * x)
def up2(x): return x.repeat(2, axis=0).repeat(2, axis=1)

class RRDBNet:
    def __init__(self, sd):
        self.sd = sd
        self.nb = 1 + max(int(k.split('.')[1]) for k in sd if k.startswith('body.') and 'rdb' in k)
    def c(self, x, name): return conv3(x, self.sd[name + '.weight'], self.sd[name + '.bias'])
    def rdb(self, x, pre):
        x1 = lrelu(self.c(x, pre + '.conv1'))
        x2 = lrelu(self.c(np.concatenate([x, x1], 2), pre + '.conv2'))
        x3 = lrelu(self.c(np.concatenate([x, x1, x2], 2), pre + '.conv3'))
        x4 = lrelu(self.c(np.concatenate([x, x1, x2, x3], 2), pre + '.conv4'))
        x5 = self.c(np.concatenate([x, x1, x2, x3, x4], 2), pre + '.conv5')
        return x5 * 0.2 + x
    def __call__(self, img):
        feat = self.c(img, 'conv_first')
        body = feat
        for i in range(self.nb):
            pre = 'body.%d' % i
            o = self.rdb(body, pre + '.rdb1'); o = self.rdb(o, pre + '.rdb2'); o = self.rdb(o, pre + '.rdb3')
            body = o * 0.2 + body
        feat = feat + self.c(body, 'conv_body')
        feat = lrelu(self.c(up2(feat), 'conv_up1'))
        feat = lrelu(self.c(up2(feat), 'conv_up2'))
        return self.c(lrelu(self.c(feat, 'conv_hr')), 'conv_last')

class SRVGG:
    def __init__(self, sd): self.sd = sd; self.n = max(int(k.split('.')[1]) for k in sd if k.startswith('body.'))
    def __call__(self, img):
        x = img; i = 0
        while i <= self.n:
            w = self.sd['body.%d.weight' % i]
            if w.ndim == 4:
                x = conv3(x, w, self.sd['body.%d.bias' % i])
            else:                                                    # PReLU
                x = np.where(x >= 0, x, x * w.reshape(1, 1, -1))
            i += 1
        H, W, C = x.shape                                            # pixel shuffle x4: C = 3*16
        x = x.reshape(H, W, 3, 4, 4).transpose(0, 3, 1, 4, 2).reshape(H * 4, W * 4, 3)
        return x + img.repeat(4, 0).repeat(4, 1)

def load(path):
    sd = load_pth(path)
    return RRDBNet(sd) if 'conv_first.weight' in sd else SRVGG(sd)

def upscale(model, rgb, tile=96, pad=8):
    """rgb: HxWx3 uint8 -> 4x upscaled uint8, processed in tiles with overlap to bound memory"""
    x = rgb.astype(np.float32) / 255.0
    H, W, _ = x.shape
    out = np.zeros((H * 4, W * 4, 3), np.float32)
    for y0 in range(0, H, tile):
        for x0 in range(0, W, tile):
            y1, x1 = min(H, y0 + tile), min(W, x0 + tile)
            ya, xa = max(0, y0 - pad), max(0, x0 - pad)
            yb, xb = min(H, y1 + pad), min(W, x1 + pad)
            r = model(x[ya:yb, xa:xb])
            out[y0 * 4:y1 * 4, x0 * 4:x1 * 4] = r[(y0 - ya) * 4:(y0 - ya + y1 - y0) * 4, (x0 - xa) * 4:(x0 - xa + x1 - x0) * 4]
    return (np.clip(out, 0, 1) * 255 + 0.5).astype(np.uint8)
