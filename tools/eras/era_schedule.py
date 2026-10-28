"""A 154-game season for the eras that played one (the majors before 1961) -> PACK/DATA/DATABASE/SCHEDULE.BIG.

    python3 tools/eras/era_schedule.py ISO PACK_DIR [GAMES = 154]

The game has only 162-game schedules (DATA/DATABASE/SCHEDULE.BIG: mlb162_1..3, one picked by the season). For each,
games are taken out until every club has GAMES: as many home as away games removed per club, spread over the whole
season but not the last four days, so every club ends on the same day as the league (a randomized matching,
seeded, tried again until exact). The minor-league files are left as they are.
The pack's members replace the disc's (an overlay directory of the archive's members); sim.txt gets season_games
(the runtime's games-per-season for the majors, hle_season_games).
"""
import sys, os, random, collections
here = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, os.path.join(here, '..'))
import isofile, refpack
from bigfile import read_big

def trim(text, games, seed):
    lines = text.split('\r\n')
    head, rows = [], []
    for i, ln in enumerate(lines):
        p = ln.split(',')
        if len(p) >= 4 and '/' in p[0]: rows.append((i, p[2].strip(), p[3].strip(), p[0]))
    n = collections.Counter()
    for _, h, a, d in rows: n[h] += 1; n[a] += 1
    def day(d): m, dd, y = map(int, d.split('/')); return (y, m, dd)
    last = sorted({day(r[3]) for r in rows})[-4:]          # every club still plays the last days: the game ends a club's
    keep = {k for k, r in enumerate(rows) if day(r[3]) in last}   # season after its last game and the league's on the last day
    full = max(n.values()); drop = full - games
    if drop <= 0: return text, 0
    clubs = [c for c in n if n[c] == full]
    for t in range(2000):
        rnd = random.Random(seed * 7919 + t)
        order = [k for k in range(len(rows)) if k not in keep]; rnd.shuffle(order)
        home = collections.Counter(); away = collections.Counter(); gone = set()
        for k in order:
            _, h, a, _d = rows[k]
            if h in clubs and a in clubs and home[h] < drop // 2 and away[a] < drop - drop // 2 and home[h] + away[h] < drop and home[a] + away[a] < drop:
                home[h] += 1; away[a] += 1; gone.add(k)
        if all(home[c] + away[c] == drop for c in clubs): break
    else:
        raise SystemExit('no exact %d-game schedule found' % games)
    kill = {rows[k][0] for k in gone}
    return '\r\n'.join(ln for i, ln in enumerate(lines) if i not in kill), len(kill)

def main():
    iso, pack = sys.argv[1], sys.argv[2]; games = int(sys.argv[3]) if len(sys.argv) > 3 else 154
    big = isofile.read_file(iso, 'DATA/DATABASE/SCHEDULE.BIG')
    out = os.path.join(pack, 'DATA', 'DATABASE', 'SCHEDULE.BIG'); os.makedirs(out, exist_ok=True)
    for name, o, s in read_big(big):
        if not name.startswith('mlb162'): continue
        raw = big[o:o + s]; comp = raw[1:2] == b'\xfb'
        text = (refpack.decompress(raw) if comp else raw).decode('latin1')
        new, k = trim(text, games, sum(map(ord, name)))
        data = new.encode('latin1')
        open(os.path.join(out, name), 'wb').write(refpack.compress(data) if comp else data)
        print('%s: %d games out -> %d a club' % (name, k, games))
    sp = os.path.join(pack, 'sim.txt')                                              # the standings count games left from it
    lines = [l for l in (open(sp).read().splitlines() if os.path.exists(sp) else []) if not l.startswith('season_games')]
    open(sp, 'w').write('\n'.join(lines + ['season_games %d' % games]) + '\n')

if __name__ == '__main__': main()
