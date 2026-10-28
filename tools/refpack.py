"""EA RefPack (0x10FB) compression, as used by MVP Baseball 2005's database tables."""
def decompress(src):
    i = 0
    flags = src[0]; assert src[1] == 0xfb, 'not refpack'
    i = 2
    if flags & 0x01: i += 3                     # compressed-size field
    big = flags & 0x80
    n = 4 if big else 3
    size = int.from_bytes(src[i:i+n], 'big'); i += n
    out = bytearray()
    while i < len(src):
        c0 = src[i]
        if c0 < 0x80:
            c1 = src[i+1]; i += 2
            lit = c0 & 3; out += src[i:i+lit]; i += lit
            ln = ((c0 >> 2) & 7) + 3; off = ((c0 & 0x60) << 3) + c1 + 1
        elif c0 < 0xc0:
            c1, c2 = src[i+1], src[i+2]; i += 3
            lit = c1 >> 6; out += src[i:i+lit]; i += lit
            ln = (c0 & 0x3f) + 4; off = ((c1 & 0x3f) << 8) + c2 + 1
        elif c0 < 0xe0:
            c1, c2, c3 = src[i+1], src[i+2], src[i+3]; i += 4
            lit = c0 & 3; out += src[i:i+lit]; i += lit
            ln = ((c0 & 0x0c) << 6) + c3 + 5; off = ((c0 & 0x10) << 12) + (c1 << 8) + c2 + 1
        elif c0 < 0xfc:
            i += 1; lit = ((c0 & 0x1f) + 1) * 4; out += src[i:i+lit]; i += lit; continue
        else:
            i += 1; lit = c0 & 3; out += src[i:i+lit]; i += lit; break
        for _ in range(ln): out.append(out[-off])
    assert len(out) == size, (len(out), size)
    return bytes(out)

def compress(data):
    """Greedy hash-chain RefPack encoder (output decodes with any RefPack decoder)."""
    n = len(data)
    out = bytearray([0x10 | (0x80 if n > 0xffffff else 0), 0xfb])
    out += n.to_bytes(4 if n > 0xffffff else 3, 'big')
    head = {}; prev = [-1] * n
    def ins(p):
        if p + 3 <= n:
            k = data[p:p+3]; prev[p] = head.get(k, -1); head[k] = p
    i = 0; lit_start = 0
    def flush_lits(upto, keep):
        # emit literals [lit_start, upto) leaving `keep` (0..3) for the match command
        nonlocal lit_start
        while upto - lit_start - keep >= 4:
            L = min(112, (upto - lit_start - keep) // 4 * 4)
            out.append(0xe0 + L // 4 - 1); out.extend(data[lit_start:lit_start+L]); lit_start += L
    while i < n:
        best_len = 0; best_off = 0
        if i + 3 <= n:
            c = head.get(data[i:i+3], -1); tries = 0
            while c >= 0 and tries < 64:
                off = i - c
                if off > 131072: break
                L = 0; mx = min(1028, n - i)
                while L < mx and data[c+L] == data[i+L]: L += 1
                if L > best_len: best_len, best_off = L, off
                c = prev[c]; tries += 1
        ok = best_len >= 3 and (best_off <= 1024 or best_len >= 4) and (best_off <= 16384 or best_len >= 5)
        if not ok:
            ins(i); i += 1; continue
        pending = i - lit_start
        flush_lits(i, pending % 4)
        lit = i - lit_start
        L, off = best_len, best_off
        if off <= 1024 and L <= 10:
            o = off - 1
            out += bytes([((o >> 3) & 0x60) | ((L - 3) << 2) | lit, o & 0xff])
        elif off <= 16384 and L <= 67:
            o = off - 1
            out += bytes([0x80 | (L - 4), (lit << 6) | (o >> 8), o & 0xff])
        else:
            L = min(L, 1028); o = off - 1
            out += bytes([0xc0 | ((o >> 12) & 0x10) | (((L - 5) >> 6) & 0x0c) | lit, (o >> 8) & 0xff, o & 0xff, (L - 5) & 0xff])
        out += data[lit_start:lit_start+lit]
        for p in range(i, i + L): ins(p)
        i += L; lit_start = i
    flush_lits(n, n - lit_start if (n - lit_start) < 4 else (n - lit_start) % 4)
    rem = n - lit_start
    out.append(0xfc + rem); out += data[lit_start:n]
    return bytes(out)

if __name__ == '__main__':
    import sys
    d = open(sys.argv[2], 'rb').read()
    open(sys.argv[3], 'wb').write(decompress(d) if sys.argv[1] == 'd' else compress(d))
