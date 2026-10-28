"""The uniforms a club wore in a season, from the game's own wardrobe -> a league pack's overlay.

    python3 tools/eras/era_uniforms.py ISO SEASON PACK_DIR

Every club on the disc has home (a) and road (b) sets plus throwbacks (g, h, i, ...): the 1970s Astros rainbow,
the Brewers' powder blue, the Expos, the Pilots, the Browns, the Brooklyn Dodgers and the New York Giants ... For
the season given, a club whose look then is one of those gets it as its home / road set: the textures
(MODELS.BIG u<club><slot>.ssh) and the numbers' font (f<club><slot>.ssh) are copied over the a / b members in
PACK_DIR/DATA/MODELS.BIG/. Nothing is drawn: a club whose look is not in the wardrobe keeps the 2005 one. The
menus' jersey thumbnails and the logos stay as they are (the game has none for the throwbacks).
Picked by eye from the disc's textures (club ids as in MODELS.BIG; 27 is Montreal / Washington).
"""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import isofile, refpack
from bigfile import read_big

# club -> [(first season, last season, home slot, road slot)]
ERA = {
    0:  [(1961, 1996, 'g', 'h'), (1997, 2001, 'i', 'j')],                                       # Angels
    1:  [(1901, 1954, 'g', 'h'), (1955, 1967, 'k', 'l'), (1968, 1986, 'i', 'j')],             # Philadelphia / Kansas City / Oakland A's
    2:  [(1977, 1992, 'g', 'h')],                                                               # Mariners
    3:  [(1972, 1983, 'g', 'h'), (1984, 1993, 'i', 'j')],                                       # Rangers
    4:  [(1901, 1970, 'g', 'h'), (1971, 1990, 'i', 'j')],                                       # White Sox
    5:  [(1901, 1977, 'g', 'h'), (1978, 1993, 'i', 'j')],                                       # Cleveland
    6:  [(1901, 1993, 'a', 'j')],                                                               # Tigers: the "DETROIT" road
    7:  [(1969, 1982, 'g', 'h'), (1983, 1991, 'i', 'j')],                                       # Royals
    8:  [(1901, 1960, 'k', 'l'), (1961, 1972, 'g', 'h'), (1973, 1986, 'i', 'j')],             # Senators / Twins
    9:  [(1902, 1953, 'k', 'k'), (1954, 1988, 'g', 'b')],                                       # Browns / Orioles (cartoon bird)
    10: [(1901, 1907, 'k', 'h'), (1908, 1930, 'i', 'j'), (1931, 1945, 'g', 'h')],             # Red Sox
    11: [(1903, 1914, 'i', 'j')],                                                               # Highlanders
    12: [(1998, 2007, 'g', 'h')],                                                               # Devil Rays
    13: [(1977, 1996, 'g', 'h')],                                                               # Blue Jays
    16: [(1901, 1957, 'i', 'j')],                                                               # Brooklyn Dodgers
    17: [(1969, 1990, 'g', 'h')],                                                               # Padres, brown and gold
    18: [(1901, 1929, 'i', 'j'), (1930, 1957, 'g', 'h')],                                       # New York Giants
    19: [(1901, 1975, 'g', 'h'), (1976, 1981, 'g', 'j'), (1982, 1996, 'g', 'h')],             # Cubs (powder-blue pinstripe road)
    20: [(1901, 1998, 'g', 'h')],                                                               # Reds
    21: [(1962, 1964, 'i', 'j'), (1965, 1974, 'h', 'b'), (1975, 1986, 'g', 'g'), (1987, 1993, 'h', 'h')],   # Colt .45s / Astros
    22: [(1969, 1969, 'l', 'm'), (1970, 1993, 'g', 'h'), (1994, 1999, 'i', 'j')],             # Pilots / Brewers
    23: [(1901, 1969, 'g', 'h'), (1970, 1984, 'i', 'k')],                                       # Pirates
    24: [(1901, 1955, 'g', 'h'), (1971, 1984, 'a', 'j')],                                       # Cardinals
    25: [(1901, 1965, 'i', 'j'), (1972, 1979, 'g', 'h'), (1980, 1986, 'g', 'k')],             # Boston / Milwaukee / Atlanta Braves
    26: [(1993, 2002, 'i', 'j')],                                                               # Marlins, teal
    27: [(1969, 1991, 'g', 'h'), (1992, 2004, 'i', 'j')],                                       # Expos
    28: [(1962, 1997, 'g', 'h')],                                                               # Mets
    29: [(1970, 1991, 'g', 'h')],                                                               # Phillies, maroon
}

def picks(season):
    out = {}
    for club, rows in ERA.items():
        for lo, hi, home, road in rows:
            if lo <= season <= hi: out[club] = (home, road)
    return out

def write_years(pack):
    """the whole table, for the runtime: the uniforms change with the seasons of a dynasty (club first last home road)"""
    with open(os.path.join(pack, 'uniform_years.txt'), 'w') as f:
        f.write('# club first-season last-season home-slot road-slot (tools/eras/era_uniforms.py); other seasons: a b\n')
        for club, rows in sorted(ERA.items()):
            for lo, hi, home, road in rows: f.write('%d %d %d %s %s\n' % (club, lo, hi, home, road))

def main():
    iso, season, pack = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    d = isofile.read_file(iso, 'DATA/MODELS.BIG')
    ents = {n: (o, s) for n, o, s in read_big(d)}
    def raw(n): o, s = ents[n]; return d[o:o + s]      # as stored (the overlay takes compressed members as they are)
    outdir = os.path.join(pack, 'DATA', 'MODELS.BIG'); os.makedirs(outdir, exist_ok=True)
    log = []
    for club, (home, road) in sorted(picks(season).items()):
        for dst, src in (('a', home), ('b', road)):
            if src == dst: continue
            for kind in ('u', 'f'):
                n_src, n_dst = '%s%03d%s.ssh' % (kind, club, src), '%s%03d%s.ssh' % (kind, club, dst)
                if n_src in ents and n_dst in ents:
                    open(os.path.join(outdir, n_dst), 'wb').write(raw(n_src))
        log.append('club %2d: home %s, road %s' % (club, home, road))
    write_years(pack)
    open(os.path.join(pack, 'uniforms.txt'), 'w').write('# era uniforms for %d (tools/eras/era_uniforms.py)\n' % season + '\n'.join(log) + '\n')
    print('%d clubs in their %d uniforms -> %s' % (len(log), season, outdir))

if __name__ == '__main__': main()
