"""Like capture.py, but for our own renderer: for every player of every club the Edit Player close-up is opened and
its triangles recorded (".once PRIMS"), OUTDIR/<key>/prims.txt and its textures; tools/portraits/render_portraits.py
draws the portraits from them.

    python3 tools/portraits/capture_prims.py RUNDIR OUTDIR [max_teams] [skip_teams]
"""
import sys, os, time, glob, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
run, out = sys.argv[1], sys.argv[2]
max_teams = int(sys.argv[3]) if len(sys.argv) > 3 else 999
skip_teams = int(sys.argv[4]) if len(sys.argv) > 4 else 0
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
def closeup(path):
    import numpy as np
    from PIL import Image
    try: im = np.asarray(Image.open(path).convert('RGB').resize((640, 448), Image.BOX)).astype(int)[60:330, 10:220]
    except Exception: return False
    key = (im[..., 0] > 180) & (im[..., 2] > 180) & (im[..., 1] < 90)
    return (~key).mean() > 0.4
def record():
    tmp = os.path.join(out, '_tmp'); shutil.rmtree(tmp, ignore_errors=True); os.makedirs(tmp)
    cmd('PRIMS ' + tmp, 0.2, 60)
    t = time.time()
    while not os.path.exists(os.path.join(tmp, 'prims.txt')) and time.time() - t < 20: time.sleep(0.1)
    time.sleep(2.0)                                      # the file is closed when the frame after it starts
    sp = os.path.join(tmp, 'shot.png')
    cmd('SHOT ' + sp, 0.2, 60)
    t = time.time()
    while not os.path.exists(sp + '.key') and time.time() - t < 30: time.sleep(0.1)
    try: k = open(sp + '.key').read().split()[0]
    except Exception: return None, tmp
    return k, tmp
done = {os.path.basename(d) for d in glob.glob(os.path.join(out, '*')) if os.path.exists(os.path.join(d, 'prims.txt'))}
def leave(): cmd('TRIANGLE', 1.5); cmd('DOWN', 0.5); cmd('CROSS', 2.0)
for _ in range(skip_teams): cmd('R1', 2.5)
first_team_key = None; teams = 0
while teams < max_teams:
    team_keys = []; again = False
    while True:
        cmd('MARK', 0.1); cmd('CROSS', 1.6); cmd('R1', 0.8)
        for _ in range(4): cmd('DOWN', 0.25)
        time.sleep(1.0)
        for tries in range(4):
            last = frames(); t = time.time()
            while frames() == last and time.time() - t < 5: time.sleep(0.1)
            time.sleep(0.2)
            if closeup(os.path.join(run, 'fr', frames())): break
            cmd('DOWN', 1.0)
        key, tmp = record()
        if key and team_keys and key == team_keys[-1] and not again:
            leave(); cmd('DOWN', 0.8); again = True; continue
        again = False
        if key and key in team_keys: leave(); break
        if key and key not in done and os.path.getsize(os.path.join(tmp, 'prims.txt')) > 10000:
            dst = os.path.join(out, key); shutil.rmtree(dst, ignore_errors=True); os.rename(tmp, dst); done.add(key)
            print('%s %s' % (time.strftime('%H:%M:%S'), key), flush=True)
        leave()
        team_keys.append(key)
        cmd('DOWN', 0.8)
    teams += 1
    if first_team_key is None: first_team_key = team_keys[0]
    print('team %d: %d players' % (teams, len(team_keys)), flush=True)
    cmd('R1', 2.5)
