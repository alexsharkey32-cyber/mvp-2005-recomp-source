"""Minimal Emotion Engine (R5900) disassembler + xref helper for SLUS_211.35.
No external deps. Covers the integer ISA, COP0/COP1, and names MMI/COP2 ops generically.

Usage:
  python3 tools/eedis.py dis 0x100008 [count]      disassemble
  python3 tools/eedis.py func 0x123456            disassemble whole function containing addr
  python3 tools/eedis.py xref 0x7d3300            find lui/addiu|ori|load pairs that build this address
  python3 tools/eedis.py str "gWidescreenMode"    find string, then xref it
  python3 tools/eedis.py calls 0x123456           find jal callers of a function
"""
import struct, sys, os

ELF = os.path.join(os.path.dirname(__file__), '..', 'game', 'SLUS_211.35')
DATA = open(ELF, 'rb').read()
BASE_OFF, BASE_VA = 0x1000, 0x100000          # single LOAD segment, linear mapping
LOAD_END = BASE_VA + 0x7bfc8c
CODE_RANGES = [(0x100000, 0x5e28a4), (0x5e28a8, 0x5e28a8 + 0x136c), (0x5e3c18, 0x5e3c18 + 0x1094),
               (0x5e4cb0, 0x5e4cb0 + 0xf102c), (0x6d5ce0, 0x6d5ce0 + 0x8894c), (0x75e630, 0x75e630 + 0x74c90)]

def va2off(va): return va - BASE_VA + BASE_OFF
def word(va): return struct.unpack_from('<I', DATA, va2off(va))[0]
def u32(va): return word(va)
def f32(va): return struct.unpack_from('<f', DATA, va2off(va))[0]
def cstr(va, n=128):
    o = va2off(va); e = DATA.find(b'\0', o, o + n); return DATA[o:e].decode('latin1')
def in_code(va): return any(a <= va < b for a, b in CODE_RANGES)

R = ['zero','at','v0','v1','a0','a1','a2','a3','t0','t1','t2','t3','t4','t5','t6','t7',
     's0','s1','s2','s3','s4','s5','s6','s7','t8','t9','k0','k1','gp','sp','fp','ra']
def r(n): return '$' + R[n]
def f(n): return '$f%d' % n
def s16(x): return x - 0x10000 if x & 0x8000 else x

SPECIAL = {0:'sll',2:'srl',3:'sra',4:'sllv',6:'srlv',7:'srav',8:'jr',9:'jalr',10:'movz',11:'movn',
    12:'syscall',13:'break',15:'sync',16:'mfhi',17:'mthi',18:'mflo',19:'mtlo',20:'dsllv',22:'dsrlv',
    23:'dsrav',24:'mult',25:'multu',26:'div',27:'divu',32:'add',33:'addu',34:'sub',35:'subu',36:'and',
    37:'or',38:'xor',39:'nor',40:'mfsa',41:'mtsa',42:'slt',43:'sltu',44:'dadd',45:'daddu',46:'dsub',
    47:'dsubu',56:'dsll',58:'dsrl',59:'dsra',60:'dsll32',62:'dsrl32',63:'dsra32'}
IMM = {8:'addi',9:'addiu',10:'slti',11:'sltiu',12:'andi',13:'ori',14:'xori',24:'daddi',25:'daddiu'}
BR = {4:'beq',5:'bne',6:'blez',7:'bgtz',20:'beql',21:'bnel',22:'blezl',23:'bgtzl'}
MEM = {32:'lb',33:'lh',34:'lwl',35:'lw',36:'lbu',37:'lhu',38:'lwr',39:'lwu',40:'sb',41:'sh',42:'swl',
    43:'sw',44:'sdl',45:'sdr',46:'swr',47:'cache',26:'ldl',27:'ldr',30:'lq',31:'sq',49:'lwc1',51:'pref',
    54:'lqc2',55:'ld',57:'swc1',62:'sqc2',63:'sd'}
REGIMM = {0:'bltz',1:'bgez',2:'bltzl',3:'bgezl',16:'bltzal',17:'bgezal',18:'bltzall',19:'bgezall'}
FPU_S = {0:'add.s',1:'sub.s',2:'mul.s',3:'div.s',4:'sqrt.s',5:'abs.s',6:'mov.s',7:'neg.s',22:'rsqrt.s',
    24:'adda.s',25:'suba.s',26:'mula.s',28:'madd.s',29:'msub.s',30:'madda.s',31:'msuba.s',36:'cvt.w.s',
    40:'max.s',41:'min.s',48:'c.f.s',50:'c.eq.s',52:'c.lt.s',54:'c.le.s'}

def dis(va, w=None):
    if w is None: w = word(va)
    op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31; rd = (w >> 11) & 31
    sa = (w >> 6) & 31; fn = w & 63; imm = w & 0xffff; si = s16(imm)
    if w == 0: return 'nop'
    if op == 0:
        m = SPECIAL.get(fn, 'special.%d' % fn)
        if fn in (0,2,3,56,58,59,60,62,63): return '%s %s, %s, %d' % (m, r(rd), r(rt), sa)
        if fn in (4,6,7,20,22,23): return '%s %s, %s, %s' % (m, r(rd), r(rt), r(rs))
        if fn == 8: return 'jr %s' % r(rs)
        if fn == 9: return 'jalr %s, %s' % (r(rd), r(rs))
        if fn in (12,13,15): return m
        if fn in (16,18,40): return '%s %s' % (m, r(rd))
        if fn in (17,19,41): return '%s %s' % (m, r(rs))
        if fn in (24,25,26,27): return '%s %s, %s, %s' % (m, r(rd), r(rs), r(rt)) if rd else '%s %s, %s' % (m, r(rs), r(rt))
        if fn == 37 and rt == 0: return 'move %s, %s' % (r(rd), r(rs))
        if fn == 45 and rt == 0: return 'move %s, %s' % (r(rd), r(rs))
        return '%s %s, %s, %s' % (m, r(rd), r(rs), r(rt))
    if op == 1:
        return '%s %s, 0x%x' % (REGIMM.get(rt, 'regimm.%d' % rt), r(rs), va + 4 + si * 4)
    if op in (2, 3):
        return '%s 0x%x' % ('j' if op == 2 else 'jal', ((va + 4) & 0xf0000000) | ((w & 0x3ffffff) << 2))
    if op in BR:
        if op in (4,) and rs == 0 and rt == 0: return 'b 0x%x' % (va + 4 + si * 4)
        if op in (6,7,22,23): return '%s %s, 0x%x' % (BR[op], r(rs), va + 4 + si * 4)
        return '%s %s, %s, 0x%x' % (BR[op], r(rs), r(rt), va + 4 + si * 4)
    if op in IMM:
        if op == 9 and rs == 0: return 'li %s, %d' % (r(rt), si)
        if op in (12,13,14): return '%s %s, %s, 0x%x' % (IMM[op], r(rt), r(rs), imm)
        return '%s %s, %s, %d' % (IMM[op], r(rt), r(rs), si)
    if op == 15: return 'lui %s, 0x%x' % (r(rt), imm)
    if op in MEM:
        m = MEM[op]
        reg = f(rt) if op in (49, 57) else ('$vf%d' % rt if op in (54, 62) else r(rt))
        if op == 47: reg = '0x%x' % rt
        return '%s %s, %d(%s)' % (m, reg, si, r(rs))
    if op == 16:
        if rs == 0: return 'mfc0 %s, $%d' % (r(rt), rd)
        if rs == 4: return 'mtc0 %s, $%d' % (r(rt), rd)
        if w == 0x42000018: return 'eret'
        if w == 0x42000038: return 'ei'
        if w == 0x42000039: return 'di'
        return 'cop0 0x%x' % w
    if op == 17:
        if rs == 0: return 'mfc1 %s, %s' % (r(rt), f(rd))
        if rs == 2: return 'cfc1 %s, $%d' % (r(rt), rd)
        if rs == 4: return 'mtc1 %s, %s' % (r(rt), f(rd))
        if rs == 6: return 'ctc1 %s, $%d' % (r(rt), rd)
        if rs == 8: return '%s 0x%x' % (['bc1f','bc1t','bc1fl','bc1tl'][rt & 3], va + 4 + si * 4)
        fd = sa; fs = rd; ft = rt
        if rs == 16:
            m = FPU_S.get(fn, 'fpu.s.%d' % fn)
            if fn in (4,5,6,7,36): return '%s %s, %s' % (m, f(fd), f(fs)) if fn != 4 else '%s %s, %s' % (m, f(fd), f(ft))
            if fn >= 48 or fn in (24,25,26,30,31): return '%s %s, %s' % (m, f(fs), f(ft))
            return '%s %s, %s, %s' % (m, f(fd), f(fs), f(ft))
        if rs == 20 and fn == 32: return 'cvt.s.w %s, %s' % (f(fd), f(fs))
        return 'cop1 0x%x' % w
    if op == 18:
        if rs == 1: return 'qmfc2 %s, $vf%d' % (r(rt), rd)
        if rs == 5: return 'qmtc2 %s, $vf%d' % (r(rt), rd)
        return 'cop2 0x%08x' % w
    if op == 28: return 'mmi.%d.%d %s, %s, %s' % (fn, sa, r(rd), r(rs), r(rt))
    return '.word 0x%08x' % w

def targets_addr(w, va):
    op = w >> 26
    if op == 3: return ((va + 4) & 0xf0000000) | ((w & 0x3ffffff) << 2)
    return None

# ---------------- function boundaries ----------------
_funcs = None
def functions():
    """Heuristic: function starts = jal targets + instruction after 'jr ra; delay' followed by addiu sp,-N."""
    global _funcs
    if _funcs is not None: return _funcs
    starts = set()
    for a, b in CODE_RANGES:
        n = (b - a) // 4
        ws = struct.unpack_from('<%dI' % n, DATA, va2off(a))
        for i, w in enumerate(ws):
            if w >> 26 == 3:
                t = ((a + 4*i + 4) & 0xf0000000) | ((w & 0x3ffffff) << 2)
                if in_code(t): starts.add(t)
            if w == 0x03e00008 and i + 2 < n:
                j = i + 2
                while j < n and ws[j] == 0: j += 1
                if j < n: starts.add(a + 4 * j)
    _funcs = sorted(starts)
    return _funcs

def func_containing(va):
    import bisect
    fs = functions(); i = bisect.bisect_right(fs, va) - 1
    start = fs[i]; end = fs[i + 1] if i + 1 < len(fs) else start + 4
    return start, end

# ---------------- xrefs ----------------
def scan_refs(want=None):
    """Linear scan tracking lui values per register; yields (va, target_address).
    Register state resets at each function start, so this is approximate but works well
    for GCC's lui/addiu, lui/lw, lui/ori pairs."""
    fset = set(functions())
    for a, b in CODE_RANGES:
        n = (b - a) // 4
        ws = struct.unpack_from('<%dI' % n, DATA, va2off(a))
        hi = {}
        for i, w in enumerate(ws):
            va = a + 4 * i
            if va in fset: hi = {}
            op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31
            if op == 15: hi[rt] = (w & 0xffff) << 16; continue
            if rs in hi and (op in (9, 25) or op in MEM):
                t = (hi[rs] + s16(w & 0xffff)) & 0xffffffff
                if want is None or t == want or (isinstance(want, tuple) and want[0] <= t < want[1]):
                    yield va, t
                if op in (9, 25) and rt != rs: pass
            elif rs in hi and op == 13:
                t = hi[rs] | (w & 0xffff)
                if want is None or t == want: yield va, t
            # invalidate overwritten regs (rough)
            if op in (9, 13, 25, 35, 55, 32, 33, 36, 37) and rt in hi and not (op in (9,13,25) and rs == rt):
                hi.pop(rt, None)
            if op == 0:
                rd = (w >> 11) & 31; hi.pop(rd, None)
            if op in (9, 13, 25) and rs == rt and rs in hi: hi.pop(rt, None)

def find_string(s):
    b = s.encode() + b'\0'; out = []; o = 0
    while True:
        o = DATA.find(b, o)
        if o < 0: break
        if o == 0 or DATA[o - 1] == 0 or True: out.append(o - BASE_OFF + BASE_VA)
        o += 1
    return out

def callers(fva):
    out = []
    for a, b in CODE_RANGES:
        n = (b - a) // 4
        ws = struct.unpack_from('<%dI' % n, DATA, va2off(a))
        want = 0x0c000000 | ((fva >> 2) & 0x3ffffff)
        for i, w in enumerate(ws):
            if w == want: out.append(a + 4 * i)
    return out

def print_dis(start, count):
    for k in range(count):
        va = start + 4 * k; w = word(va)
        print('%08x: %08x  %s' % (va, w, dis(va, w)))

if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'dis': print_dis(int(sys.argv[2], 16), int(sys.argv[3]) if len(sys.argv) > 3 else 32)
    elif cmd == 'func':
        s, e = func_containing(int(sys.argv[2], 16)); print('func_%08x (%d instrs)' % (s, (e - s) // 4)); print_dis(s, (e - s) // 4)
    elif cmd == 'xref':
        t = int(sys.argv[2], 16)
        for va, _ in scan_refs(t): print('%08x in func_%08x: %s' % (va, func_containing(va)[0], dis(va)))
    elif cmd == 'str':
        for sva in find_string(sys.argv[2]):
            print('string @ %08x' % sva)
            for va, _ in scan_refs(sva): print('  ref %08x in func_%08x' % (va, func_containing(va)[0]))
    elif cmd == 'calls':
        for va in callers(int(sys.argv[2], 16)): print('%08x in func_%08x' % (va, func_containing(va)[0]))
    elif cmd == 'nfuncs': print(len(functions()))
