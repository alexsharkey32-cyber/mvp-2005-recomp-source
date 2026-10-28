// Settings file (simple key = value lines) and applying settings to the runtime.
#include "settings.h"
#include "ee_runtime.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>

Settings g_settings;
u32 g_sim_dt_bits[2] = {0x3c888889u, 0x3d088889u};    // 1/60 s, 1/30 s (see ORI_DT in ee_ops.h)

// Real-time simulation: the game treats one vblank as 1/60 s. Its fixed steps (1/60 s, 1/30 s, see ORI_DT)
// are scaled by 60/R when the vblank rate R is raised, so at 120 or 144 Hz the game runs at normal speed.
// The game's own "dynamic vsync" (2 vblanks per frame when it's slow) keeps working on top of this, which
// is why the step follows the vblank rate and not the measured frame time.
float g_fps_ratio = 1.0f;
u64 g_sim_ticks = 0;
u32 g_dt_site_lo = 0, g_dt_site_hi = 0xffffffffu;
static struct DtRangeInit { DtRangeInit() { if (const char* r = std::getenv("MVP_DT_RANGE")) { g_dt_site_lo = (u32)std::strtoul(r, nullptr, 16);
    const char* d = std::strchr(r, '-'); if (d) g_dt_site_hi = (u32)std::strtoul(d + 1, nullptr, 16); } } } s_dt_range_init;
static inline void set_dt(double scale) {
    g_fps_ratio = (float)(1.0 / scale);
    if (scale == 1.0) { g_sim_dt_bits[0] = 0x3c888889u; g_sim_dt_bits[1] = 0x3d088889u; return; }
    float f1 = (float)(scale / 60.0), f2 = (float)(scale / 30.0);
    std::memcpy(&g_sim_dt_bits[0], &f1, 4); std::memcpy(&g_sim_dt_bits[1], &f2, 4);
}
// Adaptive rate: the game can only catch up about 2 ticks per rendered frame, so a rate the PC can't
// render at would run the game in slow motion. Every second we compare game time with real time and
// step the vblank rate down (never below 60) when the game falls behind, and back up when there's room.
int g_effective_hz = 60;
static int s_cur_hz = 0;
void sim_timing_frame(u64 now) {
    static const bool turbo = std::getenv("MVP_TURBO") != nullptr;   // test runs: keep the classic fixed step
    static const int levels[] = {60, 72, 75, 90, 100, 120, 144, 165, 180, 200, 240, 288, 360, 500};
    static u64 w_t = 0, w_ticks = 0, w_frames = 0, frames = 0, hold_until = 0;
    int& cur = s_cur_hz;
    extern bool g_fast_forward;
    extern void kernel_set_refresh(int hz);
    frames++;
    int limit = settings_fps_limit();
    if (!g_settings.sim_timing || turbo || g_fast_forward || limit <= 0 || limit <= 60) {
        if (cur) { cur = 0; kernel_set_refresh(limit); }
        g_effective_hz = limit > 0 ? limit : 0;
        set_dt(g_settings.sim_timing && !turbo && !g_fast_forward && limit > 0 && limit < 60 ? 60.0 / limit : 1.0);
        w_t = 0;
        return;
    }
    if (!cur || cur > limit) { cur = limit; kernel_set_refresh(cur); set_dt(60.0 / cur); w_t = 0; }
    if (!w_t) { w_t = now; w_ticks = g_sim_ticks; w_frames = frames; }
    double el = (now - w_t) / 1e9;
    if (el >= 1.0) {
        double dt = 1.0 / cur;
        double speed = (g_sim_ticks - w_ticks) * dt / el, fps = (frames - w_frames) / el;
        bool ticking = g_sim_ticks != w_ticks;                     // menus don't tick: judge by frame rate there
        int next = cur;
        if ((ticking && speed < 0.93) || (!ticking && fps < cur * 0.85)) {
            next = 60;
            for (int l : levels) if (l < cur && l <= fps * 1.05) next = l;
            hold_until = now + 8000000000ull;                      // don't retry higher rates for a while
        } else if (now > hold_until && fps > cur * 0.95 && (!ticking || speed > 0.98)) {
            for (int l : levels) if (l > cur && l <= limit) { next = l; break; }
        }
        if (next != cur) { cur = next; kernel_set_refresh(cur); set_dt(60.0 / cur); }
        w_t = now; w_ticks = g_sim_ticks; w_frames = frames;
    }
    g_effective_hz = cur;
}

static std::string s_path_override;
void settings_set_path(const std::string& p) { s_path_override = p; }
std::string settings_path() {
    if (!s_path_override.empty()) return s_path_override;
    if (const char* p = std::getenv("MVP_CONFIG")) return p;
    return "mvp2005.ini";
}

// One table drives loading, saving and range checks.
struct Field { const char* key; int* i; bool* b; int lo, hi; };
static std::vector<Field> fields() {
    Settings& s = g_settings;
    return {
        {"aspect", &s.aspect, nullptr, 0, 5}, {"hud_169", nullptr, &s.hud_169, 0, 1}, {"fullscreen", nullptr, &s.fullscreen, 0, 1}, {"fs_exclusive", nullptr, &s.fs_exclusive, 0, 1}, {"display", &s.display, nullptr, 0, 15},
        {"window_scale", &s.window_scale, nullptr, 1, 6}, {"scale_filter", &s.scale_filter, nullptr, 0, 3},
        {"integer_scale", nullptr, &s.integer_scale, 0, 1}, {"vsync", nullptr, &s.vsync, 0, 1},
        {"ui_scale", &s.ui_scale, nullptr, 0, 200},
        {"renderer", &s.renderer, nullptr, 0, 1}, {"gpu_api", &s.gpu_api, nullptr, 0, 3}, {"internal_res", &s.internal_res, nullptr, 0, 6}, {"tex_filter", &s.tex_filter, nullptr, 0, 1}, {"aniso", &s.aniso, nullptr, 0, 16},
        {"aa", &s.aa, nullptr, 0, 2}, {"sharpen", &s.sharpen, nullptr, 0, 100}, {"gs_threads", &s.gs_threads, nullptr, -1, 16},
        {"tex_enhance", &s.tex_enhance, nullptr, 0, 2}, {"tex_replace", nullptr, &s.tex_replace, 0, 1}, {"tex_dump", nullptr, &s.tex_dump, 0, 1},
        {"brightness", &s.brightness, nullptr, -50, 50}, {"contrast", &s.contrast, nullptr, 50, 150},
        {"saturation", &s.saturation, nullptr, 0, 200}, {"gamma", &s.gamma, nullptr, 50, 200}, {"crt", &s.crt, nullptr, 0, 2},
        {"fps_limit", &s.fps_limit, nullptr, -1, 1000}, {"sim_timing", &s.sim_timing, nullptr, 0, 1},
        {"frameskip", &s.frameskip, nullptr, 0, 4}, {"show_fps", &s.show_fps, nullptr, 0, 2}, {"fps_corner", &s.fps_corner, nullptr, 0, 3}, {"pause_unfocused", nullptr, &s.pause_unfocused, 0, 1},
        {"rosters", &s.rosters, nullptr, 0, 2}, {"commentary", nullptr, &s.commentary, 0, 1}, {"launcher", nullptr, &s.launcher, 0, 1}, {"intro_movies", nullptr, &s.intro_movies, 0, 1}, {"ai", &s.ai, nullptr, 0, 1}, {"team", &s.team, nullptr, -1, 29}, {"menu_music", nullptr, &s.menu_music, 0, 1}, {"check_updates", nullptr, &s.check_updates, 0, 1}, {"music", &s.music, nullptr, 0, 2}, {"uniforms", &s.uniforms, nullptr, 0, 1}, {"stadiums", &s.stadiums, nullptr, 0, 1}, {"portraits", nullptr, &s.portraits, 0, 1}, {"head_shapes", nullptr, &s.head_shapes, 0, 1}, {"sim_league", &s.sim_league, nullptr, 0, 1},
        {"volume", &s.volume, nullptr, 0, 100}, {"mute", nullptr, &s.mute, 0, 1}, {"setup_done", nullptr, &s.setup_done, 0, 1},
    };
}
bool settings_get(const std::string& key, int& value) {
    for (auto& fd : fields()) if (key == fd.key) { value = fd.b ? (int)*fd.b : *fd.i; return true; }
    return false;
}
bool settings_set(const std::string& key, int value) {
    for (auto& fd : fields()) {
        if (key != fd.key) continue;
        if (fd.b) *fd.b = value != 0; else if (value >= fd.lo && value <= fd.hi) *fd.i = value; else return false;
        return true;
    }
    return false;
}
static bool as_bool(const char* v) { return !std::strcmp(v, "1") || !std::strcmp(v, "true") || !std::strcmp(v, "on") || !std::strcmp(v, "yes"); }

bool settings_load(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    auto fs = fields();
    bool seen_aspect = false;
    char line[1200];
    while (std::fgets(line, sizeof line, f)) {
        char* eq = std::strchr(line, '=');
        if (!eq || line[0] == '#' || line[0] == ';') continue;
        *eq = 0;
        auto trim = [](char* s) { while (*s == ' ' || *s == '\t') s++; char* e = s + std::strlen(s);
                                  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0; return s; };
        char* k = trim(line); char* v = trim(eq + 1);
        // older files
        if (!std::strcmp(k, "filter")) { int x = std::atoi(v); g_settings.scale_filter = x == 0 ? 0 : x == 2 ? 0 : 3; g_settings.integer_scale = x == 2; continue; }
        if (!std::strcmp(k, "iso")) { g_settings.iso = v; continue; }
        if (!std::strcmp(k, "league")) { g_settings.league = v; continue; }
        if (!std::strcmp(k, "features_url")) { g_settings.features_url = v; continue; }
        if (!std::strcmp(k, "scanlines")) { if (as_bool(v)) g_settings.crt = 1; continue; }
        if (!std::strcmp(k, "widescreen")) { if (!seen_aspect) g_settings.aspect = as_bool(v) ? 1 : 0; continue; }   // older files
        if (!std::strcmp(k, "aspect")) seen_aspect = true;
        for (auto& fd : fs) {
            if (std::strcmp(k, fd.key)) continue;
            if (fd.b) *fd.b = as_bool(v);
            else { int x = std::atoi(v); *fd.i = x < fd.lo || x > fd.hi ? *fd.i : x; }
        }
    }
    std::fclose(f);
    return true;
}

bool settings_save(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "# MVP Baseball 2005 recomp settings (also editable in game: F1)\n");
    for (auto& fd : fields()) std::fprintf(f, "%s = %d\n", fd.key, fd.b ? (int)*fd.b : *fd.i);
    if (!g_settings.iso.empty()) std::fprintf(f, "iso = %s\n", g_settings.iso.c_str());
    if (!g_settings.league.empty()) std::fprintf(f, "league = %s\n", g_settings.league.c_str());
    if (!g_settings.features_url.empty()) std::fprintf(f, "features_url = %s\n", g_settings.features_url.c_str());
    std::fclose(f);
    return true;
}

double g_display_aspect = 16.0 / 9.0;
int g_display_hz = 60, g_auto_res = 2;
float g_ws_aspect = 0, g_ws_xscale = 0;             // tools/recomp/patches.txt: the widescreen projection (0 = EA's 16:9)
double settings_aspect() {
    switch (g_settings.aspect) {
    case 0: return 4.0 / 3.0;
    case 1: return 16.0 / 9.0;
    case 2: return 16.0 / 10.0;
    case 3: return 64.0 / 27.0;                     // "21:9" (2560x1080, 3440x1440 is 43:18, close)
    case 4: return 32.0 / 9.0;
    default: return std::max(1.25, std::min(4.0, g_display_aspect));
    }
}
int settings_internal_res(bool gpu) {
    int r = g_settings.internal_res > 0 ? g_settings.internal_res : g_auto_res;
    return std::max(1, std::min(gpu ? 6 : 4, r));
}
int settings_fps_limit() { return g_settings.fps_limit >= 0 ? g_settings.fps_limit : std::max(30, std::min(500, g_display_hz)); }
void settings_apply() {
    extern bool g_opt_widescreen;
    const double a = settings_aspect();
    g_settings.widescreen = a > 1.34;
    g_opt_widescreen = g_settings.widescreen;
    const bool ea = std::fabs(a - 16.0 / 9.0) < 0.01;              // EA's own numbers when it is exactly 16:9
    g_ws_aspect = g_settings.widescreen && !ea ? (float)a : 0.0f;
    g_ws_xscale = g_settings.widescreen && !ea ? (float)(a * 0.75) : 0.0f;
    extern float g_hud_k;
    g_hud_k = g_settings.hud_169 && a > 16.0 / 9.0 + 0.01 ? (float)(16.0 / 9.0 / a) : 1.0f;
    extern void kernel_set_refresh(int hz);
    kernel_set_refresh(settings_fps_limit());
    s_cur_hz = 0;                                    // real-time mode re-evaluates from the new limit
}
