// Graphics Synthesizer emulation (software reference renderer).
#pragma once
#include "ee_context.h"
#include <vector>

// GIF input: qwc quadwords from PATH1 (VU1 XGKICK), PATH2 (VIF1 DIRECT) or PATH3 (GIF DMA)
void gs_gif_write(const u8* data, u32 qwc, int path);
// Privileged registers (0x12000000..)
void gs_priv_write(u32 addr, u64 value);
u64  gs_priv_read(u32 addr);
// Called at vblank: builds the displayed image
void gs_vsync();
void gs_sync();                    // finish all queued rasterization (worker threads)
// Current displayed frame as RGBA8 (width*height*4). Returns false if nothing displayable yet.
bool gs_get_display(std::vector<u32>& rgba, int& width, int& height);
u64 gs_display_version();          // changes whenever the displayed picture changed
bool gs_screenshot(const char* png_path);
void gs_stats();
