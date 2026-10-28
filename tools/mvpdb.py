"""MVP Baseball 2005 database (DATA/DATABASE/DATABASE.BIG): a BIGF archive of RefPack-compressed tables.
Each table is text: a header line "0 name,1 name,...,;" then one line per record
"<9-hex-digit key>,0 value,1 value,...,;" (CRLF line ends)."""
import struct, sys, os
sys.path.insert(0, os.path.dirname(__file__))
import refpack, bigfile

class Table:
    def __init__(self, name, text):
        self.name = name
        lines = text.split('\r\n')
        self.header = lines[0]
        self.cols = [h.split(' ', 1)[1] for h in self.header.rstrip(';').rstrip(',').split(',') if ' ' in h]
        self.rows = []                              # [key, [values]]
        for ln in lines[1:]:
            if not ln: continue
            parts = ln.rstrip(';').rstrip(',').split(',')
            vals = []
            for i, p in enumerate(parts[1:]):
                idx, _, v = p.partition(' ')
                assert int(idx) == i, (name, ln)
                vals.append(v)
            self.rows.append([parts[0], vals])
    def text(self):
        out = [self.header]
        for k, vals in self.rows:
            out.append(k + ',' + ''.join('%d %s,' % (i, v) for i, v in enumerate(vals)) + ';')
        return '\r\n'.join(out) + '\r\n'
    def col(self, name): return self.cols.index(name)
    def by_key(self): return {k: v for k, v in self.rows}

def read_database(path):
    d = open(path, 'rb').read()
    tables = {}
    order = []
    for name, o, s in bigfile.read_big(d):
        tables[name] = Table(name, refpack.decompress(d[o:o+s]).decode('latin1'))
        order.append(name)
    return tables, order

def write_big(files):
    """files: list of (name, bytes) -> BIGF archive bytes (layout as EA's: header then data, unpadded)."""
    hdr = sum(8 + len(n) + 1 for n, _ in files) + 16
    off = hdr; ents = b''; data = b''
    for n, b in files:
        ents += struct.pack('>II', off, len(b)) + n.encode('latin1') + b'\0'; off += len(b); data += b
    return b'BIGF' + struct.pack('<I', hdr + len(data)) + struct.pack('>II', len(files), hdr) + ents + data

def sort_roster(tables):
    """roster.dat grouped by club in team.dat's order, as on the disc. The game numbers the clubs by their first
    appearance in roster.dat: rows of a club out of that order shift every later club's row, and with it the
    division read for it (standings with minor-league clubs in big-league divisions, a season that never ends)."""
    if 'roster.dat' not in tables or 'team.dat' not in tables: return
    pos = {k: i for i, (k, v) in enumerate(tables['team.dat'].rows)}
    rows = tables['roster.dat'].rows
    rows[:] = sorted(rows, key=lambda r: pos.get(r[1][0], len(pos)) if len(r[1]) > 1 else len(pos))

def write_database(path, tables, order, companions=True):
    sort_roster(tables)
    files = []
    for n in order:
        raw = tables[n].text().encode('latin1')
        c = refpack.compress(raw)
        assert refpack.decompress(c) == raw
        files.append((n, c))
    open(path, 'wb').write(write_big(files))
    if companions:
        for cp in commentary_databases(path): refresh_commentary(cp, tables, order)

# The commentary-names overlay (tools/build_rosters.py: mods/commentary-<season>, a pack's commentary/) carries a
# whole copy of the database, loaded after the league's own, with the players' and managers' audio ids filled in.
# Whatever changes the league's database afterwards (faces, salaries, names ...) must reach that copy too, or the
# game shows the copy's older rows: each write of a league's database writes the copy again from it, keeping only
# the copy's audio ids.
AUDIO_COLS = {'attrib.dat': 'playerattrib_audioid', 'manager.dat': 'manager_audioid'}
def commentary_databases(path):
    import os
    p = os.path.abspath(path); tail = os.path.join('DATA', 'DATABASE', 'DATABASE.BIG')
    if not p.endswith(tail): return []
    root = p[:-len(tail)].rstrip(os.sep); base = os.path.basename(root)
    if base == 'commentary' or base.startswith('commentary-'): return []
    out = [os.path.join(root, 'commentary', tail)]
    if base.startswith('rosters-'): out.append(os.path.join(os.path.dirname(root), 'commentary-' + base[len('rosters-'):], tail))
    return [c for c in out if os.path.exists(c)]

def refresh_commentary(cpath, tables, order):
    C, _ = read_database(cpath)
    saved = []
    for tn, col in AUDIO_COLS.items():
        if tn not in tables or tn not in C or col not in tables[tn].cols: continue
        i = tables[tn].col(col); ids = {k: v[C[tn].col(col)] for k, v in C[tn].rows}
        for k, v in tables[tn].rows:
            if k in ids and v[i] != ids[k]: saved.append((v, i, v[i])); v[i] = ids[k]
    try: write_database(cpath, tables, order, companions=False)
    finally:
        for v, i, x in saved: v[i] = x
