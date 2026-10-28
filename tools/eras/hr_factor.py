"""Home-run and hit factors for an era pack: how far each hitter's real home-run rate is from what the simulation gives a
hitter of his power rating, so the sluggers of an era stand as far above the league as they did (Ruth 1927).

    python3 tools/eras/hr_factor.py PACK_DIR LAHMAN_DIR [--dry] [--hits-only]

The ratings are quantile-mapped onto the 2005 game's (tools/build_rosters.py), so the top of every era is rated
96-100 and the simulation gives the best power hitters about twice the league's home runs a plate appearance,
whatever they really hit: in a simulated 1927 Ruth hit 15 (real 60), Gehrig 22 (47). Measured over simulated
seasons of six eras (1910-1969, hitters with 250+ plate appearances), a power rating (average of the two splits)
gives the league rate times CURVE below, the same in every era.

A hitter's real rate: his home runs a plate appearance in the pack's last two real seasons (the last one counted
whole, the one before half, as the roster builder does), with 4 home runs at the rate his power rating gives added
(as many plate appearances as those take), over the league's rate of those seasons. factor = real / CURVE(power), kept within 0.25-5. Written to
PACK_DIR/hr_factor.txt ("key factor", the player's database key in hex); hitters without a big-league line (made-up
minor leaguers, prospects) get none and play as rated. The runtime multiplies the batter's home-run chance by it
in every simulated plate appearance (hle.cpp hle_pa_probs; the league total stays on target).

Hits on balls in play likewise: the simulation gives a contact rating of 90 only 5-7 % more hits in play than the
league (HIT_CURVE), so batting races were luck (simulated 1910: Donie Bush .340, Cobb .280; 1941: Appling .358,
Williams .300). The hit factor: his real hits in play over balls in play ((H - HR) / (AB - SO - HR + SF); SO where
Lahman has it) with 300 balls in play at the curve's rate added, over the league's, over HIT_CURVE(contact),
within 0.7-1.4. Third column of the file (the runtime multiplies singles, doubles and triples by it).

--hits-only (the modern rosters, mods/rosters-2026, with their 2025-26 lines as Lahman-style CSVs): home-run factors
1 (those ratings already come from Statcast power) and a flat hit curve (a simulated 2026 gave contact 50 1.06 and
contact 90 0.99 times the league's hits in play: batting races were luck, Mickey Gasper .321).
"""
import sys, os, csv, collections, bisect
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import mvpdb

CURVE = [(30, 0.15), (40, 0.25), (45, 0.35), (50, 0.53), (55, 0.72), (60, 0.84), (65, 1.0), (70, 1.25), (75, 1.5),
         (80, 1.6), (85, 1.88), (90, 2.05), (95, 2.25), (100, 2.45)]
PRIOR_HR = 4
HIT_CURVE = [(40, 0.93), (50, 0.95), (60, 0.97), (70, 1.01), (80, 1.03), (90, 1.06), (100, 1.08)]
PRIOR_BIP = 300

def curve(pw, C=None):
    C = C or CURVE
    xs = [x for x, _ in C]
    if pw <= xs[0]: return C[0][1]
    if pw >= xs[-1]: return C[-1][1]
    i = bisect.bisect_right(xs, pw) - 1
    (x0, y0), (x1, y1) = C[i], C[i + 1]
    return y0 + (y1 - y0) * (pw - x0) / (x1 - x0)

def main():
    pack, lahman = sys.argv[1], sys.argv[2]; dry = '--dry' in sys.argv; hits_only = '--hits-only' in sys.argv
    season = int(open(os.path.join(pack, 'year.txt')).read().split()[0])
    seasons = {season - 1: 1.0, season - 2: 0.5}
    people = {}
    for r in csv.DictReader(open(os.path.join(lahman, 'People.csv'), encoding='latin-1')):
        if r['birthYear']: people.setdefault(((r['nameFirst'] or '').strip().lower(), (r['nameLast'] or '').strip().lower()), []).append((int(r['birthYear']), r['playerID']))
    bat = collections.defaultdict(lambda: [0.0, 0.0, 0.0, 0.0]); lg = [0.0, 0.0, 0.0, 0.0]   # hr, pa, hits in play, balls in play
    for r in csv.DictReader(open(os.path.join(lahman, 'Batting.csv'))):
        y = int(r['yearID']); w = seasons.get(y)
        if not w or r['lgID'] not in ('AL', 'NL', 'FL'): continue
        hr, pa = int(r['HR'] or 0), sum(int(r[c] or 0) for c in ('AB', 'BB', 'HBP', 'SH', 'SF'))
        hip, bip = int(r['H'] or 0) - hr, int(r['AB'] or 0) - int(r['SO'] or 0) - hr + int(r['SF'] or 0)
        for i, x in enumerate((hr, pa, hip, bip)): bat[r['playerID']][i] += w * x; lg[i] += w * x
    lg_rate = lg[0] / lg[1]; lg_hip = lg[2] / max(lg[3], 1)
    path = os.path.join(pack, 'DATA', 'DATABASE', 'DATABASE.BIG')
    T, _ = mvpdb.read_database(path)
    at = T['attrib.dat']; A = at.by_key(); pc = at.col('playerattrib_primary_position'); bc = at.col('playerattrib_birthday')
    L = T['lhattrib.dat'].by_key(); R = T['rhattrib.dat'].by_key(); pw_c = T['rhattrib.dat'].col('lrattrib_power'); ct_c = T['rhattrib.dat'].col('lrattrib_contact')
    out, shown, hits = [], [], []
    for k, v in A.items():
        if v[pc] in ('0', '10') or k not in L or k not in R: continue
        by = season - 57 + int(int(v[bc]) / 365.25)            # (birthdays count from the pack's season - 57)
        cands = [pid for y, pid in people.get((v[0].strip().lower(), v[1].strip().lower()), []) if abs(y - by) <= 1 and pid in bat]
        if len(cands) != 1: continue
        hr, pa, hip, bip = bat[cands[0]]
        if pa < 50: continue
        pw = (int(L[k][pw_c]) + int(R[k][pw_c])) / 2; c = curve(pw)
        real = (hr + PRIOR_HR) / (pa + PRIOR_HR / (lg_rate * c)) / lg_rate   # (small counts: toward what his rating gives)
        f = 1.0 if hits_only else max(0.25, min(5.0, real / c))
        ct = (int(L[k][ct_c]) + int(R[k][ct_c])) / 2; hc = 1.0 if hits_only else curve(ct, HIT_CURVE)
        hreal = (hip + PRIOR_BIP * lg_hip * hc) / (bip + PRIOR_BIP) / lg_hip
        hf = max(0.7, min(1.4, hreal / hc))
        if abs(f - 1) < 0.05 and abs(hf - 1) < 0.02: continue
        out.append('%08x %.3f %.3f' % (int(k, 16), f, hf)); shown.append((f, '%s %s' % (v[0], v[1]), round(pw), int(hr), int(pa)))
        hits.append((hf, '%s %s' % (v[0], v[1]), round(ct)))
    shown.sort(reverse=True)
    print('%s: season %d, league %.4f HR/PA, %d factors; highest %s; lowest %s' % (pack, season, lg_rate, len(out),
          ', '.join('%s %.2f (power %d, %d HR / %d PA)' % (n, f, p, h, a) for f, n, p, h, a in shown[:5]),
          ', '.join('%s %.2f' % (n, f) for f, n, p, h, a in shown[-3:])))
    hits.sort(reverse=True)
    print('  hit factors: highest %s; lowest %s' % (', '.join('%s %.2f (contact %d)' % (n, f, c) for f, n, c in hits[:5]), ', '.join('%s %.2f' % (n, f) for f, n, c in hits[-3:])))
    if not dry:
        with open(os.path.join(pack, 'hr_factor.txt'), 'w') as fo:
            fo.write('# player key (hex), home-run factor, hit factor (tools/eras/hr_factor.py)\n' + '\n'.join(out) + '\n')

if __name__ == '__main__':
    main()
