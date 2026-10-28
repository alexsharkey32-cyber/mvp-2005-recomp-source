// Native replacement for the IOP pad driver (PADMAN, RPC servers 0x80000100 / 0x80000101).
//
// libpad on the EE (0x5C07D8 init, 0x5C09B8 PortOpen, 0x5C0CB8 Read, 0x5C0E20 GetState,
// 0x5C1088 InfoMode) gives the IOP a 256-byte buffer per port = two 128-byte frames. The IOP keeps
// writing the older frame with fresh data and a higher frame counter; the EE reads the newer one.
// Frame layout (offsets): +0 data[32] | +80 modeTable u16[4] | +88 frame | +96 length |
// +100 modeConfig | +101 modeCurId | +104 nrOfModes | +105 modeCurOffs | +112 state | +113 reqState | +114 ok
#include "ee_ops.h"
#include "ee_runtime.h"
#include "iop.h"
#include "pad.h"
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static inline u32 rd(const u8* p, int o) { u32 v; std::memcpy(&v, p + o, 4); return v; }
static inline void wr(u8* p, int o, u32 v) { if (p) std::memcpy(p + o, &v, 4); }

PadState g_pad[2];
static u32 s_port_buf[2] = {0, 0};
static u32 s_frame = 0;
static u32 s_mode[2] = {1, 1};                 // 0 = digital, 1 = analog (DualShock)

static struct PadMan : IopServer {
    PadMan(u32 sid, const char* n) : IopServer(sid, n) {}
    void call(u32 fno, u8* send, u32 ssize, u8* out, u32 rsize, u32) override {
        u32 cmd = rd(send, 0), port = rd(send, 4), slot = rd(send, 8);
        u32 result = 1;
        switch (cmd) {
        case 0x01:                                            // open(port, slot, buffer)
            if (port < 2 && slot == 0) { s_port_buf[port] = rd(send, 16); rlog("pad", "port %u opened, buffer %08x", port, s_port_buf[port]); }
            break;
        case 0x02: if (port < 2) s_port_buf[port] = 0; break;  // close
        case 0x06:                                             // SetMainMode(port, slot, mode, lock)
            if (port < 2) { s_mode[port] = rd(send, 12) & 1; rlog("pad", "port %u mode -> %s", port, s_mode[port] ? "analog" : "digital"); }
            break;
        case 0x10: break;                                      // end / reset
        case 0x12: result = 2; break;                          // GetPortMax
        case 0x13: result = 1; break;                          // GetSlotMax
        default: rlog("pad", "%s cmd 0x%x port %u slot %u", name, cmd, port, slot); break;
        }
        (void)fno; (void)ssize;
        if (out) {
            std::memcpy(out, send, rsize < 128 ? rsize : 128);
            wr(out, 12, result);                             // queries return at +12,
            if (cmd == 0x0a) wr(out, 16, result);            // SetButtonInfo (pressure mode) at +16,
            else wr(out, 20, result);                        // SetMainMode / actuator calls at +20
        }
    }
} s_padman(0x80000100, "padman"), s_padman2(0x80000101, "padman-ext");

// ---------------------------------------------------------------- scripted input (headless testing)
// MVP_INPUT="600:CROSS,660:DOWN,700:START" presses buttons at the given vsync for 20 vsyncs ("660:DOWN/6": for 6).
struct Scripted { u64 frame; u16 buttons; u64 dur; u8 sticks = 0; };          // dur 0 = the default length
static std::vector<Scripted> s_script;
// sticks in scripts: RX- RX+ RY- RY+ LX- LX+ LY- LY+ (pushed all the way)
static u8 stick_from_name(const std::string& n) {
    static const char* t[8] = {"RX-", "RX+", "RY-", "RY+", "LX-", "LX+", "LY-", "LY+"};
    for (int i = 0; i < 8; i++) if (n == t[i]) return (u8)(1 << i);
    return 0;
}
static void parse_script() {
    static bool done = false; if (done) return; done = true;
    const char* e = std::getenv("MVP_INPUT"); if (!e) return;
    std::string s(e); size_t p = 0;
    while (p < s.size()) {
        size_t q = s.find(',', p); if (q == std::string::npos) q = s.size();
        std::string item = s.substr(p, q - p); size_t c = item.find(':');
        if (c != std::string::npos) {
            u64 f = std::strtoull(item.substr(0, c).c_str(), nullptr, 10); u16 b = 0; u8 st = 0;
            std::string names = item.substr(c + 1); size_t r = 0; u64 dur = 0;
            size_t sl = names.find('/'); if (sl != std::string::npos) { dur = std::strtoull(names.substr(sl + 1).c_str(), nullptr, 10); names = names.substr(0, sl); }   // "2840:DOWN/6": held 6 frames
            while (r <= names.size()) {
                size_t t = names.find('+', r); if (t == std::string::npos) t = names.size();
                b |= pad_button_from_name(names.substr(r, t - r).c_str()); st |= stick_from_name(names.substr(r, t - r)); r = t + 1;
            }
            s_script.push_back({f, b, dur, st});
        }
        p = q + 1;
    }
}

// Test scripts (MVP_AUTOPRESS): hold these buttons ("CROSS", "L1+R1") for some game frames, starting a few frames from now.
void pad_script_press(const char* names, u64 frames) {
    extern u64 g_game_frames;
    u16 b = 0; u8 st = 0; std::string s(names); size_t r = 0;
    while (r <= s.size()) {
        size_t t = s.find('+', r); if (t == std::string::npos) t = s.size();
        if (t > r && s[t - 1] == 'X' && t < s.size()) t = s.find('+', t + 1) == std::string::npos ? s.size() : s.find('+', t + 1);   // "RX+" itself has a '+'
        std::string n = s.substr(r, t - r); b |= pad_button_from_name(n.c_str()); st |= stick_from_name(n); r = t + 1;
    }
    if (b || st) s_script.push_back({g_game_frames + 2, b, frames, st});
}

u16 pad_button_from_name(const char* n) {
    static const struct { const char* n; u16 b; } t[] = {
        {"SELECT", PAD_SELECT}, {"L3", PAD_L3}, {"R3", PAD_R3}, {"START", PAD_START}, {"UP", PAD_UP}, {"RIGHT", PAD_RIGHT},
        {"DOWN", PAD_DOWN}, {"LEFT", PAD_LEFT}, {"L2", PAD_L2}, {"R2", PAD_R2}, {"L1", PAD_L1}, {"R1", PAD_R1},
        {"TRIANGLE", PAD_TRIANGLE}, {"CIRCLE", PAD_CIRCLE}, {"CROSS", PAD_CROSS}, {"SQUARE", PAD_SQUARE}};
    for (auto& x : t) if (std::strcmp(x.n, n) == 0) return x.b;
    return 0;
}

// ---------------------------------------------------------------- per-vsync update
static const bool s_hold = std::getenv("MVP_INPUT_HOLD") != nullptr;      // debugging: keep buttons held
void pad_vsync(u64 vsync) {
    parse_script();
    extern u64 g_game_frames; vsync = g_game_frames;       // scripts are timed in game frames
    // MVP_INPUT_MS=1: times are milliseconds of wall-clock time since start; presses last 330 ms
    static const bool ms = std::getenv("MVP_INPUT_MS") != nullptr;
    static const u64 t0 = host_time_ns();
    u64 dur = 20;
    if (ms) { vsync = (host_time_ns() - t0) / 1000000; dur = 330; }
    u16 scripted = 0; u8 sticks = 0;
    const bool hold = s_hold;
    for (auto& s : s_script) if (vsync >= s.frame && (hold || vsync < s.frame + (s.dur && !ms ? s.dur : dur))) { scripted |= s.buttons; sticks |= s.sticks; }
    s_frame++;
    for (int port = 0; port < 2; port++) {
        u32 buf = s_port_buf[port];
        if (!buf) continue;
        u8* base = ee_ptr(buf & 0x1fffffff);
        if (!base) continue;
        // write the older of the two frames
        u8* f = rd(base, 88) <= rd(base + 128, 88) ? base : base + 128;
        const PadState& p = g_pad[port];
        bool connected = port == 0 || p.connected;
        u16 buttons = p.buttons | (port == 0 ? scripted : 0);
        u8 d[32];
        std::memset(d, 0, sizeof d);
        d[0] = 0x00;                         // ok
        bool analog = s_mode[port] != 0;
        d[1] = analog ? 0x79 : 0x41;         // DualShock 2 analog+pressure (18 data bytes) or digital (2)
        u16 active_low = (u16)~buttons;
        d[2] = active_low & 0xff; d[3] = active_low >> 8;
        d[4] = p.rx; d[5] = p.ry; d[6] = p.lx; d[7] = p.ly;
        if (port == 0 && sticks)
            for (int k = 0; k < 4; k++) if (sticks & (3 << (2 * k))) d[4 + k] = (sticks & (1 << (2 * k))) ? 0x00 : 0xff;
        static const u16 press_order[12] = {PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
                                            PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2};
        for (int k = 0; k < 12; k++) d[8 + k] = (buttons & press_order[k]) ? 0xff : 0;   // pressure
        std::memcpy(f, d, 32);
        u16 modes[4] = {4, 7, 0, 0};                                // mode table: ext ids (4 digital, 7 DualShock)
        std::memcpy(f + 80, modes, 8);
        u8 act[8] = {0, 0, 0, 1, 1, 1, 1, 1};                       // actuator info: small + large motor
        std::memcpy(f + 48, act, 8);
        wr(f, 96, 32);                                              // length returned by scePadRead
        f[100] = 2;                                                 // modeConfig: mode/actuator info available
        f[101] = analog ? 0x73 : 0x41;                              // current mode id
        f[104] = 2;                                                 // number of modes
        f[105] = analog ? 1 : 0;                                    // current mode offset
        f[106] = 2;                                                 // number of actuators
        f[102] = 3;                                                 // model: DualShock 2
        f[121] = 0xff; f[122] = 0xff; f[123] = 0x03; f[124] = 0x00; // button mask 0x3ffff: pressure supported
        f[112] = connected ? 6 : 0;                                 // PAD_STATE_STABLE / DISCONN
        f[113] = 0;                                                 // PAD_RSTAT_COMPLETE
        f[114] = 1;                                                 // ok
        wr(f, 88, s_frame);                                         // publish (newer frame counter)
    }
}
