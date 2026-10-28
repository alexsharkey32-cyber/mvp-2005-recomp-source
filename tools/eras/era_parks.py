"""The ballparks a club played in that season, from the game's own classic parks -> a league pack's overlay.

    python3 tools/eras/era_parks.py ISO SEASON PACK_DIR

Besides the 2005 parks the disc has the old ones the game offers for exhibition games: Tiger Stadium, Veterans
Stadium, the Astrodome, Riverfront (Cinergy Field), Cleveland Stadium, Metropolitan Stadium, Olympic Stadium, Jack
Murphy (Qualcomm), and for earlier days Shibe Park, Forbes Field, Crosley Field, the Polo Grounds, Griffith Stadium
and Sportsman's Park. A club whose home that season is one of them plays its home games there: the park's files
(DATA/STADIUM/<code>DAY|NITE|DOME.BIG) are put in place of its 2005 park's, every variant of them (an old park
the disc has only for night games is also its day park). Its name follows in the menus (strings.txt), and the
commentators' recording of the 2005 park's name is taken out of their index, as tools/build_stadiums.py does for
renamed parks. Parks the disc does not have (Memorial Stadium, the Kingdome, Candlestick, Three Rivers ...) stay
the 2005 ones.
"""
import sys, os, struct
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import isofile

# the club's 2005 park code, its 2005 name in the game's strings, the id of its spoken name (STDNMHDR.BIG <id>a/b)
CLUB_PARK = {4: ('USCE', 'U.S. Cellular Field™', 4), 5: ('JACO', 'Jacobs Field™', 5), 6: ('COME', 'Comerica Park™', 6),
             8: ('METR', 'Hubert H. Humphrey Metrodome', 8), 18: ('SBCP', 'SBC Park™', 18), 20: ('GABA', 'Great American Ball Park™', 20),
             21: ('MINU', 'Minute Maid Park™', 21), 23: ('PNCP', 'PNC Park™', 23), 24: ('BUSH', 'Busch Stadium™', 24),
             17: ('PETO', 'PETCO Park™', 17), 27: ('RFKS', 'RFK Stadium', 44), 28: ('SHEA', 'Shea Stadium', 28),
             29: ('CITI', 'Citizens Bank Park™', 29), 1: ('NETW', 'Network Associates Coliseum™', 1), 11: ('YANK', 'Yankee Stadium™', 11)}
# club -> [(first, last, old park code, its name)]
ERA = {
    1:  [(1901, 1954, 'SHIB', 'Shibe Park')],
    5:  [(1932, 1993, 'CLEV', 'Cleveland Stadium')],
    6:  [(1912, 1999, 'TIGR', 'Tiger Stadium')],
    8:  [(1911, 1960, 'GRIF', 'Griffith Stadium'), (1961, 1981, 'METP', 'Metropolitan Stadium')],
    17: [(1969, 1980, 'QUAL', 'San Diego Stadium'), (1981, 1996, 'QUAL', 'Jack Murphy Stadium'), (1997, 2003, 'QUAL', 'Qualcomm Stadium')],
    18: [(1911, 1957, 'POLO', 'Polo Grounds')],
    20: [(1912, 1970, 'CROS', 'Crosley Field'), (1971, 1995, 'CINE', 'Riverfront Stadium'), (1996, 2002, 'CINE', 'Cinergy Field')],
    21: [(1965, 1999, 'ASTR', 'Astrodome')],
    23: [(1909, 1970, 'FORB', 'Forbes Field')],
    24: [(1920, 1966, 'SPOT', "Sportsman's Park")],
    27: [(1977, 2004, 'OLYM', 'Olympic Stadium')],
    28: [(1962, 1963, 'POLO', 'Polo Grounds')],
    29: [(1938, 1970, 'SHIB', 'Shibe Park'), (1971, 2003, 'VETE', 'Veterans Stadium')],
}

def picks(season):
    out = {}
    for club, rows in ERA.items():
        for lo, hi, code, name in rows:
            if lo <= season <= hi and club in CLUB_PARK: out[club] = (code, name)
    return out

def write_years(pack):
    """the whole table, for the runtime: the parks change with the seasons of a dynasty"""
    with open(os.path.join(pack, 'park_years.txt'), 'w', encoding='utf-8') as f:
        f.write('# club 2005-park first last park name (tools/eras/era_parks.py); other seasons: the 2005 park\n')
        for club, rows in sorted(ERA.items()):
            if club not in CLUB_PARK: continue
            f.write('= %d %s\n' % (club, CLUB_PARK[club][1]))              # its 2005 park's name in the menus
            for lo, hi, code, name in rows: f.write('%d %s %d %d %s %s\n' % (club, CLUB_PARK[club][0], lo, hi, code, name))

def main():
    iso, season, pack = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    with open(iso, 'rb') as f:
        f.seek(16 * 2048); pvd = f.read(2048); root = pvd[156:190]; files = {}
        isofile._walk(f, struct.unpack('<I', root[2:6])[0], struct.unpack('<I', root[10:14])[0], '', files)
    st = os.path.join(pack, 'DATA', 'STADIUM'); os.makedirs(st, exist_ok=True)
    names, spoken, log = [], [], []
    for club, (code, name) in sorted(picks(season).items()):
        new, old_name, sid = CLUB_PARK[club]
        src = [p for p in files if p.startswith('DATA/STADIUM/' + code) and p.endswith('.BIG')]
        dst = [p for p in files if p.startswith('DATA/STADIUM/' + new) and p.endswith('.BIG')]
        if not src or not dst: continue
        data = isofile.read_file(iso, sorted(src)[0])
        for p in dst: open(os.path.join(st, p.split('/')[-1]), 'wb').write(data)
        names.append((old_name, name)); spoken.append(sid)
        log.append('club %2d: %s (%s) in place of %s' % (club, name, code, new))
    sp = os.path.join(pack, 'strings.txt')
    old = open(sp, encoding='utf-8').read().splitlines() if os.path.exists(sp) else []
    old = [l for l in old if not any(l.startswith(a + '\t') for a, _ in names)]
    with open(sp, 'w', encoding='utf-8') as f:
        f.write('\n'.join(old + ['%s\t%s' % nb for nb in names]) + '\n')
    if spoken:                                                   # the commentators' 2005 park names: out of their index
        big = bytearray(isofile.read_file(iso, 'DATA/AUDIO/SPCH_PBP/STDNMHDR.BIG')); off = 16
        gone = {'%02d%s.hdr' % (i, ab) for i in spoken for ab in 'ab'}
        for _ in range(struct.unpack('>I', big[8:12])[0]):
            off += 8; e = big.index(b'\0', off)
            if big[off:e].decode('latin1') in gone: big[e - 1] = ord('x')
            off = e + 1
        d = os.path.join(pack, 'DATA', 'AUDIO', 'SPCH_PBP'); os.makedirs(d, exist_ok=True)
        open(os.path.join(d, 'STDNMHDR.BIG'), 'wb').write(big)
    write_years(pack)
    open(os.path.join(pack, 'parks.txt'), 'w').write('# era ballparks for %d (tools/eras/era_parks.py)\n' % season + '\n'.join(log) + '\n')
    print('%d clubs in their %d parks' % (len(log), season)); print('\n'.join(log))

if __name__ == '__main__': main()
