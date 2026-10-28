"""Plain logos for the clubs of an era pack that are not the 2005 club of their slot -> the pack's art overlay.

    python3 tools/eras/era_logos.py ISO PACK_DIR [--preview out.png]

The Montreal Expos play in the Nationals' slot, the Kansas City A's in Oakland's, the Seattle Pilots in Milwaukee's
...: without this the menus show the 2005 club's logo. A club whose city or name differs from the disc's gets
lettering of its own: no historical marks are copied, only the club's initials and names in plain type, in the
colours of the uniform it wears that season (its cap and trim, read from the pack's era uniform or the disc's).
  a  primary logo (128 x 64): the nickname over a bar, in the club's colours
  i  logo (64 x 64): a roundel with the initials
  g  cap logo (64 x 64): the initials
  l  wordmark (128 x 64): the city
  j  tiny logo (32 x 16), d / e colour bars
A club named as it is today gets today's logo from mods/uniforms-2026 when there is one (the Rays of 2012).
Written to PACK_DIR/art/DATA/FRONTEND/LOGOS.BIG/l<artid - 1>.ssh, which the runtime loads after the other mods.
"""
import sys, os, tempfile
import numpy as np
from PIL import Image, ImageDraw, ImageFont
here = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, os.path.join(here, '..'))
import isofile, refpack, ssh, mvpdb
from bigfile import read_big

def font(paths, size):
    for p in paths:
        if os.path.exists(p): return ImageFont.truetype(p, size)
    return ImageFont.load_default()
SERIF = ['/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf', '/usr/share/texmf/fonts/opentype/public/tex-gyre/texgyrebonum-bold.otf',
         '/System/Library/Fonts/Supplemental/Georgia Bold.ttf', '/Library/Fonts/Georgia Bold.ttf']
SCRIPT = ['/usr/share/fonts/truetype/google-fonts/Poppins-BoldItalic.ttf', '/usr/share/fonts/truetype/dejavu/DejaVuSans-BoldOblique.ttf',
          '/System/Library/Fonts/Supplemental/Arial Bold Italic.ttf', '/Library/Fonts/Arial Bold Italic.ttf']
BLOCK = ['/usr/share/fonts/truetype/google-fonts/Poppins-Bold.ttf', '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf',
         '/System/Library/Fonts/Supplemental/Arial Bold.ttf', '/Library/Fonts/Arial Bold.ttf']

def unpack(c): return refpack.decompress(c) if c[1:2] == b'\xfb' else c

def initials(city):
    w = [x for x in city.replace('.', ' ').split() if x[:1].isalpha()]
    if not w: return '?'
    return (w[0][0] + w[1][0]).upper() if len(w) > 1 else w[0][0].upper()

# the same place under another name (the 2005 club's logo still fits)
SAME_PLACE = [{'Anaheim', 'Los Angeles', 'California'}, {'Arizona', 'Phoenix'}, {'Colorado', 'Denver'}, {'Florida', 'Miami'},
              {'Tampa Bay', 'St. Petersburg', 'Tampa'}, {'Texas', 'Arlington'}, {'Minnesota', 'Bloomington'}, {'New York', 'Flushing', 'Queens', 'Bronx'}]
def differs(era, disc):
    if disc is None: return True
    (c1, n1), (c2, n2) = era, disc
    full = (c1 + ' ' + n1).lower()
    if n2.lower() in full and c2.lower() in full: return False          # "Los Angeles Angels of Anaheim"
    a, b = n1.strip().lower(), n2.strip().lower()
    if a != b and not (b in a or a in b or {a, b} == {'d-backs', 'diamondbacks'}): return True
    return c1 != c2 and not any(c1 in s and c2 in s for s in SAME_PLACE)

def lum(c): return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]

def colours(iso, pack, club):
    """(main, trim): the cap's colour and the strongest other colour of the jersey, from the season's home uniform"""
    name = 'u%03da.ssh' % club; raw = None
    p = os.path.join(pack, 'DATA', 'MODELS.BIG', name)
    if os.path.exists(p): raw = open(p, 'rb').read()
    else:
        d = isofile.read_file(iso, 'DATA/MODELS.BIG'); e = {n: (o, s) for n, o, s in read_big(d)}
        o, s = e[name]; raw = d[o:o + s]
    px = {}
    for sh in ssh.Ssh(unpack(raw)).shapes:
        if sh.type in (2, 14) and sh.palette() is not None and sh.name not in px: px[sh.name] = sh.rgba()
    import colorsys
    def groups(a):
        """pixels by colour family: 12 hue bins of the saturated ones, 'dark' (black / deep navy), 'light' (white, grey)"""
        a = a.reshape(-1, 4); a = a[a[:, 3] > 100][:, :3].astype(np.float32) / 255
        mx, mn = a.max(1), a.min(1); v = mx; s = np.where(mx > 0, (mx - mn) / np.maximum(mx, 1e-6), 0)
        hsv = np.array([colorsys.rgb_to_hsv(*x) for x in a[::max(1, len(a) // 4000)]]); sub = a[::max(1, len(a) // 4000)]
        out = {}
        for x, (h, s_, v_) in zip(sub, hsv):
            k = 'dark' if v_ < 0.22 else 'light' if s_ < 0.28 else int(h * 12) % 12
            out.setdefault(k, []).append(x)
        tot = sum(len(x) for x in out.values()) or 1
        res = []
        for k, xs in out.items():
            xs = np.array(xs); br = xs.max(1); sel = xs[br >= np.percentile(br, 60)]   # the lit side of the cloth
            res.append((len(xs) / tot, k, tuple(int(c) for c in (sel.mean(0) * 255).round())))
        return sorted(res, reverse=True)
    hat = px.get('hat ')
    main = None
    if hat is not None:
        g = [x for x in groups(hat) if x[1] != 'light' and x[0] > 0.05]
        if g: main = g[0][2]; main_k = g[0][1]
    trim = None
    allpx = np.concatenate([px[n].reshape(-1, 4) for n in ('hat ', 'jers', 'jerf', 'slvl') if n in px])
    if os.environ.get("ERA_LOGO_DEBUG"): print(club, [(round(f, 3), k, c) for f, k, c in groups(allpx)][:8])
    gs = groups(allpx)
    if main is None:
        g = [x for x in gs if x[1] != 'light']; main, main_k = (g[0][2], g[0][1]) if g else ((20, 30, 90), 'dark')
    for f, k, c in gs:                                   # a colour of its own: another hue
        if f < 0.015 or not isinstance(k, int) or max(c) < 140: continue
        if isinstance(main_k, int) and min((k - main_k) % 12, (main_k - k) % 12) < 2: continue
        trim = c; break
    main = main or (20, 30, 90)
    if trim is None: trim = (240, 240, 240)
    return main, trim

def text_img(txt, fnt_paths, h, fill, outline, ow, max_w=None):
    """txt drawn big (fill, outline width ow), cropped; scaled later by fit"""
    f = font(fnt_paths, h)
    bb = f.getbbox(txt, stroke_width=ow)
    im = Image.new('RGBA', (bb[2] - bb[0] + 8, bb[3] - bb[1] + 8), (0, 0, 0, 0))
    ImageDraw.Draw(im).text((4 - bb[0], 4 - bb[1]), txt, font=f, fill=fill + (255,), stroke_width=ow, stroke_fill=outline + (255,))
    return im

def fit(im, w, h, margin=0.06):
    bb = im.getbbox(); im = im.crop(bb) if bb else im
    k = min(w * (1 - 2 * margin) / im.width, h * (1 - 2 * margin) / im.height)
    im = im.resize((max(1, round(im.width * k)), max(1, round(im.height * k))), Image.LANCZOS)
    c = Image.new('RGBA', (w, h), (0, 0, 0, 0)); c.paste(im, ((w - im.width) // 2, (h - im.height) // 2), im); return c

def contrast(c, a='white'):
    return (255, 255, 255) if lum(c) < 150 else (20, 20, 30)

def make(city, nick, season, main, trim):
    """the set of pictures for one club (RGBA PIL images at 4x their size, fitted later)"""
    ini = initials(city); white = (255, 255, 255)
    old = season < 1960
    nickf = SERIF if old else SCRIPT
    # primary: the nickname with an outline, over a bar in the trim colour
    t = text_img(nick, nickf, 160, main, white if lum(main) < 200 else trim, 10)
    t = t.resize((t.width, int(t.height * 1.0)))
    bar_h = max(18, t.height // 7)
    prim = Image.new('RGBA', (t.width + 40, t.height + bar_h + 10), (0, 0, 0, 0)); d = ImageDraw.Draw(prim)
    d.rounded_rectangle((10, t.height - bar_h // 2, prim.width - 10, t.height + bar_h), radius=bar_h // 2, fill=trim + (255,), outline=white + (255,), width=5)
    prim.paste(t, (20, 0), t)
    # roundel: the initials in a ring
    R = 256; rnd = Image.new('RGBA', (R, R), (0, 0, 0, 0)); d = ImageDraw.Draw(rnd)
    d.ellipse((4, 4, R - 4, R - 4), fill=white + (255,)); d.ellipse((14, 14, R - 14, R - 14), fill=trim + (255,)); d.ellipse((30, 30, R - 30, R - 30), fill=main + (255,))
    ti = text_img(ini, SERIF, 200, contrast(main), trim, 6)
    ti = fit(ti, int(R * 0.62), int(R * 0.5), 0); rnd.paste(ti, ((R - ti.width) // 2, (R - ti.height) // 2), ti)
    # cap: the initials alone, in the colour that shows on the cap
    capf = contrast(main) if sum(abs(a - b) for a, b in zip(trim, main)) < 150 else trim
    cap = text_img(ini, SERIF, 220, capf, white if capf != white else trim, 8)
    # wordmark: the city
    word = text_img(city.upper() if not old else city, BLOCK if not old else SERIF, 160, main, white if lum(main) < 200 else trim, 8)
    return dict(a=prim, i=rnd, g=cap, l=word, j=rnd)

PLAIN_PATTERN = {'a': 'u010a.ssh', 'b': 'u010b.ssh'}      # a plain cut to build on: no piping, a chest wordmark

def plain_uniforms(iso, pack, club, city, nick, main_c, trim, season):
    """a home and road set for a club named for a minor-league club (era_names.py): plain flannels in its colours,
    the nickname across the chest, the initials on the cap (tools/uniform_maker.py) -> PACK/art/DATA/MODELS.BIG"""
    import uniform_maker
    d = isofile.read_file(iso, 'DATA/MODELS.BIG'); e = {n.lower(): (o, s) for n, o, s in read_big(d)}
    get = lambda n: unpack(d[e[n][0]:e[n][0] + e[n][1]])
    src = os.path.join(pack, 'art', '_src'); os.makedirs(src, exist_ok=True)
    white = (255, 255, 255)
    word = text_img(nick, SERIF if season < 1960 else SCRIPT, 160, main_c, trim if trim != (240, 240, 240) else main_c, 4)
    capl = text_img(initials(city), SERIF, 220, white if lum(main_c) < 150 else (20, 20, 30), white, 0)
    wp, cp = os.path.join(src, 'w%03d.png' % club), os.path.join(src, 'c%03d.png' % club)
    word.save(wp); capl.save(cp)
    hx = lambda c: '#%02x%02x%02x' % tuple(c)
    dm = os.path.join(pack, 'art', 'DATA', 'MODELS.BIG'); os.makedirs(dm, exist_ok=True)
    pv = isofile.read_file(iso, 'DATA/FRONTEND/UNIFORMS.BIG'); pe = {n.lower(): (o, s) for n, o, s in read_big(pv)}
    pn = '%03d.ssh' % club; pf = ssh.Ssh(unpack(pv[pe[pn][0]:pe[pn][0] + pe[pn][1]])) if pn in pe else None
    for slot, cloth in (('a', '#f1efe9'), ('b', '#a9abae')):
        prev = next((sh for sh in pf.shapes if sh.name == '%03d%s' % (club, slot)), None) if pf else None
        spec = dict(cloth=cloth, trim=hx(main_c), undershirt=hx(main_c), crown=hx(main_c), button=hx(trim), numbers=hx(main_c),
                    numbers_outline=hx(trim) if trim != (240, 240, 240) else None, pinstripes=False, wordmark=wp, cap_logo=cp)
        fname = 'f%03d%s.ssh' % (club, slot)
        u, fo = uniform_maker.make(get(PLAIN_PATTERN[slot]), spec, get(fname) if fname in e else None, prev)   # (and the menus' thumbnail)
        open(os.path.join(dm, 'u%03d%s.ssh' % (club, slot)), 'wb').write(refpack.compress(u))
        if fo: open(os.path.join(dm, fname), 'wb').write(refpack.compress(fo))
    if pf:
        dp = os.path.join(pack, 'art', 'DATA', 'FRONTEND', 'UNIFORMS.BIG'); os.makedirs(dp, exist_ok=True)
        open(os.path.join(dp, pn), 'wb').write(refpack.compress(pf.build()))

def main():
    iso, pack = sys.argv[1], sys.argv[2]
    season = int(open(os.path.join(pack, 'year.txt')).read().split()[0]) if os.path.exists(os.path.join(pack, 'year.txt')) else 2005
    tmp = tempfile.NamedTemporaryFile(suffix='.BIG', delete=False); tmp.write(isofile.read_file(iso, 'DATA/DATABASE/DATABASE.BIG')); tmp.close()
    D, _ = mvpdb.read_database(tmp.name); os.unlink(tmp.name)
    P, _ = mvpdb.read_database(os.path.join(pack, 'DATA', 'DATABASE', 'DATABASE.BIG'))
    def clubs(T):
        t = T['team.dat']; out = {}
        for k, v in t.rows:
            a = int(v[t.col('team_artid')])
            if 1 <= a <= 30: out[a] = (v[t.col('team_location')], v[t.col('team_long_name')])
        return out
    disc, era = clubs(D), clubs(P)
    sf = os.path.join(pack, 'standins.txt')                    # clubs named for minor-league clubs (era_names.py)
    standins = {int(l) for l in open(sf) if l.strip() and not l.startswith('#')} if os.path.exists(sf) else set()
    # a club named as today has today's logo (mods/uniforms-2026: the Guardians, the Rays ...): left to that
    root = os.path.dirname(os.path.abspath(pack)); modern = {}
    mdb = os.path.join(root, 'rosters-2026', 'DATA', 'DATABASE', 'DATABASE.BIG')
    if os.path.exists(mdb): modern = clubs(mvpdb.read_database(mdb)[0])
    have26 = lambda art: os.path.exists(os.path.join(root, 'uniforms-2026', 'DATA', 'FRONTEND', 'LOGOS.BIG', 'l%03d.ssh' % (art - 1)))
    big = isofile.read_file(iso, 'DATA/FRONTEND/LOGOS.BIG'); ents = {n.lower(): (o, s) for n, o, s in read_big(big)}
    outd = os.path.join(pack, 'art', 'DATA', 'FRONTEND', 'LOGOS.BIG'); os.makedirs(outd, exist_ok=True)
    log, previews = [], []
    for art in sorted(era):
        city, nick = era[art]
        name = 'l%03d.ssh' % (art - 1); p = os.path.join(outd, name)
        if have26(art) and era[art] == modern.get(art) and season >= 2008 and (season >= 2019 or era[art] != disc.get(art)):          # named as today: today's logo (the runtime leaves
            import shutil                                         # mods/uniforms-2026 out under an era pack)
            shutil.copy(os.path.join(root, 'uniforms-2026', 'DATA', 'FRONTEND', 'LOGOS.BIG', name), p)
            log.append('club %2d: %s %s, today\'s logo' % (art - 1, city, nick)); continue
        if not differs(era[art], disc.get(art)) and art - 1 not in standins:
            if os.path.exists(p): os.remove(p)                 # (from an earlier run)
            continue
        main_c, trim = colours(iso, pack, art - 1)
        pics = make(city, nick, season, main_c, trim)
        o, s = ents[name]; f = ssh.Ssh(unpack(big[o:o + s]))
        for sh in f.shapes:
            kind = sh.name[0]; w, h = sh.size
            if kind in pics:
                px = np.asarray(fit(pics[kind], w, h)).astype(np.float32)
                px = np.dstack([px[..., :3], np.round(px[..., 3:] * 128 / 255)]).astype(np.uint8)       # PS2 alpha
                if sh.type == 14: data, pal = ssh.image14_photo(px); sh.set_pixel_bytes(data); sh.set_palette(pal)
                elif sh.type == 5: sh.set_pixel_bytes(px.tobytes())
            elif kind in 'de' and sh.type == 5:
                px = np.frombuffer(sh.pixel_bytes(), np.uint8).reshape(h, w, 4).copy(); px[..., :3] = main_c if kind == 'e' else trim
                sh.set_pixel_bytes(px.tobytes())
        open(p, 'wb').write(refpack.compress(f.build()))
        log.append('club %2d: %s %s (was %s %s), colours %s / %s' % (art - 1, city, nick, *disc.get(art, ('?', '?')), main_c, trim))
        if art - 1 in standins and '--no-uniforms' not in sys.argv:
            plain_uniforms(iso, pack, art - 1, city, nick, main_c, trim, season); log[-1] += ', plain uniforms'
        previews.append([fit(pics[k], *wh) for k, wh in (('a', (128, 64)), ('i', (64, 64)), ('g', (64, 64)), ('l', (128, 64)))])
    open(os.path.join(pack, 'logos.txt'), 'w').write('# era logos for %d (tools/eras/era_logos.py)\n' % season + '\n'.join(log) + '\n')
    print('%d clubs with era logos' % len(log)); print('\n'.join(log))
    if '--preview' in sys.argv and previews:
        W = Image.new('RGB', (400, 70 * len(previews)), (128, 128, 128))
        for r, row in enumerate(previews):
            x = 4
            for im in row: W.paste(im, (x, 70 * r + 3), im); x += im.width + 4
        W.save(sys.argv[sys.argv.index('--preview') + 1])

if __name__ == '__main__': main()
