"""EA BIGF archive reader (big-endian header): python3 tools/bigfile.py file.BIG outdir"""
import struct, sys, os
def read_big(data):
    assert data[:4] in (b'BIGF', b'BIG4'), data[:4]
    n, hdr = struct.unpack('>II', data[8:16])
    off = 16; ents = []
    for _ in range(n):
        o, s = struct.unpack('>II', data[off:off+8]); off += 8
        e = data.index(b'\0', off); name = data[off:e].decode('latin1'); off = e + 1
        ents.append((name, o, s))
    return ents
if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    for name, o, s in read_big(d):
        print(f'{o:10d} {s:10d} {name}')
        if len(sys.argv) > 2:
            p = os.path.join(sys.argv[2], name.replace('\\', '/')); os.makedirs(os.path.dirname(p) or '.', exist_ok=True)
            open(p, 'wb').write(d[o:o+s])
