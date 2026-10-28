"""Extract SYSTEM.CNF and the main executable from your own MVP Baseball 2005 (USA) ISO.
Usage: python3 tools/extract_iso.py "MVP Baseball 2005 (USA).iso"
Writes iso/SLUS_211.35 and iso/SYSTEM.CNF (both git-ignored)."""
import struct, sys, os, hashlib
F = open(sys.argv[1], 'rb')
def rd(lba, n): F.seek(lba*2048); return F.read(n)
pvd = rd(16, 2048); assert pvd[1:6] == b'CD001', 'not an ISO9660 image'
entries = []
def walk(lba, size, path):
    data = rd(lba, size); i = 0
    while i < len(data):
        l = data[i]
        if l == 0: i = ((i//2048)+1)*2048; continue
        r = data[i:i+l]
        elba = struct.unpack('<I', r[2:6])[0]; esz = struct.unpack('<I', r[10:14])[0]
        name = r[33:33+r[32]]
        if name not in (b'\x00', b'\x01'):
            p = path + '/' + name.decode(errors='replace').split(';')[0]
            if r[25] & 2: walk(elba, esz, p)
            else: entries.append((p, elba, esz))
        i += l
root = pvd[156:190]
walk(struct.unpack('<I', root[2:6])[0], struct.unpack('<I', root[10:14])[0], '')
os.makedirs('iso', exist_ok=True)
for p, l, s in entries:
    if p in ('/SYSTEM.CNF', '/SLUS_211.35'):
        b = rd(l, s); open('iso' + p, 'wb').write(b)
        print(p, s, 'bytes  sha1', hashlib.sha1(b).hexdigest())
