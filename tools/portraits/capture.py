"""Headshot capture: drives a running game (headless, MVP_AUTOPRESS=@NAME.rules, MVP_FRAMES_BASE=1) through
Manage Rosters > Create/Edit Player > Edit Player, and for every player of every team opens Appearance > Face (the
close-up of his head in his club's uniform), switches to 4x internal resolution and takes three pictures (he blinks).
The studio backdrop (the screen's background texture, replaced through the run's textures/replace) is plain
magenta, so the pictures can be cut out (tools/portraits/build.py). Each picture comes with NAME.key: the database key of the player the screen was reading (runtime: hle_db_attr_get).

    python3 tools/portraits/capture.py RUNDIR OUTDIR [max_teams] [skip_teams]

RUNDIR is the run's directory (the game was started there with the Edit Player list on screen, cursor on the first
player); OUTDIR gets <key>_<n>.png. Already captured keys are skipped (the run can be restarted).
"""
import sys, os, time, glob
import numpy as np
from PIL import Image
def closeup(path):
    """is this the close-up of the head? (much of the left of the screen is not the magenta studio backdrop)"""
    try: im = np.asarray(Image.open(path).convert('RGB').resize((640, 448), Image.BOX)).astype(int)[60:330, 10:220]
    except Exception: return False
    key = (im[..., 0] > 180) & (im[..., 2] > 180) & (im[..., 1] < 90)
    return (~key).mean() > 0.4
run, out = sys.argv[1], sys.argv[2]
max_teams = int(sys.argv[3]) if len(sys.argv) > 3 else 999
skip_teams = int(sys.argv[4]) if len(sys.argv) > 4 else 0     # clubs already done (a restarted run): R1 past them first
os.makedirs(out, exist_ok=True)
once = run.rstrip('/') + '.rules.once'
def frames():
    fs = sorted(f for f in os.listdir(os.path.join(run, 'fr')) if f.startswith('frame_'))
    return fs[-1] if fs else ''
def cmd(c, wait=0.6, timeout=25):
    open(once, 'w').write(c)
    t = time.time()
    while os.path.exists(once) and time.time() - t < timeout: time.sleep(0.05)
    if os.path.exists(once): os.remove(once); print('  (not taken: %s)' % c, flush=True)
    time.sleep(wait)
def shot(path):
    for f in (path, path + '.key'):
        if os.path.exists(f): os.remove(f)
    cmd('SHOT ' + path, 0.2, 60)
    t = time.time()
    while not os.path.exists(path + '.key') and time.time() - t < 30: time.sleep(0.1)
    try: k, n, tot = open(path + '.key').read().split()
    except Exception: return None, 0
    return k, int(n)
done = {os.path.basename(f).split('_')[0] for f in glob.glob(os.path.join(out, '*_0.png'))}
first_team_key = None; teams = 0
def leave(): cmd('RES 1', 0.5); cmd('TRIANGLE', 1.5); cmd('DOWN', 0.5); cmd('CROSS', 2.0)     # "exit without saving? Yes"
for _ in range(skip_teams): cmd('R1', 2.5)
while teams < max_teams:
    team_keys = []; retry = 0; again = False
    while True:
        cmd('MARK', 0.1); cmd('CROSS', 1.6); cmd('R1', 0.8)
        for _ in range(4): cmd('DOWN', 0.25)
        time.sleep(1.0)
        for tries in range(4):                              # a press gets lost now and then: down to the Face row
            last = frames(); t = time.time()
            while frames() == last and time.time() - t < 5: time.sleep(0.1)
            time.sleep(0.2)
            if closeup(os.path.join(run, 'fr', frames())): break
            cmd('DOWN', 1.0)
        cmd('RES 4', 1.5)
        tmp = os.path.join(out, '_t0.png')
        key, n = shot(tmp)
        if key and team_keys and key == team_keys[-1] and not again:   # the DOWN got lost, or the end of the list: once more
            os.remove(tmp); leave(); cmd('DOWN', 0.8); again = True; continue
        again = False
        if key and key in team_keys:                        # back at the top of the list: the team is done
            os.remove(tmp); leave(); break
        if key and key not in done:
            if not closeup(tmp):                            # not the close-up after all: this one again
                print('  %s: no close-up' % key, flush=True); os.remove(tmp); leave()
                retry += 1
                if retry <= 2: continue
            else:
                os.replace(tmp, os.path.join(out, '%s_0.png' % key)); os.replace(tmp + '.key', os.path.join(out, '%s_0.png.key' % key))
                for i in (1, 2):
                    time.sleep(0.6); shot(os.path.join(out, '%s_%d.png' % (key, i)))
                done.add(key)
                print('%s %s (%d reads)' % (time.strftime('%H:%M:%S'), key, n), flush=True)
                leave()
        else: leave()
        retry = 0
        team_keys.append(key)
        cmd('DOWN', 0.8)
    teams += 1
    if first_team_key is None: first_team_key = team_keys[0]
    print('team %d: %d players' % (teams, len(team_keys)), flush=True)
    cmd('R1', 2.5)                                          # next club
    # stop when the first club comes round again
    cmd('MARK', 0.1); cmd('CROSS', 2.0); cmd('R1', 1.0)
    for _ in range(4): cmd('DOWN', 0.3)
    time.sleep(1.0)
    k, n = shot(os.path.join(out, '_probe.png'))
    cmd('TRIANGLE', 1.5); cmd('DOWN', 0.5); cmd('CROSS', 2.0)
    if k == first_team_key: print('all clubs done'); break
