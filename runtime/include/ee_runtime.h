#pragma once
#include "ee_context.h"
#include <cstdio>
#include <string>
#include <vector>

// Logging with per-key throttling so hot paths don't flood stdout.
void rlog(const char* key, const char* fmt, ...);
[[noreturn]] void rfatal(const char* fmt, ...);

// Memory
extern u8* g_ram;
extern u8 g_spr[0x4000];
void mem_init();

// ELF loader
bool elf_load(const char* path);

// Function table
EEFunc ee_find_func(u32 addr);

// Kernel / threads (ee_kernel.cpp)
void kernel_init(EEContext* boot);
void kernel_run_main(u32 entry);
EEContext* kernel_current();
void kernel_yield(EEContext* c);          // called from polling loops / syscalls
u64 host_time_ns();
// timers around the hottest calls (per VU1 program, per GIF packet, per primitive): only with MVP_TIMING=1,
// reading the clock that often is 2% of the main thread
extern bool g_host_timing;
static inline u64 prof_time_ns() { return g_host_timing ? host_time_ns() : 0; }

// Hardware (ee_hw.cpp)
void hw_init();
void hw_tick(EEContext* c);               // periodic: vblank, timers

// Config
struct RuntimeConfig {
    std::string elf_path = "game/SLUS_211.35";
    std::string iso_path;                 // optional, for cdrom0: file access
    std::vector<std::string> mods;        // --mod dir: extra disc overlays (see iso_add_overlay)
    bool trace_calls = false;
    int  log_limit = 20;                  // max prints per log key
};
extern RuntimeConfig g_cfg;
