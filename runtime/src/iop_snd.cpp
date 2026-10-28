// EA SND library IOP driver (SNDDRV.IRX) — native replacement.
//
// Bank/sample upload protocol (reverse engineered from the EE side, 0x514270 / 0x514660 / 0x5122c0):
//   EE -> IOP  user SIF cmd 0, 48-byte packet:
//        +16 type (1 = upload chunk, 4 = other), +20 request id, +24 IOP buffer (data was SIF-DMA'd there),
//        +28 SPU2 destination address, +32 (u16) size, +34 (u16) extra
//   IOP -> EE  user SIF cmd 1: +16 type (2 = done), header opt = queue index << 24
// The EE keeps per-queue FIFOs at 0x8F1510 (12 bytes each: head, tail, count); the finished request is
// the head of its queue, so we look up which queue holds the request id.
#include "ee_ops.h"
#include "ee_runtime.h"
#include "iop.h"
#include <cstring>
#include <vector>

void iop_send_user_cmd(u32 cid, const u32* payload, u32 nwords, u32 opt);

std::vector<u8> g_spu_ram(2 * 1024 * 1024);      // SPU2 sound RAM (for the future audio backend)

static const u32 EE_SND_QUEUES = 0x008f1510;

void iop_user_cmd(const u8* pkt, u32 size) {
    u32 w[12] = {};
    std::memcpy(w, pkt, size < 48 ? size : 48);
    u32 type = w[4], id = w[5], iop_buf = w[6], spu_addr = w[7], len = w[8] & 0xffff;
    if (type == 1 && len) {
        u32 n = std::min<u32>(len, (u32)g_spu_ram.size() - (spu_addr % g_spu_ram.size()));
        n = iop_clamp(iop_buf, n, "snd-upload");
        std::memcpy(&g_spu_ram[spu_addr % g_spu_ram.size()], g_iop_ram + (iop_buf & (IOP_RAM_SIZE - 1)), n);
    }
    int queue = -1;
    for (int q = 0; q < 16; q++) {
        u32 head = ee_rd<u32>(EE_SND_QUEUES + q * 12);
        if (head && ee_rd<u32>(head + 20) == id) { queue = q; break; }
    }
    rlog("snd-cmd", "type=%u id=%u iopbuf=%08x spu=%08x len=%x -> queue %d", type, id, iop_buf, spu_addr, len, queue);
    if (queue < 0) return;
    u32 payload[1] = {2};                        // "transfer complete"
    iop_send_user_cmd(1, payload, 1, (u32)queue << 24);
}

// ---------------------------------------------------------------- driver tick
// Driver ticks (user SIF cmd 1, type 0) and the command blocks they request are handled in snd_mix.cpp.
