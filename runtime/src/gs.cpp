#include <filesystem>
// Graphics Synthesizer: GIF packet parsing, GS registers, local memory (4 MB, swizzled exactly like
// the hardware), transfers, and a software rasterizer. This is the reference renderer; a GPU backend
// (Metal / Vulkan / OpenGL) will consume the same primitive stream later.
#include "gs.h"
#include "ee_runtime.h"
#include "settings.h"
#include "gs_gpu.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <map>
#include <set>
#include <tuple>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#ifndef _WIN32
#include <pthread.h>
#include <csignal>
#endif
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <sys/stat.h>

void hw_gs_event(u32 csr_bits);            // ee_hw.cpp: SIGNAL / FINISH -> CSR + GS interrupt
void gt_gs_event(u32 bits);                // gs_dma.cpp: forwards to the main thread when on the graphics thread
bool gt_active(); bool gt_on_graphics_thread(); void gt_push_vsync();
u64 g_gs_frame = 0;                        // game frame being processed by the graphics side
bool gs_is_replaying(); static inline bool s_replaying_fwd() { return gs_is_replaying(); }

// ---------------------------------------------------------------- local memory + swizzling
static u32 s_vram[1024 * 1024];            // 4 MB as 32-bit words
static u8* const s_vram8 = (u8*)s_vram;
static u16* const s_vram16 = (u16*)s_vram;

static const u8 kBlock32[4][8] = {{0,1,4,5,16,17,20,21},{2,3,6,7,18,19,22,23},{8,9,12,13,24,25,28,29},{10,11,14,15,26,27,30,31}};
static const u8 kBlockZ32[4][8] = {{24,25,28,29,8,9,12,13},{26,27,30,31,10,11,14,15},{16,17,20,21,0,1,4,5},{18,19,22,23,2,3,6,7}};
static const u8 kBlock16[8][4] = {{0,2,8,10},{1,3,9,11},{4,6,12,14},{5,7,13,15},{16,18,24,26},{17,19,25,27},{20,22,28,30},{21,23,29,31}};
static const u8 kBlock16S[8][4] = {{0,2,16,18},{1,3,17,19},{8,10,24,26},{9,11,25,27},{4,6,20,22},{5,7,21,23},{12,14,28,30},{13,15,29,31}};
static const u8 kBlockZ16[8][4] = {{24,26,16,18},{25,27,17,19},{28,30,20,22},{29,31,21,23},{8,10,0,2},{9,11,1,3},{12,14,4,6},{13,15,5,7}};
static const u8 kBlockZ16S[8][4] = {{24,26,8,10},{25,27,9,11},{16,18,0,2},{17,19,1,3},{28,30,12,14},{29,31,13,15},{20,22,4,6},{21,23,5,7}};
static const u8 kBlock8[4][8] = {{0,1,4,5,16,17,20,21},{2,3,6,7,18,19,22,23},{8,9,12,13,24,25,28,29},{10,11,14,15,26,27,30,31}};
static const u8 kBlock4[8][4] = {{0,2,8,10},{1,3,9,11},{4,6,12,14},{5,7,13,15},{16,18,24,26},{17,19,25,27},{20,22,28,30},{21,23,29,31}};
static const u8 kCol32[8][8] = {
    {0,1,4,5,8,9,12,13},{2,3,6,7,10,11,14,15},{16,17,20,21,24,25,28,29},{18,19,22,23,26,27,30,31},
    {32,33,36,37,40,41,44,45},{34,35,38,39,42,43,46,47},{48,49,52,53,56,57,60,61},{50,51,54,55,58,59,62,63}};
static const u8 kCol16[8][16] = {
    {0,2,8,10,16,18,24,26,1,3,9,11,17,19,25,27},{4,6,12,14,20,22,28,30,5,7,13,15,21,23,29,31},
    {32,34,40,42,48,50,56,58,33,35,41,43,49,51,57,59},{36,38,44,46,52,54,60,62,37,39,45,47,53,55,61,63},
    {64,66,72,74,80,82,88,90,65,67,73,75,81,83,89,91},{68,70,76,78,84,86,92,94,69,71,77,79,85,87,93,95},
    {96,98,104,106,112,114,120,122,97,99,105,107,113,115,121,123},{100,102,108,110,116,118,124,126,101,103,109,111,117,119,125,127}};
static const u8 kCol8[16][16] = {
    {0,4,16,20,32,36,48,52,2,6,18,22,34,38,50,54},{8,12,24,28,40,44,56,60,10,14,26,30,42,46,58,62},
    {33,37,49,53,1,5,17,21,35,39,51,55,3,7,19,23},{41,45,57,61,9,13,25,29,43,47,59,63,11,15,27,31},
    {96,100,112,116,64,68,80,84,98,102,114,118,66,70,82,86},{104,108,120,124,72,76,88,92,106,110,122,126,74,78,90,94},
    {65,69,81,85,97,101,113,117,67,71,83,87,99,103,115,119},{73,77,89,93,105,109,121,125,75,79,91,95,107,111,123,127},
    {128,132,144,148,160,164,176,180,130,134,146,150,162,166,178,182},{136,140,152,156,168,172,184,188,138,142,154,158,170,174,186,190},
    {161,165,177,181,129,133,145,149,163,167,179,183,131,135,147,151},{169,173,185,189,137,141,153,157,171,175,187,191,139,143,155,159},
    {224,228,240,244,192,196,208,212,226,230,242,246,194,198,210,214},{232,236,248,252,200,204,216,220,234,238,250,254,202,206,218,222},
    {193,197,209,213,225,229,241,245,195,199,211,215,227,231,243,247},{201,205,217,221,233,237,249,253,203,207,219,223,235,239,251,255}};
static const u16 kCol4[16][32] = {
    {0,8,32,40,64,72,96,104,2,10,34,42,66,74,98,106,4,12,36,44,68,76,100,108,6,14,38,46,70,78,102,110},
    {16,24,48,56,80,88,112,120,18,26,50,58,82,90,114,122,20,28,52,60,84,92,116,124,22,30,54,62,86,94,118,126},
    {65,73,97,105,1,9,33,41,67,75,99,107,3,11,35,43,69,77,101,109,5,13,37,45,71,79,103,111,7,15,39,47},
    {81,89,113,121,17,25,49,57,83,91,115,123,19,27,51,59,85,93,117,125,21,29,53,61,87,95,119,127,23,31,55,63},
    {192,200,224,232,128,136,160,168,194,202,226,234,130,138,162,170,196,204,228,236,132,140,164,172,198,206,230,238,134,142,166,174},
    {208,216,240,248,144,152,176,184,210,218,242,250,146,154,178,186,212,220,244,252,148,156,180,188,214,222,246,254,150,158,182,190},
    {129,137,161,169,193,201,225,233,131,139,163,171,195,203,227,235,133,141,165,173,197,205,229,237,135,143,167,175,199,207,231,239},
    {145,153,177,185,209,217,241,249,147,155,179,187,211,219,243,251,149,157,181,189,213,221,245,253,151,159,183,191,215,223,247,255},
    {256,264,288,296,320,328,352,360,258,266,290,298,322,330,354,362,260,268,292,300,324,332,356,364,262,270,294,302,326,334,358,366},
    {272,280,304,312,336,344,368,376,274,282,306,314,338,346,370,378,276,284,308,316,340,348,372,380,278,286,310,318,342,350,374,382},
    {321,329,353,361,257,265,289,297,323,331,355,363,259,267,291,299,325,333,357,365,261,269,293,301,327,335,359,367,263,271,295,303},
    {337,345,369,377,273,281,305,313,339,347,371,379,275,283,307,315,341,349,373,381,277,285,309,317,343,351,375,383,279,287,311,319},
    {448,456,480,488,384,392,416,424,450,458,482,490,386,394,418,426,452,460,484,492,388,396,420,428,454,462,486,494,390,398,422,430},
    {464,472,496,504,400,408,432,440,466,474,498,506,402,410,434,442,468,476,500,508,404,412,436,444,470,478,502,510,406,414,438,446},
    {385,393,417,425,449,457,481,489,387,395,419,427,451,459,483,491,389,397,421,429,453,461,485,493,391,399,423,431,455,463,487,495},
    {401,409,433,441,465,473,497,505,403,411,435,443,467,475,499,507,405,413,437,445,469,477,501,509,407,415,439,447,471,479,503,511}};

enum Psm { CT32 = 0x00, CT24 = 0x01, CT16 = 0x02, CT16S = 0x0a, T8 = 0x13, T4 = 0x14, T8H = 0x1b, T4HL = 0x24, T4HH = 0x2c,
           Z32 = 0x30, Z24 = 0x31, Z16 = 0x32, Z16S = 0x3a };

// bp: base in 256-byte blocks; bw: buffer width in 64-pixel units
static inline u32 addr32(u32 bp, u32 bw, u32 x, u32 y, bool z = false) {
    u32 page = (y >> 5) * bw + (x >> 6);
    u32 block = z ? kBlockZ32[(y >> 3) & 3][(x >> 3) & 7] : kBlock32[(y >> 3) & 3][(x >> 3) & 7];
    return (((bp + page * 32 + block) * 64) + kCol32[y & 7][x & 7]) & 0xfffff;
}
static inline u32 addr16(u32 bp, u32 bw, u32 x, u32 y, const u8 (*bt)[4]) {
    u32 page = (y >> 6) * bw + (x >> 6);
    u32 block = bt[(y >> 3) & 7][(x >> 4) & 3];
    return (((bp + page * 32 + block) * 128) + kCol16[y & 7][x & 15]) & 0x1fffff;
}
static inline u32 addr8(u32 bp, u32 bw, u32 x, u32 y) {
    u32 page = (y >> 6) * ((bw + 1) >> 1) + (x >> 7);
    u32 block = kBlock8[(y >> 4) & 3][(x >> 4) & 7];
    return (((bp + page * 32 + block) * 256) + kCol8[y & 15][x & 15]) & 0x3fffff;
}
static inline u32 addr4(u32 bp, u32 bw, u32 x, u32 y) {
    u32 page = (y >> 7) * ((bw + 1) >> 1) + (x >> 7);
    u32 block = kBlock4[(y >> 4) & 7][(x >> 5) & 3];
    return (((bp + page * 32 + block) * 512) + kCol4[y & 15][x & 31]) & 0x7fffff;
}

// VRAM page (8 KB) holding pixel (x, y) of a buffer
static inline u32 px_page(u32 psm, u32 bp, u32 bw, u32 x, u32 y) {
    x &= 2047; y &= 2047;
    switch (psm) {
    case CT32: case CT24: case T8H: case T4HL: case T4HH: return addr32(bp, bw, x, y) >> 11;
    case Z32: case Z24: return addr32(bp, bw, x, y, true) >> 11;
    case CT16: return addr16(bp, bw, x, y, kBlock16) >> 12;
    case CT16S: return addr16(bp, bw, x, y, kBlock16S) >> 12;
    case Z16: return addr16(bp, bw, x, y, kBlockZ16) >> 12;
    case Z16S: return addr16(bp, bw, x, y, kBlockZ16S) >> 12;
    case T8: return addr8(bp, bw, x, y) >> 13;
    default: return addr4(bp, bw, x, y) >> 14;
    }
}
// pages [p0, p0 + n) covering the rectangle (a superset: page order is monotonic in x and y)
static inline void px_range(u32 psm, u32 bp, u32 bw, u32 x0, u32 y0, u32 x1, u32 y1, u32& p0, u32& n) {
    u32 a = px_page(psm, bp, bw, x0, y0), b = px_page(psm, bp, bw, x1, y1);
    u32 c = px_page(psm, bp, bw, x1, y0), d = px_page(psm, bp, bw, x0, y1);
    u32 lo = std::min({a, b, c, d}), hi = std::max({a, b, c, d});
    p0 = lo; n = hi - lo + 1;
}
static inline u32 read_px(u32 psm, u32 bp, u32 bw, u32 x, u32 y) {
    switch (psm) {
    case CT32: case CT24: return s_vram[addr32(bp, bw, x, y)];
    case Z32: case Z24: return s_vram[addr32(bp, bw, x, y, true)];
    case CT16: return s_vram16[addr16(bp, bw, x, y, kBlock16)];
    case CT16S: return s_vram16[addr16(bp, bw, x, y, kBlock16S)];
    case Z16: return s_vram16[addr16(bp, bw, x, y, kBlockZ16)];
    case Z16S: return s_vram16[addr16(bp, bw, x, y, kBlockZ16S)];
    case T8: return s_vram8[addr8(bp, bw, x, y)];
    case T4: { u32 a = addr4(bp, bw, x, y); return (s_vram8[a >> 1] >> ((a & 1) * 4)) & 15; }
    case T8H: return s_vram[addr32(bp, bw, x, y)] >> 24;
    case T4HL: return (s_vram[addr32(bp, bw, x, y)] >> 24) & 15;
    case T4HH: return s_vram[addr32(bp, bw, x, y)] >> 28;
    }
    return 0;
}
static inline void write_px(u32 psm, u32 bp, u32 bw, u32 x, u32 y, u32 v, u32 mask = 0) {
    switch (psm) {
    case CT32: case Z32: { u32& d = s_vram[addr32(bp, bw, x, y, psm == Z32)]; d = (d & mask) | (v & ~mask); break; }
    case CT24: case Z24: { u32& d = s_vram[addr32(bp, bw, x, y, psm == Z24)]; u32 m = mask | 0xff000000u; d = (d & m) | (v & ~m); break; }
    case CT16: case CT16S: case Z16: case Z16S: {
        const u8 (*bt)[4] = psm == CT16 ? kBlock16 : psm == CT16S ? kBlock16S : psm == Z16 ? kBlockZ16 : kBlockZ16S;
        u16& d = s_vram16[addr16(bp, bw, x, y, bt)]; d = (u16)((d & mask) | (v & ~mask)); break;
    }
    case T8: s_vram8[addr8(bp, bw, x, y)] = (u8)v; break;
    case T4: { u32 a = addr4(bp, bw, x, y); u8& b = s_vram8[a >> 1]; b = (a & 1) ? (u8)((b & 0x0f) | (v << 4)) : (u8)((b & 0xf0) | (v & 15)); break; }
    case T8H: { u32& d = s_vram[addr32(bp, bw, x, y)]; d = (d & 0x00ffffffu) | (v << 24); break; }
    case T4HL: { u32& d = s_vram[addr32(bp, bw, x, y)]; d = (d & 0xf0ffffffu) | ((v & 15) << 24); break; }
    case T4HH: { u32& d = s_vram[addr32(bp, bw, x, y)]; d = (d & 0x0fffffffu) | ((v & 15) << 28); break; }
    }
}
static inline u32 c16to32(u32 c, u32 texa) {
    u32 r = (c & 0x1f) << 3, g = ((c >> 5) & 0x1f) << 3, b = ((c >> 10) & 0x1f) << 3;
    u32 aem = (texa >> 15) & 1, ta0 = texa & 0xff, ta1 = (texa >> 32) & 0xff;
    u32 a = (c & 0x8000) ? ta1 : ((aem && !(c & 0x7fff)) ? 0 : ta0);
    return r | (g << 8) | (b << 16) | (a << 24);
}
static inline u32 c32to16(u32 c) {
    return ((c >> 3) & 0x1f) | (((c >> 11) & 0x1f) << 5) | (((c >> 19) & 0x1f) << 10) | ((c >> 31) << 15);
}

// ---------------------------------------------------------------- registers
struct Ctx { u64 tex0 = 0, tex1 = 0, clamp = 0, xyoffset = 0, scissor = 0, alpha = 0, test = 0, fba = 0, frame = 0, zbuf = 0; };
// Which draws a skipped frame may drop: see vertex_kick. Per FBP: when it was last a display buffer, and for how
// many frames in a row the game has drawn into it (shadow maps etc. are redrawn every frame).
// MVP_VWATCH=lo-hi (block addresses, hex): log uploads and draws that write into that part of VRAM
static u32 s_vwatch_lo = ~0u, s_vwatch_hi = 0;
static long long s_rec_request = -1;                    // MVP_GSREC_DOF: frame to record, asked for by vertex_kick
static u64 s_vwatch_until = 0;                      // MVP_VWATCH_CUT=n: only for n frames after a copy of the picture into texture memory
static const long long s_vwatch_cut = std::getenv("MVP_VWATCH_CUT") ? std::atoll(std::getenv("MVP_VWATCH_CUT")) : -1;
static const bool s_vwatch_init = [] { if (const char* e = std::getenv("MVP_VWATCH")) sscanf(e, "%x-%x", &s_vwatch_lo, &s_vwatch_hi); return true; }();
static u64 s_disp_seen[512], s_tgt_last[512];
static u16 s_tgt_streak[512];
static u64 s_rtt_in_skipped = 0, s_rtt_by_fbp[512];
static struct GsRegs {
    u64 prim = 0, rgbaq = 0x3f80000000000000ull, st = 0, uv = 0, fog = 0, prmodecont = 1, prmode = 0, texclut = 0,
        texa = 0, fogcol = 0, dimx = 0, dthe = 0, colclamp = 1, pabe = 0, bitbltbuf = 0, trxpos = 0, trxreg = 0, trxdir = 3,
        signal = 0, label = 0;
    Ctx c[2];
    float q = 1.0f;
} R;

static u64 s_priv[0x100];                    // privileged registers (index = (addr >> 4) & 0xff)
static u64 s_prims = 0, s_pixels = 0, s_frames = 0;
static std::map<std::tuple<u32,u32,u32,u32>, u64> s_targets;   // (fbp, fbw, psm, tme) -> primitives

// ---------------------------------------------------------------- vertices / primitives
struct Vtx { float x, y, z; float r, g, b, a; float s, t, q; float u, v; float f; };
static Vtx s_vq[3];
static int s_vcount = 0;                      // vertices queued for the current primitive
static int s_vtotal = 0;                      // vertices since PRIM write (strip/fan)

static inline u32 bits(u64 v, int lo, int n) { return (u32)((v >> lo) & ((1ull << n) - 1)); }

struct DrawState {
    const Ctx* c; u32 prim_attr;
    bool tme, fst, abe, fge, iip;
    u32 fbp, fbw, fpsm, fbmsk, zbp, zpsm, zmsk;
    int sx0, sx1, sy0, sy1;
    u32 test, alpha, fba;
    // texture
    u32 tbp, tbw, tpsm, tw, th, tcc, tfx, cbp, cpsm, csm, csa;
    u32 wms, wmt, minu, maxu, minv, maxv;
    const u32* clut;                     // decoded palette (32-bit colours) for T8/T4 textures
    u64 texa; u32 fogcol, colclamp;      // snapshot of global registers (rasterizer threads never read R)
    bool bilinear;                       // TEX1.MMAG = linear (and the user allows filtering)
    bool hr;                             // this copy of the primitive draws into the high-resolution main target
    u32 clut_serial = 0;                 // identity of the decoded palette (texture cache key)
    std::shared_ptr<const struct TexData> tex;   // decoded texture (cache), null = sample VRAM directly
    std::shared_ptr<const struct HiData> hi;     // enhanced / replacement version of tex, if ready
};

// Changes whenever something a prepared draw state was derived from may have changed: page generations or the
// "drawn" marks the texture cache looks at (except drawing again into a page already marked drawn, which changes
// nothing for it), decoded palettes, the texture cache itself. See vertex_kick.
static u64 s_prep_epoch = 1;
static u64 s_prep_tris = 0, s_prep_n[3] = {0, 0, 0};
static bool s_state_dirty = true;            // a register setup() reads was written since vertex_kick last prepared a state
static void setup(DrawState& d) {
    u32 attr = (R.prmodecont & 1) ? (u32)R.prim : (u32)R.prmode;
    d.prim_attr = attr;
    d.iip = attr & 8; d.tme = attr & 16; d.fge = attr & 32; d.abe = attr & 64; d.fst = attr & 256;
    const Ctx& c = R.c[(attr >> 9) & 1];
    d.c = &c;
    d.fbp = bits(c.frame, 0, 9) * 32; d.fbw = bits(c.frame, 16, 6); d.fpsm = bits(c.frame, 24, 6); d.fbmsk = (u32)(c.frame >> 32);
    d.zbp = bits(c.zbuf, 0, 9) * 32; d.zpsm = bits(c.zbuf, 24, 4) | 0x30; d.zmsk = bits(c.zbuf, 32, 1);
    d.sx0 = bits(c.scissor, 0, 11); d.sx1 = bits(c.scissor, 16, 11); d.sy0 = bits(c.scissor, 32, 11); d.sy1 = bits(c.scissor, 48, 11);
    d.test = (u32)c.test; d.alpha = (u32)c.alpha | ((u32)((c.alpha >> 32) & 0xff) << 8 & 0) ; d.fba = (u32)c.fba & 1;
    d.alpha = (u32)(c.alpha & 0xff) | (bits(c.alpha, 32, 8) << 8);
    u64 t = c.tex0;
    d.tbp = bits(t, 0, 14); d.tbw = bits(t, 14, 6); d.tpsm = bits(t, 20, 6); d.tw = bits(t, 26, 4); d.th = bits(t, 30, 4);
    d.tcc = bits(t, 34, 1); d.tfx = bits(t, 35, 2); d.cbp = bits(t, 37, 14); d.cpsm = bits(t, 51, 4); d.csm = bits(t, 55, 1); d.csa = bits(t, 56, 5);
    d.wms = bits(c.clamp, 0, 2); d.wmt = bits(c.clamp, 2, 2); d.minu = bits(c.clamp, 4, 10); d.maxu = bits(c.clamp, 14, 10);
    d.minv = bits(c.clamp, 24, 10); d.maxv = bits(c.clamp, 34, 10);
    d.clut = nullptr;
    d.texa = R.texa; d.fogcol = (u32)R.fogcol; d.colclamp = (u32)R.colclamp;
    d.bilinear = g_settings.tex_filter && ((c.tex1 >> 5) & 1);
    d.hr = false;
}

static u32 clut_lookup(const DrawState& d, u32 idx) {
    u32 x, y;
    if (d.tpsm == T8 || d.tpsm == T8H) {
        u32 i = (idx & 0xe7) | ((idx & 0x08) << 1) | ((idx & 0x10) >> 1);      // CSM1 swizzle
        x = i & 15; y = i >> 4;
    } else { u32 i = idx + d.csa * 16; x = i & 7; y = (i >> 3) & 1; if (d.cpsm == CT32) y += (d.csa & 15) >> 1, x = idx & 7, y = ((d.csa & 15) * 2 + (idx >> 3)) & 31; }
    if (d.cpsm == CT32 || d.cpsm == CT24) return s_vram[addr32(d.cbp, 1, x, y)];
    return c16to32(s_vram16[addr16(d.cbp, 1, x, y, d.cpsm == CT16S ? kBlock16S : kBlock16)], R.texa);
}

// Decoded-palette cache: palettes are expanded once (VRAM swizzle + 16->32 bit conversion) instead of
// per pixel. Entries are keyed by everything clut_lookup depends on and dropped on any VRAM upload.
struct ClutEntry { u64 key = ~0ull; u64 texa = 0; u32 epoch = 0; bool valid = false; u32 page = 0; u32 serial = 0; u32 col[256]; };
static u32 s_clut_serial = 0;
static ClutEntry s_clut_cache[64];
static u32 s_clut_next = 0;
void gs_sync(); static bool gs_queue_busy(); static void gs_sync_reason(int r);
static inline void own_check_read(u32 p0, u32 np, const char* why, bool top);
static u32 gs_epoch(); static bool gs_pages_busy(u32 bp, u32 bw, u32 rows, bool writes_only);
static const u32* clut_get(DrawState& d) {
    bool t8 = d.tpsm == T8 || d.tpsm == T8H;
    u64 key = (u64)d.cbp | ((u64)d.cpsm << 14) | ((u64)d.csa << 18) | ((u64)t8 << 23);
    u64 texa = (d.cpsm == CT32 || d.cpsm == CT24) ? 0 : R.texa;
    static u32 mru = 0;                                   // (an entry is 1 KB: look at the last one used first)
    { ClutEntry& e = s_clut_cache[mru]; if (e.valid && e.key == key && e.texa == texa) { e.epoch = gs_epoch(); d.clut_serial = e.serial; return e.col; } }
    for (auto& e : s_clut_cache) if (e.valid && e.key == key && e.texa == texa) { mru = (u32)(&e - s_clut_cache); e.epoch = gs_epoch(); d.clut_serial = e.serial; return e.col; }
    own_check_read(d.cbp >> 5, ((d.cbp + 3) >> 5) - (d.cbp >> 5) + 1, "palette", false);   // 256 x 32-bit = 4 blocks
    ClutEntry& e = s_clut_cache[s_clut_next++ & 63];
    s_prep_epoch++;                                       // (a slot is being given new colours)
    // queued primitives may still point at this slot, or the palette may still be being drawn
    if (gs_queue_busy() && (e.epoch == gs_epoch() || gs_pages_busy(d.cbp, 1, 2, true))) { gs_sync_reason(1); gs_sync(); }
    e.key = key; e.texa = texa; e.valid = true; e.page = d.cbp >> 5; e.epoch = gs_epoch(); e.serial = ++s_clut_serial; d.clut_serial = e.serial;
    int n = t8 ? 256 : 16;
    for (int i = 0; i < n; i++) e.col[i] = clut_lookup(d, (u32)i);
    u64 h = 1469598103934665603ull;                     // content identity (same colours = same textures)
    for (int i = 0; i < n; i++) h = (h ^ e.col[i]) * 0x100000001b3ull;
    e.serial = (u32)(h ^ (h >> 32)); d.clut_serial = e.serial;
    return e.col;
}
static void clut_invalidate_pages(u32 p0, u32 n) {        // an upload wrote pages [p0, p0+n)
    for (auto& e : s_clut_cache) if (e.valid && ((e.page - p0) & 511) < n) e.valid = false;
    s_prep_epoch++;
}

static inline int wrap(int c, int size, u32 mode, u32 mn, u32 mx) {
    switch (mode) {
        case 0: return c & (size - 1);
        case 1: return c < 0 ? 0 : c >= size ? size - 1 : c;
        case 2: return c < (int)mn ? (int)mn : c > (int)mx ? (int)mx : c;
        default: return (int)((c & mn) | mx);
    }
}

static inline u32 tex_fetch(const DrawState& d, int u, int v) {
    int w = 1 << d.tw, h = 1 << d.th;
    u = wrap(u, w, d.wms, d.minu, d.maxu);
    v = wrap(v, h, d.wmt, d.minv, d.maxv);
    u32 raw = read_px(d.tpsm, d.tbp, d.tbw, (u32)u, (u32)v);
    switch (d.tpsm) {
        case CT32: return raw;
        case CT24: { u32 aem = (d.texa >> 15) & 1; u32 a = (aem && !(raw & 0xffffff)) ? 0 : (u32)(d.texa & 0xff); return (raw & 0xffffff) | (a << 24); }
        case CT16: case CT16S: return c16to32(raw, d.texa);
        default: return d.clut ? d.clut[raw & 255] : clut_lookup(d, raw);
    }
}

// High-resolution main target: the game's 3D/UI target (the texture of the final full-screen copy to the
// display buffer) is additionally rasterized at s_hr_scale x into linear colour/Z buffers, and the window
// shows that instead of the native 640x448 output. Everything else (RTT, VRAM, readbacks) stays native.
static int s_hr_scale = 1, s_hr_w = 0, s_hr_h = 0;
static std::vector<u32> s_hr_col, s_hr_z;
static u32 s_hr_tbp = ~0u, s_hr_tbw = 0;            // main target (blocks, 64-pixel units); ~0 = not yet known
static bool s_hr_valid = false;                     // last displayed frame came from the main target
static float s_hr_u0 = 0, s_hr_v0 = 0, s_hr_u1 = 0, s_hr_v1 = 0;   // source rect of the final copy (native pixels)

static inline u32 lerp_c(u32 a, u32 b, u32 f) {    // per-channel a + (b-a)*f/256
    u32 r = 0;
    for (int k = 0; k < 32; k += 8) {
        int ca = (a >> k) & 0xff, cb = (b >> k) & 0xff;
        r |= (u32)((ca + (((cb - ca) * (int)f) >> 8)) & 0xff) << k;
    }
    return r;
}
static inline u32 tex_bilinear(const DrawState& d, float tu, float tv) {
    tu -= 0.5f; tv -= 0.5f;
    float fu = std::floor(tu), fv = std::floor(tv);
    int iu = (int)fu, iv = (int)fv;
    u32 wu = (u32)((tu - fu) * 256.0f), wv = (u32)((tv - fv) * 256.0f);
    u32 c00 = tex_fetch(d, iu, iv), c10 = tex_fetch(d, iu + 1, iv), c01 = tex_fetch(d, iu, iv + 1), c11 = tex_fetch(d, iu + 1, iv + 1);
    return lerp_c(lerp_c(c00, c10, wu), lerp_c(c01, c11, wu), wv);
}
// ---------------------------------------------------------------- texture cache, HD textures
// Textures uploaded to VRAM are decoded once into linear RGBA (keyed by TEX0, palette and VRAM page
// generations), which also makes sampling much cheaper. Each decoded texture is identified by a 64-bit
// hash of its pixels; that hash names dumps (textures/dump/WxH_hash.png) and replacements
// (textures/replace/WxH_hash.png, any size, e.g. 4x). Without a replacement the built-in enhancer can
// upscale it 2x/4x. Textures that the game renders into (render-to-texture) are never cached.
struct HiData { int w = 0, h = 0; float sx = 1, sy = 1; bool replacement = false; std::vector<u32> px; };
struct TexData {
    int w = 0, h = 0; u64 hash = 0; std::vector<u32> px;
    std::shared_ptr<const HiData> hi;          // published by the enhancer thread (atomic_load/atomic_store)
    std::atomic<bool> queued{false};
    u64 last_use = 0;
};
struct TexKeyEntry { u64 gensum = 0; std::shared_ptr<TexData> data; u64 last_use = 0; };
static u64 s_page_gen[512];
static bool s_page_drawn[512];                    // page last written by drawing (render target) instead of upload
static u64 s_gen_counter = 1;
static std::unordered_map<u64, TexKeyEntry> s_tex_by_key;
static std::unordered_map<u64, std::shared_ptr<TexData>> s_tex_by_hash;
static std::unordered_set<u64> s_replace_names, s_dumped;
static std::unordered_map<u64, std::string> s_replace_path;       // replacements from mods (mods/x/textures/replace)
static std::vector<std::string> s_replace_dirs;
void gs_add_replace_dir(const char* dir) { s_replace_dirs.push_back(dir); }
static bool s_tex_scanned = false;
static size_t s_tex_bytes = 0;
static u64 s_tex_hits = 0, s_tex_misses = 0, s_tex_decodes = 0, s_tex_miss_new = 0, s_tex_miss_gen = 0, s_up_bumps = 0, s_up_same = 0;
static const bool s_texcache_off = std::getenv("MVP_TEXCACHE") && std::getenv("MVP_TEXCACHE")[0] == '0';
static bool s_texcache_force = std::getenv("MVP_TEXCACHE") && std::getenv("MVP_TEXCACHE")[0] == '1';
// ".once PRIMS dir": every textured triangle of the next frame (screen position, depth, colour, texture coordinates)
// goes to dir/prims.txt and each texture once to dir/<hash>.png: the portrait renderer (tools/portraits/render.py)
// draws the head again from them with its own textures, lighting and resolution.
static long long s_prims_frame = -1; static std::string s_prims_dir; static FILE* s_prims_f = nullptr; static std::unordered_set<u64> s_prims_tex;
void gs_prims_request(const std::string& dir) { extern u64 g_gs_frame; s_prims_dir = dir; s_prims_frame = (long long)g_gs_frame + 2; s_texcache_force = true; }

static void upext_pages_written(u32 p0, u32 n);
static inline void pages_bump(u32 p0, u32 n, bool drawn) {
    if (n > 512) n = 512;
    if (drawn) upext_pages_written(p0 & 511, n);
    for (u32 k = 0; k < n; k++) { u32 p = (p0 + k) & 511; s_page_gen[p] = ++s_gen_counter; s_page_drawn[p] = drawn; }
    s_prep_epoch++;
}
static inline u32 tc_page_w(u32 psm) { return (psm == T8 || psm == T4) ? 128 : 64; }
static inline u32 tc_page_h(u32 psm) {
    switch (psm) { case CT32: case CT24: case Z32: case Z24: case T8H: case T4HL: case T4HH: return 32;
                   case T4: return 128; default: return 64; }
}
// ---- how much of a texture was really uploaded
// The menus' artwork and fonts are not power-of-two sized: a 488x104 image is uploaded and then used as a 512x128
// texture. The rest of that texture is whatever lies behind it in memory (parts of other textures, changing from
// screen to screen), so the hash of "the texture" changed with it: the same font was dumped under hundreds of names
// and a pack replaced it only when the leftovers happened to match. The size of the upload at a texture's address
// is remembered here and the texels outside it are taken as empty (0) when the texture is decoded, hashed and dumped.
// A record ends when another upload overwrites the first block of the texture (block-exact: neighbours share pages).
struct UpExt { u16 lw = 0, lh = 0, mw = 0, mh = 0; u8 bw = 0, psm = 0; bool live = false; };
static UpExt s_upext[16384];                        // by block address
static std::vector<u16> s_upext_live;
static u16 s_upext_in_page[512];                   // live records starting in each page (drawing checks this per primitive)
static const bool s_texmask = std::getenv("MVP_NO_TEXMASK") == nullptr;
static inline u32 upext_block(u32 bp, u32 bw, u32 psm, u32 x, u32 y) {
    switch (psm) {
    case T8: return (addr8(bp, bw, x, y) >> 8) & 16383;
    case T4: return (addr4(bp, bw, x, y) >> 9) & 16383;
    case CT16: return (addr16(bp, bw, x, y, kBlock16) >> 7) & 16383;
    case CT16S: return (addr16(bp, bw, x, y, kBlock16S) >> 7) & 16383;
    default: return (addr32(bp, bw, x, y) >> 6) & 16383;
    }
}
static inline bool upext_same_format(u32 a, u32 b) { return a == b || ((a == CT32 || a == CT24) && (b == CT32 || b == CT24)); }
static void upext_clear() { for (u16 b : s_upext_live) s_upext[b].live = false; s_upext_live.clear(); std::memset(s_upext_in_page, 0, sizeof s_upext_in_page); }
static void upext_upload(u32 dbp, u32 bw, u32 psm, u32 dx, u32 dy, u32 w, u32 h) {
    if (!s_texmask || !w || !h || w > 2048 || h > 2048) return;
    dbp &= 16383;
    if (!s_upext_live.empty()) {                    // records whose first block this upload writes are over
        static u64 hit[256];
        std::memset(hit, 0, sizeof hit);
        const u32 sx = (psm == T4) ? 32 : (psm == T8 || psm == CT16 || psm == CT16S) ? 16 : 8, sy = (psm == T8 || psm == T4) ? 16 : 8;
        for (u32 y = dy & ~(sy - 1); y < dy + h; y += sy) for (u32 x = dx & ~(sx - 1); x < dx + w; x += sx) { u32 b = upext_block(dbp, bw, psm, x, y); hit[b >> 6] |= 1ull << (b & 63); }
        size_t n = 0;
        for (u16 b : s_upext_live) {
            if (!s_upext[b].live) continue;
            if (b != dbp && (hit[b >> 6] >> (b & 63) & 1)) { s_upext[b].live = false; s_upext_in_page[b >> 5]--; continue; }
            s_upext_live[n++] = b;
        }
        s_upext_live.resize(n);
    }
    UpExt& e = s_upext[dbp];
    const bool same = e.live && e.bw == bw && e.psm == psm;
    if (dx == 0 && dy == 0) {                       // a new image at this address (or the first part of one)
        if (!e.live) { s_upext_live.push_back((u16)dbp); s_upext_in_page[dbp >> 5]++; }
        if (!same) { e.mw = 0; e.mh = 0; }
        e.lw = (u16)w; e.lh = (u16)h; e.mw = std::max<u16>(e.mw, (u16)w); e.mh = std::max<u16>(e.mh, (u16)h);
        e.bw = (u8)bw; e.psm = (u8)psm; e.live = true;
    } else if (same) {                              // further parts
        e.lw = std::max<u16>(e.lw, (u16)(dx + w)); e.lh = std::max<u16>(e.lh, (u16)(dy + h));
        e.mw = std::max<u16>(e.mw, e.lw); e.mh = std::max<u16>(e.mh, e.lh);
    }
}
static void upext_pages_written(u32 p0, u32 n) {    // drawn into or copied over: page-exact is enough here
    bool any = false;
    for (u32 i = 0; i < n && !any; i++) any = s_upext_in_page[(p0 + i) & 511] != 0;
    if (!any) return;
    size_t k = 0;
    for (u16 b : s_upext_live) {
        if (!s_upext[b].live) continue;
        if ((((u32)(b >> 5) - p0) & 511) < n) { s_upext[b].live = false; s_upext_in_page[b >> 5]--; continue; }
        s_upext_live[k++] = b;
    }
    s_upext_live.resize(k);
}
static inline u32 pow2_ceil(u32 v) { u32 p = 1; while (p < v) p <<= 1; return p; }
// The part of a w x h texture at tbp that holds image data, if it is known to be smaller than the texture.
static bool upext_rect(u32 tbp, u32 tbw, u32 tpsm, u32 w, u32 h, u32& mw, u32& mh) {
    const UpExt& e = s_upext[tbp & 16383];
    if (!e.live || e.bw != tbw || !upext_same_format(e.psm, tpsm)) return false;
    // the latest image if the texture is as big as that image needs; otherwise everything ever uploaded here
    // (a texture updated in parts keeps its full size)
    u32 ew = e.lw, eh = e.lh;
    if (pow2_ceil(ew) != w || pow2_ceil(eh) != h) { ew = e.mw; eh = e.mh; }
    if (ew >= w && eh >= h) return false;
    if (eh < h) {                                   // an image of the same format right below: probably the next part of this one
        const u32 b = upext_block(tbp, tbw, tpsm, 0, eh); const UpExt& c = s_upext[b];
        if (b != (tbp & 16383) && c.live && c.bw == e.bw && c.psm == e.psm) return false;
    }
    mw = std::min(ew, w); mh = std::min(eh, h);
    return true;
}
static inline void upext_mask(std::vector<u32>& px, u32 w, u32 h, u32 mw, u32 mh) {
    for (u32 y = 0; y < h; y++) {
        u32* row = &px[(size_t)y * w];
        if (y >= mh) std::memset(row, 0, (size_t)w * 4); else if (mw < w) std::memset(row + mw, 0, (size_t)(w - mw) * 4);
    }
}
static inline u32 texel_raw(const DrawState& d, u32 u, u32 v) {   // decode one texel, no wrapping
    u32 raw = read_px(d.tpsm, d.tbp, d.tbw, u, v);
    switch (d.tpsm) {
        case CT32: return raw;
        case CT24: { u32 aem = (d.texa >> 15) & 1; u32 a = (aem && !(raw & 0xffffff)) ? 0 : (u32)(d.texa & 0xff); return (raw & 0xffffff) | (a << 24); }
        case CT16: case CT16S: return c16to32(raw, d.texa);
        default: return d.clut ? d.clut[raw & 255] : clut_lookup(d, raw);
    }
}
// Font pages that fill the space between their glyph cells with a see-through colour (palette entry 0, also the
// first texel): at the console's resolution that filler only tints the outermost pixels of a letter, at a higher
// internal resolution - and after upscaling - it becomes a thin box around every letter. Finds the filler (long
// bars and lines of that one colour, with upright lines between the cells) and makes it fully transparent.
// The same test as tools/hd/hdpack.py gutter(). Used for what is sampled, never for the hash or the dump.
static const bool s_texgutter = !std::getenv("MVP_NO_TEXGUTTER");
static bool tex_gutter(std::vector<u32>& px, int w, int h) {
    if (!s_texgutter || w < 128 || h < 128) return false;
    const u32 v = px[0], va = v >> 24;
    if (va == 0 || va >= 0x80) return false;
    for (int x = 0; x < w; x++) if (px[x] != v) return false;
    const size_t n = (size_t)w * h;
    size_t ng = 0, clear = 0;
    for (size_t i = 0; i < n; i++) { if (px[i] == v) ng++; else if (!(px[i] >> 24)) clear++; }
    if (ng * 100 < n * 8 || ng * 100 > n * 75 || clear * 4 < n - ng) return false;
    std::vector<u8> m(n, 0);                             // 1: in a row run >= 8, 2: in a column run >= 8, 4: in a row run >= 4
    for (int y = 0; y < h; y++) for (int x = 0; x < w;) {
        if (px[(size_t)y * w + x] != v) { x++; continue; }
        int x1 = x; while (x1 < w && px[(size_t)y * w + x1] == v) x1++;
        if (x1 - x >= 4) for (int k = x; k < x1; k++) m[(size_t)y * w + k] |= (x1 - x >= 8) ? 5 : 4;
        x = x1;
    }
    for (int x = 0; x < w; x++) for (int y = 0; y < h;) {
        if (px[(size_t)y * w + x] != v) { y++; continue; }
        int y1 = y; while (y1 < h && px[(size_t)y1 * w + x] == v) y1++;
        if (y1 - y >= 8) for (int k = y; k < y1; k++) m[(size_t)k * w + x] |= 2;
        y = y1;
    }
    size_t fill = 0; int cols = 0;
    for (size_t i = 0; i < n; i++) if (m[i] & 3) fill++;
    for (int x = 0; x < w; x++) { int c = 0; for (int y = 0; y < h; y++) { u8 k = m[(size_t)y * w + x]; if ((k & 2) && !(k & 4)) c++; } if (c >= 8) cols++; }
    if (fill * 4 < ng * 3 || cols < 6) return false;
    for (size_t i = 0; i < n; i++) if (m[i] & 3) px[i] = 0;
    return true;
}
static inline u64 mix64(u64 h, u64 v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); return h * 0xff51afd7ed558ccdull; }

// ---- enhancer / replacement loader thread
static std::mutex s_enh_mx; static std::condition_variable s_enh_cv;
static std::deque<std::shared_ptr<TexData>> s_enh_q;
static std::atomic<u64> s_enh_done{0};
// Alpha in the PNG files (dumps and replacements): 255 = the GS's 0x80, "opaque". A few textures use the range above
// that - the scoreboard and name-plate fonts go up to 0xff and are drawn with a vertex alpha of 0x40 - and doubling
// would clip their upper half (and a replacement would come out half as opaque as the original). For those the
// PNG holds the GS value as it is. Lettering whose palette merely overshoots (0x84) counts as normal.
static bool tex_wide_alpha(const std::vector<u32>& px) {
    u32 m = 0;
    for (u32 c : px) m = std::max(m, c >> 24);
    return m > 0xa0;
}
static std::string tex_name(int w, int h, u64 hash) {
    char b[64]; snprintf(b, sizeof b, "%dx%d_%016llx.png", w, h, (unsigned long long)hash); return b;
}
static inline float cubic_w(float x) {          // Catmull-Rom
    x = std::fabs(x);
    if (x < 1) return 1.5f * x * x * x - 2.5f * x * x + 1;
    if (x < 2) return -0.5f * x * x * x + 2.5f * x * x - 4 * x + 2;
    return 0;
}
// Built-in enhancement: separable Catmull-Rom upscale (colour weighted by alpha so transparent texels
// don't bleed dark fringes) followed by a light unsharp mask.
static std::shared_ptr<HiData> enhance(const TexData& t, int k) {
    auto hd = std::make_shared<HiData>();
    const int W = t.w * k, H = t.h * k;
    hd->w = W; hd->h = H; hd->sx = hd->sy = (float)k;
    // premultiply by (alpha+1) so colour interpolation ignores transparent texels
    std::vector<float> src((size_t)t.w * t.h * 4);
    for (size_t i = 0; i < (size_t)t.w * t.h; i++) {
        u32 c = t.px[i]; float a = (float)(c >> 24), m = a + 1.0f;
        src[i * 4] = (c & 0xff) * m; src[i * 4 + 1] = ((c >> 8) & 0xff) * m; src[i * 4 + 2] = ((c >> 16) & 0xff) * m; src[i * 4 + 3] = a;
    }
    float wk[16][4];                                   // weights for the k output phases
    for (int ph = 0; ph < k; ph++) { float f = (ph + 0.5f) / k - 0.5f; float fl = std::floor(f); f -= fl;
                                     for (int i = 0; i < 4; i++) wk[ph][i] = cubic_w(f - (i - 1)); }
    auto phase_base = [&](int o) { float f = (o + 0.5f) / k - 0.5f; return (int)std::floor(f); };
    std::vector<float> hor((size_t)W * t.h * 4);        // horizontal pass
    for (int y = 0; y < t.h; y++) for (int x = 0; x < W; x++) {
        int b0 = phase_base(x); const float* w = wk[x % k];
        float acc[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4; i++) { int sx = std::min(std::max(b0 + i - 1, 0), t.w - 1); const float* p = &src[((size_t)y * t.w + sx) * 4];
                                      for (int c = 0; c < 4; c++) acc[c] += w[i] * p[c]; }
        std::memcpy(&hor[((size_t)y * W + x) * 4], acc, 16);
    }
    std::vector<float> out((size_t)W * H * 4);          // vertical pass + un-premultiply
    for (int y = 0; y < H; y++) {
        int b0 = phase_base(y); const float* w = wk[y % k];
        for (int x = 0; x < W; x++) {
            float acc[4] = {0, 0, 0, 0};
            for (int i = 0; i < 4; i++) { int sy = std::min(std::max(b0 + i - 1, 0), t.h - 1); const float* p = &hor[((size_t)sy * W + x) * 4];
                                          for (int c = 0; c < 4; c++) acc[c] += w[i] * p[c]; }
            float m = std::max(acc[3], 0.0f) + 1.0f;
            float* o = &out[((size_t)y * W + x) * 4];
            o[0] = acc[0] / m; o[1] = acc[1] / m; o[2] = acc[2] / m; o[3] = acc[3];
        }
    }
    hd->px.resize((size_t)W * H);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {   // unsharp mask (cross-shaped blur)
        const float* c = &out[((size_t)y * W + x) * 4];
        const float* l = &out[((size_t)y * W + std::max(x - 1, 0)) * 4];
        const float* r = &out[((size_t)y * W + std::min(x + 1, W - 1)) * 4];
        const float* u = &out[((size_t)std::max(y - 1, 0) * W + x) * 4];
        const float* d = &out[((size_t)std::min(y + 1, H - 1) * W + x) * 4];
        u32 px = 0;
        for (int ch = 0; ch < 3; ch++) {
            float blur = (c[ch] * 2 + l[ch] + r[ch] + u[ch] + d[ch]) / 6.0f;
            float v = c[ch] + 0.4f * (c[ch] - blur);
            px |= (u32)(v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5f)) << (8 * ch);
        }
        float a = c[3]; px |= (u32)(a < 0 ? 0 : a > 255 ? 255 : (int)(a + 0.5f)) << 24;
        hd->px[(size_t)y * W + x] = px;
    }
    return hd;
}
static std::atomic<int> s_enh_inflight{0};
static std::atomic<size_t> s_enh_bytes{0};
bool png_load(const std::string& path, std::vector<u32>& rgba, int& w, int& h);
static void enh_main() {
    for (;;) {
        std::shared_ptr<TexData> t;
        {
            std::unique_lock<std::mutex> lk(s_enh_mx);
            s_enh_cv.wait(lk, [] { return !s_enh_q.empty(); });
            t = s_enh_q.front(); s_enh_q.pop_front();
            s_enh_inflight++;
        }
        std::shared_ptr<HiData> hd;
        std::string name = tex_name(t->w, t->h, t->hash);
        if (g_settings.tex_replace && s_replace_names.count(t->hash)) {   // artist-made replacement
            std::vector<u32> px; int w, h;
            auto rp = s_replace_path.find(t->hash);
            if (png_load(rp != s_replace_path.end() ? rp->second : "textures/replace/" + name, px, w, h)) {
                hd = std::make_shared<HiData>();
                hd->w = w; hd->h = h; hd->sx = (float)w / t->w; hd->sy = (float)h / t->h; hd->replacement = true;
                if (!tex_wide_alpha(t->px))
                    for (u32& c : px) c = (c & 0xffffff) | ((((c >> 24) + 1) >> 1) << 24);   // PNG alpha 0-255 -> PS2 0-128
                hd->px.swap(px);
            } else rlog("tex", "could not load textures/replace/%s", name.c_str());
        }
        if (!hd && g_settings.tex_enhance > 0 && t->w >= 16 && t->h >= 16) {
            // 4x only up to 256x256 sources, 2x up to 512x512; stop at ~1 GB of enhanced textures
            int k = g_settings.tex_enhance >= 2 && t->w <= 256 && t->h <= 256 ? 4 : 2;
            if (t->w <= 512 && t->h <= 512 && s_enh_bytes.load() < ((size_t)1 << 30)) hd = enhance(*t, k);
        }
        if (hd && std::getenv("MVP_DUMP_HI")) {
            std::vector<u32> a(t->px), b(hd->px);
            for (u32& c : a) c |= 0xff000000u; for (u32& c : b) c |= 0xff000000u;
            extern bool gs_write_png(const char*, const std::vector<u32>&, int, int);
            gs_write_png(("/tmp/hi/" + name + ".orig.png").c_str(), a, t->w, t->h);
            gs_write_png(("/tmp/hi/" + name + ".hi.png").c_str(), b, hd->w, hd->h);
        }
        if (hd) { s_enh_bytes += hd->px.size() * 4; std::atomic_store(&t->hi, std::shared_ptr<const HiData>(hd)); }
        s_enh_done++;
        s_enh_inflight--;
    }
}
static void enh_queue(const std::shared_ptr<TexData>& t) {
    static bool started = false;
    if (!started) {
        started = true;
#ifndef _WIN32
        sigset_t all, old; sigfillset(&all); pthread_sigmask(SIG_BLOCK, &all, &old);
#endif
        int n = std::max(1, (int)std::thread::hardware_concurrency() / 2);
        for (int i = 0; i < n; i++) std::thread(enh_main).detach();
#ifndef _WIN32
        pthread_sigmask(SIG_SETMASK, &old, nullptr);
#endif
    }
    if (t->queued.exchange(true)) return;
    std::lock_guard<std::mutex> lk(s_enh_mx);
    s_enh_q.push_back(t);
    s_enh_cv.notify_one();
}
size_t gs_replacement_count() { return s_replace_names.size(); }
static void tex_scan_replacements() {
    s_tex_scanned = true;
    // textures/replace/index is not required: we probe names lazily through a directory listing
    extern void list_dir(const char* dir, std::vector<std::string>& out);
    std::vector<std::string> files;
    list_dir("textures/hd", files);                                  // the HD texture pack (installed from Features)
    size_t nhd = 0;
    for (auto& f : files) {
        size_t us = f.find('_');
        if (us == std::string::npos || f.size() < us + 17) continue;
        u64 h = std::strtoull(f.substr(us + 1, 16).c_str(), nullptr, 16);
        s_replace_names.insert(h); s_replace_path[h] = "textures/hd/" + f; nhd++;
    }
    if (nhd) rlog("tex", "%zu textures in the HD pack (textures/hd)", nhd);
    files.clear(); list_dir("textures/replace", files);              // your own: they win over the pack
    for (auto& f : files) {
        size_t us = f.find('_');
        if (us == std::string::npos || f.size() < us + 17) continue;
        u64 h = std::strtoull(f.substr(us + 1, 16).c_str(), nullptr, 16);
        s_replace_names.insert(h); s_replace_path.erase(h);
    }
    for (const std::string& d : s_replace_dirs) {                    // mods' own: they win over textures/replace
        files.clear(); list_dir(d.c_str(), files); size_t k = 0;
        for (auto& f : files) {
            size_t us = f.find('_');
            if (us == std::string::npos || f.size() < us + 17) continue;
            u64 h = std::strtoull(f.substr(us + 1, 16).c_str(), nullptr, 16);
            s_replace_names.insert(h); s_replace_path[h] = d + "/" + f; k++;
        }
        if (k) rlog("tex", "%zu replacement textures in %s", k, d.c_str());
    }
    if (!s_replace_names.empty()) rlog("tex", "%zu replacement textures in all", s_replace_names.size());
}
static void tex_dump(const TexData& t) {
    if (!s_dumped.insert(t.hash).second) return;
    { std::error_code ec; std::filesystem::create_directories("textures/dump", ec); }
    std::vector<u32> px(t.px);
    if (!tex_wide_alpha(px))
        for (u32& c : px) { u32 a = (c >> 24) * 2; c = (c & 0xffffff) | ((a > 255 ? 255 : a) << 24); }
    extern bool gs_write_png_rgba(const char*, const std::vector<u32>&, int, int);
    gs_write_png_rgba(("textures/dump/" + tex_name(t.w, t.h, t.hash)).c_str(), px, t.w, t.h);
}
static const int s_hi_test = std::getenv("MVP_HI_TEST") ? std::atoi(std::getenv("MVP_HI_TEST")) : 0;
static void hi_filter(DrawState& d) {
    // Built-in enhancement only where the game samples with bilinear filtering: point-sampled textures
    // are often look-up tables / exact texel data (e.g. per-vertex colour ramps) that upscaling breaks.
    if (d.hi && !d.hi->replacement && !d.bilinear) { d.hi.reset(); return; }
    // debug: MVP_HI_TEST=1 skip region wrap, 2 skip T8/T4, 3 skip blended, 4 skip tfx>=2, 5 all, 6 hash range
    if (!d.hi || !s_hi_test) return;
    static const u64 lo = std::getenv("MVP_HI_LO") ? std::strtoull(std::getenv("MVP_HI_LO"), nullptr, 16) : 0;
    static const u64 hi = std::getenv("MVP_HI_HI") ? std::strtoull(std::getenv("MVP_HI_HI"), nullptr, 16) : ~0ull;
    if (s_hi_test == 6) { u64 hsh = d.tex->hash; if (hsh < lo || hsh > hi) d.hi.reset(); else if (std::getenv("MVP_HI_PRINT")) fprintf(stderr, "[hi-used] %016llx %dx%d psm=%02x wms=%u wmt=%u tfx=%u abe=%d bil=%d\n", (unsigned long long)hsh, d.tex->w, d.tex->h, d.tpsm, d.wms, d.wmt, d.tfx, (int)d.abe, (int)d.bilinear); return; }
    bool drop = s_hi_test == 5 || (s_hi_test == 1 && (d.wms >= 2 || d.wmt >= 2)) || (s_hi_test == 2 && d.clut) || (s_hi_test == 3 && d.abe) || (s_hi_test == 4 && d.tfx >= 2);
    if (drop) d.hi.reset();
}
// Decoded texture for this draw, or null (sample VRAM). Must be called after palette lookup.
// MVP_TEXUSE=file: per texture, in how many frames it was used and (sprites) where it was drawn on screen;
// tools/hd/make_pack.py uses it to skip one-off textures (movie frames) and to re-assemble images the game
// draws as 16-pixel strips (menu backgrounds) before upscaling them.
static const char* s_texuse_path = std::getenv("MVP_TEXUSE");
struct TexUse { u32 frames = 0; u64 last = ~0ull; float x0 = 0, y0 = 0, x1 = 0, y1 = 0, u0 = 0, v0 = 0, u1 = 0, v1 = 0; u64 first_frame = 0; bool rect = false; };
static std::unordered_map<u64, TexUse> s_texuse;
static void texuse_note(u64 hash, const Vtx* sv, const DrawState& d) {
    extern u64 g_gs_frame;
    TexUse& u = s_texuse[hash];
    if (u.last != g_gs_frame) { if (!u.frames) u.first_frame = g_gs_frame; u.frames++; u.last = g_gs_frame; }
    if (sv && !u.rect) {
        u.rect = true; u.x0 = sv[0].x; u.y0 = sv[0].y; u.x1 = sv[1].x; u.y1 = sv[1].y;
        if (d.fst) { u.u0 = sv[0].u; u.v0 = sv[0].v; u.u1 = sv[1].u; u.v1 = sv[1].v; }
        else { u.u0 = sv[0].s / sv[0].q * (1 << d.tw); u.v0 = sv[0].t / sv[0].q * (1 << d.th); u.u1 = sv[1].s / sv[1].q * (1 << d.tw); u.v1 = sv[1].t / sv[1].q * (1 << d.th); }
    }
}
static void texuse_save() {
    if (!s_texuse_path) return;
    FILE* f = fopen(s_texuse_path, "w"); if (!f) return;
    for (auto& [h, u] : s_texuse)
        fprintf(f, "%016llx %u %llu %d %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f\n", (unsigned long long)h, u.frames, (unsigned long long)u.first_frame, (int)u.rect,
                u.x0, u.y0, u.x1, u.y1, u.u0, u.v0, u.u1, u.v1);
    fclose(f);
}
static void tex_get(DrawState& d) {
    if (s_texcache_off) return;
    // Only worth it for HD textures / dumping: plain sampling from VRAM is as fast (measured on replays).
    if (!s_tex_scanned) tex_scan_replacements();
    if (!s_texcache_force && g_settings.tex_enhance == 0 && !g_settings.tex_dump && (s_replace_names.empty() || !g_settings.tex_replace)) return;
    const u32 w = 1u << d.tw, h = 1u << d.th;
    if (w * h > (1u << 20) || w > 1024 || h > 1024) return;
    const u32 pw = tc_page_w(d.tpsm), ph = tc_page_h(d.tpsm);
    const u32 bwp = std::max<u32>((std::max<u32>(d.tbw * 64, w) + pw - 1) / pw, 1);
    const u32 rows = (h + ph - 1) / ph + 1;
    const u32 p0 = d.tbp >> 5, np = std::min<u32>(bwp * rows, 512);
    u64 gensum = 0;
    for (u32 k = 0; k < np; k++) { u32 p = (p0 + k) & 511; if (s_page_drawn[p]) return; gensum = mix64(gensum, s_page_gen[p]); }
    const bool pal = !(d.tpsm == CT32 || d.tpsm == CT24 || d.tpsm == CT16 || d.tpsm == CT16S);
    u64 key = mix64(mix64(mix64(d.tbp, d.tbw | (d.tpsm << 8) | (d.tw << 16) | (d.th << 20)), pal ? d.clut_serial : 0),
                    (d.tpsm == CT24 || d.tpsm == CT16 || d.tpsm == CT16S || pal) ? d.texa : 0);
    extern u64 g_gs_frame;
    auto it = s_tex_by_key.find(key);
    if (it != s_tex_by_key.end() && it->second.gensum == gensum && it->second.data) {
        s_tex_hits++;
        it->second.last_use = g_gs_frame; it->second.data->last_use = g_gs_frame;
        d.tex = it->second.data;
        d.hi = std::atomic_load(&it->second.data->hi);
        hi_filter(d);
        return;
    }
    s_tex_misses++;
    if (it == s_tex_by_key.end()) s_tex_miss_new++; else s_tex_miss_gen++;
    if (!s_tex_scanned) tex_scan_replacements();
    // pages about to be decoded may still be drawn by queued work
    if (gs_pages_busy(d.tbp, bwp, rows, true)) { gs_sync_reason(2); gs_sync(); }
    std::vector<u32> px((size_t)w * h);
    for (u32 y = 0; y < h; y++) for (u32 x = 0; x < w; x++) px[(size_t)y * w + x] = texel_raw(d, x, y);
    { u32 mw, mh; if (upext_rect(d.tbp, d.tbw, d.tpsm, w, h, mw, mh)) upext_mask(px, w, h, mw, mh); }
    s_tex_decodes++;
    u64 hash = mix64(w, h);
    for (u32 c : px) hash = (hash ^ c) * 0x100000001b3ull;
    std::shared_ptr<TexData> td;
    auto ht = s_tex_by_hash.find(hash);
    if (ht != s_tex_by_hash.end()) td = ht->second;
    else {
        td = std::make_shared<TexData>();
        td->w = (int)w; td->h = (int)h; td->hash = hash; td->px.swap(px);
        s_tex_by_hash[hash] = td;
        s_tex_bytes += (size_t)w * h * 4;
        if (g_settings.tex_dump && w >= 8 && h >= 8) tex_dump(*td);
        if (s_hr_scale > 1) tex_gutter(td->px, (int)w, (int)h);
    }
    if (!td->queued.load() && ((g_settings.tex_replace && s_replace_names.count(hash)) || g_settings.tex_enhance > 0)) enh_queue(td);
    td->last_use = g_gs_frame;
    TexKeyEntry& e = s_tex_by_key[key];
    e.gensum = gensum; e.data = td; e.last_use = g_gs_frame;
    d.tex = td;
    d.hi = std::atomic_load(&td->hi);
    hi_filter(d);
}
// Frame boundary: drop old cache entries when over budget; forget enhanced versions when the option changed.
static void tex_cache_trim() {
    s_prep_epoch++;                                       // (settings, cache entries: once a frame is often enough)
    static int last_enh = -1;
    int cur = g_settings.tex_enhance | (g_settings.tex_replace ? 8 : 0);
    if (last_enh != cur) {
        if (last_enh != -1) { gs_sync(); s_tex_by_key.clear(); s_tex_by_hash.clear(); s_tex_bytes = 0; s_enh_bytes = 0; }
        last_enh = cur;
    }
    const size_t budget = (size_t)512 << 20;
    if (s_tex_bytes < budget && s_tex_by_key.size() < 20000) return;
    extern u64 g_gs_frame;
    gs_sync();
    for (auto it = s_tex_by_key.begin(); it != s_tex_by_key.end();)
        if (g_gs_frame - it->second.last_use > 120) it = s_tex_by_key.erase(it); else ++it;
    s_tex_bytes = 0;
    for (auto it = s_tex_by_hash.begin(); it != s_tex_by_hash.end();) {
        if (g_gs_frame - it->second->last_use > 120) { it = s_tex_by_hash.erase(it); continue; }
        s_tex_bytes += (size_t)it->second->w * it->second->h * 4;
        auto hi = std::atomic_load(&it->second->hi);
        if (hi) s_tex_bytes += hi->px.size() * 4;
        ++it;
    }
}
// Sampling from the cache (original texels or the HD version).
static inline u32 cached_texel(const DrawState& d, int u, int v) {
    const TexData* t = d.tex.get();
    u = wrap(u, t->w, d.wms, d.minu, d.maxu);
    v = wrap(v, t->h, d.wmt, d.minv, d.maxv);
    if ((unsigned)u >= (unsigned)t->w || (unsigned)v >= (unsigned)t->h) return 0;
    return t->px[(size_t)v * t->w + u];
}
static inline u32 hi_texel(const HiData* hd, int x, int y, bool repeat_u, bool repeat_v) {
    x = repeat_u ? ((x % hd->w) + hd->w) % hd->w : std::min(std::max(x, 0), hd->w - 1);
    y = repeat_v ? ((y % hd->h) + hd->h) % hd->h : std::min(std::max(y, 0), hd->h - 1);
    return hd->px[(size_t)y * hd->w + x];
}
static inline u32 tex_sample(const DrawState& d, float tu, float tv) {
    if (d.hi) {
        // wrap in original texel space, keep the sub-texel position, then sample the HD image
        const TexData* t = d.tex.get(); const HiData* hd = d.hi.get();
        float fu = std::floor(tu), fv = std::floor(tv);
        int wu = wrap((int)fu, t->w, d.wms, d.minu, d.maxu), wv = wrap((int)fv, t->h, d.wmt, d.minv, d.maxv);
        float hx = (wu + (tu - fu)) * hd->sx, hy = (wv + (tv - fv)) * hd->sy;
        bool ru = d.wms == 0, rv = d.wmt == 0;
        if (!d.bilinear) return hi_texel(hd, (int)hx, (int)hy, ru, rv);
        hx -= 0.5f; hy -= 0.5f;
        float gx = std::floor(hx), gy = std::floor(hy);
        int ix = (int)gx, iy = (int)gy;
        u32 ax = (u32)((hx - gx) * 256.0f), ay = (u32)((hy - gy) * 256.0f);
        return lerp_c(lerp_c(hi_texel(hd, ix, iy, ru, rv), hi_texel(hd, ix + 1, iy, ru, rv), ax),
                      lerp_c(hi_texel(hd, ix, iy + 1, ru, rv), hi_texel(hd, ix + 1, iy + 1, ru, rv), ax), ay);
    }
    if (!d.bilinear) return cached_texel(d, (int)std::floor(tu), (int)std::floor(tv));
    tu -= 0.5f; tv -= 0.5f;
    float fu = std::floor(tu), fv = std::floor(tv);
    int iu = (int)fu, iv = (int)fv;
    u32 wu = (u32)((tu - fu) * 256.0f), wv = (u32)((tv - fv) * 256.0f);
    return lerp_c(lerp_c(cached_texel(d, iu, iv), cached_texel(d, iu + 1, iv), wu),
                  lerp_c(cached_texel(d, iu, iv + 1), cached_texel(d, iu + 1, iv + 1), wu), wv);
}

static u64 s_plotted = 0;   // plot calls (exact only with MVP_GS_THREADS=0)
static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static bool s_skip_draw = false;       // headless: only rasterize the frames that get dumped
// F bits: 1 = textured, 2 = alpha blending, 4 = depth test (specialised so the per-pixel path has no dead work)
template <int F>
static inline void plot_t(const DrawState& d, int x, int y, u32 z, int r, int g, int b, int a, float s, float t, float q, float fu, float fv, float fog) {
    s_plotted++;
    if (x < d.sx0 || x > d.sx1 || y < d.sy0 || y > d.sy1 || x < 0 || y < 0 || x >= 2048 || y >= 2048) return;
    if ((F & 8) && (x >= s_hr_w || y >= s_hr_h)) return;
    if (F & 1) {
        float tu, tv;
        if (d.fst) { tu = fu; tv = fv; }
        else { float iq = q != 0 ? 1.0f / q : 0.0f; tu = s * iq * (1 << d.tw); tv = t * iq * (1 << d.th); }
        u32 tc = d.tex ? tex_sample(d, tu, tv) : d.bilinear ? tex_bilinear(d, tu, tv) : tex_fetch(d, (int)std::floor(tu), (int)std::floor(tv));
        int tr = tc & 0xff, tg = (tc >> 8) & 0xff, tb = (tc >> 16) & 0xff, ta = tc >> 24;
        switch (d.tfx) {
        case 0: r = (tr * r) >> 7; g = (tg * g) >> 7; b = (tb * b) >> 7; if (d.tcc) a = (ta * a) >> 7; break;
        case 1: r = tr; g = tg; b = tb; if (d.tcc) a = ta; break;
        case 2: r = ((tr * r) >> 7) + a; g = ((tg * g) >> 7) + a; b = ((tb * b) >> 7) + a; if (d.tcc) a = ta + a; break;
        default: r = ((tr * r) >> 7) + a; g = ((tg * g) >> 7) + a; b = ((tb * b) >> 7) + a; if (d.tcc) a = ta; break;
        }
        r = clamp255(r); g = clamp255(g); b = clamp255(b); a = clamp255(a);
    }
    if (d.fge) {
        int f = (int)fog, fr = d.fogcol & 0xff, fg = (d.fogcol >> 8) & 0xff, fb = (d.fogcol >> 16) & 0xff;
        r = (r * f + fr * (255 - f)) >> 8; g = (g * f + fg * (255 - f)) >> 8; b = (b * f + fb * (255 - f)) >> 8;
    }
    // alpha test
    bool fb_write = true, z_write = !d.zmsk, rgb_only = false;
    if (d.test & 1) {
        u32 atst = (d.test >> 1) & 7, aref = (d.test >> 4) & 0xff;
        bool pass;
        switch (atst) {
            case 0: pass = false; break; case 1: pass = true; break;
            case 2: pass = (u32)a < aref; break; case 3: pass = (u32)a <= aref; break;
            case 4: pass = (u32)a == aref; break; case 5: pass = (u32)a >= aref; break;
            case 6: pass = (u32)a > aref; break; default: pass = (u32)a != aref; break;
        }
        if (!pass) {
            switch ((d.test >> 12) & 3) {
                case 0: return;
                case 1: z_write = false; break;
                case 2: fb_write = false; break;
                case 3: z_write = false; rgb_only = true; break;
            }
        }
    }
    // depth test (the Z address is computed once and reused for the write)
    u32 za = 0;
    const bool z16 = d.zpsm == Z16 || d.zpsm == Z16S;
    if (F & 4) {
        u32 ztst = (d.test >> 17) & 3;
        u32 zcur;
        if (F & 8) { za = (u32)y * s_hr_w + x; zcur = s_hr_z[za]; }
        else if (!z16) { za = addr32(d.zbp, d.fbw, (u32)x, (u32)y, true); zcur = s_vram[za]; if (d.zpsm == Z24) zcur &= 0xffffff; }
        else { za = addr16(d.zbp, d.fbw, (u32)x, (u32)y, d.zpsm == Z16 ? kBlockZ16 : kBlockZ16S); zcur = s_vram16[za]; }
        u32 zz = z16 ? std::min<u32>(z, 0xffff) : d.zpsm == Z24 ? std::min<u32>(z, 0xffffff) : z;
        bool pass = ztst == 1 || (ztst == 2 && zz >= zcur) || (ztst == 3 && zz > zcur);
        if (!pass) return;
        z = zz;
    } else z_write = false;
    // frame buffer: one address for read-modify-write
    const bool f32 = d.fpsm == CT32 || d.fpsm == CT24, f16 = d.fpsm == CT16 || d.fpsm == CT16S;
    u32 fa = 0, dst = 0;
    const bool need_dst = (F & 2) || (d.test & (1 << 14)) || d.fbmsk || rgb_only || d.fpsm == CT24 || !(f32 || f16);
    if (F & 8) { fa = (u32)y * s_hr_w + x; dst = s_hr_col[fa]; }
    else if (f32) { fa = addr32(d.fbp, d.fbw, (u32)x, (u32)y); if (need_dst) dst = s_vram[fa]; }
    else if (f16) { fa = addr16(d.fbp, d.fbw, (u32)x, (u32)y, d.fpsm == CT16 ? kBlock16 : kBlock16S);
                    if (need_dst) dst = c16to32(s_vram16[fa], 0x8000000000ull | 0x80); }
    else dst = read_px(d.fpsm, d.fbp, d.fbw, (u32)x, (u32)y);
    // destination alpha test
    if (d.test & (1 << 14)) {
        if ((bool)(dst >> 31) != (bool)((d.test >> 15) & 1)) return;
    }
    if (F & 2) {
        u32 A = d.alpha & 3, B = (d.alpha >> 2) & 3, C = (d.alpha >> 4) & 3, D = (d.alpha >> 6) & 3, fix = d.alpha >> 8;
        int cs[3] = {r, g, b}, cd[3] = {(int)(dst & 0xff), (int)((dst >> 8) & 0xff), (int)((dst >> 16) & 0xff)};
        int as = a, ad = (int)(dst >> 24);
        if (d.fpsm == CT24) ad = 0x80;
        int cc = C == 0 ? as : C == 1 ? ad : (int)fix;
        int out[3];
        for (int k = 0; k < 3; k++) {
            int va = A == 0 ? cs[k] : A == 1 ? cd[k] : 0, vb = B == 0 ? cs[k] : B == 1 ? cd[k] : 0, vd = D == 0 ? cs[k] : D == 1 ? cd[k] : 0;
            out[k] = (((va - vb) * cc) >> 7) + vd;
        }
        r = out[0]; g = out[1]; b = out[2];
    }
    if (d.colclamp & 1) { r = clamp255(r); g = clamp255(g); b = clamp255(b); } else { r &= 0xff; g &= 0xff; b &= 0xff; }
    u32 col = (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)(a & 0xff) << 24);
    if (d.fba) col |= 0x80000000u;
    if (fb_write) {
        u32 mask = d.fbmsk;
        if (rgb_only) mask |= 0xff000000u;
        if (F & 8) {
            s_hr_col[fa] = mask ? ((dst & mask) | (col & ~mask)) : col;
        } else if (f32) {
            if (d.fpsm == CT24) mask |= 0xff000000u;
            s_vram[fa] = mask ? ((dst & mask) | (col & ~mask)) : col;
        } else if (f16) {
            u32 m16 = c32to16(mask); u16& w = s_vram16[fa];
            w = (u16)((w & m16) | (c32to16(col) & ~m16));
        } else write_px(d.fpsm, d.fbp, d.fbw, (u32)x, (u32)y, col, mask);
    }
    if (z_write) {
        if (F & 8) s_hr_z[za] = z;
        else if (F & 4) {
            if (z16) s_vram16[za] = (u16)z;
            else if (d.zpsm == Z24) s_vram[za] = (s_vram[za] & 0xff000000u) | (z & 0xffffff);
            else s_vram[za] = z;
        } else write_px(d.zpsm, d.zbp, d.fbw, (u32)x, (u32)y, z);
    }
}

static inline int plot_kind(const DrawState& d) { return (d.tme ? 1 : 0) | (d.abe ? 2 : 0) | ((d.test & (1 << 16)) ? 4 : 0) | (d.hr ? 8 : 0); }
static void plot(const DrawState& d, int x, int y, u32 z, int r, int g, int b, int a, float s, float t, float q, float fu, float fv, float fog) {
    switch (plot_kind(d)) {
#define PK(n) case n: plot_t<n>(d, x, y, z, r, g, b, a, s, t, q, fu, fv, fog); break;
    PK(0) PK(1) PK(2) PK(3) PK(4) PK(5) PK(6) PK(7)
    PK(8) PK(9) PK(10) PK(11) PK(12) PK(13) PK(14) PK(15)
#undef PK
    }
}

static inline bool my_row(int y, int band, int nb) { return nb <= 1 || ((y >> 3) % nb) == band; }
template <int F>
static void draw_triangle_t(const DrawState& d, const Vtx& a, const Vtx& b, const Vtx& c, int band, int nb) {
    float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    if (area == 0) return;
    int minx = (int)std::ceil(std::min({a.x, b.x, c.x})), maxx = (int)std::ceil(std::max({a.x, b.x, c.x})) - 1;
    int miny = (int)std::ceil(std::min({a.y, b.y, c.y})), maxy = (int)std::ceil(std::max({a.y, b.y, c.y})) - 1;
    minx = std::max(minx, d.sx0); maxx = std::min(maxx, d.sx1); miny = std::max(miny, d.sy0); maxy = std::min(maxy, d.sy1);
    if (maxx < minx || maxy < miny || maxx - minx > 2048 || maxy - miny > 2048) return;
    minx = std::max(minx, 0); miny = std::max(miny, 0); maxx = std::min(maxx, 2047); maxy = std::min(maxy, 2047);
    float inv = 1.0f / area;
    const Vtx& fc = c;                                  // flat shading uses the last vertex
    const bool iip = d.iip, persp = !d.fst, fog = d.fge;
    // d(w0)/dx and d(w1)/dx are constant; attributes step by their x-gradients along a span
    const float dw0 = -(c.y - b.y) * inv, dw1 = -(a.y - c.y) * inv;
    auto dx_of = [&](float va, float vb, float vc) { return (va - vc) * dw0 + (vb - vc) * dw1; };
    const float dz = dx_of(a.z, b.z, c.z);
    float dr = 0, dg = 0, db = 0, da = 0, ds = 0, dt = 0, dq = 0, du = 0, dv = 0, df = 0;
    if (iip) { dr = dx_of(a.r, b.r, c.r); dg = dx_of(a.g, b.g, c.g); db = dx_of(a.b, b.b, c.b); da = dx_of(a.a, b.a, c.a); }
    if (F & 1) { if (persp) { ds = dx_of(a.s, b.s, c.s); dt = dx_of(a.t, b.t, c.t); dq = dx_of(a.q, b.q, c.q); }
                 else { du = dx_of(a.u, b.u, c.u); dv = dx_of(a.v, b.v, c.v); } }
    if (fog) df = dx_of(a.f, b.f, c.f);
    const Vtx* V3[3] = {&a, &b, &c};
    for (int y = miny; y <= maxy; y++) {
        if (!my_row(y, band, nb)) continue;
        float py = (float)y;
        // covered x-range from the three edges (1-pixel margin; exact test per pixel below)
        float lo = (float)minx, hi = (float)maxx;
        for (int k = 0; k < 3; k++) {
            const Vtx& p0 = *V3[k]; const Vtx& p1 = *V3[(k + 1) % 3];
            float ey = p1.y - p0.y;
            if (ey == 0) continue;
            float xi = p0.x + (py - p0.y) * (p1.x - p0.x) / ey;
            const Vtx& q = *V3[(k + 2) % 3];
            float side = (p1.x - p0.x) * (q.y - p0.y) - (q.x - p0.x) * (p1.y - p0.y);
            if ((-ey > 0) == (side > 0)) lo = std::max(lo, xi - 1.0f); else hi = std::min(hi, xi + 1.0f);
        }
        if (hi < lo) continue;
        int x0 = std::max(minx, (int)std::floor(lo)), x1 = std::min(maxx, (int)std::ceil(hi));
        // skip the margin pixels that are outside, then walk the inside run incrementally
        int x = x0;
        float w0 = 0, w1 = 0;
        for (; x <= x1; x++) {
            float px = (float)x;
            w0 = ((b.x - px) * (c.y - py) - (c.x - px) * (b.y - py)) * inv;
            w1 = ((c.x - px) * (a.y - py) - (a.x - px) * (c.y - py)) * inv;
            // the third weight from its own edge, not as 1 - w0 - w1: on a row that lies exactly on a horizontal
            // edge it is exactly 0, while the difference can round to -1e-8 and drop the whole row (a black
            // line across the infield where two rows of the field mesh meet, at 2x)
            if (w0 >= 0 && w1 >= 0 && ((a.x - px) * (b.y - py) - (b.x - px) * (a.y - py)) * inv >= 0) break;
        }
        if (x > x1) continue;
        float w2 = 1.0f - w0 - w1;
        // attributes relative to the last vertex: a value that is the same at all three corners stays exactly that value
        // (w0*z + w1*z + w2*z rounds to z-1 on some rows: layers drawn at one depth with a >= test then lose those rows)
        (void)w2;
        auto at = [&](float va, float vb, float vc) { return vc + w0 * (va - vc) + w1 * (vb - vc); };
        float z = at(a.z, b.z, c.z);
        float r = 0, g = 0, bb = 0, al = 0, s = 0, t = 0, q = 0, u = 0, v = 0, f = 0;
        if (iip) { r = at(a.r, b.r, c.r); g = at(a.g, b.g, c.g); bb = at(a.b, b.b, c.b); al = at(a.a, b.a, c.a); }
        if (F & 1) { if (persp) { s = at(a.s, b.s, c.s); t = at(a.t, b.t, c.t); q = at(a.q, b.q, c.q); }
                     else { u = at(a.u, b.u, c.u); v = at(a.v, b.v, c.v); } }
        if (fog) f = at(a.f, b.f, c.f);
        for (; x <= x1; x++) {
            if (w0 < 1e-4f || w1 < 1e-4f || 1.0f - w0 - w1 < 1e-4f) {
                // near/over an edge: decide exactly; stop once past the far edge (or a margin pixel): re-check exactly, stop once past the far edge
                float px = (float)x;
                float e0 = ((b.x - px) * (c.y - py) - (c.x - px) * (b.y - py)) * inv;
                float e1 = ((c.x - px) * (a.y - py) - (a.x - px) * (c.y - py)) * inv;
                float e2 = ((a.x - px) * (b.y - py) - (b.x - px) * (a.y - py)) * inv;
                if (e0 < 0 || e1 < 0 || e2 < 0) break;
            }
            if (iip) plot_t<F>(d, x, y, (u32)std::max(0.0f, z), (int)r, (int)g, (int)bb, (int)al, s, t, q, u, v, f);
            else plot_t<F>(d, x, y, (u32)std::max(0.0f, z), (int)fc.r, (int)fc.g, (int)fc.b, (int)fc.a, s, t, q, u, v, f);
            w0 += dw0; w1 += dw1; z += dz;
            if (iip) { r += dr; g += dg; bb += db; al += da; }
            if (F & 1) { s += ds; t += dt; q += dq; u += du; v += dv; }
            if (fog) f += df;
        }
    }
}
static void draw_triangle(const DrawState& d, const Vtx& a, const Vtx& b, const Vtx& c, int band, int nb) {
    switch (plot_kind(d)) {
#define TK(n) case n: draw_triangle_t<n>(d, a, b, c, band, nb); break;
    TK(0) TK(1) TK(2) TK(3) TK(4) TK(5) TK(6) TK(7)
    TK(8) TK(9) TK(10) TK(11) TK(12) TK(13) TK(14) TK(15)
#undef TK
    }
}

static const bool s_sprite_clamp = !std::getenv("MVP_NO_SPRITE_CLAMP");
template <int F>
static void draw_sprite_t(const DrawState& d, const Vtx& a, const Vtx& b, int band, int nb) {
    int x0 = (int)std::ceil(std::min(a.x, b.x)), x1 = (int)std::ceil(std::max(a.x, b.x)) - 1;
    int y0 = (int)std::ceil(std::min(a.y, b.y)), y1 = (int)std::ceil(std::max(a.y, b.y)) - 1;
    x0 = std::max(x0, d.sx0); x1 = std::min(x1, d.sx1); y0 = std::max(y0, d.sy0); y1 = std::min(y1, d.sy1);
    x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::min(x1, 2047); y1 = std::min(y1, 2047);
    if (x1 - x0 > 2048 || y1 - y0 > 2048) return;
    float dx = b.x - a.x, dy = b.y - a.y;
    float ids = dx != 0 ? (b.s - a.s) / dx : 0, idu = dx != 0 ? (b.u - a.u) / dx : 0;
    // higher internal resolution: keep the texture coordinates inside what the console sampled (sprite_tex_range)
    float ulo = -1e30f, uhi = 1e30f, vlo = -1e30f, vhi = 1e30f, slo = -1e30f, shi = 1e30f, tlo = -1e30f, thi = 1e30f;
    if ((F & 1) && d.hr && s_sprite_clamp) {
        const float S = (float)s_hr_scale, hx = d.hi ? d.hi->sx : 1.0f, hy = d.hi ? d.hi->sy : 1.0f;
        float lo, hi;
        if (d.fst) {
            if (sprite_tex_range(a.x / S, b.x / S, a.u, b.u, hx, lo, hi)) { ulo = lo; uhi = hi; }
            if (sprite_tex_range(a.y / S, b.y / S, a.v, b.v, hy, lo, hi)) { vlo = lo; vhi = hi; }
        } else if (b.q != 0) {
            const float kw = (float)(1u << d.tw) / b.q, kh = (float)(1u << d.th) / b.q;
            if (sprite_tex_range(a.x / S, b.x / S, a.s * kw, b.s * kw, hx, lo, hi)) { slo = lo / kw; shi = hi / kw; if (slo > shi) std::swap(slo, shi); }
            if (sprite_tex_range(a.y / S, b.y / S, a.t * kh, b.t * kh, hy, lo, hi)) { tlo = lo / kh; thi = hi / kh; if (tlo > thi) std::swap(tlo, thi); }
        }
    }
    for (int y = y0; y <= y1; y++) {
        if (!my_row(y, band, nb)) continue;
        float fy = dy != 0 ? (y - a.y) / dy : 0;
        float t = a.t + (b.t - a.t) * fy, v = a.v + (b.v - a.v) * fy;
        t = std::min(std::max(t, tlo), thi); v = std::min(std::max(v, vlo), vhi);
        float s = a.s + (x0 - a.x) * ids, u = a.u + (x0 - a.x) * idu;
        for (int x = x0; x <= x1; x++) {
            plot_t<F>(d, x, y, (u32)b.z, (int)b.r, (int)b.g, (int)b.b, (int)b.a, std::min(std::max(s, slo), shi), t, b.q, std::min(std::max(u, ulo), uhi), v, b.f);
            s += ids; u += idu;
        }
    }
}
static void draw_sprite(const DrawState& d, const Vtx& a, const Vtx& b, int band, int nb) {
    switch (plot_kind(d)) {
#define SK(n) case n: draw_sprite_t<n>(d, a, b, band, nb); break;
    SK(0) SK(1) SK(2) SK(3) SK(4) SK(5) SK(6) SK(7)
    SK(8) SK(9) SK(10) SK(11) SK(12) SK(13) SK(14) SK(15)
#undef SK
    }
}

static void draw_line(const DrawState& d, const Vtx& a, const Vtx& b, int band, int nb) {
    int n = (int)std::max(std::fabs(b.x - a.x), std::fabs(b.y - a.y));
    if (n > 4096) return;
    for (int i = 0; i <= n; i++) {
        float t = n ? (float)i / n : 0;
        if (!my_row((int)(a.y + (b.y - a.y) * t), band, nb)) continue;
        plot(d, (int)(a.x + (b.x - a.x) * t), (int)(a.y + (b.y - a.y) * t), (u32)(a.z + (b.z - a.z) * t),
             (int)(a.r + (b.r - a.r) * t), (int)(a.g + (b.g - a.g) * t), (int)(a.b + (b.b - a.b) * t), (int)(a.a + (b.a - a.a) * t),
             a.s + (b.s - a.s) * t, a.t + (b.t - a.t) * t, a.q + (b.q - a.q) * t, a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t, a.f);
    }
}

// ---------------------------------------------------------------- rasterizer threads
// Primitives are queued and rasterized by worker threads, each owning every nb-th band of 8 rows, so
// per-pixel order is preserved. The queue is drained (gs_sync) before anything on the main thread
// touches VRAM (uploads, palette decode, display read-out) and before a primitive samples a texture
// that overlaps a render target drawn since the last drain. MVP_GS_THREADS=n (0 = rasterize inline).
struct DrawCmd { DrawState d; Vtx v[3]; u8 kind; };       // kind: 0 point 1 line 2 triangle 3 sprite
static void exec_cmd(const DrawCmd& c, int band, int nb) {
    switch (c.kind) {
    case 0: if (my_row((int)c.v[0].y, band, nb)) plot(c.d, (int)c.v[0].x, (int)c.v[0].y, (u32)c.v[0].z, (int)c.v[0].r, (int)c.v[0].g, (int)c.v[0].b, (int)c.v[0].a,
                                                   c.v[0].s, c.v[0].t, c.v[0].q, c.v[0].u, c.v[0].v, c.v[0].f); break;
    case 1: draw_line(c.d, c.v[0], c.v[1], band, nb); break;
    case 2: draw_triangle(c.d, c.v[0], c.v[1], c.v[2], band, nb); break;
    case 3: draw_sprite(c.d, c.v[0], c.v[1], band, nb); break;
    }
}
static const u32 QCAP = 1 << 15;
static const bool s_matdbg_gs = std::getenv("MVP_MATDBG") != nullptr; static int s_matdbg_n = 0;
static u64 s_raster_ns = 0;
static const bool s_gshaz = std::getenv("MVP_GSHAZ") != nullptr; static int s_gshaz_n = 0; static u32 s_gshaz_tbp = ~0u;   // hazard debug log
static const long long s_bigdbg = std::getenv("MVP_BIGTRI") ? std::atoll(std::getenv("MVP_BIGTRI")) : -1; static int s_bigdbg_n = 0;
static const int s_probe_x = std::getenv("MVP_PROBE") ? std::atoi(std::getenv("MVP_PROBE")) : -1;
static const int s_probe_y = std::getenv("MVP_PROBE") && std::strchr(std::getenv("MVP_PROBE"), ',') ? std::atoi(std::strchr(std::getenv("MVP_PROBE"), ',') + 1) : -1;
static std::vector<DrawCmd> s_q(QCAP);
static std::atomic<u64> s_q_pub{0};                  // (epoch << 32) | published count
static u32 s_q_n = 0, s_q_epoch = 1;
static int s_nthreads = 0;
static std::atomic<u64> s_done[16];
static std::mutex s_q_mx; static std::condition_variable s_q_cv;
static std::vector<std::thread> s_workers;
static u64 s_dirty[8];                               // VRAM pages (8 KB) rendered to since the last drain
static u64 s_read[8];                                // VRAM pages sampled as textures since the last drain
static bool s_any_dirty = false;

static void worker_main(int id) {
    u32 my = 0, my_epoch = 0; int idle = 0;
    for (;;) {
        u64 pub = s_q_pub.load(std::memory_order_acquire);
        u32 ep = (u32)(pub >> 32), n = (u32)pub;
        if (ep != my_epoch) { my_epoch = ep; my = 0; }
        if (my < n) {
            while (my < n) exec_cmd(s_q[my++], id, s_nthreads);
            s_done[id].store(((u64)my_epoch << 32) | my, std::memory_order_release);
            idle = 0;
            continue;
        }
        s_done[id].store(((u64)my_epoch << 32) | my, std::memory_order_release);
        if (++idle < 200) { std::this_thread::yield(); continue; }
        std::unique_lock<std::mutex> lk(s_q_mx);
        s_q_cv.wait_for(lk, std::chrono::microseconds(500));
    }
}
static void gs_threads_init() {
    static bool done = false; if (done) return; done = true;
    int n = (int)std::thread::hardware_concurrency() - 1;
    if (n > 8) n = 8;
    if (g_settings.gs_threads >= 0) n = g_settings.gs_threads;
    if (const char* e = std::getenv("MVP_GS_THREADS")) n = std::atoi(e);
    if (n < 2) n = 0;
    if (n > 16) n = 16;
    s_nthreads = n;
#ifndef _WIN32
    sigset_t all, old; sigfillset(&all); pthread_sigmask(SIG_BLOCK, &all, &old);   // signals stay on the main thread
#endif
    for (int i = 0; i < n; i++) { s_done[i].store(0); s_workers.emplace_back(worker_main, i); s_workers.back().detach(); }
#ifndef _WIN32
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
#endif
    if (n) rlog("gs", "rasterizing on %d threads", n);
}
static bool gs_queue_busy() { return s_q_n != 0; }
static u64 s_sync_n[6];  // drains: 0 xfer 1 clut 2 texture hazard 3 display 4 full 5 other
static int s_sync_reason = 5; static u64 s_sync_ns = 0;
static void gs_sync_reason(int r) { s_sync_reason = r; }
void gs_sync() {                                     // wait until every queued primitive is drawn
    if (s_q_n) s_sync_n[s_sync_reason]++;
    s_sync_reason = 5;
    if (!s_q_n) { s_any_dirty = false; std::memset(s_dirty, 0, sizeof s_dirty); return; }
    u64 want = ((u64)s_q_epoch << 32) | s_q_n;
    u64 t0 = host_time_ns();
    s_q_cv.notify_all();
    for (int i = 0; i < s_nthreads; i++)
        while (s_done[i].load(std::memory_order_acquire) != want) std::this_thread::yield();
    s_sync_ns += host_time_ns() - t0;
    s_q_n = 0; s_q_epoch++;
    s_q_pub.store((u64)s_q_epoch << 32, std::memory_order_release);
    s_any_dirty = false; std::memset(s_dirty, 0, sizeof s_dirty); std::memset(s_read, 0, sizeof s_read);
}
static u32 gs_epoch() { return s_q_epoch; }
static inline void mark_read(u32 bp, u32 bw, u32 rows) {
    u32 p0 = bp >> 5, n = std::max<u32>(bw, 1) * std::max<u32>(rows, 1);
    if (n > 512) n = 512;
    for (u32 k = 0; k < n; k++) { u32 p = (p0 + k) & 511; s_read[p >> 6] |= 1ull << (p & 63); }
}
// true if queued work writes (or, unless writes_only, reads) any page in the range
static bool gs_pages_busy(u32 bp, u32 bw, u32 rows, bool writes_only) {
    if (!s_q_n) return false;
    u32 p0 = bp >> 5, n = std::max<u32>(bw, 1) * std::max<u32>(rows, 1);
    if (n > 512) n = 512;
    for (u32 k = 0; k < n; k++) {
        u32 p = (p0 + k) & 511; u64 bit = 1ull << (p & 63);
        if ((s_dirty[p >> 6] & bit) || (!writes_only && (s_read[p >> 6] & bit))) return true;
    }
    return false;
}
static inline void mark_pages(u32 bp, u32 bw, u32 rows) {       // bp in blocks; bw in pages; rows of pages
    u32 p0 = bp >> 5, n = std::max<u32>(bw, 1) * std::max<u32>(rows, 1);
    if (n > 512) n = 512;
    for (u32 k = 0; k < n; k++) { u32 p = (p0 + k) & 511; s_dirty[p >> 6] |= 1ull << (p & 63); }
    s_any_dirty = true;
}
static inline bool pages_dirty(u32 bp, u32 bw, u32 rows) {
    if (!s_any_dirty) return false;
    u32 p0 = bp >> 5, n = std::max<u32>(bw, 1) * std::max<u32>(rows, 1);
    if (n > 512) n = 512;
    for (u32 k = 0; k < n; k++) { u32 p = (p0 + k) & 511; if (s_dirty[p >> 6] & (1ull << (p & 63))) return true; }
    return false;
}
static inline u32 page_h(u32 psm) {
    switch (psm) { case CT32: case CT24: case Z32: case Z24: case T8H: case T4HL: case T4HH: return 32;
                   case T4: return 128; default: return 64; }
}
static inline u32 page_w(u32 psm) { return (psm == T8 || psm == T4) ? 128 : 64; }
static void mark_rect(u32 bp, u32 bw, u32 psm, int x0, int y0, int x1, int y1) {
    if (s_gshaz) { static std::set<u64> seen; u64 k = ((u64)bp << 32) | ((u64)bw << 16) | psm;
        if (seen.insert(k).second) fprintf(stderr, "[gs-target] bp=%05x bw=%u psm=%02x rect %d,%d-%d,%d\n", bp, bw, psm, x0, y0, x1, y1); }
    u32 pw = page_w(psm), ph = page_h(psm);
    u32 bwp = std::max<u32>((bw * 64 + pw - 1) / pw, 1);
    for (u32 py = (u32)y0 / ph; py <= (u32)y1 / ph; py++)
        for (u32 px = (u32)x0 / pw; px <= (u32)x1 / pw; px++) {
            u32 p = ((bp >> 5) + py * bwp + px) & 511; s_dirty[p >> 6] |= 1ull << (p & 63);
        }
    s_any_dirty = true;
}
static u64 s_vram_gen_fwd(); 
static void submit_one(const DrawState& d, u8 kind, const Vtx* v, int nv);
static u64 s_hr_copy_frame = ~0ull;
static inline u32 display_fbp() {
    u64 pmode = s_priv[0x00];
    int circuit = (pmode & 2) ? 2 : 1;
    if (!(pmode & 3)) circuit = 1;
    return bits(s_priv[circuit == 1 ? 0x07 : 0x09], 0, 9) * 32;
}
static void hr_target_changed(); static void lazy_clear();
// Watches draws into the display buffer: a textured full-screen-ish CT32 sprite copy names the main target.
static void hr_track(const DrawState& d, u8 kind, const Vtx* v) {
    if (d.fbp != display_fbp()) return;
    if (kind == 3 && d.tme && d.tpsm == CT32 && d.tbw >= 4 && d.tbp != d.fbp) {
        if (s_hr_copy_frame != g_gs_frame || s_hr_tbp != d.tbp) {
            if (s_hr_tbp != d.tbp) { rlog("gs", "main target: %05x (tbw %u) at frame %llu", d.tbp, d.tbw, (unsigned long long)g_gs_frame); hr_target_changed(); }
            s_hr_copy_frame = g_gs_frame;
            lazy_clear();
            s_hr_u0 = s_hr_v0 = 1e9f; s_hr_u1 = s_hr_v1 = -1e9f;
        }
        s_hr_tbp = d.tbp; s_hr_tbw = d.tbw;
        float u0, v0, u1, v1;
        if (d.fst) { u0 = v[0].u; v0 = v[0].v; u1 = v[1].u; v1 = v[1].v; }
        else { float w = (float)(1 << d.tw), h = (float)(1 << d.th);
               u0 = v[0].s / v[0].q * w; v0 = v[0].t / v[0].q * h; u1 = v[1].s / v[1].q * w; v1 = v[1].t / v[1].q * h; }
        s_hr_u0 = std::min({s_hr_u0, u0, u1}); s_hr_u1 = std::max({s_hr_u1, u0, u1});
        s_hr_v0 = std::min({s_hr_v0, v0, v1}); s_hr_v1 = std::max({s_hr_v1, v0, v1});
        s_hr_valid = true;
    } else if (s_hr_copy_frame != g_gs_frame) {
        s_hr_valid = false;                          // something else draws straight to the screen (movies, loading)
    }
}
// ---------------------------------------------------------------- GPU renderer glue
// With the GPU renderer, primitives drawn into the main target go to gs_gpu.cpp instead of the software
// high-resolution copy. Textures are decoded from VRAM (any page, including render targets) into GL
// textures, cached by page generations.
static bool s_gpu_on = false;
struct GpuTexEntry {
    u64 gensum = 0; u32 id = 0; u64 last_use = 0;
    u64 chash = 0;                                // hash of the decoded pixels: the game sends unchanged textures again
    u64 rawhash = 0;                              // hash of the VRAM pages it was decoded from (0: not kept)
    std::shared_ptr<TexData> td;                  // decoded texture (shared with the HD / replacement system)
    u32 hd_id = 0; u8 hsx = 1, hsy = 1; bool hd_replacement = false; bool hd_checked = false;
};
static std::unordered_map<u64, GpuTexEntry> s_gpu_tex;
static u64 s_gpu_tex_uploads = 0, s_gpu_tex_hd = 0, s_gpu_tex_same = 0;
// 64-bit hash of decoded texels, four independent chains (one multiply after another is latency-bound)
static u64 texel_hash(const u32* p, size_t n) {
    const u64 M = 0x100000001b3ull; u64 h0 = 0xcbf29ce484222325ull ^ n, h1 = 0x9e3779b97f4a7c15ull, h2 = 0xc2b2ae3d27d4eb4full, h3 = 0x165667b19e3779f9ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        u64 q[4]; std::memcpy(q, p + i, 32);
        h0 = (h0 ^ q[0]) * M; h1 = (h1 ^ q[1]) * M; h2 = (h2 ^ q[2]) * M; h3 = (h3 ^ q[3]) * M;
    }
    for (; i < n; i++) h0 = (h0 ^ p[i]) * M;
    h1 ^= h1 >> 29; h2 ^= h2 >> 31; h3 ^= h3 >> 33;
    return ((h0 ^ (h0 >> 32)) + h1 * 3 + h2 * 5 + h3 * 7) * M;
}
// HD textures (texture packs in textures/replace/, or the built-in upscaler) are looked up by the same pixel
// hash as in the software renderer and uploaded as a larger GL texture; the shader samples it on a finer
// grid (u_hs). Only integer scale factors, and not for REGION_REPEAT addressing.
static bool gpu_hd_wanted() { return (g_settings.tex_replace && !s_replace_names.empty()) || g_settings.tex_enhance > 0 || g_settings.tex_dump; }
static u32 gpu_texture(DrawState& d, u16& tw, u16& th, u8& hsx, u8& hsy) {
    hsx = hsy = 1;
    const u32 w = 1u << d.tw, h = 1u << d.th;
    if (w > 1024 || h > 1024) return 0;
    tw = (u16)w; th = (u16)h;
    const u32 pw = tc_page_w(d.tpsm), ph = tc_page_h(d.tpsm);
    const u32 bwp = std::max<u32>((std::max<u32>(d.tbw * 64, w) + pw - 1) / pw, 1);
    const u32 rows = (h + ph - 1) / ph + 1;
    const u32 p0 = d.tbp >> 5, np = std::min<u32>(bwp * rows, 512);
    u64 gensum = 0; bool drawn = false;
    for (u32 k = 0; k < np; k++) { gensum = mix64(gensum, s_page_gen[(p0 + k) & 511]); drawn |= s_page_drawn[(p0 + k) & 511]; }
    const bool pal = !(d.tpsm == CT32 || d.tpsm == CT24 || d.tpsm == CT16 || d.tpsm == CT16S);
    u64 key = mix64(mix64(mix64(d.tbp, d.tbw | (d.tpsm << 8) | (d.tw << 16) | (d.th << 20)), pal ? d.clut_serial : 0),
                    (d.tpsm == CT24 || d.tpsm == CT16 || d.tpsm == CT16S || pal) ? d.texa : 0);
    GpuTexEntry& e = s_gpu_tex[key];
    e.last_use = g_gs_frame;
    if (!(e.id && e.gensum == gensum)) {
        if (gs_pages_busy(d.tbp, bwp, rows, true)) { gs_sync_reason(2); gs_sync(); }
        // the pages themselves first: when the game sent the same bytes again there is nothing to decode
        // (only where the pages are not much bigger than the texture)
        u64 rawhash = 0;
        if ((size_t)np * 2048 <= (size_t)w * h * 2) {
            for (u32 k = 0; k < np; k++) rawhash = mix64(rawhash, texel_hash(&s_vram[((p0 + k) & 511) * 2048], 2048));
            rawhash |= 1;
            if (e.id && e.rawhash == rawhash) { e.gensum = gensum; s_gpu_tex_same++; goto have; }
        }
        e.rawhash = rawhash;
        static std::vector<u32> px; px.resize((size_t)w * h);
        for (u32 y = 0; y < h; y++) for (u32 x = 0; x < w; x++) px[(size_t)y * w + x] = texel_raw(d, x, y);
        const u64 chash = texel_hash(px.data(), px.size());
        if (e.id && e.chash == chash) { e.gensum = gensum; s_gpu_tex_same++; goto have; }    // sent again, unchanged
        e.chash = chash;
        { static const bool texlog = std::getenv("MVP_TEXLOG") != nullptr;
        if (texlog) fprintf(stderr, "[gpu-tex] tbp=%05x tbw=%u psm=%02x %ux%u cbp=%05x %s drawn=%d\n", d.tbp, d.tbw, d.tpsm, w, h, d.cbp, e.id ? "changed" : "new", (int)s_page_drawn[p0]);
        if (e.id) gpu_tex_free(e.id);
        if (e.hd_id) gpu_tex_free(e.hd_id);
        e.hd_id = 0; e.hsx = e.hsy = 1; e.hd_checked = false; e.td.reset();
        if (s_hr_scale > 1 && w >= 128 && h >= 128 && (px[0] >> 24) && (px[0] >> 24) < 0x80) {
            std::vector<u32> fixed(px);
            e.id = gpu_tex_create(tex_gutter(fixed, (int)w, (int)h) ? fixed.data() : px.data(), (int)w, (int)h);
        } else e.id = gpu_tex_create(px.data(), (int)w, (int)h);
        e.gensum = gensum;
        s_gpu_tex_uploads++;
        if (!drawn && gpu_hd_wanted()) {             // render targets are never replaced
            if (!s_tex_scanned) tex_scan_replacements();
            { u32 mw, mh; if (upext_rect(d.tbp, d.tbw, d.tpsm, w, h, mw, mh)) upext_mask(px, w, h, mw, mh); }   // (the GL texture above keeps what is in memory)
            u64 hash = mix64(w, h);
            for (u32 c : px) hash = (hash ^ c) * 0x100000001b3ull;
            std::shared_ptr<TexData> td;
            auto ht = s_tex_by_hash.find(hash);
            if (ht != s_tex_by_hash.end()) td = ht->second;
            else {
                td = std::make_shared<TexData>();
                td->w = (int)w; td->h = (int)h; td->hash = hash; td->px = px;
                s_tex_by_hash[hash] = td; s_tex_bytes += (size_t)w * h * 4;
                if (g_settings.tex_dump && w >= 8 && h >= 8) tex_dump(*td);
                if (s_hr_scale > 1) tex_gutter(td->px, (int)w, (int)h);
            }
            td->last_use = g_gs_frame;
            if (!td->queued.load() && ((g_settings.tex_replace && s_replace_names.count(hash)) || g_settings.tex_enhance > 0)) enh_queue(td);
            e.td = td;
        }
        }
    }
have:
    if (e.td && !e.hd_checked) {                      // HD version published by the enhancer thread yet?
        if (auto hi = std::atomic_load(&e.td->hi)) {
            e.hd_checked = true;
            const int sx = hi->w / std::max(e.td->w, 1), sy = hi->h / std::max(e.td->h, 1);
            if (sx >= 1 && sy >= 1 && sx <= 16 && sy <= 16 && sx * e.td->w == hi->w && sy * e.td->h == hi->h && (sx > 1 || sy > 1)) {
                e.hd_id = gpu_tex_create(hi->px.data(), hi->w, hi->h); e.hsx = (u8)sx; e.hsy = (u8)sy;
                e.hd_replacement = hi->replacement; s_gpu_tex_hd++;
            }
        }
    }
    // same rule as the software renderer: built-in upscaling only where the game filters bilinearly
    if (e.hd_id && d.wms != 3 && d.wmt != 3 && (e.hd_replacement || d.bilinear)) { hsx = e.hsx; hsy = e.hsy; return e.hd_id; }
    return e.id;
}
static void gpu_tex_trim() {
    for (auto it = s_gpu_tex.begin(); it != s_gpu_tex.end();)
        if (g_gs_frame - it->second.last_use > 60) { gpu_tex_free(it->second.id); gpu_tex_free(it->second.hd_id); it = s_gpu_tex.erase(it); } else ++it;
}
static void gpu_submit(DrawState& d, u8 kind, const Vtx* v, int nv) {
    GpuState st; std::memset(&st, 0, sizeof st);
    st.kind = kind; st.iip = d.iip; st.fge = d.fge; st.abe = d.abe;
    st.zmsk = (u8)d.zmsk; st.fba = (u8)d.fba; st.colclamp = d.colclamp & 1; st.zpsm = (u8)d.zpsm;
    st.test = d.test; st.fbmsk = d.fbmsk;
    if (!(st.test & (1 << 16))) st.zpsm = 0;
    if (d.abe) st.alpha = d.alpha;
    if (d.fge) st.fogcol = d.fogcol & 0xffffff;
    st.sx0 = d.sx0; st.sy0 = d.sy0; st.sx1 = d.sx1; st.sy1 = d.sy1;
    if (d.tme) {
        st.tme = 1; st.fst = d.fst; st.bil = d.bilinear; st.tfx = (u8)d.tfx; st.tcc = (u8)d.tcc;
        st.wms = (u8)d.wms; st.wmt = (u8)d.wmt;
        if (d.wms >= 2) { st.minu = (u16)d.minu; st.maxu = (u16)d.maxu; }
        if (d.wmt >= 2) { st.minv = (u16)d.minv; st.maxv = (u16)d.maxv; }
        st.tex = gpu_texture(d, st.tex_w, st.tex_h, st.hsx, st.hsy);
    }
    gpu_prim(st, (const GpuVtx*)v, nv);
}
static_assert(sizeof(GpuVtx) == sizeof(Vtx), "GpuVtx must match Vtx");
bool gs_get_display_gpu(uintptr_t& fbo, int& x0, int& y0, int& w, int& h) {
    if (!s_gpu_on || !s_hr_valid || !(s_hr_u1 > s_hr_u0 && s_hr_v1 > s_hr_v0)) return false;
    gpu_flush();
    const int S = s_hr_scale;
    x0 = std::max(0, (int)std::floor(s_hr_u0 * S)); y0 = std::max(0, (int)std::floor(s_hr_v0 * S));
    int x1 = std::min(s_hr_w, (int)std::ceil(s_hr_u1 * S)), y1 = std::min(s_hr_h, (int)std::ceil(s_hr_v1 * S));
    w = x1 - x0; h = y1 - y0; fbo = gpu_target_handle();
    return w > 0 && h > 0 && fbo;
}

// ---- GPU-owned main target (the software rasterizer no longer draws it)
// s_own: VRAM pages whose current content only exists on the GPU. Anything that reads them from VRAM
// (texture, palette, transfer, display read-out) first downloads the GPU picture at native resolution.
// s_gpu_touched: pages the GPU target covers; a software write there makes the GPU copy stale, and it is
// re-uploaded from VRAM before the next GPU primitive.
static u64 s_own[8], s_gpu_touched[8], s_own_c[8], s_touched_c[8];   // *_c: colour pages only (Z24 top bytes stay valid in VRAM)
static bool s_own_any = false, s_gpu_stale = false;
static u32 s_gz_bp = ~0u, s_gz_psm = Z24;               // Z buffer used by the GPU draws
static u64 s_own_downloads = 0, s_own_resyncs = 0;
static const bool s_ownlog = std::getenv("MVP_OWNLOG") != nullptr; static int s_ownlog_n = 0;
static inline bool bit_test(const u64* b, u32 p) { return (b[(p & 511) >> 6] >> (p & 63)) & 1; }
static inline void bit_set(u64* b, u32 p) { b[(p & 511) >> 6] |= 1ull << (p & 63); }
template <class F> static void rect_pages(u32 bp, u32 bw, u32 psm, int x0, int y0, int x1, int y1, F f) {
    u32 pw = page_w(psm), ph = page_h(psm), bwp = std::max<u32>((bw * 64 + pw - 1) / pw, 1);
    x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::min(x1, 2047); y1 = std::min(y1, 2047);
    if (x1 < x0 || y1 < y0) return;
    for (u32 py = (u32)y0 / ph; py <= (u32)y1 / ph; py++)
        for (u32 px = (u32)x0 / pw; px <= (u32)x1 / pw; px++) f(((bp >> 5) + py * bwp + px) & 511);
}
static bool range_hits(const u64* b, u32 p0, u32 np) { for (u32 k = 0; k < np && k < 512; k++) if (bit_test(b, p0 + k)) return true; return false; }
static bool rect_hits(const u64* b, u32 bp, u32 bw, u32 psm, int x0, int y0, int x1, int y1) {
    bool hit = false; rect_pages(bp, bw, psm, x0, y0, x1, y1, [&](u32 p) { if (bit_test(b, p)) hit = true; }); return hit;
}
static void own_download(const char* why) {
    if (!s_own_any) return;
    s_own_any = false;
    gs_sync();
    s_own_downloads++;
    if (s_ownlog && s_ownlog_n++ < 200) fprintf(stderr, "[own] f=%llu download (%s)\n", (unsigned long long)g_gs_frame, why);
    static std::vector<u32> col; static std::vector<float> z;
    bool zown = false;
    const int W = (int)std::max<u32>(s_hr_tbw, 8) * 64, H = 512;
    if (s_gz_bp != ~0u) rect_pages(s_gz_bp, s_hr_tbw, s_gz_psm, 0, 0, W - 1, H - 1, [&](u32 p) { if (bit_test(s_own, p)) zown = true; });
    if (!gpu_read_native(col, zown ? &z : nullptr)) { std::memset(s_own, 0, sizeof s_own); std::memset(s_own_c, 0, sizeof s_own_c); return; }
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        u32 a = addr32(s_hr_tbp, s_hr_tbw, (u32)x, (u32)y);
        if (bit_test(s_own, a >> 11)) s_vram[a] = col[(size_t)y * W + x];
    }
    if (zown) {
        const bool z16 = s_gz_psm == Z16 || s_gz_psm == Z16S;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            double dz = (double)z[(size_t)y * W + x] * 4294967296.0;
            u32 zv = dz >= 4294967295.0 ? 0xffffffffu : (u32)dz;
            if (z16) {
                u32 a = addr16(s_gz_bp, s_hr_tbw, (u32)x, (u32)y, s_gz_psm == Z16 ? kBlockZ16 : kBlockZ16S);
                if (bit_test(s_own, a >> 12)) s_vram16[a] = (u16)std::min<u32>(zv, 0xffff);
            } else {
                u32 a = addr32(s_gz_bp, s_hr_tbw, (u32)x, (u32)y, true);
                if (!bit_test(s_own, a >> 11)) continue;
                if (s_gz_psm == Z24) s_vram[a] = (s_vram[a] & 0xff000000u) | std::min<u32>(zv, 0xffffff); else s_vram[a] = zv;
            }
        }
    }
    for (u32 p = 0; p < 512; p++) if (bit_test(s_own, p)) { s_page_gen[p] = ++s_gen_counter; s_page_drawn[p] = true; upext_pages_written(p, 1); }
    s_prep_epoch++;
    std::memset(s_own, 0, sizeof s_own); std::memset(s_own_c, 0, sizeof s_own_c);
}
static void gpu_resync() {                               // VRAM -> GPU after software writes to the target
    s_gpu_stale = false;
    gs_sync();
    s_own_resyncs++;
    if (s_ownlog && s_ownlog_n++ < 200) fprintf(stderr, "[own] f=%llu re-upload\n", (unsigned long long)g_gs_frame);
    const int W = (int)std::max<u32>(s_hr_tbw, 8) * 64, H = 512;
    static std::vector<u32> col; static std::vector<float> z;
    col.resize((size_t)W * H);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) col[(size_t)y * W + x] = s_vram[addr32(s_hr_tbp, s_hr_tbw, (u32)x, (u32)y)];
    const float* zp = nullptr;
    if (s_gz_bp != ~0u) {
        z.resize((size_t)W * H);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            u32 zv = read_px(s_gz_psm, s_gz_bp, s_hr_tbw, (u32)x, (u32)y);
            if (s_gz_psm == Z24) zv &= 0xffffff;
            z[(size_t)y * W + x] = (float)((double)zv / 4294967296.0);
        }
        zp = z.data();
    }
    gpu_write_native(col.data(), zp);
}
// The final copy of the main target into the display buffer is not drawn in software (the window shows the
// GPU picture). It is kept here and only executed if something reads or writes those display pages.
struct LazyCopy { DrawState d; u8 kind; Vtx v[3]; int nv; };
static std::vector<LazyCopy> s_lazy; static u64 s_lazy_pages[8]; static bool s_lazy_any = false, s_lazy_ct24 = true; static u64 s_lazy_runs = 0;   // ct24: copies only write the low 24 bits
static void lazy_clear() { s_lazy.clear(); }
static void lazy_realize(const char* why) {
    if (!s_lazy_any) return;

    s_lazy_any = false; std::memset(s_lazy_pages, 0, sizeof s_lazy_pages);
    if (s_ownlog && s_ownlog_n++ < 200) fprintf(stderr, "[own] f=%llu display copy realized (%s)\n", (unsigned long long)g_gs_frame, why);
    own_download("display copy");
    s_lazy_runs++;
    std::vector<LazyCopy> q; q.swap(s_lazy);
    for (LazyCopy& c : q) submit_one(c.d, c.kind, c.v, c.nv);
}
// before software code reads VRAM pages [p0, p0+np)
static inline void own_check_read(u32 p0, u32 np, const char* why, bool top) {
    const bool lazy_hit = s_lazy_any && !(top && s_lazy_ct24) && range_hits(s_lazy_pages, p0, np);
    if (s_ownlog && lazy_hit) fprintf(stderr, "[own] read %s pages %u+%u\n", why, p0, np);
    if (lazy_hit) lazy_realize(why);
    if (s_own_any && range_hits(top && s_gz_psm == Z24 ? s_own_c : s_own, p0, np)) {
        if (s_ownlog && s_ownlog_n < 200) fprintf(stderr, "[own] read %s pages %u+%u\n", why, p0, np);
        own_download(why);
    }
}
// before software code writes VRAM pages [p0, p0+np)
static inline bool top_byte_psm(u32 psm) { return psm == T8H || psm == T4HL || psm == T4HH; }
// top: the access only touches the top byte of 32-bit words (T8H / T4HL / T4HH), which a GPU-owned Z24
// buffer leaves valid in VRAM
static inline void own_check_write(u32 p0, u32 np, const char* why, bool top = false) {
    if (s_lazy_any && !(top && s_lazy_ct24) && range_hits(s_lazy_pages, p0, np)) lazy_realize(why);
    const bool zok = top && s_gz_psm == Z24;
    if (s_own_any && range_hits(zok ? s_own_c : s_own, p0, np)) own_download(why);
    if (s_gpu_on && range_hits(zok ? s_touched_c : s_gpu_touched, p0, np)) {
        if (s_ownlog && !s_gpu_stale && s_ownlog_n++ < 200) fprintf(stderr, "[own] f=%llu stale: %s pages %u+%u\n", (unsigned long long)g_gs_frame, why, p0, np);
        s_gpu_stale = true;
    }
}
static inline bool gpu_main(const DrawState& d) { return s_gpu_on && s_hr_tbp != ~0u && d.fbp == s_hr_tbp && d.fpsm == CT32 && d.fbw == s_hr_tbw; }
static void prim_rect(const DrawState& d, const Vtx* v, int nv, int& x0, int& y0, int& x1, int& y1) {
    float fx0 = v[0].x, fx1 = v[0].x, fy0 = v[0].y, fy1 = v[0].y;
    for (int i = 1; i < nv; i++) { fx0 = std::min(fx0, v[i].x); fx1 = std::max(fx1, v[i].x); fy0 = std::min(fy0, v[i].y); fy1 = std::max(fy1, v[i].y); }
    // pixels actually covered: [ceil(min), ceil(max) - 1] for triangles and sprites, the end point too for lines/points
    x0 = std::max((int)std::floor(fx0), d.sx0); y0 = std::max((int)std::floor(fy0), d.sy0);
    x1 = std::min((int)std::ceil(fx1) - 1, d.sx1); y1 = std::min((int)std::ceil(fy1) - 1, d.sy1);
    if (x1 < x0 && (int)std::floor(fx1) >= x0) x1 = std::min((int)std::floor(fx1), d.sx1);   // points / thin lines
    if (y1 < y0 && (int)std::floor(fy1) >= y0) y1 = std::min((int)std::floor(fy1), d.sy1);
}
static void tex_read_check(const DrawState& d) {
    if ((!s_own_any && !s_lazy_any) || !d.tme) return;
    u32 w = 1u << d.tw, h = 1u << d.th, p0, np;
    // the area actually sampled: region clamp limits it, otherwise the whole 2^tw x 2^th texture
    u32 x0 = 0, x1 = w - 1, y0 = 0, y1 = h - 1;
    if (d.wms == 2) { x0 = std::min(d.minu, w - 1); x1 = std::min(d.maxu, w - 1); }
    if (d.wmt == 2) { y0 = std::min(d.minv, h - 1); y1 = std::min(d.maxv, h - 1); }
    px_range(d.tpsm, d.tbp, std::max<u32>(d.tbw, 1), x0, y0, std::max(x0, x1), std::max(y0, y1), p0, np);
    own_check_read(p0, np, "texture", top_byte_psm(d.tpsm));
}

static void hr_target_changed() {                  // a new main target: the GPU starts from VRAM
    own_download("target change"); lazy_realize("target change");
    std::memset(s_gpu_touched, 0, sizeof s_gpu_touched); std::memset(s_touched_c, 0, sizeof s_touched_c);
    s_gpu_stale = true;
}
// ---------------------------------------------------------------- texture decompression fast path
// The game keeps most textures at 3 bits per pixel ("type 14" shapes) and has the GS unpack them, every frame,
// with two sprites into a CT32 target that is then read as an 8-bit texture: a DECAL copy of the base layer
// (4 palette indices per 32-bit texel) and an additive pass (Cd + Cs, FBMSK fc000000) of per-pixel offsets
// looked up in a palette. Here the first sprite is held back until the second arrives and both are done in one
// pass straight on VRAM. Pages only count as changed when a pixel really changes, so the unpacked texture
// stays in the texture caches instead of being re-read from VRAM every frame as a render target, and a
// skipped frame can still unpack textures cheaply. Anything that does not match exactly takes the normal path.
// MVP_NO_DECFAST=1 turns it off, MVP_DECCHECK=1 compares every result with the normal rasterizer.
static struct DecPending { bool pending = false, droppable = false; DrawState d; Vtx v[2]; } s_dec;
static u64 s_dec_pairs = 0, s_dec_px = 0, s_dec_changed = 0, s_dec_flushed = 0, s_dec_dropped = 0, s_dec_bad = 0, s_dec_hits = 0, s_dec_tabs = 0;
static const bool s_dec_off = std::getenv("MVP_NO_DECFAST") != nullptr;
static const bool s_dec_check = std::getenv("MVP_DECCHECK") != nullptr;

static inline bool dec_common(const DrawState& d) {
    return d.tme && d.fst && !d.fge && d.tfx == 1 && d.tcc == 1 && d.test == 0 && d.fpsm == CT32 && d.fbw > 0 && !d.bilinear && !d.hr &&
           d.fbp != s_hr_tbp && d.fbp != display_fbp();
}
static inline bool dec_is_a(const DrawState& d) { return dec_common(d) && !d.abe && d.tpsm == CT32 && d.tbw > 0 && d.fbmsk == 0; }
static inline bool dec_is_b(const DrawState& d, const Vtx* v) {
    const DrawState& a = s_dec.d;
    return dec_common(d) && d.abe && d.tpsm == T8 && d.clut && d.fbmsk == 0xfc000000u && d.alpha == 0x8029 &&
           d.fbp == a.fbp && d.fbw == a.fbw && d.sx0 == a.sx0 && d.sx1 == a.sx1 && d.sy0 == a.sy0 && d.sy1 == a.sy1 &&
           v[0].x == s_dec.v[0].x && v[0].y == s_dec.v[0].y && v[1].x == s_dec.v[1].x && v[1].y == s_dec.v[1].y;
}
static void tex_get(DrawState& d);
static void dec_flush() {                               // the held sprite was not part of a decompression after all
    if (!s_dec.pending) return;
    s_dec.pending = false;
    if (s_dec.droppable) { s_dec_dropped++; return; }   // it belongs to a skipped frame
    s_dec_flushed++;
    tex_get(s_dec.d);
    submit_one(s_dec.d, 3, s_dec.v, 2);
}
template <class Fn> static inline void tc_rect_pages(u32 bp, u32 bw, u32 psm, int x0, int y0, int x1, int y1, Fn fn) {   // as submit_one's page bump
    u32 pw = tc_page_w(psm), ph = tc_page_h(psm), bwp = std::max<u32>((bw * 64 + pw - 1) / pw, 1);
    for (u32 py = (u32)y0 / ph; py <= (u32)y1 / ph; py++)
        for (u32 px = (u32)x0 / pw; px <= (u32)x1 / pw; px++) fn(((bp >> 5) + py * bwp + px) & 511);
}
static void dec_run(const DrawState& db, const Vtx* vb) {
    const DrawState da = s_dec.d; const Vtx a0 = s_dec.v[0], a1 = s_dec.v[1];
    s_dec.pending = false;
    int x0 = (int)std::ceil(std::min(a0.x, a1.x)), x1 = (int)std::ceil(std::max(a0.x, a1.x)) - 1;
    int y0 = (int)std::ceil(std::min(a0.y, a1.y)), y1 = (int)std::ceil(std::max(a0.y, a1.y)) - 1;
    x0 = std::max({x0, da.sx0, 0}); x1 = std::min({x1, da.sx1, 2047}); y0 = std::max({y0, da.sy0, 0}); y1 = std::min({y1, da.sy1, 2047});
    s_dec_pairs++; s_prims += 2;
    if (x1 < x0 || y1 < y0) return;
    {   // queued primitives may still sample or draw the texture that is in this slot now
        u32 p0, np; px_range(CT32, da.fbp, da.fbw, (u32)x0, (u32)y0, (u32)x1, (u32)y1, p0, np);
        if (s_q_n && gs_pages_busy(p0 << 5, 1, np, false)) { gs_sync_reason(0); gs_sync(); }
    }
    const int W = x1 - x0 + 1, H = y1 - y0 + 1;
    std::vector<u32> ref;
    if (s_dec_check) {                                  // the normal rasterizer's result, then put the old pixels back
        gs_sync();
        std::vector<u32> old((size_t)W * H);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) old[(size_t)y * W + x] = s_vram[addr32(da.fbp, da.fbw, (u32)(x0 + x), (u32)(y0 + y))];
        DrawCmd c; c.kind = 3;
        c.d = da; c.v[0] = a0; c.v[1] = a1; exec_cmd(c, 0, 1);
        c.d = db; c.v[0] = vb[0]; c.v[1] = vb[1]; exec_cmd(c, 0, 1);
        ref.resize((size_t)W * H);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            u32 fa = addr32(da.fbp, da.fbw, (u32)(x0 + x), (u32)(y0 + y));
            ref[(size_t)y * W + x] = s_vram[fa]; s_vram[fa] = old[(size_t)y * W + x];
        }
    }
    // Where every target pixel reads its two texels and where it is written depends only on the shape (sizes,
    // buffer widths, texture coordinates) and three base addresses, and VRAM addresses are linear in the base. The
    // game uses a handful of shapes at ever changing places (its texture heap): the three address lists are worked
    // out once per shape, relative to base 0; a pass is then three table look-ups per pixel instead of two
    // texture-coordinate walks and three swizzled address computations (this loop was 15 % of the main thread).
    struct DecKey {
        u32 a_tbp, a_tbw, a_tw, a_th, a_wms, a_wmt, a_minu, a_maxu, a_minv, a_maxv;
        u32 b_tbp, b_tbw, b_tw, b_th, b_wms, b_wmt, b_minu, b_maxu, b_minv, b_maxv;
        u32 fbp, fbw; int x0, y0, x1, y1; float av[8], bv[8];
    };
    struct DecTab { DecKey k; std::vector<u32> ia, ib, it, blocks; u32 id; };   // blocks: the 64-word VRAM blocks the two inputs lie in
    static std::vector<DecTab> tabs;
    DecKey key;
    std::memset(&key, 0, sizeof key);
    key.a_tbw = da.tbw; key.a_tw = da.tw; key.a_th = da.th; key.a_wms = da.wms; key.a_wmt = da.wmt;
    key.a_minu = da.minu; key.a_maxu = da.maxu; key.a_minv = da.minv; key.a_maxv = da.maxv;
    key.b_tbw = db.tbw; key.b_tw = db.tw; key.b_th = db.th; key.b_wms = db.wms; key.b_wmt = db.wmt;
    key.b_minu = db.minu; key.b_maxu = db.maxu; key.b_minv = db.minv; key.b_maxv = db.maxv;
    key.fbw = da.fbw; key.x0 = x0; key.y0 = y0; key.x1 = x1; key.y1 = y1;
    const float avs[8] = {a0.x, a0.y, a0.u, a0.v, a1.x, a1.y, a1.u, a1.v}, bvs[8] = {vb[0].x, vb[0].y, vb[0].u, vb[0].v, vb[1].x, vb[1].y, vb[1].u, vb[1].v};
    std::memcpy(key.av, avs, sizeof avs); std::memcpy(key.bv, bvs, sizeof bvs);
    const DecTab* tab = nullptr;
    for (size_t i = 0; i < tabs.size(); i++) if (!std::memcmp(&tabs[i].k, &key, sizeof key)) { if (i > 4) std::swap(tabs[i], tabs[i / 2]); tab = &tabs[i > 4 ? i / 2 : i]; break; }
    if (!tab) {
        if (tabs.size() >= 64) tabs.erase(tabs.begin() + 32, tabs.end());          // the back half is the less used one
        tabs.emplace_back(); DecTab& t = tabs.back(); t.k = key;
        t.ia.resize((size_t)W * H); t.ib.resize((size_t)W * H); t.it.resize((size_t)W * H);
        const float dxa = a1.x - a0.x, dya = a1.y - a0.y, dxb = vb[1].x - vb[0].x, dyb = vb[1].y - vb[0].y;
        const float idua = dxa != 0 ? (a1.u - a0.u) / dxa : 0, idub = dxb != 0 ? (vb[1].u - vb[0].u) / dxb : 0;
        const int aw = 1 << da.tw, ah = 1 << da.th, bw = 1 << db.tw, bh = 1 << db.th;
        size_t i = 0;
        for (int y = y0; y <= y1; y++) {
            const float fya = dya != 0 ? (y - a0.y) / dya : 0, fyb = dyb != 0 ? (y - vb[0].y) / dyb : 0;
            const int iva = wrap((int)std::floor(a0.v + (a1.v - a0.v) * fya), ah, da.wmt, da.minv, da.maxv);
            const int ivb = wrap((int)std::floor(vb[0].v + (vb[1].v - vb[0].v) * fyb), bh, db.wmt, db.minv, db.maxv);
            float ua = a0.u + (x0 - a0.x) * idua, ub = vb[0].u + (x0 - vb[0].x) * idub;
            for (int x = x0; x <= x1; x++, i++) {
                const int iua = wrap((int)std::floor(ua), aw, da.wms, da.minu, da.maxu), iub = wrap((int)std::floor(ub), bw, db.wms, db.minu, db.maxu);
                t.ia[i] = addr32(0, da.tbw, (u32)iua, (u32)iva);
                t.ib[i] = addr8(0, db.tbw, (u32)iub, (u32)ivb);
                t.it[i] = addr32(0, da.fbw, (u32)x, (u32)y);
                ua += idua; ub += idub;
            }
        }
        {   // the blocks the inputs lie in (relative too; bit 31 marks the second layer's)
            std::vector<u32>& bl = t.blocks; u32 last = ~0u;
            for (u32 a_ : t.ia) if ((a_ & ~63u) != last) { last = a_ & ~63u; if (std::find(bl.begin(), bl.end(), last) == bl.end()) bl.push_back(last); }
            last = ~0u;
            for (u32 b_ : t.ib) { const u32 v_ = ((b_ >> 2) & ~63u) | 0x80000000u; if (v_ != last) { last = v_; if (std::find(bl.begin(), bl.end(), last) == bl.end()) bl.push_back(last); } }
        }
        static u32 next_id = 1; t.id = next_id++; s_dec_tabs++;
        if (s_dec_tabs <= 40 && std::getenv("MVP_DECLOG")) fprintf(stderr, "[dectab] #%llu fbp=%05x fbw=%u rect %d,%d-%d,%d a: tbp=%05x tbw=%u %ux%u b: tbp=%05x tbw=%u %ux%u\n", (unsigned long long)s_dec_tabs, da.fbp, da.fbw, x0, y0, x1, y1, da.tbp, da.tbw, 1u << da.tw, 1u << da.th, db.tbp, db.tbw, 1u << db.tw, 1u << db.th);
        tab = &t;
    }
    const u32* clut = db.clut;
    const u32 fba_a = da.fba ? 0x80000000u : 0, fba_b = db.fba ? 0x80000000u : 0;
    const bool clamp = db.colclamp & 1;
    bool changed = false;
    // The same few dozen textures come round every frame (their slots are shared, so each one has to be unpacked
    // again). What a pass writes depends only on its inputs: the VRAM blocks the two layers lie in and the palette.
    // Results are kept by a hash of those; a hit is confirmed by comparing the inputs themselves, then the kept
    // result is written out instead of being worked out again.
    struct DecOut { u32 tab; std::vector<u32> in, out; };
    static std::unordered_map<u64, DecOut> outs;
    static std::vector<u32> in;
    const size_t n = (size_t)W * H;
    DecOut* hit = nullptr; u64 hkey = 0;
    static const bool no_outs = std::getenv("MVP_NO_DECCACHE") != nullptr;
    if (!no_outs && tab->blocks.size() <= 720) {
        in.resize(tab->blocks.size() * 64 + 256 + 2);
        u32* q = in.data();
        for (u32 b_ : tab->blocks) { std::memcpy(q, &s_vram[((b_ & 0x7fffffffu) + ((b_ >> 31) ? db.tbp : da.tbp) * 64) & 0xfffc0], 64 * 4); q += 64; }
        std::memcpy(q, clut, 256 * 4); q += 256;
        q[0] = fba_a | (fba_b >> 1) | (clamp ? 1u : 0); q[1] = tab->id;
        u64 h0 = 0x9e3779b97f4a7c15ull, h1 = 0xc2b2ae3d27d4eb4full, h2 = 0x165667b19e3779f9ull, h3 = 0x27d4eb2f165667c5ull;
        const u64* w64 = (const u64*)in.data(); const size_t n64 = in.size() / 2;          // in.size() is even
        size_t i = 0;
        for (; i + 4 <= n64; i += 4) {                       // four independent lanes: the multiplies overlap
            h0 = (h0 ^ w64[i]) * 0x100000001b3ull; h1 = (h1 ^ w64[i + 1]) * 0x100000001b3ull;
            h2 = (h2 ^ w64[i + 2]) * 0x100000001b3ull; h3 = (h3 ^ w64[i + 3]) * 0x100000001b3ull;
        }
        for (; i < n64; i++) h0 = (h0 ^ w64[i]) * 0x100000001b3ull;
        hkey = mix64(mix64(h0, h1), mix64(h2, h3)) | 1;
        auto f = outs.find(hkey);
        if (f != outs.end() && f->second.tab == tab->id && f->second.in.size() == in.size() && !std::memcmp(f->second.in.data(), in.data(), in.size() * 4)) hit = &f->second;
    }
    if (hit) {
        s_dec_hits++;
        const u32* it = tab->it.data(); const u32* o = hit->out.data(); const u32 tb = da.fbp * 64;
        for (size_t i = 0; i < n; i++) { u32& px = s_vram[(it[i] + tb) & 0xfffff]; if (px != o[i]) { px = o[i]; changed = true; } }
    } else {
        const u32* ia = tab->ia.data(); const u32* ib = tab->ib.data(); const u32* it = tab->it.data();
        DecOut* keep = nullptr;
        if (hkey) {
            static size_t kept_bytes = 0;                    // at most 64 MB of kept results
            if (kept_bytes > (64u << 20)) { outs.clear(); kept_bytes = 0; }
            kept_bytes += (in.size() + n) * 4;
            keep = &outs[hkey]; keep->tab = tab->id; keep->in = in; keep->out.resize(n);
        }
        u32* ko = keep ? keep->out.data() : nullptr;
        const u32 ab = da.tbp * 64, bb = db.tbp * 256, tb = da.fbp * 64;
        for (size_t i = 0; i < n; i++) {
            const u32 base = s_vram[(ia[i] + ab) & 0xfffff] | fba_a;
            const u32 tc = clut[s_vram8[(ib[i] + bb) & 0x3fffff]];
            u32 r = (base & 0xff) + (tc & 0xff), g = ((base >> 8) & 0xff) + ((tc >> 8) & 0xff), bl = ((base >> 16) & 0xff) + ((tc >> 16) & 0xff);
            if (clamp) { r = std::min<u32>(r, 255); g = std::min<u32>(g, 255); bl = std::min<u32>(bl, 255); } else { r &= 255; g &= 255; bl &= 255; }
            const u32 col = r | (g << 8) | (bl << 16) | (tc & 0xff000000u) | fba_b;
            const u32 nw = (base & 0xfc000000u) | (col & 0x03ffffffu);
            if (ko) ko[i] = nw;
            u32& px = s_vram[(it[i] + tb) & 0xfffff];
            if (px != nw) { px = nw; changed = true; }
        }
    }
    if (s_dec_check) {
        u32 bad = 0;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) bad += s_vram[addr32(da.fbp, da.fbw, (u32)(x0 + x), (u32)(y0 + y))] != ref[(size_t)y * W + x];
        if (bad) { if (s_dec_bad++ < 20) fprintf(stderr, "[deccheck] f=%llu fbp=%05x %dx%d: %u pixels differ\n", (unsigned long long)g_gs_frame, da.fbp, W, H, bad); }
    }
    // texture caches: a new generation only if the content changed (or the pages still count as a render target)
    bool drawn = false;
    tc_rect_pages(da.fbp, da.fbw, CT32, x0, y0, x1, y1, [&](u32 p) { drawn |= s_page_drawn[p]; });
    if (changed) s_dec_px++;
    if (changed || drawn) {
        s_dec_changed++; s_vram_gen_fwd();
        tc_rect_pages(da.fbp, da.fbw, CT32, x0, y0, x1, y1, [&](u32 p) { s_page_gen[p] = ++s_gen_counter; s_page_drawn[p] = false; upext_pages_written(p, 1); });
        s_prep_epoch++;
    }
}
// true = the sprite was taken (held back, or the pair was unpacked)
static inline bool dec_take(const DrawState& d, const Vtx* v, bool a_cand, bool droppable) {
    if (s_dec.pending) { dec_run(d, v); return true; }             // submit() made sure this is the matching second sprite
    if (a_cand) { s_dec.pending = true; s_dec.droppable = droppable; s_dec.d = d; s_dec.v[0] = v[0]; s_dec.v[1] = v[1]; return true; }
    return false;
}
static u32 s_skip_main = ~0u;                           // main target as seen while skipping (hr_track only runs on drawn frames)
// A skipped frame is recorded (fs_*, below). If it turns out to copy the picture into texture memory, it is run
// again from the start with drawing on.
static bool s_fs_active = false, s_fs_request = false, s_fs_resim = false;
static u32 s_pic_zbp = ~0u, s_pic_fbw = 8;              // Z buffer and width of the main target, from draws into it
static inline u32 pic_main() { return s_hr_tbp != ~0u ? s_hr_tbp : s_skip_main; }   // the drawn frames' knowledge wins
static u64 s_fs_cut_frame = ~0ull;                      // frame of the last copy of the picture's colour into texture memory
// 1 = the texture is the main target's colour, 2 = its Z buffer, 0 = neither
static inline int tex_in_picture(u32 tbp) {
    const u32 main = pic_main();
    if (main == ~0u) return 0;
    u32 size = std::max<u32>(s_pic_fbw, 1) * 14 * 32;                    // 448 rows, in blocks
    if (s_pic_zbp != ~0u && s_pic_zbp > main && s_pic_zbp - main < size) size = s_pic_zbp - main;   // the Z buffer follows the colour buffer
    return tbp - main < size ? 1 : (s_pic_zbp != ~0u && tbp - s_pic_zbp < size) ? 2 : 0;
}
static inline bool fb_is_display(u32 fbp) { u64 t = s_disp_seen[fbp]; return t && g_gs_frame + 1 - t < 600; }
static inline bool fb_not_picture(u32 fbp) {            // not the main target and not a recent display buffer
    return fbp * 32 != pic_main() && !fb_is_display(fbp);
}
// ---------------------------------------------------------------- depth-of-field backdrop
// In batter walk-up shots the game blurs the background: it writes a blur amount per pixel into the main target's
// alpha (sprites at several depths), then blends a half-size copy of the picture over it with that alpha
// ((Cs - Cd) * Ad + Cd, 16 column sprites, a 32-bit 256x256 texture at the start of the dynamic texture area).
// The draw that should fill that texture never reaches the GS here (the only copies of the picture into that
// buffer are the grabs at camera cuts, about every 90 frames, and those are upside down relative to the layer),
// so the layer sampled whatever textures had been uploaded to that address since: coloured noise over the
// background. Why the game's own copy is missing is not known. The copy is made here instead, right before the
// layer: the picture at half size, bilinear, placed so that the layer's texture coordinates line up with it.
static u64 s_dof_grabs = 0, s_dof_dropped = 0;
static bool s_layer_run = false;
static const bool s_dof_off = std::getenv("MVP_NO_DOF_FIX") != nullptr;
static bool s_replaying = false;
static void submit(const DrawState& d, u8 kind, const Vtx* v, int nv);
static inline bool dof_is_layer(const DrawState& d) {     // a sprite of the blur layer
    return d.tme && d.abe && d.tpsm == CT32 && d.tbw == 4 && d.tbp >= 0x3000 && (d.alpha & 0xff) == 0x54 && d.fbp == pic_main() && !d.hr;
}
static bool dof_grab(const DrawState& layer, const Vtx* lv) {
    const float tw = (float)(1 << layer.tw), th = (float)(1 << layer.th);
    float u0, v0, u1, v1;                                  // the layer's texel coordinates at its two corners
    if (layer.fst) { u0 = lv[0].u; v0 = lv[0].v; u1 = lv[1].u; v1 = lv[1].v; }
    else { if (lv[0].q == 0 || lv[1].q == 0) return false; u0 = lv[0].s / lv[0].q * tw; v0 = lv[0].t / lv[0].q * th; u1 = lv[1].s / lv[1].q * tw; v1 = lv[1].t / lv[1].q * th; }
    const float dx = lv[1].x - lv[0].x, dy = lv[1].y - lv[0].y;
    if (dx == 0 || dy == 0 || u1 == u0 || v1 == v0) return false;
    const float ku = (u1 - u0) / dx, kv = (v1 - v0) / dy;   // texels per screen pixel (0.5)
    DrawState g = layer;
    g.tex.reset(); g.hi.reset(); g.clut = nullptr; g.hr = false;
    g.fbp = layer.tbp; g.fbw = layer.tbw; g.fpsm = CT32; g.fbmsk = 0; g.fba = 0;
    g.zbp = 0; g.zmsk = 1; g.test = 0;
    g.sx0 = 0; g.sy0 = 0; g.sx1 = (int)tw - 1; g.sy1 = (int)th - 1;
    g.tme = true; g.fst = true; g.abe = false; g.fge = false; g.iip = false;
    g.tbp = layer.fbp; g.tbw = layer.fbw; g.tpsm = CT32; g.tw = 9; g.th = 9; g.tcc = 1; g.tfx = 1;      // the main target, DECAL
    g.wms = g.wmt = 1; g.bilinear = true; g.alpha = 0;
    // buffer pixel X (centre X + 0.5) is what the layer shows at screen x = x0 + (X + 0.5 - u0) / ku
    Vtx v[2] = {lv[0], lv[1]};
    v[0].x = 0; v[0].y = 0; v[1].x = tw; v[1].y = th;
    v[0].u = lv[0].x + (0.5f - u0) / ku + 0.5f; v[1].u = v[0].u + tw / ku;
    v[0].v = lv[0].y + (0.5f - v0) / kv + 0.5f; v[1].v = v[0].v + th / kv;
    for (Vtx& k : v) { k.z = 0; k.r = k.g = k.b = k.a = 128; k.q = 1; k.f = 0; }
    submit(g, 3, v, 2);
    s_dof_grabs++;
    return true;
}
static bool s_kick_a = false, s_kick_drop = false;      // from vertex_kick: this sprite may start a decompression / belongs to a skipped frame

static void submit(const DrawState& d, u8 kind, const Vtx* v, int nv) {
    if (s_replaying) {                                   // MVP_REPLAY_PRIMS=a,b: draw only primitives a..b-1 of a replay (which one draws that?)
        static const char* e = std::getenv("MVP_REPLAY_PRIMS"); static u64 n = 0, lo = 0, hi = ~0ull;
        static bool init = false; if (!init) { init = true; if (e) { unsigned long long a = 0, b = ~0ull; sscanf(e, "%llu,%llu", &a, &b); lo = a; hi = b; } }
        const u64 k = n++; if (k < lo || k >= hi) return;
    }
    if (s_dec.pending && !(kind == 3 && dec_is_b(d, v))) dec_flush();

    hr_track(d, kind, v);
    if (s_matdbg_gs && (long long)g_gs_frame >= std::atoll(std::getenv("MVP_MATDBG")) && kind == 2 && std::fabs(v[0].x - v[1].x) + std::fabs(v[1].y - v[2].y) > 20 && v[0].q < 0 && v[1].q < 0 && v[2].q < 0 && s_matdbg_n < 12) {
        extern float g_vu1_dbg_rows[6][4]; extern u32 g_vu1_cur_start;
        s_matdbg_n++;
        fprintf(stderr, "[behind] f=%llu vu1pc=%04x tex=%05x q=%g,%g,%g xy=(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)\n", (unsigned long long)g_gs_frame, g_vu1_cur_start, d.tbp,
                v[0].q, v[1].q, v[2].q, v[0].x, v[0].y, v[1].x, v[1].y, v[2].x, v[2].y);
        for (int r = 0; r < 6; r++) fprintf(stderr, "    row%d %12.4f %12.4f %12.4f %12.4f\n", r, g_vu1_dbg_rows[r][0], g_vu1_dbg_rows[r][1], g_vu1_dbg_rows[r][2], g_vu1_dbg_rows[r][3]);
    }
    if (gpu_main(d)) {                                   // main target: GPU only
        if (s_gpu_stale) gpu_resync();
        const bool zw = (d.test & (1 << 16)) && !d.zmsk;
        if (d.test & (1 << 16)) { s_gz_bp = d.zbp; s_gz_psm = d.zpsm; }
        tex_read_check(d);
        DrawState h = d; gpu_submit(h, kind, v, nv);
        s_prims++; s_vram_gen_fwd();
        int x0, y0, x1, y1; prim_rect(d, v, nv, x0, y0, x1, y1);
        if (x1 >= x0 && y1 >= y0) {
            auto own = [&](u32 bp, u32 psm, bool color) {
                rect_pages(bp, d.fbw, psm, x0, y0, x1, y1, [&](u32 p) {
                    bit_set(s_own, p); bit_set(s_gpu_touched, p);   // generations are bumped when the pages are downloaded
                    if (color) { bit_set(s_own_c, p); bit_set(s_touched_c, p); } });
            };
            own(d.fbp, d.fpsm, true);
            if (zw) own(d.zbp, d.zpsm, s_gz_psm != Z24);
            s_own_any = true;
        }
        return;
    }
    if (s_gpu_on) {
        // the final copy of the main target to the display buffer: the window shows the GPU picture instead
        if (kind == 3 && d.tme && d.tbp == s_hr_tbp && d.tpsm == CT32 && d.fbp == display_fbp() && s_hr_valid) {
            if (s_lazy.size() > 64) s_lazy.clear();
            LazyCopy c; c.d = d; c.kind = kind; c.nv = nv; for (int i = 0; i < nv; i++) c.v[i] = v[i];
            if (s_lazy.empty()) s_lazy_ct24 = true;
            if (d.fpsm != CT24 || d.fbmsk) s_lazy_ct24 = false;
            s_lazy.push_back(c);
            int x0, y0, x1, y1; prim_rect(d, v, nv, x0, y0, x1, y1);
            rect_pages(d.fbp, d.fbw, d.fpsm, x0, y0, x1, y1, [&](u32 p) { bit_set(s_lazy_pages, p); });
            s_lazy_any = true;
            return;
        }
        {
            tex_read_check(d);
            int x0, y0, x1, y1; prim_rect(d, v, nv, x0, y0, x1, y1);
            if (x1 >= x0 && y1 >= y0) {
                if (s_lazy_any && ((!(top_byte_psm(d.fpsm) && s_lazy_ct24) && rect_hits(s_lazy_pages, d.fbp, d.fbw, d.fpsm, x0, y0, x1, y1)) ||
                    ((d.test & (1 << 16)) && !d.zmsk && rect_hits(s_lazy_pages, d.zbp, d.fbw, d.zpsm, x0, y0, x1, y1)))) lazy_realize("software draw");
                const bool top = top_byte_psm(d.fpsm) && s_gz_psm == Z24;
                bool hit_own = s_own_any && (rect_hits(top ? s_own_c : s_own, d.fbp, d.fbw, d.fpsm, x0, y0, x1, y1) ||
                               ((d.test & (1 << 16)) && !d.zmsk && rect_hits(s_own, d.zbp, d.fbw, d.zpsm, x0, y0, x1, y1)));
                if (hit_own) own_download("software draw");
                if (rect_hits(top ? s_touched_c : s_gpu_touched, d.fbp, d.fbw, d.fpsm, x0, y0, x1, y1) ||
                    ((d.test & (1 << 16)) && !d.zmsk && rect_hits(s_gpu_touched, d.zbp, d.fbw, d.zpsm, x0, y0, x1, y1))) {
                    if (s_ownlog && !s_gpu_stale && s_ownlog_n++ < 200) fprintf(stderr, "[own] f=%llu stale: software draw fbp=%05x fbw=%u psm=%02x zbp=%05x\n", (unsigned long long)g_gs_frame, d.fbp, d.fbw, d.fpsm, d.zbp);
                    s_gpu_stale = true;
                }
            }
        }
        if (kind == 3 && (s_dec.pending || s_kick_a) && dec_take(d, v, s_kick_a, s_kick_drop)) return;
        submit_one(d, kind, v, nv);
        return;
    }
    if (kind == 3 && (s_dec.pending || s_kick_a) && dec_take(d, v, s_kick_a, s_kick_drop)) return;
    submit_one(d, kind, v, nv);
    if (s_hr_scale > 1 && d.fbp == s_hr_tbp && d.fpsm == CT32 && d.fbw == s_hr_tbw) {
        const float S = (float)s_hr_scale;
        DrawState h = d; h.hr = true;
        h.sx0 = d.sx0 * s_hr_scale; h.sy0 = d.sy0 * s_hr_scale;
        h.sx1 = std::min((d.sx1 + 1) * s_hr_scale - 1, s_hr_w - 1); h.sy1 = std::min((d.sy1 + 1) * s_hr_scale - 1, s_hr_h - 1);
        Vtx w[3];
        for (int i = 0; i < nv; i++) { w[i] = v[i]; w[i].x = v[i].x * S; w[i].y = v[i].y * S; }
        submit_one(h, kind, w, nv);
    }
}
static void submit_one(const DrawState& d, u8 kind, const Vtx* v, int nv) {
    if (!d.hr) s_prims++;
    s_vram_gen_fwd();
    if (s_bigdbg >= 0 && kind == 2 && !d.hr) {
        extern u32 g_vu1_cur_start;
        float x0 = std::min({v[0].x, v[1].x, v[2].x}), x1 = std::max({v[0].x, v[1].x, v[2].x});
        float y0 = std::min({v[0].y, v[1].y, v[2].y}), y1 = std::max({v[0].y, v[1].y, v[2].y});
        auto inside = [&](float px, float py) {
            auto e = [&](const Vtx& p, const Vtx& q) { return (q.x - p.x) * (py - p.y) - (px - p.x) * (q.y - p.y); };
            float a = e(v[0], v[1]), b = e(v[1], v[2]), c2 = e(v[2], v[0]);
            return (a >= 0 && b >= 0 && c2 >= 0) || (a <= 0 && b <= 0 && c2 <= 0);
        };
        bool hit = s_probe_x >= 0 ? inside((float)s_probe_x, (float)s_probe_y) && d.fbp == 0x1180 : (x1 - x0) * (y1 - y0) > 200000;
        if ((long long)g_gs_frame >= s_bigdbg && hit && d.clut && s_bigdbg_n < 60)
            fprintf(stderr, "[bigtri]   cbp=%05x cpsm=%u csa=%u tw=%u th=%u tbw=%u rgba=%.0f,%.0f,%.0f,%.0f clut[0..3]=%08x %08x %08x %08x texel(0,0)=%u texel(mid)=%u\n",
                    d.cbp, d.cpsm, d.csa, d.tw, d.th, d.tbw, v[0].r, v[0].g, v[0].b, v[0].a, d.clut[0], d.clut[1], d.clut[2], d.clut[3],
                    read_px(d.tpsm, d.tbp, d.tbw, 0, 0), read_px(d.tpsm, d.tbp, d.tbw, (1u << d.tw) / 2, (1u << d.th) / 2));
        if ((long long)g_gs_frame >= s_bigdbg && hit && s_bigdbg_n++ < 60)
            fprintf(stderr, "[bigtri] f=%llu vu1pc=%04x fb=%05x tex=%05x psm=%02x abe=%d alpha=%x test=%x tfx=%u tcc=%u (%.1f,%.1f,%.0f q=%g) (%.1f,%.1f,%.0f q=%g) (%.1f,%.1f,%.0f q=%g)\n",
                    (unsigned long long)g_gs_frame, g_vu1_cur_start, d.fbp, d.tbp, d.tpsm, (int)d.abe, d.alpha, d.test, d.tfx, d.tcc, v[0].x, v[0].y, v[0].z, v[0].q, v[1].x, v[1].y, v[1].z, v[1].q, v[2].x, v[2].y, v[2].z, v[2].q);
    }
    if (!d.hr) {                                     // texture cache: pages this primitive renders to
        float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
        for (int i = 1; i < nv; i++) { x0 = std::min(x0, v[i].x); x1 = std::max(x1, v[i].x); y0 = std::min(y0, v[i].y); y1 = std::max(y1, v[i].y); }
        int ix0 = std::max((int)x0, d.sx0), ix1 = std::min((int)x1 + 1, d.sx1), iy0 = std::max((int)y0, d.sy0), iy1 = std::min((int)y1 + 1, d.sy1);
        if (ix1 >= ix0 && iy1 >= iy0) {
            auto bump = [&](u32 bp, u32 psm) {
                u32 pw = tc_page_w(psm), ph = tc_page_h(psm), bwp = std::max<u32>((d.fbw * 64 + pw - 1) / pw, 1);
                for (u32 py = (u32)iy0 / ph; py <= (u32)iy1 / ph; py++)
                    for (u32 px = (u32)ix0 / pw; px <= (u32)ix1 / pw; px++) {
                        u32 p = ((bp >> 5) + py * bwp + px) & 511;
                        { if (!s_page_drawn[p]) s_prep_epoch++; s_page_gen[p] = ++s_gen_counter; s_page_drawn[p] = true; upext_pages_written(p, 1); }
                    }
            };
            bump(d.fbp, d.fpsm);
            if ((d.test & (1 << 16)) && !d.zmsk) bump(d.zbp, d.zpsm);
        }
    }
    if (!s_nthreads) {                               // inline rasterization
        DrawCmd c; c.d = d; c.kind = kind; for (int i = 0; i < nv; i++) c.v[i] = v[i];
        u64 t0 = prof_time_ns();
        exec_cmd(c, 0, 1);
        s_raster_ns += prof_time_ns() - t0;
        return;
    }
    if (d.tme && !d.hr) {                            // render-to-texture hazard (the HR copy was checked already)
        u32 w = 1u << d.tw, h = 1u << d.th;
        u32 bwp = std::max<u32>((std::max<u32>(d.tbw * 64, w) + page_w(d.tpsm) - 1) / page_w(d.tpsm), 1);
        u32 trows = (h + page_h(d.tpsm) - 1) / page_h(d.tpsm) + 1;
        if (pages_dirty(d.tbp, bwp, trows)) {
            if (s_gshaz && s_gshaz_n < 40 && d.tbp != s_gshaz_tbp) { s_gshaz_n++; s_gshaz_tbp = d.tbp;
                { int fd = -1; for (u32 k = 0; k < bwp * trows && fd < 0; k++) { u32 p = ((d.tbp >> 5) + k) & 511; if (s_dirty[p >> 6] & (1ull << (p & 63))) fd = (int)p; } fprintf(stderr, "[gs-haz] tex tbp=%05x tbw=%u psm=%02x %ux%u (pages %u..+%u, first dirty %d) fb=%05x fbw=%u zbp=%05x\n", d.tbp, d.tbw, d.tpsm, 1u << d.tw, 1u << d.th, d.tbp >> 5, bwp * trows, fd, d.fbp, d.fbw, d.zbp); } }
            s_sync_reason = 2; gs_sync();
        }
        mark_read(d.tbp, bwp, trows);
    }
    if (s_q_n >= QCAP) { s_sync_reason = 4; gs_sync(); }
    DrawCmd& c = s_q[s_q_n];
    c.d = d; c.kind = kind; for (int i = 0; i < nv; i++) c.v[i] = v[i];
    s_q_n++;
    s_q_pub.store(((u64)s_q_epoch << 32) | s_q_n, std::memory_order_release);
    if ((s_q_n & 63) == 1) s_q_cv.notify_all();
    if (d.hr) return;                                // HR buffers are never sampled by the GS
    // pages actually covered by the primitive's bounding box (clipped to the scissor)
    float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
    for (int i = 1; i < nv; i++) { x0 = std::min(x0, v[i].x); x1 = std::max(x1, v[i].x); y0 = std::min(y0, v[i].y); y1 = std::max(y1, v[i].y); }
    int ix0 = std::max((int)x0, d.sx0), ix1 = std::min((int)x1 + 1, d.sx1), iy0 = std::max((int)y0, d.sy0), iy1 = std::min((int)y1 + 1, d.sy1);
    if (ix1 < ix0 || iy1 < iy0) return;
    mark_rect(d.fbp, d.fbw, d.fpsm, ix0, iy0, ix1, iy1);
    if ((d.test & (1 << 16)) && !d.zmsk) mark_rect(d.zbp, d.fbw, d.zpsm, ix0, iy0, ix1, iy1);
}

// May a skipped frame drop this primitive? Only if it draws into the picture. Everything else goes into texture
// memory, which later frames may rely on (anything the game renders once and keeps), and is cheap by comparison.
// MVP_SKIP_STREAK=n also drops geometry (not sprites) of targets drawn n frames in a row (shadow maps).
static inline bool skippable_target(u32 fbp, u32 prim) {
    static const bool all = std::getenv("MVP_SKIP_ALL") != nullptr;     // old behaviour (debugging)
    static const u32 streak = std::getenv("MVP_SKIP_STREAK") ? (u32)std::atoi(std::getenv("MVP_SKIP_STREAK")) : 0;
    if (all || !fb_not_picture(fbp)) return true;
    return streak && prim != 6 && s_tgt_streak[fbp] >= streak;
}
// cheap test on the registers: could this sprite be one of the two that unpack a texture? (dec_is_a / dec_is_b decide)
static inline bool dec_sig_regs(const Ctx& c, u32 attr) {
    if ((attr & (16 | 32 | 256)) != (16 | 256) || (u32)c.test != 0 || bits(c.frame, 24, 6) != CT32) return false;
    if (bits(c.tex0, 35, 2) != 1 || !bits(c.tex0, 34, 1)) return false;
    const u32 fbmsk = (u32)(c.frame >> 32), tpsm = bits(c.tex0, 20, 6);
    return (attr & 64) ? (fbmsk == 0xfc000000u && tpsm == T8) : (fbmsk == 0 && tpsm == CT32);
}
static const int s_dbg_fbp = std::getenv("MVP_GSLOG") ? (int)std::strtol(std::getenv("MVP_GSLOG"), nullptr, 16) : -1;
static const u64 s_dbg_from = std::getenv("MVP_GSLOG_FROM") ? std::strtoull(std::getenv("MVP_GSLOG_FROM"), nullptr, 10) : 0;
float g_hud_k = 1.0f;                    // ultrawide HUD squeeze (settings_apply): 1 = as the game draws it
static void vertex_kick(u64 xyz, u32 z, u32 f, bool draw) {
    const Ctx& c = R.c[((((R.prmodecont & 1) ? (u32)R.prim : (u32)R.prmode) >> 9) & 1)];
    Vtx v;
    v.x = ((float)(u16)(xyz & 0xffff) - (float)bits(c.xyoffset, 0, 16)) / 16.0f;
    v.y = ((float)(u16)((xyz >> 32) & 0xffff) - (float)bits(c.xyoffset, 32, 16)) / 16.0f;
    v.z = (float)z;
    v.r = (float)bits(R.rgbaq, 0, 8); v.g = (float)bits(R.rgbaq, 8, 8); v.b = (float)bits(R.rgbaq, 16, 8); v.a = (float)bits(R.rgbaq, 24, 8);
    float s, t; u32 si = (u32)R.st, ti = (u32)(R.st >> 32);
    std::memcpy(&s, &si, 4); std::memcpy(&t, &ti, 4);
    u32 qi = (u32)(R.rgbaq >> 32); std::memcpy(&v.q, &qi, 4);
    v.s = s; v.t = t;
    v.u = bits(R.uv, 0, 14) / 16.0f; v.v = bits(R.uv, 16, 14) / 16.0f;
    v.f = (float)f;
    u32 prim = bits(R.prim, 0, 3);
    static const int need[8] = {1, 2, 2, 3, 3, 3, 2, 1};
    if (s_vcount < 3) s_vq[s_vcount++] = v;
    s_vtotal++;
    if (s_vcount < need[prim]) return;
    if (draw) {                            // in how many frames in a row has this target been drawn into?
        u32 fb = bits(c.frame, 0, 9); u64 fr = g_gs_frame + 1;
        if (s_tgt_last[fb] != fr) { s_tgt_streak[fb] = s_tgt_last[fb] + 1 == fr ? (u16)std::min<u32>(s_tgt_streak[fb] + 1u, 60000u) : 0; s_tgt_last[fb] = fr; }
    }
    {   // MVP_DECLOG: one line per frame and target for texture-decompression sprites (FBMSK fc000000)
        static const bool declog = std::getenv("MVP_DECLOG") != nullptr;
        if (declog && draw && prim == 6 && (u32)(c.frame >> 32) == 0xfc000000u) {
            static u64 lf = ~0ull; static u32 lfb = ~0u; u32 fb = bits(c.frame, 0, 9);
            if (lf != g_gs_frame || lfb != fb) { lf = g_gs_frame; lfb = fb; fprintf(stderr, "[dec] frame %llu fbp=%x fbw=%u streak=%u skip=%d\n", (unsigned long long)g_gs_frame, fb * 32, (u32)bits(c.frame, 16, 6), s_tgt_streak[fb], (int)s_skip_draw); }
        }
    }
    // a held decompression sprite must be drawn before anything else looks at VRAM (palettes, texture decoding)
    if (draw && s_dec.pending && !(prim == 6 && (u32)(c.frame >> 32) == 0xfc000000u && bits(c.frame, 0, 9) * 32 == s_dec.d.fbp)) dec_flush();
    // A skipped frame drops what is drawn into the picture (the display buffers and the main target that is copied
    // to them). Draws into texture memory still run: later frames may rely on them.
    bool droppable = false, dec_cand = false;
    if (draw && s_skip_draw) {
        const u32 attr = (R.prmodecont & 1) ? (u32)R.prim : (u32)R.prmode;
        const u32 fb = bits(c.frame, 0, 9);
        // nothing has been drawn yet (headless runs): the full-screen copy to the display names the main target
        if (s_hr_tbp == ~0u && prim == 6 && (attr & 16) && bits(c.tex0, 20, 6) == CT32 && bits(c.tex0, 14, 6) >= 8 && fb_is_display(fb) &&
            bits(c.tex0, 0, 14) != fb * 32 && std::fabs(s_vq[1].x - s_vq[0].x) >= 400)
            { if (s_skip_main != bits(c.tex0, 0, 14)) rlog("gs", "main target (seen while skipping): %05x", (u32)bits(c.tex0, 0, 14)); s_skip_main = bits(c.tex0, 0, 14); }
        droppable = skippable_target(fb, prim);
        if (fb * 32 == pic_main()) { s_pic_zbp = bits(c.zbuf, 0, 9) * 32; s_pic_fbw = bits(c.frame, 16, 6); }
        // texture memory filled from the picture (the backdrop of a close-up is a copy of the main target, masked
        // with the top byte of its Z buffer): this frame needs its picture after all
        // (the Z buffer's top byte is also sampled every frame for the shadows, which are redone every frame: that
        // only counts in the frames right after a colour copy, while the backdrop is being built)
        static const u64 cut_frames = std::getenv("MVP_UNSKIP_AFTER") ? (u64)std::atoll(std::getenv("MVP_UNSKIP_AFTER")) : 30;
        const int pic = (!droppable && (attr & 16)) ? tex_in_picture(bits(c.tex0, 0, 14)) : 0;
        if (pic == 1) { s_fs_cut_frame = g_gs_frame; if (s_vwatch_cut >= 0) s_vwatch_until = g_gs_frame + (u64)s_vwatch_cut; }
        if (s_fs_active && (pic == 1 || (pic == 2 && g_gs_frame - s_fs_cut_frame <= cut_frames))) {
            s_fs_request = true;
            static const bool fslog = std::getenv("MVP_UNSKIPLOG") != nullptr; static int n = 0;
            if (fslog && n++ < 200) fprintf(stderr, "[unskip] f=%llu prim=%u fbp=%05x tbp=%05x tpsm=%02x main=%05x zbp=%05x fbw=%u\n", (unsigned long long)g_gs_frame, prim, fb * 32,
                                            (u32)bits(c.tex0, 0, 14), (u32)bits(c.tex0, 20, 6), s_hr_tbp != ~0u ? s_hr_tbp : s_skip_main, s_pic_zbp, s_pic_fbw);
        }
        if (s_fs_request) droppable = true;             // the rest of this packet is thrown away with the frame
        dec_cand = prim == 6 && !s_dec_off && !droppable && dec_sig_regs(c, attr);
        if (!droppable && !dec_cand) { s_rtt_in_skipped++; s_rtt_by_fbp[bits(c.frame, 0, 9)]++; }
    }
    {   // MVP_PICSRC: log draws into texture memory that sample the picture (main target, its Z buffer, the display)
        static const bool picsrc = std::getenv("MVP_PICSRC") != nullptr;
        if (picsrc && draw) {
            const u32 attr = (R.prmodecont & 1) ? (u32)R.prim : (u32)R.prmode; const u32 fb = bits(c.frame, 0, 9);
            if ((attr & 16) && fb * 32 == pic_main() && bits(c.tex0, 20, 6) == CT32 && bits(c.tex0, 0, 14) >= 0x2d80) {   // the picture drawn from a 32-bit buffer in texture memory
                static u64 lf2 = ~0ull; static u32 lt2 = ~0u; const u32 tbp = bits(c.tex0, 0, 14);
                static const bool recdof = std::getenv("MVP_GSREC_DOF") != nullptr;
                static const long long recdof_after = std::getenv("MVP_GSREC_DOF") ? std::atoll(std::getenv("MVP_GSREC_DOF")) : 20;   // frames after the layer first appears
                if (recdof && s_rec_request < 0 && g_gs_frame >= 3700 && tbp == 0x3280 && bits(c.tex0, 14, 6) == 4) s_rec_request = (long long)g_gs_frame + recdof_after;
                if (lf2 != g_gs_frame || lt2 != tbp) { lf2 = g_gs_frame; lt2 = tbp;
                    fprintf(stderr, "[picuse] f=%llu skip=%d prim=%u tbp=%05x tbw=%u %ux%u abe=%d alpha=%llx test=%x\n", (unsigned long long)g_gs_frame, (int)s_skip_draw, prim, tbp, (u32)bits(c.tex0, 14, 6),
                            1u << bits(c.tex0, 26, 4), 1u << bits(c.tex0, 30, 4), (int)!!(attr & 64), (unsigned long long)c.alpha, (u32)c.test); }
            }
            if (bits(c.frame, 16, 6) == 4 && fb * 32 >= 0x3000 && fb_not_picture(fb)) {      // draws into a 256-wide buffer in texture memory
                static u64 lf3 = ~0ull; static u64 lk3 = ~0ull; const u64 key = ((u64)fb << 40) | ((u64)bits(c.tex0, 0, 14) << 8) | prim | ((attr & 16) << 4);
                if (lf3 != g_gs_frame || lk3 != key) { lf3 = g_gs_frame; lk3 = key;
                    fprintf(stderr, "[bd] f=%llu skip=%d prim=%u fbp=%05x tme=%d tbp=%05x tpsm=%02x fbmsk=%08x abe=%d alpha=%llx test=%x\n", (unsigned long long)g_gs_frame, (int)s_skip_draw, prim, fb * 32,
                            (int)!!(attr & 16), (u32)bits(c.tex0, 0, 14), (u32)bits(c.tex0, 20, 6), (u32)(c.frame >> 32), (int)!!(attr & 64), (unsigned long long)c.alpha, (u32)c.test); }
            }
            if ((attr & 16) && fb_not_picture(fb)) {
                const u32 tbp = bits(c.tex0, 0, 14), main = s_hr_tbp != ~0u ? s_hr_tbp : s_skip_main;
                static u32 zb = ~0u; static u64 lf = ~0ull; static u32 lt = ~0u;
                if (!fb_not_picture(bits(c.frame, 0, 9))) {}
                // the Z buffer of the main target: remembered from draws into it
                const bool pic = (main != ~0u && tbp >= main && tbp < main + 0x1180) || fb_is_display(tbp >> 5) || (zb != ~0u && tbp >= zb && tbp < zb + 0x1180);
                (void)zb;
                if (pic && (lf != g_gs_frame || lt != tbp)) { lf = g_gs_frame; lt = tbp;
                    fprintf(stderr, "[picsrc] f=%llu skip=%d prim=%u fbp=%05x fbw=%u tbp=%05x tbw=%u tpsm=%02x %ux%u abe=%d test=%x fbmsk=%x\n", (unsigned long long)g_gs_frame, (int)s_skip_draw, prim,
                            fb * 32, (u32)bits(c.frame, 16, 6), tbp, (u32)bits(c.tex0, 14, 6), (u32)bits(c.tex0, 20, 6), 1u << bits(c.tex0, 26, 4), 1u << bits(c.tex0, 30, 4), (int)!!(attr & 64), (u32)c.test, (u32)(c.frame >> 32)); }
            }
        }
    }
    if (s_vwatch_lo != ~0u && draw && (s_vwatch_cut < 0 || g_gs_frame <= s_vwatch_until)) {      // MVP_VWATCH: draws whose target starts in (or up to 0x400 blocks before) the watched range
        const u32 fbp = bits(c.frame, 0, 9) * 32;
        static const bool vwatch_big = std::getenv("MVP_VWATCH_BIG") != nullptr;
        if (fbp + 0x400 > s_vwatch_lo && fbp < s_vwatch_hi && !vwatch_big) {
            static u64 lf = ~0ull; static u32 lb = ~0u, lm = 0; const u32 msk = (u32)(c.frame >> 32);
            if (lf != g_gs_frame || lb != fbp || lm != msk) { lf = g_gs_frame; lb = fbp; lm = msk;
                fprintf(stderr, "[vwatch] f=%llu draw prim=%u fbp=%05x fbw=%u fbmsk=%08x scis=%u,%u-%u,%u skip=%d\n", (unsigned long long)g_gs_frame, prim, fbp, (u32)bits(c.frame, 16, 6), msk,
                        (u32)bits(c.scissor, 0, 11), (u32)bits(c.scissor, 32, 11), (u32)bits(c.scissor, 16, 11), (u32)bits(c.scissor, 48, 11), (int)s_skip_draw); }
        }
    }
    auto drop = [&] {
        switch (prim) { case 2: s_vq[0] = s_vq[1]; s_vcount = 1; break; case 4: s_vq[0] = s_vq[1]; s_vq[1] = s_vq[2]; s_vcount = 2; break;
                        case 5: s_vq[1] = s_vq[2]; s_vcount = 2; break; default: s_vcount = 0; }
    };
    if (droppable && !dec_cand) { drop(); return; }
    // Triangles (all the 3D models): the draw state, its palette and its texture are prepared once and kept for as
    // long as no state register is written and nothing they were derived from changes -- a strip of a player
    // model is 10-30 triangles with the same state, and preparing it cost more than queueing the triangle.
    static DrawState kept; static u64 kept_epoch = 0; static u32 kept_q = ~0u; static bool kept_tex = false;
    static const bool keep_off = std::getenv("MVP_NO_PREP") != nullptr;
    DrawState local;
    const bool keep = prim >= 3 && prim <= 5 && !keep_off;
    if (keep) {
        s_prep_tris++;
        if (s_state_dirty || kept_epoch != s_prep_epoch || kept_q != gs_epoch()) {
            s_prep_n[s_state_dirty ? 0 : kept_epoch != s_prep_epoch ? 1 : 2]++;
            kept.tex.reset(); kept.hi.reset(); kept.clut_serial = 0;
            setup(kept);
            if (kept.tme && (kept.tpsm == T8 || kept.tpsm == T8H || kept.tpsm == T4 || kept.tpsm == T4HL || kept.tpsm == T4HH)) kept.clut = clut_get(kept);
            kept_tex = false; s_state_dirty = false; kept_epoch = s_prep_epoch; kept_q = gs_epoch();
        }
        if (draw && kept.tme && !kept_tex) {
            tex_get(kept); kept_tex = true;
            kept_epoch = s_prep_epoch; kept_q = gs_epoch();          // (decoding may have waited for the rasterizer)
        }
    } else {
        setup(local);
        if (local.tme && (local.tpsm == T8 || local.tpsm == T8H || local.tpsm == T4 || local.tpsm == T4HL || local.tpsm == T4HH)) local.clut = clut_get(local);
    }
    DrawState& d = keep ? kept : local;
    if (draw && !s_dof_off) {              // depth-of-field layer: give it its copy of the picture first (see dof_grab)
        static bool have = false;
        if (prim == 6 && dof_is_layer(d)) {
            if (!s_layer_run) { s_layer_run = true; have = dof_grab(d, s_vq); }
            if (!have) { s_dof_dropped++; drop(); return; }                                       // no blur rather than noise
        } else s_layer_run = false;
    }
    // the two sprites of a decompression read VRAM directly: no texture decode for them
    const bool dec_a = draw && prim == 6 && !s_dec_off && dec_is_a(d), dec_b = draw && prim == 6 && s_dec.pending && dec_is_b(d, s_vq);
    if (droppable && !dec_a && !dec_b) { drop(); return; }
    s_kick_a = dec_a; s_kick_drop = droppable;
    if (!keep && d.tme && draw && !dec_a && !dec_b) tex_get(d);
    if (d.tme && draw && d.tex && s_texuse_path) texuse_note(d.tex->hash, prim == 6 ? s_vq : nullptr, d);
    {   // MVP_TEXAT=frame,x0,y0,x1,y1: print the textures drawn over that screen area in that frame (debugging packs)
        static float ta[5] = {-1, 0, 0, 0, 0}; static bool ta_init = false;
        if (!ta_init) { ta_init = true; if (const char* e = std::getenv("MVP_TEXAT")) sscanf(e, "%f,%f,%f,%f,%f", &ta[0], &ta[1], &ta[2], &ta[3], &ta[4]); }
        extern u64 g_gs_frame;
        static const bool ta_from = std::getenv("MVP_TEXAT_FROM") != nullptr;      // every frame from that one on, untextured primitives too
        static int ta_n = 0;
        if (ta[0] >= 0 && draw && (d.tme || ta_from) && (ta_from ? (float)g_gs_frame >= ta[0] && ta_n < 600 : (float)g_gs_frame == ta[0])) {
            float x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9; int nv = prim == 6 ? 2 : s_vcount;
            for (int k = 0; k < nv; k++) { x0 = std::min(x0, s_vq[k].x); x1 = std::max(x1, s_vq[k].x); y0 = std::min(y0, s_vq[k].y); y1 = std::max(y1, s_vq[k].y); }
            static const float ta_min = std::getenv("MVP_TEXAT_MIN") ? (float)std::atof(std::getenv("MVP_TEXAT_MIN")) : 0;   // _FROM: at least this big
            if (ta_from ? (x0 >= ta[1] && x1 <= ta[3] && y0 >= ta[2] && y1 <= ta[4] && x1 - x0 >= ta_min && y1 - y0 >= ta_min) : (x1 >= ta[1] && x0 <= ta[3] && y1 >= ta[2] && y0 <= ta[4])) {   // _FROM: inside the area
                ta_n++;
                fprintf(stderr, "[texat] f=%llu fbp=%05x prim=%u rect %.0f,%.0f-%.0f,%.0f tme=%d abe=%d alpha=%x tfx=%u test=%x rgba=%.0f,%.0f,%.0f,%.0f tbp=%05x tbw=%u psm=%02x %ux%u cbp=%05x csa=%u hash=%016llx\n",
                        (unsigned long long)g_gs_frame, d.fbp, prim, x0, y0, x1, y1, (int)d.tme, (int)d.abe, d.alpha, d.tfx, d.test, s_vq[nv - 1].r, s_vq[nv - 1].g, s_vq[nv - 1].b, s_vq[nv - 1].a,
                        d.tbp, d.tbw, d.tpsm, 1u << d.tw, 1u << d.th, d.cbp, d.csa, d.tex ? (unsigned long long)d.tex->hash : 0ull);
                static const bool ta_v = std::getenv("MVP_TEXAT_V") != nullptr;      // also the vertices
                if (ta_v) for (int k = 0; k < nv; k++) fprintf(stderr, "    v%d xy=(%.4f,%.4f) z=%.0f st=(%f,%f,%f) uv=(%.1f,%.1f) fst=%d wm=%u,%u\n", k, s_vq[k].x, s_vq[k].y, s_vq[k].z, s_vq[k].s, s_vq[k].t, s_vq[k].q, s_vq[k].u, s_vq[k].v, (int)d.fst, d.wms, d.wmt);
            }
        }
    }
    if (draw && s_prims_frame >= 0 && prim >= 3 && prim <= 5) {
        extern u64 g_gs_frame;
        if ((long long)g_gs_frame == s_prims_frame) {
            if (!s_prims_f) { s_prims_f = fopen((s_prims_dir + "/prims.txt").c_str(), "w"); s_prims_tex.clear(); }
            if (s_prims_f) {
                const u64 th = (d.tme && d.tex) ? d.tex->hash : 0;
                fprintf(s_prims_f, "T %016llx %d %d fbp=%x tfx=%u abe=%d alpha=%x test=%x fst=%d zmsk=%u\n", (unsigned long long)th, d.tex ? d.tex->w : 0, d.tex ? d.tex->h : 0,
                        d.fbp, d.tfx, (int)d.abe, d.alpha, d.test, (int)d.fst, d.zmsk);
                for (int k = 0; k < 3; k++) {
                    const Vtx& q = s_vq[k];
                    const float su = d.fst ? q.u / (float)(1u << d.tw) : (q.q != 0 ? q.s / q.q : q.s), sv = d.fst ? q.v / (float)(1u << d.th) : (q.q != 0 ? q.t / q.q : q.t);
                    fprintf(s_prims_f, "V %.4f %.4f %.0f %.0f %.0f %.0f %.0f %.6f %.6f %.6f\n", q.x, q.y, q.z, q.r, q.g, q.b, q.a, su, sv, q.q);
                }
                if (th && !s_prims_tex.count(th)) {
                    s_prims_tex.insert(th);
                    std::vector<u32> px(d.tex->px);
                    for (u32& c : px) { u32 a = (c >> 24) * 2; c = (c & 0xffffff) | ((a > 255 ? 255 : a) << 24); }
                    extern bool gs_write_png_rgba(const char*, const std::vector<u32>&, int, int);
                    char nm[64]; snprintf(nm, sizeof nm, "/%016llx.png", (unsigned long long)th);
                    gs_write_png_rgba((s_prims_dir + nm).c_str(), px, d.tex->w, d.tex->h);
                }
            }
        } else if ((long long)g_gs_frame > s_prims_frame && s_prims_f) {
            fclose(s_prims_f); s_prims_f = nullptr; s_prims_frame = -1; rlog("gs", "prims written to %s", s_prims_dir.c_str());
        }
    }
    if (draw) s_targets[std::make_tuple(d.fbp, d.fbw, d.fpsm, (u32)d.tme)]++;
    const int dbg_fbp = s_dbg_fbp;
    const u64 dbg_from = s_dbg_from;
    {   // MVP_GSTRACE: one line per run of primitives with the same target, mask and (for 32-bit textures) source
        static const bool tr = std::getenv("MVP_GSTRACE") != nullptr;
        if (tr && draw) {
            static u64 key = ~0ull; static u32 n = 0;
            const u64 k = ((u64)d.fbp << 40) ^ ((u64)d.fbw << 34) ^ ((u64)d.fbmsk) ^ ((u64)(d.tme && d.tpsm == CT32 ? d.tbp : 0) << 14) ^ ((u64)d.abe << 62) ^ ((u64)(d.alpha & 0xff) << 50);
            if (k != key) {
                if (key != ~0ull) fprintf(stderr, " x%u\n", n);
                fprintf(stderr, "[trace] f=%llu fbp=%05x fbw=%u fbmsk=%08x prim=%u tme=%d tbp=%05x tpsm=%02x abe=%d alpha=%x test=%x zbp=%05x zmsk=%u", (unsigned long long)g_gs_frame, d.fbp, d.fbw, d.fbmsk, prim, (int)d.tme, d.tbp, d.tpsm, (int)d.abe, d.alpha, d.test, d.zbp, d.zmsk);
                key = k; n = 0;
            }
            n++;
        }
    }
    static const int dbg_tbp = std::getenv("MVP_GSLOG_TBP") ? (int)std::strtol(std::getenv("MVP_GSLOG_TBP"), nullptr, 16) : -1;   // or: primitives using this texture
    static const int dbg_tbw = std::getenv("MVP_GSLOG_TBW") ? std::atoi(std::getenv("MVP_GSLOG_TBW")) : -1;                     // ... with this buffer width
    if (draw && ((int)d.fbp == dbg_fbp || dbg_fbp == 0xfff || (d.tme && (int)d.tbp == dbg_tbp && (dbg_tbw < 0 || (int)d.tbw == dbg_tbw))) && s_frames >= dbg_from) {
        static int n = 0;
        if (n++ < (dbg_fbp == 0xfff ? 400 : 60)) {
            fprintf(stderr, "[gsdbg] fbp=%x fbw=%u fpsm=%02x prim=%u tme=%d fst=%d abe=%d tfx=%u tcc=%u tbp=%05x tbw=%u tpsm=%02x tw=%u th=%u cbp=%05x cpsm=%u test=%x alpha=%x fbmsk=%x scis=%d,%d-%d,%d\n",
                    d.fbp, d.fbw, d.fpsm, prim, d.tme, d.fst, d.abe, d.tfx, d.tcc, d.tbp, d.tbw, d.tpsm, d.tw, d.th, d.cbp, d.cpsm, d.test, d.alpha, d.fbmsk, d.sx0, d.sy0, d.sx1, d.sy1);
            for (int k = 0; k < s_vcount; k++)
                fprintf(stderr, "    v%d xy=(%.4f,%.4f) z=%.0f rgba=(%.0f,%.0f,%.0f,%.0f) st=(%f,%f,%f) uv=(%.1f,%.1f)\n", k, s_vq[k].x, s_vq[k].y, s_vq[k].z,
                        s_vq[k].r, s_vq[k].g, s_vq[k].b, s_vq[k].a, s_vq[k].s, s_vq[k].t, s_vq[k].q, s_vq[k].u, s_vq[k].v);
        }
    }
    // Ultrawide, "HUD at 16:9" (experimental): the game lays its 2D out for 16:9; at a wider aspect the sprites it
    // draws over the 3D scene (score bug, meters, text) are pulled towards the middle by 16:9 / aspect so they keep
    // their shape. Only sprites drawn into the picture without a depth test after the scene's 3D in the same frame
    // (the backdrop panoramas drawn before it stay), and not full-width ones (fades, the copy to the display) or
    // copies of rendered pictures (blur, depth of field).
    static u64 s_3d_frame = ~0ull;           // the frame in which the 3D scene was drawn (depth-tested triangles into the picture)
    if (draw && g_hud_k < 0.999f && prim >= 3 && prim <= 5 && (d.test & 0x10000) && ((d.test >> 17) & 3) >= 2 && d.fbp == pic_main()) s_3d_frame = g_gs_frame;
    if (draw && (prim == 6 || (prim >= 3 && prim <= 5)) && g_hud_k < 0.999f && s_3d_frame == g_gs_frame && (d.fbp == pic_main() || fb_is_display(d.fbp / 32))
        && (!(d.test & 0x10000) || ((d.test >> 17) & 3) == 1)
        && !(d.tme && (tex_in_picture(d.tbp) || fb_is_display(d.tbp / 32) || ((d.tpsm == CT32 || d.tpsm == CT24) && d.tbw >= 4 && (1u << d.tw) >= 256)))   // copies of rendered pictures (blur, depth of field, strips)
        && !(!s_dof_off && dof_is_layer(d))) {
        const int nv = prim == 6 ? 2 : 3;
        float x0 = s_vq[0].x, x1 = s_vq[0].x;
        for (int k = 1; k < nv; k++) { x0 = std::min(x0, s_vq[k].x); x1 = std::max(x1, s_vq[k].x); }
        const float l = (float)d.sx0, r = (float)d.sx1 + 1;
        if (r > l && x1 - x0 < (r - l) * 0.9f) {
            const float cx = (l + r) * 0.5f; Vtx q[3] = {s_vq[0], s_vq[1], s_vq[2]};
            for (int k = 0; k < nv; k++) q[k].x = cx + (q[k].x - cx) * g_hud_k;
            submit(d, prim == 6 ? 3 : 2, q, nv);
            draw = false;
        }
    }
    if (draw) {
        switch (prim) {
        case 0: { Vtx p = v; p.z = (float)z; submit(d, 0, &p, 1); break; }
        case 1: case 2: submit(d, 1, s_vq, 2); break;
        case 3: case 4: case 5: submit(d, 2, s_vq, 3); break;
        case 6: submit(d, 3, s_vq, 2); break;
        }
    }
    switch (prim) {
        case 2: s_vq[0] = s_vq[1]; s_vcount = 1; break;                   // line strip
        case 4: s_vq[0] = s_vq[1]; s_vq[1] = s_vq[2]; s_vcount = 2; break; // triangle strip
        case 5: s_vq[1] = s_vq[2]; s_vcount = 2; break;                    // fan
        default: s_vcount = 0;
    }
}

// ---------------------------------------------------------------- transfers
static struct Xfer { bool active = false; u32 x = 0, y = 0; u32 nib = 0; u32 partial = 0; int pbytes = 0; } s_x;

static u32 s_xfer_p0 = 0, s_xfer_np = 0, s_xfer_qn = 0;
// Upload de-duplication for the texture cache: EAGL re-sends the same texture data every frame; pages are
// only marked changed when an upload's bytes differ from the last upload to the same place.
static bool s_up_active = false; static u64 s_up_hash = 0, s_up_key = 0; static u32 s_up_p0 = 0, s_up_np = 0;
struct UploadRec { u64 hash = 0; u64 gen_after = 0; };
static std::unordered_map<u64, UploadRec> s_upload_hashes;
static void up_finish() {
    if (!s_up_active) return;
    s_up_active = false;
    UploadRec& r = s_upload_hashes[s_up_key];
    bool same = r.gen_after && r.hash == s_up_hash;
    // identical bytes only mean "unchanged" if nothing else wrote these pages since our last upload
    for (u32 k = 0; same && k < s_up_np && k < 512; k++) { u32 pg = (s_up_p0 + k) & 511; if (s_page_gen[pg] > r.gen_after || s_page_drawn[pg]) same = false; }
    if (!same) { pages_bump(s_up_p0, s_up_np, false); s_up_bumps++; } else s_up_same++;
    r.hash = s_up_hash; r.gen_after = s_gen_counter;
}
static const bool s_xferlog = std::getenv("MVP_XFERLOG") != nullptr; static const bool s_xferlog_all = s_xferlog && !std::strcmp(std::getenv("MVP_XFERLOG"), "all"); static int s_xferlog_n = 0;       // destination pages of the current upload
static void xfer_range(u32 bp, u32 bw, u32 psm, u32 y0, u32 h, u32 w, u32 x0) {
    u32 ph = page_h(psm), pw = page_w(psm);
    u32 bwp = std::max<u32>((std::max<u32>(bw * 64, x0 + w) + pw - 1) / pw, 1);
    u32 rows = (y0 + h + ph - 1) / ph + 1;
    s_xfer_p0 = bp >> 5; s_xfer_np = std::min<u32>(bwp * rows, 512);
}
static void xfer_check() {                                  // drain if queued work touches the upload's pages
    if (s_q_n && s_q_n != s_xfer_qn && gs_pages_busy(s_xfer_p0 << 5, 1, s_xfer_np, false)) { gs_sync_reason(0); gs_sync(); }
    s_xfer_qn = s_q_n;
}
static void xfer_begin() {
    dec_flush();
    s_vram_gen_fwd();
    u32 dir = bits(R.trxdir, 0, 2);
    {
        u32 dbp = bits(R.bitbltbuf, 32, 14), dbw = bits(R.bitbltbuf, 48, 6), dpsm = bits(R.bitbltbuf, 56, 6);
        u32 dx = bits(R.trxpos, 32, 11), dy = bits(R.trxpos, 48, 11), w = bits(R.trxreg, 0, 12), h = bits(R.trxreg, 32, 12);
        xfer_range(dbp, dbw, dpsm, dy, h, w, dx);
        if (dir == 0) upext_upload(dbp, dbw, dpsm, dx, dy, w, h);
        { static const bool tr = std::getenv("MVP_GSTRACE") != nullptr; if (tr) fprintf(stderr, "\n[trace] upload dir=%u dbp=%05x dbw=%u psm=%02x %ux%u", dir, dbp, dbw, dpsm, w, h); }
        static const bool vwatch_big = std::getenv("MVP_VWATCH_BIG") != nullptr;      // only 32-bit uploads at least 128 wide
        if (s_vwatch_lo != ~0u && (s_vwatch_cut < 0 || g_gs_frame <= s_vwatch_until) && dir != 1 && (s_xfer_p0 << 5) < s_vwatch_hi && ((s_xfer_p0 + s_xfer_np) << 5) > s_vwatch_lo && (!vwatch_big || (dpsm == CT32 && w >= 128))) {
            static u64 lf = ~0ull; static u32 lb = ~0u;
            if (lf != g_gs_frame || lb != dbp) { lf = g_gs_frame; lb = dbp; fprintf(stderr, "[vwatch] f=%llu upload dbp=%05x dbw=%u psm=%02x %ux%u\n", (unsigned long long)g_gs_frame, dbp, dbw, dpsm, w, h); }
        }
        if (s_xferlog && (s_xferlog_all || (s_xfer_p0 + s_xfer_np > 250 && s_xfer_p0 < 370) || dpsm == 0x1b || dpsm == 0x24 || dpsm == 0x2c) && s_xferlog_n < (s_xferlog_all ? 100000 : 400)) { s_xferlog_n++; fprintf(stderr, "[xfer] f=%llu dir=%u dbp=%05x dbw=%u dpsm=%02x %ux%u at %u,%u (pages %u..+%u)\n", (unsigned long long)g_gs_frame, dir, dbp, dbw, dpsm, w, h, dx, dy, s_xfer_p0, s_xfer_np); }
        { u32 op0, onp; px_range(dpsm, dbp, dbw, dx, dy, dx + (w ? w - 1 : 0), dy + (h ? h - 1 : 0), op0, onp);
          own_check_write(op0, onp, "upload", top_byte_psm(dpsm)); }
        if (dir == 2 || dir == 1) {
            u32 sbp = bits(R.bitbltbuf, 0, 14), sbw = bits(R.bitbltbuf, 16, 6), spsm = bits(R.bitbltbuf, 24, 6);
            u32 sx = bits(R.trxpos, 0, 11), sy = bits(R.trxpos, 16, 11);
            u32 op0, onp; px_range(spsm, sbp, sbw, sx, sy, sx + (w ? w - 1 : 0), sy + (h ? h - 1 : 0), op0, onp);
            own_check_read(op0, onp, "transfer source", top_byte_psm(spsm));
        }
        s_xfer_qn = ~0u; xfer_check();
        clut_invalidate_pages(s_xfer_p0, s_xfer_np);
        up_finish();
        if (dir == 2) pages_bump(s_xfer_p0, s_xfer_np, true);   // local->local copies count as rendered content
        else if (dir == 0) {                                    // host->local: bump when done, only if the data changed
            s_up_active = true; s_up_hash = 1469598103934665603ull;
            s_up_key = mix64(mix64(bits(R.bitbltbuf, 32, 32), R.trxpos), R.trxreg);
            s_up_p0 = s_xfer_p0; s_up_np = s_xfer_np;
        }
    }
    if (dir == 0) { s_x = Xfer(); s_x.active = true; return; }
    if (dir == 2) {                                        // local -> local
        {   // a VRAM-to-VRAM copy out of the picture, in a frame whose picture was skipped: run the frame again
            const u32 sbp = bits(R.bitbltbuf, 0, 14), dbp = bits(R.bitbltbuf, 32, 14);
            const int pic = tex_in_picture(sbp);
            static const bool picsrc = std::getenv("MVP_PICSRC") != nullptr;
            if (picsrc && pic) fprintf(stderr, "[picsrc] f=%llu skip=%d xfer sbp=%05x dbp=%05x %ux%u\n", (unsigned long long)g_gs_frame, (int)s_skip_draw, sbp, dbp, (u32)bits(R.trxreg, 0, 12), (u32)bits(R.trxreg, 32, 12));
            if (pic == 1) s_fs_cut_frame = g_gs_frame;
            if (pic && s_fs_active && s_skip_draw && !tex_in_picture(dbp)) s_fs_request = true;
        }
        gs_sync_reason(0); gs_sync();
        u32 sbp = bits(R.bitbltbuf, 0, 14), sbw = bits(R.bitbltbuf, 16, 6), spsm = bits(R.bitbltbuf, 24, 6);
        u32 dbp = bits(R.bitbltbuf, 32, 14), dbw = bits(R.bitbltbuf, 48, 6), dpsm = bits(R.bitbltbuf, 56, 6);
        u32 sx = bits(R.trxpos, 0, 11), sy = bits(R.trxpos, 16, 11), dx = bits(R.trxpos, 32, 11), dy = bits(R.trxpos, 48, 11);
        u32 w = bits(R.trxreg, 0, 12), h = bits(R.trxreg, 32, 12);
        for (u32 y = 0; y < h; y++) for (u32 x = 0; x < w; x++)
            write_px(dpsm, dbp, dbw, (dx + x) & 2047, (dy + y) & 2047, read_px(spsm, sbp, sbw, (sx + x) & 2047, (sy + y) & 2047));
    }
}

static void xfer_pixel(u32 v) {
    u32 dbp = bits(R.bitbltbuf, 32, 14), dbw = bits(R.bitbltbuf, 48, 6), dpsm = bits(R.bitbltbuf, 56, 6);
    u32 dx = bits(R.trxpos, 32, 11), dy = bits(R.trxpos, 48, 11), w = bits(R.trxreg, 0, 12), h = bits(R.trxreg, 32, 12);
    if (!w || s_x.y >= h) return;
    write_px(dpsm, dbp, dbw, (dx + s_x.x) & 2047, (dy + s_x.y) & 2047, v);
    if (++s_x.x >= w) { s_x.x = 0; s_x.y++; }
}

u64 g_xfer_bytes_psm[64];
static void xfer_data(const u8* p, u32 bytes) {
    g_xfer_bytes_psm[bits(R.bitbltbuf, 56, 6)] += bytes;
    if (s_q_n != s_xfer_qn) xfer_check();
    if (s_up_active) { // four chains side by side (one multiply after another was 1.5% of the main thread in a match)
                       const u64 M = 0x100000001b3ull; u64 h0 = s_up_hash, h1 = 0x9e3779b97f4a7c15ull, h2 = 0xc2b2ae3d27d4eb4full, h3 = 0x165667b19e3779f9ull;
                       u32 i = 0;
                       for (; i + 32 <= bytes; i += 32) { u64 q[4]; std::memcpy(q, p + i, 32); h0 = (h0 ^ q[0]) * M; h1 = (h1 ^ q[1]) * M; h2 = (h2 ^ q[2]) * M; h3 = (h3 ^ q[3]) * M; }
                       for (; i + 8 <= bytes; i += 8) { u64 q; std::memcpy(&q, p + i, 8); h0 = (h0 ^ q) * M; }
                       if (bytes >= 32) { h1 ^= h1 >> 29; h2 ^= h2 >> 31; h3 ^= h3 >> 33; h0 = ((h0 ^ (h0 >> 32)) + h1 * 3 + h2 * 5 + h3 * 7) * M; }
                       s_up_hash = h0;
                       struct Fin { ~Fin() { if (s_up_active && s_x.y >= bits(R.trxreg, 32, 12)) up_finish(); } } fin; (void)fin; }
    u32 dpsm = bits(R.bitbltbuf, 56, 6);
    if (dpsm == CT32 || dpsm == T8 || dpsm == T4) {        // fast paths: whole row runs, per-row address parts hoisted
        u32 dbp = bits(R.bitbltbuf, 32, 14), dbw = bits(R.bitbltbuf, 48, 6);
        u32 dx = bits(R.trxpos, 32, 11), dy = bits(R.trxpos, 48, 11), w = bits(R.trxreg, 0, 12), h = bits(R.trxreg, 32, 12);
        if (!w) return;
        u32 x = s_x.x, y = s_x.y;
        const u32 per_byte = dpsm == T4 ? 2 : 1, bpp = dpsm == CT32 ? 4 : 1;
        u32 npix = dpsm == CT32 ? bytes / 4 : bytes * per_byte, i = 0;
        while (i < npix && y < h) {
            const u32 run = std::min(w - x, npix - i);
            const u32 ry = (dy + y) & 2047;
            if (dpsm == CT32) {
                const u32 rowbase = dbp + (ry >> 5) * dbw * 32; const u8* blk = kBlock32[(ry >> 3) & 3]; const u8* col = kCol32[ry & 7];
                const u8* src = p + (size_t)i * bpp;
                u32 k = 0;
                auto one = [&](u32 k) {
                    const u32 xx = (dx + x + k) & 2047;
                    u32 v; std::memcpy(&v, src + k * 4, 4);
                    s_vram[(((rowbase + (xx >> 6) * 32 + blk[(xx >> 3) & 7]) * 64) + col[xx & 7]) & 0xfffff] = v;
                };
                while (k < run && ((dx + x + k) & 7)) one(k++);
                for (; k + 8 <= run; k += 8) {           // a whole 8-pixel column group lands in one 64-word block
                    const u32 xx = (dx + x + k) & 2047;
                    u32* b = &s_vram[((rowbase + (xx >> 6) * 32 + blk[(xx >> 3) & 7]) * 64) & 0xfffff];
                    const u8* sp = src + k * 4;
                    // (the eight words of a row are four pairs of neighbours in the block: col = c, c+1, c+4, c+5, ...)
                    std::memcpy(&b[col[0]], sp, 8); std::memcpy(&b[col[2]], sp + 8, 8); std::memcpy(&b[col[4]], sp + 16, 8); std::memcpy(&b[col[6]], sp + 24, 8);
                }
                while (k < run) one(k++);
            } else if (dpsm == T8) {
                const u32 rowbase = dbp + (ry >> 6) * ((dbw + 1) >> 1) * 32; const u8* blk = kBlock8[(ry >> 4) & 3]; const u8* col = kCol8[ry & 15];
                const u8* src = p + i;
                u32 k = 0;
                auto one = [&](u32 k) {
                    const u32 xx = (dx + x + k) & 2047;
                    s_vram8[(((rowbase + (xx >> 7) * 32 + blk[(xx >> 4) & 7]) * 256) + col[xx & 15]) & 0x3fffff] = src[k];
                };
                while (k < run && ((dx + x + k) & 15)) one(k++);
                for (; k + 16 <= run; k += 16) {         // 16 pixels of a row land in one 256-byte block
                    const u32 xx = (dx + x + k) & 2047;
                    u8* b = &s_vram8[((rowbase + (xx >> 7) * 32 + blk[(xx >> 4) & 7]) * 256) & 0x3fffff];
                    const u8* sp = src + k;
                    // (byte stores on purpose: a version with four 64-bit read-modify-writes was no faster --
                    // the time goes to fetching the block, which the rasterizer threads last had -- and stores
                    // alone do not have to wait for it)
                    for (u32 j = 0; j < 16; j++) b[col[j]] = sp[j];
                }
                while (k < run) one(k++);
            } else {
                const u32 rowbase = dbp + (ry >> 7) * ((dbw + 1) >> 1) * 32; const u8* blk = kBlock4[(ry >> 4) & 7]; const u16* col = kCol4[ry & 15];
                for (u32 k = 0; k < run; k++) {
                    const u32 xx = (dx + x + k) & 2047, ii = i + k;
                    const u32 v = (p[ii >> 1] >> ((ii & 1) * 4)) & 15;
                    const u32 a = (((rowbase + (xx >> 7) * 32 + blk[(xx >> 5) & 3]) * 512) + col[xx & 31]) & 0x7fffff;
                    u8& b = s_vram8[a >> 1];
                    b = (a & 1) ? (u8)((b & 0x0f) | (v << 4)) : (u8)((b & 0xf0) | v);
                }
            }
            i += run; x += run;
            if (x >= w) { x = 0; y++; }
        }
        s_x.x = x; s_x.y = y;
        return;
    }
    for (u32 i = 0; i < bytes; ) {
        switch (dpsm) {
        case CT32: case Z32: { u32 v; std::memcpy(&v, p + i, 4); xfer_pixel(v); i += 4; break; }
        case CT24: case Z24: {
            s_x.partial |= (u32)p[i] << (8 * s_x.pbytes); i++;
            if (++s_x.pbytes == 3) { xfer_pixel(s_x.partial); s_x.partial = 0; s_x.pbytes = 0; }
            break;
        }
        case CT16: case CT16S: case Z16: case Z16S: { u16 v; std::memcpy(&v, p + i, 2); xfer_pixel(v); i += 2; break; }
        case T8: case T8H: xfer_pixel(p[i]); i++; break;
        default: xfer_pixel(p[i] & 15); xfer_pixel(p[i] >> 4); i++; break;   // 4-bit formats
        }
    }
}

// ---------------------------------------------------------------- register writes
static void write_reg(u32 reg, u64 v) {
    // everything but vertex data, transfers and signals may change the draw state (PRIM is written with every packet)
    if (reg == 0x00) { if (R.prim != v) s_state_dirty = true; }
    else if (reg >= 0x06 && reg < 0x50 && reg != 0x0a && reg != 0x0c && reg != 0x0d && reg != 0x18 && reg != 0x19) s_state_dirty = true;
    switch (reg) {
    case 0x00: R.prim = v; s_vcount = 0; s_vtotal = 0; break;
    case 0x01: R.rgbaq = v; break;
    case 0x02: R.st = v; break;
    case 0x03: R.uv = v; break;
    // register layout: X[0:15] Y[16:31] Z[32:63] (XYZF: Z[32:55] F[56:63]); vertex_kick takes Y at [32:47]
    case 0x04: vertex_kick((v & 0xffff) | (((v >> 16) & 0xffff) << 32), bits(v, 32, 24), bits(v, 56, 8), true); break;   // XYZF2
    case 0x05: vertex_kick((v & 0xffff) | (((v >> 16) & 0xffff) << 32), (u32)(v >> 32), 0, true); break;                // XYZ2
    case 0x0c: vertex_kick((v & 0xffff) | (((v >> 16) & 0xffff) << 32), bits(v, 32, 24), bits(v, 56, 8), false); break; // XYZF3
    case 0x0d: vertex_kick((v & 0xffff) | (((v >> 16) & 0xffff) << 32), (u32)(v >> 32), 0, false); break;              // XYZ3
    case 0x06: case 0x07: R.c[reg - 6].tex0 = v;
        {   // MVP_TEXLOG: every distinct texture setup once (base, width, format, size, CLUT)
            static const bool tl = std::getenv("MVP_TEXLOG") != nullptr;
            if (tl) { static std::unordered_set<u64> seen; u64 key = v & 0xffffffffe3ffffffull;
                if (seen.size() < 20000 && seen.insert(key).second)
                    fprintf(stderr, "[tex0] f=%llu tbp=%05x tbw=%u psm=%02x %ux%u tcc=%u tfx=%u cbp=%05x cpsm=%02x csm=%u csa=%u\n", (unsigned long long)g_gs_frame,
                            (u32)bits(v, 0, 14), (u32)bits(v, 14, 6), (u32)bits(v, 20, 6), 1u << bits(v, 26, 4), 1u << bits(v, 30, 4), (u32)bits(v, 34, 1), (u32)bits(v, 35, 2),
                            (u32)bits(v, 37, 14), (u32)bits(v, 51, 4), (u32)bits(v, 55, 1), (u32)bits(v, 56, 5)); } }
        break;
    case 0x08: case 0x09: R.c[reg - 8].clamp = v; break;
    case 0x0a: R.fog = v; break;
    case 0x14: case 0x15: R.c[reg - 0x14].tex1 = v; break;
    case 0x16: case 0x17: {                                                         // TEX2: partial TEX0 update
        Ctx& c = R.c[reg - 0x16];
        const u64 m = 0xffffffe00000000ull | (0x3full << 20);
        c.tex0 = (c.tex0 & ~m) | (v & m);
        break;
    }
    case 0x18: case 0x19: R.c[reg - 0x18].xyoffset = v; break;
    case 0x1a: R.prmodecont = v; break;
    case 0x1b: R.prmode = v; break;
    case 0x1c: R.texclut = v; break;
    case 0x3b: R.texa = v; break;
    case 0x3d: R.fogcol = v; break;
    case 0x40: case 0x41: R.c[reg - 0x40].scissor = v; break;
    case 0x42: case 0x43: R.c[reg - 0x42].alpha = v; break;
    case 0x44: R.dimx = v; break;
    case 0x45: R.dthe = v; break;
    case 0x46: R.colclamp = v; break;
    case 0x47: case 0x48: R.c[reg - 0x47].test = v; break;
    case 0x49: R.pabe = v; break;
    case 0x4a: case 0x4b: R.c[reg - 0x4a].fba = v; break;
    case 0x4c: case 0x4d: R.c[reg - 0x4c].frame = v; break;
    case 0x4e: case 0x4f: R.c[reg - 0x4e].zbuf = v; break;
    case 0x50: R.bitbltbuf = v; break;
    case 0x51: R.trxpos = v; break;
    case 0x52: R.trxreg = v; break;
    case 0x53: R.trxdir = v; xfer_begin(); break;
    case 0x54: xfer_data((const u8*)&v, 8); break;                                   // HWREG
    case 0x60: R.signal = v; if (!s_replaying_fwd() && !s_fs_resim) gt_gs_event(1); break;                   // SIGNAL
    case 0x61: if (!s_replaying_fwd() && !s_fs_resim) gt_gs_event(2); break;                                 // FINISH
    case 0x62: R.label = v; break;
    default: break;
    }
}

// ---------------------------------------------------------------- GIF
struct GifPath { u32 nloop = 0, nreg = 0, flg = 0, reg = 0; u64 regs = 0; bool active = false; };
static GifPath s_path[4];

extern u64 g_prof_gif;
static void gs_gif_write_impl(const u8* data, u32 qwc, int path);
// ---------------------------------------------------------------- GS stream record / replay (renderer testing)
// MVP_GSREC=file MVP_GSREC_FRAME=n records game frame n (VRAM + register state + every GIF/privileged write);
// `mvp2005 --gs-replay file out.png` re-renders it deterministically, e.g. to A/B rasterizer changes.
static FILE* s_rec = nullptr;
static const char* s_rec_path = std::getenv("MVP_GSREC");
static long long s_rec_frame = std::getenv("MVP_GSREC_FRAME") ? std::atoll(std::getenv("MVP_GSREC_FRAME")) : -1;   // MVP_GSREC_DOF=1: set when the depth-of-field layer is first drawn
static const long long s_rec_count = std::getenv("MVP_GSREC_FRAMES") ? std::atoll(std::getenv("MVP_GSREC_FRAMES")) : 1;
static void rec_state(FILE* f, bool save) {
    auto io = [&](void* p, size_t n) { if (save) fwrite(p, 1, n, f); else if (fread(p, 1, n, f) != n) rfatal("gs replay: truncated state"); };
    io(s_vram, sizeof s_vram); io(&R, sizeof R); s_state_dirty = true; s_prep_epoch++; io(s_path, sizeof s_path); io(s_priv, sizeof s_priv);
    io(s_vq, sizeof s_vq); io(&s_vcount, sizeof s_vcount); io(&s_vtotal, sizeof s_vtotal); io(&s_x, sizeof s_x);
}
static void rec_frame_boundary(u64 frame) {
    if (!s_rec_path || s_replaying) return;
    if (s_rec_request >= 0 && s_rec_frame < 0) { s_rec_frame = s_rec_request; rlog("gs", "will record frame %lld", s_rec_frame); }
    if (!s_rec && (long long)frame == s_rec_frame) {
        gs_sync();
        // MVP_GSREC_EVERY=n: keep recording, one file per n frames (file.gsr -> file.gsr.<frame>)
        static const long long every = std::getenv("MVP_GSREC_EVERY") ? std::atoll(std::getenv("MVP_GSREC_EVERY")) : 0;
        std::string path = s_rec_path; if (every > 0) path += "." + std::to_string(frame);
        s_rec = fopen(path.c_str(), "wb");
        if (s_rec) { fwrite("MVPGS1\0\0", 1, 8, s_rec); rec_state(s_rec, true); }
    } else if (s_rec && (long long)frame >= s_rec_frame + s_rec_count) {
        fclose(s_rec); s_rec = nullptr;
        rlog("gs", "recorded frame %lld to %s", s_rec_frame, s_rec_path);
        static const long long every = std::getenv("MVP_GSREC_EVERY") ? std::atoll(std::getenv("MVP_GSREC_EVERY")) : 0;
        if (every > 0) s_rec_frame += every;
    }
}
bool gs_is_replaying() { return s_replaying; }

// ---------------------------------------------------------------- skipped frames that need their picture
// Experimental, off by default (MVP_UNSKIP=1). Frame skip drops what a frame draws into the picture; a frame that
// copies the picture into texture memory (the grab at a camera cut, the shadow pass that reads the top byte of the
// Z buffer) then copies stale data. With this on, a skipped frame keeps the state it started from and its GS
// input, and is run again from the start with drawing on when such a copy shows up. It turned out not to be
// needed: the game refills all of that memory every frame, so nothing stale survives into a drawn frame.
static struct FrameSnap {
    std::vector<u32> vram; GsRegs R; GifPath path[4]; u64 priv[0x100]; Vtx vq[3]; int vcount = 0, vtotal = 0; Xfer x; u32 xfer_p0 = 0, xfer_np = 0;
    u64 own[8], touched[8], own_c[8], touched_c[8], lazy_pages[8]; bool own_any = false, gpu_stale = false, lazy_any = false, lazy_ct24 = true;
    std::vector<LazyCopy> lazy;
    bool up_active = false; u64 up_hash = 0, up_key = 0; u32 up_p0 = 0, up_np = 0;       // an upload may straddle the frame boundary
    std::vector<u8> stream;
} s_fs;
static u64 s_fs_unskips = 0;
static void fs_begin(bool skip) {
    static const bool on = std::getenv("MVP_UNSKIP") != nullptr;
    s_fs_request = false; s_fs_active = skip && on && !s_replaying;
    if (!s_fs_active) return;
    gs_sync();
    s_fs.vram.assign(s_vram, s_vram + sizeof s_vram / 4);
    s_fs.R = R; std::memcpy(s_fs.path, s_path, sizeof s_path); std::memcpy(s_fs.priv, s_priv, sizeof s_priv);
    std::memcpy(s_fs.vq, s_vq, sizeof s_vq); s_fs.vcount = s_vcount; s_fs.vtotal = s_vtotal; s_fs.x = s_x; s_fs.xfer_p0 = s_xfer_p0; s_fs.xfer_np = s_xfer_np;
    std::memcpy(s_fs.own, s_own, sizeof s_own); std::memcpy(s_fs.touched, s_gpu_touched, sizeof s_gpu_touched);
    std::memcpy(s_fs.own_c, s_own_c, sizeof s_own_c); std::memcpy(s_fs.touched_c, s_touched_c, sizeof s_touched_c);
    std::memcpy(s_fs.lazy_pages, s_lazy_pages, sizeof s_lazy_pages);
    s_fs.own_any = s_own_any; s_fs.gpu_stale = s_gpu_stale; s_fs.lazy_any = s_lazy_any; s_fs.lazy_ct24 = s_lazy_ct24; s_fs.lazy = s_lazy;
    s_fs.up_active = s_up_active; s_fs.up_hash = s_up_hash; s_fs.up_key = s_up_key; s_fs.up_p0 = s_up_p0; s_fs.up_np = s_up_np;
    s_fs.stream.clear();
}
static inline void fs_record(u8 type, u32 a, const void* data, u32 n) {       // type 1: GIF data (a = path, n quadwords), 2: privileged write (a = address)
    if (s_fs.stream.size() > (64u << 20)) { s_fs_active = false; return; }    // an absurdly large frame: give up on it
    const u32 bytes = type == 1 ? n * 16 : 8; size_t o = s_fs.stream.size();
    s_fs.stream.resize(o + 9 + bytes);
    s_fs.stream[o] = type; std::memcpy(&s_fs.stream[o + 1], &a, 4); std::memcpy(&s_fs.stream[o + 5], &n, 4); std::memcpy(&s_fs.stream[o + 9], data, bytes);
}
static void fs_unskip() {
    s_fs_request = false;
    if (!s_fs_active) return;
    s_fs_active = false;
    gs_sync(); s_dec.pending = false;
    s_up_active = s_fs.up_active; s_up_hash = s_fs.up_hash; s_up_key = s_fs.up_key; s_up_p0 = s_fs.up_p0; s_up_np = s_fs.up_np;
    std::memcpy(s_vram, s_fs.vram.data(), sizeof s_vram);
    s_state_dirty = true;
    R = s_fs.R; std::memcpy(s_path, s_fs.path, sizeof s_path); std::memcpy(s_priv, s_fs.priv, sizeof s_priv);
    std::memcpy(s_vq, s_fs.vq, sizeof s_vq); s_vcount = s_fs.vcount; s_vtotal = s_fs.vtotal; s_x = s_fs.x; s_xfer_p0 = s_fs.xfer_p0; s_xfer_np = s_fs.xfer_np;
    std::memcpy(s_own, s_fs.own, sizeof s_own); std::memcpy(s_gpu_touched, s_fs.touched, sizeof s_gpu_touched);
    std::memcpy(s_own_c, s_fs.own_c, sizeof s_own_c); std::memcpy(s_touched_c, s_fs.touched_c, sizeof s_touched_c);
    std::memcpy(s_lazy_pages, s_fs.lazy_pages, sizeof s_lazy_pages);
    s_own_any = s_fs.own_any; s_gpu_stale = s_fs.gpu_stale; s_lazy_any = s_fs.lazy_any; s_lazy_ct24 = s_fs.lazy_ct24; s_lazy = s_fs.lazy;
    // VRAM went back in time: nothing cached from it may be reused
    for (u32 p = 0; p < 512; p++) s_page_gen[p] = ++s_gen_counter;
    s_prep_epoch++;
    upext_clear();
    for (auto& e : s_clut_cache) e.valid = false;
    s_vram_gen_fwd();
    s_skip_draw = false; s_fs_unskips++;
    extern u64 g_frames_drawn, g_frames_skipped; g_frames_drawn++; if (g_frames_skipped) g_frames_skipped--;
    s_fs_resim = true;
    for (size_t o = 0; o + 9 <= s_fs.stream.size(); ) {
        const u8 type = s_fs.stream[o]; u32 a, n; std::memcpy(&a, &s_fs.stream[o + 1], 4); std::memcpy(&n, &s_fs.stream[o + 5], 4);
        const u8* d = &s_fs.stream[o + 9];
        if (type == 1) { gs_gif_write_impl(d, n, (int)a); o += 9 + (size_t)n * 16; }
        else { u64 v; std::memcpy(&v, d, 8); s_priv[(a >> 4) & 0xff] = v; o += 17; }
    }
    s_fs_resim = false;
}

void gs_gif_write(const u8* data, u32 qwc, int path) {
    gs_threads_init();
    if (s_rec) { u8 t = 1, pth = (u8)path; fwrite(&t, 1, 1, s_rec); fwrite(&pth, 1, 1, s_rec); fwrite(&qwc, 4, 1, s_rec); fwrite(data, 16, qwc, s_rec); }
    if (s_fs_active) fs_record(1, (u32)path, data, qwc);
    u64 t0 = prof_time_ns();
    gs_gif_write_impl(data, qwc, path);
    if (s_fs_request) fs_unskip();
    g_prof_gif += prof_time_ns() - t0;
}
static void gs_gif_write_impl(const u8* data, u32 qwc, int path) {
    GifPath& P = s_path[path & 3];
    for (u32 q = 0; q < qwc; q++) {
        const u8* d = data + q * 16;
        u64 lo, hi; std::memcpy(&lo, d, 8); std::memcpy(&hi, d + 8, 8);
        if (!P.active || P.nloop == 0) {                    // new GIFtag
            P.nloop = bits(lo, 0, 15);
            u32 pre = bits(lo, 46, 1);
            P.flg = bits(lo, 58, 2);
            P.nreg = bits(lo, 60, 4); if (!P.nreg) P.nreg = 16;
            P.regs = hi; P.reg = 0;
            P.active = P.nloop != 0;
            if (pre && P.flg == 0) write_reg(0x00, bits(lo, 47, 11));
            if (P.flg >= 2) P.nloop = bits(lo, 0, 15);
            continue;
        }
        if (P.flg >= 2) {                                    // IMAGE: hand over the whole run of image quadwords
            u32 n = std::min<u32>(P.nloop, qwc - q);
            xfer_data(d, n * 16);
            q += n - 1;
            P.nloop -= n;
            if (P.nloop == 0) P.active = false;
            continue;
        }
        if (P.flg == 0) {                                    // PACKED
            u32 desc = (u32)(P.regs >> (P.reg * 4)) & 15;
            switch (desc) {
            case 0x0: write_reg(0x00, lo & 0x7ff); break;
            case 0x1: R.rgbaq = (lo & 0xff) | (((lo >> 32) & 0xff) << 8) | ((hi & 0xff) << 16) | (((hi >> 32) & 0xff) << 24) |
                                ((u64)(*(u32*)&R.q) << 32); break;
            case 0x2: {
                R.st = lo; u32 qi = (u32)hi; std::memcpy(&R.q, &qi, 4);
                // Q reaches a vertex through RGBAQ: packed ST only stores it until the next packed RGBAQ. The game sends
                // two things as (ST, XYZF2) with Q = 1 and no RGBAQ in the vertex: the people of the 3D crowd (their
                // colour goes ahead in a packet of its own) and the shine on helmets. They were textured with the Q that
                // happened to be in RGBAQ: 1.0 after another person, but the 1/w of the last seat for the first person
                // of a group, whose texture then turned into static (one spectator per walk-up shot), and the helmets'
                // reflection collapsed to the edge of its texture. A vertex format without RGBAQ whose ST says Q = 1
                // now gets Q = 1; formats with RGBAQ, and any other Q, behave as before.
                if (qi == 0x3f800000u && (u32)(R.rgbaq >> 32) != qi) {
                    bool has_rgbaq = false;
                    for (u32 k = 0; k < P.nreg; k++) if (((P.regs >> (k * 4)) & 15) == 1) { has_rgbaq = true; break; }
                    if (!has_rgbaq) R.rgbaq = (R.rgbaq & 0xffffffffull) | ((u64)qi << 32);
                }
                break;
            }
            case 0x3: R.uv = (lo & 0x3fff) | (((lo >> 32) & 0x3fff) << 16); break;
            case 0x4: {
                u64 v = (lo & 0xffff) | (((lo >> 32) & 0xffff) << 16) | ((u64)((hi >> 4) & 0xffffff) << 32) | ((u64)((hi >> 36) & 0xff) << 56);
                u64 xy = (lo & 0xffff) | ((lo >> 32 & 0xffff) << 32);
                bool adc = (hi >> 47) & 1;
                vertex_kick(xy, (u32)((v >> 32) & 0xffffff), (u32)(v >> 56), !adc);
                break;
            }
            case 0x5: {
                u64 xy = (lo & 0xffff) | ((lo >> 32 & 0xffff) << 32);
                bool adc = (hi >> 47) & 1;
                vertex_kick(xy, (u32)hi, 0, !adc);
                break;
            }
            case 0xa: R.fog = ((hi >> 36) & 0xff) << 56; break;
            case 0xc: { u64 xy = (lo & 0xffff) | ((lo >> 32 & 0xffff) << 32); vertex_kick(xy, (u32)((hi >> 4) & 0xffffff), (u32)((hi >> 36) & 0xff), false); break; }
            case 0xd: { u64 xy = (lo & 0xffff) | ((lo >> 32 & 0xffff) << 32); vertex_kick(xy, (u32)hi, 0, false); break; }
            case 0xe: write_reg((u32)(hi & 0xff), lo); break;                         // A+D
            case 0xf: break;                                                           // NOP
            default: write_reg(desc, lo); break;                                       // TEX0/CLAMP etc.
            }
            if (++P.reg >= P.nreg) { P.reg = 0; if (--P.nloop == 0) P.active = false; }
            continue;
        }
        // REGLIST: two 64-bit registers per quadword
        for (int half = 0; half < 2 && P.active; half++) {
            u64 v = half ? hi : lo;
            u32 desc = (u32)(P.regs >> (P.reg * 4)) & 15;
            if (desc < 0xe) write_reg(desc, v);
            if (++P.reg >= P.nreg) { P.reg = 0; if (--P.nloop == 0) P.active = false; }
        }
    }
}

// ---------------------------------------------------------------- privileged registers / display
void gs_priv_write(u32 addr, u64 v) {
    if (s_rec) { u8 t = 2; fwrite(&t, 1, 1, s_rec); fwrite(&addr, 4, 1, s_rec); fwrite(&v, 8, 1, s_rec); }
    if (s_fs_active) fs_record(2, addr, &v, 0);
    u32 idx = (addr >> 4) & 0xff;
    s_priv[idx] = v;
    if (idx == 0x07 || idx == 0x09) s_disp_seen[bits(v, 0, 9)] = g_gs_frame + 1;
}
static bool write_png(const char* path, const std::vector<u32>& px, int w, int h);
static int gs_replay_pass(const char* file, const char* png) {
    FILE* f = fopen(file, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", file); return 1; }
    char magic[8]; if (fread(magic, 1, 8, f) != 8 || std::memcmp(magic, "MVPGS1", 6)) { fprintf(stderr, "not a GS recording\n"); return 1; }
    s_replaying = true;
    rec_state(f, false);
    pages_bump(0, 512, false);                           // VRAM was replaced wholesale
    upext_clear();
    if (const char* r = std::getenv("MVP_REPLAY_RES")) {       // replay at an internal resolution (main target assumed 0x1180)
        s_hr_scale = std::max(1, std::min(4, std::atoi(r)));
        s_hr_tbp = std::getenv("MVP_REPLAY_TBP") ? (u32)std::strtoul(std::getenv("MVP_REPLAY_TBP"), nullptr, 16) : 0x1180; s_hr_tbw = 8;
        s_hr_w = 512 * s_hr_scale; s_hr_h = 512 * s_hr_scale;
        s_gpu_on = std::getenv("MVP_REPLAY_GPU") && (gpu_available() || gpu_init_offscreen());
        if (s_gpu_on) { gpu_set_target(512, 512, s_hr_scale); s_gpu_stale = true; gpu_set_aniso(g_settings.aniso); }
        else { s_hr_col.assign((size_t)s_hr_w * s_hr_h, 0); s_hr_z.assign((size_t)s_hr_w * s_hr_h, 0); }
    }
    auto dump_buf = [](const char* suffix) {                  // MVP_REPLAY_BUF=bp,bw,w,h,file: a 32-bit buffer as PNG, before and after
        const char* e = std::getenv("MVP_REPLAY_BUF"); if (!e) return;
        unsigned bp, bw, w, h; char file[256];
        if (sscanf(e, "%x,%u,%u,%u,%200s", &bp, &bw, &w, &h, file) != 5) return;
        std::vector<u32> px((size_t)w * h), al((size_t)w * h);
        for (u32 y = 0; y < h; y++) for (u32 x = 0; x < w; x++) { u32 v = s_vram[addr32(bp, bw, x, y)]; px[(size_t)y * w + x] = v | 0xff000000u; u32 a = std::min<u32>((v >> 24) * 2, 255); al[(size_t)y * w + x] = 0xff000000u | a * 0x010101u; }
        write_png((std::string(file) + suffix + ".png").c_str(), px, (int)w, (int)h);
        write_png((std::string(file) + suffix + "_alpha.png").c_str(), al, (int)w, (int)h);
    };
    dump_buf("_before");
    std::vector<u8> buf;
    u64 t0 = host_time_ns(), prims0 = s_prims;
    for (;;) {
        u8 t; if (fread(&t, 1, 1, f) != 1) break;
        if (t == 1) { u8 pth; u32 qwc; if (fread(&pth, 1, 1, f) != 1 || fread(&qwc, 4, 1, f) != 1) break;
                      buf.resize((size_t)qwc * 16); if (fread(buf.data(), 16, qwc, f) != qwc) break; gs_gif_write(buf.data(), qwc, pth); }
        else if (t == 2) { u32 a; u64 v; if (fread(&a, 4, 1, f) != 1 || fread(&v, 8, 1, f) != 1) break; gs_priv_write(a, v); }
        else break;
    }
    fclose(f);
    gs_sync(); gpu_flush(); if (s_gpu_on) { std::vector<u32> one; gpu_read(0, 0, 1, 1, one); }
    double ms = (host_time_ns() - t0) / 1e6;
    fprintf(stderr, "replayed %llu primitives in %.1f ms (%d rasterizer threads)\n", (unsigned long long)(s_prims - prims0), ms, s_nthreads);
    bool ok = gs_screenshot(png);
    dump_buf("_after");
    if (const char* d = std::getenv("MVP_REPLAY_TARGETS")) {       // also dump the 3D target
        std::vector<u32> px(512 * 448);
        for (int y = 0; y < 448; y++) for (int x = 0; x < 512; x++) px[(size_t)y * 512 + x] = s_vram[addr32(0x1180, 8, x, y)] | 0xff000000u;
        write_png(d, px, 512, 448);
    }
    return ok ? 0 : 1;
}
int gs_replay(const char* file, const char* png) {
    if (const char* e = std::getenv("MVP_REPLAY_ENHANCE")) g_settings.tex_enhance = std::atoi(e);
    if (const char* e = std::getenv("MVP_REPLAY_ANISO")) g_settings.aniso = std::atoi(e);
    if (std::getenv("MVP_REPLAY_DUMP")) g_settings.tex_dump = true;
    int passes = std::getenv("MVP_REPLAY_PASSES") ? std::atoi(std::getenv("MVP_REPLAY_PASSES")) : 1;
    s_replaying = true;
    int rc = 1;
    if (const char* pre = std::getenv("MVP_REPLAY_PRE")) {       // another recording first, for state that carries over (the depth-of-field grab)
        if (gs_replay_pass(pre, "/dev/null")) fprintf(stderr, "MVP_REPLAY_PRE: could not replay %s\n", pre);
        g_gs_frame += 1000;                                       // the second recording is a later frame
    }
    for (int pass = 0; pass < passes; pass++) {
        if (pass) for (;;) {                             // let the enhancer finish before the next pass
            { std::lock_guard<std::mutex> lk(s_enh_mx); if (s_enh_q.empty() && s_enh_inflight.load() == 0) break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        rc = gs_replay_pass(file, png);
        if (rc) return rc;
    }
    return rc;
}
u64 gs_priv_read(u32 addr) { return s_priv[(addr >> 4) & 0xff]; }

static u64 s_vram_gen = 1;                  // bumped by every drawn primitive / upload (display cache key)
u64 gs_display_version_real(); static u64 s_snap_published = ~0ull;
static std::mutex s_snap_mx; static std::vector<u32> s_snap; static int s_snap_w = 0, s_snap_h = 0;
static std::atomic<u64> s_snap_version{0};
static bool gs_get_display_real(std::vector<u32>& out, int& W, int& H);

static std::vector<u32> s_disp; static u64 s_disp_gen = 0, s_disp_fb = ~0ull, s_disp_regs = ~0ull, s_disp_version = 0;
static void readout(u32 psm, u32 fbp, u32 fbw, u32 dbx, u32 dby, int W, int H, std::vector<u32>& out);
bool gs_get_display(std::vector<u32>& out, int& W, int& H) {
    if (gt_active() && !gt_on_graphics_thread()) {  // main thread: the last picture the graphics thread published
        std::lock_guard<std::mutex> lk(s_snap_mx);
        if (s_snap.empty()) return false;
        out = s_snap; W = s_snap_w; H = s_snap_h;
        return true;
    }
    return gs_get_display_real(out, W, H);
}
static bool gs_get_display_real(std::vector<u32>& out, int& W, int& H) {
    if (s_gpu_on && s_hr_valid && s_hr_u1 > s_hr_u0 && s_hr_v1 > s_hr_v0) {
        uintptr_t fbo; int x0, y0;
        if (!gs_get_display_gpu(fbo, x0, y0, W, H)) return false;
        if (s_disp_gen != s_vram_gen || (int)s_disp.size() != W * H || s_disp_fb != 0xFFFFFFFEull) {
            s_disp_gen = s_vram_gen; s_disp_fb = 0xFFFFFFFEull; s_disp_regs = ~0ull; s_disp_version++;
            if (!gpu_read(x0, y0, W, H, s_disp)) return false;
            for (u32& c : s_disp) c |= 0xff000000u;
        }
        if (&out != &s_disp) out = s_disp;
        return true;
    }
    if (s_hr_scale > 1 && s_hr_valid && s_hr_u1 > s_hr_u0 && s_hr_v1 > s_hr_v0 && !s_hr_col.empty()) {
        const int S = s_hr_scale;
        int x0 = std::max(0, (int)std::floor(s_hr_u0 * S)), y0 = std::max(0, (int)std::floor(s_hr_v0 * S));
        int x1 = std::min(s_hr_w, (int)std::ceil(s_hr_u1 * S)), y1 = std::min(s_hr_h, (int)std::ceil(s_hr_v1 * S));
        W = x1 - x0; H = y1 - y0;
        if (W <= 0 || H <= 0) return false;
        if (s_disp_gen != s_vram_gen || (int)s_disp.size() != W * H || s_disp_fb != 0xFFFFFFFFull) {
            gs_sync_reason(3); gs_sync();
            s_disp_gen = s_vram_gen; s_disp_fb = 0xFFFFFFFFull; s_disp_regs = ~0ull; s_disp_version++;
            s_disp.resize((size_t)W * H);
            for (int y = 0; y < H; y++) {
                const u32* src = &s_hr_col[(size_t)(y0 + y) * s_hr_w + x0];
                u32* o = &s_disp[(size_t)y * W];
                for (int x = 0; x < W; x++) o[x] = src[x] | 0xff000000u;
            }
        }
        if (&out != &s_disp) out = s_disp;
        return true;
    }
    u64 pmode = s_priv[0x00];
    int circuit = (pmode & 2) ? 2 : (pmode & 1) ? 1 : 0;
    if (!circuit) circuit = 1;
    u64 dispfb = s_priv[circuit == 1 ? 0x07 : 0x09], display = s_priv[circuit == 1 ? 0x08 : 0x0a];
    u32 fbp = bits(dispfb, 0, 9) * 32, fbw = bits(dispfb, 9, 6), psm = bits(dispfb, 15, 5);
    u32 dbx = bits(dispfb, 32, 11), dby = bits(dispfb, 43, 11);
    u32 magh = bits(display, 23, 4) + 1, dw = bits(display, 32, 12) + 1, dh = bits(display, 44, 11) + 1;
    W = (int)(dw / magh); H = (int)dh;
    if (W <= 0 || H <= 0 || W > 2048 || H > 2048 || !fbw) return false;
    // unchanged since the last read-out? (cached; callers get a copy)
    if (s_disp_gen == s_vram_gen && s_disp_fb == dispfb && s_disp_regs == display && (int)s_disp.size() == W * H) {
        if (&out != &s_disp) out = s_disp;
        return true;
    }
    { u32 ph = page_h(psm), bwp = std::max<u32>(fbw, 1);
      own_check_read(fbp >> 5, std::min<u32>(bwp * ((dby + H + ph - 1) / ph + 1), 512), "display", false); }
    gs_sync_reason(3); gs_sync();
    s_disp_gen = s_vram_gen; s_disp_fb = dispfb; s_disp_regs = display;
    s_disp_version++;
    readout(psm, fbp, fbw, dbx, dby, W, H, s_disp);
    out = s_disp;
    return true;
}
static void readout(u32 psm, u32 fbp, u32 fbw, u32 dbx, u32 dby, int W, int H, std::vector<u32>& out) {
    out.resize((size_t)W * H);
    if (psm == CT32 || psm == CT24) {                     // fast path: whole 8-pixel column groups share a block
        for (int y = 0; y < H; y++) {
            u32* o = &out[(size_t)y * W];
            u32 yy = dby + y;
            for (int x = 0; x < W; x++) o[x] = s_vram[addr32(fbp, fbw, dbx + x, yy)] | 0xff000000u;
        }
        return;
    }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            u32 c = read_px(psm, fbp, fbw, dbx + x, dby + y);
            if (psm == CT16 || psm == CT16S) c = c16to32(c, 0x80);
            out[(size_t)y * W + x] = c | 0xff000000u;
        }
}
u64 gs_display_version_real() { return s_disp_version | (s_vram_gen << 20); }
u64 gs_display_version() { return gt_active() && !gt_on_graphics_thread() ? s_snap_version.load() : gs_display_version_real(); }
// PNG of the current display (F12 screenshots)
static bool write_png(const char* path, const std::vector<u32>& px, int w, int h);
bool gs_write_png(const char* path, const std::vector<u32>& px, int w, int h) { return write_png(path, px, w, h); }
static bool write_png_rgba(const char* path, const std::vector<u32>& px, int w, int h);
bool gs_write_png_rgba(const char* path, const std::vector<u32>& px, int w, int h) { return write_png_rgba(path, px, w, h); }
bool gs_screenshot(const char* path) {
    std::vector<u32> px; int w, h;
    if (!gs_get_display(px, w, h)) return false;
    return write_png(path, px, w, h);
}

// Minimal PNG writer (stored deflate blocks) for frame dumps.
static u32 crc_table[256];
static u32 crc32(const u8* d, size_t n, u32 c = 0xffffffffu) {
    if (!crc_table[1]) for (u32 i = 0; i < 256; i++) { u32 k = i; for (int j = 0; j < 8; j++) k = (k & 1) ? 0xedb88320u ^ (k >> 1) : k >> 1; crc_table[i] = k; }
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ d[i]) & 0xff] ^ (c >> 8);
    return c;
}
static void be32(std::string& s, u32 v) { for (int i = 3; i >= 0; i--) s += (char)((v >> (i * 8)) & 0xff); }
static void chunk(std::string& f, const char* type, const std::string& data) {
    be32(f, (u32)data.size());
    std::string td = std::string(type, 4) + data;
    f += td;
    be32(f, crc32((const u8*)td.data(), td.size()) ^ 0xffffffffu);
}
static bool write_png_n(const char* path, const std::vector<u32>& px, int w, int h, bool alpha) {
    std::string raw;
    for (int y = 0; y < h; y++) {
        raw += '\0';
        for (int x = 0; x < w; x++) { u32 c = px[(size_t)y * w + x]; raw += (char)(c & 0xff); raw += (char)((c >> 8) & 0xff); raw += (char)((c >> 16) & 0xff); if (alpha) raw += (char)(c >> 24); }
    }
    std::string z = "\x78\x01";
    u32 a = 1, b = 0;
    for (unsigned char ch : raw) { a = (a + ch) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw.size() || off == 0; off += 65535) {
        size_t n = std::min<size_t>(65535, raw.size() - off);
        bool last = off + n >= raw.size();
        z += (char)(last ? 1 : 0); z += (char)(n & 0xff); z += (char)(n >> 8); z += (char)(~n & 0xff); z += (char)((~n >> 8) & 0xff);
        z.append(raw, off, n);
        if (last) break;
    }
    be32(z, (b << 16) | a);
    std::string f = "\x89PNG\r\n\x1a\n";
    std::string ihdr; be32(ihdr, (u32)w); be32(ihdr, (u32)h); ihdr += (char)8; ihdr += (char)(alpha ? 6 : 2); ihdr += '\0'; ihdr += '\0'; ihdr += '\0';
    chunk(f, "IHDR", ihdr); chunk(f, "IDAT", z); chunk(f, "IEND", "");
    FILE* fp = fopen(path, "wb"); if (!fp) return false;
    fwrite(f.data(), 1, f.size(), fp); fclose(fp); return true;
}
static bool write_png(const char* path, const std::vector<u32>& px, int w, int h) { return write_png_n(path, px, w, h, false); }
static bool write_png_rgba(const char* path, const std::vector<u32>& px, int w, int h) { return write_png_n(path, px, w, h, true); }

static const bool s_dumping = std::getenv("MVP_FRAMES") != nullptr;
static const u64 s_dump_from = std::getenv("MVP_FRAMES_FROM") ? std::strtoull(std::getenv("MVP_FRAMES_FROM"), nullptr, 10) : 0;   // no dumps before this game frame
static const u64 s_dump_every = std::getenv("MVP_FRAME_EVERY") ? std::max(1, std::atoi(std::getenv("MVP_FRAME_EVERY"))) : 30;   // dump interval (game frames)
static const bool s_dump_ms = std::getenv("MVP_FRAMES_MS") != nullptr;                // name dumps by wall-clock ms
static const u64 s_dump_t0 = host_time_ns();
static const long long draw_from = std::getenv("MVP_DRAW_FROM") ? std::atoll(std::getenv("MVP_DRAW_FROM")) : (std::getenv("MVP_DRAW_ALL") ? 0 : -1);
// Called when the game kicks the display list of a new frame: decide whether this frame is rasterized.
// Skipping is per whole frame (a half-drawn frame would show garbage); the display keeps the last frame.
u64 g_frames_drawn = 0, g_frames_skipped = 0;
static int s_consec_skips = 0;
void gs_frame_begin(u64 frame) {
    dec_flush();
    g_gs_frame = frame;
    extern bool g_headless; bool skip;
    if (g_headless) {
        // headless: rasterize only frames that get dumped (MVP_DRAW_ALL=1 draws everything; MVP_DRAW_FROM=n from game frame n)
        static const u64 draw_every = std::getenv("MVP_DRAW_EVERY") ? std::max(1, std::atoi(std::getenv("MVP_DRAW_EVERY"))) : 0;   // e.g. texture dumping runs
        skip = !(s_dumping && g_gs_frame + 3 >= s_dump_from && (g_gs_frame % s_dump_every) >= s_dump_every - std::min<u64>(3, s_dump_every)) && !(draw_from >= 0 && (long long)g_gs_frame >= draw_from)
               && !(draw_every && g_gs_frame % draw_every == 0);
    } else {
        int fs = g_settings.frameskip;
        extern u64 kernel_vblank_late_ns(); extern u64 kernel_vblank_ns();
        if (fs == 1) skip = s_consec_skips < 3 && kernel_vblank_late_ns() > kernel_vblank_ns() / 2;   // auto: only when behind
        else if (fs >= 2) skip = (g_gs_frame % (u64)fs) != 0;
        else skip = false;
    }
    rec_frame_boundary(g_gs_frame);
    if (s_texuse_path && g_gs_frame % 300 == 0) texuse_save();
    tex_cache_trim();
    {   // internal resolution changes (menu) take effect at a frame boundary
        bool gpu = false;
        static const bool force_gpu = std::getenv("MVP_FORCE_GPU") != nullptr;   // headless testing with an offscreen context
        if (g_settings.renderer == 1 && !gt_active() && (!g_headless || s_replaying)) gpu = gpu_available() || gpu_init();
        else if (force_gpu && !gt_active()) gpu = gpu_available() || gpu_init_offscreen();
        const int want = settings_internal_res(gpu);
        if (want != s_hr_scale || gpu != s_gpu_on || ((want > 1 || gpu) && s_hr_w != (int)(std::max<u32>(s_hr_tbw, 8) * 64) * want)) {
            gs_sync();
            if (s_gpu_on) { own_download("renderer change"); lazy_realize("renderer change"); }
            gpu_flush();
            s_hr_scale = want; s_gpu_on = gpu;
            std::memset(s_gpu_touched, 0, sizeof s_gpu_touched); std::memset(s_touched_c, 0, sizeof s_touched_c); s_gpu_stale = true;
            s_hr_w = (int)std::max<u32>(s_hr_tbw, 8) * 64 * want; s_hr_h = 512 * want;
            s_hr_col.clear(); s_hr_col.shrink_to_fit(); s_hr_z.clear(); s_hr_z.shrink_to_fit();
            if (gpu) gpu_set_target(s_hr_w / want, s_hr_h / want, want);
            else if (want > 1) { s_hr_col.assign((size_t)s_hr_w * s_hr_h, 0); s_hr_z.assign((size_t)s_hr_w * s_hr_h, 0); }
            else s_hr_w = s_hr_h = 0;
            s_hr_valid = false;
        }
        if (s_gpu_on) { gpu_flush(); gpu_tex_trim(); gpu_set_aniso(g_settings.aniso); }
    }
    if (s_replaying) skip = false;
    { extern u64 g_gs_frame; if (s_prims_frame >= 0 && (long long)g_gs_frame + 1 >= s_prims_frame) skip = false; }   // a frame being recorded is drawn
    if (skip) { g_frames_skipped++; s_consec_skips++; } else { g_frames_drawn++; s_consec_skips = 0; }
    s_skip_draw = skip;
    fs_begin(skip);
}

void gs_vsync() {                                  // main thread (vblank)
    if (gt_active()) gt_push_vsync();
    else { extern void gs_vsync_exec(); gs_vsync_exec(); }
}
void gs_vsync_exec() {
    extern bool g_headless;
    if (gt_active() && !g_headless) {              // publish the picture for the window (main thread reads it)
        std::vector<u32> px; int w, h;
        if (gs_display_version_real() != s_snap_published && gs_get_display_real(px, w, h)) {
            s_snap_published = gs_display_version_real();
            std::lock_guard<std::mutex> lk(s_snap_mx);
            s_snap.swap(px); s_snap_w = w; s_snap_h = h; s_snap_version++;
        }
    }
    s_frames++;
    extern bool g_headless; const bool dumping = s_dumping;
    static u64 last_dump = 0;
    // frames are numbered by game frame (one per rendered frame), so scripted runs line up across speeds
    bool due = g_gs_frame / s_dump_every != last_dump / s_dump_every && g_gs_frame >= s_dump_from;
    {   // a command waiting in the autopress .once file is taken at the next frame, not the next dump
        static const char* apf = std::getenv("MVP_AUTOPRESS");
        static const std::string oncef = apf && apf[0] == '@' ? std::string(apf + 1) + ".once" : std::string();
        if (!due && !oncef.empty()) { if (FILE* f = fopen(oncef.c_str(), "rb")) { fclose(f); due = g_gs_frame >= s_dump_from; } }
    }
    if (!dumping || !due) return;
    last_dump = g_gs_frame;
    std::vector<u32> px; int w, h;
    if (!gs_get_display(px, w, h)) return;
    // MVP_FRAMES_BASE=1: frames at the game's own resolution (box-averaged from the internal resolution), so
    // scripted runs at 4x stay quick and their pixel rules keep their coordinates; full-size pictures on demand
    // with "SHOT file.png" in the autopress .once file
    static const bool base_only = std::getenv("MVP_FRAMES_BASE") != nullptr;
    std::vector<u32> full;
    const int S = s_hr_scale > 1 && w >= 1024 ? s_hr_scale : 1;
    if (S > 1) {
        full = px; const int fw = w, fh = h;
        if (base_only) {
            const int bw = fw / S, bh = fh / S; std::vector<u32> sm((size_t)bw * bh);
            for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) {
                u32 acc[4] = {0, 0, 0, 0};
                for (int j = 0; j < S; j++) for (int i = 0; i < S; i++) { const u32 v = full[(size_t)(y * S + j) * fw + x * S + i]; for (int k = 0; k < 4; k++) acc[k] += (v >> (8 * k)) & 0xff; }
                u32 o = 0; for (int k = 0; k < 4; k++) o |= (acc[k] / (S * S)) << (8 * k);
                sm[(size_t)y * bw + x] = o;
            }
            px.swap(sm); w = bw; h = bh;
        }
    }
    char path[512]; snprintf(path, sizeof path, "%s/frame_%06llu.png", std::getenv("MVP_FRAMES"), (unsigned long long)(g_gs_frame / s_dump_every * s_dump_every));
    if (s_dump_ms) snprintf(path, sizeof path, "%s/t_%07llu.png", std::getenv("MVP_FRAMES"), (unsigned long long)((host_time_ns() - s_dump_t0) / 1000000));
    if (write_png(path, px, w, h)) rlog("gs", "wrote %s (%dx%d)", path, w, h);
    // MVP_AUTOPRESS="CROSS=!100,105,a99857;TRIANGLE=20,30,ffffff&!40,50,000000" (test scripts): after each dumped frame
    // from MVP_AUTOPRESS_FROM on, the first rule whose pixel conditions all hold presses its button for a few frames.
    // A condition is x,y,rrggbb (that pixel has about that colour) or the same with ! in front (it has not).
    static const char* ap = std::getenv("MVP_AUTOPRESS");
    static const u64 ap_from = std::getenv("MVP_AUTOPRESS_FROM") ? std::strtoull(std::getenv("MVP_AUTOPRESS_FROM"), nullptr, 10) : 0;
    if (ap && g_gs_frame >= ap_from) {
        std::string rules(ap); size_t a = 0;
        if (ap[0] == '@') {                              // "@file": rules read from that file every time, so a running test can be
            rules.clear();                               // steered; "file.once" (button names) is pressed once and deleted
            if (FILE* f = fopen(ap + 1, "rb")) { char b[4096]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); rules.assign(b, n); }
            while (!rules.empty() && (rules.back() == '\n' || rules.back() == '\r' || rules.back() == ' ')) rules.pop_back();
            const std::string once = std::string(ap + 1) + ".once";
            if (FILE* f = fopen(once.c_str(), "rb")) {
                char b[128]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); std::remove(once.c_str());
                std::string names(b, n); while (!names.empty() && (names.back() == '\n' || names.back() == ' ')) names.pop_back();
                if (names == "MARK") { extern u32 g_attr_key_n, g_attr_mark; g_attr_mark = g_attr_key_n; return; }   // SHOT counts reads from here on
                if (names.compare(0, 6, "PRIMS ") == 0) { extern void gs_prims_request(const std::string&); gs_prims_request(names.substr(6)); return; }
                if (names.compare(0, 7, "DUMPDB ") == 0) { extern void db_dump_request(const std::string&); db_dump_request(names.substr(7)); return; }
                if (names.compare(0, 4, "RES ") == 0) { g_settings.internal_res = std::max(1, std::min(6, std::atoi(names.c_str() + 4))); return; }
                if (names.compare(0, 5, "SHOT ") == 0) {        // the picture at full internal resolution
                    if (full.empty()) write_png(names.c_str() + 5, px, w, h);
                    else write_png(names.c_str() + 5, full, w * (base_only ? S : 1), h * (base_only ? S : 1));
                    {   // whose record the screen was reading: the most frequent key of the last attribute reads
                        extern u32 g_attr_keys[4096]; extern u32 g_attr_key_n, g_attr_mark;
                        std::unordered_map<u32, int> cnt; const u32 n = std::min<u32>(g_attr_key_n - g_attr_mark, 4096);
                        for (u32 i = 0; i < n; i++) cnt[g_attr_keys[(g_attr_key_n - 1 - i) & 4095]]++;
                        u32 best = 0; int bc = 0; for (auto& kv : cnt) if (kv.second > bc) { bc = kv.second; best = kv.first; }
                        if (FILE* kf = fopen((std::string(names.c_str() + 5) + ".key").c_str(), "w")) { fprintf(kf, "%08x %d %u\n", best, bc, n); fclose(kf); }
                    }
                    return;
                }
                extern void pad_script_press(const char* names, u64 frames);
                u64 frames = 6; size_t sl = names.rfind('/');                    // "RX+/40": held 40 frames
                if (sl != std::string::npos && sl + 1 < names.size() && std::isdigit((unsigned char)names[sl + 1])) { frames = std::strtoull(names.c_str() + sl + 1, nullptr, 10); names.resize(sl); }
                pad_script_press(names.c_str(), frames); rules.clear();
            }
        }
        while (a < rules.size()) {
            size_t e = rules.find(';', a); if (e == std::string::npos) e = rules.size();
            std::string rule = rules.substr(a, e - a); a = e + 1;
            size_t eq = rule.find('='); if (eq == std::string::npos) continue;
            bool all = true; size_t c = eq + 1;
            while (c < rule.size() && all) {
                size_t ce = rule.find('&', c); if (ce == std::string::npos) ce = rule.size();
                std::string cond = rule.substr(c, ce - c); c = ce + 1;
                const bool neg = !cond.empty() && cond[0] == '!';
                int x = 0, y = 0; unsigned rgb = 0;
                if (sscanf(cond.c_str() + (neg ? 1 : 0), "%d,%d,%x", &x, &y, &rgb) != 3 || x < 0 || y < 0 || x >= w || y >= h) { all = false; break; }
                const u32 v = px[(size_t)y * w + x];
                const int dr = (int)(v & 0xff) - (int)((rgb >> 16) & 0xff), dg = (int)((v >> 8) & 0xff) - (int)((rgb >> 8) & 0xff), db = (int)((v >> 16) & 0xff) - (int)(rgb & 0xff);
                const bool match = std::abs(dr) < 28 && std::abs(dg) < 28 && std::abs(db) < 28;
                if (match == neg) all = false;
            }
            if (!all) continue;
            extern void pad_script_press(const char* names, u64 frames);
            pad_script_press(rule.substr(0, eq).c_str(), 6);
            break;
        }
    }
}

void gs_stats() {
    gpu_stats();
    if (s_gpu_on) fprintf(stderr, "GPU target: %llu read-backs, %llu re-uploads, %llu display copies realized\n", (unsigned long long)s_own_downloads, (unsigned long long)s_own_resyncs, (unsigned long long)s_lazy_runs);
    if (s_gpu_on) fprintf(stderr, "GPU textures: %llu uploads, %llu sent again unchanged (kept), %zu cached\n", (unsigned long long)s_gpu_tex_uploads,
                          (unsigned long long)s_gpu_tex_same, s_gpu_tex.size());
    if (!(gt_active() && !gt_on_graphics_thread())) gs_sync();
    if (g_host_timing) fprintf(stderr, "GS: inline rasterization %.2fs for %llu pixels plotted (%.1f ns/pixel)\n", s_raster_ns / 1e9, (unsigned long long)s_plotted, s_plotted ? (double)s_raster_ns / s_plotted : 0.0);
    fprintf(stderr, "GS: main thread waited %.1fs for rasterizer threads\n", s_sync_ns / 1e9);
    fprintf(stderr, "GS textures: %llu cache hits, %llu misses, %llu decodes, %zu unique (%zu MB), %llu enhanced/replaced\n",
            (unsigned long long)s_tex_hits, (unsigned long long)s_tex_misses, (unsigned long long)s_tex_decodes, s_tex_by_hash.size(), s_tex_bytes >> 20, (unsigned long long)s_enh_done.load());
    fprintf(stderr, "GS texture misses: %llu new keys, %llu changed pages; uploads: %llu changed, %llu identical\n",
            (unsigned long long)s_tex_miss_new, (unsigned long long)s_tex_miss_gen, (unsigned long long)s_up_bumps, (unsigned long long)s_up_same);
    fprintf(stderr, "GS drains: upload %llu, palette %llu, render-to-texture %llu, display %llu, full %llu, other %llu\n",
            (unsigned long long)s_sync_n[0], (unsigned long long)s_sync_n[1], (unsigned long long)s_sync_n[2],
            (unsigned long long)s_sync_n[3], (unsigned long long)s_sync_n[4], (unsigned long long)s_sync_n[5]);
    fprintf(stderr, "GS: %llu primitives, %llu pixels, %llu vsyncs\n", (unsigned long long)s_prims, (unsigned long long)s_pixels, (unsigned long long)s_frames);
    fprintf(stderr, "  textures unpacked by the fast path: %llu (%llu from kept results, %llu address tables made, %llu with new pixels, %llu new generations), held sprites drawn normally: %llu, dropped: %llu, check mismatches: %llu\n",
            (unsigned long long)s_dec_pairs, (unsigned long long)s_dec_hits, (unsigned long long)s_dec_tabs, (unsigned long long)s_dec_px, (unsigned long long)s_dec_changed, (unsigned long long)s_dec_flushed, (unsigned long long)s_dec_dropped, (unsigned long long)s_dec_bad);
    fprintf(stderr, "  primitives drawn into texture memory during skipped frames: %llu; skipped frames run again for their picture: %llu\n", (unsigned long long)s_rtt_in_skipped, (unsigned long long)s_fs_unskips);
    fprintf(stderr, "  triangles: %llu, draw state prepared %llu times (%llu register writes, %llu memory changes, %llu queue epochs)\n", (unsigned long long)s_prep_tris,
            (unsigned long long)(s_prep_n[0] + s_prep_n[1] + s_prep_n[2]), (unsigned long long)s_prep_n[0], (unsigned long long)s_prep_n[1], (unsigned long long)s_prep_n[2]);
    fprintf(stderr, "  depth-of-field backdrop: copied %llu times, layer sprites dropped %llu\n", (unsigned long long)s_dof_grabs, (unsigned long long)s_dof_dropped);
    for (u32 f = 0; f < 512; f++) if (s_rtt_by_fbp[f] > s_rtt_in_skipped / 100) fprintf(stderr, "    fbp=%05x: %llu\n", f * 32, (unsigned long long)s_rtt_by_fbp[f]);
    for (auto& t : s_targets) fprintf(stderr, "  target fbp=%05x fbw=%u psm=%02x tme=%u : %llu vertices\n", std::get<0>(t.first), std::get<1>(t.first), std::get<2>(t.first), std::get<3>(t.first), (unsigned long long)t.second);
    fprintf(stderr, "  PMODE=%llx DISPFB1=%llx DISPLAY1=%llx DISPFB2=%llx DISPLAY2=%llx SMODE2=%llx\n", (unsigned long long)s_priv[0], (unsigned long long)s_priv[7], (unsigned long long)s_priv[8], (unsigned long long)s_priv[9], (unsigned long long)s_priv[10], (unsigned long long)s_priv[2]);
    if (const char* d = std::getenv("MVP_FRAMES")) {             // dump every render target seen
        for (auto& t : s_targets) {
            u32 fbp = std::get<0>(t.first), fbw = std::get<1>(t.first), psm = std::get<2>(t.first);
            if (!fbw || fbw > 16) continue;
            int w = fbw * 64, h = 512; std::vector<u32> px((size_t)w * h);
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                u32 c = read_px(psm, fbp, fbw, x, y); if (psm == CT16 || psm == CT16S) c = c16to32(c, 0x80); px[(size_t)y * w + x] = c; }
            char path[512]; snprintf(path, sizeof path, "%s/target_%05x_%u_%02x.png", d, fbp, fbw, psm);
            write_png(path, px, w, h);
        }
    }
}

static u64 s_vram_gen_fwd() { return ++s_vram_gen; }
