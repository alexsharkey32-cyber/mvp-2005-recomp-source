"""Writes Real-ESRGAN checkpoints (.pth) as ONNX files without PyTorch or the onnx package: the few protobuf
messages an ONNX model needs are encoded by hand. onnxruntime then runs the models several times faster than
the NumPy fallback in esrgan.py (and on a Mac it can use Core ML).

    python3 tools/hd/onnx_export.py <model.pth> <model.onnx>
"""
import struct, sys, os, numpy as np
sys.path.insert(0, os.path.dirname(__file__))
import esrgan

def _vi(n):
    out = b''
    n &= (1 << 64) - 1
    while True:
        b = n & 0x7f; n >>= 7
        if n: out += bytes([b | 0x80])
        else: return out + bytes([b])
def _f(field, wire, payload): return _vi(field << 3 | wire) + payload
def _int(field, v): return _f(field, 0, _vi(v))
def _bytes(field, b): return _f(field, 2, _vi(len(b)) + b)
def _str(field, s): return _bytes(field, s.encode())

def tensor(name, arr):
    arr = np.ascontiguousarray(arr)
    dt = {np.dtype('float32'): 1, np.dtype('int64'): 7}[arr.dtype]
    return b''.join(_int(1, d) for d in arr.shape) + _int(2, dt) + _str(8, name) + _bytes(9, arr.tobytes())
def attr(name, v):
    a = _str(1, name)
    if isinstance(v, float): a += _f(2, 5, struct.pack('<f', v)) + _int(20, 1)
    elif isinstance(v, int): a += _int(3, v) + _int(20, 2)
    elif isinstance(v, str): a += _bytes(4, v.encode()) + _int(20, 3)
    elif isinstance(v, (list, tuple)): a += b''.join(_int(8, x) for x in v) + _int(20, 7)
    else: raise TypeError(v)
    return a
def value_info(name, shape):
    dims = b''.join(_bytes(1, _str(2, d) if isinstance(d, str) else _int(1, d)) for d in shape)
    return _str(1, name) + _bytes(2, _bytes(1, _int(1, 1) + _bytes(2, dims)))

class Graph:
    def __init__(self): self.nodes, self.inits, self.n = [], [], 0
    def new(self): self.n += 1; return 't%d' % self.n
    def init(self, arr): name = 'w%d' % len(self.inits); self.inits.append(tensor(name, arr)); return name
    def node(self, op, inputs, **attrs):
        out = self.new()
        self.nodes.append(b''.join(_str(1, i) for i in inputs) + _str(2, out) + _str(4, op) + b''.join(_bytes(5, attr(k, v)) for k, v in attrs.items()))
        return out
    def conv(self, x, w, b): return self.node('Conv', [x, self.init(w), self.init(b)], kernel_shape=[3, 3], pads=[1, 1, 1, 1])
    def lrelu(self, x): return self.node('LeakyRelu', [x], alpha=0.2)
    def up(self, x, k):
        return self.node('Resize', [x, '', self.init(np.array([1, 1, k, k], np.float32))], mode='nearest', nearest_mode='floor',
                         coordinate_transformation_mode='asymmetric')
    def model(self, inp, out):
        g = b''.join(_bytes(1, n) for n in self.nodes) + _str(2, 'realesrgan') + b''.join(_bytes(5, t) for t in self.inits)
        g += _bytes(11, value_info(inp, [1, 3, 'h', 'w'])) + _bytes(12, value_info(out, [1, 3, 'H', 'W']))
        return _int(1, 8) + _str(2, 'mvp2005-tools') + _bytes(7, g) + _bytes(8, _str(1, '') + _int(2, 13))

def export(pth, onnx_path):
    sd = esrgan.load_pth(pth)
    g = Graph(); x = 'input'
    if 'conv_first.weight' in sd:                                # RRDBNet
        c = lambda t, name: g.conv(t, sd[name + '.weight'], sd[name + '.bias'])
        k02 = g.init(np.array(0.2, np.float32))
        def rdb(t, pre):
            x1 = g.lrelu(c(t, pre + '.conv1'))
            x2 = g.lrelu(c(g.node('Concat', [t, x1], axis=1), pre + '.conv2'))
            x3 = g.lrelu(c(g.node('Concat', [t, x1, x2], axis=1), pre + '.conv3'))
            x4 = g.lrelu(c(g.node('Concat', [t, x1, x2, x3], axis=1), pre + '.conv4'))
            x5 = c(g.node('Concat', [t, x1, x2, x3, x4], axis=1), pre + '.conv5')
            return g.node('Add', [g.node('Mul', [x5, k02]), t])
        nb = 1 + max(int(k.split('.')[1]) for k in sd if k.startswith('body.') and 'rdb' in k)
        feat = c(x, 'conv_first'); body = feat
        for i in range(nb):
            o = rdb(rdb(rdb(body, 'body.%d.rdb1' % i), 'body.%d.rdb2' % i), 'body.%d.rdb3' % i)
            body = g.node('Add', [g.node('Mul', [o, k02]), body])
        feat = g.node('Add', [feat, c(body, 'conv_body')])
        feat = g.lrelu(c(g.up(feat, 2), 'conv_up1'))
        feat = g.lrelu(c(g.up(feat, 2), 'conv_up2'))
        out = c(g.lrelu(c(feat, 'conv_hr')), 'conv_last')
    else:                                                        # SRVGGNetCompact
        n = max(int(k.split('.')[1]) for k in sd if k.startswith('body.')); t = x
        for i in range(n + 1):
            w = sd['body.%d.weight' % i]
            t = g.conv(t, w, sd['body.%d.bias' % i]) if w.ndim == 4 else g.node('PRelu', [t, g.init(w.reshape(-1, 1, 1))])
        t = g.node('DepthToSpace', [t], blocksize=4, mode='CRD')
        out = g.node('Add', [t, g.up(x, 4)])
    open(onnx_path, 'wb').write(g.model('input', out))

if __name__ == '__main__':
    export(sys.argv[1], sys.argv[2]); print('wrote', sys.argv[2])
