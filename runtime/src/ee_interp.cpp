// Interpreter for code the game loads into RAM at run time.
//
// Everything in the executable is recompiled ahead of time, but the game also runs code that is not in the
// executable: its sound banks carry small compiled routines (one per sound module: a straight line of calls into
// the audio library with pointer fix-ups in between) which the audio library calls every tick through its service
// list (0x5074b0). Without them no ambience, crowd or effect module ever runs. This executes such code one
// instruction at a time with the same macros the recompiled code uses; calls into the executable go to the
// recompiled functions, calls to other loaded code come back here.
#include "ee_ops.h"
#include "ee_runtime.h"
#include <cstdlib>

void ee_call(EEContext* c, u32 addr);

static inline s32 s16x(u32 w) { return (s32)(s16)(w & 0xffff); }

// One instruction that is not a branch or jump. Returns false if it is not supported.
static bool exec_one(EEContext* c, u32 pc, u32 w) {
    const int op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31, fn = w & 63;
    const s32 si = s16x(w); const u32 imm = w & 0xffff;
    if (w == 0) return true;
    switch (op) {
    case 0:
        switch (fn) {
        case 12: SYSCALL(pc); return true;
        case 13: BREAK(pc); return true;
        case 15: return true;                                         // sync
        case 17: MTHI(rs); return true;  case 19: MTLO(rs); return true;  case 41: MTSA(rs); return true;
        case 24: MULT(rd, rs, rt); return true;  case 25: MULTU(rd, rs, rt); return true;
        case 26: DIV(rd, rs, rt); return true;   case 27: DIVU(rd, rs, rt); return true;
        }
        if (rd == 0) return fn <= 63;                                 // result discarded
        switch (fn) {
        case 0: SLL(rd, rt, sa); return true;      case 2: SRL(rd, rt, sa); return true;      case 3: SRA(rd, rt, sa); return true;
        case 56: DSLL(rd, rt, sa); return true;    case 58: DSRL(rd, rt, sa); return true;    case 59: DSRA(rd, rt, sa); return true;
        case 60: DSLL32(rd, rt, sa); return true;  case 62: DSRL32(rd, rt, sa); return true;  case 63: DSRA32(rd, rt, sa); return true;
        case 4: SLLV(rd, rt, rs); return true;     case 6: SRLV(rd, rt, rs); return true;     case 7: SRAV(rd, rt, rs); return true;
        case 20: DSLLV(rd, rt, rs); return true;   case 22: DSRLV(rd, rt, rs); return true;   case 23: DSRAV(rd, rt, rs); return true;
        case 10: MOVZ(rd, rs, rt); return true;    case 11: MOVN(rd, rs, rt); return true;
        case 16: MFHI(rd); return true;            case 18: MFLO(rd); return true;            case 40: MFSA(rd); return true;
        case 32: case 33: ADDU(rd, rs, rt); return true;   case 34: case 35: SUBU(rd, rs, rt); return true;
        case 36: AND(rd, rs, rt); return true;     case 37: OR(rd, rs, rt); return true;
        case 38: XOR(rd, rs, rt); return true;     case 39: NOR(rd, rs, rt); return true;
        case 42: SLT(rd, rs, rt); return true;     case 43: SLTU(rd, rs, rt); return true;
        case 44: case 45: DADDU(rd, rs, rt); return true;  case 46: case 47: DSUBU(rd, rs, rt); return true;
        }
        return false;
    case 8: case 9: if (rt) ADDIU(rt, rs, si); return true;
    case 10: if (rt) SLTI(rt, rs, si); return true;
    case 11: if (rt) SLTIU(rt, rs, si); return true;
    case 12: if (rt) ANDI(rt, rs, imm); return true;
    case 13: if (rt) ORI(rt, rs, imm); return true;
    case 14: if (rt) XORI(rt, rs, imm); return true;
    case 24: case 25: if (rt) DADDIU(rt, rs, si); return true;
    case 15: if (rt) LUI(rt, imm); return true;
    case 32: if (rt) LB(rt, rs, si); return true;    case 33: if (rt) LH(rt, rs, si); return true;
    case 34: if (rt) LWL(rt, rs, si); return true;   case 35: if (rt) LW(rt, rs, si); return true;
    case 36: if (rt) LBU(rt, rs, si); return true;   case 37: if (rt) LHU(rt, rs, si); return true;
    case 38: if (rt) LWR(rt, rs, si); return true;   case 39: if (rt) LWU(rt, rs, si); return true;
    case 26: if (rt) LDL(rt, rs, si); return true;   case 27: if (rt) LDR(rt, rs, si); return true;
    case 55: if (rt) LD(rt, rs, si); return true;    case 30: if (rt) LQ(rt, rs, si); return true;
    case 40: SB(rt, rs, si); return true;   case 41: SH(rt, rs, si); return true;   case 42: SWL(rt, rs, si); return true;
    case 43: SW(rt, rs, si); return true;   case 44: SDL(rt, rs, si); return true;  case 45: SDR(rt, rs, si); return true;
    case 46: SWR(rt, rs, si); return true;  case 63: SD(rt, rs, si); return true;   case 31: SQ(rt, rs, si); return true;
    case 49: LWC1(rt, rs, si); return true;
    case 57: SWC1(rt, rs, si); return true;
    case 47: case 51: return true;                                    // cache, pref
    case 28:
        switch (fn) {
        case 0: MADD(rd, rs, rt); return true;    case 1: MADDU(rd, rs, rt); return true;
        case 16: if (rd) MFHI1(rd); return true;  case 17: MTHI1(rs); return true;
        case 18: if (rd) MFLO1(rd); return true;  case 19: MTLO1(rs); return true;
        case 24: MULT1(rd, rs, rt); return true;  case 25: MULTU1(rd, rs, rt); return true;
        case 26: DIV1(rd, rs, rt); return true;   case 27: DIVU1(rd, rs, rt); return true;
        }
        INTERP(pc, w); return true;                                   // the multimedia instructions
    case 17: {
        const int ft = rt, fs = rd, fd = sa;
        if (rs == 0) { if (rt) MFC1(rt, fs); return true; }
        if (rs == 4) { MTC1(rt, fs); return true; }
        if (rs == 2) { if (rt) CFC1(rt, fs); return true; }
        if (rs == 6) { CTC1(rt, fs); return true; }
        if (rs == 20 && fn == 32) { CVT_S_W(fd, fs, 0); return true; }
        if (rs != 16) return false;
        switch (fn) {
        case 0: ADD_S(fd, fs, ft); return true;    case 1: SUB_S(fd, fs, ft); return true;   case 2: MUL_S(fd, fs, ft); return true;
        case 3: DIV_S(fd, fs, ft); return true;    case 4: SQRT_S(fd, fs, ft); return true;  case 5: ABS_S(fd, fs, ft); return true;
        case 6: MOV_S(fd, fs, ft); return true;    case 7: NEG_S(fd, fs, ft); return true;   case 22: RSQRT_S(fd, fs, ft); return true;
        case 24: ADDA_S(fd, fs, ft); return true;  case 25: SUBA_S(fd, fs, ft); return true; case 26: MULA_S(fd, fs, ft); return true;
        case 28: MADD_S(fd, fs, ft); return true;  case 29: MSUB_S(fd, fs, ft); return true;
        case 30: MADDA_S(fd, fs, ft); return true; case 31: MSUBA_S(fd, fs, ft); return true;
        case 36: CVT_W_S(fd, fs, ft); return true; case 40: MAX_S(fd, fs, ft); return true;  case 41: MIN_S(fd, fs, ft); return true;
        case 48: C_F_S(fd, fs, ft); return true;   case 50: C_EQ_S(fd, fs, ft); return true;
        case 52: C_LT_S(fd, fs, ft); return true;  case 54: C_LE_S(fd, fs, ft); return true;
        }
        return false;
    }
    }
    return false;                                                     // COP0, COP2 (VU0) and the rest: not expected in loaded code
}

// Is there something at `a` that can be run? Loaded routines set up a stack frame first; a wild pointer hardly ever
// points at "addiu sp, sp, -n".
bool ee_ram_code_at(u32 a) {
    a &= 0x1fffffff;
    if (a < 0x007d32c0 || a >= 0x02000000 - 8 || (a & 3)) return false;
    const u32 w = ee_rd<u32>(a);
    return (w >> 16) == 0x27bd && (w & 0x8000);
}

u64 g_interp_calls = 0, g_interp_steps = 0;

// Runs the routine at `pc` until it returns (jr ra).
void ee_interpret(EEContext* c, u32 pc) {
    static const bool off = std::getenv("MVP_NO_INTERP") != nullptr;
    if (off) { c->r[2].UD[0] = 0; return; }
    g_interp_calls++;
    const u32 entry = pc;
    for (u32 steps = 0;; steps++) {
        if (steps > 4000000) { rlog("interp", "routine at %08x does not end, left at %08x", entry, pc); return; }
        g_interp_steps++;
        const u32 w = ee_rd<u32>(pc);
        const int op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, fn = w & 63;
        // ---- control flow: the decision is made first, then the delay slot runs, then the jump
        int kind = 0; bool taken = false, likely = false; u32 target = 0;       // kind: 1 branch, 2 jump, 3 call, 4 return
        const u32 btgt = pc + 4 + ((u32)s16x(w) << 2), jtgt = ((pc + 4) & 0xf0000000u) | ((w & 0x03ffffffu) << 2);
        if (op == 2) { kind = 2; target = jtgt; }
        else if (op == 3) { kind = 3; target = jtgt; LINK(31, pc + 8); }
        else if (op == 0 && fn == 8) { target = GPR_U32(rs); kind = rs == 31 ? 4 : 2; }
        else if (op == 0 && fn == 9) { kind = 3; target = GPR_U32(rs); LINK(rd, pc + 8); }
        else if (op == 4 || op == 20) { kind = 1; taken = GPR_S(rs) == GPR_S(rt); likely = op == 20; }
        else if (op == 5 || op == 21) { kind = 1; taken = GPR_S(rs) != GPR_S(rt); likely = op == 21; }
        else if (op == 6 || op == 22) { kind = 1; taken = GPR_S(rs) <= 0; likely = op == 22; }
        else if (op == 7 || op == 23) { kind = 1; taken = GPR_S(rs) > 0; likely = op == 23; }
        else if (op == 1 && (rt & 0x0c) == 0) {                                  // bltz bgez bltzl bgezl
            kind = 1; taken = (rt & 1) ? GPR_S(rs) >= 0 : GPR_S(rs) < 0; likely = (rt & 2) != 0;
        } else if (op == 1 && (rt & 0x1c) == 0x10) {                             // bltzal bgezal (+ likely)
            taken = (rt & 1) ? GPR_S(rs) >= 0 : GPR_S(rs) < 0; likely = (rt & 2) != 0; LINK(31, pc + 8);
            kind = taken ? 3 : 1; target = btgt;
        } else if (op == 17 && rs == 8) { kind = 1; taken = (rt & 1) ? c->fcc : !c->fcc; likely = (rt & 2) != 0; }
        if (kind == 0) {
            if (!exec_one(c, pc, w)) { rlog("interp", "unsupported instruction %08x at %08x (routine %08x): left", w, pc, entry); return; }
            pc += 4; continue;
        }
        if (kind == 1 && !taken && likely) { pc += 8; continue; }                // a "likely" branch not taken skips its delay slot
        {   // delay slot
            const u32 dw = ee_rd<u32>(pc + 4);
            if (!exec_one(c, pc + 4, dw)) { rlog("interp", "unsupported instruction %08x in a delay slot at %08x (routine %08x): left", dw, pc + 4, entry); return; }
        }
        if (kind == 1) {
            if (!taken) { pc += 8; continue; }
            if (btgt <= pc) ee_poll(c);                                          // loops: let interrupts and other threads in
            pc = btgt; continue;
        }
        if (kind == 4) return;
        if (kind == 3) { ee_call(c, target); pc += 8; continue; }                // recompiled code, or loaded code (comes back here)
        // jump: on within loaded code, or a tail call into the executable
        if (((target & 0x1fffffff) >= 0x007d32c0) && (target & 0x1fffffff) < 0x02000000) { if (target <= pc) ee_poll(c); pc = target; continue; }
        ee_call(c, target); return;
    }
}
