"""How a simulated season looks: league rates and leaders from a live database dump.

    (in a running game)  echo "DUMPDB /path/db.txt" > RUN.rules.once
    python3 tools/simstats.py db.txt [DATABASE.BIG for names] [--min-pa 300] [--current]   (--current: the season in progress)

The dump (runtime: hle.cpp db_dump_request) lists every table slot of the game's database with each player's
values, in the order of the attribute names in the executable. Compared with the real major leagues of 2025.
"""
import sys, os, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# database slot -> (table, attribute names in the game's order). Two sets: the season being played (until its end)
# and the last finished one; at the end of a regular season the current lines move into the "last" slots (and the
# current ones then count the postseason). Checked: batting and pitching totals agree outcome by outcome.
LAST = {
    388: ('attrib', 'audioid birthday height weight jerseynum primary_position secondary_position photo bats throws ditty hidden bunting speed '
                    'throwstrength throwaccuracy fielding range durability platediscipline stealing_aggressive baserunning starpower topprospect '
                    'salary contract_length offercount personality playedtoday energylevel injurytype injuryduration'.split()),
    400: ('bstats', 'g sb cs r'.split()),
    404: ('lhbstats', 'hr rbi 1b 2b 3b so bb ibb sf sh hbp tpa'.split()),
    408: ('rhbstats', 'hr rbi 1b 2b 3b so bb ibb sf sh hbp tpa'.split()),
    460: ('pstats', 'g ip pip w l r er sv blsv gs cg sho pk wp'.split()),
    464: ('lhpstats', 'bb ibb so hbp 1b 2b 3b hr sf sh tpa'.split()),
    468: ('rhpstats', 'bb ibb so hbp 1b 2b 3b hr sf sh tpa'.split()),
}
CURRENT = {388: LAST[388], 412: LAST[400], 416: LAST[404], 420: LAST[408], 472: LAST[460], 476: LAST[464], 480: LAST[468]}
SLOTS = LAST
MLB2025 = dict(avg=.245, obp=.315, slg=.404, k=.222, bb=.084, hr=.030, babip=.291, sb_g=0.75, era=4.15)
REAL = {}   # --year Y: compare with that season (Lahman, tools/eras/league_rates.py)

def load(path):
    t = collections.defaultdict(dict)
    for ln in open(path):
        if ln.startswith('#'): continue
        p = ln.split()
        slot = int(p[0])
        if slot not in SLOTS: continue
        name, cols = SLOTS[slot]
        t[name][p[1]] = dict(zip(cols, map(int, p[2:2 + len(cols)])))
    return t

def names_from(dbpath):
    import mvpdb
    T, _ = mvpdb.read_database(dbpath)
    return {k[-8:]: '%s %s' % (v[0], v[1]) for k, v in T['attrib.dat'].rows}

def main():
    global SLOTS
    if '--current' in sys.argv: SLOTS = CURRENT
    a = [x for x in sys.argv[1:] if not x.startswith('--') and not x.isdigit()]
    t = load(a[0]); nm = names_from(a[1]) if len(a) > 1 else {}
    min_pa = int(sys.argv[sys.argv.index('--min-pa') + 1]) if '--min-pa' in sys.argv else 300
    bat = {}
    for k in t['bstats']:
        L, R = t['lhbstats'].get(k), t['rhbstats'].get(k)
        if not L or not R: continue
        s = {c: L[c] + R[c] for c in L}
        s.update(t['bstats'][k]); bat[k] = s
    tot = collections.Counter()
    for s in bat.values():
        if s['tpa'] > 0: tot.update(s)
    def rates(s):
        s = dict(s); s['bb'] = s['bb'] + s.get('ibb', 0)        # walks with the intentional ones, as Lahman counts them
        h = s['1b'] + s['2b'] + s['3b'] + s['hr']; ab = s['tpa'] - s['bb'] - s['hbp'] - s['sf'] - s['sh']
        if ab <= 0: return None
        tb = s['1b'] + 2 * s['2b'] + 3 * s['3b'] + 4 * s['hr']
        bip = ab - s['so'] - s['hr'] + s['sf']
        return dict(avg=h / ab, obp=(h + s['bb'] + s['hbp']) / max(1, ab + s['bb'] + s['hbp'] + s['sf']), slg=tb / ab,
                    k=s['so'] / s['tpa'], bb=s['bb'] / s['tpa'], hr=s['hr'] / s['tpa'], babip=(h - s['hr']) / max(1, bip), pa=s['tpa'], hrs=s['hr'])
    lg = rates(tot)
    ref, lab = MLB2025, 'MLB 2025'
    if '--year' in sys.argv:                                  # the real season from tools/eras/league_rates.txt
        y = sys.argv[sys.argv.index('--year') + 1]
        for ln in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'eras', 'league_rates.txt')):
            p = ln.split()
            if p and p[0] == y:
                k, bb, hbp, b1, b2, b3, hr = map(float, p[1:])
                h = b1 + b2 + b3 + hr; ab = 1 - bb - hbp - 0.012          # sacrifices about 1.2%
                ref = dict(avg=h / ab, obp=h + bb + hbp, slg=(b1 + 2 * b2 + 3 * b3 + 4 * hr) / ab, k=k, bb=bb, hr=hr, babip=(h - hr) / (ab - k - hr + 0.006))
                lab = 'MLB ' + y
    print('all levels, %d plate appearances' % tot['tpa'])
    for k_ in ('avg', 'obp', 'slg', 'k', 'bb', 'hr', 'babip'):
        print('  %-6s %.3f   (%s %.3f)' % (k_, lg[k_], lab, ref[k_]))
    rows = [(k, rates(s)) for k, s in bat.items() if s['tpa'] >= min_pa]
    rows = [(k, r) for k, r in rows if r]
    print('\n%d hitters with %d+ PA' % (len(rows), min_pa))
    for stat, rev in (('avg', True), ('hrs', True), ('k', True), ('bb', True)):
        top = sorted(rows, key=lambda kr: kr[1][stat], reverse=rev)[:5]
        print('  %-5s ' % stat + ', '.join('%s %.3f' % (nm.get(k, k), r[stat]) if stat != 'hrs' else '%s %d' % (nm.get(k, k), r[stat]) for k, r in top))
    import statistics
    for stat in ('avg', 'obp', 'slg', 'k', 'bb'):
        v = [r[stat] for _, r in rows]
        if len(v) > 3: print('  spread %-5s p10 %.3f  median %.3f  p90 %.3f' % (stat, sorted(v)[len(v) // 10], statistics.median(v), sorted(v)[len(v) * 9 // 10]))
    # pitchers
    pit = []
    for k, p in t['pstats'].items():
        ip = p['ip'] + p['pip'] / 3.0
        if ip < 1: continue
        L, R = t['lhpstats'].get(k, {}), t['rhpstats'].get(k, {})
        so = L.get('so', 0) + R.get('so', 0); bb = L.get('bb', 0) + R.get('bb', 0); bf = L.get('tpa', 0) + R.get('tpa', 0)
        pit.append((k, ip, p['er'] * 9 / ip, so / max(1, bf), bb / max(1, bf), p))
    big = [x for x in pit if x[1] >= 120]
    print('\n%d pitchers with 120+ IP' % len(big))
    for lab, idx, rev in (('ERA', 2, False), ('K%', 3, True), ('IP', 1, True)):
        top = sorted(big, key=lambda x: x[idx], reverse=rev)[:5]
        print('  %-4s ' % lab + ', '.join('%s %.2f' % (nm.get(x[0], x[0]), x[idx]) for x in top))

if __name__ == '__main__': main()
