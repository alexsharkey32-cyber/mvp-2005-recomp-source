"""Draft classes ranked by what the players went on to do, and given the game's star level to match.

    python3 tools/eras/draft_potential.py LAHMAN_DIR PACK_DIR

A class (PACK_DIR/draft/<year>.csv) is rated from each player's first big-league seasons, which says little
about who became a star: Gary Redus and Tom Candiotti rate like the made-up rookies around them. Here every player
is found in the Lahman database (name and birth date) and ranked by his career: plate appearances for hitters,
outs pitched (x 1.2) for pitchers. The rows are sorted best first, and the rookie generator's star level
(starpower: how far a rookie develops; topprospect: the scouts' flag) follows the rank: top 10% 4 and a top
prospect, next 15% 3, next 25% 2, the rest 1. The made-up rookies the game adds to a class get 0 (runtime:
hle_rookie), so the real future big-leaguers lead the draft board and grow into their careers.
"""
import csv, os, sys, collections

def career(lahman):
    ppl = {}
    for r in csv.DictReader(open(os.path.join(lahman, 'People.csv'), encoding='utf-8-sig')):
        try: bd = '%04d-%02d-%02d' % (int(r['birthYear']), int(r['birthMonth']), int(r['birthDay']))
        except ValueError: bd = ''
        ppl[(r['nameFirst'].lower(), r['nameLast'].lower(), bd)] = r['playerID']
        ppl.setdefault((r['nameFirst'].lower(), r['nameLast'].lower(), bd[:4]), r['playerID'])
    val = collections.Counter()
    for r in csv.DictReader(open(os.path.join(lahman, 'Batting.csv'), encoding='utf-8-sig')):
        val[r['playerID']] += int(r['AB'] or 0) + int(r['BB'] or 0)
    for r in csv.DictReader(open(os.path.join(lahman, 'Pitching.csv'), encoding='utf-8-sig')):
        val[r['playerID']] += int(1.2 * int(r['IPouts'] or 0))
    return ppl, val

def main():
    lahman, pack = sys.argv[1], sys.argv[2]
    ppl, val = career(lahman)
    d = os.path.join(pack, 'draft'); found = total = 0
    for fn in sorted(os.listdir(d)):
        if not fn.endswith('.csv'): continue
        p = os.path.join(d, fn)
        rows = list(csv.DictReader(open(p, encoding='latin1'))); cols = list(rows[0].keys()) if rows else []
        for r in rows:
            k = (r['first'].lower(), r['last'].lower(), r.get('birthday', ''))
            pid = ppl.get(k) or ppl.get((k[0], k[1], k[2][:4]))
            r['_v'] = val.get(pid, 0) if pid else 0; found += bool(pid); total += 1
        rows.sort(key=lambda r: -r['_v'])
        n = len(rows)
        for i, r in enumerate(rows):
            q = i / max(1, n)
            r['starpower'] = 4 if q < 0.10 else 3 if q < 0.25 else 2 if q < 0.50 else 1
            r['topprospect'] = 1 if q < 0.10 else 0
            del r['_v']
        with open(p, 'w', newline='', encoding='latin1') as f:
            w = csv.DictWriter(f, cols, restval=''); w.writeheader(); w.writerows(rows)
    print('%s: %d of %d draftees found in Lahman, classes ranked by career' % (pack, found, total))

if __name__ == '__main__': main()
