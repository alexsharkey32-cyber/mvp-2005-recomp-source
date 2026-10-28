#include <chrono>
// VU1 micro-mode interpreter.
// Executes EAGL's vertex microprograms uploaded with MPG and started with MSCAL/MSCNT; XGKICK sends
// GIF packets from VU1 memory to the GS (PATH1). Q/P results have hardware-accurate latency because
// hand-scheduled VU code often reads the previous Q while the next DIV is in flight.
#define VU1_CORE_IMPL
#include "vu1_core.h"
#include <algorithm>
#include <ctime>
#include <unordered_map>
#include <vector>
using namespace vu1i;
// ---------------------------------------------------------------- precompiled blocks
// A block is a straight run of pairs from an entry pc up to and including the delay slot of the first
// branch (or of the E bit). Blocks seen while playing are dumped (MVP_VU1BLOCKS=file) and turned into C++
// by `mvp2005 --vu1-gen file out` (generated/vu1_blocks.inc): each pair becomes a call of step() with a
// compile-time Dec, so the compiler inlines the handlers with their register numbers folded in. Blocks are
// found by the hash of their instruction words (EAGL moves its microprograms around in micro memory; the
// code is position-independent apart from branch targets, which are computed from the entry pc).
#if __has_include("vu1_blocks_table.inc")
#include "vu1_blocks_table.inc"
#else
static const VuBlockDef kVuBlocks[] = {{0, 0, nullptr, nullptr}};
#endif
static const bool s_blocks_off = std::getenv("MVP_VU1_NOBLOCKS") != nullptr;
static const bool s_blocks_check = std::getenv("MVP_VU1CHECK") != nullptr;
static const char* s_blocks_dump = std::getenv("MVP_VU1BLOCKS");
// What is known about the block starting at each pair: 0 not looked up yet, 0xffff interpret, else 1 + index of
// the compiled block in kVuBlocks; s_blk_dep = how many pairs from there on that answer depends on. The game
// loads a different microprogram over part of the micro memory for every kind of material (1,500 times a second
// in a match): only the answers that reach into the pairs that really changed are thrown away.
static u16 s_blk_ref[0x800], s_blk_dep[0x800]; static u32 s_max_dep = 1;
static u16 s_blk_len[0x800]; static u64 s_blk_hash[0x800];          // of the last look-up (block dumping only)
static u64 s_blk_runs = 0, s_blk_pairs = 0, s_blk_misses = 0, s_check_fail = 0;
static u64 s_blk_lookups = 0, s_interp_pairs = 0, s_mpg_changed = 0, s_redecoded = 0;      // statistics
#include <unordered_map>
#include <vector>
struct SeenBlock { u32 npairs; std::vector<u32> words; u64 count = 0; };
static std::unordered_map<u64, SeenBlock> s_seen;
static std::unordered_map<u64, const VuBlockDef*>& block_table() {
    static std::unordered_map<u64, const VuBlockDef*> t;
    if (t.empty()) for (const VuBlockDef& b : kVuBlocks) if (b.fn) t[b.hash] = &b;
    return t;
}
static inline bool is_branch_lower(u32 lw) {
    if (lw & 0x80000000u) return false;
    u32 op = lw >> 25;
    return op == 0x20 || op == 0x21 || op == 0x24 || op == 0x25 || (op >= 0x28 && op <= 0x2f);
}
// extent of the block starting at pair idx (0 = can't be a block)
static u32 block_extent(u32 idx) {
    for (u32 n = 0; n < 1024; n++) {
        u32 k = (idx + n) & 0x7ff;
        u32 lw, uw; std::memcpy(&lw, g_vu1_code + k * 8, 4); std::memcpy(&uw, g_vu1_code + k * 8 + 4, 4);
        bool ibit = uw & 0x80000000u, ebit = uw & 0x40000000u;
        if (ebit || (!ibit && is_branch_lower(lw))) {
            u32 k2 = (k + 1) & 0x7ff;
            u32 lw2, uw2; std::memcpy(&lw2, g_vu1_code + k2 * 8, 4); std::memcpy(&uw2, g_vu1_code + k2 * 8 + 4, 4);
            bool ibit2 = uw2 & 0x80000000u;
            if ((uw2 & 0x40000000u) || (!ibit2 && is_branch_lower(lw2))) return 0;   // unusual delay slot
            if (ebit && !ibit && is_branch_lower(lw)) return 0;
            return n + 2;
        }
    }
    return 0;
}
static u64 block_hash(u32 idx, u32 n) {
    u64 h = 1469598103934665603ull ^ n;
    for (u32 i = 0; i < n * 2; i++) {
        u32 w; std::memcpy(&w, g_vu1_code + (((idx + i / 2) & 0x7ff) * 8) + (i & 1) * 4, 4);
        h = (h ^ w) * 1099511628211ull;
    }
    return h;
}
static __attribute__((noinline)) u16 block_lookup(u32 idx) {
    s_blk_lookups++;
    u16& ref = s_blk_ref[idx];
    ref = 0xffff; s_blk_len[idx] = 0;
    u32 n = block_extent(idx);
    s_blk_dep[idx] = (u16)(n ? n : 1026); s_max_dep = std::max<u32>(s_max_dep, s_blk_dep[idx]);
    if (!n) return ref;
    u64 h = block_hash(idx, n);
    s_blk_hash[idx] = h; s_blk_len[idx] = (u16)n;
    if (s_blocks_dump) {
        SeenBlock& sb = s_seen[h];
        if (sb.words.empty()) { sb.npairs = n; for (u32 i = 0; i < n * 2; i++) { u32 w; std::memcpy(&w, g_vu1_code + (((idx + i / 2) & 0x7ff) * 8) + (i & 1) * 4, 4); sb.words.push_back(w); } }
    }
    if (s_blocks_off) return ref;
    auto& t = block_table();
    auto it = t.find(h);
    if (it == t.end()) { s_blk_misses++; return ref; }
    const VuBlockDef& b = *it->second;
    if (b.npairs != n) return ref;
    for (u32 i = 0; i < n * 2; i++) {                                  // guard against hash collisions
        u32 w; std::memcpy(&w, g_vu1_code + (((idx + i / 2) & 0x7ff) * 8) + (i & 1) * 4, 4);
        if (w != b.words[i]) return ref;
    }
    return ref = (u16)(1 + (&b - kVuBlocks));
}
// pairs first..last (not wrapped: last may be >= 0x800) were changed
static void blocks_invalidate(u32 first, u32 last) {
    if (last - first >= 0x7ff) { std::memset(s_blk_ref, 0, sizeof s_blk_ref); return; }
    for (u32 j = first; j <= last; j++) s_blk_ref[j & 0x7ff] = 0;
    for (u32 back = 1; back < s_max_dep && back < 0x800; back++) {     // blocks that start before and reach in
        u32 idx = (first - back) & 0x7ff;
        if (s_blk_ref[idx] && s_blk_dep[idx] > back) s_blk_ref[idx] = 0;
    }
}

// self-check: run the block, then re-run the same pairs with the interpreter from the saved state, compare
struct VuSnapshot { VU v; FlagUpd fq[16]; u32 fqh, fqt; u8 mem[0x4000]; };
static void snap_take(VuSnapshot& s) { s.v = V; std::memcpy(s.fq, s_fq, sizeof s_fq); s.fqh = s_fq_head; s.fqt = s_fq_tail; std::memcpy(s.mem, g_vu1_mem, 0x4000); }
static void snap_put(const VuSnapshot& s) { V = s.v; std::memcpy(s_fq, s.fq, sizeof s_fq); s_fq_head = s.fqh; s_fq_tail = s.fqt; std::memcpy(g_vu1_mem, s.mem, 0x4000); }
static void check_block(u32 idx, u32 pc, Run& r) {
    static VuSnapshot before, after;
    Run r0 = r;
    snap_take(before);
    const VuBlockDef& def = kVuBlocks[s_blk_ref[idx] - 1];
    def.fn(pc, r);                                                      // compiled (sends XGKICKs)
    Run rc = r;
    snap_take(after);
    snap_put(before);
    Run ri = r0;
    s_xgkick_mute = true;
    for (u32 k = 0; k < def.npairs && !ri.done; k++) {
        u32 p = V.pc; const Dec& d = s_dec[p >> 3]; if (!d.valid) decode(p >> 3);
        step(d, p, ri);
    }
    s_xgkick_mute = false;
    bool same = std::memcmp(&V.vf, &after.v.vf, sizeof V.vf) == 0 && std::memcmp(V.vi, after.v.vi, sizeof V.vi) == 0 &&
                std::memcmp(V.acc, after.v.acc, sizeof V.acc) == 0 && V.q == after.v.q && V.p == after.v.p && V.i == after.v.i &&
                V.cycle == after.v.cycle && V.pc == after.v.pc && V.clip == after.v.clip && V.mac == after.v.mac && V.status == after.v.status &&
                std::memcmp(g_vu1_mem, after.mem, 0x4000) == 0 && ri.done == rc.done && ri.ending == rc.ending && ri.have_branch == rc.have_branch;
    if (!same && s_check_fail++ < 20) fprintf(stderr, "[vu1-check] block at %04x (%u pairs) differs from the interpreter\n", pc, def.npairs);
    snap_put(after); r = rc;
}

// ---------------------------------------------------------------- recorded workloads (benchmark + regression)
// MVP_VU1REC=file [MVP_VU1REC_FRAME=n] [MVP_VU1REC_RUNS=k] [MVP_VU1REC_EVERY=m]: from game frame n on, save the
// complete input of every m-th microprogram run (registers, micro memory, data memory, TOP/ITOP), k runs in all.
// `mvp2005 --vu1-bench file [iterations]` plays them back without drawing: time per instruction pair, and a
// checksum of everything the programs left behind, which must not change when the VU1 code is made faster.
void vu1_code_uploaded(u32 addr, const u8* data, u32 bytes); void vu1_execute(u32 start_pc);
static const char* s_rec_path = std::getenv("MVP_VU1REC");
struct VuRecHead { u32 magic, pc, top, itop; };
void vif1_set_tops(u32 top, u32 itop);
static void vu1_rec_run() {
    static const long long from = std::getenv("MVP_VU1REC_FRAME") ? std::atoll(std::getenv("MVP_VU1REC_FRAME")) : 0;
    static const long long runs = std::getenv("MVP_VU1REC_RUNS") ? std::atoll(std::getenv("MVP_VU1REC_RUNS")) : 3000;
    static const long long every = std::getenv("MVP_VU1REC_EVERY") ? std::max(1LL, std::atoll(std::getenv("MVP_VU1REC_EVERY"))) : 1;
    static long long seen = 0, saved = 0; static FILE* f = nullptr;
    extern u64 g_game_frames;
    if ((long long)g_game_frames < from || saved >= runs) { if (f && saved >= runs) { fclose(f); f = nullptr; } return; }
    if (seen++ % every) return;
    if (!f) f = fopen(s_rec_path, "wb");
    if (!f) return;
    flags_flush();                                                     // (a program always ends with the queue empty)
    VuRecHead h{0x31555652u, V.pc, vif1_top(), vif1_itop()};
    fwrite(&h, sizeof h, 1, f); fwrite(&V, sizeof V, 1, f); fwrite(g_vu1_code, 1, 0x4000, f); fwrite(g_vu1_mem, 1, 0x4000, f);
    if (++saved >= runs) { fclose(f); f = nullptr; fprintf(stderr, "[vu1rec] %lld runs saved to %s\n", saved, s_rec_path); }
}
int vu1_bench(const char* path, int iters) {
    FILE* f = fopen(path, "rb"); if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    struct Rec { VuRecHead h; VU v; std::vector<u8> code, mem; };
    std::vector<Rec> recs;
    for (;;) {
        Rec r; r.code.resize(0x4000); r.mem.resize(0x4000);
        if (fread(&r.h, sizeof r.h, 1, f) != 1 || r.h.magic != 0x31555652u) break;
        if (fread(&r.v, sizeof r.v, 1, f) != 1 || fread(r.code.data(), 1, 0x4000, f) != 0x4000 || fread(r.mem.data(), 1, 0x4000, f) != 0x4000) break;
        recs.push_back(std::move(r));
    }
    fclose(f);
    if (recs.empty()) { fprintf(stderr, "no runs in %s\n", path); return 1; }
    s_xgkick_mute = true;
    u64 sum = 1469598103934665603ull; double best = 1e30, total_ns = 0; u64 pairs = 0;
    for (int it = 0; it < iters; it++) {
        double ns = 0; u64 p0 = s_vu_instrs; u64 h = 1469598103934665603ull;
        for (const Rec& r : recs) {
            if (std::memcmp(g_vu1_code, r.code.data(), 0x4000)) { std::memcpy(g_vu1_code, r.code.data(), 0x4000); vu1_code_uploaded(0, r.code.data(), 0x4000); }
            std::memcpy(g_vu1_mem, r.mem.data(), 0x4000);
            V = r.v; s_fq_head = s_fq_tail = 0;
            vif1_set_tops(r.h.top, r.h.itop);
            const auto a = std::chrono::steady_clock::now();
            vu1_execute(r.h.pc);
            ns += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - a).count();
            if (it == 0) {                                             // what the run left behind
                const u8* q = (const u8*)&V.vf; for (size_t i = 0; i < sizeof V.vf; i++) h = (h ^ q[i]) * 1099511628211ull;
                q = (const u8*)V.vi; for (size_t i = 0; i < sizeof V.vi; i++) h = (h ^ q[i]) * 1099511628211ull;
                q = (const u8*)V.acc; for (size_t i = 0; i < sizeof V.acc; i++) h = (h ^ q[i]) * 1099511628211ull;
                u32 misc[6]; std::memcpy(&misc[0], &V.q, 4); std::memcpy(&misc[1], &V.p, 4); misc[2] = V.clip; misc[3] = V.mac; misc[4] = V.status; misc[5] = V.pc;
                q = (const u8*)misc; for (size_t i = 0; i < sizeof misc; i++) h = (h ^ q[i]) * 1099511628211ull;
                for (size_t i = 0; i < 0x4000; i += 8) { u64 w; std::memcpy(&w, g_vu1_mem + i, 8); h = (h ^ w) * 1099511628211ull; }
            }
        }
        if (it == 0) { sum = h; pairs = s_vu_instrs - p0; }
        best = std::min(best, ns); total_ns += ns;
    }
    fprintf(stderr, "%zu runs, %llu pairs: best %.2f ns/pair (mean %.2f), %.1f ms per pass; checksum %016llx; blocks: %llu compiled runs, %llu pairs interpreted\n",
            recs.size(), (unsigned long long)pairs, best / pairs, total_ns / iters / pairs, best / 1e6, (unsigned long long)sum,
            (unsigned long long)s_blk_runs, (unsigned long long)s_interp_pairs);
    return 0;
}

u32 g_vu1_cur_start = 0;
float g_vu1_dbg_rows[6][4];
static const bool s_matdbg = std::getenv("MVP_MATDBG") != nullptr;
static const long long s_snap_frame = std::getenv("MVP_VU1SNAP") ? std::atoll(std::getenv("MVP_VU1SNAP")) : -1;
void vu1_execute(u32 start_pc) {
    if (start_pc != 0xffffffffu) V.pc = start_pc & 0x3ff8;
    g_vu1_cur_start = V.pc;
    { extern u64 g_game_frames;
      s_clipdbg_on = s_clipdbg_frame >= 0 && (long long)g_game_frames >= s_clipdbg_frame && V.pc == 0x10 && s_clipdbg_n < 2;
      s_clipdbg.clear(); s_clipdbg_neg = false; }
    struct ClipDbgEnd { ~ClipDbgEnd() { if (s_clipdbg_on && s_clipdbg_neg) { s_clipdbg_n++; fprintf(stderr, "[clipdbg] program 0010 run with w<0 vertices:\n%s", s_clipdbg.c_str()); } s_clipdbg_on = false; } } clipdbg_end;
    if (s_matdbg) {                                   // debug: remember the matrix block this program will use
        u32 top = vif1_top();
        for (int r = 0; r < 6; r++) std::memcpy(g_vu1_dbg_rows[r], g_vu1_mem + (((top + r) & 0x3ff) << 4), 16);
    }
    if (s_snap_frame >= 0) {                  // MVP_VU1SNAP=frame: save micro memory + data memory at each new start pc
        extern u64 g_game_frames; static u32 seen[0x800];
        if ((long long)g_game_frames >= s_snap_frame && seen[V.pc >> 3] < 1) {
            seen[V.pc >> 3]++;
            char path[128]; snprintf(path, sizeof path, "/tmp/vu1snap_%04x.bin", V.pc);
            if (FILE* f = fopen(path, "wb")) { fwrite(g_vu1_code, 1, 0x4000, f); fwrite(g_vu1_mem, 1, 0x4000, f); fclose(f); }
        }
    }
    if (s_rec_path) vu1_rec_run();
    V.vi[0] = 0;
    V.vf[0][0] = V.vf[0][1] = V.vf[0][2] = 0; V.vf[0][3] = 1.0f;
    Run r;
    const bool use_blocks = !s_clipdbg_on;
    for (int steps = 0; steps < 4000000 && !r.done; steps++) {
        u32 pc = V.pc, idx = pc >> 3;
        if (use_blocks && !r.have_branch && !r.ending) {
            u16 ref = s_blk_ref[idx];
            if (__builtin_expect(!ref, 0)) ref = block_lookup(idx);
            if (ref != 0xffff) {
                const VuBlockDef& def = kVuBlocks[ref - 1];
                s_blk_runs++; s_blk_pairs += def.npairs;
                if (s_blocks_check) check_block(idx, pc, r); else def.fn(pc, r);
                continue;
            }
        }
        if (s_blocks_dump && s_blk_ref[idx] == 0xffff && s_blk_len[idx] && !r.have_branch && !r.ending) {
            // dumping: interpret the whole block, so the recorded entry points are the ones compiled code sees
            auto it = s_seen.find(s_blk_hash[idx]); if (it != s_seen.end()) it->second.count += s_blk_len[idx];
            for (u32 k = 0, n = s_blk_len[idx]; k < n && !r.done; k++) {
                u32 p = V.pc; const Dec& d = s_dec[p >> 3]; if (!d.valid) decode(p >> 3);
                step(d, p, r);
            }
            continue;
        }
        const Dec& d = s_dec[idx];
        if (!d.valid) decode(idx);
        s_interp_pairs++;
        step(d, pc, r);
    }
    if (!r.done) rlog("vu1", "microprogram did not end (pc=%04x)", V.pc);
}

// ---------------------------------------------------------------- block code generator (--vu1-gen)
#define UPFNS(X) \
    X(up_generic) X(up_nop) X(up_clip) X(up_abs) X((up_itof<0>)) X((up_itof<1>)) X((up_itof<2>)) X((up_itof<3>)) \
    X((up_ftoi<0>)) X((up_ftoi<1>)) X((up_ftoi<2>)) X((up_ftoi<3>)) \
    X((up_bin<0,0,false>)) X((up_bin<0,1,false>)) X((up_bin<0,2,false>)) X((up_bin<0,3,false>)) X((up_bin<0,0,true>)) X((up_bin<0,1,true>)) X((up_bin<0,2,true>)) X((up_bin<0,3,true>)) \
    X((up_bin<1,0,false>)) X((up_bin<1,1,false>)) X((up_bin<1,2,false>)) X((up_bin<1,3,false>)) X((up_bin<1,0,true>)) X((up_bin<1,1,true>)) X((up_bin<1,2,true>)) X((up_bin<1,3,true>)) \
    X((up_bin<2,0,false>)) X((up_bin<2,1,false>)) X((up_bin<2,2,false>)) X((up_bin<2,3,false>)) X((up_bin<2,0,true>)) X((up_bin<2,1,true>)) X((up_bin<2,2,true>)) X((up_bin<2,3,true>)) \
    X((up_bin<3,0,false>)) X((up_bin<3,1,false>)) X((up_bin<3,2,false>)) X((up_bin<3,3,false>)) X((up_bin<3,0,true>)) X((up_bin<3,1,true>)) X((up_bin<3,2,true>)) X((up_bin<3,3,true>)) \
    X((up_bin<4,0,false>)) X((up_bin<4,1,false>)) X((up_bin<4,2,false>)) X((up_bin<4,3,false>)) X((up_bin<4,0,true>)) X((up_bin<4,1,true>)) X((up_bin<4,2,true>)) X((up_bin<4,3,true>)) \
    X((up_bin<5,0,false>)) X((up_bin<5,1,false>)) X((up_bin<5,2,false>)) X((up_bin<5,3,false>)) X((up_bin<5,0,true>)) X((up_bin<5,1,true>)) X((up_bin<5,2,true>)) X((up_bin<5,3,true>)) \
    X((up_bin<6,0,false>)) X((up_bin<6,1,false>)) X((up_bin<6,2,false>)) X((up_bin<6,3,false>)) X((up_bin<6,0,true>)) X((up_bin<6,1,true>)) X((up_bin<6,2,true>)) X((up_bin<6,3,true>))
#define LOFNS(X) X(lo_generic) X(lo_lq) X(lo_sq) X(lo_lqi) X(lo_sqi) X(lo_iaddiu) X(lo_isubiu) X(lo_iaddi) X(lo_iadd) X(lo_isub) \
    X(lo_ibne) X(lo_ibeq) X(lo_b) X(lo_move) X(lo_div) X(lo_waitq) X(lo_ilw) X(lo_isw) X(lo_fcand) X(lo_fcor) X(lo_fceq) \
    X(lo_fcset) X(lo_fcget) X(lo_mtir) X(lo_mfir) X(lo_iand) X(lo_ior) X(lo_mr32) X(lo_ibgtz) X(lo_ibltz) X(lo_iblez) X(lo_ibgez)
static std::string fn_name(UpFn f) {
#define N(fn) if (f == (UpFn)fn) { std::string s = #fn; if (s[0] == '(') s = s.substr(1, s.size() - 2); return s; }
    UPFNS(N)
#undef N
    return "";
}
static std::string fn_name(LoFn f) {
#define N(fn) if (f == (LoFn)fn) return #fn;
    LOFNS(N)
#undef N
    return "";
}
void vu1_blocks_save() {
    if (!s_blocks_dump || s_seen.empty()) return;
    // merge with an existing dump: lines "hash npairs w0 w1 ..."
    std::unordered_map<u64, SeenBlock> all;
    if (FILE* f = fopen(s_blocks_dump, "r")) {
        unsigned long long h; unsigned n;
        unsigned long long cnt;
        while (fscanf(f, "%llx %u %llu", &h, &n, &cnt) == 3) {
            SeenBlock b; b.npairs = n; b.count = cnt;
            for (unsigned i = 0; i < n * 2; i++) { unsigned w; if (fscanf(f, "%x", &w) != 1) break; b.words.push_back(w); }
            all[h] = b;
        }
        fclose(f);
    }
    for (auto& kv : s_seen) { u64 c = all.count(kv.first) ? all[kv.first].count : 0; all[kv.first] = kv.second; all[kv.first].count += c; }
    if (FILE* f = fopen(s_blocks_dump, "w")) {
        for (auto& kv : all) {
            fprintf(f, "%016llx %u %llu", (unsigned long long)kv.first, kv.second.npairs, (unsigned long long)kv.second.count);
            for (u32 w : kv.second.words) fprintf(f, " %08x", w);
            fprintf(f, "\n");
        }
        fclose(f);
        fprintf(stderr, "VU1: %zu blocks in %s\n", all.size(), s_blocks_dump);
    }
}
// Writes the blocks to <out_dir>/vu1_blocks_<k>.cpp (split so they compile in parallel) and the lookup
// table to <out_dir>/vu1_blocks_table.inc (included by this file).
int vu1_gen(const char* in, const char* out_dir) {
    FILE* f = fopen(in, "r"); if (!f) { fprintf(stderr, "cannot open %s\n", in); return 1; }
    struct Blk { unsigned long long h; unsigned n; std::vector<u32> w; };
    std::vector<Blk> blks;
    { unsigned long long h, cnt; unsigned n;
      while (fscanf(f, "%llx %u %llu", &h, &n, &cnt) == 3) {
          Blk b{h, n, std::vector<u32>(n * 2)};
          for (unsigned i = 0; i < n * 2; i++) if (fscanf(f, "%x", &b.w[i]) != 1) { fclose(f); return 1; }
          blks.push_back(std::move(b));
      } }
    fclose(f);
    const int kParts = 8;
    const char* hdr = "// Generated by `mvp2005 --vu1-gen` from VU1 blocks seen while playing. Do not edit.\n";
    std::string tpath = std::string(out_dir) + "/vu1_blocks_table.inc";
    FILE* t = fopen(tpath.c_str(), "w"); if (!t) return 1;
    fprintf(t, "%snamespace vu1i {\n", hdr);
    std::string table;
    static u8 saved[0x4000]; std::memcpy(saved, g_vu1_code, 0x4000);
    // balance the parts by pair count
    std::vector<unsigned> load(kParts, 0);
    std::vector<FILE*> parts(kParts, nullptr);
    for (int k = 0; k < kParts; k++) {
        std::string pp = std::string(out_dir) + "/vu1_blocks_" + std::to_string(k) + ".cpp";
        parts[k] = fopen(pp.c_str(), "w"); if (!parts[k]) return 1;
        fprintf(parts[k], "%s#include \"vu1_core.h\"\nnamespace vu1i {\n", hdr);
    }
    for (const Blk& b : blks) {
        int k = (int)(std::min_element(load.begin(), load.end()) - load.begin()); load[k] += b.n;
        FILE* o = parts[k];
        fprintf(t, "static const u32 vbw_%016llx[] = {", b.h);
        for (unsigned i = 0; i < b.n * 2; i++) fprintf(t, "%s0x%08x", i ? "," : "", b.w[i]);
        fprintf(t, "};\nvoid vb_%016llx(u32 start, Run& r);\n", b.h);
        // A block is only entered with no branch pending and no E bit seen, so it works on a Run of its own
        // that starts out empty: the compiler then knows there is nothing to check before the block's own branch.
        fprintf(o, "void vb_%016llx(u32 start, Run& r) {\n    Run lr;\n", b.h);
        for (unsigned k2 = 0; k2 < b.n; k2++) {
            // decode the pairs with the real decoder (placed at 0, so fields that depend on the pc are relative)
            std::memcpy(g_vu1_code + k2 * 8, &b.w[k2 * 2], 4); std::memcpy(g_vu1_code + k2 * 8 + 4, &b.w[k2 * 2 + 1], 4);
            decode(k2);
            const Dec& d = s_dec[k2];
            std::string up = fn_name(d.up), lo = fn_name(d.lo);
            if (up.empty() || lo.empty()) { fprintf(stderr, "vu1-gen: unknown handler\n"); return 1; }
            const bool rel = !d.lower_nop && is_branch_lower(d.lower);       // branch target depends on the pc
            fprintf(o, "    { static constexpr Dec d = {%s, %s, 0x%08xu, 0x%08xu, %u,%u,%u,%u,%u, %u,%u,%u,%u,%u,%u,%u,%u, %d, 0u, %d,%d,%d,%d,%d,%d};\n",
                    up.c_str(), lo.c_str(), d.upper, d.lower, d.dest, d.ft, d.fs, d.fd, d.bc, d.ldest, d.lft, d.lfs, d.lit, d.lis, d.lid, d.lfsf, d.lftf,
                    d.limm, 1, (int)d.ibit, (int)d.ebit, (int)d.lower_nop, (int)d.reads_flags, (int)d.lower_wait);
            (void)rel;
            fprintf(o, "      step_t<false>(d, (start + %u) & 0x3ff8, lr); }\n", k2 * 8);
        }
        fprintf(o, "    s_vu_instrs += %u; r = lr;\n}\n", b.n);
        char line[160]; snprintf(line, sizeof line, "    {0x%016llxull, %u, vbw_%016llx, vb_%016llx},\n", b.h, b.n, b.h, b.h);
        table += line;
    }
    std::memcpy(g_vu1_code, saved, 0x4000);
    for (FILE* o : parts) { fprintf(o, "} // namespace vu1i\n"); fclose(o); }
    fprintf(t, "static const VuBlockDef kVuBlocks[] = {\n%s    {0, 0, nullptr, nullptr}};\n} // namespace vu1i\n", table.c_str());
    fclose(t);
    fprintf(stderr, "vu1-gen: %zu blocks -> %s (%d parts)\n", blks.size(), out_dir, kParts);
    return 0;
}
void vu1_block_stats() {
    fprintf(stderr, "VU1 blocks: %llu compiled runs (%llu pairs), %llu uncompiled block entries%s\n", (unsigned long long)s_blk_runs,
            (unsigned long long)s_blk_pairs, (unsigned long long)s_blk_misses, s_blocks_check ? "" : "");
    fprintf(stderr, "VU1 code: %llu uploads changed code (%llu pairs), %llu block look-ups, %llu pairs interpreted\n", (unsigned long long)s_mpg_changed,
            (unsigned long long)s_redecoded, (unsigned long long)s_blk_lookups, (unsigned long long)s_interp_pairs);
    if (s_blocks_check) fprintf(stderr, "VU1 block check: %llu mismatches\n", (unsigned long long)s_check_fail);
    vu1_blocks_save();
}

u64 vu1_instr_count() { return s_vu_instrs; }

// ---------------------------------------------------------------- microcode upload tracking
// Each MPG payload is identified by a 64-bit hash. MVP_VU1DUMP=dir writes every distinct payload
// once as <dir>/mpg_<hash>_<addr>.bin (input for tools/recomp/vu1_recomp.py).
static u64 s_mpg_count = 0;
static const char* s_vu1dump = std::getenv("MVP_VU1DUMP");
static std::set<u64> s_dumped;
u64 vu1_hash(const u8* d, u32 n) {
    u64 h = 1469598103934665603ull;
    for (u32 i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ull; }
    return h;
}
static u64 s_pair_words[0x800]; static u8 s_pair_rf[0x800];
void vu1_code_uploaded(u32 addr, const u8* data, u32 bytes) {
    s_mpg_count++;
    u32 first = ~0u, last = 0;
    // micro memory was already updated. For each pair that changed: keep the count of flag-reading instructions
    // exact before anything runs (flags are only computed while some program reads them) and leave the decoding
    // to the interpreter, if it ever gets there (pairs inside compiled blocks are never decoded).
    for (u32 k = 0, n = std::min<u32>((bytes + 7) / 8, 0x800); k < n; k++) {
        u32 idx = ((addr >> 3) + k) & 0x7ff;
        u64 w; std::memcpy(&w, g_vu1_code + idx * 8, 8);
        if (w == s_pair_words[idx]) continue;
        u32 lw, uw; std::memcpy(&lw, g_vu1_code + idx * 8, 4); std::memcpy(&uw, g_vu1_code + idx * 8 + 4, 4);
        u8 rf = !(uw & 0x80000000u) && lower_reads_flags(lw);
        s_flag_readers += (int)rf - (int)s_pair_rf[idx]; s_pair_rf[idx] = rf;
        s_pair_words[idx] = w;
        s_dec[idx].valid = false; s_redecoded++;
        u32 pos = ((addr >> 3) & 0x7ff) + k;
        if (first == ~0u) first = pos;
        last = pos;
    }
    if (first != ~0u) { s_mpg_changed++; s_need_flags = s_flag_readers > 0 || s_force_flags; blocks_invalidate(first, last); }
    if (!s_vu1dump) return;
    u64 h = vu1_hash(data, bytes) ^ addr;
    if (!s_dumped.insert(h).second) return;
    char path[512]; snprintf(path, sizeof path, "%s/mpg_%016llx_%04x.bin", s_vu1dump, (unsigned long long)h, addr);
    if (FILE* f = fopen(path, "wb")) { fwrite(data, 1, bytes, f); fclose(f); }
}
u64 vu1_mpg_count() { return s_mpg_count; }
