"""Read one file from an ISO9660 image: read_file('game.iso', 'DATA/DATABASE/DATABASE.BIG')"""
import struct
def _walk(f, lba, size, path, out):
    f.seek(lba * 2048); data = f.read(size); i = 0
    while i < len(data):
        l = data[i]
        if l == 0: i = (i // 2048 + 1) * 2048; continue
        r = data[i:i + l]; elba, esz = struct.unpack('<I', r[2:6])[0], struct.unpack('<I', r[10:14])[0]
        name = r[33:33 + r[32]]
        if name not in (b'\0', b'\1'):
            p = (path + '/' if path else '') + name.decode('latin1').split(';')[0]
            if r[25] & 2: _walk(f, elba, esz, p, out)
            else: out[p.upper()] = (elba, esz)
        i += l
def read_file(iso, path):
    with open(iso, 'rb') as f:
        f.seek(16 * 2048); pvd = f.read(2048); assert pvd[1:6] == b'CD001', 'not an ISO9660 image'
        root = pvd[156:190]; ents = {}
        _walk(f, struct.unpack('<I', root[2:6])[0], struct.unpack('<I', root[10:14])[0], '', ents)
        lba, size = ents[path.upper().lstrip('/')]
        f.seek(lba * 2048); return f.read(size)
