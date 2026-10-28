#!/usr/bin/env python3
"""Commentary for the teams that changed their name since 2005, made only from recordings that are on the disc.

  python3 tools/build_team_speech.py <game.iso> [mods/commentary-2026]      (build_rosters.py runs this too)

The commentators recorded every team 16 ways (DATA/AUDIO/SPCH_PBP/TNAMEHDR.BIG + TNAMEDAT.BIG, one pair of
members per team, numbered like team_artid). Each recording has a code that says what it is:

  0000        the place alone                       "Cleveland"
  0001, 0101  place and nickname                    "Cleveland Indians"
  0003, 0103, 0203, 020f   the nickname             "the Indians"
  0107, 0108  a short lead-in and the nickname
  010d        the other nickname
  0202, 0204, 0205, 0209   a longer lead-in that ends in the nickname
  020a, 020b  the nickname and a few words after it

(worked out by lining the recordings up against each other, see research/notes/findings.md, session 19).
Nobody ever recorded "Guardians", "Rays", "Miami" or "Los Angeles Angels", so nothing new can be said. What can be
done is to point a code at another recording of the same team:

  nickname changed (Cleveland, Tampa Bay): every recording that has the old nickname in it is replaced by the
      place alone - the commentators call the team "Cleveland" and "Tampa Bay".
  place changed (Angels, Athletics, Marlins): the recordings with the old place in them are replaced by the
      nickname alone - "the Angels", "the Athletics", "the Marlins".

A few sentences lose their lead-in that way, none says a wrong name. The stadium announcer (SPCH_PA) has five
long recordings per team, all with the full old name; those teams are taken out of his index, and he then skips
the team name the way he already does for the teams he never recorded.
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import isofile
from bigfile import read_big

CITY, NICK, NICK1 = 0x0000, 0x0003, 0x0103
ALL = (0x0000, 0x0001, 0x0003, 0x0101, 0x0103, 0x0107, 0x0108, 0x010d, 0x0202, 0x0203, 0x0204, 0x0205, 0x0209,
       0x020a, 0x020b, 0x020f)
NICK_CHANGED = {c: CITY for c in ALL if c != CITY}                 # code -> code of the recording to play instead
PLACE_CHANGED = {0x0000: NICK, 0x0001: NICK, 0x0101: NICK1}
TEAMS = {                                                          # team_artid: (what it says now, recipe)
    1: ('Angels (was Anaheim)', PLACE_CHANGED),
    2: ('Athletics (was Oakland)', PLACE_CHANGED),
    6: ('Cleveland (was Indians)', NICK_CHANGED),
    13: ('Tampa Bay (was Devil Rays)', NICK_CHANGED),
    27: ('Marlins (was Florida)', PLACE_CHANGED),
}

def members(big): return {n: big[o:o + s] for n, o, s in read_big(big)}

def parse_hdr(h):
    """-> (count, [(offset in 256-byte units, code)])"""
    assert h[4] == 2, 'unexpected index format %d' % h[4]
    n = h[5]
    return n, [struct.unpack('>HH', h[12 + 4 * i:16 + 4 * i]) for i in range(n)]

def remap(hdr, dat, recipe):
    """new (hdr, dat): the same codes in the same order, each with its own copy of the recording it now plays"""
    n, pairs = parse_hdr(hdr)
    starts = sorted(o for o, _ in pairs) + [len(dat) // 256]
    clip = {}
    for o, c in pairs:
        end = starts[starts.index(o) + 1]
        d = dat[o * 256:end * 256]
        assert d[:4] == b'SCHl', (c, d[:4])
        clip.setdefault(c, d)                      # a code recorded more than once: the first take
    out = bytearray(); new = []
    for o, c in pairs:
        src = recipe.get(c, c)
        if src not in clip: src = c                # (a team without that recording keeps what it has)
        d = dat[o * 256:starts[starts.index(o) + 1] * 256] if src == c else clip[src]
        new.append((len(out) // 256, c)); out += d
    h = bytearray(hdr)
    h[8:12] = struct.pack('<I', len(out) // 256)
    for i, (o, c) in enumerate(new): h[12 + 4 * i:16 + 4 * i] = struct.pack('>HH', o, c)
    return bytes(h), bytes(out)

def hide_members(big, names):
    """the same archive with those members renamed to .hdx: the game no longer finds them"""
    b = bytearray(big); n = struct.unpack('>I', b[8:12])[0]; off = 16; hidden = 0
    for _ in range(n):
        off += 8; e = b.index(b'\0', off); name = b[off:e].decode('latin1')
        if name in names: b[e - 1] = ord('x'); hidden += 1
        off = e + 1
    return bytes(b), hidden

def build(iso, out):
    hdrs = members(isofile.read_file(iso, 'DATA/AUDIO/SPCH_PBP/TNAMEHDR.BIG'))
    dats = members(isofile.read_file(iso, 'DATA/AUDIO/SPCH_PBP/TNAMEDAT.BIG'))
    hd = os.path.join(out, 'DATA', 'AUDIO', 'SPCH_PBP', 'TNAMEHDR.BIG'); dd = os.path.join(out, 'DATA', 'AUDIO', 'SPCH_PBP', 'TNAMEDAT.BIG')
    os.makedirs(hd, exist_ok=True); os.makedirs(dd, exist_ok=True)
    for tid, (label, recipe) in sorted(TEAMS.items()):
        name = '%02d' % tid
        h, d = remap(hdrs[name + '.hdr'], dats[name + '.dat'], recipe)
        open(os.path.join(hd, name + '.hdr'), 'wb').write(h); open(os.path.join(dd, name + '.dat'), 'wb').write(d)
        n, pairs = parse_hdr(hdrs[name + '.hdr'])
        print('  team %2d  %-28s %2d of %d recordings replaced, %d KB' % (tid, label, sum(1 for _, c in pairs if recipe.get(c, c) != c), n, len(d) // 1024))
    pa, hidden = hide_members(isofile.read_file(iso, 'DATA/AUDIO/SPCH_PA/TNAMEHDR.BIG'), {'%02d.hdr' % t for t in TEAMS})
    os.makedirs(os.path.join(out, 'DATA', 'AUDIO', 'SPCH_PA'), exist_ok=True)
    open(os.path.join(out, 'DATA', 'AUDIO', 'SPCH_PA', 'TNAMEHDR.BIG'), 'wb').write(pa)
    print('team names: %d teams re-voiced from their own recordings, %d taken out of the stadium announcer\'s index -> %s' % (len(TEAMS), hidden, out))

if __name__ == '__main__':
    build(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else 'mods/commentary-2026')
