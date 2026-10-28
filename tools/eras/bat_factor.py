"""Strikeout and walk factors for a pack's hitters, so the contact hitters and the free swingers stand as far from the
league as they did.

    python3 tools/eras/bat_factor.py PACK_DIR LAHMAN_DIR SIM_DUMP [--current] [--dry]

The simulation squeezes hitters' strikeout rates together: a simulated 1977 gave its regulars 9.2-14.4 % (10th-90th
percentile; real 6.7-17.7 %), Dave Cash 11 % (real 1.8), Kingman 16 % (26.5); a simulated 2026 gave Luis Arraez 16 %
(real 3.7) and Jacob Wilson .227 with 107 strikeouts. As tools/eras/pitch_factor.py does for pitchers: in the dump
(a simulated season of the pack), each hitter's strikeout and walk rates over the league's, fitted (least squares,
hitters with 200+ plate appearances) on contact, power, plate discipline; his real rates (over the real regulars',
200+ plate appearances) (last two seasons, the last
whole, 200 plate appearances at the fitted rate of prior) over that, within 0.2-3. A hitter of the dump: his own
simulated rates (300 plate appearances at the fitted rate added) instead of the fit. PACK_DIR/bat_factor.txt
("key strikeouts walks"); the runtime multiplies the strikeouts and walks of his plate appearances by them, with the
pitcher's (hle.cpp hle_pa_probs; league totals stay on target). The modern rosters: their 2025-26 lines as
Lahman-style CSVs (see hr_factor.py --hits-only).
"""
import sys, os, csv, collections
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import mvpdb, simstats

PRIOR_PA = 200

def lstsq(X, y):
    """normal equations, tiny ridge"""
    n = len(X[0]); A = [[sum(r[i] * r[j] for r in X) + (1e-6 if i == j else 0) for j in range(n)] for i in range(n)]
    b = [sum(r[i] * t for r, t in zip(X, y)) for i in range(n)]
    for i in range(n):                                                    # Gauss-Jordan
        p = max(range(i, n), key=lambda r: abs(A[r][i])); A[i], A[p] = A[p], A[i]; b[i], b[p] = b[p], b[i]
        for r in range(n):
            if r != i and A[i][i]:
                f = A[r][i] / A[i][i]; A[r] = [a - f * c for a, c in zip(A[r], A[i])]; b[r] -= f * b[i]
    return [b[i] / A[i][i] if A[i][i] else 0 for i in range(n)]

def main():
    pack, lahman, dump = sys.argv[1], sys.argv[2], sys.argv[3]; dry = '--dry' in sys.argv
    season = int(open(os.path.join(pack, 'year.txt')).read().split()[0])
    seasons = {season - 1: 1.0, season - 2: 0.5}
    T, _ = mvpdb.read_database(os.path.join(pack, 'DATA', 'DATABASE', 'DATABASE.BIG'))
    at = T['attrib.dat']; A = at.by_key(); bc = at.col('playerattrib_birthday'); pc = at.col('playerattrib_primary_position')
    pd = at.col('playerattrib_platediscipline')
    L = T['lhattrib.dat'].by_key(); R = T['rhattrib.dat'].by_key(); cc = T['rhattrib.dat'].col('lrattrib_contact'); pw = T['rhattrib.dat'].col('lrattrib_power')
    I = lambda s: int(s) if s.lstrip('-').isdigit() else 0
    hitters = {k: v for k, v in A.items() if v[pc] not in ('0', '10') and k in L and k in R}
    def feats(k):
        return [1.0, (I(L[k][cc]) + I(R[k][cc])) / 200, (I(L[k][pw]) + I(R[k][pw])) / 200, I(A[k][pd]) / 100]
    simstats.SLOTS = simstats.CURRENT if '--current' in sys.argv else simstats.LAST
    t = simstats.load(dump)
    obs = []
    for k in hitters:
        kk = '%08x' % int(k, 16); l = t['lhbstats'].get(kk, {}); r = t['rhbstats'].get(kk, {})
        g = lambda x: l.get(x, 0) + r.get(x, 0)
        if g('tpa') >= 200: obs.append((k, feats(k), g('so'), g('bb') + g('ibb'), g('tpa')))
    lk = sum(o[2] for o in obs) / sum(o[4] for o in obs); lb = sum(o[3] for o in obs) / sum(o[4] for o in obs)
    ck = lstsq([o[1] for o in obs], [o[2] / o[4] / lk for o in obs]); cb = lstsq([o[1] for o in obs], [o[3] / o[4] / lb for o in obs])
    fit = lambda c, f: max(0.4, min(2.0, sum(a * b for a, b in zip(c, f))))
    seen = {o[0]: o for o in obs}
    people = {}
    for r in csv.DictReader(open(os.path.join(lahman, 'People.csv'), encoding='latin-1')):
        if r['birthYear']: people.setdefault(((r['nameFirst'] or '').strip().lower(), (r['nameLast'] or '').strip().lower()), []).append((int(r['birthYear']), r['playerID']))
    bat = collections.defaultdict(lambda: [0.0, 0.0, 0.0]); lg = [0.0, 0.0, 0.0]          # so, bb, pa
    for r in csv.DictReader(open(os.path.join(lahman, 'Batting.csv'))):
        y = int(r['yearID']); w = seasons.get(y)
        if not w or r['lgID'] not in ('AL', 'NL', 'FL'): continue
        if not r['SO']: continue                          # (early seasons without strikeouts)
        pa = sum(int(r[c] or 0) for c in ('AB', 'BB', 'HBP', 'SH', 'SF'))
        for i, x in enumerate((int(r['SO'] or 0), int(r['BB'] or 0), pa)): bat[r['playerID']][i] += w * x; lg[i] += w * x
    reg = [v for v in bat.values() if v[2] >= 200]                 # regulars, as the simulated rates are (no pitchers batting)
    rk, rb = sum(v[0] for v in reg) / sum(v[2] for v in reg), sum(v[1] for v in reg) / sum(v[2] for v in reg)
    out, shown = [], []
    for k, a in hitters.items():
        by = season - 57 + int(int(a[bc]) / 365.25)
        cands = [pid for y, pid in people.get((a[0].strip().lower(), a[1].strip().lower()), []) if abs(y - by) <= 1 and pid in bat]
        if len(cands) != 1: continue
        so, bb, pa = bat[cands[0]]
        if pa < 50: continue
        f = feats(k); ek, eb = fit(ck, f), fit(cb, f)
        if k in seen:                                   # what the simulation gave him, toward the fit (300 PA)
            o = seen[k]; ek = (o[2] + 300 * lk * ek) / (o[4] + 300) / lk; eb = (o[3] + 300 * lb * eb) / (o[4] + 300) / lb
        fk = max(0.2, min(3.0, (so + PRIOR_PA * rk * ek) / (pa + PRIOR_PA) / rk / ek))
        fb = max(0.2, min(3.0, (bb + PRIOR_PA * rb * eb) / (pa + PRIOR_PA) / rb / eb))
        if abs(fk - 1) < 0.03 and abs(fb - 1) < 0.03: continue
        out.append('%08x %.3f %.3f' % (int(k, 16), fk, fb)); shown.append((fk, fb, '%s %s' % (a[0], a[1]), int(so), int(pa)))
    shown.sort()
    print('%s: season %d, simulated K %.3f BB %.3f (%d hitters), real K %.3f BB %.3f; %d factors' % (pack, season, lk, lb, len(obs), rk, rb, len(out)))
    print('  fit K %s  BB %s' % (' '.join('%.2f' % c for c in ck), ' '.join('%.2f' % c for c in cb)))
    print('  fewest strikeouts: %s' % ', '.join('%s %.2f (%d K / %d PA)' % (n, k_, s_, p_) for k_, w_, n, s_, p_ in shown[:5]))
    print('  most: %s' % ', '.join('%s %.2f' % (n, k_) for k_, w_, n, s_, p_ in shown[-4:]))
    print('  walks: %s' % ', '.join('%s %.2f' % (n, w_) for k_, w_, n, s_, p_ in sorted(shown, key=lambda x: -x[1])[:4]))
    if not dry:
        with open(os.path.join(pack, 'bat_factor.txt'), 'w') as fo:
            fo.write('# hitter key (hex), strikeout factor, walk factor (tools/eras/bat_factor.py)\n' + '\n'.join(out) + '\n')

if __name__ == '__main__':
    main()
