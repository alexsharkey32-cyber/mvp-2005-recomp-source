"""A historical league pack from the Lahman Baseball Database, in one go:

    python3 tools/eras/build_era.py ISO LAHMAN_DIR SEASON [OUT = mods/pack-<SEASON+1>]

SEASON is the last season played: the pack's dynasty starts the season after (1976 -> the 1977 season), with the
clubs, players, managers and farm systems of that winter (tools/eras/lahman_convert.py, tools/build_rosters.py).
Pick it under Settings > Game > Rosters.
"""
import sys, os, subprocess, json, shutil
here = os.path.dirname(os.path.abspath(__file__)); tools = os.path.dirname(here)
iso, lahman, season = sys.argv[1], sys.argv[2], int(sys.argv[3])
import importlib.util
_missing = [p for m, p in (('numpy', 'numpy'), ('PIL', 'pillow'), ('cv2', 'opencv-python')) if importlib.util.find_spec(m) is None]
if _missing:                                               # (the launcher shows this line)
    print('NEED: python3 -m pip install %s' % ' '.join(_missing)); sys.exit(1)
if lahman.endswith('.zip'):                                # the database as downloaded: unpacked once into build/
    import zipfile
    dst = os.path.join('build', 'lahman')
    if not os.path.exists(os.path.join(dst, 'People.csv')):
        z = zipfile.ZipFile(lahman); os.makedirs(dst, exist_ok=True)
        for n in z.namelist():
            if n.endswith('.csv'): open(os.path.join(dst, os.path.basename(n)), 'wb').write(z.read(n))
    lahman = dst
out = sys.argv[4] if len(sys.argv) > 4 else os.path.join('mods', 'pack-%d' % (season + 1))
data = os.path.join('build', 'era-data-%d' % season)
subprocess.check_call([sys.executable, os.path.join(here, 'lahman_convert.py'), lahman, str(season), data, '--iso', iso])
subprocess.check_call([sys.executable, os.path.join(tools, 'build_rosters.py'), iso, data, out, str(season)])
first = int(open(os.path.join(out, 'year.txt')).read().split()[0])
json.dump({'format': 1, 'name': 'MLB %d' % first, 'season': first, 'description': 'The major leagues as they were after the %d season (Lahman Baseball Database)' % season,
           'author': 'tools/eras/build_era.py', 'sim': {}}, open(os.path.join(out, 'pack.json'), 'w'), indent=2)
subprocess.check_call([sys.executable, os.path.join(here, 'era_uniforms.py'), iso, str(first), out])   # the clubs' uniforms of that season
subprocess.check_call([sys.executable, os.path.join(here, 'draft_potential.py'), lahman, out])         # draft classes ranked by the careers that followed
subprocess.check_call([sys.executable, os.path.join(here, 'era_money.py'), out, str(first)])          # salaries at that season's money
subprocess.check_call([sys.executable, os.path.join(here, 'era_parks.py'), iso, str(first), out])      # the old ballparks the disc has
subprocess.check_call([sys.executable, os.path.join(here, 'fix_rosters.py'), out])                       # every club within the game's roster rules
subprocess.check_call([sys.executable, os.path.join(here, 'hr_factor.py'), out, lahman])                  # the sluggers as far above the league as they were
subprocess.check_call([sys.executable, os.path.join(here, 'era_names.py'), out])                         # clubs still to come: minor-league clubs of the day
subprocess.check_call([sys.executable, os.path.join(here, 'era_logos.py'), iso, out])                  # plain logos for clubs not the 2005 one of their slot
subprocess.check_call([sys.executable, os.path.join(here, 'era_speech.py'), iso, out])                 # what the commentators call each club
subprocess.check_call([sys.executable, os.path.join(here, 'era_teamnames.py'), lahman, out])          # club names through the years (moves, renames)
if not os.path.exists(os.path.join(os.path.dirname(os.path.abspath(out)), 'era-logos', 'logos.txt')):    # their logos (shared by every pack)
    subprocess.check_call([sys.executable, os.path.join(here, 'era_logo_years.py'), iso, os.path.join(out, 'team_years.txt')])
def season_games(y):                                                                                     # a club's season that year (the median: ties replayed, rain-outs not)
    import csv
    g = sorted(int(r['G']) for r in csv.DictReader(open(os.path.join(lahman, 'Teams.csv'), encoding='utf-8-sig')) if r.get('yearID') == str(y) and r.get('lgID') in ('AL', 'NL'))
    return g[len(g) // 2] if g else 162
g = season_games(first) if first <= season + 1 else season_games(season)
if 150 <= g < 160: subprocess.check_call([sys.executable, os.path.join(here, 'era_schedule.py'), iso, out, '154'])   # the 154-game season (to 1960; strike years keep 162)
subprocess.check_call([sys.executable, os.path.join(here, 'era_blurb.py'), lahman, out])            # a few lines for the Eras menu
cm = os.path.join(os.path.dirname(out), 'commentary-2026')
# faces and portraits of its own (tools/faces/make_all.sh, an hour or more) go on in the background when this machine
# has what they need; the pack can be played meanwhile and shows them from the next start
import importlib.util
need = [m for m in ('numpy', 'PIL', 'cv2', 'onnxruntime') if importlib.util.find_spec(m) is None]
tmpl = os.path.join('faces_work', 'templates')
if os.name != 'nt' and not need and os.path.isdir(tmpl) and '--no-faces' not in sys.argv:
    log = os.path.join('build', 'faces-%s.log' % os.path.basename(out))
    subprocess.Popen('nohup bash tools/faces/make_all.sh "%s" %s > "%s" 2>&1 &' % (iso, out, log), shell=True)
    print('faces and portraits: being made in the background (%s)' % log)
elif need: print('faces and portraits: skipped (python3 -m pip install %s)' % ' '.join({'PIL': 'pillow', 'cv2': 'opencv-python'}.get(m, m) for m in need))
elif not os.path.isdir(tmpl): print('faces and portraits: skipped (no faces_work/templates)')
subprocess.call([sys.executable, os.path.join(here, 'check_pack.py'), out])                              # (prints any problem left)
print('league pack: %s (choose "MLB %d" under Settings > Game > Rosters)' % (out, first))
