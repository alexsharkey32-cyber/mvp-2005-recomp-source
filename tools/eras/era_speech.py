"""What the commentators call each club of an era pack, from the recordings on the disc -> PACK/commentary.

    python3 tools/eras/era_speech.py ISO PACK_DIR

The commentators recorded each 2005 club 16 ways (tools/build_team_speech.py). In an era pack a slot's club may be
another one: the same nickname elsewhere (the Kansas City Athletics in Oakland's slot: they are called "the
Athletics", never "Oakland"), the same place under another name (the Cleveland Naps: "Cleveland"), or a different
club altogether (the Montreal Expos in the Nationals' slot, the Seattle Rainiers in the Mariners'): for those the
play-by-play and the stadium announcer skip the name, as they do for clubs they never recorded. Clubs named as in
2005 keep every recording. Replaces what tools/build_rosters.py put there for today's renamed clubs.
"""
import sys, os, shutil, tempfile
here = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, os.path.join(here, '..')); sys.path.insert(0, here)
import isofile, mvpdb, build_team_speech as bts
from bigfile import read_big
from era_logos import SAME_PLACE

def names(T):
    t = T['team.dat']; out = {}
    for k, v in t.rows:
        a = int(v[t.col('team_artid')])
        if 1 <= a <= 30: out[a] = (v[t.col('team_location')].strip(), v[t.col('team_long_name')].strip())
    return out

def same_place(a, b): return a == b or any(a in s and b in s for s in SAME_PLACE)
def same_nick(a, b):
    a, b = a.lower(), b.lower()
    return a == b or a.startswith(b + ' ') or b.startswith(a + ' ') or {a, b} == {'d-backs', 'diamondbacks'}

def main():
    iso, pack = sys.argv[1], sys.argv[2]
    tmp = tempfile.NamedTemporaryFile(suffix='.BIG', delete=False); tmp.write(isofile.read_file(iso, 'DATA/DATABASE/DATABASE.BIG')); tmp.close()
    disc = names(mvpdb.read_database(tmp.name)[0]); os.unlink(tmp.name)
    era = names(mvpdb.read_database(os.path.join(pack, 'DATA', 'DATABASE', 'DATABASE.BIG'))[0])
    sf = os.path.join(pack, 'standins.txt')                     # clubs named for minor-league clubs: only the very place
    standins = {int(l) for l in open(sf) if l.strip() and not l.startswith('#')} if os.path.exists(sf) else set()
    recipe, hide, log = {}, set(), []
    for a in sorted(era):
        (c1, n1), (c0, n0) = era[a], disc.get(a, ('', ''))
        sp, sn = same_place(c1, c0) or c0.lower() in (c1 + ' ' + n1).lower(), same_nick(n1, n0)
        if a - 1 in standins: sp = c1 == c0
        if sp and sn: continue
        if sn: recipe[a] = bts.PLACE_CHANGED; log.append('%2d %s %s: "the %s"' % (a, c1, n1, n0))
        elif sp: recipe[a] = bts.NICK_CHANGED; log.append('%2d %s %s: "%s"' % (a, c1, n1, c0))
        else: hide.add(a); log.append('%2d %s %s: name not said' % (a, c1, n1))
    out = os.path.join(pack, 'commentary')
    pbp = os.path.join(out, 'DATA', 'AUDIO', 'SPCH_PBP'); os.makedirs(pbp, exist_ok=True)
    for f in ('TNAMEHDR.BIG', 'TNAMEDAT.BIG'):
        p = os.path.join(pbp, f)
        if os.path.isdir(p): shutil.rmtree(p)
        elif os.path.exists(p): os.remove(p)
    hbig = isofile.read_file(iso, 'DATA/AUDIO/SPCH_PBP/TNAMEHDR.BIG'); dbig = isofile.read_file(iso, 'DATA/AUDIO/SPCH_PBP/TNAMEDAT.BIG')
    hm, dm = bts.members(hbig), bts.members(dbig)
    dd = os.path.join(pbp, 'TNAMEDAT.BIG'); os.makedirs(dd, exist_ok=True)
    files = []
    for n, o, s in read_big(hbig):                                   # the index, whole: remapped clubs, hidden clubs
        data = hbig[o:o + s]
        if n.endswith('.hdr') and n[:-4].isdigit() and len(n) == 6:
            tid = int(n[:2])
            if tid in recipe:
                data, dat = bts.remap(data, dm[n[:2] + '.dat'], recipe[tid])
                open(os.path.join(dd, n[:2] + '.dat'), 'wb').write(dat)
            if tid in hide: n = n[:-1] + 'x'
        files.append((n, data))
    open(os.path.join(pbp, 'TNAMEHDR.BIG'), 'wb').write(mvpdb.write_big(files))
    pa_dir = os.path.join(out, 'DATA', 'AUDIO', 'SPCH_PA'); os.makedirs(pa_dir, exist_ok=True)
    pa, n_pa = bts.hide_members(isofile.read_file(iso, 'DATA/AUDIO/SPCH_PA/TNAMEHDR.BIG'), {'%02d.hdr' % t for t in set(recipe) | hide})
    pp = os.path.join(pa_dir, 'TNAMEHDR.BIG')
    if os.path.isdir(pp): shutil.rmtree(pp)
    open(pp, 'wb').write(pa)
    open(os.path.join(pack, 'speech.txt'), 'w').write('# team names in the commentary (tools/eras/era_speech.py)\n' + '\n'.join(log) + '\n')
    print('%s: %d clubs re-voiced, %d not named, %d out of the announcer\'s index' % (pack, len(recipe), len(hide), n_pa))
    print('\n'.join(log))

if __name__ == '__main__': main()
