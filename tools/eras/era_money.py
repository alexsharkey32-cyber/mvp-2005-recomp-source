"""Salaries of a league pack at the money of its season.

    python3 tools/eras/era_money.py PACK_DIR SEASON

The roster builder pays players on today's scale (a star 300 x $100,000). In 1977 the average big-league salary
was $76,066, about 1/68 of 2025's: every salary (players, the pack's draft classes) is scaled
by the ratio of the season's average salary to 2025's (at least the game's smallest unit, $100,000), so a 1977
Red Sox payroll is a few million dollars, not four hundred. Club budgets in the
database are all the same number (100000, as on the disc), so they are left alone.
"""
import os, sys, csv
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import mvpdb

AVG = {1876: 1500, 1900: 2500, 1920: 5000, 1930: 7500, 1946: 11000, 1950: 13000, 1960: 18000, 1967: 19000, 1970: 29303,
       1975: 44676, 1976: 51501, 1977: 76066, 1978: 99876, 1979: 113558, 1980: 143756, 1981: 185651, 1982: 241497,
       1983: 289194, 1984: 329408, 1985: 371571, 1986: 412520, 1987: 412454, 1988: 438729, 1989: 497254, 1990: 597537,
       1991: 851492, 1992: 1028667, 1993: 1076089, 1994: 1168263, 1995: 1110766, 1996: 1119981, 1997: 1336609,
       1998: 1398831, 1999: 1611166, 2000: 1895630, 2001: 2138896, 2002: 2295649, 2003: 2372189, 2004: 2313535,
       2005: 2476589, 2010: 3297828, 2015: 3952252, 2020: 4432000, 2025: 5160000}

def avg(y):
    ks = sorted(AVG)
    if y <= ks[0]: return AVG[ks[0]]
    if y >= ks[-1]: return AVG[ks[-1]]
    for a, b in zip(ks, ks[1:]):
        if a <= y <= b: return AVG[a] + (AVG[b] - AVG[a]) * (y - a) / (b - a)

def main():
    pack, season = sys.argv[1], int(sys.argv[2])
    f = avg(season) / AVG[2025]
    path = os.path.join(pack, 'DATA', 'DATABASE', 'DATABASE.BIG')
    T, order = mvpdb.read_database(path)
    at = T['attrib.dat']; si = at.col('playerattrib_salary')
    for k, v in at.rows: v[si] = str(max(1, int(round(int(v[si]) * f))))
    # (org.dat's org_budget is 100000 for every club, on the disc too: left as it is)
    mvpdb.write_database(path, T, order)
    d = os.path.join(pack, 'draft')
    if os.path.isdir(d):
        for fn in os.listdir(d):
            if not fn.endswith('.csv'): continue
            p = os.path.join(d, fn); rows = list(csv.DictReader(open(p, encoding='latin1')))
            if not rows or 'salary' not in rows[0]: continue
            for r in rows:
                try: r['salary'] = str(max(1, int(round(int(r['salary']) * f))))
                except ValueError: pass
            with open(p, 'w', newline='', encoding='latin1') as fo:
                w = csv.DictWriter(fo, list(rows[0].keys())); w.writeheader(); w.writerows(rows)
    print('%s: money of %d (x %.4f of 2025)' % (pack, season, f))

if __name__ == '__main__': main()
