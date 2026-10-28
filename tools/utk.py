#!/usr/bin/env python3
"""EA MicroTalk (UTK) speech decoder, as used by MVP Baseball 2005's commentary (SPCH_PBP, SPCH_PA).

  python3 tools/utk.py <SLUS_211.35> <file.dat> <offset in bytes> out.wav      one recording -> WAV
  python3 tools/utk.py <SLUS_211.35> --heard <recording.wav> <game.iso>         which team / park recordings a
                                                                               recording of the game contains

A recording is its own little stream: SCHl (header: 24 kHz mono, codec 4), SCCl (block count), SCDl blocks, SCEl.
An SCDl block is: samples (u32), 0 (u32), one byte (1 in the first block: read the stream header bits), then one
frame per 432 samples. A frame starts with a marker byte (0xEE: raw samples follow the frame), then the bits of a
CELP frame read LSB first - 12 reflection coefficients, then 4 subframes of pitch lag (8 bits), pitch gain (4),
fixed gain (6) and the excitation (Huffman-coded pulses, or 1-2 bits per sample) - and ends on a byte boundary;
after an 0xEE frame come offset (s16 BE), count (s16 BE) and that many raw samples (s16 BE) that overwrite the
decoded ones (every recording starts with 216 of them, to cover the filter's start-up). The three tables
(reflection coefficients, Huffman codebooks, commands) are read from the game's executable, where its own decoder
(func_0051b6a0) keeps them at 0x7e5070, 0x7e5170 and 0x7e5370.
"""
import struct, numpy as np, sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
RC = CB = CMD = None
def load_tables(elf_path):
    """the decoder's tables, from the game's executable (file offset = address - 0xff000)"""
    global RC, CB, CMD
    elf = open(elf_path, 'rb').read()
    at = lambda v, n: elf[v - 0xff000: v - 0xff000 + n]
    RC = struct.unpack('<64f', at(0x7e5070, 256))
    CB = [at(0x7e5170, 256), at(0x7e5270, 256)]
    CMD = [struct.unpack('<iif', at(0x7e5370 + 12 * i, 12)) for i in range(29)]

class Bits:
    def __init__(s, data, pos=0):
        s.d = data; s.p = pos; s.v = s.d[s.p]; s.p += 1; s.c = 8
    def byte(s):
        b = s.d[s.p] if s.p < len(s.d) else 0
        s.p += 1; return b
    def read(s, n):
        r = s.v & ((1 << n) - 1); s.v >>= n; s.c -= n
        if s.c < 8: s.v |= s.byte() << s.c; s.c += 8
        return r

class Utk:
    def __init__(s):
        s.parsed = False; s.rc = [0.0] * 12; s.hist = [0.0] * 12; s.cb = np.zeros(324); s.params = []
    def header(s, b):
        s.reduced = b.read(1); s.thresh = 32 - b.read(4)
        g = 8.0 * (1 + b.read(4)); m = 1.04 + b.read(6) * 0.001
        s.gains = [g * m ** i for i in range(64)]
    def excitation(s, b, mp, out, base, stride):
        i = 0
        if mp:
            model = 0
            while i < 108:
                cmd = CB[model][b.v & 0xff]; model, nb, pulse = CMD[cmd]; b.read(nb)
                if cmd > 3: out[base + i] = pulse; i += stride
                elif cmd > 1:
                    cnt = 7 + b.read(6)
                    if i + cnt * stride > 108: cnt = (108 - i) // stride
                    while cnt > 0: out[base + i] = 0.0; i += stride; cnt -= 1
                else:
                    x = 7
                    while b.read(1): x += 1
                    if not b.read(1): x = -x
                    out[base + i] = float(x); i += stride
        else:
            while i < 108:
                if not b.read(1): out[base + i] = 0.0
                elif not b.read(1): out[base + i] = -2.0
                else: out[base + i] = 2.0
                i += stride
    def frame(s, b, synth=True):
        if not s.parsed: s.header(b); s.parsed = True
        exc = np.zeros(118); mp = 0; target = []
        for i in range(12):
            if i == 0:
                idx = b.read(6); mp = idx < s.thresh
            elif i < 4: idx = b.read(6)
            else: idx = 16 + b.read(5)
            target.append(RC[idx])
        delta = [(target[i] - s.rc[i]) * 0.25 for i in range(12)]
        fr = np.zeros(432); energy = 0.0
        for i in range(4):
            lag = b.read(8); pg = b.read(4) / 15.0; fg = s.gains[b.read(6)]
            if not s.reduced: s.excitation(b, mp, exc, 5, 1)
            else:
                align = b.read(1); zero = b.read(1)
                s.excitation(b, mp, exc, 5 + align, 2)
                o = 5 + (1 - align)
                if zero: exc[o:o + 108:2] = 0.0
                else:
                    for j in range(0, 108, 2):
                        p = o + j
                        exc[p] = (exc[p-5] + exc[p+5]) * 0.01803267933428287506103515625 - (exc[p-3] + exc[p+3]) * 0.114591561257839202880859375 + (exc[p-1] + exc[p+1]) * 0.597385942935943603515625
                    fg *= 0.5
            if synth:
                for j in range(108):
                    k = 108 * i + 216 - lag + j
                    past = s.cb[k] if k < 324 else fr[k - 324]
                    fr[108 * i + j] = fg * exc[5 + j] + pg * past
            energy += fg * fg * float(np.dot(exc[5:113], exc[5:113]))
        s.params.append((tuple(target), energy, mp))
        if synth:
            s.cb = fr[108:].copy()
            for i in range(4):
                for j in range(12): s.rc[j] += delta[j]
                s.synth(fr, 12 * i, 1 if i < 3 else 33)
        else:
            s.rc = list(target)
        return fr
    def synth(s, fr, off, blocks):
        rc = s.rc; lpc = [0.0] * 12; t1 = [0.0] * 12; t2 = [1.0] + list(rc[:11])
        for i in range(12):
            x = -t2[11] * rc[11]
            for j in range(10, -1, -1):
                x -= t2[j] * rc[j]; t2[j + 1] = x * rc[j] + t2[j]
            t1[i] = t2[0] = x
            for j in range(i): x -= t1[i - 1 - j] * lpc[j]
            lpc[i] = x
        h = s.hist; p = off
        for _ in range(blocks):
            for j in range(12):
                x = fr[p]
                for k in range(j): x += lpc[k] * h[k - j + 12]
                for k in range(j, 12): x += lpc[k] * h[k - j]
                h[11 - j] = x; fr[p] = x; p += 1

def decode_schl(data, off=0, synth=True):
    """returns (pcm float array, params, info)"""
    assert data[off:off + 4] == b'SCHl', data[off:off + 4]
    p = off; u = Utk(); out = []; info = []
    while p < len(data):
        tag = data[p:p + 4]; size = struct.unpack('<I', data[p + 4:p + 8])[0]
        if tag == b'SCDl':
            n = struct.unpack('<I', data[p + 8:p + 12])[0]
            q = p + 17; got = 0
            if data[p + 16]: u.parsed = False
            while got < n:
                pcm_present = data[q] == 0xee
                b = Bits(data, q + 1); fr = u.frame(b, synth); q = b.p - 1
                if pcm_present:
                    o, c = struct.unpack('>hh', data[q:q + 4]); q += 4
                    fr[o:o + c] = np.frombuffer(data[q:q + 2 * c], '>i2'); q += 2 * c
                    info.append(('pcm', o, c))
                out.append(fr); got += 432
            info.append((size - 16, q - (p + 16), n))
        elif tag == b'SCEl': break
        elif tag not in (b'SCHl', b'SCCl'): break
        p += size
    return (np.concatenate(out) if out else np.zeros(0)), u.params, info

def write_wav(path, pcm, rate=24000):
    import wave
    w = wave.open(path, 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
    w.writeframes(np.clip(np.round(pcm), -32768, 32767).astype('<i2').tobytes()); w.close()

# ---------------------------------------------------------------- what did the commentators say?
def find(x, sr, clip, csr=24000):
    """best normalised correlation of a recording inside the mono signal x: (score 0..1, time in seconds)"""
    if sr != csr:
        t = np.arange(int(len(clip) * sr / csr)) * csr / sr; clip = np.interp(t, np.arange(len(clip)), clip)
    clip = clip - clip.mean(); n = len(clip)
    N = 1 << int(np.ceil(np.log2(len(x) + n)))
    c = np.fft.irfft(np.fft.rfft(x, N) * np.conj(np.fft.rfft(clip, N)), N)[:len(x) - n]
    cs = np.concatenate([[0], np.cumsum(x.astype(np.float64) ** 2)])
    e = np.sqrt(np.maximum(cs[n:n + len(c)] - cs[:len(c)], (0.05 * np.linalg.norm(clip)) ** 2))
    s = c / (e * np.linalg.norm(clip)); i = int(np.argmax(s)); return float(s[i]), i / sr

def index_entries(h):
    """offsets (256-byte units) and codes of one .hdr: byte 4 = entry layout, byte 5 = count"""
    fmt, n = h[4], h[5]; size = (2, 3, 4)[fmt]
    for i in range(n):
        e = h[12 + size * i:12 + size * (i + 1)]
        yield struct.unpack('>H', e[:2])[0], (0 if fmt == 0 else e[2] if fmt == 1 else struct.unpack('>H', e[2:4])[0])

def heard(wav_path, iso):
    """prints the team and park recordings (SPCH_PBP) found in a recording made with MVP_WAVOUT=file.wav"""
    import wave, isofile
    from bigfile import read_big
    w = wave.open(wav_path, 'rb'); sr = w.getframerate()
    x = np.frombuffer(w.readframes(w.getnframes()), '<i2').astype(np.float32).reshape(-1, w.getnchannels()).mean(1)
    for kind in ('TNAME', 'STDNM'):
        H = isofile.read_file(iso, 'DATA/AUDIO/SPCH_PBP/%sHDR.BIG' % kind); D = isofile.read_file(iso, 'DATA/AUDIO/SPCH_PBP/%sDAT.BIG' % kind)
        dat = {n: D[o:o + s] for n, o, s in read_big(D)}
        for n, o, s in read_big(H):
            for off, code in index_entries(H[o:o + s]):
                pcm = decode_schl(dat[n[:-4] + '.dat'], off * 256)[0]
                score, t = find(x, sr, pcm)
                if score > 0.5: print('%6.2f s  %s %s code %04x  (%.2f s long, match %.2f)' % (t, kind, n[:-4], code, len(pcm) / 24000, score)); sys.stdout.flush()

if __name__ == '__main__':
    load_tables(sys.argv[1])
    if sys.argv[2] == '--heard': heard(sys.argv[3], sys.argv[4])
    else:
        pcm, params, info = decode_schl(open(sys.argv[2], 'rb').read(), int(sys.argv[3], 0))
        write_wav(sys.argv[4], pcm); print('%d samples (%.2f s) -> %s' % (len(pcm), len(pcm) / 24000, sys.argv[4]))
