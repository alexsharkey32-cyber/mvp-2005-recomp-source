// Native implementations of the IOP RPC servers MVP Baseball 2005 talks to.
#include "ee_ops.h"
#include "ee_runtime.h"
#include "iop.h"
#include <cstring>
#include <string>

static inline u32 rd(const u8* p, int o) { u32 v; std::memcpy(&v, p + o, 4); return v; }
static inline void wr(u8* p, int o, u32 v) { if (p) std::memcpy(p + o, &v, 4); }

// ---------------------------------------------------------------- LOADFILE: IRX module loading
// The real IRX drivers (sound, network, pads...) are replaced by native servers, so "loading" a
// module only records its name and reports success.
static struct LoadFile : IopServer {
    LoadFile() : IopServer(0x80000006, "loadfile") {}
    int next_id = 1;
    void call(u32 fno, u8* send, u32 ssize, u8* out, u32 rsize, u32) override {
        if (fno == 0 || fno == 1) {                         // LoadModule / LoadElf(path, args)
            const char* path = (const char*)send + 8;
            rlog("loadfile", "load module '%.252s'", path);
            wr(out, 0, next_id++); wr(out, 4, 0);
        } else {
            rlog("loadfile", "fno %u (ssize %u)", fno, ssize);
            if (out) std::memset(out, 0, rsize);
            wr(out, 0, next_id++);
        }
    }
} s_loadfile;

// ---------------------------------------------------------------- IOP heap
static struct IopHeap : IopServer {
    IopHeap() : IopServer(0x80000003, "iopheap") {}
    void call(u32 fno, u8* send, u32, u8* out, u32, u32) override {
        switch (fno) {
        case 1: { u32 size = rd(send, 0); u32 a = iop_alloc(size); rlog("iopheap", "alloc %x -> %08x", size, a); wr(out, 0, a); break; }
        case 2: { u32 a = rd(send, 0); bool ok = iop_free(a); rlog("iopheap", "free %08x%s", a, ok ? "" : " (not allocated)"); wr(out, 0, 0); break; }
        case 3: {                                            // load file into IOP memory
            const char* path = (const char*)send + 4; u32 addr = rd(send, 0);
            u32 lba, size;
            if (iso_find(path, &lba, &size) == 0) {
                std::string tmp((size + 2047) & ~2047u, '\0');
                iso_read_sectors(lba, (u32)tmp.size() / 2048, (u8*)&tmp[0]);
                std::memcpy(iop_mem(addr), tmp.data(), iop_clamp(addr, size, "iopheap-load"));
                wr(out, 0, 0);
            } else { rlog("iopheap", "load '%s': not found", path); wr(out, 0, (u32)-1); }
            break;
        }
        default: rlog("iopheap", "fno %u", fno); wr(out, 0, 0);
        }
    }
} s_iopheap;

// ---------------------------------------------------------------- CDVD (Sony cdvdfsv)
static struct CdInit : IopServer {
    CdInit() : IopServer(0x80000592, "cdvd-init") {}
    void call(u32 fno, u8*, u32, u8* out, u32 rsize, u32) override {
        if (out) std::memset(out, 0, rsize);
        wr(out, 0, 1); wr(out, 4, 0x200); wr(out, 8, 0x200); wr(out, 12, 0xff);   // result, versions, "ok"
        rlog("cdvd", "init fno=%u", fno);
    }
} s_cdinit;

static struct CdSCmd : IopServer {
    CdSCmd() : IopServer(0x80000593, "cdvd-scmd") {}
    void call(u32 fno, u8* send, u32 ssize, u8* out, u32 rsize, u32) override {
        if (out) std::memset(out, 0, rsize);
        switch (fno) {
        case 0x03: wr(out, 0, 0x14); break;                  // GetDiskType: PS2 DVD
        case 0x04: wr(out, 0, 0); break;                     // GetError: none
        case 0x01: {                                         // ReadClock: result + BCD clock
            wr(out, 0, 1);
            u8 clk[8] = {0, 0x00, 0x00, 0x12, 0, 0x01, 0x05, 0x05};
            if (out && rsize >= 12) std::memcpy(out + 4, clk, 8);
            break;
        }
        default: wr(out, 0, 1);
        }
        rlog("cdvd-scmd", "fno=0x%x ssize=%u", fno, ssize);
        (void)send;
    }
} s_cdscmd;

static struct CdNCmd : IopServer {
    CdNCmd() : IopServer(0x80000595, "cdvd-ncmd") {}
    void call(u32 fno, u8* send, u32 ssize, u8* out, u32 rsize, u32) override {
        if (out) std::memset(out, 0, rsize);
        switch (fno) {
        case 0x01: case 0x03: {                              // Read / DvdRead(lbn, sectors, ee_buf, ...)
            u32 lbn = rd(send, 0), n = rd(send, 4), buf = rd(send, 8);
            u8* dst = ee_ptr(buf & 0x1fffffff);
            bool ok = dst && iso_read_sectors(lbn, n, dst);
            rlog("cdvd-read", "read lbn=%u n=%u -> %08x %s", lbn, n, buf, ok ? "" : "FAILED");
            wr(out, 0, ok ? 1 : 0);
            break;
        }
        default:
            rlog("cdvd-ncmd", "fno=0x%x ssize=%u send=%08x %08x %08x %08x", fno, ssize, rd(send, 0), rd(send, 4), rd(send, 8), rd(send, 12));
            wr(out, 0, 1);
        }
    }
} s_cdncmd;

static struct CdSearch : IopServer {
    CdSearch() : IopServer(0x80000597, "cdvd-search") {}
    void call(u32 fno, u8* send, u32 ssize, u8* out, u32 rsize, u32) override {
        // send: sceCdlFILE (32 bytes) + name[256] + EE destination for the sceCdlFILE
        const char* name = (const char*)send + 32;
        u32 dest = ssize >= 292 ? rd(send, 288) : 0;
        u32 lba = 0, size = 0;
        int r = iso_find(name, &lba, &size);
        rlog("cdvd-search", "search '%s' -> %s lba=%u size=%u (fno %u, dest %08x)", name, r == 0 ? "found" : "NOT FOUND", lba, size, fno, dest);
        if (out) std::memset(out, 0, rsize);
        wr(out, 0, r == 0 ? 1 : 0);
        if (r == 0 && dest) {
            u8* d = ee_ptr(dest & 0x1fffffff);
            if (d) { wr(d, 0, lba); wr(d, 4, size); std::memcpy(d + 8, send + 8, 16); }
        }
    }
} s_cdsearch;

// Remaining libcdvd servers: acknowledge everything with "success".
struct CdMisc : IopServer {
    CdMisc(u32 sid, const char* n) : IopServer(sid, n) {}
    void call(u32 fno, u8*, u32 ssize, u8* out, u32 rsize, u32) override {
        if (out) std::memset(out, 0, rsize);
        wr(out, 0, 2);                                        // DiskReady => SCECdComplete (2)
        rlog(name, "fno=0x%x ssize=%u rsize=%u", fno, ssize, rsize);
    }
};
static CdMisc s_cd594(0x80000594, "cdvd-594"), s_cd596(0x80000596, "cdvd-596"), s_cd59a(0x8000059a, "cdvd-diskready"),
              s_cd59c(0x8000059c, "cdvd-59c");

// ---------------------------------------------------------------- FILEIO (rom0:FILEIO)
static struct FileIo : IopServer {
    FileIo() : IopServer(0x80000001, "fileio") {}
    void call(u32 fno, u8* send, u32 ssize, u8* out, u32 rsize, u32) override {
        if (out) std::memset(out, 0, rsize);
        rlog("fileio", "fno=%u ssize=%u rsize=%u '%.64s'", fno, ssize, rsize, (const char*)send);
    }
} s_fileio;

// ---------------------------------------------------------------- pads / memory cards (stubs for now)
struct Stub : IopServer {
    Stub(u32 sid, const char* n) : IopServer(sid, n) {}
    void call(u32 fno, u8*, u32 ssize, u8* out, u32 rsize, u32) override {
        if (out) std::memset(out, 0, rsize);
        rlog(name, "fno=%u ssize=%u rsize=%u", fno, ssize, rsize);
    }
};

