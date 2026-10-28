// Controller state shared by the input backends (SDL window, scripted input) and the pad driver.
#pragma once
#include "ee_context.h"

enum : u16 {
    PAD_SELECT = 0x0001, PAD_L3 = 0x0002, PAD_R3 = 0x0004, PAD_START = 0x0008,
    PAD_UP = 0x0010, PAD_RIGHT = 0x0020, PAD_DOWN = 0x0040, PAD_LEFT = 0x0080,
    PAD_L2 = 0x0100, PAD_R2 = 0x0200, PAD_L1 = 0x0400, PAD_R1 = 0x0800,
    PAD_TRIANGLE = 0x1000, PAD_CIRCLE = 0x2000, PAD_CROSS = 0x4000, PAD_SQUARE = 0x8000,
};

struct PadState {
    bool connected = false;
    u16 buttons = 0;                 // PAD_* bits, 1 = pressed
    u8 lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
};
extern PadState g_pad[2];

u16 pad_button_from_name(const char* name);
void pad_vsync(u64 vsync);
