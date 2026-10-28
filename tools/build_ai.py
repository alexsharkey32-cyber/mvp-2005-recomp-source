"""Builds the "2026 rules" CPU tuning overlay: mods/ai-2026/DATA/DATAFILE/DATAFILE.BIG

Usage: python3 tools/build_ai.py <your MVP Baseball 2005 ISO> [out dir = mods/ai-2026]

The game's gameplay and CPU tuning lives in DATAFILE.BIG/datafile.txt (EA's tweak database, see tools/datafile.py).
This rewrites the few sections that today's rules and strategy changed; everything else is untouched:
  - shift ban (2023): the two "extreme shift" infield alignments put three infielders on one side of second
    base. They become the strongest legal shade: two infielders on each side of the bag, all on the dirt.
  - pitch clock (2023): CPU pitchers take less time between pitches.
  - bigger bases and the pickoff limit (2023) made stealing pay again: CPU managers send fast, skilled base
    stealers more often, and a left-handed pitcher deters them less. (The game already allows two pickoff
    throws per runner, which is today's limit.)
  - the hit-and-run has nearly disappeared from the game: the CPU calls it less.
What this does not touch: the code that decides pitching changes, pinch hitters, bunts and intentional walks
is compiled into the game, not data, so those decisions are the original ones.
Turn it off in the launcher or the menu (Game > CPU play).
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
import isofile, refpack, mvpdb, datafile
from bigfile import read_big

ISO = sys.argv[1]
OUT = sys.argv[2] if len(sys.argv) > 2 else 'mods/ai-2026'

big = isofile.read_file(ISO, 'DATA/DATAFILE/DATAFILE.BIG')
ents = read_big(big)
raw = {n: big[o:o + s] for n, o, s in ents}
D = datafile.DataFile(refpack.decompress(raw['datafile.txt']).decode('latin1'))

changes = []
def put(sec, field, value):
    old = D.fields(sec)[field]
    D.set(sec, field, value)
    changes.append('%s.%s: %s -> %s' % (sec, field, old, value))
def scale(sec, field, k, fmt='%.2f'): put(sec, field, fmt % (float(D.fields(sec)[field]) * k))

# ---- shift ban. X runs towards third base, Z towards first (feet from home plate; the mound is at 42.8, 42.8),
# so a fielder is on the first-base side of second when Z > X. Legal: two on each side, within 95 ft of the mound.
legal = {
    'if_extremeshift_lh': {'FirstBase': (12.0, 118.0), 'SecondBase': (52.0, 128.0), 'ThirdBase': (100.0, 30.0), 'Shortstop': (100.0, 92.0)},
    'if_extremeshift_rh': {'FirstBase': (30.0, 100.0), 'SecondBase': (92.0, 100.0), 'ThirdBase': (118.0, 12.0), 'Shortstop': (128.0, 52.0)},
}
for sec, pos in legal.items():
    sides = [z > x for x, z in pos.values()]
    assert sides.count(True) == 2, sec
    for who, (x, z) in pos.items():
        assert ((x - 42.8) ** 2 + (z - 42.8) ** 2) ** 0.5 <= 95, (sec, who)
        put(sec, who + 'Xfeet', '%.1f' % x); put(sec, who + 'Zfeet', '%.1f' % z)

# ---- pitch clock: the CPU pitcher's delays between pitches (CPU vs CPU, and CPU pitching to you)
for f in ('CCSameBatter_NoRunners', 'CCSameBatter_WithRunners', 'CCNewBatter_NoRunners', 'CCNewBatter_WithRunners',
          'CUSameBatter_NoRunners', 'CUSameBatter_WithRunners', 'CUNewBatter_NoRunners', 'CUNewBatter_WithRunners'):
    scale('pitcher', f, 0.6)

# ---- stealing: attempts per game are up by about a quarter since 2005 and succeed more often
put('stealtune', 'SpeedMod', '14')        # bonus for fast runners (was 10)
put('stealtune', 'StealBMod', '5')        # good base stealers (was 3)
put('stealtune', 'StealCMod', '10')       # elite base stealers (was 6)
put('stealtune', 'LHP_R1st', '-14')       # a lefty holding the runner at first (was -20)

# ---- hit and run: rarely called now
put('hitnrun', 'CH_Mod', '5')             # good contact hitter at the plate (was 10)
put('hitnrun', 'RSH_Mod', '5')            # fast runner (was 10)
put('hitnrun', 'DP_Mod', '25')            # the game's "DoublePlays" bonus (was 50)

new = D.text().encode('latin1')
comp = refpack.compress(new)
assert refpack.decompress(comp) == new
files = [(n, comp if n == 'datafile.txt' else raw[n]) for n, o, s in ents]
os.makedirs(os.path.join(OUT, 'DATA', 'DATAFILE'), exist_ok=True)
open(os.path.join(OUT, 'DATA', 'DATAFILE', 'DATAFILE.BIG'), 'wb').write(mvpdb.write_big(files))
open(os.path.join(OUT, 'ai_changes.txt'), 'w').write('\n'.join(changes) + '\n')
print('\n'.join(changes)); print('wrote', os.path.join(OUT, 'DATA', 'DATAFILE', 'DATAFILE.BIG'))
