"""The game's head models (MODELS.BIG g001-g045.ord): VIF batches of up to 56 vertices.

Per batch: UNPACK V4-32 (header) -> VU 0, V4-8 x3 -> 1-3, STMOD 1 (offset: STROW 1.0) + UNPACK V4-16 unsigned -> 4
(the positions: x, y, z as 16-bit fixed point, w a per-vertex value), STMOD 0 + UNPACK V4-16 signed -> 64 (texture
coordinates s, t x 4096 and two small indices), STMASK + V3-32 -> 124, V4-8 x3 -> 184-186, MSCNT.
Positions were confirmed against a recorded draw of the Edit Player close-up: screen x, y follow an affine map of
the 16-bit x, y, z within half a pixel.
"""
import struct, numpy as np

def _walk(data, i):
    while i + 4 <= len(data):
        w = struct.unpack_from('<I', data, i)[0]; cmd = (w >> 24) & 0x7f; num = (w >> 16) & 0xff; imm = w & 0xffff
        if (cmd & 0x60) == 0x60:
            vn, vl = (cmd >> 2) & 3, cmd & 3; cnt = num or 256
            size = (cnt * ((32 >> vl) // 8) * (vn + 1) + 3) & ~3
            yield ('U', i + 4, vn, vl, cnt, imm & 0x3ff); i += 4 + size; continue
        if cmd == 0x30 or cmd == 0x31: i += 20; continue
        if cmd == 0x20: i += 8; continue
        if cmd in (0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x10, 0x11, 0x13, 0x14, 0x15, 0x17): i += 4; continue
        return

def first_batch(data):
    """offset of the first batch's header unpack (V4-32 x1 -> 0, followed by V4-8 -> 1)"""
    for i in range(0, len(data) - 32, 4):
        w = struct.unpack_from('<I', data, i)[0]
        if (w >> 24) & 0x7f == 0x6c and (w >> 16) & 0xff == 1 and w & 0x3ff == 0:
            w2 = struct.unpack_from('<I', data, i + 20)[0]
            if (w2 >> 24) & 0x7f == 0x6e and w2 & 0x3ff == 1: return i
    return None

class Head:
    def __init__(self, ord_bytes):
        self.raw = bytearray(ord_bytes); self.base = 64
        data = bytes(self.raw[self.base:]); start = first_batch(data)
        self.batches = []; cur = None
        for u in _walk(data, start):
            _, off, vn, vl, cnt, addr = u
            if (vn, vl) == (3, 1) and addr == 4: cur = dict(pos_off=off, n=cnt)
            elif (vn, vl) == (3, 1) and addr == 64 and cur: cur['uv_off'] = off; self.batches.append(cur); cur = None
        self.pos = np.vstack([np.frombuffer(data, np.uint16, b['n'] * 4, b['pos_off']).reshape(-1, 4) for b in self.batches]).copy()
        self.uv = np.vstack([np.frombuffer(data, np.int16, b['n'] * 4, b['uv_off']).reshape(-1, 4) for b in self.batches]).copy()
    def xyz(self): return self.pos[:, :3].astype(np.float64)
    def set_xyz(self, p):
        q = np.clip(np.round(p), 0, 65535).astype(np.uint16); k = 0
        for b in self.batches:
            blk = self.pos[k:k + b['n']].copy(); blk[:, :3] = q[k:k + b['n']]
            self.raw[self.base + b['pos_off']:self.base + b['pos_off'] + b['n'] * 8] = blk.tobytes(); k += b['n']
        self.pos[:, :3] = q
    def bytes(self): return bytes(self.raw)
