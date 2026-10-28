// Instruction semantics used by the generated code (generated/funcs_*.cpp).
// Every macro expects `EEContext* c` in scope. Writes to $zero are filtered out by the recompiler.
#pragma once
#include <cstring>
#include <cmath>
#include "ee_context.h"
#include "vu0_inline.h"

// ------------------------------------------------------------------ memory
extern u8* g_ram;                        // 32 MB main RAM
u8* ee_ptr_slow(u32 addr);               // mirrors + scratchpad; nullptr => hardware register
u64 ee_hw_read(u32 addr, int size);
void ee_hw_write(u32 addr, u64 value, int size);
void ee_hw_read128(u32 addr, GPR128* out);
void ee_hw_write128(u32 addr, const GPR128* in);

static inline u8* ee_ptr(u32 a) { return a < 0x02000000u ? g_ram + a : ee_ptr_slow(a); }

#ifdef EE_TRACE
extern u32 g_rwatch_lo, g_rwatch_hi; void ee_rwatch_hit(u32 a, int size);
#define EE_RWATCH(a, n) do { if ((u32)((a) & 0x1fffffff) - g_rwatch_lo < g_rwatch_hi - g_rwatch_lo) ee_rwatch_hit(a, n); } while (0)
#else
#define EE_RWATCH(a, n) ((void)0)
#endif
template <typename T> static inline T ee_rd(u32 a) {
    EE_RWATCH(a, (int)sizeof(T));
    u8* p = ee_ptr(a);
    if (p) { T v; std::memcpy(&v, p, sizeof(T)); return v; }
    return (T)ee_hw_read(a, sizeof(T));
}
#ifdef EE_TRACE
extern u32 g_watch_lo, g_watch_hi; void ee_watch_hit(u32 a, u64 v, int size);
#define EE_WATCH(a, v, n) do { if ((u32)((a) & 0x1fffffff) - g_watch_lo < g_watch_hi - g_watch_lo) ee_watch_hit(a, (u64)(v), n); } while (0)
#else
#define EE_WATCH(a, v, n) ((void)0)
#endif
template <typename T> static inline void ee_wr(u32 a, T v) {
    EE_WATCH(a, v, (int)sizeof(T));
    u8* p = ee_ptr(a);
    if (p) { std::memcpy(p, &v, sizeof(T)); return; }
    ee_hw_write(a, (u64)v, sizeof(T));
}
static inline void ee_rd128(u32 a, GPR128* o) {
    a &= ~15u; u8* p = ee_ptr(a);
    if (p) std::memcpy(o, p, 16); else ee_hw_read128(a, o);
}
static inline void ee_wr128(u32 a, const GPR128* v) {
    a &= ~15u; EE_WATCH(a, v->UD[0], 16); u8* p = ee_ptr(a);
    if (p) std::memcpy(p, v, 16); else ee_hw_write128(a, v);
}

// ------------------------------------------------------------------ runtime hooks
void ee_call(EEContext* c, u32 addr);                 // indirect call / tail jump
void ee_syscall(EEContext* c, u32 pc);
void ee_break(EEContext* c, u32 pc);
void ee_interp(EEContext* c, u32 pc, u32 word);       // rare instructions, interpreted
void ee_cop0(EEContext* c, u32 pc, u32 word);
u32  ee_cop0_read(EEContext* c, int reg);
void ee_cop0_write(EEContext* c, int reg, u32 v);
void vu0_macro(EEContext* c, u32 word);
bool vu0_cond(EEContext* c, int t);
void ee_poll(EEContext* c);                          // backward branches: deliver interrupts / thread switches
void ee_wait_poll(EEContext* c);          // like ee_poll, in a loop that only waits for an interrupt: may sleep

// ------------------------------------------------------------------ call tracing (build with -DEE_TRACE)
#ifdef EE_TRACE
extern u32 g_trace_ring[4096]; extern u32 g_trace_pos; extern u32 g_hits[0x200000];
#undef EE_TRACE
extern u32 g_logcall; void ee_logcall_enter(EEContext* c, u32 a); void ee_logcall_leave(EEContext* c, u32 a);
// MVP_DBG_ABI=1: every function must give back s0-s7, sp and fp as it got them; the first one that does not is where
// control flow (or the stack) went wrong.
extern bool g_abi_check; void ee_abi_violation(EEContext* c, u32 fn, const u64* saved);
struct EEScope {
    EEContext* c; u32 a; u64 sv[10];
    EEScope(EEContext* c_, u32 a_) : c(c_), a(a_) {
        g_trace_ring[(g_trace_pos++) & 4095] = a; g_hits[(a >> 2) & 0x1fffff]++;
        if (c->shadow_n < 1024) c->shadow[c->shadow_n] = a;
        c->shadow_n++;
        if (a == g_logcall) ee_logcall_enter(c, a);
        if (g_abi_check) { for (int i = 0; i < 8; i++) sv[i] = c->r[16 + i].UD[0]; sv[8] = c->r[29].UD[0]; sv[9] = c->r[30].UD[0]; }
    }
    ~EEScope() {
        c->shadow_n--; if (a == g_logcall) ee_logcall_leave(c, a);
        if (g_abi_check) {
            bool bad = sv[8] != c->r[29].UD[0] || sv[9] != c->r[30].UD[0];
            for (int i = 0; i < 8; i++) bad |= sv[i] != c->r[16 + i].UD[0];
            if (bad) ee_abi_violation(c, a, sv);
        }
    }
};
#define EE_TRACE(a) EEScope _ee_scope(c, a)
#else
#define EE_TRACE(a) ((void)0)
#endif
void ee_dump_trace();

// ------------------------------------------------------------------ register helpers
#define GPR_S(n)   (c->r[n].SD[0])
#define GPR_U(n)   (c->r[n].UD[0])
#define GPR_U32(n) (c->r[n].UW[0])
#define GPR_S32(n) (c->r[n].SW[0])
#define SET32(n, v) (c->r[n].SD[0] = (s64)(s32)(u32)(v))
#define EA(rs, off) ((u32)(c->r[rs].UW[0] + (u32)(s32)(off)))

#define LINK(rd, v) do { if (rd) c->r[rd].UD[0] = (u64)(u32)(v); } while (0)

// ------------------------------------------------------------------ ALU
#define ADDIU(rt, rs, i)  SET32(rt, GPR_U32(rs) + (u32)(s32)(i))
#define DADDIU(rt, rs, i) (c->r[rt].SD[0] = GPR_S(rs) + (s64)(i))
#define SLTI(rt, rs, i)   (c->r[rt].UD[0] = GPR_S(rs) < (s64)(i))
#define SLTIU(rt, rs, i)  (c->r[rt].UD[0] = GPR_U(rs) < (u64)(s64)(i))
#define ANDI(rt, rs, i)   (c->r[rt].UD[0] = GPR_U(rs) & (u64)(i))
#define ORI(rt, rs, i)    (c->r[rt].UD[0] = GPR_U(rs) | (u64)(i))
#define XORI(rt, rs, i)   (c->r[rt].UD[0] = GPR_U(rs) ^ (u64)(i))
// Simulation time step: the game hard-codes 1/60 s (0x3C888889) and 1/30 s (0x3D088889) as float constants.
// g_sim_dt_bits holds the values to use instead (defaults: unchanged; "keep game speed" at N fps: 1/N, 2/N).
extern u32 g_sim_dt_bits[2];
extern u64 g_sim_ticks;                     // simulation ticks run (Game_StepAndTime calls)
// 60.0f / 30.0f used to turn seconds into frame counts: multiplied by (frame rate / 60) in real-time mode.
extern float g_fps_ratio;
static inline u32 fps_factor_bits(u32 hi) { u32 b = hi << 16; float f; std::memcpy(&f, &b, 4); f *= g_fps_ratio; std::memcpy(&b, &f, 4); return b; }
#define LUI_FPS(rt, i) (c->r[rt].SD[0] = (s64)(s32)fps_factor_bits(i))
extern u32 g_dt_site_lo, g_dt_site_hi;            // debug: only substitute sites in [lo, hi] (MVP_DT_RANGE)
#define ORI_DT(rt, rs, i, pc) do { u64 v_ = GPR_U(rs) | (u64)(i); \
    if ((u32)(pc) - g_dt_site_lo <= g_dt_site_hi - g_dt_site_lo) { \
        if (v_ == 0x3c888889ull) v_ = g_sim_dt_bits[0]; else if (v_ == 0x3d088889ull) v_ = g_sim_dt_bits[1]; } \
    c->r[rt].UD[0] = v_; } while (0)
#define LUI(rt, i)        (c->r[rt].SD[0] = (s64)(s32)((u32)(i) << 16))

#define ADDU(rd, rs, rt)  SET32(rd, GPR_U32(rs) + GPR_U32(rt))
#define SUBU(rd, rs, rt)  SET32(rd, GPR_U32(rs) - GPR_U32(rt))
#define DADDU(rd, rs, rt) (c->r[rd].UD[0] = GPR_U(rs) + GPR_U(rt))
#define DSUBU(rd, rs, rt) (c->r[rd].UD[0] = GPR_U(rs) - GPR_U(rt))
#define AND(rd, rs, rt)   (c->r[rd].UD[0] = GPR_U(rs) & GPR_U(rt))
#define OR(rd, rs, rt)    (c->r[rd].UD[0] = GPR_U(rs) | GPR_U(rt))
#define XOR(rd, rs, rt)   (c->r[rd].UD[0] = GPR_U(rs) ^ GPR_U(rt))
#define NOR(rd, rs, rt)   (c->r[rd].UD[0] = ~(GPR_U(rs) | GPR_U(rt)))
#define SLT(rd, rs, rt)   (c->r[rd].UD[0] = GPR_S(rs) < GPR_S(rt))
#define SLTU(rd, rs, rt)  (c->r[rd].UD[0] = GPR_U(rs) < GPR_U(rt))
#define MOVZ(rd, rs, rt)  do { if (GPR_U(rt) == 0) c->r[rd].UD[0] = GPR_U(rs); } while (0)
#define MOVN(rd, rs, rt)  do { if (GPR_U(rt) != 0) c->r[rd].UD[0] = GPR_U(rs); } while (0)

#define SLL(rd, rt, sa)    SET32(rd, GPR_U32(rt) << (sa))
#define SRL(rd, rt, sa)    SET32(rd, GPR_U32(rt) >> (sa))
#define SRA(rd, rt, sa)    SET32(rd, GPR_S32(rt) >> (sa))
#define SLLV(rd, rt, rs)   SET32(rd, GPR_U32(rt) << (GPR_U32(rs) & 31))
#define SRLV(rd, rt, rs)   SET32(rd, GPR_U32(rt) >> (GPR_U32(rs) & 31))
#define SRAV(rd, rt, rs)   SET32(rd, GPR_S32(rt) >> (GPR_U32(rs) & 31))
#define DSLL(rd, rt, sa)   (c->r[rd].UD[0] = GPR_U(rt) << (sa))
#define DSRL(rd, rt, sa)   (c->r[rd].UD[0] = GPR_U(rt) >> (sa))
#define DSRA(rd, rt, sa)   (c->r[rd].SD[0] = GPR_S(rt) >> (sa))
#define DSLL32(rd, rt, sa) (c->r[rd].UD[0] = GPR_U(rt) << ((sa) + 32))
#define DSRL32(rd, rt, sa) (c->r[rd].UD[0] = GPR_U(rt) >> ((sa) + 32))
#define DSRA32(rd, rt, sa) (c->r[rd].SD[0] = GPR_S(rt) >> ((sa) + 32))
#define DSLLV(rd, rt, rs)  (c->r[rd].UD[0] = GPR_U(rt) << (GPR_U32(rs) & 63))
#define DSRLV(rd, rt, rs)  (c->r[rd].UD[0] = GPR_U(rt) >> (GPR_U32(rs) & 63))
#define DSRAV(rd, rt, rs)  (c->r[rd].SD[0] = GPR_S(rt) >> (GPR_U32(rs) & 63))

// ------------------------------------------------------------------ HI/LO, mult/div (pipeline 0 = lane 0, pipeline 1 = lane 1)
static inline void ee_mult(EEContext* c, int rd, int rs, int rt, int lane, bool uns) {
    s64 p = uns ? (s64)((u64)GPR_U32(rs) * (u64)GPR_U32(rt)) : (s64)GPR_S32(rs) * (s64)GPR_S32(rt);
    c->lo.SD[lane] = (s64)(s32)(u32)p;
    c->hi.SD[lane] = (s64)(s32)(u32)((u64)p >> 32);
    if (rd) c->r[rd].SD[0] = c->lo.SD[lane];
}
static inline void ee_div(EEContext* c, int rs, int rt, int lane, bool uns) {
    if (uns) {
        u32 a = GPR_U32(rs), b = GPR_U32(rt);
        if (b == 0) { c->lo.SD[lane] = -1; c->hi.SD[lane] = (s64)(s32)a; return; }
        c->lo.SD[lane] = (s64)(s32)(a / b); c->hi.SD[lane] = (s64)(s32)(a % b);
    } else {
        s32 a = GPR_S32(rs), b = GPR_S32(rt);
        if (b == 0) { c->lo.SD[lane] = a < 0 ? 1 : -1; c->hi.SD[lane] = a; return; }
        if (a == (s32)0x80000000 && b == -1) { c->lo.SD[lane] = a; c->hi.SD[lane] = 0; return; }
        c->lo.SD[lane] = a / b; c->hi.SD[lane] = a % b;
    }
}
static inline void ee_madd(EEContext* c, int rd, int rs, int rt, int lane, bool uns) {
    u64 acc = ((u64)c->hi.UW[lane * 2] << 32) | c->lo.UW[lane * 2];
    u64 p = uns ? (u64)GPR_U32(rs) * (u64)GPR_U32(rt) : (u64)((s64)GPR_S32(rs) * (s64)GPR_S32(rt));
    acc += p;
    c->lo.SD[lane] = (s64)(s32)(u32)acc;
    c->hi.SD[lane] = (s64)(s32)(u32)(acc >> 32);
    if (rd) c->r[rd].SD[0] = c->lo.SD[lane];
}
#define MULT(rd, rs, rt)   ee_mult(c, rd, rs, rt, 0, false)
#define MULTU(rd, rs, rt)  ee_mult(c, rd, rs, rt, 0, true)
#define MULT1(rd, rs, rt)  ee_mult(c, rd, rs, rt, 1, false)
#define MULTU1(rd, rs, rt) ee_mult(c, rd, rs, rt, 1, true)
#define DIV(rd, rs, rt)    ee_div(c, rs, rt, 0, false)
#define DIVU(rd, rs, rt)   ee_div(c, rs, rt, 0, true)
#define DIV1(rd, rs, rt)   ee_div(c, rs, rt, 1, false)
#define DIVU1(rd, rs, rt)  ee_div(c, rs, rt, 1, true)
#define MADD(rd, rs, rt)   ee_madd(c, rd, rs, rt, 0, false)
#define MADDU(rd, rs, rt)  ee_madd(c, rd, rs, rt, 0, true)
#define MFHI(rd)  (c->r[rd].UD[0] = c->hi.UD[0])
#define MFLO(rd)  (c->r[rd].UD[0] = c->lo.UD[0])
#define MTHI(rs)  (c->hi.UD[0] = GPR_U(rs))
#define MTLO(rs)  (c->lo.UD[0] = GPR_U(rs))
#define MFHI1(rd) (c->r[rd].UD[0] = c->hi.UD[1])
#define MFLO1(rd) (c->r[rd].UD[0] = c->lo.UD[1])
#define MTHI1(rs) (c->hi.UD[1] = GPR_U(rs))
#define MTLO1(rs) (c->lo.UD[1] = GPR_U(rs))
#define MFSA(rd)  (c->r[rd].UD[0] = c->sa)
#define MTSA(rs)  (c->sa = GPR_U32(rs))
#define MTSAB(rs, i) (c->sa = ((GPR_U32(rs) & 15) ^ ((i) & 15)))          /* SA in bytes */
#define MTSAH(rs, i) (c->sa = (((GPR_U32(rs) & 7) ^ ((i) & 7)) * 2))

// ------------------------------------------------------------------ 128-bit MMI (common subset; rest interpreted)
#define PCPYLD(rd, rs, rt) do { u64 _a = c->r[rt].UD[0], _b = c->r[rs].UD[0]; c->r[rd].UD[0] = _a; c->r[rd].UD[1] = _b; } while (0)
#define PCPYUD(rd, rs, rt) do { u64 _a = c->r[rs].UD[1], _b = c->r[rt].UD[1]; c->r[rd].UD[0] = _a; c->r[rd].UD[1] = _b; } while (0)
#define POR(rd, rs, rt)    do { c->r[rd].UD[0] = c->r[rs].UD[0] | c->r[rt].UD[0]; c->r[rd].UD[1] = c->r[rs].UD[1] | c->r[rt].UD[1]; } while (0)
#define PNOR(rd, rs, rt)   do { c->r[rd].UD[0] = ~(c->r[rs].UD[0] | c->r[rt].UD[0]); c->r[rd].UD[1] = ~(c->r[rs].UD[1] | c->r[rt].UD[1]); } while (0)
#define PAND(rd, rs, rt)   do { c->r[rd].UD[0] = c->r[rs].UD[0] & c->r[rt].UD[0]; c->r[rd].UD[1] = c->r[rs].UD[1] & c->r[rt].UD[1]; } while (0)
#define PXOR(rd, rs, rt)   do { c->r[rd].UD[0] = c->r[rs].UD[0] ^ c->r[rt].UD[0]; c->r[rd].UD[1] = c->r[rs].UD[1] ^ c->r[rt].UD[1]; } while (0)
static inline void ee_padduw(EEContext* c, int rd, int rs, int rt) {
    GPR128 r;
    for (int i = 0; i < 4; i++) { u64 s = (u64)c->r[rs].UW[i] + c->r[rt].UW[i]; r.UW[i] = s > 0xffffffffull ? 0xffffffffu : (u32)s; }
    c->r[rd] = r;
}
#define PADDUW(rd, rs, rt) ee_padduw(c, rd, rs, rt)
#define PMFHI(rd) (c->r[rd] = c->hi)
#define PMFLO(rd) (c->r[rd] = c->lo)
#define PMTHI(rs) (c->hi = c->r[rs])
#define PMTLO(rs) (c->lo = c->r[rs])
#define PEXTLW(rd, rs, rt) do { GPR128 _r; _r.UW[0] = c->r[rt].UW[0]; _r.UW[1] = c->r[rs].UW[0]; _r.UW[2] = c->r[rt].UW[1]; _r.UW[3] = c->r[rs].UW[1]; c->r[rd] = _r; } while (0)
#define PEXTUW(rd, rs, rt) do { GPR128 _r; _r.UW[0] = c->r[rt].UW[2]; _r.UW[1] = c->r[rs].UW[2]; _r.UW[2] = c->r[rt].UW[3]; _r.UW[3] = c->r[rs].UW[3]; c->r[rd] = _r; } while (0)
static inline void ee_qfsrv(EEContext* c, int rd, int rs, int rt) {
    u8 buf[32]; std::memcpy(buf, c->r[rt].UB, 16); std::memcpy(buf + 16, c->r[rs].UB, 16);
    std::memcpy(c->r[rd].UB, buf + (c->sa & 15), 16);
}
#define QFSRV(rd, rs, rt) ee_qfsrv(c, rd, rs, rt)
#define PCPYH(rd, rs, rt)  do { u16 _a = c->r[rt].UH[0], _b = c->r[rt].UH[4]; for (int _i = 0; _i < 4; _i++) { c->r[rd].UH[_i] = _a; c->r[rd].UH[_i + 4] = _b; } } while (0)

// ------------------------------------------------------------------ loads / stores
#define TOUCH(rs, off) ((void)ee_rd<u32>(EA(rs, off) & ~3u))
#define LB(rt, rs, off)  (c->r[rt].SD[0] = (s64)ee_rd<s8>(EA(rs, off)))
#define LBU(rt, rs, off) (c->r[rt].UD[0] = (u64)ee_rd<u8>(EA(rs, off)))
#define LH(rt, rs, off)  (c->r[rt].SD[0] = (s64)ee_rd<s16>(EA(rs, off)))
#define LHU(rt, rs, off) (c->r[rt].UD[0] = (u64)ee_rd<u16>(EA(rs, off)))
#define LW(rt, rs, off)  (c->r[rt].SD[0] = (s64)ee_rd<s32>(EA(rs, off)))
#define LWU(rt, rs, off) (c->r[rt].UD[0] = (u64)ee_rd<u32>(EA(rs, off)))
#define LD(rt, rs, off)  (c->r[rt].UD[0] = ee_rd<u64>(EA(rs, off)))
#define LQ(rt, rs, off)  do { GPR128 _q; ee_rd128(EA(rs, off), &_q); if (rt) c->r[rt] = _q; } while (0)
#define SB(rt, rs, off)  ee_wr<u8>(EA(rs, off), (u8)GPR_U(rt))
#define SH(rt, rs, off)  ee_wr<u16>(EA(rs, off), (u16)GPR_U(rt))
#define SW(rt, rs, off)  ee_wr<u32>(EA(rs, off), GPR_U32(rt))
#define SD(rt, rs, off)  ee_wr<u64>(EA(rs, off), GPR_U(rt))
#define SQ(rt, rs, off)  ee_wr128(EA(rs, off), &c->r[rt])

static inline void ee_lwl(EEContext* c, int rt, u32 a) {
    u32 sh = a & 3, m = ee_rd<u32>(a & ~3u);
    static const u32 mask[4] = {0x00ffffff, 0x0000ffff, 0x000000ff, 0};
    static const int shf[4] = {24, 16, 8, 0};
    c->r[rt].SD[0] = (s64)(s32)((GPR_U32(rt) & mask[sh]) | (m << shf[sh]));
}
static inline void ee_lwr(EEContext* c, int rt, u32 a) {
    u32 sh = a & 3, m = ee_rd<u32>(a & ~3u);
    static const u32 mask[4] = {0, 0xff000000, 0xffff0000, 0xffffff00};
    u32 v = (GPR_U32(rt) & mask[sh]) | (m >> (sh * 8));
    if (sh == 0) c->r[rt].SD[0] = (s64)(s32)v; else c->r[rt].UW[0] = v;
}
static inline void ee_swl(EEContext* c, int rt, u32 a) {
    u32 sh = a & 3, al = a & ~3u, m = ee_rd<u32>(al);
    static const u32 mask[4] = {0xffffff00, 0xffff0000, 0xff000000, 0};
    static const int shf[4] = {24, 16, 8, 0};
    ee_wr<u32>(al, (m & mask[sh]) | (GPR_U32(rt) >> shf[sh]));
}
static inline void ee_swr(EEContext* c, int rt, u32 a) {
    u32 sh = a & 3, al = a & ~3u, m = ee_rd<u32>(al);
    static const u32 mask[4] = {0, 0xff, 0xffff, 0xffffff};
    ee_wr<u32>(al, (m & mask[sh]) | (GPR_U32(rt) << (sh * 8)));
}
static inline void ee_ldl(EEContext* c, int rt, u32 a) {
    u32 sh = a & 7; u64 m = ee_rd<u64>(a & ~7u);
    u64 mask = sh == 7 ? 0 : (~0ull >> ((sh + 1) * 8));
    c->r[rt].UD[0] = (GPR_U(rt) & mask) | (m << ((7 - sh) * 8));
}
static inline void ee_ldr(EEContext* c, int rt, u32 a) {
    u32 sh = a & 7; u64 m = ee_rd<u64>(a & ~7u);
    u64 mask = sh == 0 ? 0 : (~0ull << ((8 - sh) * 8));
    c->r[rt].UD[0] = (GPR_U(rt) & mask) | (m >> (sh * 8));
}
static inline void ee_sdl(EEContext* c, int rt, u32 a) {
    u32 sh = a & 7, al = a & ~7u; u64 m = ee_rd<u64>(al);
    u64 mask = sh == 7 ? 0 : (~0ull << ((sh + 1) * 8));
    ee_wr<u64>(al, (m & mask) | (GPR_U(rt) >> ((7 - sh) * 8)));
}
static inline void ee_sdr(EEContext* c, int rt, u32 a) {
    u32 sh = a & 7, al = a & ~7u; u64 m = ee_rd<u64>(al);
    u64 mask = sh == 0 ? 0 : (~0ull >> ((8 - sh) * 8));
    ee_wr<u64>(al, (m & mask) | (GPR_U(rt) << (sh * 8)));
}
#define LWL(rt, rs, off) ee_lwl(c, rt, EA(rs, off))
#define LWR(rt, rs, off) ee_lwr(c, rt, EA(rs, off))
#define SWL(rt, rs, off) ee_swl(c, rt, EA(rs, off))
#define SWR(rt, rs, off) ee_swr(c, rt, EA(rs, off))
#define LDL(rt, rs, off) ee_ldl(c, rt, EA(rs, off))
#define LDR(rt, rs, off) ee_ldr(c, rt, EA(rs, off))
#define SDL(rt, rs, off) ee_sdl(c, rt, EA(rs, off))
#define SDR(rt, rs, off) ee_sdr(c, rt, EA(rs, off))

// ------------------------------------------------------------------ FPU (COP1). PS2 floats have no Inf/NaN; clamp on convert.
#define LWC1(ft, rs, off) (c->f[ft].u = ee_rd<u32>(EA(rs, off)))
#define SWC1(ft, rs, off) ee_wr<u32>(EA(rs, off), c->f[ft].u)
#define MFC1(rt, fs) (c->r[rt].SD[0] = (s64)c->f[fs].s)
#define MTC1(rt, fs) (c->f[fs].u = GPR_U32(rt))
#define CFC1(rt, fs) (c->r[rt].SD[0] = (s64)(s32)((fs) == 31 ? (c->fcr31 | (c->fcc ? 0x00800000u : 0)) : ((fs) == 0 ? 0x2e30u : 0)))
#define CTC1(rt, fs) do { if ((fs) == 31) { c->fcr31 = GPR_U32(rt); c->fcc = (c->fcr31 >> 23) & 1; } } while (0)
static inline float ee_fclamp(float v) { if (std::isnan(v) || std::isinf(v)) { u32 u; std::memcpy(&u, &v, 4); u = (u & 0x80000000u) | 0x7f7fffffu; std::memcpy(&v, &u, 4); } return v; }
#define FS(n) (c->f[n].f)
#define ADD_S(fd, fs, ft)   (FS(fd) = ee_fclamp(FS(fs) + FS(ft)))
#define SUB_S(fd, fs, ft)   (FS(fd) = ee_fclamp(FS(fs) - FS(ft)))
#define MUL_S(fd, fs, ft)   (FS(fd) = ee_fclamp(FS(fs) * FS(ft)))
#define DIV_S(fd, fs, ft)   (FS(fd) = FS(ft) == 0.0f ? ((c->f[fs].u ^ c->f[ft].u) & 0x80000000u ? -3.4028235e38f : 3.4028235e38f) : ee_fclamp(FS(fs) / FS(ft)))
#define SQRT_S(fd, fs, ft)  (FS(fd) = std::sqrt(std::fabs(FS(ft))))
#define RSQRT_S(fd, fs, ft) (FS(fd) = ee_fclamp(FS(fs) / std::sqrt(std::fabs(FS(ft)))))
#define ABS_S(fd, fs, ft)   (FS(fd) = std::fabs(FS(fs)))
#define MOV_S(fd, fs, ft)   (FS(fd) = FS(fs))
#define NEG_S(fd, fs, ft)   (c->f[fd].u = c->f[fs].u ^ 0x80000000u)
#define ADDA_S(fd, fs, ft)  (c->acc.f = ee_fclamp(FS(fs) + FS(ft)))
#define SUBA_S(fd, fs, ft)  (c->acc.f = ee_fclamp(FS(fs) - FS(ft)))
#define MULA_S(fd, fs, ft)  (c->acc.f = ee_fclamp(FS(fs) * FS(ft)))
#define MADDA_S(fd, fs, ft) (c->acc.f = ee_fclamp(c->acc.f + FS(fs) * FS(ft)))
#define MSUBA_S(fd, fs, ft) (c->acc.f = ee_fclamp(c->acc.f - FS(fs) * FS(ft)))
#define MADD_S(fd, fs, ft)  (FS(fd) = ee_fclamp(c->acc.f + FS(fs) * FS(ft)))
#define MSUB_S(fd, fs, ft)  (FS(fd) = ee_fclamp(c->acc.f - FS(fs) * FS(ft)))
#define MAX_S(fd, fs, ft)   (FS(fd) = FS(fs) > FS(ft) ? FS(fs) : FS(ft))
#define MIN_S(fd, fs, ft)   (FS(fd) = FS(fs) < FS(ft) ? FS(fs) : FS(ft))
static inline s32 ee_cvtw(float f) { if (f >= 2147483647.0f) return 0x7fffffff; if (f <= -2147483648.0f) return (s32)0x80000000; return (s32)f; }
#define CVT_W_S(fd, fs, ft) (c->f[fd].s = ee_cvtw(FS(fs)))
#define CVT_S_W(fd, fs, ft) (FS(fd) = (float)c->f[fs].s)
#define C_F_S(fd, fs, ft)   (c->fcc = false)
#define C_EQ_S(fd, fs, ft)  (c->fcc = FS(fs) == FS(ft))
#define C_LT_S(fd, fs, ft)  (c->fcc = FS(fs) < FS(ft))
#define C_LE_S(fd, fs, ft)  (c->fcc = FS(fs) <= FS(ft))

// ------------------------------------------------------------------ COP0 / COP2 / misc
#define MFC0(rt, rd) (c->r[rt].SD[0] = (s64)(s32)ee_cop0_read(c, rd))
#define MTC0(rt, rd) ee_cop0_write(c, rd, GPR_U32(rt))
#define COP0(pc, w)  ee_cop0(c, pc, w)
#define COP0_COND(t) ((t) == 1)
#define VU0_COND(t)  vu0_cond(c, t)
#define QMFC2(rt, rd) do { std::memcpy(&c->r[rt], c->vf[rd], 16); } while (0)
#define QMTC2(rt, rd) do { if (rd) std::memcpy(c->vf[rd], &c->r[rt], 16); } while (0)
#define CFC2(rt, rd)  (c->r[rt].SD[0] = (s64)(s32)c->vi[rd])
#define CTC2(rt, rd)  do { if (rd) c->vi[rd] = GPR_U32(rt); } while (0)
#define LQC2(ft, rs, off) do { GPR128 _q; ee_rd128(EA(rs, off), &_q); if (ft) std::memcpy(c->vf[ft], &_q, 16); } while (0)
#define SQC2(ft, rs, off) do { GPR128 _q; std::memcpy(&_q, c->vf[ft], 16); ee_wr128(EA(rs, off), &_q); } while (0)
#define VU0OP(w)       vu0i::op<(w)>(c)
#define SYSCALL(pc)    ee_syscall(c, pc)
#define BREAK(pc)      ee_break(c, pc)
#define INTERP(pc, w)  ee_interp(c, pc, w)
