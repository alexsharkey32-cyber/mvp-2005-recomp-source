// IPU: the PS2's MPEG-2 macroblock decoder (Image Processing Unit), used by Sony's libmpeg for the FMVs.
// The game's own libmpeg (recompiled EE code) parses picture/slice headers and does motion compensation;
// the IPU does bit-level work for it:
//   BCLR  clear the input FIFO          VDEC  decode one VLC symbol (MB address increment / type / motion)
//   FDEC  peek 32 bits                  BDEC  decode one macroblock: DCT coefficients, inverse quantisation,
//   SETIQ load a quantiser matrix             IDCT -> 384 16-bit samples (Y 16x16, Cb 8x8, Cr 8x8)
//   CSC   YCbCr (RAW8) -> RGB32/RGB16   PACK / SETVQ / SETTH  RGB16 / 4-bit index conversion, alpha thresholds
// The bitstream arrives through DMA channel 4 (toIPU) or the IPU input FIFO; results leave through DMA
// channel 3 (fromIPU) or the output FIFO. Commands run to completion as soon as their input is available;
// if the bitstream runs dry the command stays BUSY and is restarted from its beginning when more data comes
// (commands only commit their state on success, so a restart is exact).
#include "ee_ops.h"
#include "ee_runtime.h"
#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>

void hw_set_dma_handler(int ch, void (*h)(u32 base, u32 chcr));
u64 hw_reg(u32 addr); void hw_set_reg(u32 addr, u64 v);
void hw_dma_done(int ch);
void hw_raise_intc(int cause);

struct VlcCode { u16 code; u8 len; s16 a, b; };
#include "ipu_vlc.inc"

// ---------------------------------------------------------------- VLC lookup (16-bit window -> symbol)
struct Vlc { s16 a = 0, b = 0; u8 len = 0; };
enum { T_DCT_FIRST, T_DCT_B14, T_DCT_B15, T_MBA, T_MBI, T_MBP, T_MBB, T_CBP, T_MOTION, T_DCLUM, T_DCCHROM, T_COUNT };
static std::vector<Vlc> s_lut[T_COUNT];
static void build(int t, const VlcCode* c, size_t n) {
    s_lut[t].assign(65536, Vlc());
    for (size_t i = 0; i < n; i++) {
        u32 lo = (u32)c[i].code << (16 - c[i].len), cnt = 1u << (16 - c[i].len);
        for (u32 k = 0; k < cnt; k++) { Vlc& v = s_lut[t][lo + k]; v.a = c[i].a; v.b = c[i].b; v.len = c[i].len; }
    }
}
#define BUILD(t, arr) build(t, arr, sizeof(arr) / sizeof(arr[0]))
static void vlc_init() {
    static bool done = false; if (done) return; done = true;
    BUILD(T_DCT_FIRST, kDctB14First); BUILD(T_DCT_B14, kDctB14); BUILD(T_DCT_B15, kDctB15);
    BUILD(T_MBA, kMba); BUILD(T_MBI, kMbI); BUILD(T_MBP, kMbP); BUILD(T_MBB, kMbB); BUILD(T_CBP, kCbp);
    BUILD(T_MOTION, kMotion); BUILD(T_DCLUM, kDcLum); BUILD(T_DCCHROM, kDcChrom);
}

static const u8 kZigzag[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
static const u8 kAltScan[64] = {0, 8, 16, 24, 1, 9, 2, 10, 17, 25, 32, 40, 48, 56, 57, 49, 41, 33, 26, 18, 3, 11, 4, 12, 19, 27, 34, 42, 50, 58, 35, 43,
    51, 59, 20, 28, 5, 13, 6, 14, 21, 29, 36, 44, 52, 60, 37, 45, 53, 61, 22, 30, 7, 15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63};
static const u8 kNonLinearQ[32] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 20, 22, 24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112};
static const u8 kDefaultIntra[64] = {8, 16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58, 26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83};

// ---------------------------------------------------------------- state
static std::vector<u8> s_in;          // input: whole quadwords (internal buffer + FIFO), s_in[0..] = oldest
static u32 s_bp = 0;                  // bits consumed in the first quadword
static std::vector<u8> s_out; static size_t s_out_rd = 0;   // output FIFO (bytes, quadword multiples)
static u32 s_ctrl = 0;                // IPU_CTRL bits 16..30 (IDP, AS, IVF, QST, MP1, PCT) + ECD/SCD/CBP
static u32 s_cmd = 0, s_data = 0, s_top = 0;
static bool s_busy = false, s_topbusy = false, s_pending = false;
static u8 s_iq[64], s_niq[64];        // quantiser matrices (natural order)
static s32 s_dc_pred[3] = {128, 128, 128};
static u16 s_vqclut[16]; static u32 s_th0 = 0, s_th1 = 0;
static u64 s_stat_cmds[10], s_stat_mb = 0;
static const bool s_log = std::getenv("MVP_IPULOG") != nullptr; static int s_log_n = 0;
#define IPULOG(...) do { if (s_log && s_log_n++ < 400000) fprintf(stderr, __VA_ARGS__); } while (0)

// toIPU DMA (channel 4) as a data source, fromIPU (channel 3) as a sink
struct DmaSrc { bool active = false, chain = false, end = false; u32 madr = 0, qwc = 0, tadr = 0, chcr = 0; u32 asr[2] = {0, 0}; int sp = 0; };
static DmaSrc s_to;
static const u32 TO_BASE = 0x1000b400, FROM_BASE = 0x1000b000;

struct Underflow {};
// DMA addresses: bit 31 selects the scratchpad
static inline u8* dma_ptr(u32 a) { return (a & 0x80000000u) ? g_spr + (a & 0x3ff0) : ee_ptr(a & 0x7fffffffu); }
static inline u32 ctrl_field(int lo, int n) { return (s_ctrl >> lo) & ((1u << n) - 1); }

// ---------------------------------------------------------------- input
static void to_finish() {
    s_to.active = false;
    hw_set_reg(TO_BASE + 0x10, s_to.madr); hw_set_reg(TO_BASE + 0x20, 0); hw_set_reg(TO_BASE + 0x30, s_to.tadr);
    hw_set_reg(TO_BASE, s_to.chcr & ~0x100u);
    hw_dma_done(4);
}
// pull one quadword from the toIPU DMA; false if it has nothing (more) to give
static bool to_pull() {
    for (int guard = 0; guard < 64; guard++) {
        if (!s_to.active) return false;
        if (s_to.qwc) {
            u8* p = dma_ptr(s_to.madr);
            size_t o = s_in.size(); if (s_in.capacity() < o + 16) s_in.reserve(std::max<size_t>(o * 2, 4096)); s_in.resize(o + 16);
            if (p) std::memcpy(&s_in[o], p, 16); else std::memset(&s_in[o], 0, 16);
            s_to.madr += 16; s_to.qwc--;
            if (!s_to.qwc && (!s_to.chain || s_to.end)) to_finish();
            return true;
        }
        if (!s_to.chain || s_to.end) { to_finish(); return false; }
        // source chain: next tag
        u8* tp = dma_ptr(s_to.tadr);
        u64 tag = 0; if (tp) std::memcpy(&tag, tp, 8);
        u32 qwc = (u32)(tag & 0xffff), id = (u32)(tag >> 28) & 7, addr = (u32)(tag >> 32) & 0x7ffffff0u;
        bool irq = (tag >> 31) & 1;
        s_to.chcr = (s_to.chcr & 0xffff) | ((u32)(tag >> 16) & 0xffff0000u);
        switch (id) {
        case 0: s_to.madr = addr; s_to.qwc = qwc; s_to.tadr += 16; s_to.end = true; break;               // refe
        case 1: s_to.madr = s_to.tadr + 16; s_to.qwc = qwc; s_to.tadr = s_to.madr + qwc * 16; break;     // cnt
        case 2: s_to.madr = s_to.tadr + 16; s_to.qwc = qwc; s_to.tadr = addr; break;                      // next
        case 3: case 4: s_to.madr = addr; s_to.qwc = qwc; s_to.tadr += 16; break;                        // ref / refs
        case 5: s_to.madr = s_to.tadr + 16; s_to.qwc = qwc;                                               // call
                if (s_to.sp < 2) s_to.asr[s_to.sp++] = s_to.madr + qwc * 16; s_to.tadr = addr; break;
        case 6: s_to.madr = s_to.tadr + 16; s_to.qwc = qwc;                                               // ret
                if (s_to.sp > 0) s_to.tadr = s_to.asr[--s_to.sp]; else s_to.end = true; break;
        default: s_to.madr = s_to.tadr + 16; s_to.qwc = qwc; s_to.end = true; break;                      // end
        }
        if (irq && (s_to.chcr & 0x80)) s_to.end = true;                                                   // TIE
        if (!s_to.qwc && s_to.end) { to_finish(); return false; }
    }
    return false;
}
// keep the FIFO topped up like the hardware does (2 internal quadwords + 8 in the FIFO)
static void to_fill() { while (s_in.size() < 10 * 16 && to_pull()) {} }
static void to_sync_regs() {                        // channel registers as the EE sees them
    if (!s_to.active) return;
    hw_set_reg(TO_BASE + 0x10, s_to.madr); hw_set_reg(TO_BASE + 0x20, s_to.qwc); hw_set_reg(TO_BASE + 0x30, s_to.tadr);
}

// bit reader over s_in for one command; commits on success
struct Bits {
    size_t pos;                          // absolute bit position in s_in
    Bits() : pos(s_bp) {}
    void need(u32 n) { while (pos + n > s_in.size() * 8) if (!to_pull()) throw Underflow(); }
    u32 peek(u32 n) {                    // n <= 32
        if (!n) return 0;
        need(n);
        u64 v = 0; size_t byte = pos >> 3; u32 off = pos & 7;
        for (int i = 0; i < 5; i++) v = (v << 8) | (byte + i < s_in.size() ? s_in[byte + i] : 0);
        return (u32)((v << (24 + off)) >> (64 - n));
    }
    u32 peek_avail(u32 n) {              // like peek, zero-filled past the end (VLC windows near the end)
        while (pos + n > s_in.size() * 8 && to_pull()) {}
        u64 v = 0; size_t byte = pos >> 3; u32 off = pos & 7;
        for (int i = 0; i < 5; i++) v = (v << 8) | (byte + i < s_in.size() ? s_in[byte + i] : 0);
        return (u32)((v << (24 + off)) >> (64 - n));
    }
    void skip(u32 n) { need(n); pos += n; }
    void bytes(u8* dst, u32 n) {         // n whole bytes (fast when byte-aligned, which CSC/SETIQ data always is)
        need(n * 8);
        if (!(pos & 7)) { std::memcpy(dst, &s_in[pos >> 3], n); pos += (size_t)n * 8; return; }
        for (u32 i = 0; i < n; i++) dst[i] = (u8)get(8);
    }
    u32 get(u32 n) { u32 v = peek(n); pos += n; return v; }
    s32 sget(u32 n) { u32 v = get(n); return (s32)(v << (32 - n)) >> (32 - n); }
    const Vlc& vlc(int t) {
        const Vlc& e = s_lut[t][peek_avail(16)];
        if (!e.len) return e;
        skip(e.len);
        return e;
    }
    void commit() { size_t q = pos / 128; s_in.erase(s_in.begin(), s_in.begin() + std::min(q * 16, s_in.size())); s_bp = (u32)(pos % 128); }
};

// ---------------------------------------------------------------- output
static void out_push(const void* p, size_t n) { const u8* b = (const u8*)p; s_out.insert(s_out.end(), b, b + n); }
static size_t out_qwords() { return (s_out.size() - s_out_rd) / 16; }
static void out_compact() { if (s_out_rd && s_out_rd == s_out.size()) { s_out.clear(); s_out_rd = 0; } else if (s_out_rd > (1 << 20)) { s_out.erase(s_out.begin(), s_out.begin() + s_out_rd); s_out_rd = 0; } }
static void from_drain() {                        // fromIPU DMA: move output quadwords to memory
    u32 chcr = (u32)hw_reg(FROM_BASE);
    if (!(chcr & 0x100)) return;
    u32 madr = (u32)hw_reg(FROM_BASE + 0x10), qwc = (u32)hw_reg(FROM_BASE + 0x20);
    while (qwc && out_qwords()) {
        if (u8* p = dma_ptr(madr)) std::memcpy(p, &s_out[s_out_rd], 16);
        s_out_rd += 16; madr += 16; qwc--;
    }
    out_compact();
    hw_set_reg(FROM_BASE + 0x10, madr); hw_set_reg(FROM_BASE + 0x20, qwc);
    if (!qwc) { hw_set_reg(FROM_BASE, chcr & ~0x100u); hw_dma_done(3); }
}

// ---------------------------------------------------------------- IDCT
// Separable 8x8 inverse DCT in single precision (well within IEEE 1180 accuracy for 8-bit video); rows
// that are entirely zero (most of them) are skipped, and DC-only blocks take a shortcut.
static float s_cos[8][8];
static void idct_init() {
    static bool done = false; if (done) return; done = true;
    for (int x = 0; x < 8; x++) for (int u = 0; u < 8; u++) s_cos[x][u] = (float)((u ? 1.0 : std::sqrt(0.5)) * 0.5 * std::cos((2 * x + 1) * u * M_PI / 16.0));
}
static void idct(const s32* in, s32* out) {
    bool dc_only = true;
    for (int k = 1; k < 64 && dc_only; k++) if (in[k]) dc_only = false;
    if (dc_only) {
        s32 v = (s32)std::floor(in[0] * 0.125f + 0.5f);
        for (int k = 0; k < 64; k++) out[k] = v;
        return;
    }
    float tmp[64];
    for (int y = 0; y < 8; y++) {                                       // rows
        const s32* r = in + y * 8;
        float* t = tmp + y * 8;
        if (!(r[0] | r[1] | r[2] | r[3] | r[4] | r[5] | r[6] | r[7])) { for (int x = 0; x < 8; x++) t[x] = 0; continue; }
        for (int x = 0; x < 8; x++) {
            const float* c = s_cos[x];
            t[x] = c[0] * r[0] + c[1] * r[1] + c[2] * r[2] + c[3] * r[3] + c[4] * r[4] + c[5] * r[5] + c[6] * r[6] + c[7] * r[7];
        }
    }
    for (int x = 0; x < 8; x++) for (int y = 0; y < 8; y++) {           // columns
        const float* c = s_cos[y];
        float s = c[0] * tmp[x] + c[1] * tmp[8 + x] + c[2] * tmp[16 + x] + c[3] * tmp[24 + x] + c[4] * tmp[32 + x] + c[5] * tmp[40 + x] + c[6] * tmp[48 + x] + c[7] * tmp[56 + x];
        out[y * 8 + x] = (s32)std::floor(s + 0.5f);
    }
}

// ---------------------------------------------------------------- BDEC
static inline s32 sat(s32 v) { return v < -2048 ? -2048 : v > 2047 ? 2047 : v; }
// one 8x8 block: coefficients in natural order into blk (already holding the intra DC if any)
static void decode_block(Bits& b, s32* blk, bool intra, int qscale) {
    const bool mpeg1 = ctrl_field(23, 1), ivf = ctrl_field(21, 1) && !mpeg1, alt = ctrl_field(20, 1);
    const u8* scan = alt ? kAltScan : kZigzag;
    const u8* W = intra ? s_iq : s_niq;
    int i = intra ? 1 : 0;
    for (;; i++) {
        const int t = intra ? (ivf ? T_DCT_B15 : T_DCT_B14) : (i == 0 ? T_DCT_FIRST : T_DCT_B14);
        const Vlc& e = b.vlc(t);
        if (!e.len) break;                                   // invalid code: end the block
        if (e.a == 64) break;                                // end of block
        s32 level; bool neg;
        if (e.a == 65) {                                     // escape: 6-bit run + level
            i += b.get(6);
            if (!mpeg1) { level = b.sget(12); }
            else { level = b.sget(8); if (level == -128) level = (s32)b.get(8) - 256; else if (level == 0) level = (s32)b.get(8); }
        } else { i += e.a; level = e.b; if (b.get(1)) level = -level; }
        if (i >= 64) break;
        const int j = scan[i];
        neg = level < 0; s32 mag = neg ? -level : level;
        s32 val = intra ? (mag * qscale * W[j]) >> 4 : ((2 * mag + 1) * qscale * W[j]) >> 5;
        if (mpeg1 && val) val = (val - 1) | 1;               // MPEG-1 oddification
        blk[j] = sat(neg ? -val : val);
    }
    if (!mpeg1) {                                            // MPEG-2 mismatch control
        s32 sum = 0; for (int k = 0; k < 64; k++) sum += blk[k];
        if (!(sum & 1)) blk[63] ^= 1;
    }
}
static s32 dc_diff(Bits& b, bool luma) {
    const Vlc& e = b.vlc(luma ? T_DCLUM : T_DCCHROM);
    int size = e.a;
    if (!size) return 0;
    s32 d = (s32)b.get(size);
    if (!(d & (1 << (size - 1)))) d -= (1 << size) - 1;
    return d;
}
static void cmd_bdec(u32 v) {
    const bool mbi = (v >> 27) & 1, dcr = (v >> 26) & 1, dt = (v >> 25) & 1;
    const u32 qsc = (v >> 16) & 31;
    const int qscale = ctrl_field(22, 1) ? kNonLinearQ[qsc] : (int)qsc * 2;
    const int idp = ctrl_field(16, 2);
    Bits b; b.skip(v & 63); b.need(32);
    s32 dc[3] = {s_dc_pred[0], s_dc_pred[1], s_dc_pred[2]};
    if (dcr) dc[0] = dc[1] = dc[2] = 128 << idp;
    s16 mb[384]; std::memset(mb, 0, sizeof mb);
    u32 cbp = 0x3f;
    if (!mbi) { const Vlc& e = b.vlc(T_CBP); cbp = e.len ? (u32)e.a : 0; }
    for (int k = 0; k < 6; k++) {
        if (!(cbp & (0x20 >> k))) continue;
        s32 blk[64] = {}, px[64];
        if (mbi) { int cc = k < 4 ? 0 : k - 3; dc[cc] += dc_diff(b, k < 4); blk[0] = dc[cc] << (3 - idp); }
        decode_block(b, blk, mbi, qscale);
        idct(blk, px);
        s16* dst; int stride;
        if (k < 4) {
            if (dt) { dst = mb + (k & 1) * 8 + (k >> 1) * 16; stride = 32; }         // field DCT: alternate lines
            else { dst = mb + (k & 1) * 8 + (k >> 1) * 128; stride = 16; }
        } else { dst = mb + 256 + (k - 4) * 64; stride = 8; }
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            s32 p = px[y * 8 + x];
            if (mbi) p = p < 0 ? 0 : p > 255 ? 255 : p;
            else p = p < -32768 ? -32768 : p > 32767 ? 32767 : p;
            dst[y * stride + x] = (s16)p;
        }
    }
    // after the macroblock: a zero byte means the slice ended -> look for the next start code
    u32 scd = 0, ecd = 0;
    if (b.peek_avail(8) == 0) {
        size_t save = b.pos;
        b.pos = (b.pos + 7) & ~(size_t)7;
        for (int guard = 0; guard < 65536; guard++) {
            b.need(24);
            u32 w = b.peek(24);
            if (w) { if (w == 1) scd = 1; else ecd = 1; break; }
            b.pos += 8;
        }
        (void)save;
    }
    u32 top = b.peek_avail(32);
    // success: commit
    s_dc_pred[0] = dc[0]; s_dc_pred[1] = dc[1]; s_dc_pred[2] = dc[2];
    b.commit();
    { s64 sum = 0; for (int i = 0; i < 384; i++) sum += mb[i] < 0 ? -mb[i] : mb[i]; IPULOG("[ipu] BDEC %s cbp=%02x q=%d abs-sum %lld scd=%u ecd=%u\n", mbi ? "intra" : "inter", cbp, qscale, (long long)sum, scd, ecd); }
    out_push(mb, sizeof mb);
    s_ctrl = (s_ctrl & ~(0x3fu << 8) & ~(3u << 14)) | (cbp << 8) | (ecd << 14) | (scd << 15);
    s_top = top;
    s_stat_mb++;
}

// ---------------------------------------------------------------- VDEC / FDEC / SETIQ / SETVQ
static void cmd_vdec(u32 v) {
    Bits b; b.skip(v & 63);
    b.need(32);
    u32 data = 0;
    switch ((v >> 26) & 3) {
    case 0: {                                                // macroblock_address_increment
        const Vlc& e = b.vlc(T_MBA);
        if (e.len && (e.a != 0x22 || ctrl_field(23, 1))) data = (u32)e.a | ((u32)e.len << 16);
        else if (e.len) { b.pos -= e.len; }                  // stuffing is MPEG-1 only
        break;
    }
    case 1: {                                                // macroblock_type (by picture type)
        u32 pct = ctrl_field(24, 3); if (!pct) pct = 1;
        if (pct == 4) { data = b.get(1) ? (1u | (1u << 16)) : 0; break; }
        const Vlc& e = b.vlc(pct == 1 ? T_MBI : pct == 2 ? T_MBP : T_MBB);
        if (!e.len) break;
        data = (u32)e.a;
        if (pct == 2 && (data & 8)) data |= 128;             // frame motion (as the IPU reports it)
        if (pct == 3) data |= 128 | ((u32)e.len << 16);
        break;
    }
    case 2: {                                                // motion_code
        const Vlc& e = b.vlc(T_MOTION);
        if (!e.len) break;
        if (e.a == 0) { data = 1u << 16; break; }
        s32 m = e.a; if (b.get(1)) m = -m;
        data = (u32)m | ((u32)e.len << 16);                  // negative values keep their sign bits (as the IPU reports)
        break;
    }
    default: {                                               // dmvector: 0 -> 0, 10 -> +1, 11 -> -1
        u32 c = b.get(1);
        if (!c) data = 0 | (1u << 16);
        else data = b.get(1) ? 0xffffffffu : (1u | (2u << 16));
        break;
    }
    }
    u32 top = b.peek_avail(32);
    b.commit();
    s_data = data; s_top = top;
    IPULOG("[ipu] VDEC tbl %u pct %u -> %08x\n", (v >> 26) & 3, ctrl_field(24, 3), data);
    s_ctrl = (s_ctrl & ~(1u << 14)) | ((data == 0 ? 1u : 0u) << 14);
}
static void cmd_fdec(u32 v) {
    Bits b; b.skip(v & 63);
    u32 d = b.peek(32);
    b.commit();
    s_data = s_top = d;
    IPULOG("[ipu] FDEC -> %08x\n", d);
}
static void cmd_setiq(u32 v) {
    Bits b; b.skip(v & 63);
    u8 m[64]; b.bytes(m, 64);
    b.commit();
    u8* dst = ((v >> 27) & 1) ? s_niq : s_iq;
    for (int i = 0; i < 64; i++) dst[kZigzag[i]] = m[i];
}
static void cmd_setvq() {
    Bits b;
    u16 c[16]; for (int i = 0; i < 16; i++) { u32 lo = b.get(8), hi = b.get(8); c[i] = (u16)(lo | (hi << 8)); }
    b.commit();
    std::memcpy(s_vqclut, c, sizeof c);
}

// ---------------------------------------------------------------- CSC / PACK
static void ycbcr_to_rgb32(const u8* raw, u32* rgb) {
    const u8* Y = raw; const u8* Cb = raw + 256; const u8* Cr = raw + 320;
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
        s32 cb = (s32)Cb[(y >> 1) * 8 + (x >> 1)] - 128, cr = (s32)Cr[(y >> 1) * 8 + (x >> 1)] - 128;
        s32 lum = (0x95 * std::max(0, (s32)Y[y * 16 + x] - 16)) >> 6;
        s32 r = (lum + ((0xcc * cr) >> 6) + 1) >> 1, g = (lum + ((-0x68 * cr) >> 6) + ((-0x32 * cb) >> 6) + 1) >> 1, bl = (lum + ((0x102 * cb) >> 6) + 1) >> 1;
        r = std::min(255, std::max(0, r)); g = std::min(255, std::max(0, g)); bl = std::min(255, std::max(0, bl));
        u32 a = 0x80;
        if (s_th0 && r < (s32)s_th0 && g < (s32)s_th0 && bl < (s32)s_th0) { rgb[y * 16 + x] = 0; continue; }
        if (s_th1 && r < (s32)s_th1 && g < (s32)s_th1 && bl < (s32)s_th1) a = 0x40;
        rgb[y * 16 + x] = (u32)r | ((u32)g << 8) | ((u32)bl << 16) | (a << 24);
    }
}
static void rgb32_to_rgb16(const u32* rgb, u16* out, bool dither) {
    static const int dm[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
        u32 c = rgb[y * 16 + x]; int d = dither ? dm[y & 3][x & 3] : 0;
        int r = std::min(255, std::max(0, (int)(c & 0xff) + d)), g = std::min(255, std::max(0, (int)((c >> 8) & 0xff) + d)), b = std::min(255, std::max(0, (int)((c >> 16) & 0xff) + d));
        out[y * 16 + x] = (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | (((c >> 24) == 0x40) ? 0x8000 : 0));
    }
}
static void cmd_csc(u32 v) {
    const u32 mbc = v & 0x7ff; const bool dte = (v >> 26) & 1, ofm = (v >> 27) & 1;
    Bits b;
    std::vector<u8> out;
    for (u32 m = 0; m < mbc; m++) {
        u8 raw[384]; b.bytes(raw, 384);
        u32 rgb[256]; ycbcr_to_rgb32(raw, rgb);
        if (ofm) { u16 p[256]; rgb32_to_rgb16(rgb, p, dte); out.insert(out.end(), (u8*)p, (u8*)p + 512); }
        else out.insert(out.end(), (u8*)rgb, (u8*)rgb + 1024);
    }
    b.commit();
    { u64 sum = 0; for (size_t i = 0; i < out.size(); i += 4) sum += out[i + 1]; IPULOG("[ipu] CSC %u mbs, rgb g-sum %llu\n", mbc, (unsigned long long)sum); }
    out_push(out.data(), out.size());
    s_stat_mb += mbc;
}
static void cmd_pack(u32 v) {
    const u32 mbc = v & 0x7ff; const bool dte = (v >> 26) & 1, ofm = (v >> 27) & 1;
    Bits b;
    std::vector<u8> out;
    for (u32 m = 0; m < mbc; m++) {
        u32 rgb[256]; b.bytes((u8*)rgb, 1024);
        u16 p[256]; rgb32_to_rgb16(rgb, p, dte);
        if (ofm) out.insert(out.end(), (u8*)p, (u8*)p + 512);
        else {                                               // INDX4: nearest VQ CLUT entry
            u8 ix[128] = {};
            for (int i = 0; i < 256; i++) {
                int best = 0, bd = 1 << 30;
                for (int k = 0; k < 16; k++) {
                    int dr = (p[i] & 31) - (s_vqclut[k] & 31), dg = ((p[i] >> 5) & 31) - ((s_vqclut[k] >> 5) & 31), db = ((p[i] >> 10) & 31) - ((s_vqclut[k] >> 10) & 31);
                    int d = dr * dr + dg * dg + db * db; if (d < bd) { bd = d; best = k; }
                }
                ix[i >> 1] |= (u8)(best << ((i & 1) * 4));
            }
            out.insert(out.end(), ix, ix + 128);
        }
    }
    b.commit();
    out_push(out.data(), out.size());
}

// ---------------------------------------------------------------- command dispatch
static u64 s_ipu_ns = 0;
static bool run_command() {                    // true if finished
    const u32 v = s_cmd;
    struct T { u64 t0 = host_time_ns(); ~T() { s_ipu_ns += host_time_ns() - t0; } } timer;
    try {
        switch (v >> 28) {
        case 1: rlog("ipu", "IDEC is not implemented (cmd %08x)", v); break;
        case 2: cmd_bdec(v); break;
        case 3: cmd_vdec(v); break;
        case 4: cmd_fdec(v); break;
        case 5: cmd_setiq(v); break;
        case 6: cmd_setvq(); break;
        case 7: cmd_csc(v); break;
        case 8: cmd_pack(v); break;
        default: break;
        }
    } catch (Underflow&) { return false; }
    return true;
}
static void finish_command() {
    s_busy = false; s_topbusy = false; s_pending = false;
    to_fill(); to_sync_regs();
    from_drain();
    hw_raise_intc(8);
}
static void try_pending() {
    if (!s_pending) return;
    if (run_command()) finish_command();
}
static void write_cmd(u32 v) {
    vlc_init(); idct_init();
    s_ctrl &= ~(3u << 14);                                    // ECD, SCD
    s_cmd = v;
    const u32 op = v >> 28;
    if (op < 10) s_stat_cmds[op]++;
    IPULOG("[ipu] cmd %08x (in %zu bytes, bp %u, out %zu qw) to: act=%d madr=%08x qwc=%u\n", v, s_in.size(), s_bp, out_qwords(), (int)s_to.active, s_to.madr, s_to.qwc);
    if (op == 0) {                                            // BCLR
        s_in.clear(); s_bp = v & 0x7f; s_busy = false; s_pending = false;
        hw_raise_intc(8);
        return;
    }
    if (op == 9) { s_th0 = v & 0x1ff; s_th1 = (v >> 16) & 0x1ff; s_busy = false; hw_raise_intc(8); return; }   // SETTH
    s_busy = true; s_topbusy = true; s_pending = true;
    to_fill();
    try_pending();
    to_sync_regs();
}
static void soft_reset() {
    s_in.clear(); s_bp = 0; s_out.clear(); s_out_rd = 0;
    s_busy = s_topbusy = s_pending = false; s_data = s_top = 0; s_th0 = s_th1 = 0;
    s_ctrl &= 0x7f33f00 & ~(0x3fu << 8);
    const int idp = ctrl_field(16, 2);
    s_dc_pred[0] = s_dc_pred[1] = s_dc_pred[2] = 128 << idp;
    hw_raise_intc(8);
}

// ---------------------------------------------------------------- registers / FIFOs / DMA
static u32 fifo_counts(u32& fp) {                             // quadwords: internal buffer (0-2) and FIFO (0-8)
    u32 q = (u32)(s_in.size() / 16);
    fp = std::min<u32>(q, 2);
    return std::min<u32>(q - fp, 8);
}
u64 ipu_read(u32 a) {
    vlc_init(); idct_init();
    switch (a & 0xf0) {
    case 0x00: {                                              // IPU_CMD
        u32 op = s_cmd >> 28;
        if (!s_busy && op != 3 && op != 4) { Bits b; u32 w = b.peek_avail(32); s_data = w; }
        return (u64)s_data | ((u64)(s_busy ? 1 : 0) << 63);
    }
    case 0x10: {                                              // IPU_CTRL
        u32 fp, ifc = fifo_counts(fp);
        u32 ofc = (u32)std::min<size_t>(out_qwords(), 8);
        return (s_ctrl & ~0xffu & 0x7fffffffu) | ifc | (ofc << 4) | ((s_busy ? 1u : 0u) << 31);
    }
    case 0x20: { u32 fp, ifc = fifo_counts(fp); return (u64)(s_bp & 0x7f) | (ifc << 8) | (fp << 16); }   // IPU_BP
    default: {                                                // IPU_TOP: next 32 bits (busy until available)
        if (!s_busy) {
            Bits b;
            try { s_top = b.peek(32); s_topbusy = false; } catch (Underflow&) { s_topbusy = true; }
        }
        return (u64)s_top | ((u64)(s_topbusy ? 1 : 0) << 63);
    }
    }
}
void ipu_write(u32 a, u64 v) {
    switch (a & 0xf0) {
    case 0x00: write_cmd((u32)v); break;
    case 0x10: {
        u32 w = (u32)v;
        s_ctrl = (w & 0x47f30000u) | (s_ctrl & 0x8000ffffu);
        if (ctrl_field(16, 2) == 3) s_ctrl = (s_ctrl & ~(3u << 16)) | (1u << 16);
        if (w & (1u << 30)) soft_reset();
        break;
    }
    default: break;
    }
}
void ipu_fifo_write(const u32* qw) {                          // 0x10007010: input FIFO
    size_t o = s_in.size(); s_in.resize(o + 16); std::memcpy(&s_in[o], qw, 16);
    try_pending();
}
void ipu_fifo_read(u32* qw) {                                 // 0x10007000: output FIFO
    if (out_qwords()) { std::memcpy(qw, &s_out[s_out_rd], 16); s_out_rd += 16; out_compact(); }
    else std::memset(qw, 0, 16);
}
static void to_dma(u32 base, u32 chcr) {                      // DMA channel 4 started
    (void)base;
    s_to = DmaSrc();
    s_to.active = true; s_to.chcr = chcr;
    s_to.madr = (u32)hw_reg(TO_BASE + 0x10); s_to.qwc = (u32)hw_reg(TO_BASE + 0x20); s_to.tadr = (u32)hw_reg(TO_BASE + 0x30);
    s_to.chain = ((chcr >> 2) & 3) == 1;
    if (!s_to.chain && !s_to.qwc) { to_finish(); return; }
    to_fill(); to_sync_regs();
    try_pending();
}
static void from_dma(u32 base, u32 chcr) { (void)base; (void)chcr; from_drain(); try_pending(); from_drain(); }   // channel 3 started
void ipu_init() {
    hw_set_dma_handler(3, from_dma);
    hw_set_dma_handler(4, to_dma);
    std::memcpy(s_iq, kDefaultIntra, 64); std::memset(s_niq, 16, 64);
}
bool ipu_dma_is_async(int ch) { return ch == 3 || ch == 4; }
void ipu_stats() {
    if (!s_stat_mb && !s_stat_cmds[3]) return;
    fprintf(stderr, "IPU: %.2f s decoding\n", s_ipu_ns / 1e9);
    fprintf(stderr, "IPU: %llu macroblocks; commands BDEC %llu VDEC %llu FDEC %llu SETIQ %llu CSC %llu BCLR %llu\n", (unsigned long long)s_stat_mb,
            (unsigned long long)s_stat_cmds[2], (unsigned long long)s_stat_cmds[3], (unsigned long long)s_stat_cmds[4], (unsigned long long)s_stat_cmds[5],
            (unsigned long long)s_stat_cmds[7], (unsigned long long)s_stat_cmds[0]);
}
