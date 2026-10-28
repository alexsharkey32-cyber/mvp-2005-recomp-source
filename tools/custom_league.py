"""Your own league in one step: today's rosters with the clubs you made in the Team Maker.

    python3 tools/custom_league.py ISO [TEAMS_DIR = teams] [OUT = mods/pack-mine]

Every Team Maker export in TEAMS_DIR (team.zip files: a club's city, name, colours, logos, uniforms and ballpark,
each replacing the club of the slot chosen in the maker) goes into a copy of the modern rosters
(mods/rosters-2026), and the result is built as a league pack the game lists under Settings > Game > Rosters as
"My League" (the launcher's Eras page has a button for it). Players, faces, portraits and commentary names are the
modern rosters' own. Run it again after adding or changing a team.
"""
import sys, os, glob, json, shutil, subprocess
here = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, here)
import leaguepack, mvpdb

def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    iso = a[0]; teams = a[1] if len(a) > 1 else 'teams'; out = a[2] if len(a) > 2 else os.path.join('mods', 'pack-mine')
    zips = sorted(glob.glob(os.path.join(teams, '*.zip')))
    if not zips: print('NEED: put Team Maker exports (team .zip files) in the %s folder' % teams); sys.exit(1)
    base = os.path.join('mods', 'rosters-2026')
    season = int(open(os.path.join(base, 'year.txt')).read().split()[0]) if os.path.exists(os.path.join(base, 'year.txt')) else 2027
    src = os.path.join('build', 'custom_league')
    shutil.rmtree(src, ignore_errors=True)
    leaguepack.export(os.path.join(base, 'DATA', 'DATABASE', 'DATABASE.BIG'), src, 'My League', season)
    pj = os.path.join(src, 'pack.json'); meta = json.load(open(pj))
    meta.update(name='My League', description='Today\'s rosters with your own clubs (%d from the Team Maker)' % len(zips), author='tools/custom_league.py')
    json.dump(meta, open(pj, 'w'), indent=2)
    done = []
    for z in zips:
        try: leaguepack.add_team(src, z); done.append(os.path.basename(z))
        except Exception as e: print('problem: %s: %s' % (os.path.basename(z), e))
    shutil.rmtree(out, ignore_errors=True)
    probs = leaguepack.build(src, out, iso=iso)
    # the modern rosters' faces, portraits and commentary names come along (same players, same keys)
    for sub in ('textures', 'faces.txt', 'faces_params.txt', 'sim.txt'):
        s = os.path.join(base, sub)
        if os.path.isdir(s): shutil.copytree(s, os.path.join(out, sub), dirs_exist_ok=True)
        elif os.path.exists(s): shutil.copy(s, out)
    if os.path.isdir(os.path.join('mods', 'portraits-2026')):
        shutil.copytree(os.path.join('mods', 'portraits-2026'), os.path.join(out, 'portraits'), dirs_exist_ok=True)
    cm = os.path.join('mods', 'commentary-2026')
    if os.path.isdir(cm):
        shutil.copytree(cm, os.path.join(out, 'commentary'), dirs_exist_ok=True)
        T, order = mvpdb.read_database(os.path.join(out, 'DATA', 'DATABASE', 'DATABASE.BIG'))
        mvpdb.refresh_commentary(os.path.join(out, 'commentary', 'DATA', 'DATABASE', 'DATABASE.BIG'), T, order)
    subprocess.call([sys.executable, os.path.join(here, 'eras', 'check_pack.py'), out])
    print('teams: %s' % ', '.join(done))
    print('league pack: %s (choose "My League" under Settings > Game > Rosters)' % out)

if __name__ == '__main__': main()
