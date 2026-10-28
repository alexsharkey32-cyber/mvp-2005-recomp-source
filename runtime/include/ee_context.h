// Emotion Engine (R5900) CPU state for recompiled code.
#pragma once
#include <cstdint>
#include <cstddef>

typedef uint8_t u8;   typedef int8_t s8;
typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32;
typedef uint64_t u64; typedef int64_t s64;

union alignas(16) GPR128 {
    u64 UD[2]; s64 SD[2];
    u32 UW[4]; s32 SW[4];
    u16 UH[8]; s16 SH[8];
    u8  UB[16]; s8 SB[16];
    float F[4];
};

union FPR { float f; u32 u; s32 s; };

struct alignas(16) EEContext {
    GPR128 r[32];
    GPR128 hi, lo;            // .UD[0] = HI/LO, .UD[1] = HI1/LO1 (pipeline 1)
    u32 sa;
    FPR f[32];
    FPR acc;
    u32 fcr31;
    bool fcc;
    // VU0 (macro mode)
    alignas(16) float vf[32][4];
    u32 vi[32];
    alignas(16) float vacc[4];
    float vq, vp;
    // COP0
    u32 cop0[32];
    u64 cycles;               // advances as code runs; drives COP0 Count
    // debug (EE_TRACE builds): guest call stack
    u32 shadow[1024];
    int shadow_n;
};

// Dispatch tables emitted by the recompiler.
typedef void (*EEFunc)(EEContext*);
typedef void (*EEFuncAt)(EEContext*, u32);
struct EEFuncEntry { u32 start, end; EEFunc fn; };
struct EEEntryLabel { u32 addr; EEFuncAt fn; };
extern const EEFuncEntry g_ee_funcs[];
extern const size_t g_ee_funcs_count;
extern const EEEntryLabel g_ee_labels[];
extern const size_t g_ee_labels_count;
extern const u32 g_ee_entry;
