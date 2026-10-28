"""Builds modern MLB rosters for MVP Baseball 2005.

Usage: python3 tools/build_rosters.py <your MVP Baseball 2005 ISO> [data dir] [out dir] [season]
  data dir (default game/rosters): mvp_statsapi_<season>.txt, mvp_splits_..., mvp_savant_..., mvp_teams_...
            written by tools/fetch_mlb_data.py (statsapi.mlb.com / baseballsavant.mlb.com)
  out dir (default mods/rosters-2026): the runtime loads it when Settings > Game > Rosters is Modern
  out dir:  gets DATA/DATABASE/DATABASE.BIG (+ DATA/FEENG.LOC, DATA/IGENG.LOC team names)

Ratings: every rating is derived from the player's real stats (2026, with 2025 at half weight, minor-league
lines translated to MLB level and regressed toward average for small samples) and then *quantile-mapped*
onto the distribution the original 2005 database used for that rating (by position where it matters), so a
player at the 90th percentile today gets the 90th-percentile 2005 rating and the game stays balanced.
Legends / Heroes / EA teams and the Default player are kept as they were.
"""
import sys, os, math, random, bisect, datetime, collections, re
sys.path.insert(0, os.path.dirname(__file__))
import mvpdb, isofile, loc

ISO = sys.argv[1]
DATA = sys.argv[2] if len(sys.argv) > 2 else 'game/rosters'
OUT = sys.argv[3] if len(sys.argv) > 3 else 'mods/rosters-2026'
SEASON = int(sys.argv[4]) if len(sys.argv) > 4 else 2026
TODAY = min(datetime.date.today(), datetime.date(SEASON, 10, 1))
# The year a dynasty with these rosters starts in. MVP 2005 came out before the 2005 season with 2004 as "last
# year"; the same here: once SEASON's regular season is as good as over it is last year, and the first season to
# play is the next one. The runtime reads it from year.txt and moves the game's calendar (which starts in 2005) there.
FIRST = SEASON + 1 if TODAY >= datetime.date(SEASON, 9, 15) else SEASON
YEAR_SHIFT = FIRST - 2005

# ---------------------------------------------------------------- input data
people, rost = {}, []
H = collections.defaultdict(list)     # id -> [(season, sport, [HF])]
Q = collections.defaultdict(list)     # pitching
F = collections.defaultdict(list)     # id -> [(season, sport, pos, [FF])]
SH, SPl = {}, {}                      # (season, id, code) -> list
_H_rows, _Q_rows = collections.defaultdict(list), collections.defaultdict(list)   # id -> [(season, MLB club id, stats)] for HIST.DAT
CH, CP = {}, {}
HF = 'g pa ab h d t hr rbi r bb ibb so hbp sb cs sh sf go ao'.split()
PF = 'g gs outs h r er hr bb ibb so hbp w l sv bs cg sho bf go ao pk d t sf sh hld'.split()
FF = 'g gs inn po a e ch dp cs sb pb'.split()
def nums(s): return [float(x) if x not in ('', '-.--', '-') else 0.0 for x in s.split(',')]
def dct(keys, vals): return dict(zip(keys, vals))
for ln in open(os.path.join(DATA, 'mvp_statsapi_%d.txt' % SEASON), encoding='utf-8'):
    p = ln.rstrip('\n').split('|')
    if p[0] == 'P':
        people[int(p[1])] = dict(id=int(p[1]), first=p[2], last=p[3], full=p[4], num=p[5], pos=p[6], bat=p[7], thr=p[8],
                                 height=p[9], weight=p[10], birth=p[11], debut=p[12], country=p[13])
    elif p[0] == 'R': rost.append(dict(team=int(p[1]), sport=int(p[2]), org=int(p[3]), type=p[4], id=int(p[5]), num=p[6], pos=p[7], status=p[8]))
    elif p[0] == 'H':
        H[int(p[2])].append((int(p[1]), int(p[3]), dict(dct(HF, nums(p[5])), _tm=int(p[4] or 0))))
        if p[3] == '1' and p[4]: _H_rows[int(p[2])].append((int(p[1]), int(p[4]), dct(HF, nums(p[5]))))
    elif p[0] == 'Q':
        Q[int(p[2])].append((int(p[1]), int(p[3]), dict(dct(PF, nums(p[5])), _tm=int(p[4] or 0))))
        if p[3] == '1' and p[4]: _Q_rows[int(p[2])].append((int(p[1]), int(p[4]), dct(PF, nums(p[5]))))
    elif p[0] == 'F': F[int(p[2])].append((int(p[1]), int(p[3]), p[5], dct(FF, nums(p[6]))))
    elif p[0] == 'CH': CH[int(p[1])] = dct(HF, nums(p[2]))
    elif p[0] == 'CP': CP[int(p[1])] = dct(PF, nums(p[2]))
for ln in open(os.path.join(DATA, 'mvp_splits_%d.txt' % SEASON), encoding='utf-8'):
    p = ln.rstrip('\n').split('|')
    if p[0] == 'SH': SH[(int(p[1]), int(p[2]), p[3])] = dct(HF, nums(p[4]))
    elif p[0] == 'SP': SPl[(int(p[1]), int(p[2]), p[3])] = dct(PF, nums(p[4]))
SAV = {}
for sec in open(os.path.join(DATA, 'mvp_savant_%d.txt' % SEASON), encoding='utf-8').read().split('\n\n#####\n'):
    lines = sec.strip().split('\n'); name, cols = lines[0].split('\t'); cols = cols.split(' ')[0].split(',')
    d = {}
    for l in lines[1:]:
        v = l.split(',')
        try: d[int(v[0])] = dict(zip(cols, v[1:]))
        except ValueError: pass
    SAV[name] = d
# Statcast arm angles (optional files): pid -> degrees above horizontal at release, this season before last
ARM = {}
for yr in (SEASON - 1, SEASON):
    fn_ = os.path.join(DATA, 'mvp_armangle_%d.txt' % yr)
    if not os.path.exists(fn_): continue
    acc = collections.defaultdict(lambda: [0, 0.0])
    for ln in open(fn_, encoding='utf-8'):
        v = ln.strip().split(',')
        try: n = int(v[2]); acc[int(v[0])][0] += n; acc[int(v[0])][1] += n * float(v[3])
        except (ValueError, IndexError): pass
    for k_, (n, tot_) in acc.items():
        if n >= 30: ARM[k_] = tot_ / n
def sav(name, pid, col):
    r = SAV.get(name, {}).get(pid)
    if not r: return None
    try: return float(r[col])
    except (ValueError, KeyError, TypeError): return None
teams_api = {}
for ln in open(os.path.join(DATA, 'mvp_teams_%d.txt' % SEASON), encoding='utf-8'):
    p = ln.rstrip('\n').split('|'); teams_api[int(p[0])] = dict(sport=int(p[1]), org=int(p[2]), abbr=p[3], loc=p[4], name=p[5])

def rng(pid, salt=''):
    return random.Random('%s:%s' % (pid, salt))
# An era (tools/eras/lahman_convert.py wrote the data): clubs, names and managers as they were in that season
ERA = os.path.exists(os.path.join(DATA, 'era.txt'))
ERA_MGR = {}
if ERA and os.path.exists(os.path.join(DATA, 'era_managers.txt')):
    for ln in open(os.path.join(DATA, 'era_managers.txt'), encoding='utf-8'):
        p_ = ln.rstrip('\n').split('|')
        if len(p_) >= 3: ERA_MGR[int(p_[0])] = (p_[1], p_[2])

# ---------------------------------------------------------------- original database
import tempfile
_tmp = tempfile.NamedTemporaryFile(delete=False, suffix='.BIG'); _tmp.write(isofile.read_file(ISO, 'DATA/DATABASE/DATABASE.BIG')); _tmp.close()
T, ORDER = mvpdb.read_database(_tmp.name); os.unlink(_tmp.name)
AT = T['attrib.dat']; AT_by = AT.by_key()
TEAM = T['team.dat'].by_key()
ORG = T['org.dat'].by_key()
PIT_by = T['pitcher.dat'].by_key()
LH_by, RH_by = T['lhattrib.dat'].by_key(), T['rhattrib.dat'].by_key()
roster_rows = T['roster.dat'].rows
ci = {c: i for i, c in enumerate(AT.cols)}

special_teams = {k for k, v in TEAM.items() if v[4] == '10'}
keep_players = {'0f58f3c1b'} | {v[1] for k, v in roster_rows if v[0] in special_teams}
orig_team_of = {}
for k, v in roster_rows: orig_team_of.setdefault(v[1], v[0])
def level_of_team(tk):
    lg = int(TEAM[tk][4])
    return 1 if lg in (0, 1) else 11 if lg in (2, 3) else 12 if lg in (4, 5, 6) else 13 if lg in (7, 8, 9) else 0
ref_players = [k for k in AT_by if k not in keep_players and k in orig_team_of and level_of_team(orig_team_of[k]) in (1, 11, 12, 13)]
ref_mlb = [k for k in ref_players if level_of_team(orig_team_of[k]) == 1]
def is_pitcher_orig(k): return AT_by[k][ci['playerattrib_primary_position']] in ('0', '10')

# quantile maps: sorted reference value lists
def ref_values(keys, getter):
    vals = []
    for k in keys:
        try: vals.append(float(getter(k)))
        except (ValueError, TypeError): pass
    vals.sort(); return vals
def qmap(ref, pct):
    """percentile (0..1) -> value from the reference distribution"""
    if not ref: return 0
    i = min(len(ref) - 1, max(0, int(round(pct * (len(ref) - 1)))))
    return ref[i]

# ---------------------------------------------------------------- level translation helpers
LEVELS = {1: 1.0, 11: 0.9, 12: 0.85, 13: 0.8}
# Where a line was put up. A club's hitters and the hitters its pitchers faced, against the level's average: the
# part both have in common is the park (and, in the minors, the league: Albuquerque, Reno, Las Vegas, El Paso).
# Without it the 2027 batting race of a simulated season was led by three Rockies farmhands. Hits and home runs
# of a line are divided by its factor; only clubs with a full season's worth of at-bats on both sides get one.
def _park_factors():
    bat = collections.defaultdict(lambda: [0.0, 0.0, 0.0]); pit = collections.defaultdict(lambda: [0.0, 0.0, 0.0])
    for lines, acc, ab in ((H, bat, lambda s: s['ab']), (Q, pit, lambda s: s['bf'] - s['bb'] - s['hbp'])):
        for rows in lines.values():
            for season, sport, s in rows:
                if sport in LEVELS and s.get('_tm'):
                    a = acc[(season, sport, s['_tm'])]; a[0] += ab(s); a[1] += s['h']; a[2] += s['hr']
    out = {}
    for season, sport in {k[:2] for k in bat}:
        ks = [k for k in bat if k[:2] == (season, sport) and k in pit and bat[k][0] >= 2500 and pit[k][0] >= 2500]
        if len(ks) < 6: continue
        lab = sum(bat[k][0] + pit[k][0] for k in ks); lh = sum(bat[k][1] + pit[k][1] for k in ks) / lab; lhr = sum(bat[k][2] + pit[k][2] for k in ks) / lab
        for k in ks:
            fh = ((bat[k][1] / bat[k][0]) * (pit[k][1] / pit[k][0])) ** 0.5 / lh
            fhr = ((bat[k][2] / bat[k][0]) * (pit[k][2] / pit[k][0])) ** 0.5 / lhr
            out[k] = (min(1.09, max(0.93, fh)), min(1.28, max(0.80, fhr)))
    return out
PARK = _park_factors()
def park(season, sport, s): return PARK.get((season, sport, s.get('_tm')), (1.0, 1.0))
def hit_line(pid):
    """MLB-equivalent weighted hitting line (2026 x1, 2025 x0.5)."""
    tot = collections.defaultdict(float); raw = collections.defaultdict(float)
    for season, sport, s in H.get(pid, []):
        if sport not in LEVELS: continue
        w = (1.0 if season == SEASON else 0.5); f = LEVELS[sport]
        pa = s['pa']
        if pa <= 0: continue
        ph, phr = park(season, sport, s)
        # translation: fewer hits, less power, fewer walks and more strikeouts at the MLB level (AAA: hits x0.90,
        # doubles and walks x0.85, home runs x0.74, strikeouts x1.18; AA 0.85 / 0.78 / 0.63 / 1.30)
        tot['pa'] += w * pa; tot['ab'] += w * s['ab']
        tot['h'] += w * s['h'] * f / ph; tot['d'] += w * s['d'] * f ** 1.5 / ph; tot['t'] += w * s['t'] * f ** 1.5 / ph
        tot['hr'] += w * s['hr'] * f ** 2.8 / phr; tot['bb'] += w * s['bb'] * f ** 1.5; tot['so'] += w * min(s['so'] / f ** 1.6, s['ab'] * 0.6)
        # (the minors run far more: 1.0 steals a game in AAA, 1.4 in High-A, 0.7 in the majors; untranslated, the
        # top stealing ratings all went to farmhands and nobody in a simulated season stole 30)
        fs = {1: 1.0, 11: 0.65, 12: 0.55, 13: 0.48}[sport]
        tot['hbp'] += w * s['hbp']; tot['sb'] += w * s['sb'] * fs; tot['cs'] += w * s['cs'] * fs; tot['sh'] += w * s['sh']; tot['sf'] += w * s['sf']
        tot['go'] += w * s['go']; tot['ao'] += w * s['ao']; tot['g'] += w * s['g']
        if season == SEASON:
            for k in s:
                if k != '_tm': raw[(sport, k)] += s[k]
    return tot, raw
def pit_line(pid):
    tot = collections.defaultdict(float)
    for season, sport, s in Q.get(pid, []):
        if sport not in LEVELS: continue
        w = (1.0 if season == SEASON else 0.5); f = LEVELS[sport]
        if s['bf'] <= 0: continue
        ph, phr = park(season, sport, s)
        # (AAA: strikeouts x0.85, walks x1.15, home runs x1.23, hits x1.09)
        tot['bf'] += w * s['bf']; tot['outs'] += w * s['outs']; tot['g'] += w * s['g']; tot['gs'] += w * s['gs']
        tot['so'] += w * s['so'] * f ** 1.5; tot['bb'] += w * s['bb'] / f ** 1.3; tot['hr'] += w * s['hr'] / f ** 2 / phr
        tot['h'] += w * s['h'] / f ** 0.8 / ph; tot['er'] += w * s['er'] / f ** 1.5 / (ph * phr) ** 0.5; tot['go'] += w * s['go']; tot['ao'] += w * s['ao']
        tot['sv'] += w * s['sv']; tot['hld'] += w * s['hld']; tot['pk'] += w * s['pk']
    return tot

def regress(x, n, mean, k):
    return (x * n + mean * k) / (n + k)

# ---------------------------------------------------------------- choose the players for every slot
mlb_slot_org = {'Ana': 108, 'Oak': 133, 'Sea': 136, 'Tex': 140, 'CWS': 145, 'Cle': 114, 'Det': 116, 'KC': 118, 'Min': 142,
                'Bal': 110, 'Bos': 111, 'NYY': 147, 'TB': 139, 'Tor': 141, 'Ari': 109, 'Col': 115, 'LA': 119, 'SD': 135,
                'SF': 137, 'ChC': 112, 'Cin': 113, 'Hou': 117, 'Mil': 158, 'Pit': 134, 'StL': 138, 'Atl': 144, 'Fla': 146,
                'Was': 120, 'NYM': 121, 'Phi': 143}
team_key_by_short = {v[0]: k for k, v in TEAM.items()}
def dec_key(n): return '%09x' % (int(n) & 0xffffffff)
slots = []    # (team key, org id, level, size)
for short, org in mlb_slot_org.items():
    tk = team_key_by_short[short]; o = ORG[tk]
    slots.append((tk, org, 1, 25))
    slots.append((dec_key(o[2]), org, 11, 25))
    slots.append((dec_key(o[3]), org, 12, 22))
    slots.append((dec_key(o[4]), org, 13, 22))
for tk, org, lvl, n in slots: assert tk in TEAM, (tk, org, lvl)

def is_pitcher(pid):
    p = people[pid]; return p['pos'] in ('P', 'TWP') and not two_way_hitter(pid)
def two_way_hitter(pid):
    return people[pid]['pos'] == 'TWP'

def quality(pid):
    """rough value used only to pick who makes a roster (not a rating)"""
    if people[pid]['pos'] in ('P',):
        t = pit_line(pid)
        if t['bf'] < 1: return -1.0
        k = regress(t['so'] / t['bf'], t['bf'], 0.19, 150); b = regress(t['bb'] / t['bf'], t['bf'], 0.09, 150)
        hr = regress(t['hr'] / t['bf'], t['bf'], 0.03, 300)
        return (k - b * 1.3 - hr * 3) * 10 + math.log1p(t['outs']) * 0.08
    t, _ = hit_line(pid)
    if t['pa'] < 1: return -1.0
    obp = regress((t['h'] + t['bb'] + t['hbp']) / t['pa'], t['pa'], 0.29, 200)
    slg = regress((t['h'] + t['d'] + 2 * t['t'] + 3 * t['hr']) / max(1, t['ab']), t['ab'], 0.36, 200)
    return (obp * 1.7 + slg) + math.log1p(t['pa']) * 0.02

by_org = collections.defaultdict(lambda: collections.defaultdict(list))
for r in rost:
    if r['id'] not in people: continue
    by_org[r['org']][(r['sport'], r['type'])].append(r)
used = set(); assign = {}   # team key -> [pid]
jersey = {}
def _mlb_time(pid):
    return sum(st['pa'] * (1.0 if se == SEASON else 0.4) for se, sp, st in H.get(pid, []) if sp == 1) + \
           sum(st['bf'] * (1.0 if se == SEASON else 0.4) for se, sp, st in Q.get(pid, []) if sp == 1)
def pick(cands, n_p, n_h, majors=0.0):
    # A big-league club's 25 are the players it used: the rough value alone (rates pulled towards the average for
    # small samples) let a call-up with thirty good plate appearances take the place of an everyday catcher who
    # hits .230. Time played in the majors counts for about as much as the spread in value between regulars.
    def val(c): return quality(c) + ((0.14 if people[c]['pos'] != 'P' else 0.22) * min(1.0, _mlb_time(c) / (380.0 if people[c]['pos'] != 'P' else 260.0))) * majors
    ps = sorted({c for c in cands if people[c]['pos'] == 'P' and c not in used}, key=val, reverse=True)
    hs = sorted({c for c in cands if people[c]['pos'] != 'P' and c not in used}, key=val, reverse=True)
    # at least 2 catchers and a shortstop / centre fielder when available
    chosen_h = []
    for need, cnt in (('C', 2), ('SS', 1), ('CF', 1), ('2B', 1), ('3B', 1)):
        for h in hs:
            if len([x for x in chosen_h if people[x]['pos'] == need]) >= cnt: break
            if people[h]['pos'] == need and h not in chosen_h: chosen_h.append(h)
    # ... and enough outfielders and infielders to field a team (the Phillies' thirteen best bats had one
    # outfielder among them: Harper played right and a utility infielder left)
    if n_h >= 11:
        for group, cnt in ((('LF', 'CF', 'RF', 'OF'), 4), (('1B', '2B', '3B', 'SS', 'IF'), 5)):
            for h in hs:
                if len([x for x in chosen_h if people[x]['pos'] in group]) >= cnt: break
                if people[h]['pos'] in group and h not in chosen_h: chosen_h.append(h)
    for h in hs:
        if len(chosen_h) >= n_h: break
        if h not in chosen_h: chosen_h.append(h)
    # five starters first (three clubs had only four among their twelve best arms, and a one-inning reliever
    # with the least stamina of the staff started every fifth day), then the best arms left, whatever their job
    def starter(c): t = pit_line(c); return t['g'] > 0 and t['gs'] / t['g'] >= 0.5
    chosen_p = [c for c in ps if starter(c)][:min(5, n_p)]
    chosen_p += [c for c in ps if c not in chosen_p][:n_p - len(chosen_p)]
    return chosen_p, chosen_h[:n_h]

orig_comp = {}
for tk, org, lvl, n in slots:
    ps = [v[1] for k, v in roster_rows if v[0] == tk]
    npit = sum(1 for p in ps if is_pitcher_orig(p))
    orig_comp[tk] = (npit, len(ps) - npit)
leftover = collections.defaultdict(list)
for lvl in (1, 11, 12, 13):
    for tk, org, l, n in slots:
        if l != lvl: continue
        g = by_org[org]
        if lvl == 1:
            cands = list({r['id'] for r in g[(1, 'active')] + g[(1, '40Man')]})
            extra = [r['id'] for r in g[(1, '40Man')]] + [r['id'] for r in g[(11, 'active')]]
        else:
            cands = [r['id'] for r in g[(lvl, 'active')]] + leftover[org]
            extra = [r['id'] for r in g[(lvl + 1, 'active')]] if lvl < 13 else []
        npit, nhit = 12 if lvl == 1 else 11 if n == 22 else 13, 13 if lvl == 1 else 11 if n == 22 else 12
        cp, chh = pick(cands, npit, nhit, majors=1.0 if lvl == 1 else 0.6 if lvl == 11 else 0.0)   # AAA: the club's 26th to 40th men
        if len(cp) < npit or len(chh) < nhit:        # short: borrow from the next pool
            for x in cp + chh: used.add(x)
            ep, eh = pick(extra, npit - len(cp), nhit - len(chh)); cp += ep; chh += eh
        sel = cp + chh
        for x in sel: used.add(x)
        leftover[org] = [c for c in set(cands + extra) if c not in used]
        assign[tk] = sel
        for r in rost:
            if r['id'] in sel and r['org'] == org and r['num']: jersey.setdefault(r['id'], r['num'])
short = {tk: n - len(assign[tk]) for tk, org, l, n in slots if len(assign[tk]) != n}
if short: print('warning: short rosters', {TEAM[k][0]: v for k, v in short.items()})
# ---------------------------------------------------------------- free agents
# The game's free-agent pool is two hidden teams (Burnaby Argonauts and Chimaera, EA Canada's home town): the 39
# players nobody had signed when it shipped (Delgado, Ordonez, Olerud, Larkin ...) and eleven unlockable old-timers.
# Left alone, the clubs of a modern dynasty sign the stars of 2005. The 39 places go to big-leaguers of SEASON who
# did not get one of the 25 places of their club here or of its AAA team: by time played in the majors, each in a
# place of his own position where there is one. The old-timers (all "born" in 1948) stay.
FA_TEAMS = [tk for tk in special_teams if TEAM[tk][0] in ('Eaa', 'Eab')]
def _fa_pos(pid):
    if people[pid]['pos'] == 'P':
        t = pit_line(pid); return 0 if t['g'] > 0 and t['gs'] / t['g'] >= 0.5 else 10
    return {'C': 1, '1B': 2, '2B': 3, '3B': 4, 'SS': 5, 'LF': 6, 'CF': 7, 'RF': 8, 'OF': 8, 'DH': 2, 'IF': 3}.get(people[pid]['pos'], 9)
fa_slots = [(v[0], v[1]) for k, v in roster_rows if v[0] in FA_TEAMS and int(AT_by[v[1]][ci['playerattrib_birthday']]) > 400]
# (big-league time first; when those run out, the best of the others, so that no free agent of 2005 is left: one
# of them, 58 years old by then, was signed in July 2027 and hit .316)
_cand = sorted({r['id'] for r in rost if r['id'] in people and r['id'] not in used and people[r['id']]['pos'] != 'TWP' and
                quality(r['id']) > -1.0}, key=lambda q: (-(_mlb_time(q) if _mlb_time(q) >= 40 else 0), -quality(q), q))
fa_new = {}                                    # 2005 free agent's key -> the player who takes his place
for exact in (True, False):                    # first everyone into a place of his own position, then the rest
    for tk, old in fa_slots:
        if old in fa_new: continue
        want = int(AT_by[old][ci['playerattrib_primary_position']])
        for c in _cand:
            if c in used: continue
            got = _fa_pos(c)
            if got == want if exact else (got in (0, 10)) == (want in (0, 10)):
                fa_new[old] = c; used.add(c); assign.setdefault(tk, []).append(c); break
for r in rost:
    if r['id'] in fa_new.values() and r['num']: jersey.setdefault(r['id'], r['num'])
keep_players -= set(fa_new)
print('free agents: %d of the %d places of 2005 filled (%s ...)' % (len(fa_new), len(fa_slots), ', '.join(people[c]['full'] for c in list(fa_new.values())[:6])))

all_new = [pid for tk in assign for pid in assign[tk]]
print('players selected:', len(all_new))
# an era's draft classes (lahman_convert.py: era_draft.txt "id|year"): rated with everyone, written to OUT/draft/<year>.csv
DRAFT = {}
if os.path.exists(os.path.join(DATA, 'era_draft.txt')):
    for ln in open(os.path.join(DATA, 'era_draft.txt')):
        p_ = ln.strip().split('|')
        if len(p_) == 2 and int(p_[0]) in people and int(p_[0]) not in set(all_new): DRAFT[int(p_[0])] = int(p_[1])
    all_new += list(DRAFT)
    print('draft-class players:', len(DRAFT))

team_level = {tk: l for tk, org, l, n in slots}
for tk in FA_TEAMS: team_level[tk] = 1         # their lines on the cards are their big-league ones
level_of = {pid: team_level[tk] for tk in assign for pid in assign[tk]}
for pid in DRAFT: level_of[pid] = 13
team_of = {pid: tk for tk in assign for pid in assign[tk]}

# ---------------------------------------------------------------- per-player derived stats
def primary_pos(pid):
    p = people[pid]['pos']
    if p == 'P':
        t = pit_line(pid)
        return 'SP' if t['g'] > 0 and t['gs'] / t['g'] >= 0.5 else 'RP' if t['g'] > 0 else 'SP'
    if p == 'OF':
        best = fielding_positions(pid)
        return next((q for q, _ in best if q in ('LF', 'CF', 'RF')), 'LF')
    if p == 'IF': return next((q for q, _ in fielding_positions(pid) if q in ('1B', '2B', '3B', 'SS')), '2B')
    if p in ('DH', 'TWP'):
        # No player of the 2005 database has DH as his position (Ortiz and Edgar Martinez are first basemen with DH
        # as their second position): the glove he used most if he used one for 20 games, else first base.
        return next((q for q, g in fielding_positions(pid) if q in FIELD_POS and g >= 20), '1B')
    return p
FIELD_POS = ('C', '1B', '2B', '3B', 'SS', 'LF', 'CF', 'RF')
def is_dh(pid):
    """a hitter who mostly does not play the field"""
    g = dict(fielding_positions(pid)); f = sum(v for q, v in g.items() if q in FIELD_POS)
    return people[pid]['pos'] in ('DH', 'TWP') or g.get('DH', 0) > max(20, 1.5 * f)
def fielding_positions(pid):
    g = collections.defaultdict(float)
    for season, sport, pos, s in F.get(pid, []):
        if pos == 'P': continue
        g[pos] += s['g'] * (1.0 if season == SEASON else 0.5)
    return sorted(g.items(), key=lambda x: -x[1])
POSCODE = {'SP': 0, 'C': 1, '1B': 2, '2B': 3, '3B': 4, 'SS': 5, 'LF': 6, 'CF': 7, 'RF': 8, 'DH': 9, 'RP': 10}
prim = {pid: primary_pos(pid) for pid in all_new}
def secondary(pid):
    pp = prim[pid]
    if pp in ('SP', 'RP'): return 10 if pp == 'SP' else 0
    if is_dh(pid): return 9
    others = [q for q, g in fielding_positions(pid) if q != pp and q in FIELD_POS and g >= 5]
    if not others: return {'LF': 11, 'RF': 11, 'CF': 11}.get(pp, POSCODE[pp])
    ifs = [q for q in others if q in ('2B', '3B', 'SS')]; ofs = [q for q in others if q in ('LF', 'CF', 'RF')]
    if len(ifs) >= 2: return 12
    if len(ofs) >= 2: return 11
    if ifs and ofs: return 13
    return POSCODE[others[0]]

# hitting components (MLB-equivalent, regressed)
LG = dict(avg=0.245, iso=0.155, k=0.225, bb=0.085, hrpa=0.03)
hitc = {}
for pid in all_new:
    t, raw = hit_line(pid)
    pa = t['pa']; ab = max(t['ab'], 1)
    avg = regress(t['h'] / ab if t['ab'] else 0, t['ab'], LG['avg'] - 0.02, 250)
    iso = regress((t['d'] + 2 * t['t'] + 3 * t['hr']) / ab if t['ab'] else 0, t['ab'], LG['iso'] - 0.03, 250)
    k = regress(t['so'] / pa if pa else 0, pa, LG['k'] + 0.02, 120)
    bb = regress(t['bb'] / pa if pa else 0, pa, LG['bb'] - 0.01, 150)
    hrpa = regress(t['hr'] / pa if pa else 0, pa, LG['hrpa'] - 0.005, 300)
    ob = t['h'] - t['d'] - t['t'] - t['hr'] + t['bb'] + t['hbp']
    sba = regress((t['sb'] + t['cs']) / max(ob, 1), ob, 0.06, 60)
    sbp = regress(t['sb'] / max(t['sb'] + t['cs'], 1), t['sb'] + t['cs'], 0.7, 15)
    trip = regress(t['t'] / ab if t['ab'] else 0, t['ab'], 0.004, 300)
    shr = regress(t['sh'] / pa if pa else 0, pa, 0.003, 200)
    goao = regress(t['go'] / max(t['ao'], 1), t['go'] + t['ao'], 1.0, 80)
    hitc[pid] = dict(pa=pa, avg=avg, iso=iso, k=k, bb=bb, hrpa=hrpa, sba=sba, sbp=sbp, trip=trip, shr=shr, goao=goao, g=t['g'])

def split_line(pid, code):
    """vs LHP ('vl') / vs RHP ('vr') MLB-equivalent avg / iso / k with the player's overall line as the prior"""
    c = hitc[pid]; tot = collections.defaultdict(float)
    for season in (SEASON, SEASON - 1):
        s = SH.get((season, pid, code))
        if not s: continue
        w = 1.0 if season == SEASON else 0.5
        for key in ('pa', 'ab', 'h', 'd', 't', 'hr', 'so', 'bb'): tot[key] += w * s[key]
    bat = people[pid]['bat']
    # typical platoon split when the player has no split data: same-side pitchers cost ~20 points of average
    same = (bat == 'L' and code == 'vl') or (bat == 'R' and code == 'vr')
    adj = 0 if bat == 'S' else (-0.012 if same else 0.006) * (1.6 if bat == 'L' else 1.0)
    pa, ab = tot['pa'], max(tot['ab'], 1)
    avg = regress(tot['h'] / ab if tot['ab'] else 0, tot['ab'], c['avg'] + adj, 550)
    iso = regress((tot['d'] + 2 * tot['t'] + 3 * tot['hr']) / ab if tot['ab'] else 0, tot['ab'], c['iso'] + adj * 1.2, 500)
    k = regress(tot['so'] / pa if pa else 0, pa, c['k'] - adj, 300)
    return avg, iso, k

# savant helpers (2026 first, 2025 fallback)
def sv2(name, pid, col):
    v = sav(name, pid, col)
    return v if v is not None else sav(name + '25', pid, col)

# sprint speed model for players without Statcast data: fit on players who have it
fitX, fitY = [], []
for pid in all_new:
    s = sv2('sprint', pid, 'sprint_speed')
    if s is not None and people[pid]['pos'] != 'P' and hitc[pid]['pa'] > 50:
        c = hitc[pid]; fitX.append([1, c['sba'], c['trip'] * 20, 1 if prim[pid] == 'C' else 0, 1 if prim[pid] in ('SS', 'CF') else 0, 1 if prim[pid] in ('1B', 'DH') else 0]); fitY.append(s)
def lstsq(X, Y):
    n = len(X[0]); A = [[sum(x[i] * x[j] for x in X) + (1e-3 if i == j else 0) for j in range(n)] for i in range(n)]
    b = [sum(x[i] * y for x, y in zip(X, Y)) for i in range(n)]
    for i in range(n):                                  # gaussian elimination
        piv = A[i][i]
        for j in range(i, n): A[i][j] /= piv
        b[i] /= piv
        for r in range(n):
            if r != i and A[r][i]:
                f = A[r][i]
                for j in range(i, n): A[r][j] -= f * A[i][j]
                b[r] -= f * b[i]
    return b
# (without Statcast - an era built from the Lahman database - the fit of today's players)
SPD_DEFAULT = [26.21, 9.666, 5.644, -0.850, 0.228, -0.758]
spd_coef = lstsq(fitX, fitY) if len(fitX) >= 30 else SPD_DEFAULT
if os.environ.get('MVP_PRINT_SPD'): print('sprint fit', spd_coef)
def sprint(pid):
    s = sv2('sprint', pid, 'sprint_speed')
    if s is not None: return s
    c = hitc[pid]; x = [1, c['sba'], c['trip'] * 20, 1 if prim[pid] == 'C' else 0, 1 if prim[pid] in ('SS', 'CF') else 0, 1 if prim[pid] in ('1B', 'DH') else 0]
    age = age_of(pid)
    return sum(a * b for a, b in zip(spd_coef, x)) - max(0, age - 30) * 0.08
def age_of(pid):
    try: b = datetime.date.fromisoformat(people[pid]['birth'])
    except ValueError: return 27
    return (TODAY - b).days / 365.25

# fielding
def field_stats(pid, pos):
    tot = collections.defaultdict(float)
    for season, sport, fpos, s in F.get(pid, []):
        if fpos != pos: continue
        w = (1.0 if season == SEASON else 0.5) * LEVELS.get(sport, 0.8)
        for k in ('g', 'inn', 'po', 'a', 'e', 'ch', 'cs', 'sb'): tot[k] += w * s[k]
    return tot
fld = {}
for pid in all_new:
    pp = prim[pid]
    if pp in ('SP', 'RP', 'DH'):
        pos = 'P' if pp in ('SP', 'RP') else next((q for q, _ in fielding_positions(pid)), '1B')
    else: pos = pp
    t = field_stats(pid, pos)
    fp = regress((t['po'] + t['a']) / max(t['ch'], 1) if t['ch'] else 0, t['ch'], 0.975, 150)
    rf = regress((t['po'] + t['a']) / max(t['inn'], 1) * 9 if t['inn'] else 0, t['inn'], 0, 1)
    oaa = sv2('oaa', pid, 'outs_above_average')
    arm = sav('arm', pid, 'arm_overall')
    csr = regress(t['cs'] / max(t['cs'] + t['sb'], 1), t['cs'] + t['sb'], 0.22, 40) if pos == 'C' else None
    fld[pid] = dict(pos=pos, fp=fp, rf=rf, inn=t['inn'], oaa=oaa, arm=arm, csr=csr, g=t['g'])
# range factor relative to position average
rf_mean = collections.defaultdict(list)
for pid, f in fld.items():
    if f['inn'] > 100: rf_mean[f['pos']].append(f['rf'])
rf_mean = {k: sum(v) / len(v) for k, v in rf_mean.items()}

# pitching components
LGP = dict(k=0.215, bb=0.085, hr=0.029, gb=0.43)
pitc = {}
for pid in all_new:
    if people[pid]['pos'] != 'P': continue
    t = pit_line(pid); bf = t['bf']
    k = regress(t['so'] / bf if bf else 0, bf, LGP['k'] - 0.02, 150)
    bb = regress(t['bb'] / bf if bf else 0, bf, LGP['bb'] + 0.01, 200)
    hr = regress(t['hr'] / bf if bf else 0, bf, LGP['hr'], 400)
    ipgs = regress(t['outs'] / 3 / max(t['gs'], 1) if t['gs'] else 0, t['gs'], 4.8, 6)
    ipg = regress(t['outs'] / 3 / max(t['g'], 1) if t['g'] else 0, t['g'], 1.1, 10)
    gb = sv2('cp', pid, 'groundballs_percent')
    gb = gb / 100 if gb is not None else regress(t['go'] / max(t['go'] + t['ao'], 1), t['go'] + t['ao'], 0.5, 100)
    whiff = sv2('cp', pid, 'whiff_percent')
    pitc[pid] = dict(bf=bf, k=k, bb=bb, hr=hr, ipgs=ipgs, ipg=ipg, gb=gb, whiff=whiff, g=t['g'], gs=t['gs'], sv=t['sv'], hld=t['hld'], pk=t['pk'])

# ---------------------------------------------------------------- ratings by quantile mapping
def pct_rank(values):
    """dict key->value  =>  key->percentile (0..1), ties averaged"""
    items = sorted(values.items(), key=lambda x: x[1]); n = len(items); out = {}
    i = 0
    while i < n:
        j = i
        while j + 1 < n and items[j + 1][1] == items[i][1]: j += 1
        p = ((i + j) / 2) / max(n - 1, 1)
        for q in range(i, j + 1): out[items[q][0]] = p
        i = j + 1
    return out
def attr(k, col): return AT_by[k][ci['playerattrib_' + col]]

hitters = [p for p in all_new if people[p]['pos'] != 'P']
pitchers = [p for p in all_new if people[p]['pos'] == 'P']
ref_h = [k for k in ref_players if not is_pitcher_orig(k)]
ref_p = [k for k in ref_players if is_pitcher_orig(k)]
def mapped(keys_new, score, ref_keys, getter, jitter=0.0, salt=''):
    """Ratings by percentile, level by level: today's big-leaguers get the ratings the game's big-leaguers had,
    AAA players those of its AAA players, and so on. (One mapping over everybody gave the big-leaguers the top
    of every scale -- the 2005 database is not that steep -- and the pitchers gained more from it than the
    hitters: simulated seasons scored 3.6 runs a game, with five starters under 1.70.)"""
    out = {}
    for lvl in sorted({level_of[p] for p in keys_new}):
        grp = [p for p in keys_new if level_of[p] == lvl]
        refk = [k for k in ref_keys if level_of_team(orig_team_of[k]) == lvl]
        if len(refk) < 12 or len(grp) < 4: refk = ref_keys
        pr = pct_rank({p: score(p) for p in grp}); ref = ref_values(refk, getter)
        for p in grp:
            q = pr[p]
            if jitter: q = min(1, max(0, q + rng(p, salt).uniform(-jitter, jitter)))
            out[p] = int(round(qmap(ref, q)))
    return out

R = collections.defaultdict(dict)   # pid -> rating name -> value
def put(name, d):
    for p, v in d.items(): R[p][name] = v

# contact / power vs LHP and RHP (lhattrib = vs left-handed pitchers)
for code, tab, by in (('vl', 'lh', LH_by), ('vr', 'rh', RH_by)):
    sl = {p: split_line(p, code) for p in hitters}
    put(tab + '_contact', mapped(hitters, lambda p: sl[p][0] - 0.25 * (sl[p][2] - 0.22), ref_h, lambda k: by[k][2]))
    put(tab + '_power', mapped(hitters, lambda p: sl[p][1] + 0.4 * hitc[p]['hrpa'], ref_h, lambda k: by[k][3]))
put('platediscipline', mapped(hitters, lambda p: hitc[p]['bb'] - 0.25 * hitc[p]['k'] - 0.002 * (sv2('cb', p, 'oz_swing_percent') or 30), ref_h, lambda k: attr(k, 'platediscipline')))
put('speed', mapped(hitters, sprint, ref_h, lambda k: attr(k, 'speed')))
put('stealing_aggressive', mapped(hitters, lambda p: hitc[p]['sba'], ref_h, lambda k: attr(k, 'stealing_aggressive')))
put('baserunning', mapped(hitters, lambda p: sprint(p) + 3 * hitc[p]['sbp'], ref_h, lambda k: attr(k, 'baserunning'), 0.05, 'br'))
put('bunting', mapped(hitters, lambda p: hitc[p]['shr'] * 100 + sprint(p) * 0.05, ref_h, lambda k: attr(k, 'bunting'), 0.15, 'bunt'))
put('durability', mapped(all_new, lambda p: (hitc[p]['g'] if p in hitters else pitc[p]['g'] * (4 if pitc[p]['gs'] > 5 else 1.5)) - max(0, age_of(p) - 32) * 8, ref_players, lambda k: attr(k, 'durability'), 0.1, 'dur'))
# defence by position group (a shortstop is compared with 2005 shortstops)
pos_groups = collections.defaultdict(list)
for p in hitters: pos_groups[prim[p]].append(p)
ref_by_pos = collections.defaultdict(list)
INV = {v: k for k, v in POSCODE.items()}
for k in ref_h: ref_by_pos[INV.get(int(attr(k, 'primary_position')), 'DH')].append(k)
for pos, group in pos_groups.items():
    refk = ref_by_pos.get(pos) or ref_h
    def rscore(p):
        f = fld[p]
        base = (f['oaa'] / max(f['g'], 20) * 20) if f['oaa'] is not None else 0
        rfr = (f['rf'] - rf_mean.get(f['pos'], f['rf'])) if f['inn'] > 50 else 0
        return base + rfr * 2 - max(0, age_of(p) - 30) * 0.05
    put('range', mapped(group, rscore, refk, lambda k: attr(k, 'range'), 0.05, 'rng'))
    put('fielding', mapped(group, lambda p: fld[p]['fp'] + 0.002 * (fld[p]['oaa'] or 0), refk, lambda k: attr(k, 'fielding'), 0.05, 'fld'))
    def arm(p):
        f = fld[p]
        if f['arm'] is not None: return f['arm']
        if f['csr'] is not None: return 80 + f['csr'] * 20
        return 82 + rng(p, 'arm').uniform(-4, 4) + (3 if pos in ('SS', '3B', 'RF') else 0)
    put('throwstrength', mapped(group, arm, refk, lambda k: attr(k, 'throwstrength')))
    put('throwaccuracy', mapped(group, lambda p: fld[p]['fp'] * 50 + arm(p) * 0.2, refk, lambda k: attr(k, 'throwaccuracy'), 0.1, 'acc'))

# pitchers' own defence / running: copy a 2005 pitcher with a similar build
sp = [p for p in pitchers if prim[p] == 'SP']; rp = [p for p in pitchers if prim[p] == 'RP']
ref_sp = [k for k in ref_p if attr(k, 'primary_position') == '0']; ref_rp = [k for k in ref_p if attr(k, 'primary_position') == '10']
PIT = T['pitcher.dat']; pci = {c: i for i, c in enumerate(PIT.cols)}
def pv(k, col): return PIT_by[k][pci['pitchattrib_' + col]]
def mean_move(k):                    # a 2005 pitcher's breaking stuff: the average movement of his other pitches
    mv = [int(pv(k, 'pitch%d_movement' % n)) for n in range(2, 6) if pv(k, 'pitch%d_type' % n) != '-']
    return sum(mv) / len(mv) if mv else 5
put('stamina', mapped(sp, lambda p: pitc[p]['ipgs'], ref_sp, lambda k: pv(k, 'stamina')))
put('stamina', mapped(rp, lambda p: pitc[p]['ipg'], ref_rp, lambda k: pv(k, 'stamina')))
put('fb_control', mapped(pitchers, lambda p: -pitc[p]['bb'], ref_p, lambda k: pv(k, 'fastball_control')))
put('stuff', mapped(pitchers, lambda p: pitc[p]['k'] + (pitc[p]['whiff'] or 24) / 400 - pitc[p]['hr'] * 2, ref_p, mean_move))
put('pickoff', mapped(pitchers, lambda p: pitc[p]['pk'] / max(pitc[p]['g'], 5) + (0.2 if people[p]['thr'] == 'L' else 0), ref_p, lambda k: pv(k, 'pickoff'), 0.2, 'pk'))

# ---------------------------------------------------------------- pitch repertoires
SAV_PT = {'ff': None, 'si': 5, 'fc': 9, 'sl': 6, 'ch': 1, 'cu': 2, 'fs': 7, 'kn': 3, 'st': 14, 'sv': 14}
desc_mode = {}
cnt = collections.defaultdict(collections.Counter)
for k in ref_p:
    for n in range(2, 6):
        tp = pv(k, 'pitch%d_type' % n)
        if tp != '-': cnt[(tp, attr(k, 'throws'))][pv(k, 'pitch%d_description' % n)] += 1
for key, c in cnt.items(): desc_mode[key] = c.most_common(1)[0][0]
def arsenal(pid):
    out = []
    for yr in ('ars', 'ars25'):
        r = SAV.get(yr, {}).get(pid)
        if not r: continue
        for i, pt in enumerate(['ff', 'si', 'fc', 'sl', 'ch', 'cu', 'fs', 'kn', 'st', 'sv']):
            v = list(r.values())[i] if i < len(r) else ''
            if v and '/' in v:
                s, u = v.split('/'); out.append((pt, float(s), float(u)))
        if out: break
    return out
donor_cache = {}
# Pitching deliveries. The game has ten generic ones (0-9) and 49 that belong to one pitcher each (Randy Johnson,
# Wakefield's knuckleball motion, Dontrelle Willis ...). 9 is the sidearm / submarine motion (Bradford, Myers), 8 the
# low three-quarter one (Choate, Fuentes, League). New pitchers used to get the delivery of a random 2005 pitcher
# of the same hand: one in sixteen threw sidearm, one in twenty had somebody's signature motion. Now the arm angle
# decides (Statcast, where there is one), and nobody new gets a signature motion.
GENERIC_DELIV = ['0', '1', '2', '3', '5', '6', '7']
def delivery(pid):
    left = '1' if people[pid]['thr'] == 'L' else '0'
    pool = [pv(k, 'pitcher_delivery') for k in ref_p if attr(k, 'throws') == left and pv(k, 'pitcher_delivery') in GENERIC_DELIV]
    a = ARM.get(pid)
    if a is None:                          # no Statcast: the rates of those who have (2.5 % / 6.5 %)
        u = rng(pid, 'armslot').random()
        a = 0 if u < 0.025 else 15 if u < 0.09 else 40
    if a < 8: return '9'
    if a < 20: return '8'
    return rng(pid, 'deliv').choice(pool)
def donor(pid):
    """a 2005 pitcher with a similar role, hand and strikeout/walk profile (for minor leaguers' pitch mixes)"""
    if pid in donor_cache: return donor_cache[pid]
    role = '0' if prim[pid] == 'SP' else '10'; thr = '1' if people[pid]['thr'] == 'L' else '0'
    pool = [k for k in ref_p if attr(k, 'primary_position') == role and attr(k, 'throws') == thr] or ref_p
    r = rng(pid, 'donor'); d = r.choice(pool); donor_cache[pid] = d; return d
def repertoire(pid):
    """-> fastball (control, velocity), [(type, movement, description, control, velocity)] x up to 4"""
    thr = '1' if people[pid]['thr'] == 'L' else '0'
    r = rng(pid, 'rep'); fbc = R[pid]['fb_control']; stuff = R[pid]['stuff']
    ars = arsenal(pid)
    extra = []
    if ars:
        fast = [a for a in ars if a[0] in ('ff', 'si')]
        fb = max(fast, key=lambda a: a[2]) if fast else max(ars, key=lambda a: a[1])
        fbv = fb[1] + (0.5 if fb[0] == 'si' else 0)
        for pt, spd, use in sorted(ars, key=lambda a: -a[2]):
            code = SAV_PT.get(pt)
            if code is None or use < 3: continue
            if pt == 'si' and fb[0] == 'si' and not any(a[0] == 'ff' for a in ars): code = 8     # two-seamer as the second fastball
            if any(e[0] == code for e in extra): continue
            if pt == 'ch' and r.random() < 0.25: code = 10          # some changeups are circle changes
            if pt == 'cu' and r.random() < 0.2: code = 12           # and some curves knuckle-curves
            # (2005: every other pitch averages 6.5 under the fastball's control, whatever its place; movement
            # averages the pitcher's "stuff". The most-used pitch is a little better at both, the rest a little worse.)
            mv = max(1, min(15, int(round(stuff + max(-1.0, min(1.0, (use - 18) / 10)) + r.uniform(-2, 2)))))
            ctl = max(20, min(99, int(round(fbc - 6.5 + max(-3.0, min(3.0, (use - 18) / 5)) + r.uniform(-9, 9)))))
            extra.append((code, mv, desc_mode.get((str(code), thr), '4'), ctl, int(round(spd))))
        extra = extra[:4]
    else:
        d = donor(pid)
        dfb = float(pv(d, 'fastball_velocity'))
        # velocity: strikeout rate is the best available proxy without pitch tracking
        fbv = 92.5 + (pitc[pid]['k'] - 0.2) * 22 + (1.5 if prim[pid] == 'RP' else 0) + r.uniform(-1.5, 1.5)
        for n in range(2, 6):
            tp = pv(d, 'pitch%d_type' % n)
            if tp == '-': continue
            dv = float(pv(d, 'pitch%d_velocity' % n))
            mv = max(1, min(15, int(round(stuff + r.uniform(-2.5, 2.5)))))
            ctl = max(20, min(99, int(round(fbc - 6.5 + r.uniform(-9, 9)))))
            extra.append((int(tp), mv, pv(d, 'pitch%d_description' % n), ctl, int(round(dv + (fbv - dfb)))))
    fbv = int(round(max(80, min(101, fbv))))
    extra = [(t, m, d, c, max(55, min(fbv - 1, v))) for t, m, d, c, v in extra]
    if not extra: extra = [(1, stuff, desc_mode.get(('1', thr), '4'), max(20, fbc - 8), fbv - 9)]
    return (fbc, fbv), extra

# ---------------------------------------------------------------- appearance / misc from the 2005 distribution
def sample_orig(pid, col, salt, keys=None):
    keys = keys or ref_players
    return attr(rng(pid, salt).choice(keys), col)
orig_by_name = {}
for k, v in AT_by.items():
    orig_by_name[(v[0].lower(), v[1].lower(), v[ci['playerattrib_birthday']])] = k
def bday_days(pid):
    try: return (datetime.date.fromisoformat(people[pid]['birth']) - datetime.date(1947, 12, 31)).days
    except ValueError: return 12000
# The game's calendar starts in 2005; the runtime moves it to FIRST by adding YEAR_SHIFT to the year of every date
# the game makes out of a day number and taking it off every date it turns into one (and to the year constants in
# its code). The day numbers themselves stay what they were in 2005, so a birthday is stored YEAR_SHIFT years early:
# the game then shows the real date, and the age - on the card, and in everything a dynasty does with it (who
# improves, who declines, who retires) - is the player's age in the spring of FIRST.
def game_bday(pid):
    try: b = datetime.date.fromisoformat(people[pid]['birth'])
    except ValueError: return 12000
    y = b.year - YEAR_SHIFT
    try: b = b.replace(year=y)
    except ValueError: b = b.replace(year=y, day=28)             # 29 February
    return max(1, (b - datetime.date(1947, 12, 31)).days)
def height_in(pid):
    m = re.match(r"(\d+)' (\d+)", people[pid]['height'] or '')
    return int(m.group(1)) * 12 + int(m.group(2)) if m else 73
def weight_lb(pid):
    try: return int(people[pid]['weight'])
    except ValueError: return 200
def djb(s):
    h = 5381
    for c in s.encode('utf-8'): h = (h * 33 + c) & 0xffffffff
    return h
def ascii_name(s):
    import unicodedata
    return ''.join(c for c in unicodedata.normalize('NFKD', s) if not unicodedata.combining(c)).encode('latin1', 'replace').decode('latin1').replace(',', '')

# ---------------------------------------------------------------- appearance from MLB headshots
# game/rosters/appearance_<season>.txt (tools/faces/make_appearance.py): per player what his headshot shows: skin
# lightness, facial hair, and the hair that is visible under the cap (colour, how far down it reaches).
#
# The game has 15 generic heads (face 901-915). Each is a face texture with its own skin colour, and the 2005
# database always pairs a head with one body skin tone (FACE_SKIN), so the head is chosen first and the body
# follows. EA used 901-905 for white players, 906 for East Asian players, 907-910 for Latino players and 911-915
# for Black players; the textures get darker from 901 to 915. Here the head comes from the measured skin
# lightness, ranked among all measured players and mapped onto the 2005 shares of each head (906: players born
# in Japan, Korea or Taiwan).
# Texture names are g<head><hair colour><hair style><facial hair>.ssh in GHEAD.BIG, which gives the codes:
#   hair colour  0 black, 1 blond, 2 light brown, 3 red, 4 brown, 5 dark brown, 6 grey
#   hair style   0 shaved, 1-2 long, 3-6 short, 7-8 cropped, 9 braids
#   facial hair  0 none, 1 full beard, 2-4 moustaches, 5 goatee (moustache and chin), 6 chin beard, 7 soul patch
FACE_SKIN = {901: 0, 902: 1, 903: 1, 904: 2, 905: 2, 906: 3, 907: 4, 908: 5, 909: 5, 910: 6, 911: 7, 912: 8, 913: 9, 914: 9, 915: 10}
def face_group(f): return 'W' if f <= 905 else 'A' if f == 906 else 'L' if f <= 910 else 'B'
EAST_ASIA = {'Japan', 'JPN', 'Republic of Korea', 'South Korea', 'Korea', 'KOR', 'Taiwan', 'TWN', 'China', 'CHN'}
APPEAR = {}
_ap = os.path.join(DATA, 'appearance_%d.txt' % SEASON)
if os.path.exists(_ap):
    for l in open(_ap):
        if l.startswith('#') or not l.strip(): continue
        v = l.split(); APPEAR[int(v[0])] = ([float(x) for x in v[1:]] + [-1.0] * 17)[:17]
_gen_ref = [k for k in ref_players if int(attr(k, 'face')) in FACE_SKIN]
_by_group = collections.defaultdict(list)
for k in _gen_ref: _by_group[face_group(int(attr(k, 'face')))].append(k)
_ladder = [f for f in sorted(FACE_SKIN) if f != 906]                      # lightest -> darkest
_face_n = collections.Counter(int(attr(k, 'face')) for k in _gen_ref)
_cum, _acc = [], 0.0
for f in _ladder: _acc += _face_n[f] / max(sum(_face_n[g] for g in _ladder), 1); _cum.append((_acc, f))
_lights = sorted(v[0] for v in APPEAR.values())

def hair_code(L, a, b, skin, mixed=False):
    """OpenCV Lab of hair pixels -> the game's hair colour. skin: (L, a, b) of the face; mixed: beard pixels,
    with skin showing through. Skin in shadow looks like red hair (same hue, darker), so red needs more yellow
    than the skin has; grey is rare among players and needs a clearly colourless reading."""
    sL, sa, sb = skin
    if mixed: L -= 18
    ca, cb = a - 128, b - 128; chroma = math.hypot(ca, cb)
    if L > 125 and chroma < 4 and not mixed: return 6                                    # grey
    if cb >= (sb - 128) + 3 and ca >= (sa - 128) - 7 and ca >= 9 and L < sL - 25 and 50 < L < 150: return 3   # red
    if L > 128: return 1                                                                  # blond
    if L > 98: return 2                                                                   # light brown
    if L > 70: return 4                                                                   # brown
    if L > 46 or chroma > 14: return 5                                                    # dark brown
    return 0                                                                              # black

def appearance(pid):
    """-> the look columns for a new player (None-safe: without a headshot everything comes from the 2005
    players with the same kind of head, so head, body, hair and beard always belong together)."""
    a = APPEAR.get(pid)
    r = rng(pid, 'look')
    if people[pid].get('country') in EAST_ASIA: face = 906
    elif a:
        q = 1 - bisect.bisect_left(_lights, a[0]) / max(len(_lights) - 1, 1)     # 0 = lightest
        face = next(f for cum, f in _cum if q <= cum + 1e-9)
    else: face = r.choice([int(attr(k, 'face')) for k in _gen_ref if int(attr(k, 'face')) != 906])
    grp = face_group(face); pool = _by_group[grp]
    donor = r.choice(pool)
    out = {'face': face, 'face2004style': 0, 'skin_tone': FACE_SKIN[face],
           'haircolour': attr(donor, 'haircolour'), 'hairstyle': attr(donor, 'hairstyle'), 'facialhair': attr(donor, 'facialhair')}
    if not a: return out
    L, p_any, p_full, lip, chin, jaw, n, long_, hL, ha, hb, bL, ba, bb, browL, sa, sb = a
    skin = (L, sa if sa >= 0 else 146, sb if sb >= 0 else 148)
    seen = n >= 120 and hL >= 0                                   # hair shows beside the face
    long_hair = seen and long_ >= 0.10                            # ... down to the collar
    trust = seen and (long_hair or hL < L - 30)                   # a light reading from a few pixels is usually ear or neck
    # facial hair
    if p_full > 0.5: fh = 1
    elif p_any > 0.5: fh = r.choice((3, 3, 2, 4)) if lip >= 0.27 and lip - chin >= 0.15 and jaw < 0.25 else 5     # moustache only, else goatee
    else: fh = 0
    out['facialhair'] = fh
    # hair colour: the hair beside the face if enough of it shows, else a full beard, else the 2005 mix for this
    # kind of head, steered by how light the eyebrows are
    if grp == 'B': col = 0
    elif trust: col = hair_code(hL, ha, hb, skin)
    elif (fh == 1 or p_any > 0.6) and bL >= 0: col = hair_code(bL, ba, bb, skin, mixed=True)
    else:
        col = int(out['haircolour'])
        if grp == 'W' and browL >= 0:
            light = [k for k in pool if attr(k, 'haircolour') in ('1', '2')]; dark = [k for k in pool if attr(k, 'haircolour') in ('0', '4', '5')]
            if browL > 118 and light: col = int(attr(r.choice(light), 'haircolour'))
            elif browL < 60 and dark: col = int(attr(r.choice(dark), 'haircolour'))
    if grp in ('A', 'L') and col in (1, 2, 3, 6) and not trust: col = 5   # a light beard reading on a darker face: not trusted
    out['haircolour'] = col
    # hair style: long hair shows beside the jaw; otherwise a short style as EA gave this kind of head
    if long_hair: out['hairstyle'] = 9 if grp == 'B' else r.choice((1, 2))
    else:
        short = [k for k in pool if attr(k, 'hairstyle') not in ('1', '2', '9')]
        out['hairstyle'] = attr(r.choice(short or pool), 'hairstyle')
    return out

# body: skeleton/build (boneprofile) and body type from the 2005 players closest in height *and* weight
_body_ref = [(int(attr(k, 'height')) + 48, int(attr(k, 'weight')) + 100, attr(k, 'boneprofile'), attr(k, 'bodytype')) for k in ref_players]
def body_shape(pid):
    h, w = height_in(pid), weight_lb(pid)
    near = sorted(_body_ref, key=lambda b: ((b[0] - h) / 1.5) ** 2 + ((b[1] - w) / 8.0) ** 2)[:9]
    r = rng(pid, 'body')
    bone = r.choice(near)[2]
    heavy = sum(1 for b in near if b[3] == '2')
    return bone, ('2' if heavy * 2 > len(near) else '1')
# today's hitters wear far more protective gear than in 2005 (elbow guards on about half of them, many leg guards)
def modern_gear(pid, s):
    r = rng(pid, 'gear')
    s('elbowguard', r.choice([1, 2, 2, 3]) if r.random() < 0.45 else 0)
    s('shinguard', r.choice([1, 1, 1, 2]) if r.random() < 0.35 else 0)
    s('batglove', 1)

existing_keys = set(AT_by.keys())
newkey = {}
reuse = {}
for pid in all_new:
    p = people[pid]
    ok = orig_by_name.get((ascii_name(p['first']).lower(), ascii_name(p['last']).lower(), str(bday_days(pid))))
    if ok and ok not in keep_players:
        newkey[pid] = ok; reuse[pid] = ok; continue
    h = djb('mlbam:%d' % pid)
    while '%09x' % h in existing_keys or '%09x' % h in newkey.values(): h = (h + 1) & 0xffffffff
    newkey[pid] = '%09x' % h
print('players carried over from 2005 (same name and birthday):', [people[p]['full'] for p in reuse])

# Two keys the executable asks for by name. The batting and pitching mini-games build their practice pitcher ("Roy",
# no. 99) from the player whose key is hash("Reynolds, Shane") and their catcher from "Piazza, Mike" (0x20d638,
# 0x20d760). With neither in the database the game loaded an empty record and died looking for face file c000.ssh.
# A key is only a label, so two of today's players carry them: a right-handed starter of the club that had Reynolds
# (Arizona's AAA team) and the Mets' catcher, as in 2005.
def _carry_key(old_first, old_last, want):
    old = next((k for k, v in AT.rows if v[0] == old_first and v[1] == old_last), None)
    if old is None or old in keep_players or old in newkey.values(): return
    was_team = orig_team_of.get(old)
    cands = [p_ for p_ in all_new if want(p_) and p_ not in reuse and p_ not in DRAFT]
    cands.sort(key=lambda p_: (team_of[p_] != was_team, p_))
    if cands: newkey[cands[0]] = old; print('key of %s %s now carried by %s' % (old_first, old_last, people[cands[0]]['full']))
_carry_key('Shane', 'Reynolds', lambda p_: prim[p_] == 'SP' and people[p_].get('thr') == 'R')
_carry_key('Mike', 'Piazza', lambda p_: prim[p_] == 'C')

overall = {}
for p in hitters:
    r = R[p]; overall[p] = (r['lh_contact'] + r['rh_contact'] * 2 + r['lh_power'] + r['rh_power'] * 2) / 6 + r['speed'] * 0.1 + r['range'] + r['fielding']
for p in pitchers:
    r = R[p]; overall[p] = r['fb_control'] * 0.5 + r['stuff'] * 4 + (r['stamina'] * 0.2 if prim[p] == 'SP' else 5)
ov_pct = {}
ov_pct.update(pct_rank({p: overall[p] for p in hitters})); ov_pct.update(pct_rank({p: overall[p] for p in pitchers}))
minor_young = sorted([p for p in all_new if level_of[p] > 1 and age_of(p) <= 24], key=lambda p: -overall[p] * (1 + (24 - age_of(p)) * 0.05))
top_prospects = set(minor_young[:100])

# ---------------------------------------------------------------- write tables
def row_template(table, key):
    return list(T[table].by_key()[key])
DEFAULT = '0f58f3c1b'
new_rows = collections.defaultdict(list)
arow = {}                              # pid -> attrib row (commentary ids are filled in later)
def svc_years(pid):
    d = people[pid]['debut']
    try: return max(0, (TODAY - datetime.date.fromisoformat(d)).days / 365.25)
    except ValueError: return 0
for pid in all_new:
    p = people[pid]; key = newkey[pid]; r = R[pid]; is_p = pid in pitc
    fn, ln = ascii_name(p['first']), ascii_name(p['last'])
    a = row_template('attrib.dat', reuse.get(pid, DEFAULT))
    def s(col, v): a[ci['playerattrib_' + col]] = str(v)
    s('jerseynum', int(jersey.get(pid) or p['num'] or rng(pid, 'num').randint(1, 99)) if (jersey.get(pid) or p['num'] or '').isdigit() else rng(pid, 'num').randint(1, 99))
    s('bats', {'R': 0, 'L': 1, 'S': 2}.get(p['bat'], 0)); s('throws', 1 if p['thr'] == 'L' else 0)
    s('primary_position', POSCODE[prim[pid]]); s('secondary_position', secondary(pid))
    s('height', max(18, min(35, height_in(pid) - 48))); s('weight', max(50, min(190, weight_lb(pid) - 100)))
    if pid not in reuse:
        # generic appearance drawn from the 2005 distribution (edit in Manage Rosters > Edit Player)
        s('boneprofile', body_shape(pid)[0])
        for col in ('battingstance', 'batcolour', 'batglove', 'wristband', 'elbowguard', 'shinguard', 'socks', 'catchermask', 'fieldglovecolour', 'ditty'):
            s(col, sample_orig(pid, col, col))
        s('audioid', 0); s('photo', 2)
        for col, v in appearance(pid).items(): s(col, v)
    if pid not in reuse:
        s('bodytype', body_shape(pid)[1])
        if not is_p: modern_gear(pid, s)
    if is_p:
        d = donor(pid)
        for col in ('platediscipline', 'bunting', 'stealing_aggressive', 'baserunning', 'speed', 'fielding', 'range', 'throwstrength', 'throwaccuracy'):
            s(col, attr(d, col))
        s('fielding', max(2, min(15, int(attr(d, 'fielding')))))
    else:
        for col in ('platediscipline', 'bunting', 'stealing_aggressive', 'baserunning', 'speed', 'fielding', 'range', 'throwstrength', 'throwaccuracy'):
            s(col, r[col])
    s('durability', r['durability'])
    s('swingtype', 1 if (not is_p and max(r['lh_power'], r['rh_power']) >= 80) else 0)
    yrs = svc_years(pid); q = ov_pct[pid]
    if level_of[pid] > 1 or yrs < 3: sal = 8 if level_of[pid] == 1 else 3
    else: sal = int(10 + q ** 3 * 300 * min(1, (yrs - 2) / 4))
    s('salary', min(400, sal)); s('contract_length', 1 + rng(pid, 'con').randint(0, 5) if sal > 20 else 1 + rng(pid, 'con').randint(0, 2))
    s('starpower', 4 if q > 0.97 and level_of[pid] == 1 else 3 if q > 0.9 and level_of[pid] == 1 else 2 if q > 0.75 else 1 if q > 0.4 else 0)
    s('topprospect', 1 if pid in top_prospects else 0); s('hidden', 0)
    s('birthday', game_bday(pid))
    a[0], a[1] = fn, ln
    new_rows['attrib.dat'].append([key, a]); arow[pid] = a

    # batting tendencies vs LHP / RHP
    for tab, by, code in (('lhattrib.dat', LH_by, 'vl'), ('rhattrib.dat', RH_by, 'vr')):
        cols = T[tab].cols; row = list(by[reuse[pid]] if pid in reuse else by[DEFAULT]); row[0], row[1] = fn, ln
        if is_p:
            d = donor(pid); src = by[d]; row[2:] = src[2:]
        else:
            src = by[rng(pid, 'hz' + tab).choice(ref_h)]
            row[2:] = src[2:]
            row[2] = str(r[tab[:2] + '_contact']); row[3] = str(r[tab[:2] + '_power'])
            c = hitc[pid]
            pull = sv2('bb', pid, 'pull'); cent = sv2('bb', pid, 'straight'); oppo = sv2('bb', pid, 'oppo')
            if pull is not None and cent is not None and oppo is not None and (pull + cent + oppo) > 0:
                tot = pull + cent + oppo; pull, cent, oppo = [int(round(100 * x / tot)) for x in (pull, cent, oppo)]
                left = pull if p['bat'] == 'R' or (p['bat'] == 'S' and code == 'vl') else oppo
                right = 100 - left - cent
                row[cols.index('lrattrib_lf_pct')] = str(left); row[cols.index('lrattrib_cf_pct')] = str(cent); row[cols.index('lrattrib_rf_pct')] = str(right)
            gb = sv2('bb', pid, 'gb'); fb = sv2('bb', pid, 'fb'); ld = sv2('bb', pid, 'ld'); pu = sv2('bb', pid, 'pu')
            if gb is None:
                gb = 100 * c['goao'] / (1 + c['goao']) * 0.8; fb = (100 - gb) * 0.72; ld = 100 - gb - fb; pu = 0
            row[cols.index('lrattrib_gb_pct')] = str(int(round(gb))); row[cols.index('lrattrib_fb_pct')] = str(int(round(fb + (pu or 0))))
            row[cols.index('lrattrib_ld_pct')] = str(int(round(ld)))
            row[cols.index('lrattrib_hr_pct')] = str(max(0, min(25, int(round(c['hrpa'] / max(1 - c['k'] - c['bb'], 0.3) * 100)))))
            chase = sv2('cb', pid, 'oz_swing_percent'); whiff = sv2('cb', pid, 'whiff_percent')
            ch15 = (chase - 18) / 22 * 15 if chase is not None else 7 - (c['bb'] - 0.085) * 60
            mi15 = (whiff - 12) / 25 * 15 if whiff is not None else (c['k'] - 0.12) / 0.2 * 15
            for col in cols:
                if col.startswith('lrattrib_chase'): row[cols.index(col)] = str(max(0, min(15, int(round(ch15 + rng(pid, col).uniform(-2, 2))))))
                if col.startswith('lrattrib_miss'): row[cols.index(col)] = str(max(0, min(15, int(round(mi15 + rng(pid, col).uniform(-2, 2))))))
                if col.startswith('lrattrib_take'): row[cols.index(col)] = str(max(0, min(15, int(round(15 - ch15 + rng(pid, col).uniform(-3, 3))))))
        new_rows[tab].append([key, row])

    if is_p:
        pr = list(PIT_by[reuse[pid]] if pid in reuse and reuse[pid] in PIT_by else PIT_by[DEFAULT]); pr[0], pr[1] = fn, ln
        (fbc, fbv), extra = repertoire(pid)
        def ps(col, v): pr[pci['pitchattrib_' + col]] = str(v)
        ps('stamina', r['stamina']); ps('pickoff', r['pickoff']); ps('fastball_movement', 0); ps('fastball_description', 0)
        ps('fastball_control', fbc); ps('fastball_velocity', fbv)
        for n in range(2, 6):
            e = extra[n - 2] if n - 2 < len(extra) else None
            for j, col in enumerate(('type', 'movement', 'description', 'control', 'velocity')):
                ps('pitch%d_%s' % (n, col), e[j] if e else '-')
        if pid not in reuse: ps('pitcher_delivery', delivery(pid))
        new_rows['pitcher.dat'].append([key, pr])

    # ------------------------------------------------ last-season and career stats (shown on the player cards)
    lvl_stats = [s for season, sport, s in H.get(pid, []) if season == SEASON and sport == 1] or \
                [s for season, sport, s in H.get(pid, []) if season == SEASON and sport == level_of[pid]]
    hs = collections.defaultdict(float)
    for s_ in lvl_stats:
        for k_, v in s_.items(): hs[k_] += v
    def I(x): return str(int(round(x)))
    new_rows['bstats.dat'].append([key, [fn, ln, I(hs['g']), I(hs['r']), I(hs['sb']), I(hs['cs'])]])
    fs = collections.defaultdict(float)
    for season, sport, fpos, s_ in F.get(pid, []):
        if season == SEASON and fpos == fld[pid]['pos']:
            for k_ in ('g', 'po', 'a', 'e'): fs[k_] += s_[k_]
    new_rows['fbstats.dat'].append([key, [fn, ln, I(fs['g']), I(fs['po']), I(fs['a']), I(fs['e'])]])
    for tab, code, share in (('lhbstats.dat', 'vl', 0.28), ('rhbstats.dat', 'vr', 0.72)):
        s_ = SH.get((SEASON, pid, code)) if lvl_stats and any(sp == 1 for se, sp, _ in H.get(pid, []) if se == SEASON) else None
        if s_ is None: s_ = collections.defaultdict(float, {k_: v * share for k_, v in hs.items()})
        one = s_['h'] - s_['d'] - s_['t'] - s_['hr']
        new_rows[tab].append([key, [fn, ln] + [I(x) for x in (one, s_['d'], s_['t'], s_['hr'], s_['rbi'], s_['so'], s_['bb'], s_['ibb'], s_['sh'], s_['sf'], s_['hbp'], s_['pa'])]])
    ch = CH.get(pid, collections.defaultdict(float)) if level_of[pid] == 1 else collections.defaultdict(float)
    new_rows['career.dat'].append([key, [fn, ln] + [I(ch[k_]) for k_ in ('g', 'ab', 'h', 'hr', 'rbi', 'r', 'sb')]])
    if is_p:
        qs = collections.defaultdict(float)
        src = [s_ for season, sport, s_ in Q.get(pid, []) if season == SEASON and sport == 1] or \
              [s_ for season, sport, s_ in Q.get(pid, []) if season == SEASON and sport == level_of[pid]]
        for s_ in src:
            for k_, v in s_.items(): qs[k_] += v
        new_rows['pstats.dat'].append([key, [fn, ln] + [I(qs[k_]) for k_ in ('g', 'gs', 'r', 'er', 'w', 'l', 'sv', 'bs', 'cg', 'sho', 'pk')] + [I(qs['outs'] // 3), I(qs['outs'] % 3)]])
        for tab, code, share in (('lhpstats.dat', 'vl', 0.4), ('rhpstats.dat', 'vr', 0.6)):
            s_ = SPl.get((SEASON, pid, code))
            if s_ is None or not any(sp == 1 for se, sp, _ in Q.get(pid, []) if se == SEASON): s_ = collections.defaultdict(float, {k_: v * share for k_, v in qs.items()})
            one = s_['h'] - s_['d'] - s_['t'] - s_['hr']
            new_rows[tab].append([key, [fn, ln] + [I(x) for x in (s_['so'], s_['bb'], s_['ibb'], s_['hbp'], s_['sf'], s_['sh'], one, s_['d'], s_['t'], s_['hr'], s_['bf'])]])
        cp = CP.get(pid, collections.defaultdict(float)) if level_of[pid] == 1 else collections.defaultdict(float)
        new_rows['careerp.dat'].append([key, [fn, ln, I(cp['g']), I(cp['outs'] // 3), I(cp['outs'] % 3), I(cp['er']), I(cp['w']), I(cp['l']), I(cp['sv']), I(cp['so']), I(cp['bb'])]])

# draft classes out of the tables, into OUT/draft/<year>.csv (the columns of tools/leaguepack.py's players.csv)
if DRAFT:
    import csv as _csv
    dkeys = {newkey[p_]: p_ for p_ in DRAFT}
    PREF = {'attrib.dat': ('playerattrib_', ''), 'lhattrib.dat': ('lrattrib_', 'vsL_'), 'rhattrib.dat': ('lrattrib_', 'vsR_'), 'pitcher.dat': ('pitchattrib_', 'pitch_')}
    recs = collections.defaultdict(dict)
    for tab in list(new_rows):
        keep_ = []
        for k_, v_ in new_rows[tab]:
            if k_ not in dkeys: keep_.append([k_, v_]); continue
            if tab in PREF:
                dbp, pp = PREF[tab]; r_ = recs[k_]; r_['first'], r_['last'] = v_[0], v_[1]
                for c_, x_ in zip(T[tab].cols[2:], v_[2:]):
                    nm_ = pp + (c_[len(dbp):] if c_.startswith(dbp) else c_)
                    if c_.endswith('_birthday'):
                        d_ = datetime.date(1947, 12, 31) + datetime.timedelta(days=int(x_))
                        try: d_ = d_.replace(year=d_.year + YEAR_SHIFT)
                        except ValueError: d_ = d_.replace(year=d_.year + YEAR_SHIFT, day=28)
                        x_ = d_.isoformat()
                    r_[nm_] = x_
        new_rows[tab] = keep_
    os.makedirs(os.path.join(OUT, 'draft'), exist_ok=True)
    by_year = collections.defaultdict(list)
    for k_, r_ in recs.items(): by_year[DRAFT[dkeys[k_]]].append((overall.get(dkeys[k_], 0), r_))
    for y_, lst in sorted(by_year.items()):
        lst.sort(key=lambda t: -t[0])                       # the best first: the game's draft order picks from the top
        cols_ = []
        for _, r_ in lst:
            for c_ in r_:
                if c_ not in cols_: cols_.append(c_)
        with open(os.path.join(OUT, 'draft', '%d.csv' % y_), 'w', newline='', encoding='latin1') as f_:
            w_ = _csv.DictWriter(f_, cols_, restval=''); w_.writeheader(); w_.writerows(r_ for _, r_ in lst)
    print('draft classes: %d years, %d players -> %s' % (len(by_year), len(recs), os.path.join(OUT, 'draft')))

# keep legends / Default rows, replace everyone else
for tab in ORDER:
    tb = T[tab]
    if tb.cols[:2] != ['first_name', 'last_name'] or tab == 'manager.dat': continue
    kept = [[k, v] for k, v in tb.rows if k in keep_players]
    rows = kept + new_rows[tab]
    # the game ships its tables sorted by last name, first name
    rows.sort(key=lambda kv: (kv[0] != DEFAULT, kv[1][1].lower(), kv[1][0].lower()))
    tb.rows = rows
    for k, v in rows: assert len(v) == len(tb.cols), (tab, k, len(v), len(tb.cols))

# ---------------------------------------------------------------- lineups and pitching roles
DEF_WEIGHT = {'C': 1.2, 'SS': 1.4, 'CF': 1.3, '2B': 1.0, '3B': 1.0, 'RF': 0.8, 'LF': 0.7, '1B': 0.5}
_OF = ('LF', 'CF', 'RF')
def pos_fit(p, pos):
    """how well a hitter plays a position: 1 = it is his, 0 = he does not play it"""
    pp = prim[p]
    if pp == pos: return 1.0
    g = dict(fielding_positions(p)).get(pos, 0.0)          # games there, last season's counted half
    if pos == 'C': return 0.9 if g >= 30 else 0.75 if g >= 10 else 0.0
    if g >= 40: return 0.95
    if g >= 15: return 0.85
    if pos in ('LF', 'RF') and pp == 'CF': return 0.9
    if pos in ('LF', 'RF') and pp in _OF: return 0.85
    if pos == 'CF' and pp in _OF: return 0.7 if g >= 5 else 0.4
    if pos in ('2B', '3B') and pp == 'SS': return 0.8
    if pos == '1B' and pp in ('3B', 'LF', 'RF', 'C', '2B'): return 0.75 if g >= 5 else 0.55
    if (pos, pp) in (('2B', '3B'), ('3B', '2B')): return 0.75 if g >= 5 else 0.5
    if pos == 'SS' and pp in ('2B', '3B'): return 0.7 if g >= 5 else 0.3
    if pos in ('LF', 'RF') and pp in ('1B', '2B', '3B', 'SS'): return 0.6 if g >= 5 else 0.25
    return 0.6 if g >= 5 else 0.0
def assign_best(w):
    """w[i][j] = worth of column j in row i (rows <= columns): the column for every row that makes the sum largest
    (Hungarian method, so the builder needs nothing beyond Python)"""
    n, m = len(w), len(w[0]); INF = float('inf')
    u = [0.0] * (n + 1); v = [0.0] * (m + 1); pcol = [0] * (m + 1); way = [0] * (m + 1)
    for i in range(1, n + 1):
        pcol[0] = i; j0 = 0; minv = [INF] * (m + 1); usedc = [False] * (m + 1)
        while True:
            usedc[j0] = True; i0 = pcol[j0]; delta = INF; j1 = 0
            for j in range(1, m + 1):
                if usedc[j]: continue
                cur = -w[i0 - 1][j - 1] - u[i0] - v[j]
                if cur < minv[j]: minv[j] = cur; way[j] = j0
                if minv[j] < delta: delta = minv[j]; j1 = j
            for j in range(m + 1):
                if usedc[j]: u[pcol[j]] += delta; v[j] -= delta
                else: minv[j] -= delta
            j0 = j1
            if pcol[j0] == 0: break
        while True:
            j1 = way[j0]; pcol[j0] = pcol[j1]; j0 = j1
            if j0 == 0: break
    res = [0] * n
    for j in range(1, m + 1):
        if pcol[j]: res[pcol[j] - 1] = j - 1
    return res

def lineup_rows(tk, pids):
    """roster.dat rows for one team: AL/NL lineups vs RHP and LHP, pitching staff roles"""
    ps = [p for p in pids if p in pitc]; hs = [p for p in pids if p not in pitc]
    sps = sorted([p for p in ps if prim[p] == 'SP'], key=lambda p: -overall[p])
    rps = sorted([p for p in ps if prim[p] == 'RP'], key=lambda p: -overall[p])
    while len(sps) < 5 and rps:                             # not enough starters: the reliever who goes longest starts
        lm = max(rps, key=lambda p: pitc[p]['ipg']); rps.remove(lm); sps.append(lm)
    rotation = sps[:5]; pen = sps[5:] + rps
    pen_by_save = sorted(pen, key=lambda p: -(pitc[p]['sv'] * 3 + pitc[p]['hld'] + overall[p] * 0.2))
    role = {}
    for i, p in enumerate(rotation): role[p] = 'SP%d' % (i + 1)
    rest = list(pen_by_save)
    if rest: role[rest.pop(0)] = 'CP'
    if rest: role[rest.pop(0)] = 'SU'
    long_men = sorted(rest, key=lambda p: -pitc[p]['ipg'])[:2] if len(rest) > 3 else []
    for p in rest: role[p] = 'LR' if p in long_men else 'MR'
    out = {p: [role.get(p, 'B'), '-1', role.get(p, 'B'), '-1', role.get(p, 'B'), '-1', role.get(p, 'B'), '-1'] for p in ps}
    for p in hs: out[p] = ['B', '-1', 'B', '-1', 'B', '-1', 'B', '-1']
    for side, base in (('vr', 0), ('vl', 4)):
        tab = 'rh' if side == 'vr' else 'lh'
        def off(p): return R[p][tab + '_contact'] * 1.2 + R[p][tab + '_power'] + R[p]['platediscipline'] * 3 + R[p]['speed'] * 0.15
        for dh in (True, False):
            # Who plays where: the nine (or eight) that are worth most together, each at a position he plays.
            # (This used to go position by position and take the best bat left, with a small penalty for a
            # position he never played: Soto at second base, Trea Turner at DH with a second baseman at short.)
            spots = list(FIELD_POS) + (['DH'] if dh else [])
            def worth(p, pos):
                if pos == 'DH': return off(p) + (10 if is_dh(p) else 0)
                fit = pos_fit(p, pos)
                if fit <= 0: return off(p) - 1000
                glove = (R[p]['fielding'] * 3 + R[p]['throwstrength'] * 2) if pos == 'C' else (R[p]['range'] * 3 + R[p]['fielding'] * 2)
                return off(p) + DEF_WEIGHT[pos] * glove * fit - (1 - fit) * 150 - (25 if is_dh(p) else 0)
            pick_ = assign_best([[worth(p, pos) for p in hs] for pos in spots]) if len(hs) >= len(spots) else {}
            taken = {pos: hs[j] for pos, j in zip(spots, pick_)} if pick_ else {}
            starters = list(taken.values())
            # Batting order: a man who gets on base and can run first, the best hitter second, the next best third,
            # the most power of the rest fourth, then as they come; the pitcher last.
            def onbase(p): return R[p][tab + '_contact'] + R[p]['platediscipline'] * 4 + R[p]['speed'] * 0.25
            rest = sorted(starters, key=lambda p: -off(p)); order_ = []
            if rest:
                lead = max(rest[:6], key=lambda p: onbase(p) - R[p][tab + '_power'] * 0.35); order_.append(lead); rest.remove(lead)
            for _ in range(2):
                if rest: order_.append(rest.pop(0))
            if rest:
                clean = max(rest[:4], key=lambda p: R[p][tab + '_power'] * 1.5 + R[p][tab + '_contact'] * 0.5); order_.append(clean); rest.remove(clean)
            order_ += rest
            if not dh and rotation: order_.append(rotation[0])
            col = base + (0 if dh else 2)
            for pos, p in taken.items(): out[p][col] = pos
            for i, p in enumerate(order_[:9]): out[p][col + 1] = str(i + 1)
    return [[tk, newkey[p]] + out[p] for p in pids]
new_roster = []
for tk, org, lvl, n in slots: new_roster += lineup_rows(tk, assign[tk])
# All-Star teams: best of each league, same shape as a big-league roster
for short_, lg in (('AL', '0'), ('NL', '1')):
    tk = team_key_by_short[short_]
    pool = [p for t_, org, lvl, n in slots if lvl == 1 and TEAM[t_][4] == lg for p in assign[t_]]
    ps = sorted([p for p in pool if p in pitc], key=lambda p: -overall[p] - (40 if prim[p] == 'SP' else 0))
    sp_ = [p for p in ps if prim[p] == 'SP'][:5]; rp_ = [p for p in ps if prim[p] == 'RP'][:7]
    hs = []
    for pos in ('C', '1B', '2B', '3B', 'SS', 'LF', 'CF', 'RF', 'DH', 'C', 'SS', 'CF', '1B'):
        c = [p for p in pool if p not in pitc and prim[p] == pos and p not in hs]
        if c: hs.append(max(c, key=lambda p: overall[p]))
    rest = sorted([p for p in pool if p not in pitc and p not in hs], key=lambda p: -overall[p])
    hs += rest[:13 - len(hs)]
    new_roster += lineup_rows(tk, sp_ + rp_ + hs[:13])
kept_roster = [[k, [v[0], newkey[fa_new[v[1]]] if v[1] in fa_new else v[1]] + v[2:]] for k, v in roster_rows if v[0] in special_teams]
rows = [[None, [r[0], r[1]] + r[2:]] for r in new_roster] + [[None, v] for k, v in kept_roster]
# The game numbers the teams in the order they first appear in roster.dat and loads its in-game logos by that
# number (frontend/logos.big l<number - 1>.ssh): keep the 2005 team order, or every logo on the batter and pitcher
# cards is another team's file and shows as a black square.
team_order = {}
for k, v in roster_rows: team_order.setdefault(v[0], len(team_order))
rows.sort(key=lambda kv: team_order.get(kv[1][0], len(team_order)))          # stable: lineups keep their order
T['roster.dat'].rows = [['%09x' % i, v] for i, (_, v) in enumerate(rows)]

# ---------------------------------------------------------------- teams, managers
tm = T['team.dat']; tci = {c: i for i, c in enumerate(tm.cols)}
RENAME = {'Ana': ('Los Angeles', 'Angels', 'LAA'), 'Oak': ('Sacramento', 'Athletics', 'ATH'), 'Cle': ('Cleveland', 'Guardians', 'CLE'),
          'TB': ('Tampa Bay', 'Rays', 'TB'), 'Fla': ('Miami', 'Marlins', 'MIA')}
if ERA:                                  # the clubs of that season, in their franchises' slots (mvp_teams: city, nickname, abbreviation)
    RENAME = {}
    org_short = {o: sh for sh, o in mlb_slot_org.items()}
    for tid, t in teams_api.items():
        if t['sport'] == 1 and t['org'] in org_short: RENAME[org_short[t['org']]] = (t['loc'], t['name'], t['abbr'][:3].upper())
ORIG_NAMES = {v[0]: (v[1], v[2], v[3]) for k, v in tm.rows}
for k, v in tm.rows:
    if v[0] in RENAME: v[1], v[2], v[3] = RENAME[v[0]]
MANAGERS = {'Ana': ('Kurt', 'Suzuki', 8), 'Oak': ('Mark', 'Kotsay', 7), 'Sea': ('Dan', 'Wilson', 6), 'Tex': ('Skip', 'Schumaker', 55),
            'CWS': ('Will', 'Venable', 1), 'Cle': ('Stephen', 'Vogt', 12), 'Det': ('A.J.', 'Hinch', 14), 'KC': ('Matt', 'Quatraro', 33),
            'Min': ('Derek', 'Shelton', 8), 'Bal': ('Craig', 'Albernaz', 55), 'Bos': ('Chad', 'Tracy', 17), 'NYY': ('Aaron', 'Boone', 17),
            'TB': ('Kevin', 'Cash', 16), 'Tor': ('John', 'Schneider', 14), 'Ari': ('Torey', 'Lovullo', 17), 'Col': ('Warren', 'Schaeffer', 4),
            'LA': ('Dave', 'Roberts', 30), 'SD': ('Craig', 'Stammen', 14), 'SF': ('Tony', 'Vitello', 23), 'ChC': ('Craig', 'Counsell', 11),
            'Cin': ('Terry', 'Francona', 77), 'Hou': ('Joe', 'Espada', 21), 'Mil': ('Pat', 'Murphy', 49), 'Pit': ('Don', 'Kelly', 12),
            'StL': ('Oliver', 'Marmol', 37), 'Atl': ('Walt', 'Weiss', 22), 'Fla': ('Clayton', 'McCullough', 86), 'Was': ('Blake', 'Butera', 10),
            'NYM': ('Andy', 'Green', 70), 'Phi': ('Don', 'Mattingly', 8)}
if ERA:
    MANAGERS = {sh: (ERA_MGR[o][0], ERA_MGR[o][1], 0) for sh, o in mlb_slot_org.items() if o in ERA_MGR}
MG = T['manager.dat']; mci = {c: i for i, c in enumerate(MG.cols)}; mg_by = MG.by_key()
for tk, o in ORG.items():
    short_ = TEAM[tk][0]
    if short_ not in MANAGERS: continue
    mk = dec_key(o[12]); row = mg_by.get(mk)
    if row is None: continue
    f_, l_, num = MANAGERS[short_]
    if (row[0], row[1]) == (f_, l_): continue
    row[0], row[1] = f_, l_; row[mci['manager_jerseynum']] = str(num); row[mci['manager_audioid']] = '0'

os.makedirs(os.path.join(OUT, 'DATA', 'DATABASE'), exist_ok=True)
mvpdb.write_database(os.path.join(OUT, 'DATA', 'DATABASE', 'DATABASE.BIG'), T, ORDER)
print('wrote', os.path.join(OUT, 'DATA', 'DATABASE', 'DATABASE.BIG'))

# ---------------------------------------------------------------- season-by-season history (DATA/DATABASE/HIST.DAT)
# The player cards' year-by-year lines. The file is a count and 25-byte records: player key, 01, four zero bytes, year,
# club (0-29 in the order of mlb_slot_org), 0x81 batting / 0x82 pitching, three packed words:
#   batting:  AB:10 G:8 SB:7 | H:9 RBI:8 R:8 HR:7 | 0        pitching: ER:8 BB:8 SO:9 G:7 | IP:9 thirds:2 W:6 L:6 | SV
# The game loads all of it at start-up into a table with room for 3750 players, one entry per key, whether or not the
# player still exists. With the 2005 file left in place its ~1200 departed players kept their entries, the new players
# got theirs as they first appeared in a simulated game, and a dynasty froze for good on about its seventh day
# (the table full, a live entry handed out twice, the search tree tied in a knot). So: the 2005 lines only for the
# players who are still here, and for everyone what the stats feed has for the seasons before FIRST. The years are
# shown as they are written.
import struct as _st
_hist = isofile.read_file(ISO, 'DATA/DATABASE/HIST.DAT')
_n = _st.unpack_from('<I', _hist, 0)[0]
_old = [_hist[4 + 25 * i: 29 + 25 * i] for i in range(_n)]
_carried = {int(k, 16) for k in reuse.values()} | {int(k, 16) for k in keep_players}
_final = {int(k, 16) for k, v in T['attrib.dat'].rows}
_recs = [r for r in _old if _st.unpack_from('<I', r, 0)[0] in _carried and _st.unpack_from('<I', r, 0)[0] in _final]
_club = {org: i for i, org in enumerate(mlb_slot_org.values())}
def _cl(x, bits): return max(0, min((1 << bits) - 1, int(round(x))))
_added = 0
for pid in sorted(all_new, key=lambda q: (ascii_name(people[q]['last']).lower(), ascii_name(people[q]['first']).lower(), q)):
    key = int(newkey[pid], 16)
    for kind, src in ((0x81, _H_rows.get(pid, [])), (0x82, _Q_rows.get(pid, []))):
        for season, team, st in sorted(src, key=lambda r: r[0]):
            year = season
            if year >= FIRST or team not in _club or st['g'] <= 0: continue
            if kind == 0x81:
                w = (_cl(st['ab'], 10) | _cl(st['g'], 8) << 10 | _cl(st['sb'], 7) << 18,
                     _cl(st['h'], 9) | _cl(st['rbi'], 8) << 9 | _cl(st['r'], 8) << 17 | _cl(st['hr'], 7) << 25, 0)
            else:
                outs = int(round(st['outs']))
                w = (_cl(st['er'], 8) | _cl(st['bb'], 8) << 8 | _cl(st['so'], 9) << 16 | _cl(st['g'], 7) << 25,
                     _cl(outs // 3, 9) | (outs % 3) << 9 | _cl(st['w'], 6) << 11 | _cl(st['l'], 6) << 17, _cl(st['sv'], 8))
            _recs.append(_st.pack('<IB4xHBB3I', key, 1, year, _club[team], kind, *w)); _added += 1
assert all(len(r) == 25 for r in _recs) and len({_st.unpack_from('<I', r, 0)[0] for r in _recs} - _final) == 0
open(os.path.join(OUT, 'DATA', 'DATABASE', 'HIST.DAT'), 'wb').write(_st.pack('<I', len(_recs)) + b''.join(_recs))
print('wrote HIST.DAT: %d of the %d lines of 2005 kept, %d lines for %d players from the stats feed' % (len(_recs) - _added, _n, _added, len({_st.unpack_from('<I', r, 0)[0] for r in _recs[len(_recs) - _added:]})))

# ---------------------------------------------------------------- commentary names (separate overlay)
# The commentators recorded surnames (PBP, 4 inflections each) and the stadium announcer full names (PA), keyed by
# playerattrib_audioid. The game's own rookie generator names its made-up players through ROOKIE.BIG/audioid.csv
# (surname -> id); new players get their 2005 namesake's id on a full-name match, otherwise the surname's id.
# The PA clip of a surname-only id would announce the 2005 player's first name, so those PA clips are silenced
# (see below).
import struct
from bigfile import read_big
COMM = os.path.join(OUT, 'commentary') if ERA else os.path.join(os.path.dirname(os.path.normpath(OUT)), 'commentary-%d' % SEASON)   # an era's goes inside its pack
def norm_name(x): return re.sub(r'[^a-z ]', '', ascii_name(x).lower().replace(' jr.', '').replace(' jr', '')).strip()
_rk = isofile.read_file(ISO, 'DATA/DATABASE/ROOKIE.BIG')
import refpack
aud_csv = {}
for n_, o_, s_ in read_big(_rk):
    if n_.lower() != 'audioid.csv': continue
    c_ = _rk[o_:o_ + s_]
    if c_[:2] != b'Pl': c_ = refpack.decompress(c_)
    for l_ in c_.decode('latin1').splitlines()[1:]:
        i_, _, nm_ = l_.partition(',')
        if i_.strip().isdigit(): aud_csv.setdefault(norm_name(nm_.strip().strip('"')), int(i_))
pbp_ids = set(int(n_[:4]) for n_, o_, s_ in read_big(isofile.read_file(ISO, 'DATA/AUDIO/SPCH_PBP/PNAMEHDR.BIG')))
full05 = {}
for k_, row_ in AT_by.items():                     # the original rows (the table itself now holds the new ones)
    aid_ = int(row_[ci['playerattrib_audioid']])
    if aid_: full05.setdefault((norm_name(row_[0]), norm_name(row_[1])), aid_)
used_kept = set(int(v_[ci['playerattrib_audioid']]) for k_, v_ in AT.rows if k_ in keep_players) | \
            set(int(arow[p_][ci['playerattrib_audioid']]) for p_ in reuse if p_ in arow)
n_full = n_sur = 0; sur_ids, full_ids = set(), set()
for pid, a in arow.items():
    if pid in reuse: continue
    fn_, ln_ = norm_name(people[pid]['first']), norm_name(people[pid]['last'])
    aid = full05.get((fn_, ln_))
    if aid and aid in pbp_ids: n_full += 1; full_ids.add(aid)
    else:
        aid = aud_csv.get(ln_) or aud_csv.get(ln_.split(' ')[-1])
        if not aid or aid not in pbp_ids: continue
        n_sur += 1; sur_ids.add(aid)
    a[ci['playerattrib_audioid']] = str(aid)
for row_ in MG.rows:                                 # renamed managers
    v_ = row_[1]
    if v_[mci['manager_audioid']] != '0': continue
    aid = aud_csv.get(norm_name(v_[1]))
    if aid and aid in pbp_ids: v_[mci['manager_audioid']] = str(aid); sur_ids.add(aid)
silence = sur_ids - full_ids - used_kept
os.makedirs(os.path.join(COMM, 'DATA', 'DATABASE'), exist_ok=True)
mvpdb.write_database(os.path.join(COMM, 'DATA', 'DATABASE', 'DATABASE.BIG'), T, ORDER)
# hide those ids from the announcer's index (PNAMEHDR.BIG): the game then skips the name, exactly as for its
# generated rookies whose surname has no announcer clip. The entry is renamed NNNN.hdr -> NNNN.hdx (sort order kept).
pa = bytearray(isofile.read_file(ISO, 'DATA/AUDIO/SPCH_PA/PNAMEHDR.BIG')); n_sil = 0
n_ent = struct.unpack('>I', pa[8:12])[0]; off = 16
for _ in range(n_ent):
    e_ = pa.index(b'\0', off + 8); nm_ = pa[off + 8:e_].decode('latin1')
    if nm_.endswith('.hdr') and nm_[:4].isdigit() and int(nm_[:4]) in silence: pa[e_ - 1:e_] = b'x'; n_sil += 1
    off = e_ + 1
os.makedirs(os.path.join(COMM, 'DATA', 'AUDIO', 'SPCH_PA'), exist_ok=True)
old_dat = os.path.join(COMM, 'DATA', 'AUDIO', 'SPCH_PA', 'PNAMEDAT.BIG')
if os.path.exists(old_dat): os.remove(old_dat)
open(os.path.join(COMM, 'DATA', 'AUDIO', 'SPCH_PA', 'PNAMEHDR.BIG'), 'wb').write(pa)
print('commentary: %d new players named by full name, %d by surname, %d without; %d announcer clips hidden -> %s' %
      (n_full, n_sur, len(arow) - len(reuse) - n_full - n_sur, n_sil, COMM))
import build_team_speech                     # the renamed teams, said with recordings that are on the disc
build_team_speech.build(ISO, COMM)

# ---------------------------------------------------------------- team names in the menus / in-game text
def rename_loc(L):
    S = L.strings; n = 0
    year_text = {'You failed to meet the expectations set for your team during the 2005 season.':
                 'You failed to meet the expectations set for your team this season.'}
    exact = {'Anaheim Angels': 'Los Angeles Angels', 'Oakland Athletics': 'Athletics', 'Tampa Bay Devil Rays': 'Tampa Bay Rays',
             'Cleveland Indians': 'Cleveland Guardians', 'Florida Marlins': 'Miami Marlins', 'Devil Rays': 'Rays'}
    ctx = {('Indians', 'White Sox'): 'Guardians', ('ANA', 'MIN'): 'LAA', ('OAK', 'ANA'): 'ATH', ('FLA', 'ATL'): 'MIA',
           ('Oakland', 'Anaheim'): 'Sacramento', ('Florida', 'Atlanta'): 'Miami'}
    prev = [S[i - 1] if i else '' for i in range(len(S))]
    if ERA:                             # the 2005 names -> that season's: full names and nicknames as they are, cities and
        exact, ctx = {}, {}             # abbreviations by the one before them in the game's lists of clubs
        order = list(mlb_slot_org)
        nicks = collections.Counter(ORIG_NAMES[sh][1] for sh in order)
        for i, sh in enumerate(order):
            if sh not in RENAME: continue
            (oc, on, oa), (ec, en, ea) = ORIG_NAMES[sh], RENAME[sh]
            if (oc, on) != (ec, en): exact['%s %s' % (oc, on)] = '%s %s' % (ec, en)
            if on != en and nicks[on] == 1: exact[on] = en
            if i:
                pc, pn, pa = ORIG_NAMES[order[i - 1]]
                if oc != ec: ctx[(oc, pc)] = ec
                if oa != ea: ctx[(oa.upper(), pa.upper())] = ea; ctx[(oa, pa)] = ea
    for i, s_ in enumerate(S):
        if s_ in exact: S[i] = exact[s_]; n += 1
        elif s_ in year_text: S[i] = year_text[s_]; n += 1
        elif s_ == '2005' and prev[i] == 'Random': S[i] = str(FIRST); n += 1          # draft order: last year's standings
        elif (s_, prev[i]) in ctx: S[i] = ctx[(s_, prev[i])]; n += 1
        elif not ERA and s_ == 'Anaheim' and i + 1 < len(S) and S[i + 1] == 'Oakland': S[i] = 'Los Angeles'; n += 1
    return n
for name in ('FEENG.LOC', 'IGENG.LOC'):
    L = loc.Loc(isofile.read_file(ISO, 'DATA/' + name)); n = rename_loc(L)
    open(os.path.join(OUT, 'DATA', name), 'wb').write(L.build()); print('wrote %s (%d team-name strings updated)' % (name, n))

open(os.path.join(OUT, 'year.txt'), 'w').write('%d\n' % FIRST)
# Balance of simulated games. The game rates a fastball control / 2 + velocity * 1.5 - 0.93 (velocity = mph / 101)
# and its own big-league pitchers average 90 mph; with today's real velocities the constant moves up by the
# difference, so the average fastball rates what it did (the runtime reads sim.txt, see hle_pitch_quality).
_v_old = [int(pv(k, 'fastball_velocity')) for k in ref_mlb if is_pitcher_orig(k) and k in PIT_by]
_pid_of_key = {k_: p_ for p_, k_ in newkey.items()}
_v_new = [int(pr_[pci['pitchattrib_fastball_velocity']]) for k_, pr_ in new_rows['pitcher.dat'] if level_of.get(_pid_of_key.get(k_)) == 1]
fb_base = 0.93 + 1.5 * (sum(_v_new) / len(_v_new) - sum(_v_old) / len(_v_old)) / 101 if _v_old and _v_new and not ERA else 0.93
if ERA and os.path.exists(os.path.join(DATA, 'era_runs.txt')):
    # An era's ratings are spread like 2005's, so its games score like 2005's (4.42 runs a club and game) unless the
    # fastball constant moves: about 5.5 runs per unit (0.9300 -> 0.9773 took 3.62 to 3.88). Clamped, since the
    # line is only measured near 2005's scoring.
    _rg = float(open(os.path.join(DATA, 'era_runs.txt')).read().split()[0])
    fb_base = max(0.75, min(1.12, 0.93 + (_rg - 4.42) / 5.5))
    print('era scoring %.2f runs a game: fastball_base %.4f' % (_rg, fb_base))
open(os.path.join(OUT, 'sim.txt'), 'w').write('fastball_base %.4f\n' % fb_base)
print('big-league fastballs: %.1f mph in 2005, %.1f now: fastball_base %.4f (sim.txt)' % (sum(_v_old) / len(_v_old), sum(_v_new) / len(_v_new), fb_base))
print('a dynasty with these rosters starts in %d (year.txt)' % FIRST)

# summary for checking
with open(os.path.join(OUT, 'rosters_summary.txt'), 'w', encoding='utf-8') as f:
    f.write('== free agents\n')
    for p in sorted(fa_new.values(), key=lambda p: (p not in pitc, -overall[p])):
        f.write('  %-24s %-3s overall %.0f, %d %s in the majors\n' % (people[p]['full'], prim[p], overall[p], _mlb_time(p), 'batters faced' if p in pitc else 'plate appearances'))
    for tk, org, lvl, n in slots:
        f.write('== %s %s (level %d)\n' % (TEAM[tk][1], TEAM[tk][2], lvl))
        for p in sorted(assign[tk], key=lambda p: (p not in pitc, -overall[p])):
            r = R[p]
            if p in pitc:
                (fbc, fbv), ex = repertoire(p)
                f.write('  %-24s %-3s %s  STA %2d  FB %d/%d  %s\n' % (people[p]['full'], prim[p], people[p]['thr'], r['stamina'], fbv, fbc,
                        ' '.join('%d:%d/%d/%d' % (e[0], e[4], e[1], e[3]) for e in ex)))
            else:
                f.write('  %-24s %-3s %s  CON %2d/%2d POW %2d/%2d SPD %2d DIS %2d FLD %2d RNG %2d ARM %2d\n' % (people[p]['full'], prim[p], people[p]['bat'],
                        r['lh_contact'], r['rh_contact'], r['lh_power'], r['rh_power'], r['speed'], r['platediscipline'], r['fielding'], r['range'], r['throwstrength']))
