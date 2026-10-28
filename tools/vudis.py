#!/usr/bin/env python3
"""VU1 micro-mode disassembler (matches runtime/src/vu1.cpp's decoding).
usage: vudis.py snapshot.bin start_pc [count]   (snapshot = 16 KB micro memory, e.g. from MVP_VU1SNAP)"""
import struct, sys

DEST = lambda w: ''.join(c for i, c in enumerate('xyzw') if (w >> 21) & (8 >> i))
BC = 'xyzw'

def upper(w):
    dest, ft, fs, fd, fn = (w >> 21) & 15, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    d = '.' + DEST(w)
    flags = ('[I]' if w & 0x80000000 else '') + ('[E]' if w & 0x40000000 else '') + ('[M]' if w & 0x20000000 else '') + \
            ('[D]' if w & 0x10000000 else '') + ('[T]' if w & 0x08000000 else '')
    if fn < 60:
        if fn < 28:
            op = ['ADD', 'SUB', 'MADD', 'MSUB', 'MAX', 'MINI', 'MUL'][fn >> 2] + BC[fn & 3]
            return f'{op}{d} vf{fd}, vf{fs}, vf{ft}{BC[fn & 3]}' + flags
        names = {28: 'MULq', 29: 'MAXi', 30: 'MULi', 31: 'MINIi', 32: 'ADDq', 33: 'MADDq', 34: 'ADDi', 35: 'MADDi',
                 36: 'SUBq', 37: 'MSUBq', 38: 'SUBi', 39: 'MSUBi', 40: 'ADD', 41: 'MADD', 42: 'MUL', 43: 'MAX',
                 44: 'SUB', 45: 'MSUB', 46: 'OPMSUB', 47: 'MINI'}
        n = names.get(fn, f'?up{fn}')
        src = f'vf{ft}' if fn >= 40 else ('Q' if 'q' in n else 'I')
        return f'{n}{d} vf{fd}, vf{fs}, {src}' + flags
    op = ((w >> 6) & 31) * 4 + (fn & 3)
    bc = BC[op & 3]
    if op < 16: return f"{['ADDA', 'SUBA', 'MADDA', 'MSUBA'][op >> 2]}{bc}{d} ACC, vf{fs}, vf{ft}{bc}" + flags
    if op < 20: return f'ITOF{[0, 4, 12, 15][op - 16]}{d} vf{ft}, vf{fs}' + flags
    if op < 24: return f'FTOI{[0, 4, 12, 15][op - 20]}{d} vf{ft}, vf{fs}' + flags
    if op < 28: return f'MULA{bc}{d} ACC, vf{fs}, vf{ft}{bc}' + flags
    names = {28: 'MULAq', 29: 'ABS', 30: 'MULAi', 31: 'CLIP', 32: 'ADDAq', 33: 'MADDAq', 34: 'ADDAi', 35: 'MADDAi',
             36: 'SUBAq', 37: 'MSUBAq', 38: 'SUBAi', 39: 'MSUBAi', 40: 'ADDA', 41: 'MADDA', 42: 'MULA', 44: 'SUBA',
             45: 'MSUBA', 46: 'OPMULA', 47: 'NOP'}
    n = names.get(op, f'?ups{op}')
    if n == 'NOP': return 'NOP' + flags
    if n == 'CLIP': return f'CLIPw.xyz vf{fs}, vf{ft}w' + flags
    if n == 'ABS': return f'ABS{d} vf{ft}, vf{fs}' + flags
    return f'{n}{d} ACC, vf{fs}, vf{ft}' + flags

def imm11(w):
    v = w & 0x7ff
    return v - 0x800 if v & 0x400 else v

def lower(w, pc):
    dest, ft, fs, fd = (w >> 21) & 15, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    it, is_, id_ = ft & 15, fs & 15, fd & 15
    d = '.' + DEST(w)
    if w & 0x80000000:
        fn = w & 63
        simple = {0x30: f'IADD vi{id_}, vi{is_}, vi{it}', 0x31: f'ISUB vi{id_}, vi{is_}, vi{it}',
                  0x34: f'IAND vi{id_}, vi{is_}, vi{it}', 0x35: f'IOR vi{id_}, vi{is_}, vi{it}'}
        if fn in simple: return simple[fn]
        if fn == 0x32:
            imm = (w >> 6) & 31
            if imm & 16: imm -= 32
            return f'IADDI vi{it}, vi{is_}, {imm}'
        if fn < 0x3c: return 'NOP' if w == 0x8000033c else f'?lo{w:08x}'
        op = ((w >> 6) & 31) * 4 + (fn & 3)
        fsf, ftf = BC[(w >> 21) & 3], BC[(w >> 23) & 3]
        t = {48: f'MOVE{d} vf{ft}, vf{fs}', 49: f'MR32{d} vf{ft}, vf{fs}', 52: f'LQI{d} vf{ft}, (vi{is_}++)',
             53: f'SQI{d} vf{fs}, (vi{it}++)', 54: f'LQD{d} vf{ft}, (--vi{is_})', 55: f'SQD{d} vf{fs}, (--vi{it})',
             56: f'DIV Q, vf{fs}{fsf}, vf{ft}{ftf}', 57: f'SQRT Q, vf{ft}{ftf}', 58: f'RSQRT Q, vf{fs}{fsf}, vf{ft}{ftf}',
             59: 'WAITQ', 60: f'MTIR vi{it}, vf{fs}{fsf}', 61: f'MFIR{d} vf{ft}, vi{is_}', 62: f'ILWR.{DEST(w)} vi{it}, (vi{is_})',
             63: f'ISWR.{DEST(w)} vi{it}, (vi{is_})', 64: f'RNEXT{d} vf{ft}, R', 65: f'RGET{d} vf{ft}, R',
             66: f'RINIT R, vf{fs}{fsf}', 67: f'RXOR R, vf{fs}{fsf}', 100: f'MFP{d} vf{ft}, P', 104: f'XTOP vi{it}',
             105: f'XITOP vi{it}', 108: f'XGKICK vi{is_}', 112: f'ESADD P, vf{fs}', 113: f'ERSADD P, vf{fs}',
             114: f'ELENG P, vf{fs}', 115: f'ERLENG P, vf{fs}', 116: f'EATANxy P, vf{fs}', 117: f'EATANxz P, vf{fs}',
             118: f'ESUM P, vf{fs}', 120: f'ESQRT P, vf{fs}{fsf}', 121: f'ERSQRT P, vf{fs}{fsf}', 122: f'ERCPR P, vf{fs}{fsf}',
             123: 'WAITP', 124: f'ESIN P, vf{fs}{fsf}', 125: f'EATAN P, vf{fs}{fsf}', 126: f'EEXP P, vf{fs}{fsf}'}
        return t.get(op, f'?los{op}')
    op = w >> 25
    i11 = imm11(w)
    tgt = ((pc // 8) + 1 + i11) * 8
    i15 = ((w >> 10) & 0x7800) | (w & 0x7ff)
    i12 = ((w >> 10) & 0x800) | (w & 0x7ff)
    t = {0x00: f'LQ{d} vf{ft}, {i11}(vi{is_})', 0x01: f'SQ{d} vf{fs}, {i11}(vi{it})',
         0x04: f'ILW.{DEST(w)} vi{it}, {i11}(vi{is_})', 0x05: f'ISW.{DEST(w)} vi{it}, {i11}(vi{is_})',
         0x08: f'IADDIU vi{it}, vi{is_}, {i15}', 0x09: f'ISUBIU vi{it}, vi{is_}, {i15}',
         0x10: f'FCEQ vi1, 0x{w & 0xffffff:06x}', 0x11: f'FCSET 0x{w & 0xffffff:06x}', 0x12: f'FCAND vi1, 0x{w & 0xffffff:06x}',
         0x13: f'FCOR vi1, 0x{w & 0xffffff:06x}', 0x14: f'FSEQ vi{it}, 0x{i12:03x}', 0x15: f'FSSET 0x{i12:03x}',
         0x16: f'FSAND vi{it}, 0x{i12:03x}', 0x17: f'FSOR vi{it}, 0x{i12:03x}', 0x18: f'FMEQ vi{it}, vi{is_}',
         0x1a: f'FMAND vi{it}, vi{is_}', 0x1b: f'FMOR vi{it}, vi{is_}', 0x1c: f'FCGET vi{it}',
         0x20: f'B 0x{tgt:04x}', 0x21: f'BAL vi{it}, 0x{tgt:04x}', 0x24: f'JR vi{is_}', 0x25: f'JALR vi{it}, vi{is_}',
         0x28: f'IBEQ vi{it}, vi{is_}, 0x{tgt:04x}', 0x29: f'IBNE vi{it}, vi{is_}, 0x{tgt:04x}',
         0x2c: f'IBLTZ vi{is_}, 0x{tgt:04x}', 0x2d: f'IBGTZ vi{is_}, 0x{tgt:04x}', 0x2e: f'IBLEZ vi{is_}, 0x{tgt:04x}',
         0x2f: f'IBGEZ vi{is_}, 0x{tgt:04x}'}
    return t.get(op, f'?lo{op:02x}')

def main():
    code = open(sys.argv[1], 'rb').read()[:0x4000]
    pc = int(sys.argv[2], 16)
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 200
    for _ in range(n):
        lo, up = struct.unpack_from('<II', code, pc)
        u = upper(up)
        l = f'LOI 0x{lo:08x} ({struct.unpack("<f", struct.pack("<I", lo))[0]:g})' if up & 0x80000000 else lower(lo, pc)
        print(f'{pc:04x}: {u:<44} {l}')
        pc = (pc + 8) & 0x3ff8

if __name__ == '__main__':
    main()
