// EE hardware registers (DMAC, INTC, timers, GS privileged regs, SIF), COP0, rare instructions, VU0 macro mode.
#include "ee_ops.h"
#include <vector>
#include <cstdlib>
#include "ee_runtime.h"
#include "gs.h"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <unordered_map>

void kernel_raise_intc(int cause);
void kernel_raise_dmac(int ch);
void ee_poll(EEContext* c);

// ---------------------------------------------------------------- register file
static std::unordered_map<u32, u64> s_regs;
static u32 s_intc_stat = 0, s_intc_mask = 0;
static u32 s_dstat = 0;
static u64 s_gs_csr = 0;
static u32 s_sif_smflg = 0x00070000;               // IOP pretends SIF/CMD/boot are ready
static u32 s_sif_msflg = 0;

void gs_dma_init(); void ipu_init(); bool ipu_dma_is_async(int ch);
u64 ipu_read(u32 a); void ipu_write(u32 a, u64 v); void ipu_fifo_write(const u32* qw); void ipu_fifo_read(u32* qw);
void hw_init() { s_regs.clear(); gs_dma_init(); ipu_init(); }

// ---------------------------------------------------------------- timers (T0-T3)
// Counters run from host time. Compare / overflow interrupts are raised from hw_poll_timers().
struct Timer { u32 mode = 0, comp = 0, hold = 0; u32 base_count = 0; u64 base_ns = 0; u64 last_ticks = 0; };
static Timer s_tim[4];
static double timer_rate(u32 mode) {
    static const double r[4] = {147456000.0, 147456000.0 / 16, 147456000.0 / 256, 15734.0};
    return r[mode & 3];
}
static u64 timer_ticks(const Timer& t, u64 now) {           // total ticks since base (unwrapped)
    if (!(t.mode & 0x80)) return 0;
    return (u64)((now - t.base_ns) * timer_rate(t.mode) / 1e9);
}
static u32 timer_read(u32 a) {
    Timer& t = s_tim[(a >> 11) & 3];
    switch (a & 0x30) {
        case 0x00: return (u32)((t.base_count + timer_ticks(t, host_time_ns())) & 0xffff);
        case 0x10: return t.mode;
        case 0x20: return t.comp;
        default: return t.hold;
    }
}
static void timer_write(u32 a, u32 v) {
    Timer& t = s_tim[(a >> 11) & 3];
    u64 now = host_time_ns();
    switch (a & 0x30) {
        case 0x00: t.base_count = v & 0xffff; t.base_ns = now; t.last_ticks = 0; break;
        case 0x10: {
            u32 cur = timer_read(a & ~0x30u);
            t.mode = (t.mode & 0xc00 & ~(v & 0xc00)) | (v & 0x3ff);   // bits 10/11: write 1 to clear
            t.base_count = cur; t.base_ns = now; t.last_ticks = 0;
            break;
        }
        case 0x20: t.comp = v & 0xffff; break;
        default: t.hold = v & 0xffff; break;
    }
}
bool hw_poll_timers() {
    bool any = false;
    u64 now = host_time_ns();
    for (int i = 0; i < 4; i++) {
        Timer& t = s_tim[i];
        if (!(t.mode & 0x80) || !(t.mode & 0x300)) continue;
        u64 ticks = timer_ticks(t, now);
        u64 start = t.base_count + t.last_ticks, end = t.base_count + ticks;
        if (end <= start) continue;
        bool fire = false;
        if (t.mode & 0x100) {                                  // compare interrupt
            u64 period = (t.mode & 0x40) ? (t.comp ? t.comp : 0x10000) : 0x10000;
            u64 first = t.comp > (start % period) ? start - (start % period) + t.comp : start - (start % period) + period + t.comp;
            if ((t.mode & 0x40) && t.comp) first = start - (start % t.comp) + t.comp;
            if (first <= end) { fire = true; t.mode |= 0x400; }
        }
        if ((t.mode & 0x200) && (start >> 16) != (end >> 16)) { fire = true; t.mode |= 0x800; }
        t.last_ticks = ticks;
        if (fire) { s_intc_stat |= 1u << (9 + i); kernel_raise_intc(9 + i); any = true; }
    }
    return any;
}
void pad_vsync(u64 vsync); u64 kernel_vblank_count();
// MVP_FINDMEM=<hex bytes>: every 30 vblanks search main RAM for the pattern and log new matches (debugging:
// where did the game put this data?).
static void dbg_findmem() {
    static const char* pat_hex = std::getenv("MVP_FINDMEM");
    if (!pat_hex) return;
    static std::vector<u8> pat; static std::vector<u32> seen;
    if (pat.empty()) for (const char* p = pat_hex; p[0] && p[1]; p += 2) { char b[3] = {p[0], p[1], 0}; pat.push_back((u8)std::strtoul(b, nullptr, 16)); }
    if (kernel_vblank_count() % 30 || pat.empty()) return;
    for (u32 a = 0; a + pat.size() < 0x02000000u; a++) {
        const u8* q = (const u8*)std::memchr(g_ram + a, pat[0], 0x02000000u - pat.size() - a);
        if (!q) break;
        a = (u32)(q - g_ram);
        if (!std::memcmp(q, pat.data(), pat.size()) && std::find(seen.begin(), seen.end(), a) == seen.end()) {
            seen.push_back(a); extern u64 g_game_frames;
            fprintf(stderr, "[findmem] pattern at %08x (vblank %llu, frame %llu)\n", a, (unsigned long long)kernel_vblank_count(), (unsigned long long)g_game_frames);
        }
    }
}
// MVP_CHAIN=7d66c8,4,10,2c[:w]: follow the pointers ([7d66c8] + 4 -> [..] + 0x10 -> ...) every vblank; the last
// number is the offset of the field to watch: reads of it are logged (as MVP_RWATCH), with ":w" writes (MVP_WATCH).
// Prints the field's address and value whenever either changes. (Debugging: who uses this setting?)
static void dbg_chain() {
    static const char* spec = std::getenv("MVP_CHAIN");
    if (!spec) return;
    static std::vector<u32> offs; static bool wr = false; static u32 last_a = 0, last_v = 0xdeadbeef;
    if (offs.empty()) { const char* p = spec; while (*p && *p != ':') { char* e; offs.push_back((u32)std::strtoul(p, &e, 16)); p = *e == ',' ? e + 1 : e; } wr = *p == ':'; }
    if (offs.size() < 2) return;
    u32 a = offs[0];
    for (size_t i = 1; i < offs.size(); i++) {
        a &= 0x1ffffffc; if (a < 0x80000 || a >= 0x02000000u - 4) return;
        u32 v; std::memcpy(&v, g_ram + a, 4); a = v + offs[i];
    }
    a &= 0x1fffffff; if (a < 0x80000 || a >= 0x02000000u - 4) return;
    u32 v; std::memcpy(&v, g_ram + a, 4);
    extern u32 g_rwatch_lo, g_rwatch_hi, g_watch_lo, g_watch_hi;
    if (wr) { g_watch_lo = a; g_watch_hi = a + 4; } else { g_rwatch_lo = a; g_rwatch_hi = a + 4; }
    if (a != last_a || v != last_v) { extern u64 g_game_frames; fprintf(stderr, "[chain] field at %08x = %08x (frame %llu)\n", a, v, (unsigned long long)g_game_frames); last_a = a; last_v = v; }
}
void era_uniforms_poll();
void hw_vblank_start() { if ((kernel_vblank_count() & 63) == 0) era_uniforms_poll(); dbg_findmem(); dbg_chain(); s_intc_stat |= 1u << 2; s_gs_csr |= 8; s_gs_csr ^= 0x2000; gs_vsync(); extern void frontend_vsync(); frontend_vsync(); pad_vsync(kernel_vblank_count()); extern void snd_vsync(u64); snd_vsync(kernel_vblank_count()); }
u32 kernel_gs_imr();
void hw_gs_event(u32 bits) {                       // GS SIGNAL (1) / FINISH (2)
    s_gs_csr |= bits;
    u32 imr = kernel_gs_imr();
    if (((bits & 1) && !(imr & 0x100)) || ((bits & 2) && !(imr & 0x200))) { s_intc_stat |= 1; kernel_raise_intc(0); }
}
void hw_vblank_end() { s_intc_stat |= 1u << 3; }
void hw_raise_intc(int cause) { s_intc_stat |= 1u << cause; kernel_raise_intc(cause); }

static const char* dma_name(u32 base) {
    switch (base) {
        case 0x10008000: return "VIF0"; case 0x10009000: return "VIF1"; case 0x1000a000: return "GIF";
        case 0x1000b000: return "fromIPU"; case 0x1000b400: return "toIPU"; case 0x1000c000: return "SIF0";
        case 0x1000c400: return "SIF1"; case 0x1000c800: return "SIF2"; case 0x1000d000: return "fromSPR";
        case 0x1000d400: return "toSPR"; default: return nullptr;
    }
}
static int dma_index(u32 base) {
    static const u32 b[] = {0x10008000, 0x10009000, 0x1000a000, 0x1000b000, 0x1000b400, 0x1000c000, 0x1000c400, 0x1000c800, 0x1000d000, 0x1000d400};
    for (int i = 0; i < 10; i++) if (b[i] == base) return i;
    return -1;
}

// DMA consumers are plugged in here as they get implemented (GIF -> GS renderer, VIF1 -> VU1, SIF -> IOP HLE).
typedef void (*DmaHandler)(u32 base, u32 chcr);
static DmaHandler s_dma_handlers[10] = {};
void hw_set_dma_handler(int ch, DmaHandler h) { s_dma_handlers[ch] = h; }
u64 hw_reg(u32 addr) { auto it = s_regs.find(addr); return it != s_regs.end() ? it->second : 0; }
void hw_set_reg(u32 addr, u64 v) { s_regs[addr] = v; }

u32 g_dma_last_tag_hi = 0;
static inline u8* dma_ptr(u32 a) { return (a & 0x80000000u) ? g_spr + (a & 0x3ff0) : ee_ptr(a & 0x7ffffff0u); }
// scratchpad DMA: normal, chain (toSPR: source chain from memory, fromSPR: destination chain from SPR) and
// interleave modes
static void spr_dma(u32 base, u32 chcr) {
    const bool to = base == 0x1000d400;
    u32 madr = (u32)s_regs[base + 0x10], qwc = (u32)s_regs[base + 0x20], tadr = (u32)s_regs[base + 0x30];
    u32 sadr = (u32)s_regs[base + 0x80] & 0x3ff0;
    const u32 mode = (chcr >> 2) & 3;
    auto copy = [&](u32 n) {
        for (u32 i = 0; i < n; i++) {
            u8* mem = dma_ptr(madr); u8* spr = g_spr + (sadr & 0x3ff0);
            if (mem) { if (to) std::memcpy(spr, mem, 16); else std::memcpy(mem, spr, 16); }
            madr += 16; sadr = (sadr + 16) & 0x3ff0;
        }
    };
    if (mode == 2) {                                             // interleave: SQWC.TQWC transfer, SQWC.SQWC skip (memory side)
        u32 sqwc = (u32)s_regs[0x1000e030];
        u32 skip = sqwc & 0xff, tq = (sqwc >> 16) & 0xff;
        if (!tq) tq = qwc;
        while (qwc) { u32 n = std::min(tq, qwc); copy(n); qwc -= n; if (qwc) madr += skip * 16; }
    } else if (mode == 1 && to) {                                // source chain (tags in memory)
        copy(qwc); qwc = 0;
        bool end = false; u32 asr[2]; int sp = 0;
        for (int guard = 0; guard < 100000 && !end; guard++) {
            u8* tp = dma_ptr(tadr); u64 tag = 0; if (tp) std::memcpy(&tag, tp, 8);
            u32 n = (u32)(tag & 0xffff), id = (u32)(tag >> 28) & 7, addr = (u32)(tag >> 32) & 0xfffffff0u;
            g_dma_last_tag_hi = (u32)(tag >> 16) & 0xffff;
            if (chcr & 0x40) { std::memcpy(g_spr + (sadr & 0x3ff0), tp + 8, 8); std::memset(g_spr + (sadr & 0x3ff0) + 8, 0, 8); sadr = (sadr + 16) & 0x3ff0; }   // TTE: tag transfer
            switch (id) {
            case 0: madr = addr; tadr += 16; end = true; break;                            // refe
            case 1: madr = tadr + 16; tadr = madr + n * 16; break;                         // cnt
            case 2: madr = tadr + 16; tadr = addr; break;                                  // next
            case 3: case 4: madr = addr; tadr += 16; break;                                // ref / refs
            case 5: madr = tadr + 16; if (sp < 2) asr[sp++] = madr + n * 16; tadr = addr; break;   // call
            case 6: madr = tadr + 16; if (sp > 0) tadr = asr[--sp]; else end = true; break;         // ret
            default: madr = tadr + 16; end = true; break;                                 // end
            }
            copy(n);
            if (id == 1) tadr = madr;
            if (((tag >> 31) & 1) && (chcr & 0x80)) end = true;                            // IRQ + TIE
        }
    } else if (mode == 1) {                                      // fromSPR destination chain (tags in SPR)
        copy(qwc); qwc = 0;
        for (int guard = 0; guard < 100000; guard++) {
            u64 tag; std::memcpy(&tag, g_spr + (sadr & 0x3ff0), 8);
            sadr = (sadr + 16) & 0x3ff0;
            u32 n = (u32)(tag & 0xffff), id = (u32)(tag >> 28) & 7;
            madr = (u32)(tag >> 32) & 0xfffffff0u;
            g_dma_last_tag_hi = (u32)(tag >> 16) & 0xffff;
            copy(n);
            if (id == 7 || (((tag >> 31) & 1) && (chcr & 0x80))) break;                  // end / IRQ
        }
    } else copy(qwc);
    s_regs[base + 0x10] = madr; s_regs[base + 0x20] = 0; s_regs[base + 0x30] = tadr; s_regs[base + 0x80] = sadr;
}

static void dma_start(u32 base, u32 chcr) {
    int idx = dma_index(base);
    u32 madr = (u32)s_regs[base + 0x10], qwc = (u32)s_regs[base + 0x20], tadr = (u32)s_regs[base + 0x30];
    rlog(dma_name(base), "start chcr=%08x madr=%08x qwc=%x tadr=%08x", chcr, madr, qwc, tadr);
    extern bool gs_dma_is_async(int ch);
    if (idx >= 0 && s_dma_handlers[idx] && (gs_dma_is_async(idx) || ipu_dma_is_async(idx))) {   // channel completes itself (hw_dma_done)
        s_regs[base] = chcr;
        s_dma_handlers[idx](base, chcr);
        return;
    }
    if (base == 0x1000d000 || base == 0x1000d400) spr_dma(base, chcr);
    else if (idx >= 0 && s_dma_handlers[idx]) s_dma_handlers[idx](base, chcr);
    // complete immediately; CHCR[31:16] holds the upper half of the last DMA tag read (chain modes)
    extern u32 g_dma_last_tag_hi;
    u32 hi = ((chcr >> 2) & 3) ? g_dma_last_tag_hi : (chcr >> 16);
    s_regs[base] = (chcr & 0xfeffu) | (hi << 16);
    if (idx >= 0) { s_dstat |= 1u << idx; kernel_raise_dmac(idx); }
}

void hw_dma_done(int ch) { s_dstat |= 1u << ch; kernel_raise_dmac(ch); }
u32 vif1_read_reg(u32 a); void vif1_write_reg(u32 a, u32 v);

u64 ee_hw_read(u32 a, int size) {
    a &= 0x1fffffff;
    {   // graphics status (VIF1/GIF DMA, D_STAT, GS CSR, VIF1 regs) reflects the graphics thread: let it finish first
        extern void gt_sync(); extern void gt_poll_events();
        if ((a >= 0x10009000u && a < 0x1000a100u) || a == 0x1000e010u || (a & 0xffff0000u) == 0x12000000u ||
            (a >= 0x10003c00u && a < 0x10004000u)) gt_sync();
        else gt_poll_events();
    }
    if (a < 0x10000000u) {
        rlog("bad-read", "read outside RAM: %08x", a);
        static int shown = 0;                          // who did it (the call stack exists in trace builds only)
        extern EEContext* kernel_active_ctx(); EEContext* c = kernel_active_ctx();
        if (c && shown < 12) { shown++; fprintf(stderr, "     ra=%08x a0=%08x a1=%08x v0=%08x s0=%08x s1=%08x", c->r[31].UW[0], c->r[4].UW[0], c->r[5].UW[0], c->r[2].UW[0], c->r[16].UW[0], c->r[17].UW[0]);
            if (c->shadow_n > 0) { fprintf(stderr, " stack:"); for (int k = c->shadow_n > 10 ? c->shadow_n - 10 : 0; k < c->shadow_n; k++) fprintf(stderr, " %08x", c->shadow[k]); }
            fprintf(stderr, "\n"); }
        return 0;
    }
    if ((a >> 16) == 0x1100) rlog("vu-mem-read", "%08x (%d bytes)", a, size);
    u64 v = 0;
    if ((a & 0xfffff000u) == 0x10003000u && (a & 0xf00) >= 0xc00) { v = vif1_read_reg(a); ee_poll(kernel_current()); return v; }
    if ((a & 0xffffff00u) == 0x10002000u) {                           // IPU registers
        v = ipu_read(a & ~7u);
        if (a & 4) v >>= 32; else if (size < 8) v &= 0xffffffffu;
        ee_poll(kernel_current()); return v;
    }
    switch (a & ~0xfu) {
        case 0x10000000: case 0x10000800: case 0x10001000: case 0x10001800:
        case 0x10000010: case 0x10000810: case 0x10001010: case 0x10001810:
        case 0x10000020: case 0x10000820: case 0x10001020: case 0x10001820:
        case 0x10000030: case 0x10000830: case 0x10001030: case 0x10001830:
            v = timer_read(a); break;
        case 0x1000e010: v = s_dstat; break;
        case 0x1000f000: v = s_intc_stat; break;
        case 0x1000f010: v = s_intc_mask; break;
        case 0x1000f220: v = s_sif_msflg; break;
        case 0x1000f230: v = s_sif_smflg; break;
        case 0x12001000: v = s_gs_csr | (2u << 16) /* rev */; break;
        case 0x10003000: case 0x10003010: case 0x10003020: v = 0; break;       // GIF_CTRL/MODE/STAT: idle
        case 0x10003800: case 0x10003810: case 0x10003820: case 0x10003830: v = 0; break;  // VIF0: idle
        default: {
            auto it = s_regs.find(a & ~3u);
            v = it != s_regs.end() ? it->second : 0;
            if (dma_name(a & ~0xfffu | ((a & 0xc00) & 0xc00)) == nullptr) rlog("hw-read", "%08x (%d bytes) -> %llx", a, size, (unsigned long long)v);
        }
    }
    ee_poll(kernel_current());
    return v;
}

void ee_hw_write(u32 a, u64 v, int size) {
    a &= 0x1fffffff;
    if ((a >> 16) == 0x1100) rlog("vu-mem-write", "%08x <- %llx (%d bytes)", a, (unsigned long long)v, size);
    if (a < 0x10000000u) {
        EEContext* c = kernel_current();
        rfatal("write outside RAM: %08x <- %llx (size %d) ra=%08x sp=%08x", a, (unsigned long long)v, size, c ? c->r[31].UW[0] : 0, c ? c->r[29].UW[0] : 0);
    }
    u32 w = (u32)v;
    switch (a) {
        case 0x1000e010: s_dstat = (s_dstat & ~(w & 0xffff)) ^ (w & 0xffff0000u); return;   // clear status, toggle mask
        case 0x1000f000: s_intc_stat &= ~w; return;
        case 0x1000f010: s_intc_mask ^= w; return;
        case 0x1000f220: s_sif_msflg |= w; return;
        case 0x1000f230: s_sif_smflg &= ~w; return;
        case 0x12001000: s_gs_csr &= ~(v & 0x1f); if (v & 0x200) s_gs_csr = 0; return;
        case 0x12001010: return;                                    // IMR (via GsPutIMR)
        default: break;
    }
    if (a < 0x10002000u && (a & 0x7ff) < 0x40) { timer_write(a, w); return; }
    if ((a & 0xffffff00u) == 0x10002000u) { if (!(a & 4)) ipu_write(a, v); return; }
    if ((a & 0xfffff000u) == 0x10003000u && (a & 0xf00) >= 0xc00) { vif1_write_reg(a, w); return; }
    if ((a & 0xffff0000u) == 0x12000000u) { extern void gt_push_priv(u32, u64); gt_push_priv(a, v); s_regs[a] = v; return; }
    u32 base = a & ~0xffu;
    if (dma_name(base) && (a & 0xff) == 0 && (w & 0x100)) { s_regs[a] = w; dma_start(base, w); return; }
    if (a == 0x10009030) {
        extern EEContext* kernel_active_ctx(); EEContext* c = kernel_active_ctx();
        rlog("dbg", "TADR<-%x a0=%llx s0=%llx s4=%llx a1=%llx ra=%x", w, (unsigned long long)c->r[4].UD[0], (unsigned long long)c->r[16].UD[0],
             (unsigned long long)c->r[20].UD[0], (unsigned long long)c->r[5].UD[0], c->r[31].UW[0]);
        if (c->shadow_n > 0) { fprintf(stderr, "     stack:"); for (int k = 0; k < c->shadow_n && k < 64; k++) fprintf(stderr, " %08x", c->shadow[k]); fprintf(stderr, "\n"); }
    }
    if (!dma_name(base)) rlog("hw-write", "%08x <- %llx (%d bytes)", a, (unsigned long long)v, size);
    else rlog("dma-reg", "%08x <- %llx (%d bytes)", a, (unsigned long long)v, size);
    s_regs[a] = v;
}

void ee_hw_read128(u32 a, GPR128* out) {
    a &= 0x1fffffff;
    if ((a >> 16) == 0x1100) rlog("vu-mem-read", "%08x (16 bytes)", a);
    if ((a & 0xfffff000u) == 0x10007000u) { ipu_fifo_read(out->UW); return; }   // IPU output FIFO
    rlog("hw-read128", "%08x", a);
    std::memset(out, 0, 16);
    out->UD[0] = ee_hw_read(a, 8);
}
void ee_hw_write128(u32 a, const GPR128* in) {
    a &= 0x1fffffff;
    // FIFOs: 0x10004000 VIF0, 0x10005000 VIF1, 0x10006000 GIF, 0x10007000 IPU
    if ((a & 0xfffff000u) == 0x10006000u) { extern void gt_push_data(int, const u32*); gt_push_data(1, in->UW); return; }
    if ((a & 0xfffff000u) == 0x10005000u) { extern void vif1_fifo_write(const u32*); vif1_fifo_write(in->UW); return; }
    if (a == 0x10007010u) { ipu_fifo_write(in->UW); return; }                   // IPU input FIFO
    rlog("hw-write128", "%08x <- %016llx%016llx", a, (unsigned long long)in->UD[1], (unsigned long long)in->UD[0]);
    if (a < 0x10004000u) ee_hw_write(a, in->UD[0], 8);
}

// ---------------------------------------------------------------- SIF syscalls (IOP side is HLE, see iop_hle.cpp later)
static u32 s_sif_regs[32];
static u32 s_sif_dma_id = 1;
bool sif_syscall(EEContext* c, int num) {
    switch (num) {
    case 0x76: c->r[2].SD[0] = -1; return true;                  // SifDmaStat: done
    case 0x77: {                                                   // SifSetDma(transfers*, count)
        u32 t = c->r[4].UW[0], n = c->r[5].UW[0];
        for (u32 i = 0; i < n; i++) {
            u32 src = ee_rd<u32>(t + i * 16), dst = ee_rd<u32>(t + i * 16 + 4), size = ee_rd<u32>(t + i * 16 + 8), attr = ee_rd<u32>(t + i * 16 + 12);
            extern void iop_sif_dma(u32 src, u32 dst, u32 size, u32 attr);
            iop_sif_dma(src, dst, size, attr);
        }
        c->r[2].SD[0] = s_sif_dma_id++;
        return true;
    }
    case 0x78: return true;                                        // SifSetDChain
    case 0x79: {                                                   // SifSetReg(reg, value)
        u32 r = c->r[4].UW[0], v = c->r[5].UW[0];
        if (r & 0x80000000u) s_sif_regs[r & 31] = v;
        else if (r == 1) s_sif_msflg = v; else if (r == 2) s_sif_smflg = v;
        else s_sif_regs[r & 31] = v;
        c->r[2].SD[0] = 0; return true;
    }
    case 0x7a: {                                                   // SifGetReg(reg)
        u32 r = c->r[4].UW[0], v;
        if (r == 4) v = s_sif_smflg; else if (r == 3) v = s_sif_msflg; else v = s_sif_regs[r & 31];
        if (r == 0x80000002u) v = 1;                               // SIF_SYSREG_RPCINIT
        c->r[2].SD[0] = (s32)v; return true;
    }
    }
    return false;
}

// ---------------------------------------------------------------- COP0
bool g_eie = true;                                             // COP0 Status.EIE (interrupts enabled)
u32 ee_cop0_read(EEContext* c, int reg) {
    switch (reg) {
        case 9: return (u32)(u64)(host_time_ns() * 0.294912);           // Count @ 294.912 MHz
        case 12: return (c->cop0[12] & ~0x10000u) | (g_eie ? 0x10000u : 0) | 0x10000000u;   // Status (EIE = bit 16)
        case 15: return 0x2e20;                                    // PRId: R5900
        case 25: return (u32)(u64)(host_time_ns() * 0.294912);          // perf counters
        default: return c->cop0[reg];
    }
}
void ee_cop0_write(EEContext* c, int reg, u32 v) { c->cop0[reg] = v; if (reg == 12) g_eie = (v >> 16) & 1; }
void ee_cop0(EEContext* c, u32 pc, u32 w) {
    (void)c;
    if (w == 0x42000038) { g_eie = true; return; }                 // ei
    if (w == 0x42000039) { g_eie = false; return; }                // di: no interrupts / preemption until ei
    if ((w >> 21 & 31) == 16) {
        switch (w & 63) { case 2: case 6: case 1: case 8: return; }  // TLB ops
    }
    rlog("cop0", "unhandled %08x at %08x", w, pc);
}
void ee_break(EEContext* c, u32 pc) { rlog("break", "BREAK at %08x ra=%08x", pc, c->r[31].UW[0]); }

// ---------------------------------------------------------------- rare instructions (interpreted)
static inline s16 sat16(s32 v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (s16)v; }
void ee_interp(EEContext* c, u32 pc, u32 w) {
    int op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31, fn = w & 63;
    GPR128 S = c->r[rs], T = c->r[rt], D = c->r[rd];
    bool wr = true;
    if (op == 28) {
        switch (fn) {
        case 4:  for (int i = 0; i < 2; i++) { s32 v = S.SW[i]; u32 x = v < 0 ? ~(u32)v : (u32)v; int n = 0; while (n < 32 && !(x & 0x80000000u)) { x <<= 1; n++; } D.UW[i] = n - 1; } break;  // PLZCW
        case 32: case 33: {                                                      // MADD1 / MADDU1
            ee_madd(c, rd, rs, rt, 1, fn == 33); return; }
        case 52: for (int i = 0; i < 8; i++) D.UH[i] = T.UH[i] << (sa & 15); break;  // PSLLH
        case 54: for (int i = 0; i < 8; i++) D.UH[i] = T.UH[i] >> (sa & 15); break;  // PSRLH
        case 55: for (int i = 0; i < 8; i++) D.SH[i] = T.SH[i] >> (sa & 15); break;  // PSRAH
        case 60: for (int i = 0; i < 4; i++) D.UW[i] = T.UW[i] << sa; break;         // PSLLW
        case 62: for (int i = 0; i < 4; i++) D.UW[i] = T.UW[i] >> sa; break;         // PSRLW
        case 63: for (int i = 0; i < 4; i++) D.SW[i] = T.SW[i] >> sa; break;         // PSRAW
        case 48: {                                                                    // PMFHL
            if (sa == 0) { D.UW[0] = c->lo.UW[0]; D.UW[1] = c->hi.UW[0]; D.UW[2] = c->lo.UW[2]; D.UW[3] = c->hi.UW[2]; }
            else if (sa == 1) { D.UW[0] = c->lo.UW[1]; D.UW[1] = c->hi.UW[1]; D.UW[2] = c->lo.UW[3]; D.UW[3] = c->hi.UW[3]; }
            else if (sa == 3) { for (int i = 0; i < 2; i++) { D.SH[i*4+0] = sat16(c->lo.SW[i*2]); D.SH[i*4+1] = sat16(c->lo.SW[i*2+1]); D.SH[i*4+2] = sat16(c->hi.SW[i*2]); D.SH[i*4+3] = sat16(c->hi.SW[i*2+1]); } }
            else { rlog("interp", "PMFHL.%d at %08x", sa, pc); }
            break;
        }
        case 8:   // MMI0
            switch (sa) {
            case 0: for (int i = 0; i < 4; i++) D.UW[i] = S.UW[i] + T.UW[i]; break;   // PADDW
            case 1: for (int i = 0; i < 4; i++) D.UW[i] = S.UW[i] - T.UW[i]; break;   // PSUBW
            case 2: for (int i = 0; i < 4; i++) D.UW[i] = S.SW[i] > T.SW[i] ? ~0u : 0; break;
            case 3: for (int i = 0; i < 4; i++) D.SW[i] = S.SW[i] > T.SW[i] ? S.SW[i] : T.SW[i]; break;
            case 4: for (int i = 0; i < 8; i++) D.UH[i] = S.UH[i] + T.UH[i]; break;
            case 5: for (int i = 0; i < 8; i++) D.UH[i] = S.UH[i] - T.UH[i]; break;
            case 6: for (int i = 0; i < 8; i++) D.UH[i] = S.SH[i] > T.SH[i] ? 0xffff : 0; break;
            case 7: for (int i = 0; i < 8; i++) D.SH[i] = S.SH[i] > T.SH[i] ? S.SH[i] : T.SH[i]; break;
            case 8: for (int i = 0; i < 16; i++) D.UB[i] = S.UB[i] + T.UB[i]; break;
            case 9: for (int i = 0; i < 16; i++) D.UB[i] = S.UB[i] - T.UB[i]; break;
            case 18: D.UW[0] = T.UW[0]; D.UW[1] = S.UW[0]; D.UW[2] = T.UW[1]; D.UW[3] = S.UW[1]; break;   // PEXTLW
            case 19: D.UW[0] = T.UW[0]; D.UW[1] = T.UW[2]; D.UW[2] = S.UW[0]; D.UW[3] = S.UW[2]; break;   // PPACW
            case 22: for (int i = 0; i < 4; i++) { D.UH[i*2] = T.UH[i]; D.UH[i*2+1] = S.UH[i]; } break;   // PEXTLH
            case 23: for (int i = 0; i < 4; i++) { D.UH[i] = T.UH[i*2]; D.UH[i+4] = S.UH[i*2]; } break;   // PPACH
            case 26: for (int i = 0; i < 8; i++) { D.UB[i*2] = T.UB[i]; D.UB[i*2+1] = S.UB[i]; } break;   // PEXTLB
            case 27: for (int i = 0; i < 8; i++) { D.UB[i] = T.UB[i*2]; D.UB[i+8] = S.UB[i*2]; } break;   // PPACB
            default: wr = false; rlog("interp", "MMI0.%d at %08x", sa, pc);
            }
            break;
        case 40:  // MMI1
            switch (sa) {
            case 1: for (int i = 0; i < 4; i++) D.SW[i] = S.SW[i] < 0 ? -S.SW[i] : S.SW[i]; break;
            case 2: for (int i = 0; i < 4; i++) D.UW[i] = S.UW[i] == T.UW[i] ? ~0u : 0; break;
            case 3: for (int i = 0; i < 4; i++) D.SW[i] = S.SW[i] < T.SW[i] ? S.SW[i] : T.SW[i]; break;
            case 6: for (int i = 0; i < 8; i++) D.UH[i] = S.UH[i] == T.UH[i] ? 0xffff : 0; break;
            case 7: for (int i = 0; i < 8; i++) D.SH[i] = S.SH[i] < T.SH[i] ? S.SH[i] : T.SH[i]; break;
            case 10: for (int i = 0; i < 16; i++) D.UB[i] = S.UB[i] == T.UB[i] ? 0xff : 0; break;
            case 16: ee_padduw(c, rd, rs, rt); return;
            case 18: D.UW[0] = T.UW[2]; D.UW[1] = S.UW[2]; D.UW[2] = T.UW[3]; D.UW[3] = S.UW[3]; break;   // PEXTUW
            case 22: for (int i = 0; i < 4; i++) { D.UH[i*2] = T.UH[i+4]; D.UH[i*2+1] = S.UH[i+4]; } break;
            case 26: for (int i = 0; i < 8; i++) { D.UB[i*2] = T.UB[i+8]; D.UB[i*2+1] = S.UB[i+8]; } break;
            case 27: {                                                                  // QFSRV
                u32 sh = c->sa & 15; u8 buf[32]; std::memcpy(buf, T.UB, 16); std::memcpy(buf + 16, S.UB, 16);
                std::memcpy(D.UB, buf + sh, 16); break; }
            default: wr = false; rlog("interp", "MMI1.%d at %08x", sa, pc);
            }
            break;
        case 9:   // MMI2
            switch (sa) {
            case 2: for (int i = 0; i < 2; i++) D.SD[i] = (s32)(T.UW[i*2] << (S.UW[i*2] & 31)); break;   // PSLLVW
            case 3: for (int i = 0; i < 2; i++) D.SD[i] = (s32)(T.UW[i*2] >> (S.UW[i*2] & 31)); break;
            case 8: D = c->hi; break; case 9: D = c->lo; break;
            case 10: for (int i = 0; i < 4; i++) { D.UH[i*2] = T.UH[i]; D.UH[i*2+1] = S.UH[i+4]; } break;  // PINTH
            case 12: for (int i = 0; i < 2; i++) { s64 p = (s64)S.SW[i*2] * T.SW[i*2]; D.SD[i] = p; c->lo.SD[i] = (s32)p; c->hi.SD[i] = (s32)(p >> 32); } break;  // PMULTW
            case 14: D.UD[0] = T.UD[0]; D.UD[1] = S.UD[0]; break;
            case 18: D.UD[0] = S.UD[0] & T.UD[0]; D.UD[1] = S.UD[1] & T.UD[1]; break;
            case 19: D.UD[0] = S.UD[0] ^ T.UD[0]; D.UD[1] = S.UD[1] ^ T.UD[1]; break;
            case 26: D.UH[0] = T.UH[2]; D.UH[1] = T.UH[1]; D.UH[2] = T.UH[0]; D.UH[3] = T.UH[3]; D.UH[4] = T.UH[6]; D.UH[5] = T.UH[5]; D.UH[6] = T.UH[4]; D.UH[7] = T.UH[7]; break;  // PEXEH
            case 27: for (int i = 0; i < 2; i++) for (int j = 0; j < 4; j++) D.UH[i*4+j] = T.UH[i*4+3-j]; break;  // PREVH
            case 30: D.UW[0] = T.UW[2]; D.UW[1] = T.UW[1]; D.UW[2] = T.UW[0]; D.UW[3] = T.UW[3]; break;  // PEXEW
            case 31: D.UW[0] = T.UW[1]; D.UW[1] = T.UW[2]; D.UW[2] = T.UW[0]; D.UW[3] = T.UW[3]; break;  // PROT3W
            default: wr = false; rlog("interp", "MMI2.%d at %08x", sa, pc);
            }
            break;
        case 41:  // MMI3
            switch (sa) {
            case 3: for (int i = 0; i < 2; i++) D.SD[i] = T.SW[i*2] >> (S.UW[i*2] & 31); break;   // PSRAVW
            case 8: c->hi = S; return; case 9: c->lo = S; return;
            case 10: for (int i = 0; i < 4; i++) { D.UH[i*2] = T.UH[i*2]; D.UH[i*2+1] = S.UH[i*2]; } break;  // PINTEH
            case 12: for (int i = 0; i < 2; i++) { u64 p = (u64)S.UW[i*2] * T.UW[i*2]; D.UD[i] = p; c->lo.SD[i] = (s32)p; c->hi.SD[i] = (s32)(p >> 32); } break;
            case 14: D.UD[0] = S.UD[1]; D.UD[1] = T.UD[1]; break;
            case 18: D.UD[0] = S.UD[0] | T.UD[0]; D.UD[1] = S.UD[1] | T.UD[1]; break;
            case 19: D.UD[0] = ~(S.UD[0] | T.UD[0]); D.UD[1] = ~(S.UD[1] | T.UD[1]); break;
            case 26: D.UH[0] = T.UH[0]; D.UH[1] = T.UH[2]; D.UH[2] = T.UH[1]; D.UH[3] = T.UH[3]; D.UH[4] = T.UH[4]; D.UH[5] = T.UH[6]; D.UH[6] = T.UH[5]; D.UH[7] = T.UH[7]; break;  // PEXCH
            case 27: for (int i = 0; i < 4; i++) { D.UH[i] = T.UH[0]; D.UH[i+4] = T.UH[4]; } break;
            case 30: D.UW[0] = T.UW[0]; D.UW[1] = T.UW[2]; D.UW[2] = T.UW[1]; D.UW[3] = T.UW[3]; break;  // PEXCW
            default: wr = false; rlog("interp", "MMI3.%d at %08x", sa, pc);
            }
            break;
        default: wr = false; rlog("interp", "MMI fn=%d at %08x (%08x)", fn, pc, w);
        }
        if (wr && rd) c->r[rd] = D;
        return;
    }
    if (op == 17) {
        int fmt = rs, ft = rt, fs = rd, fd = sa;
        if (fmt == 16) {
            switch (fn) {
            case 49: c->fcc = false; return;                         // c.un.s
            case 51: case 53: case 55: case 57: case 59: case 61: case 63: rlog("interp", "fpu cmp %d", fn); c->fcc = false; return;
            }
        }
        (void)ft; (void)fs; (void)fd;
    }
    // A branch sitting in a delay slot: execute only its non-control effects (linking).
    if (op == 3) { c->r[31].UD[0] = pc + 8; rlog("interp", "jal in delay slot at %08x (target ignored)", pc); return; }
    rlog("interp", "unimplemented instruction %08x at %08x", w, pc);
}

// ---------------------------------------------------------------- VU0 macro mode
static inline float vclamp(float v) { return ee_fclamp(v); }
bool vu0_cond(EEContext* c, int t) { (void)c; return t == 0; }   // VU0 never "busy"

void vu0_macro(EEContext* c, u32 w) {
    int dest = (w >> 21) & 15, ft = (w >> 16) & 31, fs = (w >> 11) & 31, fd = (w >> 6) & 31, fn = w & 63;
    int it = ft & 15, is = fs & 15, id = fd & 15;
    float (*vf)[4] = c->vf;
    float* acc = c->vacc;
    float I; std::memcpy(&I, &c->vi[21], 4);
    float Q = c->vq;
    auto msk = [&](int i) { return dest & (8 >> i); };
    auto store = [&](int reg, const float r[4]) { if (reg == 0) return; for (int i = 0; i < 4; i++) if (msk(i)) vf[reg][i] = vclamp(r[i]); };
    auto store_acc = [&](const float r[4]) { for (int i = 0; i < 4; i++) if (msk(i)) acc[i] = vclamp(r[i]); };
    float r[4];
    if (fn < 60) {
        int bc = fn & 3;
        float bcv = vf[ft][bc];
        auto binop = [&](int kind, float t_scalar, bool scalar, bool to_acc) {
            for (int i = 0; i < 4; i++) {
                float a = vf[fs][i], b = scalar ? t_scalar : vf[ft][i];
                switch (kind) {
                case 0: r[i] = a + b; break; case 1: r[i] = a - b; break;
                case 2: r[i] = acc[i] + a * b; break; case 3: r[i] = acc[i] - a * b; break;
                case 4: r[i] = a > b ? a : b; break; case 5: r[i] = a < b ? a : b; break;
                case 6: r[i] = a * b; break;
                }
            }
            if (to_acc) store_acc(r); else store(fd, r);
        };
        if (fn < 28) { static const int k[7] = {0, 1, 2, 3, 4, 5, 6}; binop(k[fn >> 2], bcv, true, false); return; }
        switch (fn) {
        case 28: binop(6, Q, true, false); return; case 29: binop(4, I, true, false); return;
        case 30: binop(6, I, true, false); return; case 31: binop(5, I, true, false); return;
        case 32: binop(0, Q, true, false); return; case 33: binop(2, Q, true, false); return;
        case 34: binop(0, I, true, false); return; case 35: binop(2, I, true, false); return;
        case 36: binop(1, Q, true, false); return; case 37: binop(3, Q, true, false); return;
        case 38: binop(1, I, true, false); return; case 39: binop(3, I, true, false); return;
        case 40: binop(0, 0, false, false); return; case 41: binop(2, 0, false, false); return;
        case 42: binop(6, 0, false, false); return; case 43: binop(4, 0, false, false); return;
        case 44: binop(1, 0, false, false); return; case 45: binop(3, 0, false, false); return;
        case 46: {                                                                     // VOPMSUB
            float a[3] = {vf[fs][1] * vf[ft][2], vf[fs][2] * vf[ft][0], vf[fs][0] * vf[ft][1]};
            float o[4] = {acc[0] - a[0], acc[1] - a[1], acc[2] - a[2], vf[fd][3]};
            int save = dest; dest = 14; store(fd, o); dest = save; return;
        }
        case 47: binop(5, 0, false, false); return;
        case 48: if (id) c->vi[id] = (u16)(c->vi[is] + c->vi[it]); return;             // VIADD
        case 49: if (id) c->vi[id] = (u16)(c->vi[is] - c->vi[it]); return;
        case 50: { int imm = (w >> 6) & 31; if (imm & 16) imm -= 32; if (it) c->vi[it] = (u16)(c->vi[is] + imm); return; }
        case 52: if (id) c->vi[id] = c->vi[is] & c->vi[it]; return;
        case 53: if (id) c->vi[id] = c->vi[is] | c->vi[it]; return;
        case 56: case 57: rlog("vu0", "VCALLMS not implemented (%08x)", w); return;
        }
        rlog("vu0", "special1 fn=%d (%08x)", fn, w);
        return;
    }
    int op2 = ((w >> 6) & 31) * 4 + (fn & 3);
    int bc = op2 & 3;
    auto accop = [&](int kind, float s, bool scalar) {
        for (int i = 0; i < 4; i++) {
            float a = vf[fs][i], b = scalar ? s : vf[ft][i];
            switch (kind) {
            case 0: r[i] = a + b; break; case 1: r[i] = a - b; break;
            case 2: r[i] = acc[i] + a * b; break; case 3: r[i] = acc[i] - a * b; break;
            case 6: r[i] = a * b; break;
            }
        }
        store_acc(r);
    };
    if (op2 < 16) { static const int k[4] = {0, 1, 2, 3}; accop(k[op2 >> 2], vf[ft][bc], true); return; }
    if (op2 >= 24 && op2 < 28) { accop(6, vf[ft][bc], true); return; }
    switch (op2) {
    case 16: case 17: case 18: case 19: {                                               // VITOF
        static const float sc[4] = {1.0f, 1.0f / 16, 1.0f / 4096, 1.0f / 32768};
        for (int i = 0; i < 4; i++) { s32 v; std::memcpy(&v, &vf[fs][i], 4); r[i] = v * sc[op2 - 16]; }
        store(ft, r); return;
    }
    case 20: case 21: case 22: case 23: {                                               // VFTOI
        static const float sc[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
        if (!ft) return;
        for (int i = 0; i < 4; i++) if (msk(i)) { s32 v = ee_cvtw(vf[fs][i] * sc[op2 - 20]); std::memcpy(&vf[ft][i], &v, 4); }
        return;
    }
    case 28: accop(6, Q, true); return; case 29: { for (int i = 0; i < 4; i++) r[i] = std::fabs(vf[fs][i]); store(ft, r); return; }
    case 30: accop(6, I, true); return;
    case 31: {                                                                          // VCLIPw
        float wv = std::fabs(vf[ft][3]); u32 f = 0;
        for (int i = 0; i < 3; i++) { if (vf[fs][i] > wv) f |= 1u << (i * 2); if (vf[fs][i] < -wv) f |= 2u << (i * 2); }
        c->vi[18] = ((c->vi[18] << 6) | f) & 0xffffff; return;
    }
    case 32: accop(0, Q, true); return; case 33: accop(2, Q, true); return;
    case 34: accop(0, I, true); return; case 35: accop(2, I, true); return;
    case 36: accop(1, Q, true); return; case 37: accop(3, Q, true); return;
    case 38: accop(1, I, true); return; case 39: accop(3, I, true); return;
    case 40: accop(0, 0, false); return; case 41: accop(2, 0, false); return;
    case 42: accop(6, 0, false); return; case 44: accop(1, 0, false); return;
    case 45: accop(3, 0, false); return;
    case 46: {                                                                          // VOPMULA
        float o[4] = {vf[fs][1] * vf[ft][2], vf[fs][2] * vf[ft][0], vf[fs][0] * vf[ft][1], acc[3]};
        int save = dest; dest = 14; store_acc(o); dest = save; return;
    }
    case 47: case 59: return;                                                           // VNOP, VWAITQ
    case 48: { for (int i = 0; i < 4; i++) r[i] = vf[fs][i]; store(ft, r); return; }   // VMOVE
    case 49: { r[0] = vf[fs][1]; r[1] = vf[fs][2]; r[2] = vf[fs][3]; r[3] = vf[fs][0]; store(ft, r); return; }  // VMR32
    case 56: { int fsf = (w >> 21) & 3, ftf = (w >> 23) & 3; float b = vf[ft][ftf]; c->vq = b == 0 ? 3.4028235e38f : vf[fs][fsf] / b; std::memcpy(&c->vi[22], &c->vq, 4); return; }
    case 57: { int ftf = (w >> 23) & 3; c->vq = std::sqrt(std::fabs(vf[ft][ftf])); std::memcpy(&c->vi[22], &c->vq, 4); return; }
    case 58: { int fsf = (w >> 21) & 3, ftf = (w >> 23) & 3; float b = std::sqrt(std::fabs(vf[ft][ftf])); c->vq = b == 0 ? 3.4028235e38f : vf[fs][fsf] / b; std::memcpy(&c->vi[22], &c->vq, 4); return; }
    case 60: { int fsf = (w >> 21) & 3; s32 v; std::memcpy(&v, &vf[fs][fsf], 4); if (it) c->vi[it] = (u16)v; return; }  // VMTIR
    case 61: { if (!ft) return; for (int i = 0; i < 4; i++) if (msk(i)) { s32 v = (s16)c->vi[is]; std::memcpy(&vf[ft][i], &v, 4); } return; }  // VMFIR
    }
    rlog("vu0", "special2 op=%d (%08x)", op2, w);
}
