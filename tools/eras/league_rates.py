"""Real major-league batting rates per season from the Lahman database, for the simulation's league environment.

    python3 tools/eras/league_rates.py LAHMAN_DIR OUT.txt

OUT: '# year k bb hbp 1b 2b 3b hr' then one line per season, each a share of plate appearances (bb with the
intentional ones). The runtime (hle.cpp, hle_pa_probs) steers simulated seasons toward the line of the year
being played; packs carry it as rates.txt.
"""
import csv, os, sys, collections

def rates(lahman):
    t = collections.defaultdict(collections.Counter)
    for r in csv.DictReader(open(os.path.join(lahman, 'Batting.csv'), encoding='utf-8-sig')):
        y = int(r['yearID'])
        if r['lgID'] not in ('AL', 'NL'): continue
        for k in ('AB', 'H', '2B', '3B', 'HR', 'BB', 'SO', 'HBP', 'SF', 'SH'):
            t[y][k] += int(r[k] or 0)
    out = {}
    for y, s in sorted(t.items()):
        pa = s['AB'] + s['BB'] + s['HBP'] + s['SF'] + s['SH']
        if pa < 10000 or not s['SO']: continue
        b1 = s['H'] - s['2B'] - s['3B'] - s['HR']
        out[y] = dict(k=s['SO'] / pa, bb=s['BB'] / pa, hbp=s['HBP'] / pa, b1=b1 / pa, b2=s['2B'] / pa, b3=s['3B'] / pa, hr=s['HR'] / pa)
    return out

def write(out, path, years=None):
    with open(path, 'w') as f:
        f.write('# year k bb hbp 1b 2b 3b hr (shares of plate appearances, real major leagues)\n')
        for y, r in sorted(out.items()):
            if years and y not in years: continue
            f.write('%d %.4f %.4f %.4f %.4f %.4f %.4f %.4f\n' % (y, r['k'], r['bb'], r['hbp'], r['b1'], r['b2'], r['b3'], r['hr']))

if __name__ == '__main__':
    write(rates(sys.argv[1]), sys.argv[2])
