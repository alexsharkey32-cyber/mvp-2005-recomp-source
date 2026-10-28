"""Clubs that did not exist yet in an era pack's season get the name of a real minor-league club of their city then.

    python3 tools/eras/era_names.py PACK_DIR

A 1961 start has 30 clubs although the majors had 18: the slots of franchises still to come (Seattle, Kansas City,
Houston, the Mets, Montreal, San Diego, Denver ...) are expansion clubs of made-up players (lahman_convert.py).
Rather than the Denver Rockies of 1961, each is named for a well-known minor-league club of that city in those
years (the Pacific Coast League's Seattle Rainiers and San Diego Padres, the Houston Buffaloes, the Montreal
Royals, the Toronto Maple Leafs, the Denver Bears ...), or of another city when the slot's own had a big-league
club then. Only team.dat's city, name and abbreviation change; tools/eras/era_logos.py then gives the club
plain lettering in its colours. A dynasty keeps the names as the seasons go by.
"""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import mvpdb

# art id (team_artid) -> first big-league season of the franchise, [(first season, last season, city, name, abbr)]
STAND_IN = {
    1:  (1961, [(1903, 1957, 'Los Angeles', 'Angels', 'LAA'), (1958, 1960, 'Hollywood', 'Stars', 'HOL')]),
    3:  (1977, [(1901, 1918, 'Seattle', 'Giants', 'SEA'), (1919, 1937, 'Seattle', 'Indians', 'SEA'), (1938, 1964, 'Seattle', 'Rainiers', 'SEA'),
                (1965, 1968, 'Seattle', 'Angels', 'SEA'), (1969, 1969, 'Tacoma', 'Cubs', 'TAC'), (1970, 1976, 'Seattle', 'Rainiers', 'SEA')]),
    4:  (1961, [(1901, 1960, 'Fort Worth', 'Cats', 'FTW')]),
    8:  (1969, [(1901, 1954, 'Kansas City', 'Blues', 'KCB'), (1955, 1968, 'Omaha', 'Cardinals', 'OMA')]),
    13: (1998, [(1901, 1997, 'St. Petersburg', 'Saints', 'STP')]),
    14: (1977, [(1901, 1967, 'Toronto', 'Maple Leafs', 'TOR'), (1968, 1976, 'Rochester', 'Red Wings', 'ROC')]),
    15: (1998, [(1901, 1957, 'Phoenix', 'Senators', 'PHX'), (1958, 1985, 'Phoenix', 'Giants', 'PHX'), (1986, 1997, 'Phoenix', 'Firebirds', 'PHX')]),
    16: (1993, [(1901, 1984, 'Denver', 'Bears', 'DEN'), (1985, 1992, 'Denver', 'Zephyrs', 'DEN')]),
    18: (1969, [(1901, 1935, 'Oakland', 'Oaks', 'OAK'), (1936, 1968, 'San Diego', 'Padres', 'SD')]),
    22: (1962, [(1901, 1961, 'Houston', 'Buffaloes', 'HOU')]),
    23: (1969, [(1901, 1952, 'Milwaukee', 'Brewers', 'MIL'), (1953, 1960, 'Minneapolis', 'Millers', 'MPL'), (1961, 1968, 'Indianapolis', 'Indians', 'IND')]),
    27: (1993, [(1901, 1955, 'Miami', 'Sun Sox', 'MIA'), (1956, 1970, 'Miami', 'Marlins', 'MIA'), (1971, 1988, 'Miami', 'Orioles', 'MIA'),
                (1989, 1992, 'Miami', 'Miracle', 'MIA')]),
    28: (1969, [(1901, 1960, 'Montreal', 'Royals', 'MON'), (1961, 1968, 'Buffalo', 'Bisons', 'BUF')]),
    29: (1962, [(1901, 1949, 'Newark', 'Bears', 'NEW'), (1950, 1961, 'Louisville', 'Colonels', 'LOU')]),
}

def pick(art, season):
    first, rows = STAND_IN.get(art, (0, []))
    if season >= first: return None
    for lo, hi, city, name, ab in rows:
        if lo <= season <= hi: return city, name, ab
    return rows[-1][2:] if rows else None

def main():
    pack = sys.argv[1]
    season = int(open(os.path.join(pack, 'year.txt')).read().split()[0])
    path = os.path.join(pack, 'DATA', 'DATABASE', 'DATABASE.BIG')
    T, order = mvpdb.read_database(path); t = T['team.dat']; log = []
    for k, v in t.rows:
        art = int(v[t.col('team_artid')]); p = pick(art, season)
        if not p: continue
        old = '%s %s' % (v[t.col('team_location')], v[t.col('team_long_name')])
        v[t.col('team_location')], v[t.col('team_long_name')], v[t.col('team_short_name')] = p
        log.append('club %2d: %s %s (was %s)' % (art - 1, p[0], p[1], old))
    mvpdb.write_database(path, T, order)
    with open(os.path.join(pack, 'standins.txt'), 'w') as f:    # (era_logos.py: lettering for these even when the name is the 2005 one)
        f.write('# clubs named for minor-league clubs (tools/eras/era_names.py): club index\n' + ''.join('%d\n' % int(l.split()[1].rstrip(':')) for l in log))
    sp = os.path.join(pack, 'rosters_summary.txt')            # (the summary's headings follow)
    if os.path.exists(sp) and log:
        s = open(sp, encoding='utf-8').read()
        for l in log:
            new, old = l.split(': ', 1)[1].split(' (was ')
            s = s.replace('== %s (' % old.rstrip(')'), '== %s (' % new)
        open(sp, 'w', encoding='utf-8').write(s)
    print('%d clubs not founded yet in %d named for minor-league clubs of the day' % (len(log), season)); print('\n'.join(log))

if __name__ == '__main__': main()
