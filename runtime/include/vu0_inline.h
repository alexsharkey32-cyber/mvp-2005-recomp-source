// VU0 macro-mode (COP2) instructions specialised at compile time. The recompiled code calls
// VU0OP(word) with a constant instruction word; vu0t<word> folds the decode away so each call site becomes
// a few float operations. Semantics match vu0_macro() in ee_hw.cpp, which handles everything not listed here.
#pragma once
#include "ee_context.h"
#include <cmath>
#include <cstring>

void vu0_macro(EEContext* c, u32 word);

namespace vu0i {
#if defined(__GNUC__)
#define VU0_INL inline __attribute__((always_inline))
#else
#define VU0_INL __forceinline
#endif
static VU0_INL float clampf(float v) {
    u32 u; std::memcpy(&u, &v, 4);
    if ((u & 0x7f800000u) == 0x7f800000u) { u = (u & 0x80000000u) | 0x7f7fffffu; std::memcpy(&v, &u, 4); }
    return v;
}
// K: 0 add 1 sub 2 madd 3 msub 4 max 5 min 6 mul.  S: 0 vector ft, 1 ft[bc], 2 Q, 3 I.  ACC: result to ACC
template <u32 W, int K, int S, bool ACC>
static VU0_INL void bin(EEContext* c) {
    constexpr int dest = (W >> 21) & 15, ft = (W >> 16) & 31, fs = (W >> 11) & 31, fd = (W >> 6) & 31, bc = W & 3;
    float (*vf)[4] = c->vf; float* acc = c->vacc;
    float sv = 0;
    if constexpr (S == 1) sv = vf[ft][bc];
    else if constexpr (S == 2) sv = c->vq;
    else if constexpr (S == 3) std::memcpy(&sv, &c->vi[21], 4);
    float r[4];
    for (int i = 0; i < 4; i++) {
        const float a = vf[fs][i], b = S == 0 ? vf[ft][i] : sv;
        if constexpr (K == 0) r[i] = a + b;
        else if constexpr (K == 1) r[i] = a - b;
        else if constexpr (K == 2) r[i] = acc[i] + a * b;
        else if constexpr (K == 3) r[i] = acc[i] - a * b;
        else if constexpr (K == 4) r[i] = a > b ? a : b;
        else if constexpr (K == 5) r[i] = a < b ? a : b;
        else r[i] = a * b;
    }
    if constexpr (ACC) { for (int i = 0; i < 4; i++) if (dest & (8 >> i)) acc[i] = clampf(r[i]); }
    else if constexpr (fd != 0) { for (int i = 0; i < 4; i++) if (dest & (8 >> i)) vf[fd][i] = clampf(r[i]); }
}
template <u32 W>
static VU0_INL void op(EEContext* c) {
    constexpr int dest = (W >> 21) & 15, ft = (W >> 16) & 31, fs = (W >> 11) & 31, fd = (W >> 6) & 31, fn = W & 63;
    constexpr int it = ft & 15, is = fs & 15, id = fd & 15;
    float (*vf)[4] = c->vf;
    if constexpr (fn < 60) {
        if constexpr (fn < 28) {
            constexpr int k = fn >> 2;                         // ADDbc SUBbc MADDbc MSUBbc MAXbc MINIbc MULbc
            bin<W, k, 1, false>(c);
        }
        else if constexpr (fn == 28) bin<W, 6, 2, false>(c);  // MULq
        else if constexpr (fn == 29) bin<W, 4, 3, false>(c);  // MAXi
        else if constexpr (fn == 30) bin<W, 6, 3, false>(c);  // MULi
        else if constexpr (fn == 31) bin<W, 5, 3, false>(c);  // MINIi
        else if constexpr (fn == 32) bin<W, 0, 2, false>(c);  // ADDq
        else if constexpr (fn == 33) bin<W, 2, 2, false>(c);  // MADDq
        else if constexpr (fn == 34) bin<W, 0, 3, false>(c);  // ADDi
        else if constexpr (fn == 35) bin<W, 2, 3, false>(c);  // MADDi
        else if constexpr (fn == 36) bin<W, 1, 2, false>(c);  // SUBq
        else if constexpr (fn == 37) bin<W, 3, 2, false>(c);  // MSUBq
        else if constexpr (fn == 38) bin<W, 1, 3, false>(c);  // SUBi
        else if constexpr (fn == 39) bin<W, 3, 3, false>(c);  // MSUBi
        else if constexpr (fn == 40) bin<W, 0, 0, false>(c);  // ADD
        else if constexpr (fn == 41) bin<W, 2, 0, false>(c);  // MADD
        else if constexpr (fn == 42) bin<W, 6, 0, false>(c);  // MUL
        else if constexpr (fn == 43) bin<W, 4, 0, false>(c);  // MAX
        else if constexpr (fn == 44) bin<W, 1, 0, false>(c);  // SUB
        else if constexpr (fn == 45) bin<W, 3, 0, false>(c);  // MSUB
        else if constexpr (fn == 47) bin<W, 5, 0, false>(c);  // MINI
        else if constexpr (fn == 48) { if constexpr (id != 0) c->vi[id] = (u16)(c->vi[is] + c->vi[it]); }      // IADD
        else if constexpr (fn == 49) { if constexpr (id != 0) c->vi[id] = (u16)(c->vi[is] - c->vi[it]); }      // ISUB
        else if constexpr (fn == 50) {                                                                        // IADDI
            constexpr int imm = ((W >> 6) & 31) & 16 ? (int)((W >> 6) & 31) - 32 : (int)((W >> 6) & 31);
            if constexpr (it != 0) c->vi[it] = (u16)(c->vi[is] + imm);
        }
        else if constexpr (fn == 52) { if constexpr (id != 0) c->vi[id] = c->vi[is] & c->vi[it]; }             // IAND
        else if constexpr (fn == 53) { if constexpr (id != 0) c->vi[id] = c->vi[is] | c->vi[it]; }             // IOR
        else vu0_macro(c, W);
    } else {
        constexpr int op2 = ((W >> 6) & 31) * 4 + (fn & 3);
        if constexpr (op2 < 16) bin<W, (op2 >> 2), 1, true>(c);                  // ADDAbc SUBAbc MADDAbc MSUBAbc
        else if constexpr (op2 >= 24 && op2 < 28) bin<W, 6, 1, true>(c);        // MULAbc
        else if constexpr (op2 >= 16 && op2 < 20) {                             // ITOF0/4/12/15
            constexpr float sc = op2 == 16 ? 1.0f : op2 == 17 ? 1.0f / 16 : op2 == 18 ? 1.0f / 4096 : 1.0f / 32768;
            float r[4];
            for (int i = 0; i < 4; i++) { s32 v; std::memcpy(&v, &vf[fs][i], 4); r[i] = v * sc; }
            if constexpr (ft != 0) { for (int i = 0; i < 4; i++) if (dest & (8 >> i)) vf[ft][i] = clampf(r[i]); }
        }
        else if constexpr (op2 == 28) bin<W, 6, 2, true>(c);    // MULAq
        else if constexpr (op2 == 30) bin<W, 6, 3, true>(c);    // MULAi
        else if constexpr (op2 == 32) bin<W, 0, 2, true>(c);    // ADDAq
        else if constexpr (op2 == 33) bin<W, 2, 2, true>(c);    // MADDAq
        else if constexpr (op2 == 34) bin<W, 0, 3, true>(c);    // ADDAi
        else if constexpr (op2 == 35) bin<W, 2, 3, true>(c);    // MADDAi
        else if constexpr (op2 == 36) bin<W, 1, 2, true>(c);    // SUBAq
        else if constexpr (op2 == 37) bin<W, 3, 2, true>(c);    // MSUBAq
        else if constexpr (op2 == 38) bin<W, 1, 3, true>(c);    // SUBAi
        else if constexpr (op2 == 39) bin<W, 3, 3, true>(c);    // MSUBAi
        else if constexpr (op2 == 40) bin<W, 0, 0, true>(c);    // ADDA
        else if constexpr (op2 == 41) bin<W, 2, 0, true>(c);    // MADDA
        else if constexpr (op2 == 42) bin<W, 6, 0, true>(c);    // MULA
        else if constexpr (op2 == 44) bin<W, 1, 0, true>(c);    // SUBA
        else if constexpr (op2 == 45) bin<W, 3, 0, true>(c);    // MSUBA
        else if constexpr (op2 == 47 || op2 == 59) {}          // NOP, WAITQ
        else if constexpr (op2 == 48) {                         // MOVE
            if constexpr (ft != 0) { float r[4]; for (int i = 0; i < 4; i++) r[i] = vf[fs][i];
                                     for (int i = 0; i < 4; i++) if (dest & (8 >> i)) vf[ft][i] = clampf(r[i]); }
        }
        else if constexpr (op2 == 49) {                         // MR32
            if constexpr (ft != 0) { float r[4] = {vf[fs][1], vf[fs][2], vf[fs][3], vf[fs][0]};
                                     for (int i = 0; i < 4; i++) if (dest & (8 >> i)) vf[ft][i] = clampf(r[i]); }
        }
        else if constexpr (op2 == 60) {                         // MTIR
            constexpr int fsf = (W >> 21) & 3;
            if constexpr (it != 0) { s32 v; std::memcpy(&v, &vf[fs][fsf], 4); c->vi[it] = (u16)v; }
        }
        else vu0_macro(c, W);
    }
}
}  // namespace vu0i
