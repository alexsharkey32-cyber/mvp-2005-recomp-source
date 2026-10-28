"""EA .LOC string tables (FEENG.LOC / IGENG.LOC): LOCH header, LOCI id->index table, LOCL offsets + UTF-16 strings."""
import struct
class Loc:
    def __init__(self, data):
        assert data[:4] == b'LOCH'
        self.head = data[:20]
        loci_size = struct.unpack('<I', data[24:28])[0]
        l = struct.unpack('<I', data[16:20])[0]            # offset of LOCL
        self.pre = data[:l]
        assert data[l:l + 4] == b'LOCL'
        size, z, n = struct.unpack('<III', data[l + 4:l + 16])
        offs = struct.unpack('<%dI' % n, data[l + 16:l + 16 + 4 * n])
        self.l_zero = z
        self.strings = []
        for o in offs:
            e = o
            while data[l + e:l + e + 2] != b'\0\0': e += 2
            self.strings.append(data[l + o:l + e].decode('utf-16-le'))
        self.tail = data[l + size:]                       # anything after LOCL (kept)
    def build(self):
        n = len(self.strings)
        body = b''; offs = []
        base = 16 + 4 * n
        for s in self.strings:
            offs.append(base + len(body)); body += s.encode('utf-16-le') + b'\0\0\0\0'
        locl_payload = struct.pack('<II', self.l_zero, n) + struct.pack('<%dI' % n, *offs) + body
        locl_payload += b'\0' * (-(len(locl_payload) + 8) % 4)
        locl = b'LOCL' + struct.pack('<I', len(locl_payload) + 8) + locl_payload
        return self.pre + locl + self.tail
