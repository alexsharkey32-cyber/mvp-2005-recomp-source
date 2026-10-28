"""Logos for the names a club takes during a dynasty -> mods/era-logos/ (shared by every era pack).

    python3 tools/eras/era_logo_years.py ISO TEAM_YEARS [OUT_DIR]

TEAM_YEARS is a pack's team_years.txt (tools/eras/era_teamnames.py; the same table in every pack). For every name a
club slot carries in a season of the table that is not the 2005 club's own (the Milwaukee Braves, the Kansas City
Athletics, the Colt .45's, the Montreal Expos ...), tools/eras/era_logos.py's plain lettering in the colours of the
uniform the club wore that season (tools/eras/era_uniforms.py's table). Written as LOGOS.BIG members
l<slot>_<first season>.ssh plus logos.txt ("art file city|nickname"); the runtime puts the right one in when a
dynasty's season changes. Stand-in names (minor-league clubs) keep the pack's own logo and are left out.
"""
import sys, os, tempfile, shutil
here = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, here); sys.path.insert(0, os.path.join(here, '..'))
import numpy as np
import isofile, refpack, ssh, mvpdb
from bigfile import read_big
import era_logos as EL
from era_uniforms import ERA

def main():
    iso, ty = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(ty))), 'era-logos')
    os.makedirs(out, exist_ok=True)
    tmp = tempfile.NamedTemporaryFile(suffix='.BIG', delete=False); tmp.write(isofile.read_file(iso, 'DATA/DATABASE/DATABASE.BIG')); tmp.close()
    D, _ = mvpdb.read_database(tmp.name); os.unlink(tmp.name)
    t = D['team.dat']; disc = {}
    for k, v in t.rows:
        a = int(v[t.col('team_artid')])
        if 1 <= a <= 30: disc[a] = (v[t.col('team_location')], v[t.col('team_long_name')])
    models = isofile.read_file(iso, 'DATA/MODELS.BIG'); me = {n.lower(): (o, s) for n, o, s in read_big(models)}
    big = isofile.read_file(iso, 'DATA/FRONTEND/LOGOS.BIG'); ents = {n.lower(): (o, s) for n, o, s in read_big(big)}
    phases = {}                                             # (art, city, nick) -> [first, last]
    for line in open(ty, encoding='utf-8'):
        if line.startswith('#') or not line.strip(): continue
        art, lo, hi, rest = line.rstrip('\n').split(' ', 3)
        f = rest.split('|')
        if len(f) > 3 and f[3] == 'S': continue
        key = (int(art), f[1], f[2]); lo, hi = int(lo), int(hi)
        if key in phases: phases[key][1] = max(phases[key][1], hi); phases[key][0] = min(phases[key][0], lo)
        else: phases[key] = [lo, hi]
    root = os.path.dirname(os.path.abspath(out)); modern = {}         # names as today's: today's logos (mods/uniforms-2026)
    mdb = os.path.join(root, 'rosters-2026', 'DATA', 'DATABASE', 'DATABASE.BIG')
    if os.path.exists(mdb):
        M = mvpdb.read_database(mdb)[0]['team.dat']
        for k, v in M.rows:
            a = int(v[M.col('team_artid')])
            if 1 <= a <= 30: modern[a] = (v[M.col('team_location')], v[M.col('team_long_name')])
    index = ['# art file city|nickname (tools/eras/era_logo_years.py)']
    for (art, city, nick), (lo, hi) in sorted(phases.items(), key=lambda x: (x[0][0], x[1][0])):
        if not EL.differs((city, nick), disc.get(art)): continue
        club = art - 1
        today = os.path.join(root, 'uniforms-2026', 'DATA', 'FRONTEND', 'LOGOS.BIG', 'l%03d.ssh' % club)
        if modern.get(art) == (city, nick) and os.path.exists(today):
            fn = 'l%03d_%d.ssh' % (club, lo); shutil.copy(today, os.path.join(out, fn))
            index.append('%d %s %s|%s' % (art, fn, city, nick)); print(art, lo, hi, city, nick, "today's logo"); continue
        slot = next((h for a, b, h, r in ERA.get(club, []) if a <= lo <= b), 'a')          # the home set of its first season
        fake = tempfile.mkdtemp(); dm = os.path.join(fake, 'DATA', 'MODELS.BIG'); os.makedirs(dm)
        n = 'u%03d%s.ssh' % (club, slot); o, s = me[n]; open(os.path.join(dm, 'u%03da.ssh' % club), 'wb').write(models[o:o + s])
        main_c, trim = EL.colours(iso, fake, club); shutil.rmtree(fake)
        pics = EL.make(city, nick, lo, main_c, trim)
        name = 'l%03d.ssh' % club; o, s = ents[name]; f = ssh.Ssh(EL.unpack(big[o:o + s]))
        for sh in f.shapes:
            kind = sh.name[0]; w, h = sh.size
            if kind in pics:
                px = np.asarray(EL.fit(pics[kind], w, h)).astype(np.float32)
                px = np.dstack([px[..., :3], np.round(px[..., 3:] * 128 / 255)]).astype(np.uint8)
                if sh.type == 14: data, pal = ssh.image14_photo(px); sh.set_pixel_bytes(data); sh.set_palette(pal)
                elif sh.type == 5: sh.set_pixel_bytes(px.tobytes())
            elif kind in 'de' and sh.type == 5:
                px = np.frombuffer(sh.pixel_bytes(), np.uint8).reshape(h, w, 4).copy(); px[..., :3] = main_c if kind == 'e' else trim
                sh.set_pixel_bytes(px.tobytes())
        fn = 'l%03d_%d.ssh' % (club, lo)
        open(os.path.join(out, fn), 'wb').write(refpack.compress(f.build()))
        index.append('%d %s %s|%s' % (art, fn, city, nick))
        print(art, lo, hi, city, nick, main_c, trim)
    open(os.path.join(out, 'logos.txt'), 'w').write('\n'.join(index) + '\n')

if __name__ == '__main__': main()
