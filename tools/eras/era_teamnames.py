"""Club names through the years, for an era pack's dynasty -> PACK_DIR/team_years.txt.

    python3 tools/eras/era_teamnames.py LAHMAN_DIR PACK_DIR [PACK_DIR ...]

A dynasty that starts in 1951 should see the Braves move to Milwaukee in 1953, the Browns become the Orioles in
1954 and the Dodgers and Giants go west in 1958. For every slot of the game (art id 1-30) and every season this
lists the club's city, nickname and abbreviation: from the Lahman Teams table for the seasons its franchise played,
from tools/eras/era_names.py's minor-league stand-ins before. The runtime renames a club in the dynasty's database
when the season changes and its entry for the new season differs from the old one's.
Lines: "art first last abbr|city|nickname" (+ "|S" for a minor-league stand-in).
"""
import sys, os, csv
here = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, here)
from era_names import STAND_IN

ART = {'ANA': 1, 'OAK': 2, 'SEA': 3, 'TEX': 4, 'CHW': 5, 'CLE': 6, 'DET': 7, 'KCR': 8, 'MIN': 9, 'BAL': 10, 'BOS': 11,
       'NYY': 12, 'TBD': 13, 'TOR': 14, 'ARI': 15, 'COL': 16, 'LAD': 17, 'SDP': 18, 'SFG': 19, 'CHC': 20, 'CIN': 21,
       'HOU': 22, 'MIL': 23, 'PIT': 24, 'STL': 25, 'ATL': 26, 'FLA': 27, 'WSN': 28, 'NYM': 29, 'PHI': 30}
TWO_WORD = {'Red Sox', 'White Sox', 'Blue Jays', 'Devil Rays', 'Red Stockings', 'White Stockings', 'Blue Stockings',
            'Black Sox', 'Red Legs', "Colt .45's"}

def split(nm):                                   # as tools/eras/lahman_convert.py names the clubs of a pack
    city, _, nick = nm.rpartition(' ')
    w = nm.split()
    if len(w) > 2 and ' '.join(w[-2:]) in TWO_WORD: city, nick = ' '.join(w[:-2]), ' '.join(w[-2:])
    if ' of ' in nm: city, _, nick = nm.split(' of ')[0].rpartition(' ')
    if nm == 'Athletics': return 'Sacramento', 'Athletics'      # 2025: no city in the name
    return city or nm, nick

def table(lahman):
    with open(os.path.join(lahman, 'Teams.csv'), newline='', encoding='utf-8-sig', errors='replace') as f:
        teams = [t for t in csv.DictReader(f) if t['yearID'].isdigit() and t['lgID'] in ('AL', 'NL')]
    by = {}
    for t in teams:
        a = ART.get(t['franchID'])
        if a: by[(a, int(t['yearID']))] = (t.get('teamIDBR') or t['teamID'],) + split(t['name'])
    last = max(y for _, y in by)
    out = {}
    for a in range(1, 31):
        first, rows = STAND_IN.get(a, (0, []))
        for y in range(1901, last + 1):
            v = by.get((a, y))
            if not v:
                for lo, hi, city, name, ab in rows:
                    if lo <= y <= hi: v = (ab, city, name, 'S')      # a stand-in: the pack's plain uniforms and logo
            if v: out[(a, y)] = v
        for y in range(last + 1, 2126):             # later seasons: as the last one
            if (a, last) in out: out[(a, y)] = out[(a, last)]
    return out

def write(out, pack):
    lines = ['# art first last abbr|city|nickname (tools/eras/era_teamnames.py)']
    for a in range(1, 31):
        ys = sorted(y for b, y in out if b == a); i = 0
        while i < len(ys):
            j = i
            while j + 1 < len(ys) and ys[j + 1] == ys[j] + 1 and out[(a, ys[j + 1])] == out[(a, ys[i])]: j += 1
            lines.append('%d %d %d %s' % (a, ys[i], ys[j], '|'.join(out[(a, ys[i])])))
            i = j + 1
    open(os.path.join(pack, 'team_years.txt'), 'w', encoding='utf-8').write('\n'.join(lines) + '\n')

if __name__ == '__main__':
    t = table(sys.argv[1])
    for p in sys.argv[2:]: write(t, p); print(p)
