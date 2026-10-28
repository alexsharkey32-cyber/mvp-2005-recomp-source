"""Checks a league pack before anyone plays it: the things that stop a dynasty or show the wrong thing.

    python3 tools/eras/check_pack.py PACK_DIR [PACK_DIR ...] [--iso ISO]

Rosters (the game's own rules, FEENG.LOC 4388-4391 and the trade / draft limits): every club of every level with
its full roster (the disc: 25 in the majors and at AAA, 22 at AA and A), at least 5 starting pitchers, a reliever,
10 fielders, a catcher, 6-15 pitchers on a big-league club; no organisation above 100 players; nobody on two clubs.
Files: year.txt, pack.json, the commentary copy of the database in step with the database (only the audio ids may
differ), draft classes for the next ten seasons, logos / uniforms / parks overlays readable.
Prints one line per problem and a summary; exit status 1 when there are problems.
"""
import sys, os, collections, json
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import mvpdb

SIZE = {0: 25, 1: 25, 2: 22, 3: 22}
LEVEL = ('MLB', 'AAA', 'AA', 'A')

def check(pack):
    probs, info = [], []
    p = lambda s: probs.append(s)
    for f in ('year.txt', 'pack.json', os.path.join('DATA', 'DATABASE', 'DATABASE.BIG')):
        if not os.path.exists(os.path.join(pack, f)): p('missing %s' % f)
    if probs: return probs, info
    season = int(open(os.path.join(pack, 'year.txt')).read().split()[0])
    meta = json.load(open(os.path.join(pack, 'pack.json')))
    T, order = mvpdb.read_database(os.path.join(pack, 'DATA', 'DATABASE', 'DATABASE.BIG'))
    at = T['attrib.dat']; A = at.by_key(); pc = at.col('playerattrib_primary_position')
    tm = T['team.dat']; tname = {k: '%s %s' % (v[tm.col('team_location')], v[tm.col('team_long_name')]) for k, v in tm.rows}
    org = T['org.dat']; lvl, org_of = {}, {}
    for k, v in org.rows:
        if v[org.col('org_is_franchise')] != '1': continue             # (the all-star clubs)
        for i, c in enumerate(('org_team_mlb', 'org_team_aaa', 'org_team_aa', 'org_team_a')):
            t = '%09x' % (int(v[org.col(c)]) & 0xfffffffff); lvl[t] = i; org_of[t] = k
    pos = {k: i for i, (k, v) in enumerate(tm.rows)}
    seq = [pos.get(v[0], len(pos)) for k, v in T['roster.dat'].rows if len(v) > 1]
    if seq != sorted(seq): p('roster.dat not in team.dat order: the game would put clubs in the wrong divisions (any mvpdb.write_database fixes it)')
    clubs = collections.defaultdict(list); seen = collections.Counter()
    for k, v in T['roster.dat'].rows:
        if len(v) < 2: continue
        clubs[v[0]].append(v[1])
        if v[0] in lvl: seen[v[1]] += 1                              # (the all-star clubs share their players)
    dup = [k for k, n in seen.items() if n > 1]
    if dup: p('%d players on two clubs (e.g. %s %s)' % (len(dup), *A.get(dup[0], ['?', '?'])[:2]))
    missing = [k for k in seen if k not in A]
    if missing: p('%d roster entries without a player' % len(missing))
    per_org = collections.Counter()
    for t, ks in clubs.items():
        if t not in lvl: continue
        l = lvl[t]; per_org[org_of[t]] += len(ks)
        pos = [int(A[k][pc]) for k in ks if k in A]
        sp = sum(1 for x in pos if x == 0); rp = sum(1 for x in pos if x == 10); P = sp + rp
        F = len(pos) - P; C = sum(1 for x in pos if x == 1)
        nm = '%s (%s)' % (tname.get(t, t), LEVEL[l])
        if len(ks) != SIZE[l]: p('%s: %d players (the game wants %d)' % (nm, len(ks), SIZE[l]))
        if sp < 5: p('%s: %d starting pitchers (minimum 5)' % (nm, sp))
        if rp < 1: p('%s: no relief pitcher (a closer is required)' % nm)
        if F < 10: p('%s: %d fielders (minimum 10)' % (nm, F))
        if C < 1: p('%s: no catcher' % nm)
        gone = [n for q, n in ((2, '1B'), (3, '2B'), (4, '3B'), (5, 'SS'), (6, 'LF'), (7, 'CF'), (8, 'RF')) if q not in pos]
        if gone: p('%s: nobody at %s' % (nm, ', '.join(gone)))
        if l == 0 and not 6 <= P <= 15: p('%s: %d pitchers (6-15 on a big-league club)' % (nm, P))
    for o, n in per_org.items():
        if n > 100: p('organisation %s: %d players (maximum 100)' % (o, n))
    info.append('org sizes %d-%d' % (min(per_org.values()), max(per_org.values())))
    # the commentary copy of the database
    cp = os.path.join(pack, 'commentary', 'DATA', 'DATABASE', 'DATABASE.BIG')
    if os.path.exists(cp):
        C_, _ = mvpdb.read_database(cp); bad = collections.Counter()
        for n in T:
            if n not in C_: bad[n + ' (table missing)'] += 1; continue
            if len({k for k, v in T[n].rows}) < len(T[n].rows):              # (roster.dat: no key of its own)
                if sorted(map(tuple, (v for k, v in T[n].rows))) != sorted(map(tuple, (v for k, v in C_[n].rows))): bad[n] += 1
                continue
            rc = C_[n].by_key(); audio = mvpdb.AUDIO_COLS.get(n)
            ai = T[n].cols.index(audio) if audio in T[n].cols else -1
            for k, v in T[n].rows:
                w = rc.get(k)
                if w is None: bad[n + ' rows'] += 1; continue
                if any(x != y for i, (x, y) in enumerate(zip(v, w)) if i != ai): bad[n] += 1
        if bad: p('commentary copy of the database out of step: %s (python3 -c "import mvpdb" ... refresh_commentary)' % dict(bad))
    # draft classes
    if meta.get('era'):
        d = os.path.join(pack, 'draft'); have = sorted(int(f[:4]) for f in os.listdir(d) if f[:4].isdigit()) if os.path.isdir(d) else []
        if not have: p('no draft classes')
        else: info.append('draft classes %d-%d' % (have[0], have[-1]))
    for sub in ('art/DATA/FRONTEND/LOGOS.BIG', 'art/DATA/MODELS.BIG', 'DATA/MODELS.BIG', 'DATA/STADIUM'):
        dd = os.path.join(pack, sub)
        if os.path.isdir(dd):
            empty = [f for f in os.listdir(dd) if os.path.isfile(os.path.join(dd, f)) and os.path.getsize(os.path.join(dd, f)) == 0]
            if empty: p('%s: empty files %s' % (sub, empty[:3]))
    info.append('%d players, %d clubs, season %d' % (len(A), len(clubs), season))
    if os.path.exists(os.path.join(pack, 'faces.txt')): info.append('faces')
    if os.path.exists(os.path.join(pack, 'portraits', 'portraits.txt')): info.append('portraits')
    return probs, info

def main():
    packs = [a for i, a in enumerate(sys.argv[1:], 1) if not a.startswith('--') and sys.argv[i - 1] != '--iso']
    bad = 0
    for pk in packs:
        probs, info = check(pk)
        print('%s: %s; %s' % (pk, 'OK' if not probs else '%d problems' % len(probs), ', '.join(info)))
        for x in probs: print('  ' + x)
        bad += len(probs)
    sys.exit(1 if bad else 0)

if __name__ == '__main__': main()
