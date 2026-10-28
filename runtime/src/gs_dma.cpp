// Graphics DMA paths: VIF1 (-> VU1) and GIF (-> GS).
//
// VIF1 is a resumable state machine because EAGL synchronises VIF1 and GIF (PATH3) with interrupts:
// a VIFcode with the I bit raises INTC 5 and stalls VIF1 until the handler writes VIF1_FBRST.STC,
// and GIF chains stop on tags with the IRQ bit (TIE) until the DMAC handler restarts them.
#include "ee_ops.h"
#include "ee_runtime.h"
#include "gs.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

void kernel_raise_intc(int cause);
void kernel_raise_dmac(int ch);
// Graphics thread (see "graphics thread" below): these forward to the main thread when called from it.
void gt_raise_intc(int cause);
void gt_set_reg(u32 addr, u64 v);
void gt_dma_done(int ch);
void hw_set_dma_handler(int ch, void (*h)(u32 base, u32 chcr));
u64 hw_reg(u32 addr);
void hw_set_reg(u32 addr, u64 v);
extern u32 g_dma_last_tag_hi;

u8 g_vu1_mem[0x4000];      // VU1 data memory (16 KB)
u8 g_vu1_code[0x4000];     // VU1 micro memory (16 KB)
static u64 s_gif_qwords = 0, s_vif1_qwords = 0, s_gif_kicks = 0, s_vu1_calls = 0;

static inline const u8* dma_mem(u32 a) {
    return (a & 0x80000000u) ? g_spr + (a & 0x3ff0) : ee_ptr(a & 0x01fffff0);
}

// ---------------------------------------------------------------- VIF1 state
// Word FIFO backed by a vector with a read index: VIFcode payloads are consumed in place (no per-word
// deque operations, no copies).
struct WordFifo {
    std::vector<u32> buf; size_t rd = 0;
    bool empty() const { return rd >= buf.size(); }
    size_t size() const { return buf.size() - rd; }
    u32 front() const { return buf[rd]; }
    void pop_front() { rd++; compact(); }
    void skip(size_t n) { rd += n; compact(); }
    void push_back(u32 w) { buf.push_back(w); }
    void append(const u32* w, size_t n) { buf.insert(buf.end(), w, w + n); }
    const u32* at(size_t i) const { return &buf[rd + i]; }
    void clear() { buf.clear(); rd = 0; }
    void compact() { if (rd >= buf.size()) { buf.clear(); rd = 0; } else if (rd > 65536 && rd * 2 > buf.size()) { buf.erase(buf.begin(), buf.begin() + rd); rd = 0; } }
};
struct Vif1 {
    u32 cl = 1, wl = 1, mask = 0, mode = 0, itop = 0, itops = 0, base = 0, ofst = 0, tops = 0, top = 0, dbf = 0;
    u32 row[4] = {}, col[4] = {};
    u32 code = 0, mark = 0, err = 0;
    bool mrk = false;          // STAT.MRK: a MARK VIFcode ran since the CPU last wrote the MARK register
    bool stalled = false;      // stopped after an I-bit VIFcode until FBRST.STC
    bool pending_int = false;
    WordFifo fifo;             // words received from DMA, not yet consumed
};
static Vif1 v1;

void gt_sync();
u32 vif1_read_reg(u32 a) {
    gt_sync();                                       // VIF1 state lives on the graphics thread
    switch (a & 0xff0) {
        case 0xc00: return (v1.stalled ? (1u << 10) | (1u << 11) : 0) | (v1.dbf << 7) | (v1.mrk ? 1u << 6 : 0);   // STAT: VIS/INT, DBF, MRK
        case 0xc20: return v1.err;                                   // ERR
        case 0xc30: return v1.mark;                                  // MARK (the game's frame profiler indexes a table with it)
        case 0xc40: return v1.cl | (v1.wl << 8);                     // CYCLE
        case 0xc50: return v1.mode;  case 0xc70: return v1.mask;     // MODE, MASK (0xc60 NUM reads 0 between packets)
        case 0xc80: return v1.code;
        case 0xc90: return v1.itops; case 0xca0: return v1.base; case 0xcb0: return v1.ofst;
        case 0xcc0: return v1.tops; case 0xcd0: return v1.itop; case 0xce0: return v1.top;
    }
    if ((a & 0xfc0) == 0xd00) return v1.row[(a >> 4) & 3];           // R0-R3
    if ((a & 0xfc0) == 0xd40) return v1.col[(a >> 4) & 3];           // C0-C3
    return 0;
}

static void vif1_dma_pump();
void vif1_run_pub();
void gt_push_vif1_reg(u32 a, u32 v);
void vif1_write_reg(u32 a, u32 v) { gt_push_vif1_reg(a, v); }
static void vif1_write_reg_exec(u32 a, u32 v) {
    if ((a & 0xff0) == 0xc10) {                       // FBRST
        if (v & 1) { v1.fifo.clear(); v1.stalled = false; }   // RST
        if (v & 8) {                                   // STC: cancel stall, resume
            v1.stalled = false;
            vif1_dma_pump();
        }
    } else if ((a & 0xff0) == 0xc20) v1.err = v;
    else if ((a & 0xff0) == 0xc30) { v1.mark = v & 0xffff; v1.mrk = false; }
}

u32 vif1_top() { return v1.top; }
u32 vif1_itop() { return v1.itop; }
void vif1_set_tops(u32 top, u32 itop) { v1.top = top; v1.itop = itop; }      // --vu1-bench
static void vif1_fifo_write_exec(const u32* w) { v1.fifo.append(w, 4); vif1_run_pub(); }
void gt_push_data(int kind, const u32* w);
void vif1_fifo_write(const u32* w) { gt_push_data(0, w); }

// ---------------------------------------------------------------- UNPACK
static inline u32 elem(const u8* d, int bits, int i, bool usn) {
    switch (bits) {
        case 32: { u32 x; std::memcpy(&x, d + i * 4, 4); return x; }
        case 16: { u16 x; std::memcpy(&x, d + i * 2, 2); return usn ? (u32)x : (u32)(s32)(s16)x; }
        default: { u8 x = d[i]; return usn ? (u32)x : (u32)(s32)(s8)x; }
    }
}

static u32 unpack_bytes(u32 cmd, u32 num) {
    if (num == 0) num = 256;
    u32 vn = (cmd >> 2) & 3, vl = cmd & 3;
    u32 bits = (vn == 3 && vl == 3) ? 16 : (32 >> vl) * (vn + 1);
    u32 n = num;
    if (v1.wl > v1.cl) n = v1.cl * (num / v1.wl) + std::min(num % v1.wl, v1.cl);
    return (n * bits + 7) / 8;
}

// Fast path for the common case (no write mask, MODE 0): one specialised loop per format.
template <int VN, int VL, bool USN>
static void unpack_fast(const u8* src, u32 addr, u32 num, u32 cl, u32 wl) {
    constexpr int bits = 32 >> VL, ncomp = VN + 1;
    constexpr bool v45 = VN == 3 && VL == 3;
    constexpr u32 in_bytes = v45 ? 2 : (u32)(bits / 8) * ncomp;
    u32 cycle = 0;
    for (u32 w = 0; w < num; w++) {
        u32 val[4];
        if constexpr (v45) {
            u16 p; std::memcpy(&p, src, 2);
            val[0] = (p & 0x1f) << 3; val[1] = ((p >> 5) & 0x1f) << 3; val[2] = ((p >> 10) & 0x1f) << 3; val[3] = (p >> 15) << 7;
        } else {
            u32 e[4];
            for (int k = 0; k < ncomp; k++) {
                if constexpr (bits == 32) std::memcpy(&e[k], src + k * 4, 4);
                else if constexpr (bits == 16) { u16 x; std::memcpy(&x, src + k * 2, 2); e[k] = USN ? (u32)x : (u32)(s32)(s16)x; }
                else { u8 x = src[k]; e[k] = USN ? (u32)x : (u32)(s32)(s8)x; }
            }
            if constexpr (ncomp == 1) val[0] = val[1] = val[2] = val[3] = e[0];
            else if constexpr (ncomp == 2) { val[0] = e[0]; val[1] = e[1]; val[2] = e[0]; val[3] = e[1]; }
            else if constexpr (ncomp == 3) { val[0] = e[0]; val[1] = e[1]; val[2] = e[2]; val[3] = 0; }
            else { val[0] = e[0]; val[1] = e[1]; val[2] = e[2]; val[3] = e[3]; }
        }
        src += in_bytes;
        std::memcpy(g_vu1_mem + ((addr * 16) & 0x3ff0), val, 16);
        cycle++; addr++;
        if (cycle == wl) { addr += cl - wl; cycle = 0; }
    }
}
// General case (write mask, MODE 1/2 row add, or wl > cl filling): same loop as do_unpack_slow, with the
// element decode specialised per format and the per-row mask decoded once.
template <int VN, int VL, bool USN>
static void unpack_general(const u8* src, u32 addr, u32 num, u32 cl, u32 wl, bool masked) {
    constexpr int bits = 32 >> VL, ncomp = VN + 1;
    constexpr bool v45 = VN == 3 && VL == 3;
    constexpr u32 in_bytes = v45 ? 2 : (u32)(bits / 8) * ncomp;
    u8 mrow[4][4];
    for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) mrow[r][k] = masked ? (v1.mask >> (r * 8 + k * 2)) & 3 : 0;
    const u32 mode = v1.mode;
    u32 cycle = 0;
    for (u32 w = 0; w < num; w++) {
        bool from_data = (wl <= cl) || (cycle < cl);
        u32 val[4] = {0, 0, 0, 0};
        if (from_data) {
            if constexpr (v45) {
                u16 p; std::memcpy(&p, src, 2);
                val[0] = (p & 0x1f) << 3; val[1] = ((p >> 5) & 0x1f) << 3; val[2] = ((p >> 10) & 0x1f) << 3; val[3] = (p >> 15) << 7;
            } else {
                u32 e[4];
                for (int k = 0; k < ncomp; k++) {
                    if constexpr (bits == 32) std::memcpy(&e[k], src + k * 4, 4);
                    else if constexpr (bits == 16) { u16 x; std::memcpy(&x, src + k * 2, 2); e[k] = USN ? (u32)x : (u32)(s32)(s16)x; }
                    else { u8 x = src[k]; e[k] = USN ? (u32)x : (u32)(s32)(s8)x; }
                }
                if constexpr (ncomp == 1) val[0] = val[1] = val[2] = val[3] = e[0];
                else if constexpr (ncomp == 2) { val[0] = e[0]; val[1] = e[1]; val[2] = e[0]; val[3] = e[1]; }
                else if constexpr (ncomp == 3) { val[0] = e[0]; val[1] = e[1]; val[2] = e[2]; }
                else { val[0] = e[0]; val[1] = e[1]; val[2] = e[2]; val[3] = e[3]; }
            }
            src += in_bytes;
        }
        u32* dst = (u32*)(g_vu1_mem + ((addr * 16) & 0x3ff0));
        u32 row_idx = cycle < 3 ? cycle : 3;
        for (int k = 0; k < 4; k++) {
            u32 m = mrow[row_idx][k];
            if (!from_data && !masked) m = 1;
            u32 out;
            if (m == 0) {
                out = val[k];
                if (mode == 1) out += v1.row[k];
                else if (mode == 2) { out += v1.row[k]; v1.row[k] = out; }
            } else if (m == 1) out = v1.row[k];
            else if (m == 2) out = v1.col[row_idx];
            else continue;
            dst[k] = out;
        }
        cycle++; addr++;
        if (wl <= cl) { if (cycle == wl) { addr += cl - wl; cycle = 0; } }
        else if (cycle == wl) cycle = 0;
    }
}
typedef void (*UnpackGenFn)(const u8*, u32, u32, u32, u32, bool);
#define UNG(vn, vl) {unpack_general<vn, vl, false>, unpack_general<vn, vl, true>}
static const UnpackGenFn kUnpackGen[16][2] = { UNG(0,0), UNG(0,1), UNG(0,2), UNG(0,3), UNG(1,0), UNG(1,1), UNG(1,2), UNG(1,3),
                                               UNG(2,0), UNG(2,1), UNG(2,2), UNG(2,3), UNG(3,0), UNG(3,1), UNG(3,2), UNG(3,3) };
#undef UNG
typedef void (*UnpackFn)(const u8*, u32, u32, u32, u32);
#define UNP(vn, vl) {unpack_fast<vn, vl, false>, unpack_fast<vn, vl, true>}
static const UnpackFn kUnpack[16][2] = { UNP(0,0), UNP(0,1), UNP(0,2), UNP(0,3), UNP(1,0), UNP(1,1), UNP(1,2), UNP(1,3),
                                         UNP(2,0), UNP(2,1), UNP(2,2), UNP(2,3), UNP(3,0), UNP(3,1), UNP(3,2), UNP(3,3) };
#undef UNP

static void do_unpack_slow(u32 code, const u8* data) {
    u32 cmd = (code >> 24) & 0x7f, num = (code >> 16) & 0xff, imm = code & 0xffff;
    if (num == 0) num = 256;
    u32 vn = (cmd >> 2) & 3, vl = cmd & 3;
    bool usn = imm & 0x4000, masked = cmd & 0x10;
    u32 addr = (imm & 0x3ff) + ((imm & 0x8000) ? v1.tops : 0);
    int bits = 32 >> vl;
    int ncomp = vn + 1;
    bool v45 = (vn == 3 && vl == 3);
    u32 in_bytes = v45 ? 2 : (u32)(bits / 8) * ncomp;
    u32 cl = v1.cl ? v1.cl : 1, wl = v1.wl ? v1.wl : 1;
    u32 cycle = 0;
    const u8* src = data;
    for (u32 w = 0; w < num; w++) {
        bool from_data = (wl <= cl) || (cycle < cl);
        u32 val[4] = {0, 0, 0, 0};
        if (from_data) {
            if (v45) {
                u16 p; std::memcpy(&p, src, 2);
                val[0] = (p & 0x1f) << 3; val[1] = ((p >> 5) & 0x1f) << 3; val[2] = ((p >> 10) & 0x1f) << 3; val[3] = (p >> 15) << 7;
            } else if (ncomp == 1) {
                u32 x = elem(src, bits, 0, usn); val[0] = val[1] = val[2] = val[3] = x;
            } else {
                for (int k = 0; k < ncomp; k++) val[k] = elem(src, bits, k, usn);
                if (ncomp == 2) { val[2] = val[0]; val[3] = val[1]; }
            }
            src += in_bytes;
        }
        u32* dst = (u32*)(g_vu1_mem + ((addr * 16) & 0x3ff0));
        u32 row_idx = std::min(cycle, 3u);
        for (int k = 0; k < 4; k++) {
            u32 m = masked ? (v1.mask >> (row_idx * 8 + k * 2)) & 3 : 0;
            if (!from_data && !masked) m = 1;
            u32 out;
            switch (m) {
                case 0:
                    out = val[k];
                    if (v1.mode == 1) out += v1.row[k];
                    else if (v1.mode == 2) { out += v1.row[k]; v1.row[k] = out; }
                    break;
                case 1: out = v1.row[k]; break;
                case 2: out = v1.col[row_idx]; break;
                default: continue;                                  // write protected
            }
            dst[k] = out;
        }
        cycle++; addr++;
        if (wl <= cl) { if (cycle == wl) { addr += cl - wl; cycle = 0; } }
        else if (cycle == wl) cycle = 0;
    }
}
static void do_unpack(u32 code, const u8* data) {
    u32 cmd = (code >> 24) & 0x7f, num = (code >> 16) & 0xff, imm = code & 0xffff;
    u32 n = num ? num : 256; u32 cl = v1.cl ? v1.cl : 1, wl = v1.wl ? v1.wl : 1;
    u32 fmt = cmd & 0xf;
    if (fmt == 0x3 || fmt == 0x7 || fmt == 0xb) { do_unpack_slow(code, data); return; }   // invalid formats
    u32 addr = (imm & 0x3ff) + ((imm & 0x8000) ? v1.tops : 0);
    const int usn = (imm & 0x4000) ? 1 : 0;
    // fast path: no write mask, MODE 0, writing at least as many vectors as skipping; otherwise the general one
    const bool simple = !(cmd & 0x10) && v1.mode == 0 && wl <= cl;
    static const bool check = std::getenv("MVP_UNPACKCHECK") != nullptr;      // compare with the reference path
    if (!check) {
        if (simple) kUnpack[fmt][usn](data, addr, n, cl, wl); else kUnpackGen[fmt][usn](data, addr, n, cl, wl, (cmd & 0x10) != 0);
        return;
    }
    static u8 before[0x4000], fast[0x4000]; static u64 checked = 0, bad = 0;
    u32 row_before[4]; std::memcpy(row_before, v1.row, sizeof row_before);
    std::memcpy(before, g_vu1_mem, 0x4000);
    if (simple) kUnpack[fmt][usn](data, addr, n, cl, wl); else kUnpackGen[fmt][usn](data, addr, n, cl, wl, (cmd & 0x10) != 0);
    u32 row_fast[4]; std::memcpy(row_fast, v1.row, sizeof row_fast); std::memcpy(v1.row, row_before, sizeof row_before);
    std::memcpy(fast, g_vu1_mem, 0x4000); std::memcpy(g_vu1_mem, before, 0x4000);
    do_unpack_slow(code, data);
    checked++;
    if ((std::memcmp(fast, g_vu1_mem, 0x4000) || std::memcmp(row_fast, v1.row, sizeof row_fast)) && bad++ < 10)
        fprintf(stderr, "unpack mismatch cmd=%02x num=%u imm=%04x cl=%u wl=%u mode=%u %s\n", cmd, n, imm, cl, wl, v1.mode, simple ? "fast" : "general");
    if ((checked & 0xfffff) == 0) fprintf(stderr, "unpack check: %llu checked, %llu bad\n", (unsigned long long)checked, (unsigned long long)bad);
}

// ---------------------------------------------------------------- VIFcode execution
void vu1_execute(u32 start_pc);                     // vu1.cpp
void vu1_code_uploaded(u32 addr, const u8* data, u32 bytes);

u64 g_prof_vu1 = 0, g_prof_gif = 0;
static void mscal(u32 addr) {
    v1.top = v1.tops; v1.itop = v1.itops;
    v1.dbf ^= 1;
    v1.tops = v1.base + (v1.dbf ? v1.ofst : 0);
    s_vu1_calls++;
    u64 t0 = prof_time_ns();
    vu1_execute(addr);
    g_prof_vu1 += prof_time_ns() - t0;
}

// Consume as many complete VIFcodes as possible from n words at w; stops on an I-bit stall. Returns the words
// consumed. Payloads are used where they are: a DMA block is parsed straight out of the game's memory (it used to
// be copied into the FIFO first, and its last packet -- usually a whole texture -- once more into a zeroed
// buffer: 45 MB a second in a match, about 4% of the main thread in memcpy and memset).
static u8 s_vif_tail[4096 + 32];
static size_t vif1_parse(const u32* w, size_t n) {
    size_t i = 0;
    while (!v1.stalled && i < n) {
        u32 code = w[i];
        u32 cmd = (code >> 24) & 0x7f, num = (code >> 16) & 0xff, imm = code & 0xffff;
        u32 need = 0;                                    // payload words
        if (cmd == 0x20) need = 1;
        else if (cmd == 0x30 || cmd == 0x31) need = 4;
        else if (cmd == 0x4a) need = (num ? num : 256) * 2;
        else if (cmd == 0x50 || cmd == 0x51) need = (imm ? imm : 65536) * 4;
        else if (cmd >= 0x60) need = (unpack_bytes(cmd, num) + 3) / 4;
        if (n - i < 1 + need) break;                      // wait for more data
        // payload: in place (UNPACK may read a few bytes past its end, so it is copied when it is the last thing there)
        const u8* data = (const u8*)(w + i + 1);
        if (cmd >= 0x60 && n - i < 1 + need + 4 && need * 4 <= 4096) {
            if (need) std::memcpy(s_vif_tail, data, need * 4);
            std::memset(s_vif_tail + need * 4, 0, 16); data = s_vif_tail;
        }
        i += 1 + need;
        v1.code = code;
        switch (cmd) {
        case 0x00: break;                                                   // NOP
        case 0x01: v1.cl = imm & 0xff; v1.wl = (imm >> 8) & 0xff; break;    // STCYCL
        case 0x02: v1.ofst = imm & 0x3ff; v1.dbf = 0; v1.tops = v1.base; break;
        case 0x03: v1.base = imm & 0x3ff; break;
        case 0x04: v1.itops = imm & 0x3ff; break;
        case 0x05: v1.mode = imm & 3; break;
        case 0x06: break;                                                   // MSKPATH3
        case 0x07: v1.mark = imm; v1.mrk = true; break;
        case 0x10: case 0x11: case 0x13: break;                             // FLUSHE/FLUSH/FLUSHA (VU1 runs synchronously)
        case 0x14: mscal(imm * 8); break;                                   // MSCAL
        case 0x15: mscal(imm * 8); break;                                   // MSCALF
        case 0x17: mscal(0xffffffffu); break;                               // MSCNT: continue at last PC
        case 0x20: std::memcpy(&v1.mask, data, 4); break;                   // STMASK
        case 0x30: std::memcpy(v1.row, data, 16); break;                    // STROW
        case 0x31: std::memcpy(v1.col, data, 16); break;                    // STCOL
        case 0x4a: {                                                        // MPG
            u32 a = imm * 8;
            for (u32 k = 0; k < need; k++) std::memcpy(g_vu1_code + ((a + k * 4) & 0x3fff), data + k * 4, 4);
            vu1_code_uploaded(a, data, need * 4);
            break;
        }
        case 0x50: case 0x51: gs_gif_write(data, need / 4, 2); break;       // DIRECT/DIRECTHL -> PATH2
        default:
            if (cmd >= 0x60) do_unpack(code, data);
            else rlog("vif1", "unknown VIFcode %08x", code);
        }
        if ((code & 0x80000000u) && cmd != 0x07) {                          // I bit: interrupt + stall
            v1.stalled = true;
            v1.pending_int = true;
        }
    }
    return i;
}
// raised when the FIFO is in order again: the handler may resume VIF1 (FBRST), which re-enters vif1_run
static inline void vif1_raise() { if (v1.pending_int) { v1.pending_int = false; gt_raise_intc(5); } }
static void vif1_run() {
    if (!v1.fifo.empty()) { v1.fifo.rd += vif1_parse(v1.fifo.at(0), v1.fifo.size()); v1.fifo.compact(); }
    vif1_raise();
}

// ---------------------------------------------------------------- DMA channel state (source chain)
struct Chan {
    bool active = false;
    u32 base = 0, chcr = 0, madr = 0, qwc = 0, tadr = 0, asr[2] = {0, 0};
    int sp = 0;
    bool last = false;        // current data block is the last one
};
static Chan s_vif1_ch, s_gif_ch;

// Fetch next tag; returns false on a bad address.
static bool chain_next_tag(Chan& ch, void (*tte)(const u8*)) {
    const u8* tp = dma_mem(ch.tadr);
    if (!tp) { rlog("dma", "bad tag address %08x", ch.tadr); ch.last = true; ch.qwc = 0; return false; }
    u64 tag; std::memcpy(&tag, tp, 8);
    u32 tq = (u32)(tag & 0xffff), id = (u32)(tag >> 28) & 7;
    u32 addr = (u32)(tag >> 32) & 0x7ffffff0u;
    if (tag >> 63) addr |= 0x80000000u;
    bool irq = (tag >> 31) & 1;
    g_dma_last_tag_hi = (u32)(tag >> 16) & 0xffff;
    ch.chcr = (ch.chcr & 0xffff) | (g_dma_last_tag_hi << 16);
    if ((ch.chcr & 0x40) && tte) tte(tp + 8);
    u32 cur = ch.tadr;
    ch.qwc = tq;
    switch (id) {
    case 0: ch.madr = addr; ch.tadr = cur + 16; ch.last = true; break;             // refe
    case 1: ch.madr = cur + 16; ch.tadr = cur + 16 + tq * 16; break;               // cnt
    case 2: ch.madr = cur + 16; ch.tadr = addr; break;                             // next
    case 3: case 4: ch.madr = addr; ch.tadr = cur + 16; break;                     // ref / refs
    case 5: ch.madr = cur + 16; if (ch.sp < 2) ch.asr[ch.sp++] = cur + 16 + tq * 16; ch.tadr = addr; break;  // call
    case 6: ch.madr = cur + 16; if (ch.sp > 0) ch.tadr = ch.asr[--ch.sp]; else ch.last = true; break;       // ret
    case 7: ch.madr = cur + 16; ch.last = true; break;                             // end
    }
    if (irq && (ch.chcr & 0x80)) ch.last = true;
    return true;
}

static void chan_finish(Chan& ch, int idx) {
    ch.active = false;
    gt_set_reg(ch.base, ch.chcr & ~0x100u);
    gt_set_reg(ch.base + 0x10, ch.madr); gt_set_reg(ch.base + 0x20, 0); gt_set_reg(ch.base + 0x30, ch.tadr);
    gt_dma_done(idx);
}

static const bool s_vif_copy = std::getenv("MVP_VIF_COPY") != nullptr;      // debugging: always through the FIFO
void vif1_run_pub() { vif1_run(); }
static void vif1_tte(const u8* p) { u32 w[2]; std::memcpy(w, p, 8); v1.fifo.push_back(w[0]); v1.fifo.push_back(w[1]); }

static void vif1_dma_pump() {
    Chan& ch = s_vif1_ch;
    vif1_run();
    for (int guard = 0; ch.active && guard < 1000000; guard++) {
        if (v1.stalled) return;                      // resumes on FBRST.STC
        if (ch.qwc) {
            const u8* p = dma_mem(ch.madr);
            const size_t n = (size_t)ch.qwc * 4;
            s_vif1_qwords += ch.qwc;
            ch.madr += ch.qwc * 16; ch.qwc = 0;
            if (p && v1.fifo.empty() && !s_vif_copy) {      // nothing waiting: parse the block where it is
                const size_t used = vif1_parse((const u32*)p, n);
                if (used < n) v1.fifo.append((const u32*)p + used, n - used);     // an incomplete packet, or stalled
                vif1_raise();
                continue;
            }
            if (p) v1.fifo.append((const u32*)p, n);
            vif1_run();
            continue;
        }
        u32 mode = (ch.chcr >> 2) & 3;
        if (mode == 0 || ch.last) {
            if (v1.stalled) return;
            chan_finish(ch, 1);
            return;
        }
        chain_next_tag(ch, vif1_tte);
    }
}

u64 g_game_frames = 0;
static const bool s_fps_log = std::getenv("MVP_FPS") != nullptr;
static const u64 s_t0 = host_time_ns();          // EAGL kicks one VIF1 chain per rendered frame
static void vif1_dma_exec(u32 base, u32 chcr, u32 madr, u32 qwc, u32 tadr, u64 frame) {
    Chan& ch = s_vif1_ch;
    extern void gs_frame_begin(u64 frame); gs_frame_begin(frame);
    ch.active = true; ch.base = base; ch.chcr = chcr;
    ch.madr = madr; ch.qwc = qwc; ch.tadr = tadr;
    ch.sp = 0; ch.last = false;
    vif1_dma_pump();
}
void gt_push_kick(int ch, u32 base, u32 chcr, u32 madr, u32 qwc, u32 tadr, u64 frame);
static void vif1_dma(u32 base, u32 chcr) {
    g_game_frames++;
    extern void sim_timing_frame(u64); sim_timing_frame(host_time_ns());   // real-time step for this frame
    if (s_fps_log && g_game_frames % 300 == 0) {        // MVP_FPS=1: print speed every 300 frames
        static u64 last_t = 0, last_vb = 0, last_tk = 0; extern u64 kernel_vblank_count();
        u64 t = host_time_ns(), vb = kernel_vblank_count(), tk = g_sim_ticks;
        float step; std::memcpy(&step, &g_sim_dt_bits[0], 4);
        if (last_t) fprintf(stderr, "[fps] t=%.1fs frame %llu: %.1f frames/s, %.1f vblanks/s, %.1f sim ticks/s x %.2f ms = %.0f%% game speed\n", (t - s_t0) / 1e9,
                            (unsigned long long)g_game_frames, 300e9 / (t - last_t), (vb - last_vb) * 1e9 / (t - last_t),
                            (tk - last_tk) * 1e9 / (t - last_t), step * 1000, (tk - last_tk) * step * 1e11 / (t - last_t));
        last_t = t; last_vb = vb; last_tk = tk;
    }
    gt_push_kick(1, base, chcr, (u32)hw_reg(base + 0x10), (u32)hw_reg(base + 0x20), (u32)hw_reg(base + 0x30), g_game_frames);
}

static void gif_dma_exec(u32 base, u32 chcr, u32 madr, u32 qwc, u32 tadr) {
    Chan& ch = s_gif_ch;
    ch.active = true; ch.base = base; ch.chcr = chcr;
    ch.madr = madr; ch.qwc = qwc; ch.tadr = tadr;
    ch.sp = 0; ch.last = false;
    s_gif_kicks++;
    for (int guard = 0; guard < 1000000; guard++) {
        if (ch.qwc) {
            if (const u8* p = dma_mem(ch.madr)) gs_gif_write(p, ch.qwc, 3);
            s_gif_qwords += ch.qwc;
            ch.madr += ch.qwc * 16; ch.qwc = 0;
            continue;
        }
        if (((ch.chcr >> 2) & 3) == 0 || ch.last) break;
        chain_next_tag(ch, nullptr);
    }
    chan_finish(ch, 2);
}
static void gif_dma(u32 base, u32 chcr) {
    gt_push_kick(2, base, chcr, (u32)hw_reg(base + 0x10), (u32)hw_reg(base + 0x20), (u32)hw_reg(base + 0x30), g_game_frames);
}

// DMA handlers return true if they completed the transfer themselves (asynchronously).
void gs_dma_init() {
    hw_set_dma_handler(1, vif1_dma);
    hw_set_dma_handler(2, gif_dma);
}
bool gs_dma_is_async(int ch) { return ch == 1 || ch == 2; }
void gs_dma_stats() {
    if (g_host_timing) fprintf(stderr, "time: VU1 %.1fs (incl. XGKICK rendering), GS(all paths) %.1fs\n", g_prof_vu1 / 1e9, g_prof_gif / 1e9);
    fprintf(stderr, "graphics: %llu GIF qwords (%llu kicks), %llu VIF1 qwords, %llu VU1 programs run\n",
            (unsigned long long)s_gif_qwords, (unsigned long long)s_gif_kicks, (unsigned long long)s_vif1_qwords,
            (unsigned long long)s_vu1_calls);
    extern u64 vu1_mpg_count(); extern u64 vu1_instr_count();
    fprintf(stderr, "VU1: %llu MPG uploads, %llu instruction pairs\n", (unsigned long long)vu1_mpg_count(), (unsigned long long)vu1_instr_count());
    gs_stats();
}

// ---------------------------------------------------------------- graphics thread
// VIF1 (unpack, VU1 microprograms), GIF parsing and primitive setup run on their own thread, fed in order
// by the EE through a command ring: DMA kicks, VIF1/GIF FIFO writes, VIF1 FBRST, GS privileged register
// writes and vsyncs. Its side effects on EE-visible state (DMA channel registers, DMA/INTC interrupts, GS
// SIGNAL/FINISH) go back as events applied by the main thread in deliver_events, so the EE keeps running
// its game logic while the previous frame is being transformed and drawn. Experimental: MVP_GS_THREAD=1 enables it.
// Status reads of VIF1/GIF/GS registers and every vblank synchronise with it (EAGL's render watchdog
// times out otherwise); on a 4-core test machine that costs more than the overlap gains, so it is off.
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#ifndef _WIN32
#include <pthread.h>
#include <csignal>
#endif
enum { GT_KICK_VIF1, GT_KICK_GIF, GT_VIF1_REG, GT_VIF1_FIFO, GT_GIF_FIFO, GT_GS_PRIV, GT_VSYNC };
struct GtCmd { u8 op; u32 a, b, c, d, e; u64 v; u32 w[4]; };
static const u32 GT_CAP = 1 << 14;
static GtCmd s_gt[GT_CAP];
static std::atomic<u32> s_gt_head{0}, s_gt_tail{0};    // head: produced (main), tail: consumed (graphics thread)
static std::atomic<bool> s_gt_busy{false};
static std::atomic<int> s_gt_pending_frames{0};
static bool s_gt_on = false, s_gt_started = false;
static const bool s_gt_lockstep = std::getenv("MVP_GT_SYNC") != nullptr;
void gt_sync();
static thread_local bool t_on_gt = false;
static std::mutex s_gt_mx; static std::condition_variable s_gt_cv;
struct GtEvent { u8 kind; u32 a; u64 v; };           // 0 intc, 1 set reg, 2 dma done, 3 gs event
static std::mutex s_ev_mx; static std::vector<GtEvent> s_ev; static std::atomic<bool> s_ev_any{false};
static std::condition_variable s_ev_cv;        // signalled when the graphics thread posts an event

void gs_priv_write(u32 addr, u64 v); void gs_vsync_exec();
static const bool s_gt_debug = std::getenv("MVP_GT_DEBUG") != nullptr;
static void gt_exec(const GtCmd& c) {
    if (s_gt_debug && c.op != GT_GS_PRIV && c.op != GT_VSYNC) rlog("gt-cmd", "op=%u a=%08x b=%08x c=%08x d=%x e=%08x", c.op, c.a, c.b, c.c, c.d, c.e);
    switch (c.op) {
    case GT_KICK_VIF1: vif1_dma_exec(c.a, c.b, c.c, c.d, c.e, c.v); s_gt_pending_frames--; break;
    case GT_KICK_GIF: gif_dma_exec(c.a, c.b, c.c, c.d, c.e); break;
    case GT_VIF1_REG: vif1_write_reg_exec(c.a, c.b); break;
    case GT_VIF1_FIFO: vif1_fifo_write_exec(c.w); break;
    case GT_GIF_FIFO: gs_gif_write((const u8*)c.w, 1, 3); break;
    case GT_GS_PRIV: gs_priv_write(c.a, c.v); break;
    case GT_VSYNC: gs_vsync_exec(); break;
    }
}
static void gt_main() {
    t_on_gt = true;
    int idle = 0;
    for (;;) {
        u32 t = s_gt_tail.load(std::memory_order_relaxed);
        if (t != s_gt_head.load(std::memory_order_acquire)) {
            s_gt_busy = true;
            gt_exec(s_gt[t % GT_CAP]);
            s_gt_tail.store(t + 1, std::memory_order_release);
            idle = 0;
            continue;
        }
        s_gt_busy = false;
        if (++idle < 2000) { std::this_thread::yield(); continue; }
        std::unique_lock<std::mutex> lk(s_gt_mx);
        s_gt_cv.wait_for(lk, std::chrono::microseconds(300));
    }
}
static void gt_start() {
    if (s_gt_started) return;
    s_gt_started = true;
    const char* e = std::getenv("MVP_GS_THREAD");      // experimental, off by default (no gain measured yet)
    s_gt_on = e && e[0] == '1';
    if (!s_gt_on) return;
#ifndef _WIN32
    sigset_t all, old; sigfillset(&all); pthread_sigmask(SIG_BLOCK, &all, &old);
#endif
    std::thread(gt_main).detach();
#ifndef _WIN32
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
#endif
    rlog("gs", "graphics thread started");
}
static void gt_push(const GtCmd& c) {
    gt_start();
    if (!s_gt_on) { gt_exec(c); return; }
    u32 h = s_gt_head.load(std::memory_order_relaxed);
    while (h - s_gt_tail.load(std::memory_order_acquire) >= GT_CAP) std::this_thread::yield();
    s_gt[h % GT_CAP] = c;
    s_gt_head.store(h + 1, std::memory_order_release);
    if (((h + 1) & 15) == 0 || c.op == GT_KICK_VIF1 || c.op == GT_VSYNC) s_gt_cv.notify_one();
    if (s_gt_lockstep) gt_sync();                   // MVP_GT_SYNC=1: debug, wait for every command
}
void gt_poll_events();
void gt_sync() {                                    // wait until the graphics thread has done everything queued
    if (!s_gt_on || t_on_gt) return;
    s_gt_cv.notify_one();
    while (s_gt_tail.load(std::memory_order_acquire) != s_gt_head.load(std::memory_order_acquire) || s_gt_busy.load())
        std::this_thread::yield();
    gt_poll_events();
}
bool gt_active() { return s_gt_on; }
bool gt_on_graphics_thread() { return t_on_gt; }
void gt_push_kick(int ch, u32 base, u32 chcr, u32 madr, u32 qwc, u32 tadr, u64 frame) {
    GtCmd c{}; c.op = ch == 1 ? GT_KICK_VIF1 : GT_KICK_GIF; c.a = base; c.b = chcr; c.c = madr; c.d = qwc; c.e = tadr; c.v = frame;
    if (ch == 1) {
        // keep the EE at most two frames ahead of the graphics thread
        s_gt_pending_frames++;
        while (s_gt_on && s_gt_pending_frames.load() > 2) { s_gt_cv.notify_one(); std::this_thread::yield(); gt_poll_events(); }
    }
    gt_push(c);
}
// Vblanks must not run ahead of the graphics work: the game counts vblanks while it waits for a frame
// (EAGL has a render watchdog), and on hardware a frame's GS work finishes within its vblank budget.
// Called by the kernel just before it delivers a vblank.
void gt_wait_frames() {
    if (!s_gt_on) return;
    gt_sync();
}
void gt_push_vif1_reg(u32 a, u32 v) { GtCmd c{}; c.op = GT_VIF1_REG; c.a = a; c.b = v; gt_push(c); }
void gt_push_data(int kind, const u32* w) { GtCmd c{}; c.op = kind == 0 ? GT_VIF1_FIFO : GT_GIF_FIFO; std::memcpy(c.w, w, 16); gt_push(c); }
void gt_push_priv(u32 a, u64 v) { GtCmd c{}; c.op = GT_GS_PRIV; c.a = a; c.v = v; gt_push(c); }
void gt_push_vsync() { GtCmd c{}; c.op = GT_VSYNC; gt_push(c); }

static void post(u8 kind, u32 a, u64 v) {
    if (s_gt_debug) rlog("gt-ev", "post kind=%u a=%08x v=%llx", kind, a, (unsigned long long)v);
    { std::lock_guard<std::mutex> lk(s_ev_mx);
      s_ev.push_back({kind, a, v});
      s_ev_any.store(true, std::memory_order_release); }
    s_ev_cv.notify_one();
}
// main thread, waiting in a guest spin loop: sleep until the graphics thread posts an event (or ns pass)
void gt_wait_event(u64 ns) {
    if (!s_gt_on) { std::this_thread::sleep_for(std::chrono::nanoseconds(ns)); return; }
    std::unique_lock<std::mutex> lk(s_ev_mx);
    s_ev_cv.wait_for(lk, std::chrono::nanoseconds(ns), [] { return !s_ev.empty(); });
}
void hw_gs_event(u32 bits);
void gt_raise_intc(int cause) { if (t_on_gt) post(0, (u32)cause, 0); else kernel_raise_intc(cause); }
void gt_set_reg(u32 addr, u64 v) { if (t_on_gt) post(1, addr, v); else hw_set_reg(addr, v); }
void gt_dma_done(int ch) { extern void hw_dma_done(int ch); if (t_on_gt) post(2, (u32)ch, 0); else hw_dma_done(ch); }
void gt_gs_event(u32 bits) { if (t_on_gt) post(3, bits, 0); else hw_gs_event(bits); }
static std::deque<GtEvent> s_ev_main;              // main thread: events not yet applied (order kept)
void gt_poll_events() {                             // main thread: apply what the graphics thread reported
    if (s_ev_any.load(std::memory_order_acquire)) {
        std::vector<GtEvent> ev;
        { std::lock_guard<std::mutex> lk(s_ev_mx); ev.swap(s_ev); s_ev_any.store(false); }
        for (auto& e : ev) s_ev_main.push_back(e);
    }
    if (s_ev_main.empty()) return;
    extern void hw_dma_done(int ch); extern bool kernel_in_interrupt();
    while (!s_ev_main.empty()) {
        GtEvent e = s_ev_main.front();
        // An interrupt raised while a handler runs would only be latched and could merge with the next one of
        // the same kind (the game counts them), so interrupt events wait until no handler is active.
        if (e.kind != 1 && kernel_in_interrupt()) return;
        s_ev_main.pop_front();
        switch (e.kind) {
        case 0: kernel_raise_intc((int)e.a); break;
        case 1: hw_set_reg(e.a, e.v); break;
        case 2: hw_dma_done((int)e.a); break;
        case 3: hw_gs_event(e.a); break;
        }
    }
}
