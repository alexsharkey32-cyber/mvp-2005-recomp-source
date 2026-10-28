// IOP high-level emulation interfaces.
#pragma once
#include <map>
#include <string>
#include <vector>
#include "ee_context.h"

static const u32 IOP_RAM_SIZE = 0x00200000;   // 2 MB
extern u8 g_iop_ram[];                       // IOP_RAM_SIZE bytes + a guard area for reads
// Clamp an IOP memory access [addr, addr+n) to the 2 MB RAM (logs once when a request runs past the end).
u32 iop_clamp(u32 addr, u32 n, const char* who);
static inline u8* iop_mem(u32 addr) { return g_iop_ram + (addr & (IOP_RAM_SIZE - 1)); }
u32 iop_alloc(u32 size);
bool iop_free(u32 addr);                   // false: not a block iop_alloc gave out
bool iop_deliver();

// A native replacement for one IOP RPC server (identified by its server id).
struct IopServer {
    u32 sid;
    const char* name;
    u32 buffer_size = 0x8000;
    IopServer(u32 sid, const char* name);
    virtual ~IopServer() {}
    // send: request data (already in IOP memory); out: EE receive buffer (may be null)
    virtual void call(u32 fno, u8* send, u32 ssize, u8* out, u32 rsize, u32 out_addr) = 0;
};

// Disc image access (iso.cpp)
bool iso_open(const char* path);
bool iso_is_open();
// Returns file handle >= 0 for a path like "cdrom0:\\DATA\\FILE.BIG;1" or "DATA/FILE.BIG"
int  iso_find(const char* path, u32* lba, u32* size);
bool iso_read_sectors(u32 lba, u32 count, u8* dst);
void iso_close();
std::string iso_check_game(const char* path, const char* elf_out);   // launcher: "" if it's the right disc
int  iso_add_overlay(const char* dir);           // mods/<name>/ with the disc layout: files replace the disc's
bool iso_add_overlay_data(const char* path, std::vector<u8>&& data, const char* from);   // one file, from memory
int iso_big_switchable(const char* archive, const std::map<std::string, std::vector<std::string>>& slots);   // members that change at run time
bool iso_big_switch(const char* archive, const char* member, const char* cand);
bool iso_file_switchable(const char* path, const std::vector<std::string>& cands);   // whole files that change at run time
std::vector<std::string> iso_list(const char* prefix);
bool iso_file_switch(const char* path, const char* cand);                           // "" as loaded, "=" the disc's own
bool iso_file_bytes(const char* path, std::vector<u8>& out);                             // as the game will see it
