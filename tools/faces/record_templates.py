import os, time, shutil, sys
from PIL import Image
# record_templates.py RUN_DIR OUT_DIR [--as fNN_sC]: the head templates for tools/portraits/render_league.py, from a
# studio run (.once commands; parked on Edit Player > Appearance of a generic-head Red Sox player, the list scrolled so
# Weight is the top row). Every face 1-15 with hair styles 0, 1, 2, 3-8 (one class) and 9: 75 recordings, each named
# by its face texture's hash (GHEAD.BIG names: ghead_hash*.txt next to this script). Resumable.
a = [x for i, x in enumerate(sys.argv[1:], 1) if not x.startswith('--') and sys.argv[i - 1] != '--as']
run = a[0].rstrip('/'); once = run + '.rules.once'; out = a[1]
H = {}
for f in [os.path.join(os.path.dirname(os.path.abspath(__file__)), n) for n in ('ghead_hash.txt', 'ghead_hash_r26.txt')]:
    if not os.path.exists(f): continue
    for ln in open(f): h, n = ln.split(); H[h] = n
def cmd(c, wait=0.6, timeout=25):
    open(once, 'w').write(c); t = time.time()
    while os.path.exists(once) and time.time() - t < timeout: time.sleep(0.05)
    if os.path.exists(once): os.remove(once); print('  (not taken: %s)' % c, flush=True)
    time.sleep(wait)
def shot():
    p = '/tmp/claude-0/_row.png'
    if os.path.exists(p + '.key'): os.remove(p + '.key')
    cmd('SHOT ' + p, 0.2); t = time.time()
    while not os.path.exists(p + '.key') and time.time() - t < 20: time.sleep(0.1)
    return Image.open(p).convert('RGB')
YS = (178, 212, 245, 279, 312, 346)            # weight, body type, face, hair colour, hair style, facial hair
def row():
    im = shot()
    sc = [sum(1 for x in range(236, 330) for yy in (y - 3, y, y + 3) if (lambda c: c[0] > 140 and c[0] - c[2] > 40)(im.getpixel((x, yy)))) for y in YS]
    return sc.index(max(sc)) if max(sc) > 10 else -1
def goto(target):
    for _ in range(12):
        r = row()
        if r == target: return True
        cmd('DOWN' if (r < target and r >= 0) else 'UP', 0.8)
    return False
def record():
    d = os.path.join(out, '_tmp'); shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
    cmd('PRIMS ' + d, 0.2, 60); t = time.time()
    while not os.path.exists(os.path.join(d, 'prims.txt')) and time.time() - t < 20: time.sleep(0.1)
    time.sleep(2.0)
    cnt = {}
    for ln in open(os.path.join(d, 'prims.txt')):
        p = ln.split()
        if p[0] == 'T' and p[2] == '128' and p[3] == '256': cnt[p[1]] = cnt.get(p[1], 0) + 1
    name = H.get(max(cnt, key=cnt.get)) if cnt else None
    return d, name
def cls(s): return s if s in (0, 1, 2, 9) else 4
if '--as' in sys.argv:
    d, name = record(); dst = os.path.join(out, sys.argv[sys.argv.index('--as') + 1]); shutil.rmtree(dst, ignore_errors=True); os.rename(d, dst); print(dst); sys.exit()
have = set()
for dn in os.listdir(out):
    if dn.startswith('f') and '_s' in dn: have.add((int(dn[1:3]) - 1, int(dn.split('_s')[1])))
print('have', len(have), flush=True)
for step in range(40):
    goto(2); time.sleep(2.0)
    d, name = record()
    if not name: print('unknown face', flush=True); cmd('RIGHT', 1); continue
    s = ord(name[3]) - 97; c = cls(s)
    print('style', s, 'class', c, flush=True)
    if not all((h, c) in have for h in range(15)):
        for i in range(16):
            d, name = record()
            if name:
                h = ord(name[1]) - 97; s2 = ord(name[3]) - 97
                if cls(s2) == c and (h, c) not in have:
                    dst = os.path.join(out, 'f%02d_s%d' % (h + 1, c)); shutil.rmtree(dst, ignore_errors=True); os.rename(d, dst); have.add((h, c))
                    print('  ', name, '->', os.path.basename(dst), flush=True)
            cmd('RIGHT', 1.6)
    if all((h, c) in have for h in range(15) for c in (0, 1, 2, 4, 9)): break
    goto(4); cmd('RIGHT', 1.0)
print('done', len(have))
