#!/usr/bin/env python3
"""
MVP Baseball 2005 (SLUS_211.35) — Emotion Engine static recompiler.

Translates every MIPS R5900 function in the executable into C++ that runs natively.
Each guest function becomes `void f_XXXXXXXX(EEContext* c)`. Instruction semantics are
macros in runtime/include/ee_ops.h, so this script only has to handle control flow.

Usage: python3 tools/recomp/ee_recomp.py [--elf game/SLUS_211.35] [--out generated]
"""
import argparse, bisect, os, struct, sys
from collections import defaultdict

# ----------------------------------------------------------------------------- ELF
class Elf:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        d = self.data
        assert d[:4] == b'\x7fELF'
        self.entry = struct.unpack_from('<I', d, 24)[0]
        phoff, shoff = struct.unpack_from('<II', d, 28)
        phnum = struct.unpack_from('<H', d, 44)[0]
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 46)
        self.segs = []
        for i in range(phnum):
            t, off, va, pa, fsz, msz, fl, al = struct.unpack_from('<8I', d, phoff + 32 * i)
            if t == 1: self.segs.append((va, off, fsz, msz))
        strtab_off = struct.unpack_from('<I', d, shoff + shstrndx * shentsize + 16)[0]
        self.sections = []
        for i in range(shnum):
            nm, typ, flg, addr, off, size = struct.unpack_from('<6I', d, shoff + i * shentsize)
            e = d.index(b'\0', strtab_off + nm)
            name = d[strtab_off + nm:e].decode()
            self.sections.append((name, typ, flg, addr, off, size))
        # executable sections = code
        self.code = [(a, a + s) for (n, t, f, a, o, s) in self.sections if (f & 4) and s > 0]
        self.data_secs = [(a, a + s) for (n, t, f, a, o, s) in self.sections
                          if (f & 2) and not (f & 4) and t == 1 and s > 0]

    def off(self, va):
        for v, o, fs, ms in self.segs:
            if v <= va < v + fs: return o + va - v
        return None

    def word(self, va):
        o = self.off(va)
        return struct.unpack_from('<I', self.data, o)[0] if o is not None else 0

    def in_code(self, va):
        return any(a <= va < b for a, b in self.code)

    def code_end(self, va):
        for a, b in self.code:
            if a <= va < b: return b
        return va

# ----------------------------------------------------------------------------- decode helpers
def s16(x): return x - 0x10000 if x & 0x8000 else x
def f_rs(w): return (w >> 21) & 31
def f_rt(w): return (w >> 16) & 31
def f_rd(w): return (w >> 11) & 31
def f_sa(w): return (w >> 6) & 31
def f_fn(w): return w & 63
def f_op(w): return w >> 26

JR_RA = 0x03e00008

def branch_info(w, pc):
    """Returns (kind, target) for control-flow instructions, else None.
    kinds: 'b' conditional, 'bl' likely, 'j', 'jal', 'jr', 'jalr', 'bal' (branch-and-link)"""
    op = f_op(w); rs = f_rs(w); rt = f_rt(w)
    tgt = (pc + 4 + (s16(w & 0xffff) << 2)) & 0xffffffff
    if op == 2: return ('j', ((pc + 4) & 0xf0000000) | ((w & 0x3ffffff) << 2))
    if op == 3: return ('jal', ((pc + 4) & 0xf0000000) | ((w & 0x3ffffff) << 2))
    if op in (4, 5, 6, 7): return ('b', tgt)
    if op in (20, 21, 22, 23): return ('bl', tgt)
    if op == 1:
        if rt in (0, 1): return ('b', tgt)
        if rt in (2, 3): return ('bl', tgt)
        if rt in (16, 17): return ('bal', tgt)
        if rt in (18, 19): return ('bl', tgt)       # bltzall/bgezall: treat as likely+link
    if op == 0 and f_fn(w) == 8: return ('jr', None)
    if op == 0 and f_fn(w) == 9: return ('jalr', None)
    if op == 17 and rs == 8: return ('bl' if rt & 2 else 'b', tgt)   # bc1f/t(l)
    if op == 16 and rs == 8: return ('bl' if rt & 2 else 'b', tgt)   # bc0
    if op == 18 and rs == 8: return ('bl' if rt & 2 else 'b', tgt)   # bc2
    return None

# ----------------------------------------------------------------------------- function discovery
def discover(elf):
    jal_targets = set()
    after_ret = set()
    for a, b in elf.code:
        for pc in range(a, b, 4):
            w = elf.word(pc)
            if f_op(w) == 3:
                t = ((pc + 4) & 0xf0000000) | ((w & 0x3ffffff) << 2)
                if elf.in_code(t): jal_targets.add(t)
            if w == JR_RA:
                q = pc + 8
                while q < b and elf.word(q) == 0: q += 4
                if q < b: after_ret.add(q)
    # data pointers into code (vtables, callbacks, jump tables)
    ptrs = set()
    for a, b in elf.data_secs:
        for va in range(a, b - 3, 4):
            v = elf.word(va)
            if v & 3 == 0 and elf.in_code(v): ptrs.add(v)
    # addresses of code built in registers (lui/addiu pairs) = function pointers
    code_refs = set()
    for a, b in elf.code:
        hi = {}
        for pc in range(a, b, 4):
            w = elf.word(pc); op = f_op(w)
            if op == 15: hi[f_rt(w)] = (w & 0xffff) << 16; continue
            if op in (9, 13) and f_rs(w) in hi:
                v = (hi[f_rs(w)] + s16(w & 0xffff)) & 0xffffffff if op == 9 else hi[f_rs(w)] | (w & 0xffff)
                if v & 3 == 0 and elf.in_code(v): code_refs.add(v)
            if w == JR_RA: hi = {}
    starts = set(jal_targets) | {elf.entry} | code_refs
    for a, b in elf.code: starts.add(a)
    # data pointers that look like functions (vtables, callbacks, static-constructor lists)
    def is_prologue(p):
        w = elf.word(p)
        return f_op(w) == 9 and f_rs(w) == 29 and f_rt(w) == 29 and s16(w & 0xffff) < 0
    ptr_at = {}
    for a, b in elf.data_secs:
        for va in range(a, b - 3, 4):
            v = elf.word(va)
            if v & 3 == 0 and elf.in_code(v): ptr_at[va] = v
    for va, p in ptr_at.items():
        prev = elf.word(p - 8)
        isolated = (va - 4) not in ptr_at and (va + 4) not in ptr_at
        near_ret = False
        for k in range(8, 44, 4):                      # jr ra; delay; [padding / dead epilogue words]; p
            if elf.word(p - k) == JR_RA:
                near_ret = all(elf.word(q) == 0 or (elf.word(q) >> 16) == 0x27bd for q in range(p - k + 8, p, 4))
                break
        if p in after_ret or is_prologue(p) or near_ret:
            starts.add(p)
    # after-return addresses that begin with a stack frame are functions too
    for q in after_ret:
        w = elf.word(q)
        if f_op(w) == 9 and f_rs(w) == 29 and f_rt(w) == 29 and s16(w & 0xffff) < 0:
            starts.add(q)

    # Data stored inside a code section: the APT overlay (the online lobby's screen engine) keeps its read-only data
    # between its functions, so its switch tables and virtual-function tables are in no data section. Two ways to
    # tell such a word from an instruction:
    #  - it is the address of something in its own section and no valid instruction has that encoding
    #    (addresses decode as SPECIAL; with rs != 0 that needs a zero shift field and a real function code), or
    #    it sits within four words of one that is (tables of {offset, pointer} pairs);
    #  - a run of 4 or more words that all point a short way into the same section (instructions that happen to
    #    look like addresses, "and v0,v0,v1" = 0x00431024, come in ones and twos and point all over the place).
    SPECIAL_OK = {0x04, 0x06, 0x07, 0x08, 0x0a, 0x0b, 0x14, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
                  0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
                  0x30, 0x31, 0x32, 0x33, 0x34, 0x36}
    def could_be_insn(w):
        if f_rs(w) == 0: return (w & 63) in (0, 2, 3, 0x38, 0x3a, 0x3b, 0x3c, 0x3e, 0x3f)      # shifts by a constant
        return (w >> 6) & 31 == 0 and (w & 63) in SPECIAL_OK
    code_data_targets = set()
    for a, b in elf.code:
        cand = {va: elf.word(va) for va in range(a, b, 4) if elf.word(va) & 3 == 0 and a <= elf.word(va) < b}
        sure = {va for va, v in cand.items() if not could_be_insn(v)}
        for va, v in cand.items():
            if va in sure or any((va + d) in sure for d in (-16, -12, -8, -4, 4, 8, 12, 16)): code_data_targets.add(v)
        va = a
        while va < b:
            run = []
            while va in cand: run.append(cand[va]); va += 4
            if len(run) >= 4 and max(run) - min(run) < 0x10000: code_data_targets.update(run)
            if not run: va += 4
    for p in code_data_targets:
        near_ret = False
        for k in range(8, 44, 4):
            if elf.word(p - k) == JR_RA:
                near_ret = all(elf.word(q) == 0 or (elf.word(q) >> 16) == 0x27bd for q in range(p - k + 8, p, 4))
                break
        if p in after_ret or is_prologue(p) or near_ret: starts.add(p)

    # A function cannot begin in the delay slot of a branch. Such "starts" are numbers that happen to look like
    # code addresses (0x119400 = 1152000 built with lui/addiu) or branch targets shared with a delay slot. Cutting
    # a function there loses the fall-through path after the branch: the piece ends with a jump to an address that
    # is no entry point, the runtime skips it, and the function returns without its epilogue (stack pointer and
    # saved registers of every caller wrong from then on). They become entry labels inside their function instead.
    sec_starts = {a for a, b in elf.code}
    def in_delay_slot(p):          # (a called address or a stack frame set-up right after a bare "jr ra" is a real function)
        return (p not in sec_starts and p != elf.entry and p not in jal_targets and not is_prologue(p)
                and branch_info(elf.word(p - 4), p - 4) is not None)
    demoted = {p for p in starts if in_delay_slot(p)}
    starts -= demoted

    # Remaining after-return spots: new function unless an earlier branch in the region reaches past it
    ss = sorted(starts)
    for q in sorted(after_ret - starts):
        i = bisect.bisect_right(ss, q) - 1
        s = ss[i]
        reach = 0
        for pc in range(s, q, 4):
            bi = branch_info(elf.word(pc), pc)
            if bi and bi[0] in ('b', 'bl', 'j', 'bal') and bi[1] >= q and bi[1] < q + 0x10000:
                reach = 1; break
        if not reach and q not in ptrs:
            starts.add(q)
        elif not reach and q in ptrs:
            starts.add(q)
    # closure: branch/jump targets outside the current function become function starts
    changed = True
    while changed:
        changed = False
        ss = sorted(starts)
        for i, s in enumerate(ss):
            e = ss[i + 1] if i + 1 < len(ss) else elf.code_end(s)
            e = min(e, elf.code_end(s))
            for pc in range(s, e, 4):
                bi = branch_info(elf.word(pc), pc)
                if bi and bi[0] in ('b', 'bl', 'j', 'bal'):
                    t = bi[1]
                    if not (s <= t < e) and elf.in_code(t) and t not in starts:
                        if in_delay_slot(t): demoted.add(t)
                        else: starts.add(t); changed = True
    ss = sorted(starts)
    funcs = []
    for i, s in enumerate(ss):
        e = ss[i + 1] if i + 1 < len(ss) else elf.code_end(s)
        funcs.append((s, min(e, elf.code_end(s))))
    # jump tables: runs of >= 2 consecutive data words pointing into code. Every target that is not a
    # function start becomes a label (entry point) in whichever function piece contains it.
    fstartset = set(f[0] for f in funcs)
    labels = set()
    for a, b in elf.data_secs:
        va = a
        while va < b - 3:
            run = []
            while va < b - 3:
                v = elf.word(va)
                if v & 3 == 0 and elf.in_code(v): run.append(v); va += 4
                else: break
            # every code pointer found in data must be callable: function start or entry label
            labels.update(x for x in run if x not in fstartset)
            if not run: va += 4
    labels.update(x for x in demoted if x not in fstartset)
    labels.update(x for x in code_data_targets if x not in fstartset)
    # a piece that ends in a conditional branch whose delay slot is the next piece's first instruction falls through
    # to the instruction after that: it has to be an entry point too
    for s, e in funcs:
        bi = branch_info(elf.word(e - 4), e - 4) if e - 4 >= s else None
        if bi and bi[0] in ('b', 'bl', 'bal', 'jal', 'jalr') and (e + 4) not in fstartset and elf.in_code(e + 4): labels.add(e + 4)
    return funcs, labels

# ----------------------------------------------------------------------------- instruction translation
LOADS = {32: 'LB', 33: 'LH', 34: 'LWL', 35: 'LW', 36: 'LBU', 37: 'LHU', 38: 'LWR', 39: 'LWU',
         26: 'LDL', 27: 'LDR', 55: 'LD', 30: 'LQ'}
STORES = {40: 'SB', 41: 'SH', 42: 'SWL', 43: 'SW', 44: 'SDL', 45: 'SDR', 46: 'SWR', 63: 'SD', 31: 'SQ'}
SPECIAL = {
    0: 'SLL', 2: 'SRL', 3: 'SRA', 56: 'DSLL', 58: 'DSRL', 59: 'DSRA', 60: 'DSLL32', 62: 'DSRL32', 63: 'DSRA32',
    4: 'SLLV', 6: 'SRLV', 7: 'SRAV', 20: 'DSLLV', 22: 'DSRLV', 23: 'DSRAV',
    10: 'MOVZ', 11: 'MOVN', 16: 'MFHI', 17: 'MTHI', 18: 'MFLO', 19: 'MTLO', 40: 'MFSA', 41: 'MTSA',
    24: 'MULT', 25: 'MULTU', 26: 'DIV', 27: 'DIVU',
    32: 'ADDU', 33: 'ADDU', 34: 'SUBU', 35: 'SUBU', 36: 'AND', 37: 'OR', 38: 'XOR', 39: 'NOR',
    42: 'SLT', 43: 'SLTU', 44: 'DADDU', 45: 'DADDU', 46: 'DSUBU', 47: 'DSUBU'}
SHIFT_IMM = {0, 2, 3, 56, 58, 59, 60, 62, 63}
IMMS = {8: 'ADDIU', 9: 'ADDIU', 10: 'SLTI', 11: 'SLTIU', 12: 'ANDI', 13: 'ORI', 14: 'XORI',
        24: 'DADDIU', 25: 'DADDIU'}
MMI = {0: 'MADD', 1: 'MADDU', 16: 'MFHI1', 17: 'MTHI1', 18: 'MFLO1', 19: 'MTLO1',
       24: 'MULT1', 25: 'MULTU1', 26: 'DIV1', 27: 'DIVU1'}
MMI_SUB = {(9, 14): 'PCPYLD', (41, 14): 'PCPYUD', (41, 18): 'POR', (9, 18): 'PAND', (9, 19): 'PXOR',
           (41, 19): 'PNOR', (40, 16): 'PADDUW', (9, 8): 'PMFHI', (9, 9): 'PMFLO', (41, 8): 'PMTHI',
           (41, 9): 'PMTLO', (8, 18): 'PEXTLW', (40, 18): 'PEXTUW', (41, 27): 'PCPYH', (40, 27): 'QFSRV'}
FPU_S = {0: 'ADD_S', 1: 'SUB_S', 2: 'MUL_S', 3: 'DIV_S', 4: 'SQRT_S', 5: 'ABS_S', 6: 'MOV_S', 7: 'NEG_S',
         22: 'RSQRT_S', 24: 'ADDA_S', 25: 'SUBA_S', 26: 'MULA_S', 28: 'MADD_S', 29: 'MSUB_S',
         30: 'MADDA_S', 31: 'MSUBA_S', 36: 'CVT_W_S', 40: 'MAX_S', 41: 'MIN_S',
         48: 'C_F_S', 50: 'C_EQ_S', 52: 'C_LT_S', 54: 'C_LE_S'}

FPS_LUI_SITES = set()       # lui 60.0f / 30.0f used as "seconds -> frames" factors (see find_fps_sites)
def find_fps_sites(elf, funcs):
    """lui rt,0x4270 (60.0f) or 0x41f0 (30.0f) -> mtc1 rt,fX -> mul.s using fX -> jal 0x5C8BE0 (float->int):
    a duration in seconds converted to a frame count. Those factors follow the frame rate in real-time mode."""
    for s, e in funcs:
        for pc in range(s, e + 4, 4):
            w = elf.word(pc)
            if f_op(w) != 15 or (w & 0xffff) not in (0x4270, 0x41f0): continue
            rt = f_rt(w); fx = None; mul_at = None
            for k in range(1, 9):
                x = elf.word(pc + 4 * k)
                if fx is None and f_op(x) == 17 and f_rs(x) == 4 and f_rt(x) == rt: fx = f_rd(x); continue
                if fx is not None and f_op(x) == 17 and f_rs(x) == 16 and f_fn(x) == 2 and fx in (f_rd(x), f_rt(x)):
                    mul_at = pc + 4 * k; break
            if mul_at is None: continue
            for k in range(0, 4):
                x = elf.word(mul_at + 4 * k)
                if f_op(x) == 3 and ((x & 0x3ffffff) << 2) == 0x5c8be0: FPS_LUI_SITES.add(pc); break
            else:
                x = elf.word(mul_at - 4)
                if f_op(x) == 3 and ((x & 0x3ffffff) << 2) == 0x5c8be0: FPS_LUI_SITES.add(pc)

# Instructions whose immediate is a calendar year (tools/recomp/years.txt): the year the game's calendar starts in
# (2005) is added to at run time, g_year_shift = first year - 2005, so a modern roster's dynasty starts in its own year.
YEAR_SITES = {}                                             # pc -> 'i' (integer immediate) / 'f' (lui+ori of a float)
def xlate_year(w, pc):
    op = f_op(w); rs = f_rs(w); rt = f_rt(w); imm = w & 0xffff; si = s16(imm)
    kind = YEAR_SITES[pc]
    if kind == 'i':
        assert op in (9, 10, 11) and 1900 <= abs(si) <= 2200, 'years.txt: 0x%x is not a year immediate' % pc
        return '%s(%d,%d,%d %s g_year_shift);' % (IMMS[op], rt, rs, si, '+' if si > 0 else '-')
    if kind == 'f':                                         # lui rt, hi  (the ori that follows is listed as 'g')
        w2 = YEAR_SITES_WORDS[pc + 4]; assert op == 15 and f_op(w2) == 13 and f_rt(w2) == rt
        y = struct.unpack('<f', struct.pack('<I', (imm << 16) | (w2 & 0xffff)))[0]; assert y == int(y) and 1900 <= y <= 2200
        return 'LUI(%d, year_fbits(%d) >> 16);' % (rt, int(y))
    if kind == 'g':
        w1 = YEAR_SITES_WORDS[pc - 4]
        y = struct.unpack('<f', struct.pack('<I', ((w1 & 0xffff) << 16) | imm))[0]
        return 'ORI(%d,%d, year_fbits(%d) & 0xffff);' % (rt, rs, int(y))
    raise ValueError(kind)
YEAR_SITES_WORDS = {}

def xlate(w, pc):
    """Translate one non-branch instruction to a C++ statement."""
    if pc in YEAR_SITES: return xlate_year(w, pc)
    op = f_op(w); rs = f_rs(w); rt = f_rt(w); rd = f_rd(w); sa = f_sa(w); fn = f_fn(w)
    imm = w & 0xffff; si = s16(imm)
    if w == 0: return ''
    if op == 0:
        if fn == 12: return 'SYSCALL(0x%x);' % pc
        if fn == 13: return 'BREAK(0x%x);' % pc
        if fn == 15: return ''                              # sync
        if fn in SPECIAL:
            m = SPECIAL[fn]
            if m in ('MTHI', 'MTLO', 'MTSA'): return '%s(%d);' % (m, rs)
            if m in ('MULT', 'MULTU', 'DIV', 'DIVU'): return '%s(%d,%d,%d);' % (m, rd, rs, rt)
            if rd == 0: return ''
            if fn in SHIFT_IMM: return '%s(%d,%d,%d);' % (m, rd, rt, sa)
            if m in ('MFHI', 'MFLO', 'MFSA'): return '%s(%d);' % (m, rd)
            if fn in (4, 6, 7, 20, 22, 23): return '%s(%d,%d,%d);' % (m, rd, rt, rs)
            return '%s(%d,%d,%d);' % (m, rd, rs, rt)
    if op == 1 and rt == 24: return 'MTSAB(%d,%d);' % (rs, imm)   # funnel-shift amount (bytes)
    if op == 1 and rt == 25: return 'MTSAH(%d,%d);' % (rs, imm)
    if op in IMMS:
        if rt == 0: return ''
        # 1/60 s and 1/30 s float constants (lui 0x3c88/0x3d08 + ori 0x8889): the game's fixed simulation step.
        # ORI_DT substitutes the runtime step so the sim can keep real-time speed above 60 fps.
        # The main loop's frame bookkeeping (0x26A000-0x26BFFF, Game_StepAndTime and friends) converts vblank /
        # tick counts with 1/60 and must stay fixed: scaling it desynchronised the attract-demo state machine
        # (null virtual call at 0x2C98A4 after ~40 s at 120 fps).
        if op == 13 and imm == 0x8889 and not (0x26a000 <= pc < 0x26c000): return 'ORI_DT(%d,%d,%d,0x%x);' % (rt, rs, imm, pc)
        return '%s(%d,%d,%d);' % (IMMS[op], rt, rs, si if op not in (12, 13, 14) else imm)
    if op == 15:
        if rt and pc in FPS_LUI_SITES: return 'LUI_FPS(%d,0x%x);' % (rt, imm)
        return 'LUI(%d,0x%x);' % (rt, imm) if rt else ''
    if op in LOADS:
        if rt == 0 and op != 30: return 'TOUCH(%d,%d);' % (rs, si)
        return '%s(%d,%d,%d);' % (LOADS[op], rt, rs, si)
    if op in STORES: return '%s(%d,%d,%d);' % (STORES[op], rt, rs, si)
    if op == 49: return 'LWC1(%d,%d,%d);' % (rt, rs, si)
    if op == 57: return 'SWC1(%d,%d,%d);' % (rt, rs, si)
    if op == 54: return 'LQC2(%d,%d,%d);' % (rt, rs, si)
    if op == 62: return 'SQC2(%d,%d,%d);' % (rt, rs, si)
    if op == 47 or op == 51: return ''                     # cache, pref
    if op == 28:
        if fn in MMI:
            m = MMI[fn]
            if m in ('MTHI1', 'MTLO1'): return '%s(%d);' % (m, rs)
            if m in ('MFHI1', 'MFLO1'): return '%s(%d);' % (m, rd) if rd else ''
            return '%s(%d,%d,%d);' % (m, rd, rs, rt)
        key = (fn, sa)
        if key in MMI_SUB:
            m = MMI_SUB[key]
            if m in ('PMTHI', 'PMTLO'): return '%s(%d);' % (m, rs)
            if m in ('PMFHI', 'PMFLO'): return '%s(%d);' % (m, rd) if rd else ''
            if rd == 0: return ''
            return '%s(%d,%d,%d);' % (m, rd, rs, rt)
        return 'INTERP(0x%x,0x%08x);' % (pc, w)
    if op == 16:
        if rs == 0: return 'MFC0(%d,%d);' % (rt, rd) if rt else ''
        if rs == 4: return 'MTC0(%d,%d);' % (rt, rd)
        return 'COP0(0x%x,0x%08x);' % (pc, w)
    if op == 17:
        ft = rt; fs = rd; fd = sa
        if rs == 0: return 'MFC1(%d,%d);' % (rt, fs) if rt else ''
        if rs == 4: return 'MTC1(%d,%d);' % (rt, fs)
        if rs == 2: return 'CFC1(%d,%d);' % (rt, fs) if rt else ''
        if rs == 6: return 'CTC1(%d,%d);' % (rt, fs)
        if rs == 16 and fn in FPU_S: return '%s(%d,%d,%d);' % (FPU_S[fn], fd, fs, ft)
        if rs == 20 and fn == 32: return 'CVT_S_W(%d,%d,0);' % (fd, fs)
        return 'INTERP(0x%x,0x%08x);' % (pc, w)
    if op == 18:
        if rs == 1: return 'QMFC2(%d,%d);' % (rt, rd) if rt else ''
        if rs == 5: return 'QMTC2(%d,%d);' % (rt, rd)
        if rs == 2: return 'CFC2(%d,%d);' % (rt, rd) if rt else ''
        if rs == 6: return 'CTC2(%d,%d);' % (rt, rd)
        return 'VU0OP(0x%08x);' % w
    return 'INTERP(0x%x,0x%08x);' % (pc, w)

def cond_expr(w):
    op = f_op(w); rs = f_rs(w); rt = f_rt(w)
    R = lambda n: 'GPR_S(%d)' % n
    if op in (4, 20): return '%s == %s' % (R(rs), R(rt)) if not (rs == 0 and rt == 0) else 'true'
    if op in (5, 21): return '%s != %s' % (R(rs), R(rt))
    if op in (6, 22): return '%s <= 0' % R(rs)
    if op in (7, 23): return '%s > 0' % R(rs)
    if op == 1:
        k = rt & 1
        return '%s >= 0' % R(rs) if k else '%s < 0' % R(rs)
    if op == 17: return 'c->fcc' if rt & 1 else '!c->fcc'
    if op == 16: return 'COP0_COND(%d)' % (rt & 1)
    if op == 18: return 'VU0_COND(%d)' % (rt & 1)
    raise ValueError(hex(w))

# ----------------------------------------------------------------------------- function emission

def var_patch(nw, w, orig):
    """a float constant built by lui + ori, taken from a runtime variable instead while it is above 0"""
    v = nw[0]; op = w >> 26; rt = (w >> 16) & 31
    if op == 0x0f:
        return 'if (%s > 0.0f) { uint32_t _b; std::memcpy(&_b, &%s, 4); c->r[%d].SD[0] = (int32_t)_b; } else { %s }' % (v, v, rt, orig or ';')
    return 'if (!(%s > 0.0f)) { %s }' % (v, orig or ';')

class Gen:
    def __init__(self, elf, funcs, ptrs):
        self.elf = elf
        self.funcs = funcs
        self.starts = [s for s, e in funcs]
        self.startset = set(self.starts)
        self.ptrs = ptrs
        self.multi_entry = {}       # func start -> sorted list of extra entry labels
        self.stats = defaultdict(int)
        self.patches = {}
        self.waitloops = set()
        self.options = set()

    def fname(self, a): return 'f_%08x' % a

    def call_expr(self, t):
        if t in self.startset: return '%s(c);' % self.fname(t)
        return 'ee_call(c, 0x%x);' % t

    def emit_func(self, s, e, name=None):
        elf = self.elf
        words = {pc: elf.word(pc) for pc in range(s, e + 4, 4)}
        labels = set()
        for pc in range(s, e, 4):
            bi = branch_info(words[pc], pc)
            if bi and bi[1] is not None and s <= bi[1] < e: labels.add(bi[1])
        entry_labels = sorted(p for p in self.ptrs if s < p < e)   # jump-table targets (may be reached from a split sibling)
        labels |= set(entry_labels)
        has_jr_table = any(f_op(words[pc]) == 0 and f_fn(words[pc]) == 8 and f_rs(words[pc]) != 31
                           for pc in range(s, e, 4))
        out = []
        name = name or self.fname(s)
        if entry_labels:
            self.multi_entry[s] = entry_labels
            out.append('void %s_at(EEContext* c, uint32_t entry) {' % name)
        else:
            out.append('void %s(EEContext* c) {' % name)
        out.append('  uint32_t _jt = 0; (void)_jt;')
        out.append('  EE_TRACE(0x%x);' % s)
        if s == 0x26a580: out.append('  g_sim_ticks++;               // Game_StepAndTime: one simulation tick')
        if entry_labels:
            out.append('  switch (entry) {')
            for l in entry_labels: out.append('    case 0x%x: goto L_%x;' % (l, l))
            out.append('    default: break;')
            out.append('  }')

        need_dispatch = False
        pc = s
        terminal = False
        while pc < e:
            w = words[pc]
            if pc in labels: out.append('L_%x:;' % pc)
            bi = branch_info(w, pc)
            if bi is None and w != 0: terminal = False
            if bi is None:
                st = xlate(w, pc)
                if pc in self.patches:
                    nw, opt = self.patches[pc]
                    if isinstance(nw, tuple): st = var_patch(nw, w, st)
                    else:
                        st = 'if (g_opt_%s) { %s } else { %s }' % (opt, xlate(nw, pc) or ';', st or ';')
                        self.options.add(opt)
                if st: out.append('  ' + st)
                pc += 4
                continue
            kind, tgt = bi
            terminal = kind in ('j', 'jr') or (kind == 'b' and cond_expr(w) == 'true')
            dpc = pc + 4
            dw = words.get(dpc, 0) if dpc < e + 4 else 0
            if dpc >= e:
                dw = elf.word(dpc)
            if branch_info(dw, dpc) is not None:
                self.stats['branch_in_delay'] += 1
                ds = 'INTERP(0x%x,0x%08x); /* branch in delay slot */' % (dpc, dw)
            else:
                ds = xlate(dw, dpc)
                if dpc in self.patches:
                    nw, opt = self.patches[dpc]
                    if isinstance(nw, tuple): ds = var_patch(nw, dw, ds)
                    else:
                        ds = 'if (g_opt_%s) { %s } else { %s }' % (opt, xlate(nw, dpc) or ';', ds or ';')
                        self.options.add(opt)
            def goto(t):
                if s <= t < e: return ('{ %s(c); goto L_%x; }' % ('ee_wait_poll' if pc in self.waitloops else 'ee_poll', t)) if t <= pc else ('goto L_%x;' % t)
                self.stats['tail_branch'] += 1
                return '{ %s return; }' % self.call_expr(t)
            if kind == 'b':
                c_ = cond_expr(w)
                if c_ == 'true':
                    out.append('  { %s %s }' % (ds, goto(tgt)))
                else:
                    out.append('  { const bool _b = %s; %s if (_b) %s }' % (c_, ds, goto(tgt)))
            elif kind == 'bl':
                c_ = cond_expr(w)
                link = ''
                if f_op(w) == 1 and f_rt(w) in (18, 19): link = 'LINK(31,0x%x); ' % (pc + 8)
                out.append('  if (%s) { %s%s %s }' % (c_, link, ds, goto(tgt)))
            elif kind == 'bal':
                c_ = cond_expr(w)
                out.append('  { const bool _b = %s; LINK(31,0x%x); %s if (_b) { %s } }'
                           % (c_, pc + 8, ds, self.call_expr(tgt)))
            elif kind == 'j':
                out.append('  { %s %s }' % (ds, goto(tgt)))
            elif kind == 'jal':
                out.append('  LINK(31,0x%x); %s %s' % (pc + 8, ds, self.call_expr(tgt)))
            elif kind == 'jr':
                r = f_rs(w)
                if r == 31:
                    out.append('  { %s return; }' % ds)
                else:
                    self.stats['jr_reg'] += 1
                    need_dispatch = True
                    out.append('  { _jt = GPR_U32(%d); %s goto L_dispatch; }' % (r, ds))
            elif kind == 'jalr':
                r = f_rs(w); rd = f_rd(w)
                out.append('  { const uint32_t _t = GPR_U32(%d); LINK(%d,0x%x); %s ee_call(c, _t); }'
                           % (r, rd, pc + 8, ds))
            # delay slot as its own entry point
            if dpc in labels:
                out.append('  goto L_%x;' % (pc + 8) if (pc + 8) < e else '  { %s return; }' % self.call_expr(pc + 8))
                out.append('L_%x:;' % dpc)
                if ds: out.append('  ' + ds)
                if pc + 8 < e and (pc + 8) not in labels: labels.add(pc + 8)
                terminal = False
            if dpc >= e and not terminal:
                self.stats['branch_at_end'] += 1
                out.append('  %s' % self.call_expr(pc + 8))
                terminal = True
            pc += 8
        # fall-through into the next function
        if not terminal and e in self.startset:
            out.append('  %s' % self.call_expr(e))
            self.stats['fallthrough'] += 1
        if need_dispatch:
            out.append('  return;')
            out.append('L_dispatch:')
            cases = ' '.join('case 0x%x: goto L_%x;' % (l, l) for l in sorted(labels))
            out.append('  switch (_jt) { %s default: ee_call(c, _jt); return; }' % cases)
        out.append('}')
        # labels that are only targets of jr tables need to exist; labels referenced but never emitted are fixed below
        body = '\n'.join(out)
        if entry_labels:
            body += '\nvoid %s(EEContext* c) { %s_at(c, 0x%x); }' % (name, name, s)
        return body

# ----------------------------------------------------------------------------- driver
def write_if_changed(path, text):
    """keeps the file's timestamp when the content is the same, so make only rebuilds what changed"""
    try:
        if open(path).read() == text: return
    except OSError: pass
    open(path, 'w').write(text)

def main():
    ap = argparse.ArgumentParser()
    root = os.path.join(os.path.dirname(__file__), '..', '..')
    ap.add_argument('--elf', default=os.path.join(root, 'game', 'SLUS_211.35'))
    ap.add_argument('--out', default=os.path.join(root, 'generated'))
    ap.add_argument('--per-file', type=int, default=400, help='functions per .cpp file')
    ap.add_argument('--only', help='comma list of function addresses (debug)')
    args = ap.parse_args()

    elf = Elf(args.elf)
    funcs, ptrs = discover(elf)
    print('functions: %d   jump-table labels: %d' % (len(funcs), len(ptrs)))
    find_fps_sites(elf, funcs)
    print('frame-rate factor sites (60.0/30.0 -> frames):', len(FPS_LUI_SITES))
    if args.only:
        keep = {int(x, 16) for x in args.only.split(',')}
        funcs_emit = [f for f in funcs if f[0] in keep]
    else:
        funcs_emit = funcs
    overrides = {}
    ov_path = os.path.join(os.path.dirname(__file__), 'overrides.txt')
    if os.path.exists(ov_path):
        for line in open(ov_path):
            line = line.split('#')[0].split()
            if len(line) >= 2: overrides[int(line[0], 16)] = line[1]
    wraps = {}                                 # address -> native wrapper; the recompiled original is f_<address>_orig
    wr_path = os.path.join(os.path.dirname(__file__), 'wraps.txt')
    if os.path.exists(wr_path):
        for line in open(wr_path):
            line = line.split('#')[0].split()
            if len(line) >= 2: wraps[int(line[0], 16)] = line[1]
    yr_path = os.path.join(os.path.dirname(__file__), 'years.txt')
    if os.path.exists(yr_path):
        for line in open(yr_path):
            f = line.split('#')[0].split()
            if not f: continue
            pc = int(f[0], 16); kind = f[1] if len(f) > 1 else 'i'
            YEAR_SITES[pc] = kind
            if kind == 'f':
                YEAR_SITES[pc + 4] = 'g'
                YEAR_SITES_WORDS[pc] = elf.word(pc); YEAR_SITES_WORDS[pc + 4] = elf.word(pc + 4)
    patches = {}
    pt_path = os.path.join(os.path.dirname(__file__), 'patches.txt')
    if os.path.exists(pt_path):
        for line in open(pt_path):
            f = line.split('#')[0].split()
            if len(f) >= 3: patches[int(f[0], 16)] = ((f[1][1:],) if f[1].startswith('=') else int(f[1], 16), f[2])
    g = Gen(elf, funcs, ptrs)
    g.patches = patches
    # backward branches of loops in which the game only spins until an interrupt handler clears a flag
    g.waitloops = set()
    wl_path = os.path.join(os.path.dirname(__file__), 'waitloops.txt')
    if os.path.exists(wl_path):
        for line in open(wl_path):
            line = line.split('#')[0].split()
            if line: g.waitloops.add(int(line[0], 16))
    os.makedirs(args.out, exist_ok=True)
    stale = {f for f in os.listdir(args.out) if f.startswith('funcs_') and f.endswith('.cpp')}

    decl = ['// generated by ee_recomp.py — do not edit', '#pragma once', '#include "ee_ops.h"']
    for s, e in funcs:
        decl.append('void f_%08x(EEContext* c);' % s)
    nfile = 0
    for i in range(0, len(funcs_emit), args.per_file):
        chunk = funcs_emit[i:i + args.per_file]
        body = ['// generated by ee_recomp.py — do not edit', '#include "funcs.h"', '']
        for s, e in chunk:
            if s in overrides:
                body.append('void f_%08x(EEContext* c) { %s(c); }  // override' % (s, overrides[s])); body.append('')
                continue
            if s in wraps:
                assert s not in g.ptrs
                body.append(g.emit_func(s, e, 'f_%08x_orig' % s)); body.append('')
                body.append('void f_%08x(EEContext* c) { %s(c); }  // wrapped' % (s, wraps[s])); body.append('')
                if s in g.multi_entry:       # entries into the middle (jump tables) go to the original, not the wrapper
                    body.append('void f_%08x_at(EEContext* c, uint32_t entry) { f_%08x_orig_at(c, entry); }' % (s, s)); body.append('')
                continue
            body.append(g.emit_func(s, e)); body.append('')
        fn_ = 'funcs_%03d.cpp' % nfile
        write_if_changed(os.path.join(args.out, fn_), '\n'.join(body))
        stale.discard(fn_)
        nfile += 1
    for f in stale:                      # can't always delete in synced folders: blank them instead
        open(os.path.join(args.out, f), 'w').write('// empty\n')
    for opt in sorted(set(v[1] for v in patches.values() if not isinstance(v[0], tuple))):
        decl.append('extern bool g_opt_%s;' % opt)
    for var in sorted(set(v[0][0] for v in patches.values() if isinstance(v[0], tuple))):
        decl.append('extern float %s;' % var)
    if YEAR_SITES:
        decl.append('extern int g_year_shift;                        // years.txt: first year of the calendar - 2005')
        decl.append('static inline uint32_t year_fbits(int y) { float f = (float)(y + g_year_shift); uint32_t b; std::memcpy(&b, &f, 4); return b; }')
    for name in sorted(set(overrides.values()) | set(wraps.values())):
        decl.append('void %s(EEContext* c);' % name)
    for s in sorted(wraps):
        decl.append('void f_%08x_orig(EEContext* c);' % s)
        if s in g.multi_entry: decl.append('void f_%08x_orig_at(EEContext* c, uint32_t entry);' % s)
    for s, labels in g.multi_entry.items():
        decl.append('void f_%08x_at(EEContext* c, uint32_t entry);' % s)
    write_if_changed(os.path.join(args.out, 'funcs.h'), '\n'.join(decl) + '\n')

    # dispatch tables
    tab = ['// generated by ee_recomp.py — do not edit', '#include "funcs.h"', '#include "ee_runtime.h"', '',
           'const EEFuncEntry g_ee_funcs[] = {']
    for s, e in funcs: tab.append('  {0x%x, 0x%x, f_%08x},' % (s, e, s))
    tab.append('};')
    tab.append('const size_t g_ee_funcs_count = %d;' % len(funcs))
    tab.append('const EEEntryLabel g_ee_labels[] = {')
    n = 0
    for s, labels in sorted(g.multi_entry.items()):
        for l in labels:
            tab.append('  {0x%x, f_%08x_at},' % (l, s)); n += 1
    if n == 0: tab.append('  {0, nullptr},')
    tab.append('};')
    tab.append('const size_t g_ee_labels_count = %d;' % n)
    tab.append('const uint32_t g_ee_entry = 0x%x;' % elf.entry)
    write_if_changed(os.path.join(args.out, 'functable.cpp'), '\n'.join(tab) + '\n')
    print('files: %d   stats: %s' % (nfile, dict(g.stats)))

if __name__ == '__main__':
    main()
