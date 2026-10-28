"""Strikeout and walk factors for an era pack's pitchers, so its aces stand as far above the league as they did.

    python3 tools/eras/pitch_factor.py PACK_DIR LAHMAN_DIR SIM_DUMP [--current] [--dry]

SIM_DUMP: a database dump of a simulated season of the pack (runtime "DUMPDB file" in the autopress .once file; at
the draft, or --current for the season in progress), with no pitch_factor.txt in the pack while it ran.

The ratings are quantile-mapped onto the 2005 game's (tools/build_rosters.py), and the simulation spread strikeouts
little: its regulars' strikeout rates run 0.73-1.23 times the league's (10th-90th percentile), against real aces at
1.8-2.5 (simulated 1910: top 161 strikeouts; Walter Johnson struck out 313, Walsh 258). What the simulation gives a
pitcher depends on fastball control above all, then velocity and movement, differently by era (its sim.txt
fastball_base), so it is measured: in the dump, each pitcher's strikeout and walk rates over the league's, fitted
(least squares, pitchers with 150+ batters faced) on his fastball velocity, fastball control, best movement and
average control of his other pitches (within 0.5-1.8; a pitcher of the dump: his own simulated rates, 300
batters faced at the fitted rate added). His real rates: strikeouts and walks a batter faced in the pack's last two
real seasons (the last whole, the one before half), with 400 batters faced at the fitted rate added, over the
league's. factor = real / fitted, within 0.4-2.5. Written to PACK_DIR/pitch_factor.txt ("key strikeouts walks");
the runtime multiplies the strikeouts and walks of the plate appearances he pitches by them (hle.cpp hle_pa_probs;
league totals stay on target).
"""
import sys, os, csv, collections
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import mvpdb, simstats

PRIOR_BF = 400

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
    at = T['attrib.dat']; A = at.by_key(); bc = at.col('playerattrib_birthday')
    P = T['pitcher.dat']; PK = P.by_key(); pc = P.col
    I = lambda s: int(s) if s.lstrip('-').isdigit() else 0
    def feats(v):
        mv = [I(v[pc('pitchattrib_pitch%d_movement' % i)]) for i in range(2, 6)]
        ctl = [I(v[pc('pitchattrib_pitch%d_control' % i)]) for i in range(2, 6) if I(v[pc('pitchattrib_pitch%d_control' % i)]) > 0]
        return [1.0, I(v[pc('pitchattrib_fastball_velocity')]) / 100, I(v[pc('pitchattrib_fastball_control')]) / 100, max(mv) / 10,
                (sum(ctl) / len(ctl) if ctl else 50) / 100]
    simstats.SLOTS = simstats.CURRENT if '--current' in sys.argv else simstats.LAST
    t = simstats.load(dump)
    obs = []
    for k, v in PK.items():
        kk = '%08x' % int(k, 16); l = t['lhpstats'].get(kk, {}); r = t['rhpstats'].get(kk, {})
        bf = l.get('tpa', 0) + r.get('tpa', 0)
        if bf >= 150: obs.append((k, feats(v), l.get('so', 0) + r.get('so', 0), l.get('bb', 0) + r.get('bb', 0), bf))
    lk = sum(o[2] for o in obs) / sum(o[4] for o in obs); lb = sum(o[3] for o in obs) / sum(o[4] for o in obs)
    ck = lstsq([o[1] for o in obs], [o[2] / o[4] / lk for o in obs]); cb = lstsq([o[1] for o in obs], [o[3] / o[4] / lb for o in obs])
    fit = lambda c, f: max(0.5, min(1.8, sum(a * b for a, b in zip(c, f))))
    seen = {o[0]: o for o in obs}
    people = {}
    for r in csv.DictReader(open(os.path.join(lahman, 'People.csv'), encoding='latin-1')):
        if r['birthYear']: people.setdefault(((r['nameFirst'] or '').strip().lower(), (r['nameLast'] or '').strip().lower()), []).append((int(r['birthYear']), r['playerID']))
    pit = collections.defaultdict(lambda: [0.0, 0.0, 0.0]); lg = [0.0, 0.0, 0.0]          # so, bb, bf
    for r in csv.DictReader(open(os.path.join(lahman, 'Pitching.csv'))):
        y = int(r['yearID']); w = seasons.get(y)
        if not w or r['lgID'] not in ('AL', 'NL', 'FL'): continue
        bf = int(r['BFP'] or 0) or int(r['IPouts'] or 0) + int(r['H'] or 0) + int(r['BB'] or 0)
        for i, x in enumerate((int(r['SO'] or 0), int(r['BB'] or 0), bf)): pit[r['playerID']][i] += w * x; lg[i] += w * x
    rk, rb = lg[0] / lg[2], lg[1] / lg[2]
    out, shown = [], []
    for k, v in PK.items():
        a = A.get(k)
        if not a: continue
        by = season - 57 + int(int(a[bc]) / 365.25)
        cands = [pid for y, pid in people.get((a[0].strip().lower(), a[1].strip().lower()), []) if abs(y - by) <= 1 and pid in pit]
        if len(cands) != 1: continue
        so, bb, bf = pit[cands[0]]
        if bf < 50: continue
        f = feats(v); ek, eb = fit(ck, f), fit(cb, f)
        if k in seen:                                   # what the simulation gave him, toward the fit (300 batters)
            o = seen[k]; ek = (o[2] + 300 * lk * ek) / (o[4] + 300) / lk; eb = (o[3] + 300 * lb * eb) / (o[4] + 300) / lb
        fk = max(0.4, min(2.5, (so + PRIOR_BF * rk * ek) / (bf + PRIOR_BF) / rk / ek))
        fb = max(0.4, min(2.5, (bb + PRIOR_BF * rb * eb) / (bf + PRIOR_BF) / rb / eb))
        if abs(fk - 1) < 0.03 and abs(fb - 1) < 0.03: continue
        out.append('%08x %.3f %.3f' % (int(k, 16), fk, fb)); shown.append((fk, fb, '%s %s' % (a[0], a[1]), int(so), int(bf)))
    shown.sort(reverse=True)
    print('%s: season %d, simulated league K %.3f BB %.3f (%d pitchers), real K %.3f BB %.3f; %d factors' % (pack, season, lk, lb, len(obs), rk, rb, len(out)))
    print('  fit K %s  BB %s' % (' '.join('%.2f' % c for c in ck), ' '.join('%.2f' % c for c in cb)))
    print('  strikeouts: %s' % ', '.join('%s %.2f (%d K / %d BF)' % (n, k_, s_, b_) for k_, w_, n, s_, b_ in shown[:6]))
    print('  walks: %s' % ', '.join('%s %.2f' % (n, w_) for k_, w_, n, s_, b_ in sorted(shown, key=lambda x: -x[1])[:5]))
    if not dry:
        with open(os.path.join(pack, 'pitch_factor.txt'), 'w') as fo:
            fo.write('# pitcher key (hex), strikeout factor, walk factor (tools/eras/pitch_factor.py)\n' + '\n'.join(out) + '\n')

if __name__ == '__main__':
    main()
