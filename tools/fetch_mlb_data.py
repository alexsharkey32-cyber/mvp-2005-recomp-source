"""Downloads current MLB rosters and stats for tools/build_rosters.py.

Usage: python3 tools/fetch_mlb_data.py [season] [out dir]      (defaults: this year, game/rosters)
Sources: MLB Stats API (statsapi.mlb.com) and Baseball Savant (baseballsavant.mlb.com) public endpoints.
Writes mvp_statsapi_<season>.txt, mvp_splits_<season>.txt, mvp_savant_<season>.txt, mvp_teams_<season>.txt,
mvp_armangle_<season>.txt and mvp_armangle_<season-1>.txt
(the builder reads the *_2026 names; pass the season you fetched to it as well if it differs).
Takes a few minutes: ~1500 small requests.
"""
import sys, os, json, csv, io, time, datetime, urllib.request, concurrent.futures

SEASON = int(sys.argv[1]) if len(sys.argv) > 1 else datetime.date.today().year
OUT = sys.argv[2] if len(sys.argv) > 2 else 'game/rosters'
os.makedirs(OUT, exist_ok=True)
API = 'https://statsapi.mlb.com'
SAV = 'https://baseballsavant.mlb.com'
UA = {'User-Agent': 'mvp2005-roster-builder/1.0'}

def get(url, tries=4):
    for i in range(tries):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r: return r.read().decode('utf-8')
        except Exception as e:
            if i == tries - 1: raise
            time.sleep(2 * (i + 1))
def jget(path): return json.loads(get(API + path))

HF = ['gamesPlayed', 'plateAppearances', 'atBats', 'hits', 'doubles', 'triples', 'homeRuns', 'rbi', 'runs', 'baseOnBalls', 'intentionalWalks',
      'strikeOuts', 'hitByPitch', 'stolenBases', 'caughtStealing', 'sacBunts', 'sacFlies', 'groundOuts', 'airOuts']
PF = ['gamesPlayed', 'gamesStarted', 'outs', 'hits', 'runs', 'earnedRuns', 'homeRuns', 'baseOnBalls', 'intentionalWalks', 'strikeOuts', 'hitByPitch',
      'wins', 'losses', 'saves', 'blownSaves', 'completeGames', 'shutouts', 'battersFaced', 'groundOuts', 'airOuts', 'pickoffs', 'doubles',
      'triples', 'sacFlies', 'sacBunts', 'holds']
FF = ['gamesPlayed', 'gamesStarted', 'innings', 'putOuts', 'assists', 'errors', 'chances', 'doublePlays', 'caughtStealing', 'stolenBases', 'passedBall']
def row(st, fields): return ','.join('' if st.get(f) is None else str(st.get(f)) for f in fields)

print('teams...')
teams = jget('/api/v1/teams?sportIds=1,11,12,13,14&season=%d' % SEASON)['teams']
with open(os.path.join(OUT, 'mvp_teams_%d.txt' % SEASON), 'w', encoding='utf-8') as f:
    for t in teams:
        f.write('|'.join(str(x) for x in [t['id'], t['sport']['id'], t.get('parentOrgId', t['id']), t.get('abbreviation', ''), t.get('locationName', ''),
                                          t.get('teamName', ''), t.get('name', ''), t.get('league', {}).get('name', ''), t.get('division', {}).get('name', ''),
                                          t.get('venue', {}).get('name', '')]) + '\n')

print('rosters...')
people, R = {}, []
def roster(t, typ):
    j = jget('/api/v1/teams/%d/roster?rosterType=%s&season=%d&hydrate=person' % (t['id'], typ, SEASON))
    org = t['id'] if t['sport']['id'] == 1 else t.get('parentOrgId', 0)
    return [(t['id'], t['sport']['id'], org, typ, e) for e in j.get('roster', [])]
jobs = [(t, typ) for t in teams if t['sport']['id'] in (1, 11, 12, 13) for typ in (['40Man', 'active'] if t['sport']['id'] == 1 else ['active'])]
with concurrent.futures.ThreadPoolExecutor(6) as ex:
    for res in ex.map(lambda a: roster(*a), jobs):
        for tid, sport, org, typ, e in res:
            p = e['person']; people[p['id']] = p
            R.append([tid, sport, org, typ, p['id'], e.get('jerseyNumber', ''), e['position']['abbreviation'], e.get('status', {}).get('code', '')])

print('season stats...')
lines = []
for g in ('hitting', 'pitching', 'fielding'):
    for season in (SEASON - 1, SEASON):
        for sp in (1, 11, 12, 13, 14):
            off = 0
            while True:
                j = jget('/api/v1/stats?stats=season&group=%s&season=%d&sportId=%d&playerPool=ALL&limit=1000&offset=%d' % (g, season, sp, off))
                splits = (j.get('stats') or [{}])[0].get('splits', [])
                for s in splits:
                    tm = s.get('team', {}).get('id', '')
                    if g == 'hitting': lines.append('|'.join(map(str, ['H', season, s['player']['id'], sp, tm, row(s['stat'], HF)])))
                    elif g == 'pitching': lines.append('|'.join(map(str, ['Q', season, s['player']['id'], sp, tm, row(s['stat'], PF)])))
                    else: lines.append('|'.join(map(str, ['F', season, s['player']['id'], sp, tm, s.get('position', {}).get('abbreviation', ''), row(s['stat'], FF)])))
                if len(splits) < 1000: break
                off += 1000

print('career stats...')
mlb_ids = sorted({r[4] for r in R if r[1] == 1})
def career(batch):
    j = jget('/api/v1/people?personIds=%s&hydrate=stats(group=[hitting,pitching],type=[career],sportId=1)' % ','.join(map(str, batch)))
    out = []
    for p in j.get('people', []):
        for st in p.get('stats', []):
            sp = st.get('splits') or []
            if not sp: continue
            if st['group']['displayName'] == 'hitting': out.append('CH|%d|%s' % (p['id'], row(sp[0]['stat'], HF)))
            else: out.append('CP|%d|%s' % (p['id'], row(sp[0]['stat'], PF)))
    return out
with concurrent.futures.ThreadPoolExecutor(4) as ex:
    for res in ex.map(career, [mlb_ids[i:i + 40] for i in range(0, len(mlb_ids), 40)]): lines += res

with open(os.path.join(OUT, 'mvp_statsapi_%d.txt' % SEASON), 'w', encoding='utf-8') as f:
    f.write('#PEOPLE id|first|last|fullName|num|pos|bat|throw|height|weight|birth|debut|country\n')
    for p in people.values():
        f.write('|'.join(str(x) for x in ['P', p['id'], p.get('useName') or p.get('firstName', ''), p.get('useLastName') or p.get('lastName', ''),
                                          p.get('fullName', ''), p.get('primaryNumber', ''), p.get('primaryPosition', {}).get('abbreviation', ''),
                                          p.get('batSide', {}).get('code', ''), p.get('pitchHand', {}).get('code', ''), p.get('height', ''),
                                          p.get('weight', ''), p.get('birthDate', ''), p.get('mlbDebutDate', ''), p.get('birthCountry', '')]) + '\n')
    f.write('#ROSTER team|sport|org|type|id|num|pos|status\n')
    for r in R: f.write('R|' + '|'.join(map(str, r)) + '\n')
    f.write('\n'.join(lines) + '\n')

print('platoon splits...')
with open(os.path.join(OUT, 'mvp_splits_%d.txt' % SEASON), 'w', encoding='utf-8') as f:
    for g in ('hitting', 'pitching'):
        for season in (SEASON - 1, SEASON):
            for code in ('vl', 'vr'):
                off = 0
                while True:
                    j = jget('/api/v1/stats?stats=statSplits&sitCodes=%s&group=%s&season=%d&sportId=1&playerPool=ALL&limit=1000&offset=%d' % (code, g, season, off))
                    splits = (j.get('stats') or [{}])[0].get('splits', [])
                    for s in splits:
                        f.write('|'.join(map(str, ['SH' if g == 'hitting' else 'SP', season, s['player']['id'], code, row(s['stat'], HF if g == 'hitting' else PF)])) + '\n')
                    if len(splits) < 1000: break
                    off += 1000

print('Statcast...')
def scsv(path): return list(csv.DictReader(io.StringIO(get(SAV + path).lstrip('﻿'))))
def r1(x):
    try: v = round(float(x), 1)
    except (ValueError, TypeError): return (x or '').replace(',', ' ')
    return str(int(v)) if v == int(v) else str(v)
def pick(name, rows, idk, cols):
    return name + '\t' + ','.join(cols) + '\n' + '\n'.join(','.join([str(r.get(idk, ''))] + [r1(r.get(c)) for c in cols]) for r in rows)
secs = []
secs.append(pick('sprint', scsv('/leaderboard/sprint_speed?year=%d&position=&team=&min=0&csv=true' % SEASON), 'player_id', ['sprint_speed']))
secs.append(pick('sprint25', scsv('/leaderboard/sprint_speed?year=%d&position=&team=&min=0&csv=true' % (SEASON - 1)), 'player_id', ['sprint_speed']))
PT = ['ff', 'si', 'fc', 'sl', 'ch', 'cu', 'fs', 'kn', 'st', 'sv']
for tag, yr in (('', SEASON), ('25', SEASON - 1)):
    sp = scsv('/leaderboard/pitch-arsenals?year=%d&min=1&type=avg_speed&hand=&csv=true' % yr)
    us = {r['pitcher']: r for r in scsv('/leaderboard/pitch-arsenals?year=%d&min=1&type=n_&hand=&csv=true' % yr)}
    body = []
    for r in sp:
        u = us.get(r['pitcher'], {})
        cells = []
        for p in PT:
            s, n = r.get(p + '_avg_speed'), u.get('n_' + p)
            cells.append('%d/%d' % (round(float(s)), round(float(n))) if s and n else '')
        body.append(r['pitcher'] + ',' + ','.join(cells))
    secs.append('ars%s\t%s (speed/usage)\n%s' % (tag, ','.join(PT), '\n'.join(body)))
secs.append(pick('oaa', scsv('/leaderboard/outs_above_average?type=Fielder&startYear=%d&endYear=%d&split=no&team=&range=year&min=1&pos=&roles=&viz=hide&csv=true' % (SEASON, SEASON)),
                 'player_id', ['primary_pos_formatted', 'outs_above_average']))
for tag, yr in (('', SEASON), ('25', SEASON - 1)):
    rows = scsv('/leaderboard/batted-ball?year=%d&type=batter&min=1&csv=true' % yr)
    secs.append('bb%s\tbbe,gb,fb,ld,pu,pull,straight,oppo\n' % tag + '\n'.join(
        ','.join([r['id'], r.get('bbe', '')] + [str(round(float(r[c]) * 100)) if r.get(c) not in (None, '') else '' for c in
                  ('gb_rate', 'fb_rate', 'ld_rate', 'pu_rate', 'pull_rate', 'straight_rate', 'oppo_rate')]) for r in rows))
sel = 'pa,k_percent,bb_percent,xba,xslg,xwoba,barrel_batted_rate,hard_hit_percent,exit_velocity_avg,oz_swing_percent,z_swing_percent,whiff_percent,sprint_speed'
for tag, yr in (('', SEASON), ('25', SEASON - 1)):
    secs.append(pick('cb' + tag, scsv('/leaderboard/custom?year=%d&type=batter&filter=&min=1&selections=%s&chart=false&csv=true' % (yr, sel)), 'player_id',
                     ['pa', 'k_percent', 'bb_percent', 'xba', 'xslg', 'barrel_batted_rate', 'hard_hit_percent', 'oz_swing_percent', 'whiff_percent']))
psel = 'pa,p_formatted_ip,k_percent,bb_percent,xwoba,whiff_percent,oz_swing_percent,groundballs_percent,flyballs_percent,linedrives_percent'
for tag, yr in (('', SEASON), ('25', SEASON - 1)):
    secs.append(pick('cp' + tag, scsv('/leaderboard/custom?year=%d&type=pitcher&filter=&min=1&selections=%s&chart=false&csv=true' % (yr, psel)), 'player_id',
                     ['pa', 'k_percent', 'bb_percent', 'xwoba', 'whiff_percent', 'oz_swing_percent', 'groundballs_percent', 'flyballs_percent', 'linedrives_percent']))
secs.append(pick('arm', scsv('/leaderboard/arm-strength?type=player&year=%d&minThrows=1&pos=&team=&csv=true' % SEASON), 'player_id',
                 ['primary_position', 'arm_overall', 'max_arm_strength']))
open(os.path.join(OUT, 'mvp_savant_%d.txt' % SEASON), 'w', encoding='utf-8').write('\n\n#####\n'.join(secs))
# arm angles (degrees above horizontal at release; negative = submarine): one file per season, "id,hand,pitches,angle"
for yr in (SEASON, SEASON - 1):
    rows = scsv('/leaderboard/pitcher-arm-angles?batSide=&dateStart=&dateEnd=&gameType=R&groupBy=&min=10&minGroupPitches=1'
                '&perspective=back&pitchHand=&pitchType=&season=%d&size=small&sort=ascending&team=&csv=true' % yr)
    with open(os.path.join(OUT, 'mvp_armangle_%d.txt' % yr), 'w', encoding='utf-8') as f:
        for r in rows:
            if r.get('pitcher') and r.get('ball_angle') not in (None, ''):
                f.write('%s,%s,%s,%s\n' % (r['pitcher'], r.get('pitch_hand', ''), r.get('n_pitches', '0'), r['ball_angle']))
print('done: %d players, %d roster entries -> %s' % (len(people), len(R), OUT))
