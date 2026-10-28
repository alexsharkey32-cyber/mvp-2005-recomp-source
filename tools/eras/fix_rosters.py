"""Makes every club of a league pack meet the game's roster rules, so a dynasty never stops on "the ... do not have
the minimum of 5 starting pitchers and a closer" / "10 fielders" / "N players" (tools/eras/check_pack.py).

    python3 tools/eras/fix_rosters.py PACK_DIR [--dry]

Within each organisation (its MLB, AAA, AA and A clubs; nobody joins or leaves it):
  1. sizes: players move from clubs above their size (25 / 25 / 22 / 22) to clubs below it;
  2. a catcher, 10 fielders, 6 pitchers for every club: a bench player is swapped with one of another club;
  3. 5 starting pitchers and a reliever: relievers with the most stamina become starters (or the other way);
  4. the depth charts: in each of the four lineups (vs RHP / LHP, AL / NL rules) SP1-SP5 are starters, CP a
     reliever and C a catcher, by swapping that lineup's roles between two players of the club.
Only bench players (B, LR, MR, SU in every lineup) are moved between clubs. An organisation short of its 94 players
gets made-up ones (copies of a bench player of the same kind with another name, no statistics, another player's
looks), as the game's own rookie generator does. Writes the database (and so its
commentary copy, tools/mvpdb.py).
"""
import sys, os, collections, random
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import mvpdb

SIZE = (25, 25, 22, 22)
BENCH = {'B', 'LR', 'MR', 'SU'}

def fix(pack, dry=False):
    path = os.path.join(pack, 'DATA', 'DATABASE', 'DATABASE.BIG')
    T, order = mvpdb.read_database(path)
    at = T['attrib.dat']; A = at.by_key(); pc = at.col('playerattrib_primary_position'); sc = at.col('playerattrib_secondary_position')
    pt = T['pitcher.dat']; stam = {k: int(v[pt.col('pitchattrib_stamina')]) for k, v in pt.rows}
    pos = lambda k: int(A[k][pc])
    isP = lambda k: pos(k) in (0, 10)
    rows_by_team = collections.defaultdict(list)
    for k, v in T['roster.dat'].rows:
        if len(v) >= 10: rows_by_team[v[0]].append(v)
    org = T['org.dat']; log = []
    rng = random.Random(len(A))
    names = [(v[0], v[1]) for k, v in at.rows]
    LOOKS = [c for c in at.cols if c.split('_', 1)[-1] in ('face', 'face2004style', 'skin_tone', 'hairstyle', 'haircolour', 'facialhair', 'bodytype', 'height', 'weight', 'boneprofile')]
    STATS = ('bstats.dat', 'fbstats.dat', 'lhbstats.dat', 'rhbstats.dat', 'lhpstats.dat', 'rhpstats.dat', 'pstats.dat', 'career.dat', 'careerp.dat')
    tables_of = {n: t.by_key() for n, t in T.items() if n.endswith('.dat') and t.cols[:2] == ['first_name', 'last_name']}
    def new_player(donor, team, role):
        while True:
            key = '0%08x' % rng.getrandbits(32)
            if key not in A: break
        first, last = rng.choice(names)[0], rng.choice(names)[1]
        looks = A[rng.choice(names and list(A))]
        for n, d in tables_of.items():
            if donor not in d: continue
            v = list(d[donor]); v[0], v[1] = first, last
            if n in STATS: v[2:] = ['0'] * (len(v) - 2)
            if n == 'attrib.dat':
                for c in LOOKS: v[at.col(c)] = looks[at.col(c)]
                v[at.col('playerattrib_audioid')] = '0'; v[at.col('playerattrib_jerseynum')] = str(rng.randint(1, 99))
            T[n].rows.append((key, v)); d[key] = v
        A[key] = tables_of['attrib.dat'][key]
        if donor in stam: stam[key] = stam[donor]
        r = [team, key] + [role, '-1'] * 4
        T['roster.dat'].rows.append(('000000000', r)); return r
    hexid = lambda x: '%09x' % (int(x) & 0xfffffffff)
    for ok, ov in org.rows:
        if ov[org.col('org_is_franchise')] != '1': continue
        teams = [hexid(ov[org.col(c)]) for c in ('org_team_mlb', 'org_team_aaa', 'org_team_aa', 'org_team_a')]
        M = [[r for r in rows_by_team[t] if r[1] in A] for t in teams]
        def counts(l):
            ks = [r[1] for r in M[l]]
            sp = sum(pos(k) == 0 for k in ks); rp = sum(pos(k) == 10 for k in ks)
            return dict(n=len(ks), sp=sp, rp=rp, P=sp + rp, F=len(ks) - sp - rp, C=sum(pos(k) == 1 for k in ks))
        def ok_without(l, k):                                   # the club still meets the rules without k
            c = counts(l); p = pos(k)
            if p == 1 and c['C'] <= 1: return False
            if p in (0, 10):
                if c['P'] <= (6 if l else 11): return False
            elif c['F'] <= 10: return False
            return True
        def movable(l, typ):
            out = [r for r in M[l] if set(r[2:10:2]) <= BENCH and isP(r[1]) == (typ == 'P') and ok_without(l, r[1])]
            return sorted(out, key=lambda r: (pos(r[1]) == 1, -stam.get(r[1], 0) if typ == 'P' else 0))
        def move(r, src, dst, why):
            M[src].remove(r); M[dst].append(r); r[0] = teams[dst]
            log.append('%s -> %s: %s %s (%s)' % (('MLB', 'AAA', 'AA', 'A')[src], ('MLB', 'AAA', 'AA', 'A')[dst], A[r[1]][0], A[r[1]][1], why))
        # 0. an organisation short of players: made-up ones where the clubs are short
        for l in range(4):
            while len(M[l]) < SIZE[l] and sum(len(m) for m in M) < sum(SIZE):
                c = counts(l); typ = 'P' if c['P'] < SIZE[l] // 2 or c['sp'] < 5 else 'F'
                pool = [r[1] for m in M for r in m if isP(r[1]) == (typ == 'P') and set(r[2:10:2]) <= BENCH and pos(r[1]) != 1] or \
                       [r[1] for m in M for r in m if isP(r[1]) == (typ == 'P')]
                donor = rng.choice(pool)
                r = new_player(donor, teams[l], 'MR' if typ == 'P' else 'B'); M[l].append(r)
                log.append('new %s %s for %s (%s, like %s %s)' % (A[r[1]][0], A[r[1]][1], ('MLB', 'AAA', 'AA', 'A')[l], typ, A[donor][0], A[donor][1]))
        # 1. sizes
        for _ in range(60):
            over = [l for l in range(4) if len(M[l]) > SIZE[l]]; under = [l for l in range(4) if len(M[l]) < SIZE[l]]
            if not over or not under: break
            u = under[0]; c = counts(u); want_p = SIZE[u] // 2
            typ = 'P' if c['P'] < want_p else 'F'
            done = False
            for o in sorted(over, key=lambda o: abs(o - u)):
                cand = movable(o, typ) or movable(o, 'F' if typ == 'P' else 'P')
                if cand: move(cand[0], o, u, 'size'); done = True; break
            if not done: break
        # 2. catcher / fielders / pitchers by swaps
        for l in range(4):
            for need, test, typ in (('catcher', lambda c: c['C'] < 1, 'C'), ('fielders', lambda c: c['F'] < 10, 'F'),
                                    ('pitchers', lambda c: c['P'] < 6, 'P')):
                for _ in range(10):
                    if not test(counts(l)): break
                    got = False
                    for o in sorted(range(4), key=lambda o: abs(o - l)):
                        if o == l: continue
                        if typ == 'C': cand = [r for r in M[o] if pos(r[1]) == 1 and set(r[2:10:2]) <= BENCH and counts(o)['C'] >= 2]
                        else: cand = movable(o, typ)
                        back = movable(l, 'P' if typ in ('C', 'F') else 'F')
                        if cand and back:
                            move(cand[0], o, l, need); move(back[0], l, o, 'swap for ' + need); got = True; break
                    if not got: break
                if test(counts(l)) and typ == 'C':                # nobody to swap: the fielder who has caught (secondary) or the last one catches
                    fs = [r[1] for r in M[l] if not isP(r[1])]
                    k = next((k for k in fs if int(A[k][sc]) == 1), fs[-1] if fs else None)
                    if k: A[k][pc] = '1'; log.append('%s %s now a catcher' % tuple(A[k][:2]))
        # 3. starters and relievers
        for l in range(4):
            ks = [r[1] for r in M[l]]
            cp = {r[1] for r in M[l] if 'CP' in r[2:10:2]}
            while counts(l)['sp'] < 5 and counts(l)['rp'] > 1:
                k = max((k for k in ks if pos(k) == 10 and k not in cp), key=lambda k: stam.get(k, 0), default=None) or \
                    max((k for k in ks if pos(k) == 10), key=lambda k: stam.get(k, 0))
                A[k][pc] = '0'; log.append('%s %s: reliever -> starter' % tuple(A[k][:2]))
            while counts(l)['rp'] < 1 and counts(l)['sp'] > 5:
                k = min((k for k in ks if pos(k) == 0), key=lambda k: stam.get(k, 0))
                A[k][pc] = '10'; log.append('%s %s: starter -> reliever' % tuple(A[k][:2]))
        # 3b. every position covered (the disc has a player of each position on nearly every club): a bench player
        # of that position from another club of the organisation (for a bench player of a doubled position), else the
        # player of the club who plays it as his second position, else one from the club's most doubled position
        POS = (1, 2, 3, 4, 5, 6, 7, 8)
        for l in range(4):
            for p in POS:
                prim = collections.Counter(pos(r[1]) for r in M[l])
                if prim[p]: continue
                dup = [q for q in POS if prim[q] >= (3 if q == 1 else 2)]          # (a second catcher is kept)
                grp = (6, 7, 8) if p in (6, 7, 8) else (2, 3, 4, 5)
                done = False
                for o in sorted(range(4), key=lambda o: abs(o - l)):
                    if o == l: continue
                    po = collections.Counter(pos(r[1]) for r in M[o])
                    if po[p] < 2: continue
                    give = next((r for r in M[o] if pos(r[1]) == p and set(r[2:10:2]) <= BENCH), None)
                    back = next((r for r in M[l] if pos(r[1]) in dup and set(r[2:10:2]) <= BENCH and po[pos(r[1])] < 2), None) or \
                           next((r for r in M[l] if pos(r[1]) in dup and set(r[2:10:2]) <= BENCH), None)
                    if give and back: move(give, o, l, 'position %d' % p); move(back, l, o, 'swap for position %d' % p); done = True; break
                if done: continue
                cand = [r[1] for r in M[l] if not isP(r[1]) and int(A[r[1]][sc]) == p and prim[pos(r[1])] >= 2]
                if not cand: cand = sorted((r[1] for r in M[l] if not isP(r[1]) and pos(r[1]) in dup), key=lambda k: (pos(k) not in grp, pos(k) == 1))
                if cand:
                    k = cand[0]; old_p = pos(k); A[k][pc] = str(p); A[k][sc] = str(old_p)
                    log.append('%s %s: position %d -> %d' % (A[k][0], A[k][1], old_p, p))
        # 4. depth charts, lineup by lineup (columns 2/3, 4/5, 6/7, 8/9: role, batting order)
        for l in range(4):
            R = M[l]
            for j in (2, 4, 6, 8):
                def swap(a, b): a[j], b[j] = b[j], a[j]; a[j + 1], b[j + 1] = b[j + 1], a[j + 1]
                for role in ('SP1', 'SP2', 'SP3', 'SP4', 'SP5'):
                    h = next((r for r in R if r[j] == role), None)
                    if h is None or pos(h[1]) == 0: continue
                    sub = max((r for r in R if pos(r[1]) == 0 and not r[j].startswith('SP') and r[j] != 'CP'), key=lambda r: stam.get(r[1], 0), default=None)
                    if sub: swap(h, sub)
                h = next((r for r in R if r[j] == 'CP'), None)
                if h is not None and pos(h[1]) != 10:
                    sub = next((r for r in R if pos(r[1]) == 10 and r[j] in BENCH), None)
                    if sub: swap(h, sub)
                h = next((r for r in R if r[j] == 'C'), None)
                if h is not None and pos(h[1]) != 1:
                    sub = next((r for r in R if pos(r[1]) == 1 and r[j] == 'B'), None) or next((r for r in R if pos(r[1]) == 1 and not isP(r[1])), None)
                    if sub: swap(h, sub)
    if not dry: mvpdb.write_database(path, T, order)
    return log

if __name__ == '__main__':
    for pk in [a for a in sys.argv[1:] if not a.startswith('--')]:
        lg = fix(pk, '--dry' in sys.argv)
        print('%s: %d changes' % (pk, len(lg)))
        for x in lg[:400]: print('  ' + x)
