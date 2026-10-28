// VU1 core: machine state, decoded-pair handlers and step(), shared by vu1.cpp and the generated
// block translation units (generated/vu1_blocks_*.cpp). State is defined once, in vu1.cpp (VU1_CORE_IMPL).
#pragma once
#include "ee_ops.h"
#include "ee_runtime.h"
#include "gs.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

extern u8 g_vu1_mem[0x4000];
extern u8 g_vu1_code[0x4000];
u32 vif1_top(); u32 vif1_itop();

#if defined(__GNUC__)
#define VU_INLINE inline __attribute__((always_inline))
#else
#define VU_INLINE __forceinline
#endif
#ifdef VU1_CORE_IMPL
#define VU_GLOBAL
#define VU_INIT(x) x
#else
#define VU_GLOBAL extern
#define VU_INIT(x)
#endif
namespace vu1i {
struct VU {
    alignas(16) float vf[32][4];
    u16 vi[16];
    float acc[4];
    float q = 0, p = 0, i = 0;
    float q_pending = 0, p_pending = 0;
    u64 q_ready = 0, p_ready = 0;
    u32 r = 0x3f800000;
    u32 status = 0, mac = 0, clip = 0;
    u32 pc = 0;
    u64 cycle = 0;
};
VU_GLOBAL VU V;

VU_GLOBAL u64 s_vu_instrs;
VU_GLOBAL bool s_clipdbg_on, s_clipdbg_neg; VU_GLOBAL std::string s_clipdbg; VU_GLOBAL int s_clipdbg_n;
static const long long s_clipdbg_frame = std::getenv("MVP_CLIPDBG") ? std::atoll(std::getenv("MVP_CLIPDBG")) : -1;

static inline float fclamp(float v) {
    u32 u; std::memcpy(&u, &v, 4);
    if ((u & 0x7f800000u) == 0x7f800000u) { u = (u & 0x80000000u) | 0x7f7fffffu; std::memcpy(&v, &u, 4); }
    else if ((u & 0x7f800000u) == 0) { u &= 0x80000000u; std::memcpy(&v, &u, 4); }   // denormals flush to zero
    return v;
}
static VU_INLINE void sync_q() { if (V.q_ready && V.cycle >= V.q_ready) { V.q = V.q_pending; V.q_ready = 0; } }
static VU_INLINE void sync_p() { if (V.p_ready && V.cycle >= V.p_ready) { V.p = V.p_pending; V.p_ready = 0; } }

static inline u8* mem_q(u32 addr_qw) { return g_vu1_mem + ((addr_qw & 0x3ff) << 4); }

// ---------------------------------------------------------------- upper (FMAC) instructions
struct UpperOut { int reg = -1; float v[4]; u32 dest = 0; bool acc = false; };

// FMAC flag results (MAC/status and CLIP) reach the flag registers 4 cycles after the instruction
// issues; hand-scheduled microcode relies on this (e.g. FCAND reading the CLIP result of 4 pairs ago
// while newer CLIPs are in flight). Pending updates are applied at the start of each pair.
struct FlagUpd { u64 at; u8 kind; u32 v; };            // kind 0: MAC (status derived), 1: CLIP bits
VU_GLOBAL FlagUpd s_fq[16]; VU_GLOBAL u32 s_fq_head, s_fq_tail;
static const bool s_flag_nodelay = std::getenv("MVP_VU1_NODELAY") != nullptr;
static VU_INLINE void apply_flag(const FlagUpd& u) {
    if (u.kind == 0) {
        V.mac = u.v;
        V.status = (V.status & 0xfc0) | ((u.v & 0xf) ? 1 : 0) | ((u.v & 0xf0) ? 2 : 0);
        V.status |= (V.status & 3) << 6;              // sticky
    } else V.clip = ((V.clip << 6) | u.v) & 0xffffff;
}
static VU_INLINE void push_flag(u8 kind, u32 v) {
    FlagUpd u{V.cycle + 4, kind, v};
    if (s_flag_nodelay) { apply_flag(u); return; }
    if (s_fq_tail - s_fq_head >= 16) apply_flag(s_fq[s_fq_head++ & 15]);
    s_fq[s_fq_tail++ & 15] = u;
}
static VU_INLINE void flags_advance() {
    while (s_fq_head != s_fq_tail && s_fq[s_fq_head & 15].at <= V.cycle) apply_flag(s_fq[s_fq_head++ & 15]);
}
static inline void flags_flush() { while (s_fq_head != s_fq_tail) apply_flag(s_fq[s_fq_head++ & 15]); }

VU_GLOBAL bool s_need_flags VU_INIT(= true);       // false while no loaded microcode reads MAC/status flags
static VU_INLINE void set_mac(const float r[4], u32 dest) {
    if (!s_need_flags) return;
    u32 m = 0;
    for (int k = 0; k < 4; k++) {
        if (!(dest & (8 >> k))) continue;
        u32 bit = 3 - k;                          // x -> bit 3, w -> bit 0
        if (r[k] == 0.0f) m |= 1u << bit;
        if (std::signbit(r[k])) m |= 1u << (bit + 4);
    }
    push_flag(0, m);
}

static void exec_upper(u32 w, UpperOut& o) {
    u32 dest = (w >> 21) & 15, ft = (w >> 16) & 31, fs = (w >> 11) & 31, fd = (w >> 6) & 31, fn = w & 63;
    float (*vf)[4] = V.vf;
    float r[4];
    o.dest = dest;
    auto bin = [&](int kind, bool scalar, float sv, bool to_acc, int out_reg) {
        for (int k = 0; k < 4; k++) {
            float a = vf[fs][k], b = scalar ? sv : vf[ft][k];
            switch (kind) {
            case 0: r[k] = a + b; break; case 1: r[k] = a - b; break;
            case 2: r[k] = V.acc[k] + a * b; break; case 3: r[k] = V.acc[k] - a * b; break;
            case 4: r[k] = a > b ? a : b; break; case 5: r[k] = a < b ? a : b; break;
            case 6: r[k] = a * b; break;
            }
            r[k] = fclamp(r[k]);
        }
        if (kind != 4 && kind != 5) set_mac(r, dest);
        o.acc = to_acc; o.reg = to_acc ? -2 : out_reg;
        std::memcpy(o.v, r, 16);
    };
    sync_q();
    if (fn < 60) {
        int bc = fn & 3;
        float bcv = vf[ft][bc];
        static const int kinds[7] = {0, 1, 2, 3, 4, 5, 6};
        if (fn < 28) { bin(kinds[fn >> 2], true, bcv, false, fd); return; }
        switch (fn) {
        case 28: bin(6, true, V.q, false, fd); return;  case 29: bin(4, true, V.i, false, fd); return;
        case 30: bin(6, true, V.i, false, fd); return;  case 31: bin(5, true, V.i, false, fd); return;
        case 32: bin(0, true, V.q, false, fd); return;  case 33: bin(2, true, V.q, false, fd); return;
        case 34: bin(0, true, V.i, false, fd); return;  case 35: bin(2, true, V.i, false, fd); return;
        case 36: bin(1, true, V.q, false, fd); return;  case 37: bin(3, true, V.q, false, fd); return;
        case 38: bin(1, true, V.i, false, fd); return;  case 39: bin(3, true, V.i, false, fd); return;
        case 40: bin(0, false, 0, false, fd); return;   case 41: bin(2, false, 0, false, fd); return;
        case 42: bin(6, false, 0, false, fd); return;   case 43: bin(4, false, 0, false, fd); return;
        case 44: bin(1, false, 0, false, fd); return;   case 45: bin(3, false, 0, false, fd); return;
        case 46: {                                                                   // OPMSUB
            r[0] = fclamp(V.acc[0] - vf[fs][1] * vf[ft][2]);
            r[1] = fclamp(V.acc[1] - vf[fs][2] * vf[ft][0]);
            r[2] = fclamp(V.acc[2] - vf[fs][0] * vf[ft][1]);
            r[3] = vf[fd][3];
            o.dest = 14; set_mac(r, 14); o.reg = fd; std::memcpy(o.v, r, 16); return;
        }
        case 47: bin(5, false, 0, false, fd); return;
        }
        rlog("vu1", "unknown upper fn %d (%08x)", fn, w);
        return;
    }
    int op = ((w >> 6) & 31) * 4 + (fn & 3);
    int bc = op & 3;
    if (op < 16) { static const int k[4] = {0, 1, 2, 3}; bin(k[op >> 2], true, vf[ft][bc], true, 0); return; }
    if (op >= 24 && op < 28) { bin(6, true, vf[ft][bc], true, 0); return; }
    switch (op) {
    case 16: case 17: case 18: case 19: {                                           // ITOF
        static const float sc[4] = {1.0f, 1.0f / 16, 1.0f / 4096, 1.0f / 32768};
        for (int k = 0; k < 4; k++) { s32 v; std::memcpy(&v, &vf[fs][k], 4); r[k] = (float)v * sc[op - 16]; }
        o.reg = ft; std::memcpy(o.v, r, 16); return;
    }
    case 20: case 21: case 22: case 23: {                                           // FTOI
        static const float sc[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
        for (int k = 0; k < 4; k++) {
            float f = vf[fs][k] * sc[op - 20];
            s32 v = f >= 2147483647.0f ? 0x7fffffff : f <= -2147483648.0f ? (s32)0x80000000 : (s32)f;
            std::memcpy(&r[k], &v, 4);
        }
        o.reg = ft; std::memcpy(o.v, r, 16); return;
    }
    case 28: bin(6, true, V.q, true, 0); return;
    case 29: for (int k = 0; k < 4; k++) r[k] = std::fabs(vf[fs][k]); o.reg = ft; std::memcpy(o.v, r, 16); return;   // ABS
    case 30: bin(6, true, V.i, true, 0); return;
    case 31: {                                                                      // CLIP
        float wv = std::fabs(vf[ft][3]); u32 f = 0;
        for (int k = 0; k < 3; k++) { if (vf[fs][k] > wv) f |= 1u << (k * 2); if (vf[fs][k] < -wv) f |= 2u << (k * 2); }
        push_flag(1, f);
        if (s_clipdbg_on) { char b[160]; snprintf(b, sizeof b, "  %04x CLIP (%.1f %.1f %.1f | w %.1f) -> %02x\n", V.pc, vf[fs][0], vf[fs][1], vf[fs][2], vf[ft][3], f); s_clipdbg += b; if (vf[ft][3] < 0) s_clipdbg_neg = true; }
        return;
    }
    case 32: bin(0, true, V.q, true, 0); return;  case 33: bin(2, true, V.q, true, 0); return;
    case 34: bin(0, true, V.i, true, 0); return;  case 35: bin(2, true, V.i, true, 0); return;
    case 36: bin(1, true, V.q, true, 0); return;  case 37: bin(3, true, V.q, true, 0); return;
    case 38: bin(1, true, V.i, true, 0); return;  case 39: bin(3, true, V.i, true, 0); return;
    case 40: bin(0, false, 0, true, 0); return;   case 41: bin(2, false, 0, true, 0); return;
    case 42: bin(6, false, 0, true, 0); return;   case 44: bin(1, false, 0, true, 0); return;
    case 45: bin(3, false, 0, true, 0); return;
    case 46: {                                                                      // OPMULA
        r[0] = fclamp(vf[fs][1] * vf[ft][2]); r[1] = fclamp(vf[fs][2] * vf[ft][0]); r[2] = fclamp(vf[fs][0] * vf[ft][1]); r[3] = V.acc[3];
        o.dest = 14; o.acc = true; o.reg = -2; set_mac(r, 14); std::memcpy(o.v, r, 16); return;
    }
    case 47: return;                                                                // NOP
    }
    rlog("vu1", "unknown upper special %d (%08x)", op, w);
}

static VU_INLINE void commit_upper(const UpperOut& o) {
    if (o.reg == -1) return;
    float* dst = o.reg == -2 ? V.acc : V.vf[o.reg];
    if (o.reg == 0) return;
    for (int k = 0; k < 4; k++) if (o.dest & (8 >> k)) dst[k] = o.v[k];
}

// ---------------------------------------------------------------- lower instructions
struct LowerOut { int vf_reg = -1; float v[4]; u32 dest = 0; int branch = 0; u32 target = 0; bool ebit_stop = false; };

static inline s32 imm11(u32 w) { s32 v = w & 0x7ff; return (v & 0x400) ? v - 0x800 : v; }
static inline int field_of(u32 dest) { return (dest & 8) ? 0 : (dest & 4) ? 1 : (dest & 2) ? 2 : 3; }

static void xgkick(u32 addr_qw);

static void efu(int kind, float val) {
    static const int lat[] = {11, 18, 18, 24, 54, 54, 12, 12, 18, 12, 29, 54, 44};
    V.p_pending = fclamp(val);
    V.p_ready = V.cycle + lat[kind];
}

static void exec_lower(u32 w, u32 pc, LowerOut& o) {
    u32 dest = (w >> 21) & 15, ft = (w >> 16) & 31, fs = (w >> 11) & 31, fd = (w >> 6) & 31;
    u32 it = ft & 15, is = fs & 15, id = fd & 15;
    u16* vi = V.vi;
    float (*vf)[4] = V.vf;
    o.dest = dest;
    auto setvi = [&](u32 r, u32 v) { if (r) vi[r] = (u16)v; };
    if (w & 0x80000000u) {
        u32 fn = w & 63;
        switch (fn) {
        case 0x30: setvi(id, vi[is] + vi[it]); return;
        case 0x31: setvi(id, vi[is] - vi[it]); return;
        case 0x32: { s32 imm = (w >> 6) & 31; if (imm & 16) imm -= 32; setvi(it, vi[is] + imm); return; }
        case 0x34: setvi(id, vi[is] & vi[it]); return;
        case 0x35: setvi(id, vi[is] | vi[it]); return;
        }
        if (fn < 0x3c) { if (w != 0x8000033c) rlog("vu1", "unknown lower %08x", w); return; }
        u32 op = ((w >> 6) & 31) * 4 + (fn & 3);
        u32 fsf = (w >> 21) & 3, ftf = (w >> 23) & 3;
        switch (op) {
        case 48: o.vf_reg = ft; std::memcpy(o.v, vf[fs], 16); return;                              // MOVE
        case 49: o.vf_reg = ft; o.v[0] = vf[fs][1]; o.v[1] = vf[fs][2]; o.v[2] = vf[fs][3]; o.v[3] = vf[fs][0]; return;  // MR32
        case 52: o.vf_reg = ft; std::memcpy(o.v, mem_q(vi[is]), 16); setvi(is, vi[is] + 1); return;     // LQI
        case 53: { u8* m = mem_q(vi[it]); for (int k = 0; k < 4; k++) if (dest & (8 >> k)) std::memcpy(m + k * 4, &vf[fs][k], 4); setvi(it, vi[it] + 1); return; }  // SQI
        case 54: setvi(is, vi[is] - 1); o.vf_reg = ft; std::memcpy(o.v, mem_q(vi[is]), 16); return;     // LQD
        case 55: { setvi(it, vi[it] - 1); u8* m = mem_q(vi[it]); for (int k = 0; k < 4; k++) if (dest & (8 >> k)) std::memcpy(m + k * 4, &vf[fs][k], 4); return; }  // SQD
        case 56: { float b = vf[ft][ftf]; float a = vf[fs][fsf];                                   // DIV
                   V.q_pending = b == 0 ? ((std::signbit(a) != std::signbit(b)) ? -3.4028235e38f : 3.4028235e38f) : fclamp(a / b);
                   V.q_ready = V.cycle + 7; return; }
        case 57: V.q_pending = std::sqrt(std::fabs(vf[ft][ftf])); V.q_ready = V.cycle + 7; return;    // SQRT
        case 58: { float b = std::sqrt(std::fabs(vf[ft][ftf])); V.q_pending = b == 0 ? 3.4028235e38f : fclamp(vf[fs][fsf] / b); V.q_ready = V.cycle + 13; return; }
        case 59: if (V.q_ready) { V.cycle = std::max(V.cycle, V.q_ready); sync_q(); } return;           // WAITQ
        case 60: { u32 u; std::memcpy(&u, &vf[fs][fsf], 4); setvi(it, u & 0xffff); return; }           // MTIR
        case 61: { s32 v = (s16)vi[is]; o.vf_reg = ft; for (int k = 0; k < 4; k++) std::memcpy(&o.v[k], &v, 4); return; }  // MFIR
        case 62: { u32 u; std::memcpy(&u, mem_q(vi[is]) + field_of(dest) * 4, 4); setvi(it, u); return; }   // ILWR
        case 63: { u8* m = mem_q(vi[is]); u32 v = vi[it]; for (int k = 0; k < 4; k++) if (dest & (8 >> k)) std::memcpy(m + k * 4, &v, 4); return; }  // ISWR
        case 64: { u32 x = V.r; x = ((x << 1) | (((x >> 4) ^ (x >> 22)) & 1)) & 0x7fffff; V.r = x;     // RNEXT
                   u32 f = 0x3f800000 | x; o.vf_reg = ft; for (int k = 0; k < 4; k++) std::memcpy(&o.v[k], &f, 4); return; }
        case 65: { u32 f = 0x3f800000 | (V.r & 0x7fffff); o.vf_reg = ft; for (int k = 0; k < 4; k++) std::memcpy(&o.v[k], &f, 4); return; }  // RGET
        case 66: { u32 u; std::memcpy(&u, &vf[fs][fsf], 4); V.r = 0x3f800000 | (u & 0x7fffff); return; }  // RINIT
        case 67: { u32 u; std::memcpy(&u, &vf[fs][fsf], 4); V.r = 0x3f800000 | ((V.r ^ u) & 0x7fffff); return; }  // RXOR
        case 100: sync_p(); o.vf_reg = ft; for (int k = 0; k < 4; k++) o.v[k] = V.p; return;           // MFP
        case 104: setvi(it, vif1_top()); return;                                                   // XTOP
        case 105: setvi(it, vif1_itop()); return;                                                  // XITOP
        case 108: xgkick(vi[is]); return;                                                          // XGKICK
        case 112: efu(0, vf[fs][0] * vf[fs][0] + vf[fs][1] * vf[fs][1] + vf[fs][2] * vf[fs][2]); return;      // ESADD
        case 113: { float s = vf[fs][0] * vf[fs][0] + vf[fs][1] * vf[fs][1] + vf[fs][2] * vf[fs][2]; efu(1, s ? 1.0f / s : 3.4e38f); return; }
        case 114: efu(2, std::sqrt(vf[fs][0] * vf[fs][0] + vf[fs][1] * vf[fs][1] + vf[fs][2] * vf[fs][2])); return;
        case 115: { float s = std::sqrt(vf[fs][0] * vf[fs][0] + vf[fs][1] * vf[fs][1] + vf[fs][2] * vf[fs][2]); efu(3, s ? 1.0f / s : 3.4e38f); return; }
        case 116: efu(4, std::atan2(vf[fs][1], vf[fs][0])); return;
        case 117: efu(5, std::atan2(vf[fs][2], vf[fs][0])); return;
        case 118: efu(6, vf[fs][0] + vf[fs][1] + vf[fs][2] + vf[fs][3]); return;
        case 120: efu(7, std::sqrt(std::fabs(vf[fs][fsf]))); return;
        case 121: { float s = std::sqrt(std::fabs(vf[fs][fsf])); efu(8, s ? 1.0f / s : 3.4e38f); return; }
        case 122: efu(9, vf[fs][fsf] ? 1.0f / vf[fs][fsf] : 3.4e38f); return;
        case 123: if (V.p_ready) { V.cycle = std::max(V.cycle, V.p_ready); sync_p(); } return;          // WAITP
        case 124: efu(10, std::sin(vf[fs][fsf])); return;
        case 125: efu(11, std::atan(vf[fs][fsf])); return;
        case 126: efu(12, std::exp(-vf[fs][fsf])); return;
        }
        rlog("vu1", "unknown lower special %u (%08x)", op, w);
        return;
    }
    u32 op = w >> 25;
    s32 i11 = imm11(w);
    u32 target = (u32)((s32)(pc / 8) + 1 + i11) * 8;
    switch (op) {
    case 0x00: o.vf_reg = ft; std::memcpy(o.v, mem_q(vi[is] + i11), 16); return;                       // LQ
    case 0x01: { u8* m = mem_q(vi[it] + i11); for (int k = 0; k < 4; k++) if (dest & (8 >> k)) std::memcpy(m + k * 4, &vf[fs][k], 4); return; }  // SQ
    case 0x04: { u32 u; std::memcpy(&u, mem_q(vi[is] + i11) + field_of(dest) * 4, 4); setvi(it, u); return; }  // ILW
    case 0x05: { u8* m = mem_q(vi[is] + i11); u32 v = vi[it]; for (int k = 0; k < 4; k++) if (dest & (8 >> k)) std::memcpy(m + k * 4, &v, 4); return; }  // ISW
    case 0x08: setvi(it, vi[is] + (((w >> 10) & 0x7800) | (w & 0x7ff))); return;                      // IADDIU
    case 0x09: setvi(it, vi[is] - (((w >> 10) & 0x7800) | (w & 0x7ff))); return;                      // ISUBIU
    case 0x10: vi[1] = (V.clip & 0xffffff) == (w & 0xffffff); return;                                 // FCEQ
    case 0x11: V.clip = w & 0xffffff; return;                                                         // FCSET
    case 0x12: vi[1] = (V.clip & w & 0xffffff) != 0;                                                  // FCAND
               if (s_clipdbg_on) { char b[96]; snprintf(b, sizeof b, "  %04x FCAND %06x clip=%06x -> %d\n", pc, w & 0xffffff, V.clip, vi[1]); s_clipdbg += b; } return;
    case 0x13: vi[1] = ((V.clip | w) & 0xffffff) == 0xffffff;                                         // FCOR
               if (s_clipdbg_on) { char b[96]; snprintf(b, sizeof b, "  %04x FCOR %06x clip=%06x -> %d\n", pc, w & 0xffffff, V.clip, vi[1]); s_clipdbg += b; } return;
    case 0x14: setvi(it, (V.status & 0xfff) == ((((w >> 10) & 0x800) | (w & 0x7ff)))); return;       // FSEQ
    case 0x15: V.status = (V.status & 0x3f) | ((((w >> 10) & 0x800) | (w & 0x7ff)) & 0xfc0); return;  // FSSET
    case 0x16: setvi(it, V.status & (((w >> 10) & 0x800) | (w & 0x7ff))); return;                     // FSAND
    case 0x17: setvi(it, V.status | (((w >> 10) & 0x800) | (w & 0x7ff))); return;                     // FSOR
    case 0x18: setvi(it, (V.mac & 0xffff) == vi[is]); return;                                         // FMEQ
    case 0x1a: setvi(it, V.mac & vi[is]); return;                                                     // FMAND
    case 0x1b: setvi(it, V.mac | vi[is]); return;                                                     // FMOR
    case 0x1c: setvi(it, V.clip & 0xfff); return;                                                     // FCGET
    case 0x20: o.branch = 1; o.target = target; return;                                               // B
    case 0x21: setvi(it, (pc + 16) / 8); o.branch = 1; o.target = target; return;                     // BAL
    case 0x24: o.branch = 1; o.target = vi[is] * 8; return;                                           // JR
    case 0x25: { u32 t = vi[is] * 8; setvi(it, (pc + 16) / 8); o.branch = 1; o.target = t; return; } // JALR
    case 0x28: if (vi[it] == vi[is]) { o.branch = 1; o.target = target; } return;                      // IBEQ
    case 0x29: if (vi[it] != vi[is]) { o.branch = 1; o.target = target; } return;                      // IBNE
    case 0x2c: if ((s16)vi[is] < 0) { o.branch = 1; o.target = target; } return;                       // IBLTZ
    case 0x2d: if ((s16)vi[is] > 0) { o.branch = 1; o.target = target; } return;                       // IBGTZ
    case 0x2e: if ((s16)vi[is] <= 0) { o.branch = 1; o.target = target; } return;                      // IBLEZ
    case 0x2f: if ((s16)vi[is] >= 0) { o.branch = 1; o.target = target; } return;                      // IBGEZ
    }
    rlog("vu1", "unknown lower op %02x (%08x)", op, w);
}

// ---------------------------------------------------------------- XGKICK: PATH1 GIF packet from VU memory
VU_GLOBAL bool s_xgkick_mute;          // XGKICK suppressed (block self-check re-runs)
static inline bool s_xgkick_mute_fwd() { return s_xgkick_mute; }
static void xgkick(u32 addr_qw) {
    if (s_xgkick_mute_fwd()) return;
    u32 a = addr_qw & 0x3ff;
    for (int guard = 0; guard < 1024; guard++) {
        u8* tagp = mem_q(a);
        u64 lo; std::memcpy(&lo, tagp, 8);
        u32 nloop = lo & 0x7fff, eop = (lo >> 15) & 1, flg = (lo >> 58) & 3, nreg = (lo >> 60) & 15;
        if (!nreg) nreg = 16;
        u32 qw = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
        // copy out (VU memory wraps at 16 KB)
        static u8 buf[0x4000 + 16];
        u32 total = 1 + qw;
        if (total > 0x400) total = 0x400;
        for (u32 k = 0; k < total; k++) std::memcpy(buf + k * 16, mem_q(a + k), 16);
        gs_gif_write(buf, total, 1);
        a = (a + total) & 0x3ff;
        if (eop) break;
    }
}

// ---------------------------------------------------------------- predecoded execution
// Every instruction pair in micro memory is decoded once into handler pointers (re-decoded only when
// an MPG upload changes it). Common FMAC ops get template-specialised handlers; everything else goes
// through the generic exec_upper/exec_lower above, so behaviour is identical to the plain interpreter.
struct Dec;
typedef void (*UpFn)(const Dec&, UpperOut&);
typedef void (*LoFn)(const Dec&, u32 pc, LowerOut&);
struct Dec {
    UpFn up; LoFn lo; u32 upper, lower;
    u8 dest, ft, fs, fd, bc;
    u8 ldest, lft, lfs, lit, lis, lid, lfsf, lftf;
    s32 limm; u32 ltarget;
    bool valid, ibit, ebit, lower_nop, reads_flags, lower_wait;   // lower_wait: WAITQ/WAITP (stalls the whole pair)
};
VU_GLOBAL Dec s_dec[0x800];
VU_GLOBAL int s_flag_readers;
static const bool s_force_flags = std::getenv("MVP_VU1_FLAGS") != nullptr;

static VU_INLINE float fclamp_fast(float v) {
    u32 u; std::memcpy(&u, &v, 4);
    u32 e = u & 0x7f800000u;
    if (e == 0x7f800000u) u = (u & 0x80000000u) | 0x7f7fffffu;
    else if (e == 0) u &= 0x80000000u;
    std::memcpy(&v, &u, 4);
    return v;
}
#if defined(__GNUC__) || defined(__clang__)
// The same on all four components at once. Vector types of the compiler (GCC and clang, SSE and NEON alike), so
// there is one version to keep right; every operation is the scalar one above, component by component.
typedef float v4f __attribute__((vector_size(16)));
typedef u32 v4u __attribute__((vector_size(16)));
typedef s32 v4i __attribute__((vector_size(16)));
static VU_INLINE v4f vclamp_fast(v4f x) {
    const v4u u = (v4u)x, em = {0x7f800000u, 0x7f800000u, 0x7f800000u, 0x7f800000u};
    const v4u e = u & em;
    const v4u inf = (v4u)(e == em), den = (v4u)(e == (v4u){0, 0, 0, 0}), any = inf | den;
    const v4u sign = u & (v4u){0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u};
    return (v4f)((u & ~any) | (any & sign) | (inf & (v4u){0x7f7fffffu, 0x7f7fffffu, 0x7f7fffffu, 0x7f7fffffu}));
}
// K: 0 add 1 sub 2 madd 3 msub 4 max 5 mini 6 mul.  S: 0 vector ft, 1 broadcast ft[bc], 2 Q, 3 I
template <int K, int S, bool ACC>
static VU_INLINE void up_bin(const Dec& d, UpperOut& o) {
    v4f a, b, x;
    std::memcpy(&a, V.vf[d.fs], 16);
    if (S == 0) std::memcpy(&b, V.vf[d.ft], 16);
    else { const float sv = S == 1 ? V.vf[d.ft][d.bc] : S == 2 ? V.q : V.i; b = (v4f){sv, sv, sv, sv}; }
    if (K == 2 || K == 3) {
        v4f acc; std::memcpy(&acc, V.acc, 16);
        x = K == 2 ? acc + a * b : acc - a * b;
    } else if (K == 4 || K == 5) {
        const v4u m = K == 4 ? (v4u)(a > b) : (v4u)(a < b);              // false for NaN: b, as "a > b ? a : b"
        x = (v4f)((m & (v4u)a) | (~m & (v4u)b));
    } else x = K == 0 ? a + b : K == 1 ? a - b : a * b;
    const v4f r = vclamp_fast(x);
    std::memcpy(o.v, &r, 16);
    if (K != 4 && K != 5) set_mac(o.v, d.dest);
    o.dest = d.dest; o.acc = ACC; o.reg = ACC ? -2 : d.fd;
}
#else
// K: 0 add 1 sub 2 madd 3 msub 4 max 5 mini 6 mul.  S: 0 vector ft, 1 broadcast ft[bc], 2 Q, 3 I
template <int K, int S, bool ACC>
static VU_INLINE void up_bin(const Dec& d, UpperOut& o) {
    const float* a = V.vf[d.fs];
    const float* bv = V.vf[d.ft];
    float sv = S == 1 ? bv[d.bc] : S == 2 ? V.q : S == 3 ? V.i : 0.0f;
    float r[4];
    for (int k = 0; k < 4; k++) {
        float b = S == 0 ? bv[k] : sv, x;
        switch (K) {
        case 0: x = a[k] + b; break; case 1: x = a[k] - b; break;
        case 2: x = V.acc[k] + a[k] * b; break; case 3: x = V.acc[k] - a[k] * b; break;
        case 4: x = a[k] > b ? a[k] : b; break; case 5: x = a[k] < b ? a[k] : b; break;
        default: x = a[k] * b; break;
        }
        r[k] = fclamp_fast(x);
    }
    if (K != 4 && K != 5) set_mac(r, d.dest);
    o.dest = d.dest; o.acc = ACC; o.reg = ACC ? -2 : d.fd;
    std::memcpy(o.v, r, 16);
}
#endif
static void up_generic(const Dec& d, UpperOut& o) { exec_upper(d.upper, o); }
// CLIP, ITOF/FTOI, ABS (same semantics as exec_upper)
static VU_INLINE void up_clip(const Dec& d, UpperOut&) {
    const float* a = V.vf[d.fs]; float wv = std::fabs(V.vf[d.ft][3]); u32 f = 0;
    for (int k = 0; k < 3; k++) { if (a[k] > wv) f |= 1u << (k * 2); if (a[k] < -wv) f |= 2u << (k * 2); }
    push_flag(1, f);
}
template <int N> static VU_INLINE void up_itof(const Dec& d, UpperOut& o) {
    static const float sc[4] = {1.0f, 1.0f / 16, 1.0f / 4096, 1.0f / 32768};
    for (int k = 0; k < 4; k++) { s32 v; std::memcpy(&v, &V.vf[d.fs][k], 4); o.v[k] = (float)v * sc[N]; }
    o.dest = d.dest; o.reg = d.ft;
}
template <int N> static VU_INLINE void up_ftoi(const Dec& d, UpperOut& o) {
    static const float sc[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
    for (int k = 0; k < 4; k++) {
        float f = V.vf[d.fs][k] * sc[N];
        s32 v = f >= 2147483647.0f ? 0x7fffffff : f <= -2147483648.0f ? (s32)0x80000000 : (s32)f;
        std::memcpy(&o.v[k], &v, 4);
    }
    o.dest = d.dest; o.reg = d.ft;
}
static VU_INLINE void up_abs(const Dec& d, UpperOut& o) { for (int k = 0; k < 4; k++) o.v[k] = std::fabs(V.vf[d.fs][k]); o.dest = d.dest; o.reg = d.ft; }
static VU_INLINE void up_nop(const Dec&, UpperOut&) {}

static UpFn pick_upper(u32 w) {
    u32 fn = w & 63;
    if (fn < 60) {
        if (fn < 28) {
            switch (fn >> 2) {
            case 0: return up_bin<0, 1, false>; case 1: return up_bin<1, 1, false>; case 2: return up_bin<2, 1, false>;
            case 3: return up_bin<3, 1, false>; case 4: return up_bin<4, 1, false>; case 5: return up_bin<5, 1, false>;
            case 6: return up_bin<6, 1, false>;
            }
        }
        switch (fn) {
        case 28: return up_bin<6, 2, false>; case 29: return up_bin<4, 3, false>; case 30: return up_bin<6, 3, false>;
        case 31: return up_bin<5, 3, false>; case 32: return up_bin<0, 2, false>; case 33: return up_bin<2, 2, false>;
        case 34: return up_bin<0, 3, false>; case 35: return up_bin<2, 3, false>; case 36: return up_bin<1, 2, false>;
        case 37: return up_bin<3, 2, false>; case 38: return up_bin<1, 3, false>; case 39: return up_bin<3, 3, false>;
        case 40: return up_bin<0, 0, false>; case 41: return up_bin<2, 0, false>; case 42: return up_bin<6, 0, false>;
        case 43: return up_bin<4, 0, false>; case 44: return up_bin<1, 0, false>; case 45: return up_bin<3, 0, false>;
        case 47: return up_bin<5, 0, false>;
        }
        return up_generic;
    }
    int op = ((w >> 6) & 31) * 4 + (fn & 3);
    if (op < 16) {
        switch (op >> 2) { case 0: return up_bin<0, 1, true>; case 1: return up_bin<1, 1, true>;
                           case 2: return up_bin<2, 1, true>; default: return up_bin<3, 1, true>; }
    }
    if (op >= 24 && op < 28) return up_bin<6, 1, true>;
    switch (op) {
    case 16: return up_itof<0>; case 17: return up_itof<1>; case 18: return up_itof<2>; case 19: return up_itof<3>;
    case 20: return up_ftoi<0>; case 21: return up_ftoi<1>; case 22: return up_ftoi<2>; case 23: return up_ftoi<3>;
    case 29: return up_abs; case 31: return up_clip;
    case 28: return up_bin<6, 2, true>; case 30: return up_bin<6, 3, true>;
    case 32: return up_bin<0, 2, true>; case 33: return up_bin<2, 2, true>; case 34: return up_bin<0, 3, true>;
    case 35: return up_bin<2, 3, true>; case 36: return up_bin<1, 2, true>; case 37: return up_bin<3, 2, true>;
    case 38: return up_bin<1, 3, true>; case 39: return up_bin<3, 3, true>;
    case 40: return up_bin<0, 0, true>; case 41: return up_bin<2, 0, true>; case 42: return up_bin<6, 0, true>;
    case 44: return up_bin<1, 0, true>; case 45: return up_bin<3, 0, true>;
    case 47: return up_nop;
    }
    return up_generic;
}
// ---- specialised lower handlers (identical semantics to exec_lower)
static void lo_generic(const Dec& d, u32 pc, LowerOut& o) { exec_lower(d.lower, pc, o); }
static VU_INLINE void lo_lq(const Dec& d, u32, LowerOut& o) { o.dest = d.ldest; o.vf_reg = d.lft; std::memcpy(o.v, mem_q(V.vi[d.lis] + d.limm), 16); }
static VU_INLINE void lo_sq(const Dec& d, u32, LowerOut&) {
    u8* m = mem_q(V.vi[d.lit] + d.limm); const float* f = V.vf[d.lfs];
    if (d.ldest == 15) { std::memcpy(m, f, 16); return; }
    for (int k = 0; k < 4; k++) if (d.ldest & (8 >> k)) std::memcpy(m + k * 4, &f[k], 4);
}
static VU_INLINE void lo_lqi(const Dec& d, u32, LowerOut& o) {
    o.dest = d.ldest; o.vf_reg = d.lft; std::memcpy(o.v, mem_q(V.vi[d.lis]), 16); if (d.lis) V.vi[d.lis]++;
}
static VU_INLINE void lo_sqi(const Dec& d, u32, LowerOut&) {
    u8* m = mem_q(V.vi[d.lit]); const float* f = V.vf[d.lfs];
    if (d.ldest == 15) std::memcpy(m, f, 16);
    else for (int k = 0; k < 4; k++) if (d.ldest & (8 >> k)) std::memcpy(m + k * 4, &f[k], 4);
    if (d.lit) V.vi[d.lit]++;
}
static VU_INLINE void lo_iaddiu(const Dec& d, u32, LowerOut&) { if (d.lit) V.vi[d.lit] = (u16)(V.vi[d.lis] + d.limm); }
static VU_INLINE void lo_isubiu(const Dec& d, u32, LowerOut&) { if (d.lit) V.vi[d.lit] = (u16)(V.vi[d.lis] - d.limm); }
static VU_INLINE void lo_iaddi(const Dec& d, u32, LowerOut&) { if (d.lit) V.vi[d.lit] = (u16)(V.vi[d.lis] + d.limm); }
static VU_INLINE void lo_iadd(const Dec& d, u32, LowerOut&) { if (d.lid) V.vi[d.lid] = (u16)(V.vi[d.lis] + V.vi[d.lit]); }
static VU_INLINE void lo_isub(const Dec& d, u32, LowerOut&) { if (d.lid) V.vi[d.lid] = (u16)(V.vi[d.lis] - V.vi[d.lit]); }
// Target of a pc-relative branch, from the pair's own pc: compiled blocks run wherever the game loaded the code,
// and with the pair's Dec left a compile-time constant its handlers are inlined (a copy with the target patched
// in made the compiler call them through pointers: one such pair in every block).
static VU_INLINE u32 rel_target(const Dec& d, u32 pc) { return (u32)((s32)(pc / 8) + 1 + imm11(d.lower)) * 8; }
static VU_INLINE void lo_ibne(const Dec& d, u32 pc, LowerOut& o) { if (V.vi[d.lit] != V.vi[d.lis]) { o.branch = 1; o.target = rel_target(d, pc); } }
static VU_INLINE void lo_ibeq(const Dec& d, u32 pc, LowerOut& o) { if (V.vi[d.lit] == V.vi[d.lis]) { o.branch = 1; o.target = rel_target(d, pc); } }
static VU_INLINE void lo_b(const Dec& d, u32 pc, LowerOut& o) { o.branch = 1; o.target = rel_target(d, pc); }
static VU_INLINE void lo_move(const Dec& d, u32, LowerOut& o) { o.dest = d.ldest; o.vf_reg = d.lft; std::memcpy(o.v, V.vf[d.lfs], 16); }
static VU_INLINE void lo_div(const Dec& d, u32, LowerOut&) {
    float b = V.vf[d.lft][d.lftf], a = V.vf[d.lfs][d.lfsf];
    V.q_pending = b == 0 ? ((std::signbit(a) != std::signbit(b)) ? -3.4028235e38f : 3.4028235e38f) : fclamp(a / b);
    V.q_ready = V.cycle + 7;
}
static VU_INLINE void lo_ilw(const Dec& d, u32, LowerOut&) { u32 u; std::memcpy(&u, mem_q(V.vi[d.lis] + d.limm) + field_of(d.ldest) * 4, 4); if (d.lit) V.vi[d.lit] = (u16)u; }
static VU_INLINE void lo_isw(const Dec& d, u32, LowerOut&) { u8* m = mem_q(V.vi[d.lis] + d.limm); u32 v = V.vi[d.lit]; for (int k = 0; k < 4; k++) if (d.ldest & (8 >> k)) std::memcpy(m + k * 4, &v, 4); }
static VU_INLINE void lo_fcand(const Dec& d, u32, LowerOut&) { V.vi[1] = (V.clip & d.lower & 0xffffff) != 0; }
static VU_INLINE void lo_fcor(const Dec& d, u32, LowerOut&) { V.vi[1] = ((V.clip | d.lower) & 0xffffff) == 0xffffff; }
static VU_INLINE void lo_fceq(const Dec& d, u32, LowerOut&) { V.vi[1] = (V.clip & 0xffffff) == (d.lower & 0xffffff); }
static VU_INLINE void lo_fcset(const Dec& d, u32, LowerOut&) { V.clip = d.lower & 0xffffff; }
static VU_INLINE void lo_fcget(const Dec& d, u32, LowerOut&) { if (d.lit) V.vi[d.lit] = (u16)(V.clip & 0xfff); }
static VU_INLINE void lo_mtir(const Dec& d, u32, LowerOut&) { u32 u; std::memcpy(&u, &V.vf[d.lfs][d.lfsf], 4); if (d.lit) V.vi[d.lit] = (u16)u; }
static VU_INLINE void lo_mfir(const Dec& d, u32, LowerOut& o) { s32 v = (s16)V.vi[d.lis]; o.dest = d.ldest; o.vf_reg = d.lft; for (int k = 0; k < 4; k++) std::memcpy(&o.v[k], &v, 4); }
static VU_INLINE void lo_iand(const Dec& d, u32, LowerOut&) { if (d.lid) V.vi[d.lid] = V.vi[d.lis] & V.vi[d.lit]; }
static VU_INLINE void lo_ior(const Dec& d, u32, LowerOut&) { if (d.lid) V.vi[d.lid] = V.vi[d.lis] | V.vi[d.lit]; }
static VU_INLINE void lo_mr32(const Dec& d, u32, LowerOut& o) { const float* f = V.vf[d.lfs]; o.dest = d.ldest; o.vf_reg = d.lft; o.v[0] = f[1]; o.v[1] = f[2]; o.v[2] = f[3]; o.v[3] = f[0]; }
static VU_INLINE void lo_ibgtz(const Dec& d, u32 pc, LowerOut& o) { if ((s16)V.vi[d.lis] > 0) { o.branch = 1; o.target = rel_target(d, pc); } }
static VU_INLINE void lo_ibltz(const Dec& d, u32 pc, LowerOut& o) { if ((s16)V.vi[d.lis] < 0) { o.branch = 1; o.target = rel_target(d, pc); } }
static VU_INLINE void lo_iblez(const Dec& d, u32 pc, LowerOut& o) { if ((s16)V.vi[d.lis] <= 0) { o.branch = 1; o.target = rel_target(d, pc); } }
static VU_INLINE void lo_ibgez(const Dec& d, u32 pc, LowerOut& o) { if ((s16)V.vi[d.lis] >= 0) { o.branch = 1; o.target = rel_target(d, pc); } }
static VU_INLINE void lo_waitq(const Dec&, u32, LowerOut&) { if (V.q_ready) { V.cycle = std::max(V.cycle, V.q_ready); sync_q(); } }
static LoFn pick_lower(Dec& d) {
    u32 w = d.lower;
    d.ldest = (w >> 21) & 15; d.lft = (w >> 16) & 31; d.lfs = (w >> 11) & 31;
    d.lit = d.lft & 15; d.lis = d.lfs & 15; d.lid = ((w >> 6) & 31) & 15;
    d.lfsf = (w >> 21) & 3; d.lftf = (w >> 23) & 3;
    if (w & 0x80000000u) {
        u32 fn = w & 63;
        switch (fn) {
        case 0x30: return lo_iadd;
        case 0x31: return lo_isub;
        case 0x32: { s32 imm = (w >> 6) & 31; if (imm & 16) imm -= 32; d.limm = imm; return lo_iaddi; }
        case 0x34: return lo_iand;
        case 0x35: return lo_ior;
        }
        if (fn < 0x3c) return lo_generic;
        u32 op = ((w >> 6) & 31) * 4 + (fn & 3);
        switch (op) {
        case 48: return lo_move;
        case 49: return lo_mr32;
        case 60: return lo_mtir;
        case 61: return lo_mfir;
        case 52: return lo_lqi;
        case 53: return lo_sqi;
        case 56: return lo_div;
        case 59: return lo_waitq;
        }
        return lo_generic;
    }
    u32 op = w >> 25;
    s32 i11 = imm11(w);
    d.limm = i11;
    switch (op) {
    case 0x00: return lo_lq;
    case 0x01: return lo_sq;
    case 0x04: return lo_ilw;
    case 0x05: return lo_isw;
    case 0x10: return lo_fceq;
    case 0x11: return lo_fcset;
    case 0x12: return lo_fcand;
    case 0x13: return lo_fcor;
    case 0x1c: return lo_fcget;
    case 0x08: d.limm = (s32)(((w >> 10) & 0x7800) | (w & 0x7ff)); return lo_iaddiu;
    case 0x09: d.limm = (s32)(((w >> 10) & 0x7800) | (w & 0x7ff)); return lo_isubiu;
    }
    return lo_generic;                   // branches need the pair's pc: resolved per decode below
}
static bool lower_reads_flags(u32 w) {
    if (w & 0x80000000u) return false;
    u32 op = w >> 25;
    return (op >= 0x14 && op <= 0x1b);        // FSEQ/FSSET/FSAND/FSOR/FMEQ/FMAND/FMOR
}
// (the count of flag-reading pairs is kept by vu1_code_uploaded, for all of micro memory, decoded or not)
static void decode(u32 idx) {
    Dec& d = s_dec[idx];
    std::memcpy(&d.lower, g_vu1_code + idx * 8, 4);
    std::memcpy(&d.upper, g_vu1_code + idx * 8 + 4, 4);
    u32 w = d.upper;
    d.dest = (w >> 21) & 15; d.ft = (w >> 16) & 31; d.fs = (w >> 11) & 31; d.fd = (w >> 6) & 31; d.bc = w & 3;
    d.up = pick_upper(w);
    d.ibit = w & 0x80000000u; d.ebit = w & 0x40000000u;
    d.lower_nop = d.ibit || d.lower == 0x8000033cu;
    {   // WAITQ / WAITP stall the pair before it issues, so the upper instruction of the same pair already sees
        // the new Q / P (EAGL's near-plane clipper does "MULq ... | WAITQ")
        u32 lw = d.lower; int op = ((lw >> 6) & 31) * 4 + (lw & 3);
        d.lower_wait = !d.ibit && (lw & 0x80000000u) && (lw & 63) >= 0x3c && (op == 59 || op == 123);
    }
    d.lo = d.lower_nop ? lo_generic : pick_lower(d);
    if (!d.lower_nop && !(d.lower & 0x80000000u)) {        // PC-relative branches: target is fixed per pair
        u32 op = d.lower >> 25; u32 pc = idx * 8;
        d.ltarget = (u32)((s32)(pc / 8) + 1 + imm11(d.lower)) * 8;
        if (op == 0x20) d.lo = lo_b;
        else if (op == 0x28) d.lo = lo_ibeq;
        else if (op == 0x29) d.lo = lo_ibne;
        else if (op == 0x2c) d.lo = lo_ibltz;
        else if (op == 0x2d) d.lo = lo_ibgtz;
        else if (op == 0x2e) d.lo = lo_iblez;
        else if (op == 0x2f) d.lo = lo_ibgez;
    }
    d.reads_flags = !d.ibit && lower_reads_flags(d.lower);
    d.valid = true;
}

// ---------------------------------------------------------------- one instruction pair
struct Run { u32 branch_pending = 0; bool have_branch = false, ending = false, done = false; };
// Lower ops that read or overwrite the flag registers (FCEQ..FCGET, FSxxx, FMxxx: opcodes 0x10-0x1c).
// Delayed flag results are applied lazily, just before one of these (and at the end of the program):
// the queue is FIFO, so the registers they see are exactly those eager per-pair application would give.
static VU_INLINE bool touches_flags(const Dec& d) {
    return !d.ibit && !(d.lower & 0x80000000u) && (d.lower >> 25) >= 0x10 && (d.lower >> 25) <= 0x1c;
}
// COUNT = false: the caller adds the pairs to s_vu_instrs itself (compiled blocks, once per block).
template <bool COUNT> static VU_INLINE void step_t(const Dec& d, u32 pc, Run& r) {
    if (COUNT) s_vu_instrs++;
    V.cycle++;
    if (touches_flags(d)) flags_advance();
    UpperOut uo; LowerOut lo;
    if (d.lower_wait) {
        if (V.q_ready && ((d.lower >> 6) & 31) == 14) V.cycle = std::max(V.cycle, V.q_ready);
        if (V.p_ready && ((d.lower >> 6) & 31) == 30) V.cycle = std::max(V.cycle, V.p_ready);
        sync_p();
    }
    sync_q();
    d.up(d, uo);
    if (d.ibit) std::memcpy(&V.i, &d.lower, 4);    // I bit: lower word is an immediate for I
    else if (!d.lower_nop) d.lo(d, pc, lo);
    // commit: upper result wins when both write the same register
    if (lo.vf_reg > 0 && !(uo.reg == lo.vf_reg)) {
        for (int k = 0; k < 4; k++) if (lo.dest & (8 >> k)) V.vf[lo.vf_reg][k] = lo.v[k];
    }
    commit_upper(uo);
    V.vi[0] = 0;
    sync_q(); sync_p();
    u32 next = (pc + 8) & 0x3ff8;
    if (r.have_branch) { next = r.branch_pending & 0x3ff8; r.have_branch = false; }
    if (lo.branch) { r.have_branch = true; r.branch_pending = lo.target; }
    V.pc = next;
    if (r.ending) { flags_flush(); r.done = true; return; }           // this was the E-bit delay slot
    if (d.ebit) r.ending = true;                                       // E bit: execute one more pair
}

static VU_INLINE void step(const Dec& d, u32 pc, Run& r) { step_t<true>(d, pc, r); }

typedef void (*VuBlockFn)(u32 start, Run& r);
struct VuBlockDef { u64 hash; u32 npairs; const u32* words; VuBlockFn fn; };
static VU_INLINE Dec rel_branch(Dec d, u32 pc) { d.ltarget = (u32)((s32)(pc / 8) + 1 + imm11(d.lower)) * 8; return d; }
} // namespace vu1i
