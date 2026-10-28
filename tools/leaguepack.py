"""League packs: a whole league (clubs, players, line-ups, managers, stats, art) as plain files you can edit in
a spreadsheet, and the tool that turns one into a mod the game loads.

    python3 tools/leaguepack.py export DATABASE.BIG PACKDIR [--name "..."] [--season 2026]
    python3 tools/leaguepack.py build PACKDIR [OUTDIR = mods/pack-<folder name>] [--iso your.iso (for the art)]
    python3 tools/leaguepack.py check PACKDIR          (build in memory, report problems)
    python3 tools/leaguepack.py add-team PACKDIR team.zip   (a club made in the Team Maker)

A pack folder:
  pack.json      name, season (the first season a dynasty plays), description, author, sim settings
  teams.csv      one row per club (the 30 franchises, their AAA/AA/A affiliates, the All-Star squads):
                 code (the game's 2-3 letter slot, e.g. Bos; fixed), city, name, abbr, league, division, ...,
                 affiliates and managers by code / manager id, team tendencies (0-2), first/second half record
  players.csv    one row per player: id (9 hex digits, any unique value), first, last, team (code; blank = free
                 agent), then every attribute the game keeps: appearance, fielding, running, contract, the split
                 batting ratings vs left-handers (vsL_...) and right-handers (vsR_...), and for pitchers pitch_...
  lineups.csv    the game's depth charts: per club, each player's role and batting order against right- and
                 left-handed starters, with and without the DH (SP1..SP5, RP.., C, 1B, ... ; -1 = not in the order)
  stats.csv      last season's numbers (shown on player cards, used by the game's ratings screens) and career totals
  managers.csv   managers (appearance and tendencies)
  art/           optional: logo / wordmark PNGs named in teams.csv (columns logo, cap_logo, wordmark, color1, color2)
  uniforms/      optional: <code>_<slot>.json uniform specs (tools/uniform_maker.py); a club with a logo and colours
                 and no spec gets home and road uniforms in its colours
  draft/         optional: draft/<year>.csv, the players clubs can draft after that season (players.csv columns,
                 without id and team): the game's made-up rookies of that class are replaced by them in order
  strings.txt    optional: text in the game's menus to replace, "old<TAB>new" per line (e.g. a club's name)
  overlay/       optional: files copied into the mod as they are (same layout as the disc: overlay/DATA/...)
Values are the game's own numbers (see docs/league_packs.md for what they mean and their ranges).
"""
import sys, os, csv, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mvpdb

PLAYER_TABLES = [   # table, column prefix in the database, prefix in players.csv / stats.csv, file
    ('attrib.dat', 'playerattrib_', '', 'players'),
    ('lhattrib.dat', 'lrattrib_', 'vsL_', 'players'),
    ('rhattrib.dat', 'lrattrib_', 'vsR_', 'players'),
    ('pitcher.dat', 'pitchattrib_', 'pitch_', 'players'),
    ('bstats.dat', 'batstat_', 'bat_', 'stats'),
    ('fbstats.dat', 'fieldstat_', 'field_', 'stats'),
    ('lhbstats.dat', 'lrbatstat_', 'batL_', 'stats'),
    ('rhbstats.dat', 'lrbatstat_', 'batR_', 'stats'),
    ('pstats.dat', 'pitchstat_', 'pit_', 'stats'),
    ('lhpstats.dat', 'lrpitchstat_', 'pitL_', 'stats'),
    ('rhpstats.dat', 'lrpitchstat_', 'pitR_', 'stats'),
    ('career.dat', 'batcareerstats_', 'careerbat_', 'stats'),
    ('careerp.dat', 'pitchcareer_', 'careerpit_', 'stats'),
]
TABLE_ORDER = ['attrib.dat', 'lhattrib.dat', 'rhattrib.dat', 'bstats.dat', 'fbstats.dat', 'lhbstats.dat', 'lhpstats.dat', 'pstats.dat',
               'rhbstats.dat', 'rhpstats.dat', 'pitcher.dat', 'team.dat', 'tstat.dat', 'roster.dat', 'career.dat', 'careerp.dat', 'org.dat', 'manager.dat']

import datetime
EPOCH = datetime.date(1947, 12, 31)
def to_date(days, shift):
    """the database's birthday (days from 1/1/1948 in the game's 2005-based calendar) -> real date"""
    d = EPOCH + datetime.timedelta(days=int(days))
    try: return d.replace(year=d.year + shift).isoformat()
    except ValueError: return d.replace(year=d.year + shift, day=28).isoformat()
def from_date(text, shift):
    text = text.strip()
    if text.lstrip('-').isdigit(): return text
    d = datetime.date.fromisoformat(text)
    try: d = d.replace(year=d.year - shift)
    except ValueError: d = d.replace(year=d.year - shift, day=28)
    return str(max(1, (d - EPOCH).days))
def short(col, prefix): return col[len(prefix):] if col.startswith(prefix) else col
def s32(v): v = int(v) & 0xffffffff; return v - (1 << 32) if v >= 1 << 31 else v

# ---------------------------------------------------------------- export: database -> pack
def export(db_path, pack, name=None, season=None):
    t, order = mvpdb.read_database(db_path)
    assert order == TABLE_ORDER, order
    os.makedirs(pack, exist_ok=True)
    team_code = {int(k, 16): v[0] for k, v in t['team.dat'].rows}
    mgr_id = {int(k, 16): k for k, v in t['manager.dat'].rows}
    # players
    roster_team = {}
    for k, v in t['roster.dat'].rows: roster_team.setdefault(v[1], team_code[int(v[0], 16)])
    files = {'players': {}, 'stats': {}}
    heads = {'players': ['id', 'first', 'last', 'team'], 'stats': ['id', 'first', 'last']}
    shift = (season or 2005) - 2005
    for tab, dbp, pp, fn in PLAYER_TABLES:
        T = t[tab]
        heads[fn] += [pp + short(c, dbp) for c in T.cols[2:]]
        for k, v in T.rows:
            row = files[fn].setdefault(k, {'id': k, 'first': v[0], 'last': v[1]})
            if fn == 'players': row['team'] = roster_team.get(k, '')
            assert row['first'] == v[0] and row['last'] == v[1], (tab, k)
            for c, x in zip(T.cols[2:], v[2:]): row[pp + short(c, dbp)] = to_date(x, shift) if c.endswith('_birthday') else x
    for fn in ('players', 'stats'):
        with open(os.path.join(pack, fn + '.csv'), 'w', newline='', encoding='latin1') as f:
            w = csv.DictWriter(f, heads[fn], restval=''); w.writeheader()
            for k, _ in t['attrib.dat'].rows:
                if k in files[fn]: w.writerow(files[fn][k])
    # teams: team.dat + tstat.dat + org.dat (affiliates and managers by code / id)
    TT, TS, OR = t['team.dat'], t['tstat.dat'], t['org.dat']
    ts, org = TS.by_key(), OR.by_key()
    th = ['id', 'code'] + [short(c, 'team_') for c in TT.cols[1:]] + [short(c, 'team_') for c in TS.cols[1:]] + \
         ['org_' + short(c, 'org_') for c in OR.cols[1:]]
    th = [h.replace('org_org_', 'org_') for h in th]
    with open(os.path.join(pack, 'teams.csv'), 'w', newline='', encoding='latin1') as f:
        w = csv.writer(f); w.writerow(th)
        for k, v in TT.rows:
            o = org.get(k)
            orow = [''] * (len(OR.cols) - 1)
            if o is not None:
                assert o[0] == v[0]
                orow = list(o[1:])
                for i, c in enumerate(OR.cols[1:]):
                    if c.startswith('org_team_') and int(orow[i]) in team_code: orow[i] = team_code[int(orow[i])]
                    elif c.endswith('_manager') and (int(orow[i]) & 0xffffffff) in mgr_id: orow[i] = 'm:' + mgr_id[int(orow[i]) & 0xffffffff]
            s = ts[k]; assert s[0] == v[0]
            w.writerow([k] + v + s[1:] + orow)
    # line-ups
    R = t['roster.dat']
    with open(os.path.join(pack, 'lineups.csv'), 'w', newline='', encoding='latin1') as f:
        w = csv.writer(f); w.writerow(['team', 'player'] + [short(c, 'rh_roster_').replace('lh_roster_', 'L_') if c.startswith('lh_') else 'R_' + short(c, 'rh_roster_') for c in R.cols[2:]])
        for k, v in R.rows: w.writerow([team_code[int(v[0], 16)], v[1]] + v[2:])
    M = t['manager.dat']
    with open(os.path.join(pack, 'managers.csv'), 'w', newline='', encoding='latin1') as f:
        w = csv.writer(f); w.writerow(['id', 'first', 'last'] + [short(c, 'manager_') for c in M.cols[2:]])
        bc = M.cols.index('manager_birthday')
        for k, v in M.rows: w.writerow([k] + [to_date(x, shift) if i == bc else x for i, x in enumerate(v)])
    meta = {'format': 1, 'name': name or os.path.basename(os.path.normpath(pack)), 'season': season or 2005,
            'description': '', 'author': '', 'sim': {}}
    mp = os.path.join(pack, 'pack.json')
    if os.path.exists(mp): meta.update(json.load(open(mp)))
    if season: meta['season'] = season
    json.dump(meta, open(mp, 'w'), indent=2)
    print('exported %d players, %d clubs, %d managers to %s' % (len(t['attrib.dat'].rows), len(TT.rows), len(M.rows), pack))

# ---------------------------------------------------------------- build: pack -> database
class PackError(Exception): pass

def read_csv(pack, fn):
    p = os.path.join(pack, fn)
    if not os.path.exists(p): raise PackError('%s is missing' % fn)
    with open(p, newline='', encoding='latin1') as f: return list(csv.DictReader(f))

def to_db(pack, template_db):
    """the pack as database tables (template_db: a database with the game's column layout, e.g. the disc's)"""
    tpl, order = mvpdb.read_database(template_db)
    probs = []
    teams = read_csv(pack, 'teams.csv'); players = read_csv(pack, 'players.csv')
    stats = {r['id']: r for r in read_csv(pack, 'stats.csv')} if os.path.exists(os.path.join(pack, 'stats.csv')) else {}
    lineups = read_csv(pack, 'lineups.csv'); managers = read_csv(pack, 'managers.csv')
    meta = json.load(open(os.path.join(pack, 'pack.json')))
    shift = int(meta.get('season', 2005)) - 2005
    code_key = {r['code']: r['id'] for r in teams}
    if len(code_key) != len(teams): probs.append('teams.csv: a club code is used twice')
    T = {}
    def new(tab): T[tab] = mvpdb.Table(tab, tpl[tab].header + '\r\n'); return T[tab]
    # players
    ids = set()
    for tab, dbp, pp, fn in PLAYER_TABLES:
        tt = new(tab); cols = tpl[tab].cols
        src = players if fn == 'players' else None
        for p in players:
            pid = p['id'].strip().lower()
            if tab == 'attrib.dat':
                if len(pid) != 9 or any(c not in '0123456789abcdef' for c in pid): probs.append('players.csv: id %r is not 9 hex digits' % pid)
                if pid in ids: probs.append('players.csv: id %s used twice' % pid)
                ids.add(pid)
            row = p if fn == 'players' else stats.get(p['id'])
            if row is None: continue
            vals = [p['first'], p['last']]
            has = False
            for c in cols[2:]:
                x = row.get(pp + short(c, dbp), '')
                if x is None: x = ''
                x = x.strip(); has |= x != ''
                if c.endswith('_birthday') and x:
                    try: x = from_date(x, shift)
                    except ValueError: probs.append('%s: birthday %r is not YYYY-MM-DD' % (pid, x))
                vals.append(x)
            if not has and tab not in ('attrib.dat', 'lhattrib.dat', 'rhattrib.dat'): continue   # not a pitcher / no stats
            missing = [cols[2 + i] for i, x in enumerate(vals[2:]) if x == '']
            if missing: probs.append('%s %s %s: no %s' % (pid, p['first'], p['last'], ', '.join(pp + short(c, dbp) for c in missing[:4])))
            tt.rows.append([pid, vals])
    # clubs
    TT, TS, OR = new('team.dat'), new('tstat.dat'), new('org.dat')
    mgr_key = {r['id']: r['id'] for r in managers}
    for r in teams:
        k = r['id']
        TT.rows.append([k, [r['code']] + [r[short(c, 'team_')] for c in tpl['team.dat'].cols[1:]]])
        TS.rows.append([k, [r['code']] + [r[short(c, 'team_')] for c in tpl['tstat.dat'].cols[1:]]])
        oc = tpl['org.dat'].cols[1:]
        ov = [r.get(('org_' + short(c, 'org_')), '') for c in oc]
        if any(ov):
            for i, c in enumerate(oc):
                x = ov[i]
                if x in code_key: x = str(int(code_key[x], 16))
                elif x.startswith('m:'):
                    if x[2:] not in mgr_key: probs.append('teams.csv %s: no manager %s' % (r['code'], x[2:]))
                    x = str(s32(int(x[2:], 16)))
                ov[i] = x
            OR.rows.append([k, [r['code']] + ov])
    RR = new('roster.dat')
    for i, r in enumerate(lineups):
        if r['team'] not in code_key: probs.append('lineups.csv: no club %s' % r['team']); continue
        if r['player'] not in ids: probs.append('lineups.csv: no player %s' % r['player'])
        vals = [code_key[r['team']], r['player']]
        for c in tpl['roster.dat'].cols[2:]:
            h = ('L_' + c[len('lh_roster_'):]) if c.startswith('lh_') else 'R_' + c[len('rh_roster_'):]
            vals.append(r[h])
        RR.rows.append(['%09x' % i, vals])
    MM = new('manager.dat')
    for r in managers: MM.rows.append([r['id'], [r['first'], r['last']] + [from_date(r[short(c, 'manager_')], shift) if c == 'manager_birthday' else r[short(c, 'manager_')] for c in tpl['manager.dat'].cols[2:]]])
    return T, order, probs

def build(pack, out=None, template_db=None, write=True, iso=None):
    template_db = template_db or os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'mods', 'rosters-2026', 'DATA', 'DATABASE', 'DATABASE.BIG')
    T, order, probs = to_db(pack, template_db)
    meta = json.load(open(os.path.join(pack, 'pack.json')))
    for p in probs[:40]: print('problem:', p)
    if len(probs) > 40: print('... %d problems in all' % len(probs))
    if not write: return probs
    out = out or os.path.join('mods', 'pack-' + os.path.basename(os.path.normpath(pack)))
    d = os.path.join(out, 'DATA', 'DATABASE'); os.makedirs(d, exist_ok=True)
    mvpdb.write_database(os.path.join(d, 'DATABASE.BIG'), T, order)
    open(os.path.join(out, 'year.txt'), 'w').write('%d\n' % int(meta.get('season', 2005)))
    sim = meta.get('sim', {})
    if sim: open(os.path.join(out, 'sim.txt'), 'w').write(''.join('%s %s\n' % kv for kv in sim.items()))
    json.dump(meta, open(os.path.join(out, 'pack.json'), 'w'), indent=2)
    if os.path.exists(os.path.join(pack, 'strings.txt')): shutil.copy(os.path.join(pack, 'strings.txt'), out)
    if os.path.isdir(os.path.join(pack, 'draft')):            # draft classes: draft/<year>.csv, the columns of players.csv
        shutil.copytree(os.path.join(pack, 'draft'), os.path.join(out, 'draft'), dirs_exist_ok=True)
    ov = os.path.join(pack, 'overlay')
    if os.path.isdir(ov): shutil.copytree(ov, out, dirs_exist_ok=True)
    if iso: build_art(pack, out, iso); build_parks(pack, out, iso, read_csv(pack, 'teams.csv'))
    print('built %s: %d players, %d clubs -> %s' % (meta.get('name'), len(T['attrib.dat'].rows), len(T['team.dat'].rows), out))
    return probs

# ---------------------------------------------------------------- art: logos
# LOGOS.BIG l<artid - 1>.ssh, per club: a (primary logo 128x64), i (logo 64x64), j (tiny logo 32x16, 32-bit),
# g (cap logo 64x64), l (wordmark 128x64), d and e (colour bars 8x32, 32-bit). teams.csv may name, per club,
# `logo`, `cap_logo`, `wordmark` (PNG files in the pack, with transparency) and `color1`, `color2` (#rrggbb).
def fit(img, w, h, margin=0.06):
    """an RGBA image scaled into w x h (kept in proportion, centred) -> uint8 (h, w, 4), PS2 alpha"""
    from PIL import Image
    import numpy as np
    im = img.convert('RGBA'); bb = im.getbbox()
    if bb: im = im.crop(bb)
    k = min(w * (1 - 2 * margin) / im.width, h * (1 - 2 * margin) / im.height)
    nw, nh = max(1, round(im.width * k)), max(1, round(im.height * k))
    # premultiplied resize
    a = np.asarray(im).astype(np.float32) / 255.0
    pm = Image.fromarray((np.dstack([a[..., :3] * a[..., 3:], a[..., 3:]]) * 255).astype(np.uint8), 'RGBA').resize((nw, nh), Image.LANCZOS)
    c = Image.new('RGBA', (w, h), (0, 0, 0, 0)); c.paste(pm, ((w - nw) // 2, (h - nh) // 2))
    q = np.asarray(c).astype(np.float32) / 255.0
    al = q[..., 3:]; rgb = np.where(al > 0.004, q[..., :3] / np.maximum(al, 1e-3), 0)
    return np.dstack([np.clip(rgb * 255, 0, 255), np.round(al * 128)]).astype(np.uint8)

def hexcol(t):
    t = (t or '').strip().lstrip('#')
    return (int(t[0:2], 16), int(t[2:4], 16), int(t[4:6], 16)) if len(t) == 6 else None

def build_art(pack, out, iso):
    """logos of the clubs that bring their own -> OUT/art/DATA/FRONTEND/LOGOS.BIG/ (loaded after the other mods)"""
    import numpy as np
    from PIL import Image
    import isofile, refpack, ssh
    from bigfile import read_big
    teams = read_csv(pack, 'teams.csv')
    want = [r for r in teams if any((r.get(k) or '').strip() for k in ('logo', 'cap_logo', 'wordmark', 'color1', 'color2'))]
    if not want: return 0
    big = isofile.read_file(iso, 'DATA/FRONTEND/LOGOS.BIG'); ents = {n.lower(): (o, s) for n, o, s in read_big(big)}
    d = os.path.join(out, 'art', 'DATA', 'FRONTEND', 'LOGOS.BIG'); os.makedirs(d, exist_ok=True)
    n = 0
    for r in want:
        idx = int(r['artid']) - 1; name = 'l%03d.ssh' % idx
        if name not in ents: print('problem: %s: no logo file %s on the disc' % (r['code'], name)); continue
        o, s = ents[name]; c = big[o:o + s]; c = refpack.decompress(c) if c[1:2] == b'\xfb' else c
        f = ssh.Ssh(c)
        def img(col):
            p = (r.get(col) or '').strip()
            return Image.open(os.path.join(pack, p)) if p else None
        logo, cap, word = img('logo'), img('cap_logo'), img('wordmark')
        cap = cap or logo; word = word or logo
        for sh in f.shapes:
            kind = sh.name[0]; w, h = sh.size
            src = {'a': logo, 'i': logo, 'j': logo, 'g': cap, 'l': word}.get(kind)
            if src is not None:
                px = fit(src, w, h)
                if sh.type == 14: data, pal = ssh.image14_photo(px); sh.set_pixel_bytes(data); sh.set_palette(pal)
                elif sh.type == 5: sh.set_pixel_bytes(px.tobytes())
            col = hexcol(r.get('color1')) if kind == 'e' else hexcol(r.get('color2')) if kind == 'd' else None
            if col and sh.type == 5:
                px = np.frombuffer(sh.pixel_bytes(), np.uint8).reshape(h, w, 4).copy(); px[..., :3] = col; sh.set_pixel_bytes(px.tobytes())
        open(os.path.join(d, name), 'wb').write(refpack.compress(f.build())); n += 1
    print('logos for %d clubs' % n)
    build_uniforms(pack, out, iso, teams)
    return n

def build_uniforms(pack, out, iso, teams):
    """uniforms/<code>_<slot>.json (tools/uniform_maker.py specs, paths relative to the pack); clubs with a logo and
    colours but no spec get a home (a) and road (b) set in their colours"""
    import glob, isofile, refpack, ssh, uniform_maker
    from bigfile import read_big
    specs = {}
    for r in teams:
        c1, c2 = (r.get('color1') or '').strip(), (r.get('color2') or '').strip()
        if (r.get('logo') or '').strip() and c1:
            base = dict(trim=c1, undershirt=c1, crown=c1, button=c2 or c1, numbers=c1, numbers_outline=c2 or None,
                        wordmark=r.get('wordmark') or r.get('logo'), cap_logo=r.get('cap_logo') or r.get('logo'))
            specs[(r['code'], 'a')] = dict(base, cloth='#f1efe9')
            specs[(r['code'], 'b')] = dict(base, cloth='#a9abae')
    for p in sorted(glob.glob(os.path.join(pack, 'uniforms', '*.json'))):
        code, _, slot = os.path.basename(p)[:-5].rpartition('_')
        specs[(code, slot)] = json.load(open(p))
    if not specs: return 0
    by_code = {r['code']: r for r in teams}
    models = isofile.read_file(iso, 'DATA/MODELS.BIG'); me = {n.lower(): (o, s) for n, o, s in read_big(models)}
    prevs = isofile.read_file(iso, 'DATA/FRONTEND/UNIFORMS.BIG'); pe = {n.lower(): (o, s) for n, o, s in read_big(prevs)}
    def get(big, ents, n):
        o, s = ents[n]; c = big[o:o + s]; return refpack.decompress(c) if c[1:2] == b'\xfb' else c
    dm = os.path.join(out, 'art', 'DATA', 'MODELS.BIG'); dp = os.path.join(out, 'art', 'DATA', 'FRONTEND', 'UNIFORMS.BIG')
    os.makedirs(dm, exist_ok=True); os.makedirs(dp, exist_ok=True)
    pfiles = {}
    for (code, slot), spec in sorted(specs.items()):
        if code not in by_code: print('problem: uniforms for unknown club %s' % code); continue
        team = int(by_code[code]['artid']) - 1
        spec = dict(spec)
        for k in ('wordmark', 'cap_logo', 'helmet_logo'):
            if spec.get(k): spec[k] = os.path.join(pack, spec[k])
        name = 'u%03d%s.ssh' % (team, slot); pat = spec.get('pattern', name).lower()
        if pat not in me: print('problem: %s %s: no uniform %s on the disc' % (code, slot, pat)); continue
        fname = 'f%03d%s.ssh' % (team, slot)
        pn = '%03d.ssh' % team
        if pn in pe and pn not in pfiles: pfiles[pn] = ssh.Ssh(get(prevs, pe, pn))
        prev = None
        if pn in pfiles:
            for sh in pfiles[pn].shapes:
                if sh.name == '%03d%s' % (team, slot): prev = sh
        u, fo = uniform_maker.make(get(models, me, pat), spec, get(models, me, fname) if fname in me else None, prev)
        open(os.path.join(dm, name), 'wb').write(refpack.compress(u))
        if fo: open(os.path.join(dm, fname), 'wb').write(refpack.compress(fo))
        print('uniform %s %s' % (code, slot), flush=True)
    for pn, f in pfiles.items(): open(os.path.join(dp, pn), 'wb').write(refpack.compress(f.build()))
    return len(specs)

# ---------------------------------------------------------------- ballparks
# teams.csv may give a club's park: `park_name` (shown in the menus), `park_base` (the code of another park on the
# disc to play in: TIGR Tiger Stadium, POLO Polo Grounds, FORB Forbes Field, SHIB Shibe Park, CROS Crosley Field,
# GRIF Griffith Stadium, SPOT Sportsman's Park, ASTR Astrodome, VETE Veterans Stadium, CINE Riverfront, CLEV
# Cleveland Stadium, METP Metropolitan Stadium, OLYM Olympic Stadium, QUAL Jack Murphy, FENC old Fenway, or any of
# the 2005 parks' codes) and `park_fences` (posted distances in feet: "left line-left centre-centre-right centre-
# right line", e.g. 330-375-400-375-330). The outfield is reshaped to those distances (tools/build_stadiums.py).
PARK_OF = ['ANAH', 'NETW', 'SAFE', 'ARLI', 'USCE', 'JACO', 'COME', 'KAUF', 'METR', 'ORIO', 'FENW', 'YANK', 'TROP', 'SKYD', 'BANK',
           'COOR', 'DODG', 'PETO', 'SBCP', 'WRIG', 'GABA', 'MINU', 'MILL', 'PNCP', 'BUSH', 'TURN', 'PROP', 'RFKS', 'SHEA', 'CITI']
PARK_NAME = ['Angel Stadium of Anaheim', 'Network Associates Coliseum™', 'SAFECO Field™', 'Ameriquest Field in Arlington™', 'U.S. Cellular Field™',
             'Jacobs Field™', 'Comerica Park™', 'Kauffman Stadium™', 'Hubert H. Humphrey Metrodome', 'Oriole Park™', 'Fenway Park™',
             'Yankee Stadium™', 'Tropicana Field™', 'SkyDome™', 'Bank One Ballpark™', 'Coors Field™', 'Dodger Stadium™', 'PETCO Park™',
             'SBC Park™', 'Wrigley Field™', 'Great American Ball Park™', 'Minute Maid Park™', 'Miller Park™', 'PNC Park™', 'Busch Stadium™',
             'Turner Field™', 'Pro Player Stadium™', 'RFK Stadium', 'Shea Stadium', 'Citizens Bank Park™']
def park_files(iso):
    import isofile, struct
    with open(iso, 'rb') as f:
        f.seek(16 * 2048); pvd = f.read(2048); root = pvd[156:190]; files = {}
        isofile._walk(f, struct.unpack('<I', root[2:6])[0], struct.unpack('<I', root[10:14])[0], '', files)
    return sorted(p for p in files if p.startswith('DATA/STADIUM/') and p.endswith('.BIG'))
def build_parks(pack, out, iso, teams):
    import isofile, refpack, mvpdb
    import build_stadiums as bs
    from bigfile import read_big
    want = [r for r in teams if any((r.get(k) or '').strip() for k in ('park_name', 'park_base', 'park_fences'))]
    if not want: return 0
    bs.ISO = iso; files = park_files(iso); names = []
    for r in want:
        club = int(r['artid']) - 1
        if not 0 <= club < 30: print('problem: %s: only the 30 big-league clubs have parks' % r['code']); continue
        own = PARK_OF[club]; base = (r.get('park_base') or '').strip().upper() or own
        src = [p for p in files if p.startswith('DATA/STADIUM/' + base)]; dst = [p for p in files if p.startswith('DATA/STADIUM/' + own)]
        if not src: print('problem: %s: no park %s on the disc' % (r['code'], base)); continue
        spath = src[0]
        fences = [float(v) for v in (r.get('park_fences') or '').replace(' ', '').split('-') if v]
        big = isofile.read_file(iso, spath); members = [(n, big[o:o + s]) for n, o, s in read_big(big)]
        if len(fences) == 5:
            def get(name):
                c = dict(members)[name]; return refpack.decompress(c) if c[:2] in (b'\x10\xfb', b'\x11\xfb', b'\x90\xfb', b'\x91\xfb') else c
            prof = bs.fence_profile(bs.wall_segments(get('wall.dat').decode('latin1')))
            deltas = tuple(fences[i] - prof[a] / 12.0 for i, a in enumerate((1, 22, 45, 68, 89)))
            changed, before, after, moved, _ = bs.build_park(spath, deltas)
            pick = lambda pr: '-'.join('%d' % round(pr[a] / 12) for a in (1, 22, 45, 68, 89))
            print('park %s: %s %s -> %s ft' % (r['code'], base, pick(before), pick(after)))
            members = [(n, refpack.compress(changed[n]) if n in changed else b) for n, b in members]
        data = mvpdb.write_big(members)
        d = os.path.join(out, 'art', 'DATA', 'STADIUM'); os.makedirs(d, exist_ok=True)
        for p in dst: open(os.path.join(d, p.split('/')[-1]), 'wb').write(data)
        if (r.get('park_name') or '').strip(): names.append((PARK_NAME[club].replace('\\xa0', '\xa0'), r['park_name'].strip()))
    if names:
        sp = os.path.join(out, 'strings.txt')
        lines = open(sp, encoding='utf-8').read().splitlines() if os.path.exists(sp) else []
        lines = [l for l in lines if not any(l.startswith(a + '\t') for a, _ in names)] + ['%s\t%s' % nb for nb in names]
        open(sp, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    return len(want)

# ---------------------------------------------------------------- a club from the Team Maker
def add_team(pack, zpath):
    """merge a Team Maker export (team.zip: team.json + PNGs) into a pack: the club named by its slot code gets the
    new city, name, abbreviation, colours, logos and uniforms; strings.txt learns the old name -> the new one"""
    import zipfile
    z = zipfile.ZipFile(zpath); t = json.loads(z.read('team.json'))
    code = t['slot']
    rows = read_csv(pack, 'teams.csv')
    row = next((r for r in rows if r['code'] == code), None)
    if row is None: raise PackError('teams.csv has no club %s' % code)
    art = os.path.join(pack, 'art', code); os.makedirs(art, exist_ok=True)
    files = {}
    for n in z.namelist():
        if n.endswith('.png'):
            dst = os.path.join(art, os.path.basename(n)); open(dst, 'wb').write(z.read(n)); files[os.path.basename(n)] = os.path.relpath(dst, pack)
    old = (row['location'], row['long_name'])
    row.update(location=t['city'], long_name=t['name'], short_name=t['abbr'],
               logo=files.get('logo.png', ''), cap_logo=files.get('cap_logo.png', ''), wordmark=files.get('wordmark.png', ''),
               color1=t['colors'][0], color2=t['colors'][1])
    park = t.get('park') or {}
    if park:
        row.update(park_name=park.get('name', ''), park_base=park.get('base', ''),
                   park_fences='-'.join(str(int(v)) for v in park.get('fences', [])) if park.get('fences') else '')
    head = list(rows[0].keys())
    for k in ('logo', 'cap_logo', 'wordmark', 'color1', 'color2', 'park_name', 'park_base', 'park_fences'):
        if k not in head: head.append(k)
    with open(os.path.join(pack, 'teams.csv'), 'w', newline='', encoding='latin1') as f:
        w = csv.DictWriter(f, head, restval=''); w.writeheader(); w.writerows(rows)
    ud = os.path.join(pack, 'uniforms'); os.makedirs(ud, exist_ok=True)
    for slot, spec in (t.get('uniforms') or {}).items():
        spec = dict(spec)
        for k in ('wordmark', 'cap_logo', 'helmet_logo'):
            if spec.get(k): spec[k] = files.get(os.path.basename(spec[k]), '')
        json.dump(spec, open(os.path.join(ud, '%s_%s.json' % (code, slot)), 'w'), indent=2)
    sp = os.path.join(pack, 'strings.txt')
    lines = open(sp, encoding='utf-8').read().splitlines() if os.path.exists(sp) else []
    for a, b in (('%s %s' % old, '%s %s' % (t['city'], t['name'])), (old[1], t['name'])):
        if a != b and not any(l.startswith(a + '\t') for l in lines): lines.append('%s\t%s' % (a, b))
    open(sp, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    print('%s: %s %s (%s) with %d pictures and %d uniforms' % (code, t['city'], t['name'], t['abbr'], len(files), len(t.get('uniforms') or {})))

if __name__ == '__main__':
    a, skip = [], False
    for x in sys.argv[1:]:
        if skip: skip = False
        elif x in ('--name', '--season', '--iso'): skip = True
        elif not x.startswith('--'): a.append(x)
    opt = lambda k: sys.argv[sys.argv.index(k) + 1] if k in sys.argv else None
    if a[0] == 'export': export(a[1], a[2], opt('--name'), int(opt('--season')) if opt('--season') else None)
    elif a[0] == 'build': sys.exit(1 if build(a[1], a[2] if len(a) > 2 else None, iso=opt('--iso')) and '--strict' in sys.argv else 0)
    elif a[0] == 'add-team': add_team(a[1], a[2])
    elif a[0] == 'check': sys.exit(1 if build(a[1], write=False) else 0)
    else: print(__doc__)
