// IOP (I/O processor) high-level emulation.
//
// On a PS2 the EE talks to the IOP through SIF: it DMAs command packets (RPC bind/call) into IOP
// memory, the IOP modules service them and DMA reply packets back, raising the EE's SIF0 DMA
// interrupt. We keep the EE-side SIF library exactly as recompiled and emulate only the IOP:
// requests are decoded here, serviced by native "servers", and replies are written into the EE's
// SIF0 receive buffer followed by a SIF0 (DMAC channel 5) interrupt.
#include "ee_ops.h"
#include "ee_runtime.h"
#include "iop.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

void kernel_raise_dmac(int ch);

alignas(16) u8 g_iop_ram[IOP_RAM_SIZE + 0x10000];
u32 iop_clamp(u32 addr, u32 n, const char* who) {
    u32 off = addr & (IOP_RAM_SIZE - 1);
    // Buffers placed at the very top of IOP RAM may run a little past it; the 64 KB guard area keeps
    // those contiguous (the HLE side reads them linearly). Anything larger is clamped.
    if (off + (u64)n <= IOP_RAM_SIZE + 0x10000) return n;
    static int warned = 0;
    if (warned++ < 10) rlog("iop", "%s: access %08x+%x runs past IOP RAM, clamped", who, addr, n);
    return IOP_RAM_SIZE + 0x10000 - off;
}

// ---------------------------------------------------------------- SIF command ids
enum : u32 {
    CMD_CHANGE_SADDR = 0x80000000, CMD_SET_SREG = 0x80000001, CMD_INIT_CMD = 0x80000002,
    CMD_RESET = 0x80000003, CMD_RPC_END = 0x80000008, CMD_RPC_BIND = 0x80000009,
    CMD_RPC_CALL = 0x8000000a, CMD_RPC_RDATA = 0x8000000c,
};

static u32 s_ee_recv_buf = 0;                 // EE SIF0 receive buffer (from CHANGE_SADDR)
struct Reply { std::vector<u8> pkt; u64 ready_ns; };
static std::deque<Reply> s_replies;
static const u64 IOP_LATENCY_NS = 60000;       // replies arrive ~60 us later, like real SIF round trips
static u32 s_sreg[32];

// ---------------------------------------------------------------- server registry
static std::map<u32, IopServer*>& servers() { static std::map<u32, IopServer*> m; return m; }
IopServer::IopServer(u32 sid_, const char* name_) : sid(sid_), name(name_) { servers()[sid] = this; }

struct BoundServer { u32 sid; u32 buff; IopServer* impl; };
static std::vector<BoundServer> s_bound;       // index+1 = fake IOP "serve" pointer
static const u32 IOP_HEAP_BASE = 0x00040000;
static u32 s_iop_alloc = IOP_HEAP_BASE;        // bump allocator for server buffers / iopheap in IOP RAM

// Blocks the game gave back (sceSifFreeIopHeap). Each match loads its sound banks into ~225 KB of IOP memory and
// frees them afterwards; without reuse the 1.7 MB were gone after the sixth match of a session ("IOP RAM exhausted").
static std::map<u32, u32> s_iop_used, s_iop_free;            // address -> size (64-byte units, so blocks can be merged)

u32 iop_alloc(u32 size) {
    size = (size + 63) & ~63u; if (!size) size = 64;
    for (auto it = s_iop_free.begin(); it != s_iop_free.end(); ++it) {          // first fit
        if (it->second < size) continue;
        const u32 a = it->first, rest = it->second - size;
        s_iop_free.erase(it);
        if (rest) s_iop_free[a + size] = rest;
        s_iop_used[a] = size;
        return a;
    }
    u32 a = (s_iop_alloc + 63) & ~63u;
    s_iop_alloc = a + size;
    if (s_iop_alloc > 0x1f0000) rfatal("IOP RAM exhausted");       // top 64 KB reserved (SND command blocks)
    s_iop_used[a] = size;
    return a;
}
bool iop_free(u32 a) {
    auto u = s_iop_used.find(a);
    if (u == s_iop_used.end()) return false;
    u32 size = u->second; s_iop_used.erase(u);
    auto nx = s_iop_free.lower_bound(a);
    if (nx != s_iop_free.end() && a + size == nx->first) { size += nx->second; nx = s_iop_free.erase(nx); }
    if (nx != s_iop_free.begin()) { auto pv = std::prev(nx); if (pv->first + pv->second == a) { a = pv->first; size += pv->second; s_iop_free.erase(pv); } }
    if (a + size == s_iop_alloc) s_iop_alloc = a;            // the top block: back to the untouched area
    else s_iop_free[a] = size;
    return true;
}

// ---------------------------------------------------------------- replies
static void queue_rpc_end(const u8* req, u32 cid, u32 server, u32 buff, u32 cbuff) {
    std::vector<u8> p(64, 0);
    u32 w[16] = {};
    w[0] = 64;                                  // psize = 64, dsize = 0
    w[2] = CMD_RPC_END;
    std::memcpy(&w[4], req + 16, 16);           // rec_id, pkt_addr, rpc_id, client echoed back
    w[8] = cid; w[9] = server; w[10] = buff; w[11] = cbuff;
    std::memcpy(p.data(), w, 64);
    s_replies.push_back({std::move(p), host_time_ns() + IOP_LATENCY_NS});
}

// Called from the kernel's event loop: hand one reply to the EE per interrupt.
bool iop_deliver() {
    if (s_replies.empty() || !s_ee_recv_buf) return false;
    u8* dst = ee_ptr(s_ee_recv_buf);
    if (!dst) return false;
    if (dst[0] != 0) return false;              // EE hasn't consumed the previous packet yet
    if (host_time_ns() < s_replies.front().ready_ns) return false;
    auto& p = s_replies.front().pkt;
    std::memcpy(dst + 1, p.data() + 1, p.size() - 1);
    dst[0] = p[0];                              // psize written last, like a completed DMA
    s_replies.pop_front();
    kernel_raise_dmac(5);
    return true;
}

void iop_dump() {
    const u8* dst = s_ee_recv_buf ? ee_ptr(s_ee_recv_buf) : nullptr;
    fprintf(stderr, "SIF: %zu replies waiting, EE receive buffer %s\n", s_replies.size(), !dst ? "not set" : dst[0] ? "still holds a packet" : "free");
    for (auto& r : s_replies) { u32 w[12] = {}; std::memcpy(w, r.pkt.data(), std::min<size_t>(r.pkt.size(), 48)); fprintf(stderr, "  reply cmd %08x for %08x\n", w[2], w[8]); }
}

// ---------------------------------------------------------------- request handling
static void handle_packet(const u8* pkt, u32 size) {
    u32 w[16] = {};
    std::memcpy(w, pkt, size < 64 ? size : 64);
    u32 cid = w[2];
    switch (cid) {
    case CMD_CHANGE_SADDR:
        s_ee_recv_buf = w[4];
        rlog("iop", "EE SIF0 receive buffer = %08x", s_ee_recv_buf);
        break;
    case CMD_SET_SREG:
        s_sreg[w[4] & 31] = w[5];
        if (w[4] == 1) {                                 // multi-thread RPC start-up (the online lobby): the IOP side
            std::vector<u8> p(24, 0);                    // answers by setting the same register on the EE
            u32 r[6] = {24, 0, CMD_SET_SREG, 0, 1, 1};
            std::memcpy(p.data(), r, 24);
            s_replies.push_back({std::move(p), host_time_ns() + IOP_LATENCY_NS});
            rlog("iop", "multi-thread RPC start-up acknowledged");
        }
        break;
    case CMD_INIT_CMD:
        if (w[3] == 0) { s_ee_recv_buf = w[4]; rlog("iop", "INIT_CMD: EE SIF0 receive buffer = %08x", s_ee_recv_buf); }
        else rlog("iop", "INIT_CMD opt=%08x", w[3]);
        break;
    case CMD_RESET:
        rlog("iop", "IOP reset requested (%s)", (const char*)(pkt + 24));
        s_iop_alloc = IOP_HEAP_BASE;                    // a rebooted IOP starts with an empty heap
        break;
    case CMD_RPC_BIND: {
        u32 sid = w[8];
        auto it = servers().find(sid);
        IopServer* impl = it != servers().end() ? it->second : nullptr;
        u32 buff = 0;
        for (auto& b : s_bound) if (b.sid == sid) buff = b.buff;           // rebinding reuses the buffer
        if (!buff) buff = iop_alloc(impl ? impl->buffer_size : 0x8000);
        s_bound.push_back({sid, buff, impl});
        u32 serve = 0x00080000 + (u32)s_bound.size() * 16;
        if (impl) rlog("iop-bind", "bind %08x (%s)", sid, impl->name);
        else rlog("iop-bind", "bind %08x — NO NATIVE SERVER (calls will return zeros)", sid);
        queue_rpc_end(pkt, CMD_RPC_BIND, serve, buff, buff);
        break;
    }
    case CMD_RPC_CALL: {
        u32 fno = w[8], ssize = w[9], recv = w[10], rsize = w[11], serve = w[13];
        int idx = (int)((serve - 0x00080000) / 16) - 1;
        if (idx < 0 || idx >= (int)s_bound.size()) { rlog("iop", "call on unknown serve %08x", serve); queue_rpc_end(pkt, CMD_RPC_CALL, 0, 0, 0); break; }
        BoundServer& b = s_bound[idx];
        u8* send = g_iop_ram + (b.buff & (IOP_RAM_SIZE - 1));
        u8* out = recv && rsize ? ee_ptr(recv) : nullptr;
        if (b.sid == 0x00534e44) { extern void snd_driver_started(); snd_driver_started(); }   // EA SND driver init
        if (b.impl) {
            b.impl->call(fno, send, ssize, out, rsize, recv);
        } else {
            rlog("iop-call", "unhandled server %08x fno=%u ssize=%u rsize=%u", b.sid, fno, ssize, rsize);
            if (std::getenv("MVP_RPCDUMP") && b.sid == 0x00534e44) {
                static int n = 0;
                if (n++ < 400) { fprintf(stderr, "[snd-rpc] fno=%u ssize=%u rsize=%u:", fno, ssize, rsize);
                    for (u32 i = 0; i < ssize && i < 96; i += 4) { u32 v; std::memcpy(&v, send + i, 4); fprintf(stderr, " %08x", v); }
                    fprintf(stderr, "\n"); }
            }
            if (out) std::memset(out, 0, rsize);
        }
        queue_rpc_end(pkt, CMD_RPC_CALL, 0, 0, 0);
        break;
    }
    case CMD_RPC_RDATA: {                       // copy IOP memory -> EE
        u32 src = w[8], dst = w[9], n = w[10];
        u8* d = ee_ptr(dst);
        if (d) std::memcpy(d, iop_mem(src), iop_clamp(src, n, "rdata"));
        queue_rpc_end(pkt, CMD_RPC_RDATA, 0, 0, 0);
        break;
    }
    default:
        if (!(cid & 0x80000000u)) {
            extern void iop_user_cmd(const u8* pkt, u32 size);
            iop_user_cmd(pkt, size);
            break;
        }
        rlog("iop", "unhandled SIF command %08x", cid);
    }
}

void iop_sif_dma(u32 src, u32 dst, u32 size, u32 attr) {
    (void)attr;
    u8* s = ee_ptr(src & 0x1fffffff);
    if (!s) { rlog("iop", "SIF DMA from non-RAM %08x", src); return; }
    if (dst == 0 && size >= 16) { handle_packet(s, size); return; }   // command packets go to IOP address 0
    if (std::getenv("MVP_RPCDUMP")) {
        static std::map<u32, u32> seen; u32& c = seen[dst];
        if (c++ < 3) { fprintf(stderr, "[sif-dma] %08x -> iop %08x size %x:", src, dst, size);
            for (u32 i = 0; i < size && i < 64; i += 4) { u32 v; std::memcpy(&v, s + i, 4); fprintf(stderr, " %08x", v); }
            fprintf(stderr, "\n"); }
    }
    std::memcpy(iop_mem(dst), s, iop_clamp(dst, size, "sif-dma"));
}

// Send a user-level SIF command (cid < 0x80000000) to the EE, e.g. driver completion notices.
void iop_send_user_cmd(u32 cid, const u32* payload, u32 nwords, u32 opt) {
    std::vector<u8> p(16 + nwords * 4, 0);
    u32 hdr[4] = {(u32)p.size(), 0, cid, opt};
    std::memcpy(p.data(), hdr, 16);
    std::memcpy(p.data() + 16, payload, nwords * 4);
    s_replies.push_back({std::move(p), host_time_ns() + IOP_LATENCY_NS});
}
