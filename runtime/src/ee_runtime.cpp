#include <filesystem>
// Memory map, ELF loading, call dispatch, logging, entry point.
#include "ee_ops.h"
#include "ee_runtime.h"
#include "iop.h"
#include "settings.h"
#include "music_import.h"
#include "loc_patch.h"
#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <map>
#include <fstream>
#include <sstream>
#include <mutex>
#include <string>
#include <vector>
#include <csignal>

RuntimeConfig g_cfg;
extern int g_year_shift;                     // hle.cpp: the calendar's first year - 2005
bool g_opt_nodh = false;                     // no DH in either league (era packs before 1973)
bool g_opt_dh = false;                       // CPU play "2026 rules": the DH in every park (tools/recomp/patches.txt)
float g_sim_drain = -1.0f, g_sim_pull = -1.0f;      // patches.txt: tiring and pitching changes in simulated games (-1: the game's)
bool g_opt_hook = false;                     // ... pitchers taken out earlier in simulated games
float g_sim_steal = -1.0f;                 // patches.txt: steal attempts in simulated games (-1: the game's 0.3728)
bool g_opt_steal = false;                    // ... and stolen-base attempts in simulated games at today's rate
static bool g_era_pack = false; int g_era_year = 0;   // a league pack of a past season (tools/eras)
bool g_opt_fastboot = std::getenv("MVP_SLOWBOOT") == nullptr;   // no 4 s black wait at boot (tools/recomp/patches.txt)
bool g_opt_widescreen = false;               // --widescreen: EA's hidden 16:9 mode (tools/recomp/patches.txt)
u8* g_ram = nullptr;
u8 g_spr[0x4000];

// ---------------------------------------------------------------- logging
static std::map<std::string, int> s_logcount;
static std::recursive_mutex s_log_mx;   // recursive: the crash/SIGINT handler logs from inside rlog too
std::string g_roster_status = "Original (2005)";
void rlog(const char* key, const char* fmt, ...) {
    std::lock_guard<std::recursive_mutex> lk(s_log_mx);  // the graphics / raster threads log too
    int& n = s_logcount[key];
    static const char* unlimited = std::getenv("MVP_LOG_ALL");    // MVP_LOG_ALL=key1,key2: no print limit for these keys
    if (unlimited && n >= 0) { const char* p = std::strstr(unlimited, key); size_t kl = std::strlen(key);
        if (p && (p == unlimited || p[-1] == ',') && (p[kl] == 0 || p[kl] == ',')) n = -1000000000; }
    if (++n > g_cfg.log_limit) { if (n == g_cfg.log_limit + 1) fprintf(stderr, "[%s] (further messages suppressed)\n", key); return; }
    va_list ap; va_start(ap, fmt);
    static const bool with_frame = std::getenv("MVP_LOG_FRAMES") != nullptr;   // prefix lines with the game frame
    if (with_frame) { extern u64 g_game_frames; fprintf(stderr, "f%llu ", (unsigned long long)g_game_frames); }
    fprintf(stderr, "[%s] ", key); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}
u32 g_trace_ring[4096]; u32 g_trace_pos; u32 g_hits[0x200000];
u32 g_watch_lo = 0xffffffff, g_watch_hi = 0xffffffff;
u32 g_rwatch_lo = 0xffffffff, g_rwatch_hi = 0xffffffff;       // MVP_RWATCH=lo-hi: log reads
void ee_rwatch_hit(u32 a, int size) {
    EEContext* c = kernel_current();
    rlog("rwatch", "read %08x (%d bytes) in fn %08x ra=%08x", a, size, g_trace_ring[(g_trace_pos - 1) & 4095], c ? c->r[31].UW[0] : 0);
}
u32 g_logcall = 0xffffffff;                                   // MVP_LOGCALL=addr: log args/return of one function
static u32 s_lc_pos = 0, s_lc_a0 = 0;
static std::string lc_str(u32 a) {            // printable guest string at a, or ""
    std::string o; if (a < 0x100000 || a >= 0x02000000) return o;
    for (u32 i = 0; i < 80; i++) { u8 ch = g_ram[a + i]; if (!ch) break; if (ch < 32 || ch > 126) return ""; o += (char)ch; }
    return o.size() >= 2 ? o : "";
}
static bool s_lc_on = true;
void ee_logcall_enter(EEContext* c, u32 a) {
    static const int every = std::getenv("MVP_LOGCALL_EVERY") ? std::atoi(std::getenv("MVP_LOGCALL_EVERY")) : 1;   // sample one call in n
    static u64 nth = 0; if (every > 1 && (nth++ % every) != 0) { s_lc_on = false; return; }
    s_lc_on = true;
    // MVP_LOGCALL_IF="a2:0=170": only calls where the word at register + offset has that value
    static const char* cond = std::getenv("MVP_LOGCALL_IF");
    if (cond) {
        u32 v = c->r[4 + (cond[1] - '0')].UW[0];
        if (cond[2] == '=') { s_lc_on = v == (u32)std::strtoul(cond + 3, nullptr, 0); if (!s_lc_on) return; goto on; }   // "a3=684": the register itself
        char* q; u32 off = (u32)std::strtoul(cond + 3, &q, 0); u32 want = (u32)std::strtoul(q + 1, nullptr, 0);
        s_lc_on = v >= 0x10000 && v < 0x02000000 - off && ee_rd<u32>(v + off) == want;
        if (!s_lc_on) return;
    }
    on:
    fprintf(stderr, "[logcall] %08x(a0=%08x a1=%08x a2=%08x a3=%08x) ra=%08x t=%.3f\n", a, c->r[4].UW[0], c->r[5].UW[0], c->r[6].UW[0], c->r[7].UW[0], c->r[31].UW[0], host_time_ns() / 1e9);
    if (false) rlog("logcall", "%08x(a0=%08x a1=%08x a2=%08x a3=%08x) ra=%08x t=%.3f", a, c->r[4].UW[0], c->r[5].UW[0], c->r[6].UW[0], c->r[7].UW[0], c->r[31].UW[0], host_time_ns() / 1e9);
    for (int k = 4; k < 8; k++) { std::string t = lc_str(c->r[k].UW[0]); if (!t.empty()) fprintf(stderr, "   a%d = \"%s\"\n", k - 4, t.c_str()); }
    // MVP_LOGCALL_PEEK="a0:0:1020:1780:20,a0:0:1740": follow pointers from a register (a0-a3) through those offsets
    static const char* peek = std::getenv("MVP_LOGCALL_PEEK");
    if (peek) {
        fprintf(stderr, "   peek:");
        for (const char* p = peek; *p;) {
            u32 v = c->r[4 + (p[1] - '0')].UW[0]; p += 2; fprintf(stderr, " [%08x", v);
            while (*p == ':') { u32 off = (u32)std::strtoul(p + 1, (char**)&p, 0); v = (v >= 0x10000 && v < 0x02000000 - off) ? ee_rd<u32>(v + off) : 0xdeadbeef; fprintf(stderr, " +%u=%08x", off, v); }
            fprintf(stderr, "]"); if (*p == ',') p++;
        }
        fprintf(stderr, "\n");
    }
    static const bool hex = std::getenv("MVP_LOGCALL_HEX") != nullptr;      // first bytes behind a1 (short or unprintable names)
    if (hex) { u32 a1 = c->r[5].UW[0]; if (a1 >= 0x100000 && a1 < 0x02000000 - 16) { fprintf(stderr, "   a1 bytes:"); for (int k = 0; k < 12; k++) fprintf(stderr, " %02x", g_ram[a1 + k]); fprintf(stderr, "\n"); } }
    static const int hist = std::getenv("MVP_LOGCALL_HIST") ? std::atoi(std::getenv("MVP_LOGCALL_HIST")) : 0;   // the calls before this one
    if (hist > 0) { fprintf(stderr, "   before:"); for (int k = hist; k >= 1; k--) fprintf(stderr, " %08x", g_trace_ring[(g_trace_pos - k) & 4095]); fprintf(stderr, "\n"); }
    static int shown = 0; static const bool all_stacks = std::getenv("MVP_LOGCALL_STACK") != nullptr;
    s_lc_a0 = c->r[4].UW[0];
    if (shown++ < 3 || all_stacks) { fprintf(stderr, "   stack:"); for (int k = 0; k < c->shadow_n && k < 64; k++) fprintf(stderr, " %08x", c->shadow[k]); fprintf(stderr, "\n"); }
    s_lc_pos = g_trace_pos;
}
void ee_logcall_leave(EEContext* c, u32 a) {
    if (!s_lc_on) return;
    fprintf(stderr, "[logcall] %08x -> v0=%08x\n", a, c->r[2].UW[0]);
    static const int soff = std::getenv("MVP_LOGCALL_STROFF") ? std::atoi(std::getenv("MVP_LOGCALL_STROFF")) : -1;   // string at a0 + n after the call
    if (soff >= 0) fprintf(stderr, "   [a0+%d] = \"%s\"\n", soff, lc_str(s_lc_a0 + (u32)soff).c_str());
    static const bool tree = std::getenv("MVP_LOGCALL_TREE") != nullptr; static int shown = 0;
    if (tree && shown++ < 40) { fprintf(stderr, "   called:"); for (u32 p = s_lc_pos; p != g_trace_pos && p - s_lc_pos < 400; p++) fprintf(stderr, " %08x", g_trace_ring[p & 4095]); fprintf(stderr, "\n"); }
}
// MVP_DBG_SYMTAB=1 (trace builds): watches the game's symbol table (object at 0x8f3980: count, size, slots; the
// run-time linker's name -> value map). Reports every write to the slot array that does not come from the table's
// own insert/remove code, and the first vblank at which a slot holds something that is not a pointer into RAM.
static bool s_dbg_symtab = false; static u32 s_symtab_slots = 0, s_symtab_size = 0;
void dbg_symtab_check(u64 vblank) {
    static int init = 0; if (!init) { init = 1; s_dbg_symtab = std::getenv("MVP_DBG_SYMTAB") != nullptr; }
    if (!s_dbg_symtab) return;
    u32 size = ee_rd<u32>(0x8f3984), slots = ee_rd<u32>(0x8f3988);
    if (slots != s_symtab_slots || size != s_symtab_size) {
        fprintf(stderr, "[symtab] vblank %llu: slots %08x size %u count %u\n", (unsigned long long)vblank, slots, size, ee_rd<u32>(0x8f3980));
        s_symtab_slots = slots; s_symtab_size = size;
#ifdef EE_TRACE
        g_watch_lo = slots & 0x1fffffff; g_watch_hi = g_watch_lo + size * 4;
#endif
    }
    static int reported = 0;
    for (u32 i = 0; slots && i < size && reported < 20; i++) {
        u32 v = ee_rd<u32>(slots + 4 * i);
        if (v && ((v & 0x1fffffff) < 0x00100000 || (v & 0x1fffffff) >= 0x02000000 || (v & 3))) {
            reported++; fprintf(stderr, "[symtab] vblank %llu: slot %u of %u holds %08x; around:", (unsigned long long)vblank, i, size, v);
            for (int k = -4; k < 12; k++) fprintf(stderr, " %08x", ee_rd<u32>(slots + 4 * (i + k)));
            fprintf(stderr, "\n");
        }
    }
}
bool g_abi_check = false;
void ee_abi_violation(EEContext* c, u32 fn, const u64* sv) {
    u32 first = ee_rd<u32>(fn);
    if ((first >> 16) != 0x27bd || !(first & 0x8000)) return;    // not a function with its own frame: a fragment that
                                                                 // was entered in the middle (jump tables, shared tails)
    static std::map<u32, int> seen; int& n = seen[fn];
    if (n++ >= 3 || seen.size() > 60) return;                    // each function three times, 60 functions
    { extern u64 kernel_vblank_count(); fprintf(stderr, "v%llu ", (unsigned long long)kernel_vblank_count()); }
    static const char* nm[10] = {"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "sp", "fp"};
    fprintf(stderr, "[abi] %08x returned with", fn);
    for (int i = 0; i < 10; i++) { u64 now = c->r[i < 8 ? 16 + i : i == 8 ? 29 : 30].UD[0]; if (now != sv[i]) fprintf(stderr, " %s %08x->%08x", nm[i], (u32)sv[i], (u32)now); }
    fprintf(stderr, "  called from:");
    for (int k = c->shadow_n > 8 ? c->shadow_n - 8 : 0; k < c->shadow_n; k++) fprintf(stderr, " %08x", c->shadow[k]);
    fprintf(stderr, "\n");
}
void ee_watch_hit(u32 a, u64 v, int size) {
    EEContext* c = kernel_current();
    if (s_dbg_symtab) {
        u32 fn = g_trace_ring[(g_trace_pos - 1) & 4095];
        if (fn == 0x0055d868 || fn == 0x0055da60) return;            // the table's own insert (and growth) and remove
        static int n = 0; if (n++ > 40) return;
        fprintf(stderr, "[symtab] foreign write %08x <- %llx (%d bytes) ra=%08x fn=%08x stack:", a, (unsigned long long)v, size, c ? c->r[31].UW[0] : 0, fn);
        if (c) for (int k = c->shadow_n > 12 ? c->shadow_n - 12 : 0; k < c->shadow_n; k++) fprintf(stderr, " %08x", c->shadow[k]);
        fprintf(stderr, "\n"); return;
    }
    static int nw = 0; if (nw++ > 3000) return;
    fprintf(stderr, "[watch] write %08x <- %llx (%d bytes) ra=%08x stack:", a, (unsigned long long)v, size, c ? c->r[31].UW[0] : 0);
    if (c) for (int k = c->shadow_n > 16 ? c->shadow_n - 16 : 0; k < c->shadow_n; k++) fprintf(stderr, " %08x", c->shadow[k]);
    fprintf(stderr, "\n");
}
void ee_dump_trace() {
    // most recent distinct functions, newest last
    std::vector<u32> seen;
    for (u32 i = 0; i < 4096 && seen.size() < 48; i++) {
        u32 a = g_trace_ring[(g_trace_pos - 1 - i) & 4095];
        if (!a) break;
        if (std::find(seen.begin(), seen.end(), a) == seen.end()) seen.push_back(a);
    }
    fprintf(stderr, "recent distinct guest functions (newest last):\n");
    for (size_t i = seen.size(); i-- > 0;) fprintf(stderr, " %08x", seen[i]);
    fprintf(stderr, "\n");
}
void rfatal(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[FATAL] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
    ee_dump_trace();
    { extern void kernel_dump(); extern EEContext* kernel_active_ctx(); kernel_dump();
      EEContext* c = kernel_active_ctx();
      if (c && c->shadow_n > 0) { fprintf(stderr, "active stack:"); for (int k = 0; k < c->shadow_n && k < 1024; k++) fprintf(stderr, " %08x", c->shadow[k]); fprintf(stderr, "\n"); } }
    fflush(stderr); fflush(stdout);
    std::_Exit(1);         // not exit(): static destructors can wait for ever on the graphics and audio threads
}

// ---------------------------------------------------------------- memory
void mem_init() {
    g_ram = (u8*)std::calloc(1, 0x02000000);
    if (!g_ram) rfatal("out of memory");
    std::memset(g_spr, 0, sizeof g_spr);
}

u8* ee_ptr_slow(u32 a) {
    switch (a >> 28) {
        case 0x0: case 0x2: case 0x3: case 0x8: case 0x9: case 0xA: case 0xB: {
            u32 p = a & 0x0fffffff;
            if (p < 0x02000000) return g_ram + p;
            if ((a >> 28) == 0 || p >= 0x10000000) break;
            break;
        }
        case 0x7:
            if ((a & 0x0fffffff) < 0x4000) return g_spr + (a & 0x3fff);
            break;
        default: break;
    }
    if ((a & 0x0fffffff) >= 0x10000000 || (a & 0xf0000000) == 0x10000000 || (a & 0xf0000000) == 0x12000000) return nullptr;
    if ((a & 0xfff00000) == 0x1fc00000 || (a & 0xfff00000) == 0xbfc00000) return nullptr;   // BIOS
    return nullptr;
}

// ---------------------------------------------------------------- ELF
bool elf_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    std::vector<u8> d;
    fseek(f, 0, SEEK_END); d.resize(ftell(f)); fseek(f, 0, SEEK_SET);
    if (fread(d.data(), 1, d.size(), f) != d.size()) { fclose(f); return false; }
    fclose(f);
    if (d.size() < 52 || std::memcmp(d.data(), "\x7f" "ELF", 4) != 0) return false;
    u32 phoff; u16 phnum;
    std::memcpy(&phoff, &d[28], 4); std::memcpy(&phnum, &d[44], 2);
    for (int i = 0; i < phnum; i++) {
        u32 ph[8]; std::memcpy(ph, &d[phoff + 32 * i], 32);
        if (ph[0] != 1) continue;
        u32 off = ph[1], va = ph[2], fsz = ph[4], msz = ph[5];
        std::memcpy(g_ram + (va & 0x01ffffff), &d[off], fsz);
        std::memset(g_ram + (va & 0x01ffffff) + fsz, 0, msz - fsz);
        rlog("elf", "loaded segment va=%08x filesz=%x memsz=%x", va, fsz, msz);
    }
    return true;
}

// ---------------------------------------------------------------- dispatch
// Indirect calls (virtual methods, jump tables, function pointers) were a binary search over the 30 000 functions
// on every call: 6 % of the main thread in a simulated season. A flat table over the executable's code range answers
// in one load (built once; 16 MB of pointers).
static EEFunc* s_func_flat = nullptr; static u32 s_flat_lo = 0, s_flat_hi = 0;
static void build_func_flat() {
    if (!g_ee_funcs_count) return;
    s_flat_lo = g_ee_funcs[0].start & ~3u; s_flat_hi = (g_ee_funcs[g_ee_funcs_count - 1].start & ~3u) + 4;
    s_func_flat = (EEFunc*)calloc((s_flat_hi - s_flat_lo) / 4, sizeof(EEFunc));
    if (!s_func_flat) return;
    for (size_t i = 0; i < g_ee_funcs_count; i++) if ((g_ee_funcs[i].start & 3) == 0) s_func_flat[(g_ee_funcs[i].start - s_flat_lo) / 4] = g_ee_funcs[i].fn;
}
EEFunc ee_find_func(u32 addr) {
    if (!s_func_flat) build_func_flat();
    if (s_func_flat) return (addr - s_flat_lo < s_flat_hi - s_flat_lo && (addr & 3) == 0) ? s_func_flat[(addr - s_flat_lo) / 4] : nullptr;
    size_t lo = 0, hi = g_ee_funcs_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_ee_funcs[mid].start < addr) lo = mid + 1; else hi = mid;
    }
    if (lo < g_ee_funcs_count && g_ee_funcs[lo].start == addr) return g_ee_funcs[lo].fn;
    return nullptr;
}

static EEFuncAt find_label(u32 addr) {
    size_t lo = 0, hi = g_ee_labels_count;                 // table is sorted by address
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (g_ee_labels[mid].addr < addr) lo = mid + 1; else hi = mid; }
    return (lo < g_ee_labels_count && g_ee_labels[lo].addr == addr) ? g_ee_labels[lo].fn : nullptr;
}

static const bool s_strict_calls = std::getenv("MVP_STRICT") != nullptr;   // file scope: no guarded statics (coroutines!)
void ee_call(EEContext* c, u32 addr) {
    addr &= 0x1fffffff;                    // kseg mirrors of code
    if (g_cfg.trace_calls) fprintf(stderr, "call %08x\n", addr);
    if (EEFunc fn = ee_find_func(addr)) { fn(c); return; }
    if (EEFuncAt fa = find_label(addr)) { fa(c, addr); return; }
    { extern bool ee_ram_code_at(u32); extern void ee_interpret(EEContext*, u32);       // code the game loaded into RAM
      if (ee_ram_code_at(addr)) { ee_interpret(c, addr); return; } }
    // A call into data means state already diverged from real hardware. Log it and carry on (returning 0)
    // instead of dying; set MVP_STRICT=1 to stop here while debugging.
    const bool strict = s_strict_calls;
    if (!strict && addr != 0) {
        // inside the program's code: the recompiler has no entry point there. That is a hole in the recompiler, not
        // a wild pointer - the caller's frame is lost if this was a fall-through (see findings, session 20).
        if (addr >= 0x00100000 && addr < 0x007d32c0) rlog("no-entry", "jump to %08x: no entry point there (ra=%08x), skipped", addr, c->r[31].UW[0]);
        else {
            rlog("bad-call", "call to non-code address %08x (ra=%08x) skipped; it holds %08x %08x %08x %08x %08x %08x", addr, c->r[31].UW[0],
                 ee_rd<u32>(addr), ee_rd<u32>(addr + 4), ee_rd<u32>(addr + 8), ee_rd<u32>(addr + 12), ee_rd<u32>(addr + 16), ee_rd<u32>(addr + 20));
            if (const char* dir = std::getenv("MVP_DUMP_BADCALL")) {            // the memory around the first few targets, to look at
                static int n = 0;
                if (n++ < 6 && addr >= 0x00110000 && addr < 0x01ff0000) {
                    char fn[512]; std::snprintf(fn, sizeof fn, "%s/badcall_%08x.bin", dir, addr);
                    if (FILE* f = std::fopen(fn, "wb")) { std::fwrite(g_ram + addr - 0x10000, 1, 0x20000, f); std::fclose(f); }
                }
            }
        }
        c->r[2].UD[0] = 0;
        return;
    }
    fprintf(stderr, "regs: v0=%08x v1=%08x a0=%08x a1=%08x a2=%08x a3=%08x s0=%08x s1=%08x s2=%08x s3=%08x sp=%08x\n",
            c->r[2].UW[0], c->r[3].UW[0], c->r[4].UW[0], c->r[5].UW[0], c->r[6].UW[0], c->r[7].UW[0],
            c->r[16].UW[0], c->r[17].UW[0], c->r[18].UW[0], c->r[19].UW[0], c->r[29].UW[0]);
    rfatal("indirect call to unknown address %08x (ra=%08x)", addr, c->r[31].UW[0]);
}

static void on_signal(int sig);
// ---------------------------------------------------------------- sampling profiler (Linux, MVP_PROF=file)
#if defined(__linux__)
#include <sys/time.h>
#include <ucontext.h>
#include <execinfo.h>
#include <dlfcn.h>
static uintptr_t s_prof_pc[1 << 20]; static volatile u32 s_prof_n = 0;
extern u64 g_game_frames;
static const u64 s_prof_from = std::getenv("MVP_PROF_FROM") ? std::strtoull(std::getenv("MVP_PROF_FROM"), nullptr, 10) : 0;
static void on_prof(int, siginfo_t*, void* uc) {
    u32 n = s_prof_n; if (n >= (1u << 20) || g_game_frames < s_prof_from) return;
#if defined(__aarch64__)
    s_prof_pc[n] = (uintptr_t)((ucontext_t*)uc)->uc_mcontext.pc;
#elif defined(__x86_64__)
    s_prof_pc[n] = (uintptr_t)((ucontext_t*)uc)->uc_mcontext.gregs[REG_RIP];
#endif
    s_prof_n = n + 1;
}
static void on_segv(int sig, siginfo_t* si, void* uc) {
    uintptr_t pc = 0;
#if defined(__aarch64__)
    pc = (uintptr_t)((ucontext_t*)uc)->uc_mcontext.pc;
#elif defined(__x86_64__)
    pc = (uintptr_t)((ucontext_t*)uc)->uc_mcontext.gregs[REG_RIP];
#endif
    fprintf(stderr, "\n[crash] signal %d at host pc=%lx, fault address %p\n", sig, (unsigned long)pc, si->si_addr);
    void* bt[32]; int n = backtrace(bt, 32); backtrace_symbols_fd(bt, n, 2);
    on_signal(sig);
}
static void crash_handler_install() {
    struct sigaction sa = {}; sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    static char altstack[1 << 16]; stack_t st = {}; st.ss_sp = altstack; st.ss_size = sizeof altstack; sigaltstack(&st, nullptr);
    sigaction(SIGSEGV, &sa, nullptr); sigaction(SIGBUS, &sa, nullptr);
}
static void prof_start() {
    struct sigaction sa = {}; sa.sa_sigaction = on_prof; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigaction(SIGPROF, &sa, nullptr);
    itimerval t = {{0, 1000}, {0, 1000}}; setitimer(ITIMER_PROF, &t, nullptr);
}
static void prof_dump() {
    const char* f = std::getenv("MVP_PROF"); if (!f) return;
    Dl_info di; uintptr_t base = 0; if (dladdr((void*)&prof_start, &di)) base = (uintptr_t)di.dli_fbase;
    if (FILE* o = fopen(f, "w")) { fprintf(o, "base %lx\n", (unsigned long)base); for (u32 i = 0; i < s_prof_n; i++) fprintf(o, "%lx\n", (unsigned long)s_prof_pc[i]); fclose(o); }
    // the memory map next to it, to put names on samples in shared libraries
    if (FILE* m = fopen("/proc/self/maps", "r")) {
        if (FILE* o = fopen((std::string(f) + ".maps").c_str(), "w")) { char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof buf, m)) > 0) fwrite(buf, 1, n, o); fclose(o); }
        fclose(m);
    }
}
#else
static void crash_handler_install() {}
static void prof_start() {}
static void prof_dump() {}
#endif

// ---------------------------------------------------------------- main
static void on_signal(int sig) {
    prof_dump();
    fprintf(stderr, "\n[signal %d]\n", sig);
    ee_dump_trace();
    extern void kernel_dump(); kernel_dump();
    extern void gs_dma_stats(); gs_dma_stats(); extern void ipu_stats(); ipu_stats(); extern void snd_stats(); snd_stats(); extern void vu1_block_stats(); vu1_block_stats();
    { extern u64 g_xfer_bytes_psm[64]; for (int i = 0; i < 64; i++) if (g_xfer_bytes_psm[i]) fprintf(stderr, "xfer psm %02x: %llu bytes\n", i, (unsigned long long)g_xfer_bytes_psm[i]); }
    if (const char* rd = std::getenv("MVP_RAMDUMP")) {      // MVP_RAMDUMP=file : the 32 MB of main memory at exit
        if (FILE* f = fopen(rd, "wb")) { fwrite(g_ram, 1, 32u << 20, f); fclose(f); }
    }
    if (const char* hd = std::getenv("MVP_HITDUMP")) {      // MVP_HITDUMP=file : all function call counts
        if (FILE* f = fopen(hd, "w")) {
            for (u32 i = 0; i < 0x200000; i++) if (g_hits[i]) fprintf(f, "%08x %u\n", i << 2, g_hits[i]);
            fclose(f);
        }
    }
    if (const char* h = std::getenv("MVP_HITS")) {         // MVP_HITS=func[,func...] : call counts (EE_TRACE builds)
        std::string s(h); size_t p = 0;
        while (p < s.size()) { size_t q = s.find(',', p); if (q == std::string::npos) q = s.size();
            u32 a = (u32)std::strtoul(s.substr(p, q - p).c_str(), nullptr, 16);
            fprintf(stderr, "  calls to %08x: %u\n", a, g_hits[(a >> 2) & 0x1fffff]);
            p = q + 1; }
    }
    if (const char* dbg = std::getenv("MVP_DUMP")) {       // MVP_DUMP=addr[,addr...] : print words at exit
        std::string s(dbg); size_t p = 0;
        while (p < s.size()) { size_t q = s.find(',', p); if (q == std::string::npos) q = s.size();
            u32 a = (u32)std::strtoul(s.substr(p, q - p).c_str(), nullptr, 16);
            fprintf(stderr, "  [%08x] = %08x %08x %08x %08x\n", a, ee_rd<u32>(a), ee_rd<u32>(a + 4), ee_rd<u32>(a + 8), ee_rd<u32>(a + 12));
            p = q + 1; }
    }
    { extern void snd_audio_shutdown(); snd_audio_shutdown(); }
    std::_Exit(128 + sig);
}

int main(int argc, char** argv) {
    { extern void data_dir_init(const char*); data_dir_init(argc ? argv[0] : nullptr); }   // the game folder becomes the current directory
    std::signal(SIGTERM, on_signal);
    if (const char* w = std::getenv("MVP_RWATCH")) {
        g_rwatch_lo = (u32)std::strtoul(w, nullptr, 16);
        const char* d = std::strchr(w, '-'); g_rwatch_hi = d ? (u32)std::strtoul(d + 1, nullptr, 16) : g_rwatch_lo + 4;
    }
    if (const char* lc = std::getenv("MVP_LOGCALL")) g_logcall = (u32)std::strtoul(lc, nullptr, 16);
    g_abi_check = std::getenv("MVP_DBG_ABI") != nullptr;
    if (const char* w = std::getenv("MVP_WATCH")) {        // MVP_WATCH=lo-hi (hex): log guest writes to that range
        g_watch_lo = (u32)std::strtoul(w, nullptr, 16);
        const char* d = std::strchr(w, '-'); g_watch_hi = d ? (u32)std::strtoul(d + 1, nullptr, 16) : g_watch_lo + 4;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGSEGV, on_signal);
    crash_handler_install();
    if (std::getenv("MVP_PROF")) prof_start();
    if (argc >= 4 && std::string(argv[1]) == "--vu1-gen") { extern int vu1_gen(const char*, const char*); int rc = vu1_gen(argv[2], argv[3]); fflush(stderr); std::_Exit(rc); }
    if (argc >= 3 && std::string(argv[1]) == "--vu1-bench") { extern int vu1_bench(const char*, int); int rc = vu1_bench(argv[2], argc > 3 ? atoi(argv[3]) : 10); prof_dump(); fflush(stderr); std::_Exit(rc); }
    if (argc >= 4 && std::string(argv[1]) == "--gs-replay") { extern int gs_replay(const char*, const char*); int rc = gs_replay(argv[2], argv[3]); fflush(stderr); std::_Exit(rc); }
    if (argc >= 3 && std::string(argv[1]) == "--music-import") {       // convert songs into music/ without the launcher
        int bad = 0;
        for (int i = 2; i < argc; i++) { std::string e = music_import(argv[i]); if (!e.empty()) bad++; fprintf(stderr, "%s: %s\n", argv[i], e.empty() ? "imported" : e.c_str()); }
        for (const MusicTrack& t : music_tracks()) fprintf(stderr, "  %s  (%s - %s, %d:%02d)\n", t.path.c_str(), t.artist.c_str(), t.title.c_str(), (int)t.seconds / 60, (int)t.seconds % 60);
        fflush(stderr); std::_Exit(bad ? 1 : 0);
    }
    if (argc >= 4 && std::string(argv[1]) == "--music-export") {       // music/x.asf (or a song copied off the disc) -> WAV, for checking
        std::vector<s16> pcm; int rate = 0, ch = 0;
        bool ok = music_decode_asf(argv[2], pcm, rate, ch) && music_write_wav(argv[3], pcm, rate, ch);
        fprintf(stderr, ok ? "wrote %s (%d Hz, %d ch, %.1f s)\n" : "could not decode %s\n", ok ? argv[3] : argv[2], rate, ch, ch ? pcm.size() / (double)ch / rate : 0.0);
        fflush(stderr); std::_Exit(ok ? 0 : 1);
    }
    for (int i = 1; i < argc; i++) if (std::string(argv[i]) == "--config" && i + 1 < argc) settings_set_path(argv[i + 1]);
    if (settings_load(settings_path())) fprintf(stderr, "settings: %s\n", settings_path().c_str());
    if (argc >= 2 && std::string(argv[1]) == "--features") {          // list / install / remove features without the launcher
        extern int features_cli(int, char**); int rc = features_cli(argc - 2, argv + 2); fflush(stderr); std::_Exit(rc);
    }
    bool force_launcher = false, skip_launcher = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--trace-calls") g_cfg.trace_calls = true;
        else if (a == "--config" && i + 1 < argc) i++;
        else if (a == "--fps" && i + 1 < argc) g_settings.fps_limit = std::atoi(argv[++i]);
        else if (a == "--res" && i + 1 < argc) g_settings.internal_res = std::atoi(argv[++i]);
        else if (a == "--frameskip" && i + 1 < argc) g_settings.frameskip = std::atoi(argv[++i]);
        else if (a == "--fullscreen") g_settings.fullscreen = true;
        else if (a == "--windowed") g_settings.fullscreen = false;
        else if (a == "--4:3") g_settings.aspect = 0;
        else if (a == "--iso" && i + 1 < argc) g_cfg.iso_path = argv[++i];
        else if (a == "--elf" && i + 1 < argc) g_cfg.elf_path = argv[++i];
        else if (a == "--mod" && i + 1 < argc) g_cfg.mods.push_back(argv[++i]);
        else if (a == "--log-limit" && i + 1 < argc) g_cfg.log_limit = std::atoi(argv[++i]);
        else if (a == "--widescreen") g_settings.aspect = 1;
        else if (a == "--aspect" && i + 1 < argc) g_settings.aspect = std::max(0, std::min(5, std::atoi(argv[++i])));
        else if (a == "--headless") { extern bool g_headless; g_headless = true; }
        else if (a == "--launcher") force_launcher = true;
        else if (a == "--play") skip_launcher = true;
        else {
            fprintf(stderr, "usage: %s [--iso game.iso] [--elf game/SLUS_211.35] [--config file.ini]\n"
                            "          [--widescreen | --4:3] [--fullscreen | --windowed] [--fps N (0 = unlimited)] [--frameskip 0-4] [--res 1-4]\n"
                            "          [--launcher | --play] [--headless] [--trace-calls]\n", argv[0]);
            return 1;
        }
    }
    if (std::getenv("MVP_WIDESCREEN")) g_settings.aspect = 1;
    if (const char* a = std::getenv("MVP_ASPECT")) g_settings.aspect = std::max(0, std::min(5, std::atoi(a)));
    if (const char* h = std::getenv("MVP_HUD169")) g_settings.hud_169 = std::atoi(h) != 0;
    settings_apply();
    fprintf(stderr, "MVP Baseball 2005 recomp — %zu recompiled functions\n", g_ee_funcs_count);
    // window first: the launcher (settings + importing the disc) runs before anything of the game is loaded
    { extern void frontend_init(); frontend_init(); }
    if (!skip_launcher) {
        extern bool frontend_launcher(std::string& iso_path, const std::string& elf_path, bool force);
        if (!frontend_launcher(g_cfg.iso_path, g_cfg.elf_path, force_launcher)) return 0;
    }
    if (g_cfg.iso_path.empty() && !g_settings.iso.empty()) g_cfg.iso_path = g_settings.iso;
    settings_apply();
    mem_init();
    if (!elf_load(g_cfg.elf_path.c_str())) rfatal("cannot load %s (start the game without --play and import your disc in the launcher)", g_cfg.elf_path.c_str());
    hw_init();
    { extern void snd_audio_init(); extern void snd_audio_shutdown(); snd_audio_init(); std::atexit(snd_audio_shutdown); }
    if (g_cfg.iso_path.empty()) {
        // default: first .iso in game/
        const char* guesses[] = {"game/MVP Baseball 2005 (USA).iso", "game/game.iso", "game/mvp2005.iso"};
        for (const char* g : guesses) if (FILE* f = fopen(g, "rb")) { fclose(f); g_cfg.iso_path = g; break; }
    }
    if (g_cfg.iso_path.empty() || !iso_open(g_cfg.iso_path.c_str()))
        fprintf(stderr, "warning: no disc image (use --iso <path to your MVP Baseball 2005 ISO>); file access will fail\n");
    else {
        // modern rosters (mods/rosters-2026), then any --mod folders (later ones win)
        extern std::string g_roster_status;
        g_roster_status = "Original (2005)";
        // a dynasty's first year and the balance of simulated games, written by the roster builders
        auto league_files = [](const std::string& dir) {
            if (FILE* yf = fopen((dir + "/year.txt").c_str(), "r")) {
                // the game's own calendar starts in 2005, see g_year_shift
                int y = 0;
                if (fscanf(yf, "%d", &y) == 1 && y >= 1871 && y <= 2100 && y != 2005) { g_year_shift = y - 2005; g_roster_status += ", first season " + std::to_string(y); }
                fclose(yf);
            }
            { extern int hr_factors_load(const std::string&); hr_factors_load(dir + "/hr_factor.txt"); }   // hle.cpp
            { extern int pitch_factors_load(const std::string&); pitch_factors_load(dir + "/pitch_factor.txt"); }
            { extern int bat_factors_load(const std::string&); bat_factors_load(dir + "/bat_factor.txt"); }
            if (FILE* sf = fopen((dir + "/sim.txt").c_str(), "r")) {                    // see hle_pitch_quality
                extern float g_fb_base; char key[64]; float v;
                while (fscanf(sf, "%63s %f", key, &v) == 2)
                    if (!strcmp(key, "fastball_base") && v > 0.7f && v < 1.2f) g_fb_base = v;
                    else if (!strcmp(key, "contact_spread") && v > 0.3f && v < 1.5f) { extern float g_contact_spread; g_contact_spread = v; }
                    else if (!strcmp(key, "power_spread") && v > 0.3f && v < 1.5f) { extern float g_power_spread; g_power_spread = v; }
                    else if (!strcmp(key, "contact_shift") && v > -0.4f && v < 0.4f) { extern float g_contact_shift; g_contact_shift = v; }
                    else if (!strcmp(key, "power_shift") && v > -0.4f && v < 0.4f) { extern float g_power_shift; g_power_shift = v; }
                    else if (!strcmp(key, "calibrate")) { extern int g_sim_calibrate; g_sim_calibrate = v != 0; }
                    else if (!strcmp(key, "wild_pitch") && v >= 0 && v < 0.2f) { extern float g_wild_pitch; g_wild_pitch = v; }
                    else if (!strcmp(key, "error_rate") && v >= 0 && v < 0.4f) { extern float g_error_rate; g_error_rate = v; }
                    else if (!strcmp(key, "season_games") && v >= 100 && v <= 180) { extern int g_season_games; g_season_games = (int)v; }
                    else if (!strcmp(key, "drain") && v > 0 && v < 4) { extern float g_sim_drain; g_sim_drain = v; }   // tiring in simulated games
                    else if (!strcmp(key, "pull") && v > 0 && v < 1) { extern float g_sim_pull; g_sim_pull = v; }     // when the starter goes
                    else if (!strncmp(key, "spread_", 7) && v >= 0.5f && v <= 3.0f) {
                        extern float g_sim_spread[7]; static const char* const nm[7] = {"k", "bb", "hbp", "1b", "2b", "3b", "hr"};
                        for (int i = 0; i < 7; i++) if (!strcmp(key + 7, nm[i])) g_sim_spread[i] = v; }
                fclose(sf);
            }
            extern int league_rates_load(const char*);                                   // the league's real rates by season (hle_pa_probs)
            league_rates_load((dir + "/rates.txt").c_str());
            {   // its own HD textures (the players' made faces: tools/faces/build_faces.py)
                extern void gs_add_replace_dir(const char*); std::error_code ec;
                if (std::filesystem::is_directory(dir + "/textures/replace", ec)) { static std::vector<std::string> keep; keep.push_back(dir + "/textures/replace"); gs_add_replace_dir(keep.back().c_str()); }
            }
        };
        { extern int league_rates_load(const char*); extern int g_sim_calibrate; league_rates_load("mods/league_rates.txt"); g_sim_calibrate = g_settings.sim_league; }   // real major-league rates, 1871-2025
        if (g_settings.rosters == 1) {
            if (iso_add_overlay("mods/rosters-2026") > 0) {
                g_roster_status = "Modern (2026)";
                league_files("mods/rosters-2026");
                // commentator surname clips for the new players (same database plus audio ids, silenced PA clips)
                if (g_settings.commentary && iso_add_overlay("mods/commentary-2026") > 0) g_roster_status += " + commentary names";
            }
            else g_roster_status = "Original (mods/rosters-2026 not found)";
        }
        else if (g_settings.rosters == 2) {                                              // a league pack (tools/leaguepack.py)
            const std::string dir = "mods/" + g_settings.league;
            {   // an era pack (tools/eras/build_era.py) brings its season's uniforms, parks and logos: today's are not put over them
                std::ifstream pj(dir + "/pack.json"); std::stringstream ss; ss << pj.rdbuf();
                g_era_pack = ss.str().find("\"era\": true") != std::string::npos;
                std::ifstream yf(dir + "/year.txt"); if (g_era_pack) yf >> g_era_year;
            }
            if (!g_settings.league.empty() && g_settings.league.find("..") == std::string::npos && iso_add_overlay(dir.c_str()) > 0) {
                g_roster_status = "League pack " + g_settings.league;
                league_files(dir);
                loc_apply_strings(dir.c_str());                                            // its clubs' names in the menus (strings.txt)
                if (g_settings.commentary && iso_add_overlay((dir + "/commentary").c_str()) > 0) g_roster_status += " + commentary names";
                extern int draft_classes_load(const char*);
                if (int n = draft_classes_load(dir.c_str())) g_roster_status += " + draft classes (" + std::to_string(n) + " players)";
            }
            else g_roster_status = "Original (league pack " + dir + " not found)";
        }
        if (const char* e = getenv("MVP_YEAR")) { int y = atoi(e); if (y >= 2005 && y <= 2100) g_year_shift = y - 2005; }   // debug
        g_opt_dh = g_opt_steal = g_opt_hook = g_settings.ai == 1;
        if (g_era_pack && g_era_year && g_era_year < 2022) g_opt_dh = false;                // the season's own rule: the DH in the AL only (the game's)
        if (g_era_pack && g_era_year && g_era_year < 1973) g_opt_nodh = true;              // ... and before 1973 pitchers hit in both leagues
        if (g_era_pack && g_era_year) {                                                    // the rest of "2026 rules" by the season too
            g_opt_hook = g_opt_hook && g_era_year >= 2015;                                 // starters out a time through the order earlier
            g_opt_steal = g_opt_steal && ((g_era_year >= 1975 && g_era_year <= 1992) || g_era_year >= 2023);   // the running eras
        }
        if (g_opt_steal) g_sim_steal = 0.48f;                                              // today's running game (2026 rules)
        if (g_era_pack && g_era_year && g_settings.ai == 1) { extern void sim_steal_for(int); extern bool g_steal_by_season; g_steal_by_season = true; sim_steal_for(g_era_year); }   // or the era's
        if (g_opt_hook) { g_sim_drain = 2.0f; g_sim_pull = 0.4f; extern float g_sim_relief; g_sim_relief = 3.2f; extern int g_sim_bf_cap; g_sim_bf_cap = 27; extern float g_sim_one_inning; g_sim_one_inning = 0.6f;   // one-inning relievers (hle_sim_tire)
            if (const char* e = getenv("MVP_RELIEF")) g_sim_relief = (float)atof(e);
            if (const char* e = getenv("MVP_PULL")) g_sim_pull = (float)atof(e);
            if (const char* e = getenv("MVP_DRAIN")) g_sim_drain = (float)atof(e);
            if (const char* e = getenv("MVP_RESTED")) { extern float g_sim_rested; g_sim_rested = (float)atof(e); }
            extern float g_sim_spent; g_sim_spent = 0.25f; if (const char* e = getenv("MVP_SPENT")) g_sim_spent = (float)atof(e);
            if (const char* e = getenv("MVP_FLOOR")) { extern float g_sim_floor; g_sim_floor = (float)atof(e); }
            if (const char* e = getenv("MVP_MAXRUN")) { extern int g_sim_max_run; g_sim_max_run = atoi(e); }
            if (const char* e = getenv("MVP_WIN")) { extern int g_sim_win; g_sim_win = atoi(e); }
            if (const char* e = getenv("MVP_WINDAYS")) { extern int g_sim_win_days; g_sim_win_days = atoi(e); }
            if (const char* e = getenv("MVP_BFCAP")) { extern int g_sim_bf_cap; g_sim_bf_cap = atoi(e); }
            if (const char* e = getenv("MVP_CLOSER")) { extern float g_sim_closer; g_sim_closer = (float)atof(e); }
            if (const char* e = getenv("MVP_ONEINN")) { extern float g_sim_one_inning; g_sim_one_inning = (float)atof(e); } }                       // today's managers (2026 rules)
        else if (g_era_pack && g_era_year && g_settings.ai == 1 && g_sim_drain < 0 && g_sim_pull < 0) {   // the complete-game eras
            extern bool g_sim_workload_by_season; g_sim_workload_by_season = true;          // (hle.cpp: follows the dynasty's seasons)
            extern void sim_workload_for(int); sim_workload_for(g_era_year);
        }
        if (const char* e = getenv("MVP_FB_BASE")) { extern float g_fb_base; g_fb_base = (float)atof(e); }                // debug
        if (const char* e = getenv("MVP_SIM")) {                                                                          // debug: "contact_shift=-0.05,power_spread=1.1"
            extern float g_contact_spread, g_power_spread, g_contact_shift, g_power_shift;
            std::string s(e); size_t p = 0;
            while (p < s.size()) { size_t q = s.find(',', p); if (q == std::string::npos) q = s.size(); std::string kv = s.substr(p, q - p); size_t eq = kv.find('=');
                if (eq != std::string::npos) { std::string k = kv.substr(0, eq); float v = (float)atof(kv.c_str() + eq + 1);
                    if (k == "contact_spread") g_contact_spread = v; else if (k == "power_spread") g_power_spread = v; else if (k == "contact_shift") g_contact_shift = v; else if (k == "power_shift") g_power_shift = v; else if (k == "fastball_base") { extern float g_fb_base; g_fb_base = v; } else if (k == "calibrate") { extern int g_sim_calibrate; g_sim_calibrate = v != 0; } else if (k == "wild_pitch") { extern float g_wild_pitch; g_wild_pitch = v; } else if (k == "error_rate") { extern float g_error_rate; g_error_rate = v; } else if (k == "season_games") { extern int g_season_games; g_season_games = (int)v; } else if (k == "drain" || k == "pull") { (k == "drain" ? g_sim_drain : g_sim_pull) = v; extern bool g_sim_workload_by_season; g_sim_workload_by_season = false; }
                    else if (k.rfind("spread_", 0) == 0) { extern float g_sim_spread[7]; static const char* const nm[7] = {"k", "bb", "hbp", "1b", "2b", "3b", "hr"}; for (int i = 0; i < 7; i++) if (k.substr(7) == nm[i]) g_sim_spread[i] = v; } }
                p = q + 1; } }
        // CPU tuning for today's rules (the game's own tuning file, edited: tools/build_ai.py)
        if (g_settings.ai == 1 && iso_add_overlay("mods/ai-2026") > 0) g_roster_status += " | CPU: 2026 rules";
        if (g_settings.head_shapes && iso_add_overlay("mods/heads") > 0) g_roster_status += " | varied heads";   // tools/heads/head_shapes.py
        // uniforms and ballparks as they are today, as far as the game's own art allows (tools/build_uniforms.py, build_stadiums.py)
        if (g_settings.uniforms == 1 && !g_era_pack && iso_add_overlay("mods/uniforms-2026") > 0) g_roster_status += " | uniforms: 2026";
        if (g_settings.stadiums == 1 && !g_era_pack && iso_add_overlay("mods/stadiums-2026") + loc_apply_strings("mods/stadiums-2026") > 0) g_roster_status += " | ballparks: 2026";
        // rendered portraits of today's players (tools/portraits): new pictures in PORTRAIT.BIG, the players they
        // belong to, and their HD versions
        // a league pack's own (mods/<pack>/portraits/: the same three things, made from that pack's players)
        if (g_settings.rosters == 2 && g_settings.portraits && !g_settings.league.empty() && g_settings.league.find("..") == std::string::npos) {
            extern int portraits_load(const char*); extern void gs_add_replace_dir(const char*);
            const std::string pd = "mods/" + g_settings.league + "/portraits";
            int n = portraits_load((pd + "/portraits.txt").c_str());
            if (n && iso_add_overlay(pd.c_str()) > 0) { gs_add_replace_dir((pd + "/textures/replace").c_str()); g_roster_status += " | portraits: " + std::to_string(n); }
        }
        if (g_settings.rosters == 1 && g_settings.portraits) {
            extern int portraits_load(const char*); extern void gs_add_replace_dir(const char*);
            int n = portraits_load("mods/portraits-2026/portraits.txt");
            if (n && iso_add_overlay("mods/portraits-2026") > 0) { gs_add_replace_dir("mods/portraits-2026/textures/replace"); g_roster_status += " | portraits: " + std::to_string(n); }
        }
        if (g_settings.rosters == 2 && !g_settings.league.empty() && g_settings.league.find("..") == std::string::npos)
            iso_add_overlay(("mods/" + g_settings.league + "/art").c_str());                // a league pack's logos win over the 2026 ones
        for (const std::string& m : g_cfg.mods) { iso_add_overlay(m.c_str()); loc_apply_strings(m.c_str()); }
        if (g_era_pack && g_settings.rosters == 2) {                                      // its uniforms follow the dynasty's seasons
            extern int era_uniforms_setup(const char*);
            if (era_uniforms_setup(("mods/" + g_settings.league).c_str()) > 0) g_roster_status += " | uniforms by season";
        }
        // your own music in place of the game's menu songs (music/*.asf, converted by the launcher's import)
        if (g_settings.music) { int n = music_apply(g_settings.music); if (n) g_roster_status += " | your music in " + std::to_string(n) + " of 9 song slots"; }
    }
    kernel_run_main(g_ee_entry);
    fprintf(stderr, "guest program returned\n");
    return 0;
}
