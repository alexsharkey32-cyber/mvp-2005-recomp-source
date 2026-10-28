"""A few lines about the season an era pack starts in, for the Eras menu (pack.json "blurb").

    python3 tools/eras/era_blurb.py LAHMAN_DIR PACK_DIR

From the Lahman database: the champions of the season before, the clubs that are new, last season's home-run
and wins leaders, and the best careers in the pack's coming draft classes (PACK_DIR/draft, ranked by
tools/eras/draft_potential.py).
"""
import csv, os, sys, json, collections

def rows(lahman, f): return list(csv.DictReader(open(os.path.join(lahman, f), encoding='utf-8-sig')))

def blurb(lahman, pack):
    first = int(open(os.path.join(pack, 'year.txt')).read().split()[0]); last = first - 1
    MAJOR = ('AL', 'NL', 'AA', 'UA', 'PL', 'FL')                       # the leagues an era pack holds (not the Negro leagues)
    teams = [t for t in rows(lahman, 'Teams.csv') if t['lgID'] in MAJOR]
    tname = {(t['yearID'], t['teamID']): t['name'] for t in teams}
    out = []
    ws = [r for r in rows(lahman, 'SeriesPost.csv') if r['yearID'] == str(last) and r['round'] == 'WS']
    if ws: out.append('The %s are the champions of %d.' % (tname.get((str(last), ws[0]['teamIDwinner']), ws[0]['teamIDwinner']), last))
    fr_first = collections.defaultdict(lambda: 9999)
    for t in teams: fr_first[t['franchID']] = min(fr_first[t['franchID']], int(t['yearID']))
    new = [t['name'] for t in teams if t['yearID'] == str(first) and fr_first[t['franchID']] == first]
    if new: out.append('New this year: %s.' % ' and '.join(new) if len(new) <= 2 else 'New this year: %s.' % ', '.join(new))
    ppl = {p['playerID']: '%s %s' % (p['nameFirst'], p['nameLast']) for p in rows(lahman, 'People.csv')}
    bat = collections.Counter(); win = collections.Counter()
    for r in rows(lahman, 'Batting.csv'):
        if r['yearID'] == str(last) and r['lgID'] in MAJOR: bat[r['playerID']] += int(r['HR'] or 0)
    for r in rows(lahman, 'Pitching.csv'):
        if r['yearID'] == str(last) and r['lgID'] in MAJOR: win[r['playerID']] += int(r['W'] or 0)
    if bat and win:
        (b, hr), (p, w) = bat.most_common(1)[0], win.most_common(1)[0]
        out.append('Last year %s hit %d home runs and %s won %d.' % (ppl.get(b, b), hr, ppl.get(p, p), w))
    d = os.path.join(pack, 'draft')
    if os.path.isdir(d):                                   # the best careers among the classes to come
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import draft_potential
        pids, val = draft_potential.career(lahman)
        best = []
        for fn in sorted(os.listdir(d)):
            if not fn[:4].isdigit() or int(fn[:4]) > first + 10: continue        # the next ten classes
            for x in csv.DictReader(open(os.path.join(d, fn), encoding='latin1')):
                k = (x['first'].lower(), x['last'].lower(), x.get('birthday', ''))
                pid = pids.get(k) or pids.get((k[0], k[1], k[2][:4]))
                if pid: best.append((val.get(pid, 0), x['first'] + ' ' + x['last'], fn[:4]))
        best.sort(reverse=True)
        if best: out.append('In the drafts of the next ten years: %s.' % ', '.join('%s (%s)' % (n, y) for _, n, y in best[:4]))
    return ' '.join(out)

if __name__ == '__main__':
    lahman, pack = sys.argv[1], sys.argv[2]
    b = blurb(lahman, pack)
    pj = os.path.join(pack, 'pack.json'); meta = json.load(open(pj)) if os.path.exists(pj) else {}
    meta['blurb'] = b; meta['era'] = True
    json.dump(meta, open(pj, 'w'), indent=2)
    print(b)
