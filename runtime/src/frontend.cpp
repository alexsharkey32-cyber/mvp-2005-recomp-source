// Window, input, the pause/settings menu and on-screen overlays. Built with SDL2 when available (-DMVP_SDL),
// otherwise headless. Drawing goes through a Presenter (OpenGL with post effects, or SDL_Renderer).
//
// Keyboard:  arrows = D-pad       Enter = START      Right Shift = SELECT
//            Z = Cross   X = Circle   A = Square   S = Triangle
//            Q = L1   W = R1   1 = L2   2 = R2       I/J/K/L = left stick
//            F1 / Esc = menu (pauses)   F2 = FPS counter   F11 = fullscreen   F12 = screenshot
//            Tab (hold) = fast forward
// Gamepads are mapped automatically; Back+Start or the Guide button opens the menu. The mouse works in the menu.
#include "ee_runtime.h"
#include "gs.h"
#include "pad.h"
#include "settings.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

bool g_headless = false;

#ifdef MVP_SDL
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "present.h"
#include "gs_gpu.h"
#include "ui.h"
#include "game_art.h"
#include "menu_music.h"
#include "music_import.h"
#include <ctime>

extern bool g_fast_forward;
extern u64 g_game_frames, g_frames_drawn, g_frames_skipped;
u64 kernel_vblank_count();
size_t gs_replacement_count();

// ---------------------------------------------------------------- state
static SDL_Window* s_win = nullptr;
static Presenter* s_pres = nullptr;
static SDL_GameController* s_pad = nullptr;
static std::vector<u32> s_px;
static int s_game_w = 0, s_game_h = 0;
static u64 s_shown_version = ~0ull;
static u64 s_last_present = 0;
static bool s_cur_fullscreen = false, s_cur_vsync = false, s_cur_excl = false; static int s_cur_display = -1;
static UiBatch s_ui;
static float s_mouse_x = -1, s_mouse_y = -1;         // drawable pixels
static bool s_mouse_down = false;

static std::string s_toast; static u64 s_toast_t = 0;
static void toast(const std::string& m) { s_toast = m; s_toast_t = host_time_ns(); }

// test hooks
static const long long s_gui_test = std::getenv("MVP_GUI_TEST") ? std::atoll(std::getenv("MVP_GUI_TEST")) : -1;
static bool s_gui_test_done = false;
static long long s_win_shot_frame = std::getenv("MVP_WIN_SHOT") ? std::atoll(std::getenv("MVP_WIN_SHOT")) : -1;
static std::string s_win_shot_path = std::getenv("MVP_WIN_SHOT") && std::strchr(std::getenv("MVP_WIN_SHOT"), ':') ? std::strchr(std::getenv("MVP_WIN_SHOT"), ':') + 1 : "window.png";

// palette
static const u32 C_PANEL = rgba(16, 21, 29, 244), C_SIDE = rgba(11, 15, 21, 255), C_ROW_HOVER = rgba(36, 45, 58, 255);
static const u32 C_ACCENT = rgba(229, 72, 60), C_ACCENT2 = rgba(255, 196, 72), C_TEXT = rgba(236, 240, 245), C_DIM = rgba(143, 156, 173);
static const u32 C_CTRL = rgba(35, 43, 55), C_LINE = rgba(48, 58, 72), C_OK = rgba(94, 214, 140);

// ---------------------------------------------------------------- menu model
enum ItemType { IT_CHOICE, IT_TOGGLE, IT_SLIDER, IT_BUTTON, IT_INFO };
struct Item {
    ItemType type; std::string label, help;
    std::vector<std::string> options;              // IT_CHOICE
    std::function<int()> get; std::function<void(int)> set;
    int lo = 0, hi = 100, step = 5; std::string unit; // IT_SLIDER
    bool restart = false;
    std::function<std::string()> info;             // IT_INFO / dynamic text
};
struct Page { std::string name, subtitle; std::vector<Item> items; int kind = 0; int group = 0; bool preview = false; };
// kind 1: the home-team grid, 2: the leagues carousel; group 1: a settings section (listed at the side); preview: a live
// preview of the picture beside the rows
static std::vector<Page> s_pages;
static bool s_menu = false;
static int s_page = 0, s_sel = 0;                  // selected page / item
static bool s_focus_side = true;                   // keyboard focus in the sidebar
static float s_anim = 0, s_sel_anim = 0;           // open animation, highlight position
static int s_action = -1;
enum { ACT_RESUME, ACT_SAVE, ACT_RESET, ACT_SHOT, ACT_QUIT, ACT_RESET_IMAGE, ACT_IMPORT, ACT_PLAY, ACT_MUSIC_IMPORT, ACT_MUSIC_CLEAR, ACT_GET_UPDATE, ACT_UPDATE_FEATURES };
static std::vector<MusicTrack> s_my_music; static bool s_my_music_read = false;          // music/*.asf (refreshed after an import)
static bool s_music_failed = false; static int s_music_mode = -1;       // launcher music: no device or nothing to play / the mode playing
static const std::vector<MusicTrack>& my_music() { if (!s_my_music_read) { s_my_music = music_tracks(); s_my_music_read = true; } return s_my_music; }

// launcher state (the settings menu shown before the game starts, with a "Game files" page)
static bool s_launcher = false;
static std::string s_launch_iso, s_launch_elf, s_launch_msg;
static bool s_launch_ok = false;
static std::string short_path(const std::string& p, size_t n = 38) { return p.size() <= n ? p : "…" + p.substr(p.size() - n); }
static bool file_exists(const std::string& p) { if (FILE* f = fopen(p.c_str(), "rb")) { fclose(f); return true; } return false; }

static const int kFps[] = {30, 50, 60, 72, 75, 90, 100, 120, 144, 165, 180, 200, 240, 360, -1, 0};
static const int kNFps = 16;
static std::vector<std::string> fps_options() {
    std::vector<std::string> o;
    for (int i = 0; i < kNFps; i++) o.push_back(kFps[i] == 0 ? std::string("Unlimited") : kFps[i] < 0 ? "Match my display (" + std::to_string(g_display_hz) + ")" : kFps[i] == 60 ? std::string("60 (standard)") : std::to_string(kFps[i]));
    return o;
}
static int fps_index() {
    for (int i = 0; i < kNFps; i++) if (kFps[i] == g_settings.fps_limit) return i;
    int best = 2, bd = 1 << 30;
    for (int i = 0; i < kNFps; i++) { if (kFps[i] <= 0) continue; int d = std::abs(kFps[i] - g_settings.fps_limit); if (d < bd) { bd = d; best = i; } }
    return best; }
static std::string aspect_name(double a) {
    struct { double v; const char* n; } k[] = {{4.0 / 3, "4:3"}, {5.0 / 4, "5:4"}, {16.0 / 10, "16:10"}, {16.0 / 9, "16:9"}, {64.0 / 27, "21:9"}, {43.0 / 18, "21:9"}, {32.0 / 9, "32:9"}, {3.0 / 2, "3:2"}};
    for (auto& e : k) if (std::fabs(a - e.v) < 0.012) return e.n;
    char b[16]; snprintf(b, sizeof b, "%.2f:1", a); return b;
}
static std::vector<std::string> display_names() {
    std::vector<std::string> o; int n = std::max(1, SDL_GetNumVideoDisplays());
    for (int i = 0; i < n; i++) {
        SDL_DisplayMode m{}; const char* nm = SDL_GetDisplayName(i);
        std::string s = std::to_string(i + 1) + ": " + (nm && *nm ? nm : "Display");
        if (SDL_GetDesktopDisplayMode(i, &m) == 0) s += "  (" + std::to_string(m.w) + " x " + std::to_string(m.h) + (m.refresh_rate ? ", " + std::to_string(m.refresh_rate) + " Hz)" : ")");
        o.push_back(s);
    }
    return o;
}
static std::vector<std::string> api_options() {
#if defined(__APPLE__)
    return {"Auto (Metal)", "OpenGL", "Metal", "Vulkan"};
#else
    return {"Auto (OpenGL)", "OpenGL", "Metal (Mac only)", "Vulkan"};
#endif
}
// quality presets: 1 original, 2 performance, 3 balanced, 4 quality, 5 remastered (0 = custom: none matches)
struct Preset { int res, aa, aniso, enh; bool pack; int filt, sharp; };
static const Preset kPresets[6] = {{0, 0, 0, 0, 0, 0, 0}, {1, 0, 0, 0, false, 1, 0}, {2, 0, 0, 0, true, 1, 0}, {0, 1, 8, 0, true, 1, 10}, {4, 1, 16, 0, true, 1, 10}, {4, 2, 16, 2, true, 1, 15}};
static int preset_get() {
    const Settings& S = g_settings;
    for (int i = 5; i >= 1; i--) { const Preset& p = kPresets[i];
        if (S.internal_res == p.res && S.aa == p.aa && S.aniso == p.aniso && S.tex_enhance == p.enh && S.tex_replace == p.pack && S.tex_filter == p.filt && S.sharpen == p.sharp && (i == 1 || S.renderer == 1)) return i; }
    return 0;
}
static void preset_set(int v) {
    if (v < 1 || v > 5) return;
    Settings& S = g_settings; const Preset& p = kPresets[v];
    if (v > 1) S.renderer = 1;
    S.internal_res = p.res; S.aa = p.aa; S.aniso = p.aniso; S.tex_enhance = p.enh; S.tex_replace = p.pack; S.tex_filter = p.filt; S.sharpen = p.sharp;
}
static void resize_window_to_aspect();
#include "installer.h"
static std::string features_summary() {
    if (features_load_state() != 2) return "MVP Baseball 2005 (USA) \xE2\x9C\x93";
    int n = 0, m = 0; for (const Feature& f : features()) { m++; if (f.installed) n++; }
    return "Disc \xE2\x9C\x93  \xC2\xB7  " + std::to_string(n) + " of " + std::to_string(m) + " features";
}
static void apply_video();

static Item choice(const std::string& l, std::vector<std::string> o, std::function<int()> g, std::function<void(int)> s, const std::string& h) {
    Item it{IT_CHOICE, l, h}; it.options = std::move(o); it.get = g; it.set = s; return it;
}
static Item toggle(const std::string& l, bool* v, const std::string& h) {
    Item it{IT_TOGGLE, l, h}; it.get = [v] { return (int)*v; }; it.set = [v](int x) { *v = x != 0; }; return it;
}
static Item slider(const std::string& l, int* v, int lo, int hi, int step, const std::string& unit, const std::string& h) {
    Item it{IT_SLIDER, l, h}; it.get = [v] { return *v; }; it.set = [v, lo, hi](int x) { *v = std::min(hi, std::max(lo, x)); };
    it.lo = lo; it.hi = hi; it.step = step; it.unit = unit; return it;
}
static Item button(const std::string& l, int act, const std::string& h) {
    Item it{IT_BUTTON, l, h}; it.get = [] { return 0; }; it.set = [act](int) { s_action = act; }; return it;
}
static Item info(const std::string& l, std::function<std::string()> f) { Item it{IT_INFO, l, ""}; it.info = f; return it; }
// league packs: mods/pack-*/pack.json ("name": "...")
static std::vector<std::pair<std::string, std::string>> s_packs;              // folder, name
static std::vector<std::string> roster_options() {
    s_packs.clear(); std::error_code ec;
    for (auto it = std::filesystem::directory_iterator("mods", ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        std::string d = it->path().filename().string();
        if (d.rfind("pack-", 0) != 0 || !std::filesystem::exists(it->path() / "DATA", ec)) continue;
        std::string name = d.substr(5);
        if (FILE* f = fopen((it->path() / "pack.json").string().c_str(), "r")) {
            char buf[4096]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
            if (const char* p = strstr(buf, "\"name\"")) if ((p = strchr(p + 6, ':')) && (p = strchr(p, '"'))) { const char* e = strchr(p + 1, '"'); if (e) name = std::string(p + 1, e); }
        }
        s_packs.push_back({d, name});
    }
    std::sort(s_packs.begin(), s_packs.end());
    std::vector<std::string> o = {"Original (2005)", "Modern (2026)"};
    for (auto& p : s_packs) o.push_back(p.second);
    return o;
}
static int roster_get() {
    if (g_settings.rosters < 2) return g_settings.rosters;
    for (size_t i = 0; i < s_packs.size(); i++) if (s_packs[i].first == g_settings.league) return 2 + (int)i;
    return 1;
}
static void roster_set(int v) {
    if (v < 2 || v - 2 >= (int)s_packs.size()) { g_settings.rosters = std::min(v, 1); return; }
    g_settings.rosters = 2; g_settings.league = s_packs[v - 2].first;
}

// ---------------------------------------------------------------- eras: the league packs made from history (pack.json "era": true)
struct Era { std::string dir, name, blurb; int season = 0; };
static std::vector<Era> s_eras;
static std::string json_str(const std::string& j, const char* key) {
    size_t p = j.find(std::string("\"") + key + "\""); if (p == std::string::npos) return "";
    p = j.find(':', p); if (p == std::string::npos) return "";
    size_t q = j.find_first_not_of(" \t\r\n", p + 1); if (q == std::string::npos) return "";
    if (j[q] != '"') { size_t e = j.find_first_of(",}\n", q); return j.substr(q, e - q); }
    std::string out; for (size_t i = q + 1; i < j.size() && j[i] != '"'; i++) { if (j[i] == '\\' && i + 1 < j.size()) i++; out += j[i]; }
    return out;
}
static void eras_scan() {
    s_eras.clear(); std::error_code ec;
    for (auto it = std::filesystem::directory_iterator("mods", ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        std::string d = it->path().filename().string();
        if (d.rfind("pack-", 0) != 0) continue;
        FILE* f = fopen((it->path() / "pack.json").string().c_str(), "r"); if (!f) continue;
        std::string j; char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) j.append(buf, n); fclose(f);
        if (json_str(j, "era") != "true") continue;
        Era e; e.dir = d; e.name = json_str(j, "name"); e.blurb = json_str(j, "blurb"); e.season = std::atoi(json_str(j, "season").c_str());
        s_eras.push_back(e);
    }
    std::sort(s_eras.begin(), s_eras.end(), [](const Era& a, const Era& b) { return a.season < b.season; });
}
static int s_build_year = 1961;
static std::string s_build_log;                        // the era being built: its log file
static std::string era_build_status() {
    if (s_build_log.empty()) return "";
    FILE* f = fopen(s_build_log.c_str(), "r"); if (!f) return "starting...";
    std::string all; char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n); fclose(f);
    while (!all.empty() && (all.back() == '\n' || all.back() == '\r')) all.pop_back();
    size_t nl = all.rfind('\n'); std::string last = nl == std::string::npos ? all : all.substr(nl + 1);
    if (last.find("league pack:") != std::string::npos) return "done: pick it above";
    if (size_t n = all.find("NEED: "); n != std::string::npos) return all.substr(n + 6, all.find('\n', n) - n - 6);
    if (all.find("Traceback") != std::string::npos || all.find("rror") != std::string::npos) return "failed: see " + s_build_log;
    return last.size() > 60 ? last.substr(0, 60) : last;
}
static void era_build_start() {
    std::error_code ec;
    std::string lahman;
    for (const char* c : {"lahman_1871-2025_csv", "lahman", "lahman_1871-2025_csv.zip"}) if (std::filesystem::exists(c, ec)) { lahman = c; break; }
    if (lahman.empty()) { toast("Put the Lahman database (lahman_1871-2025_csv.zip from sabr.org) in the game folder first"); return; }
    std::string iso = !s_launch_iso.empty() ? s_launch_iso : g_settings.iso;
    if (iso.empty()) { toast("Import your game disc first"); return; }
    std::filesystem::create_directories("build", ec);
    s_build_log = "build/era-" + std::to_string(s_build_year) + ".log";
    std::string cmd = "python3 tools/eras/build_era.py \"" + iso + "\" \"" + lahman + "\" " + std::to_string(s_build_year - 1) + " > \"" + s_build_log + "\" 2>&1 &";
#ifdef _WIN32
    cmd = "start /b " + cmd.substr(0, cmd.size() - 2);
#endif
    if (std::system(cmd.c_str()) != 0) toast("Could not start Python 3 (needed to build a season)");
    else toast("Building MLB " + std::to_string(s_build_year) + " (a few minutes)");
}
static void league_build_start() {                      // "My League": today's rosters with the Team Maker clubs in teams/
    std::error_code ec;
    std::string iso = !s_launch_iso.empty() ? s_launch_iso : g_settings.iso;
    if (iso.empty()) { toast("Import your game disc first"); return; }
    std::filesystem::create_directories("teams", ec); std::filesystem::create_directories("build", ec);
    s_build_log = "build/my-league.log";
    std::string cmd = "python3 tools/custom_league.py \"" + iso + "\" teams mods/pack-mine > \"" + s_build_log + "\" 2>&1 &";
#ifdef _WIN32
    cmd = "start /b " + cmd.substr(0, cmd.size() - 2);
#endif
    if (std::system(cmd.c_str()) != 0) toast("Could not start Python 3 (needed to build a league)");
    else toast("Building My League from the teams folder (a few minutes)");
}

static void build_pages() {
    Settings& S = g_settings;
    s_pages.clear();
    {   // Eras: start a dynasty in another time
        eras_scan();
        Page pg{"Leagues & Eras", "today, 2005, or a season from history", {}, 2};
        for (size_t i = 0; i < s_eras.size(); i++) {
            const Era e = s_eras[i];
            const bool cur = g_settings.rosters == 2 && g_settings.league == e.dir;
            Item it{IT_BUTTON, (cur ? "> " : "") + (e.name.empty() ? e.dir : e.name) + (cur ? " (chosen)" : ""), e.blurb + " Choose it, then start a New Dynasty."};
            it.get = [] { return 0; };
            it.set = [e](int) {
                g_settings.rosters = 2; g_settings.league = e.dir; settings_save(settings_path());
                { extern void frontend_rebuild_pages(); frontend_rebuild_pages(); }
                if (s_launcher) s_action = ACT_PLAY;
                else toast(e.name + " is chosen: quit and start the game again, then New Dynasty");
            };
            pg.items.push_back(it);
        }
        if (s_eras.empty()) pg.items.push_back(info("Seasons", [] { return std::string("none built yet: build one below"); }));
        {   // back to today
            const bool cur = g_settings.rosters == 1;
            Item it{IT_BUTTON, std::string(cur ? "> " : "") + "Today's rosters" + (cur ? " (chosen)" : ""), "The modern rosters (mods/rosters-2026): today's players, ratings, faces and portraits."};
            it.get = [] { return 0; };
            it.set = [](int) { g_settings.rosters = 1; settings_save(settings_path()); extern void frontend_rebuild_pages(); frontend_rebuild_pages();
                               if (s_launcher) s_action = ACT_PLAY; else toast("Today's rosters are chosen: quit and start the game again"); };
            pg.items.push_back(it);
        }
        {   // your own league
            std::error_code ec; const bool have = std::filesystem::exists("mods/pack-mine/pack.json", ec);
            const bool cur = have && g_settings.rosters == 2 && g_settings.league == "pack-mine";
            if (have) {
                Item it{IT_BUTTON, std::string(cur ? "> " : "") + "My League" + (cur ? " (chosen)" : ""), "Today's rosters with the clubs you made in the Team Maker. Choose it, then start a New Dynasty."};
                it.get = [] { return 0; };
                it.set = [](int) { g_settings.rosters = 2; g_settings.league = "pack-mine"; settings_save(settings_path()); extern void frontend_rebuild_pages(); frontend_rebuild_pages();
                                   if (s_launcher) s_action = ACT_PLAY; else toast("My League is chosen: quit and start the game again, then New Dynasty"); };
                pg.items.push_back(it);
            }
            Item b{IT_BUTTON, have ? "Rebuild My League" : "Build My League",
                   "Puts every club you made in the Team Maker (tools/teammaker/team_maker.html: Export saves a team .zip) into today's rosters: save the .zip files in the teams folder of the game folder, then press this. Each club takes the place of the slot you chose in the maker, with its name, colours, logos, uniforms and ballpark."};
            b.get = [] { return 0; }; b.set = [](int) { league_build_start(); }; pg.items.push_back(b);
        }
        pg.items.push_back(slider("Build another season", &s_build_year, 1901, 2025, 1, "",
            "Any season from 1901 to 2025, made from the Lahman Baseball Database (free from sabr.org: put lahman_1871-2025_csv.zip in the game folder). Needs Python 3 with numpy and Pillow; takes a few minutes. Before 1961 the majors had 16 clubs: the game's other 14 are named for minor-league clubs of the day (the Seattle Rainiers, Houston Buffaloes, Montreal Royals ...) with made-up players."));
        { Item b{IT_BUTTON, "Build it", "Starts building the season chosen above. It appears in this list when it is done."}; b.get = [] { return 0; }; b.set = [](int) { era_build_start(); }; pg.items.push_back(b); }
        pg.items.push_back(info("Build status", [] { std::string st = era_build_status(); if (st.rfind("done", 0) == 0 && s_eras.size() < 999) { static std::string seen; if (seen != s_build_log) { seen = s_build_log; extern void frontend_rebuild_pages(); frontend_rebuild_pages(); } } return st.empty() ? std::string("-") : st; }));
        s_pages.push_back(pg);
    }
    s_pages.push_back({"Home team", "your club's ballpark and colours, from the disc", {}, 1});
    const size_t first_setting = s_pages.size();
    s_pages.push_back({"Display", "window, aspect ratio and scaling", {
        choice("Aspect ratio", {"4:3 (original)", "16:9", "16:10", "21:9 ultrawide", "32:9 super ultrawide", "Match my display"},
               [&] { return S.aspect; }, [&](int v) { S.aspect = v; resize_window_to_aspect(); },
               "Anything wider than 4:3 turns on EA's hidden widescreen mode and widens the camera to the shape you pick: the field of view grows sideways, nothing is stretched. "
               "Match my display uses your monitor's shape (" + aspect_name(g_display_aspect) + " here). The game's 2D (score bug, menus) was laid out for 16:9: see HUD on ultrawide below."),
        choice("HUD on ultrawide", {"Stretched", "16:9 shape (experimental)"}, [&] { return (int)S.hud_169; }, [&](int v) { S.hud_169 = v != 0; },
               "Wider than 16:9 the game's score bug, meters and menus are stretched sideways like the rest of its 2D. 16:9 shape pulls them towards the middle so they keep their proportions; "
               "a few markers drawn over the field (the batting cursor) may then sit off their spot."),
        choice("Display mode", {"Window", "Fullscreen (borderless)", "Fullscreen (exclusive)"},
               [&] { return S.fullscreen ? (S.fs_exclusive ? 2 : 1) : 0; }, [&](int v) { S.fullscreen = v > 0; if (v) S.fs_exclusive = v == 2; },
               "Borderless covers the screen at your desktop resolution and switches instantly (F11). Exclusive takes over the display, which can lower input lag and lets the GPU drive the monitor directly."),
        choice("Monitor", display_names(), [&] { return std::min(S.display, std::max(0, SDL_GetNumVideoDisplays() - 1)); }, [&](int v) { S.display = v; },
               "Which screen the game uses for fullscreen (and where the window opens)."),
        choice("Window size", {"1x  (448 lines)", "2x  (896 lines)", "3x  (1344 lines)", "4x  (1792 lines)"},
               [&] { return std::min(3, S.window_scale - 1); }, [&](int v) { S.window_scale = v + 1; resize_window_to_aspect(); },
               "Size of the window when not in fullscreen; its width follows the aspect ratio. You can also drag the window's edges."),
        choice("Scaling filter", {"Nearest (pixelated)", "Bilinear (soft)", "Bicubic (sharp)", "Auto (best quality)"},
               [&] { return S.scale_filter; }, [&](int v) { S.scale_filter = v; },
               "How the picture is resized to the window. Auto averages pixels when shrinking a high-resolution image (supersampling) and uses bicubic when enlarging."),
        toggle("Integer scaling", &S.integer_scale, "Only whole-number zoom factors: perfectly even pixels, with black borders."),
        toggle("VSync", &S.vsync, "Wait for the monitor's refresh to avoid tearing. Caps the frame rate at the monitor's rate; turn it off for the lowest input lag."),
        choice("Interface size", {"Auto", "75%", "100%", "125%", "150%", "200%"},
               [&] { static const int m[] = {0, 75, 100, 125, 150, 200}; for (int i = 0; i < 6; i++) if (m[i] == S.ui_scale) return i; return 0; },
               [&](int v) { static const int m[] = {0, 75, 100, 125, 150, 200}; S.ui_scale = m[v]; }, "Size of this menu and the overlays."),
    }});
    s_pages.push_back({"Graphics", "resolution, anti-aliasing and sharpness", {
        choice("Quality preset", {"Custom", "Original (as on PS2)", "Performance", "Balanced", "Quality", "Remastered"},
               [&] { return preset_get(); }, [&](int v) { preset_set(v); },
               "Sets the options below and on the Textures page in one go. Performance: 2x resolution, no extras. Balanced: resolution to fit your window, FXAA, 8x anisotropic. "
               "Quality: 4x, FXAA, 16x anisotropic, the HD pack. Remastered: Quality plus high-quality FXAA, built-in upscaling for anything the pack lacks and a little sharpening. Original: what the console drew."),
        choice("Renderer", {"Software (CPU)", "GPU"}, [&] { return S.renderer; }, [&](int v) { S.renderer = v; },
               "GPU draws the game on your graphics card: much faster, especially at higher internal resolutions. Software is the reference renderer."),
        [&] { Item it = choice("Graphics API", api_options(), [&] { return S.gpu_api; }, [&](int v) { S.gpu_api = v; },
#if defined(__APPLE__)
               "What the GPU renderer and the window use. Auto: Metal. Metal draws GS blending and masks exactly; Vulkan runs through MoltenVK. Falls back to OpenGL when unavailable. Applies at the next start.");
#else
               "What the GPU renderer and the window use. Auto: OpenGL. Vulkan needs a Vulkan driver (most GPUs from 2016 on) and is often faster. Falls back to OpenGL when unavailable. Applies at the next start.");
#endif
               it.restart = true; return it; }(),
        choice("Internal resolution", {"Auto (fit the window)", "1x native (512 x 448)", "2x (1024 x 896)", "3x (1536 x 1344)", "4x (2048 x 1792)", "5x (2560 x 2240)", "6x (3072 x 2688)"},
               [&] { return S.internal_res; }, [&](int v) { S.internal_res = v; },
               "Renders the 3D scene and menus at a higher resolution: sharper edges and textures. Auto picks the one that matches your window's height (now " + std::to_string(g_auto_res) + "x). 5x and 6x need the GPU renderer; above your window's resolution they act as supersampling."),
        choice("Anti-aliasing", {"Off", "FXAA", "FXAA (high quality)"}, [&] { return S.aa; }, [&](int v) { S.aa = v; },
               "Smooths jagged edges after drawing. Combine with a higher internal resolution and Auto scaling for supersampling."),
        slider("Sharpening", &S.sharpen, 0, 100, 5, "%", "Contrast-adaptive sharpening (AMD CAS): brings back detail after scaling and FXAA."),
        choice("Texture filtering", {"Nearest (sharp)", "Bilinear (as on PS2)"}, [&] { return S.tex_filter; }, [&](int v) { S.tex_filter = v; },
               "Bilinear smooths textures where the game asks for it, like the real console."),
        choice("Anisotropic filtering", {"Off (as on PS2)", "2x", "4x", "8x", "16x"},
               [&] { int a = S.aniso; return a >= 16 ? 4 : a >= 8 ? 3 : a >= 4 ? 2 : a >= 2 ? 1 : 0; },
               [&](int v) { static const int m[] = {0, 2, 4, 8, 16}; S.aniso = m[v]; },
               "Keeps the field, the crowd and signs sharp at shallow angles instead of blurring them (GPU renderer). Uses mipmaps, so distant textures look smoother than on the PS2."),
        choice("Render threads", {"Auto", "Off (1 thread)", "2", "3", "4", "6", "8"},
               [&] { int t = S.gs_threads; return t < 0 ? 0 : t < 2 ? 1 : t == 2 ? 2 : t == 3 ? 3 : t == 4 ? 4 : t <= 6 ? 5 : 6; },
               [&](int v) { static const int m[] = {-1, 0, 2, 3, 4, 6, 8}; S.gs_threads = m[v]; },
               "CPU threads used for drawing. Auto uses all cores but one."),
    }});
    s_pages.back().items.back().restart = true;
    s_pages.push_back({"Textures", "HD upscaling and texture packs", {
        choice("HD upscaling", {"Off", "2x", "4x"}, [&] { return S.tex_enhance; }, [&](int v) { S.tex_enhance = v; },
               "Built-in texture enhancement: every texture the game filters is upscaled (bicubic + sharpening) in the background. Best with internal resolution 2x or higher."),
        toggle("Texture packs", &S.tex_replace, "Use replacement textures from textures/replace/ (the HD texture pack from Features). They take priority over HD upscaling."),
        info("Packs found", [] { size_t n = gs_replacement_count(); return n ? std::to_string(n) + " textures in textures/replace/" : std::string("none: add the HD texture pack in Features"); }),
        toggle("Dump textures", &S.tex_dump, "Saves every new texture to textures/dump/ as WIDTHxHEIGHT_hash.png. Edit or upscale them and put them in textures/replace/ with the same name to make a pack."),
    }});
    s_pages.push_back({"Image", "colour and retro effects", {
        slider("Brightness", &S.brightness, -50, 50, 5, "", "Adds or removes light evenly."),
        slider("Contrast", &S.contrast, 50, 150, 5, "%", "Distance between dark and bright."),
        slider("Saturation", &S.saturation, 0, 200, 5, "%", "Colour intensity. 0% is black and white."),
        slider("Gamma", &S.gamma, 50, 200, 5, "", "Brightens (above 100) or darkens mid-tones without clipping."),
        choice("Retro effect", {"Off", "Scanlines", "CRT (scanlines + curvature)"}, [&] { return S.crt; }, [&](int v) { S.crt = v; },
               "Imitates a TV of the era: scanlines at the original 448 lines, optional screen curvature and vignette."),
        button("Reset image settings", ACT_RESET_IMAGE, "Brightness, contrast, saturation and gamma back to neutral."),
    }});
    s_pages.push_back({"Frame rate", "speed, timing and the FPS counter", {
        choice("Frame rate limit", fps_options(), [&] { return fps_index(); }, [&](int v) { S.fps_limit = kFps[v]; },
               "How many frames per second the game runs at. Match my display uses your monitor's refresh rate (" + std::to_string(g_display_hz) + " Hz here). With real-time timing the game keeps its normal speed at any rate."),
        choice("Game timing", {"Classic (speed follows frame rate)", "Real-time (constant speed)"}, [&] { return S.sim_timing; }, [&](int v) { S.sim_timing = v; },
               "Real-time: the game's physics step follows the actual frame time, so 120 fps is smoother but not faster, and a slow PC doesn't cause slow motion. Classic: one fixed 1/60 s step per frame, as on the PS2. Some menu timers still count frames."),
        choice("Frame skip", {"Off", "Auto", "Draw 1 of 2", "Draw 1 of 3", "Draw 1 of 4"}, [&] { return S.frameskip; }, [&](int v) { S.frameskip = v; },
               "Auto skips drawing a frame only when the PC falls behind."),
        choice("FPS counter", {"Off", "Simple", "Detailed"}, [&] { return S.show_fps; }, [&](int v) { S.show_fps = v; },
               "Frame rate and game speed in a corner; Detailed adds a frame-time graph. F2 cycles it."),
        choice("Counter position", {"Top left", "Top right", "Bottom left", "Bottom right"}, [&] { return S.fps_corner; }, [&](int v) { S.fps_corner = v; },
               "Which corner of the picture the FPS counter sits in."),
        toggle("Pause in the background", &S.pause_unfocused, "Pauses the game and its sound while you are in another window, and picks up where it was when you come back."),
    }});
    s_pages.push_back({"Audio", "volume, soundtrack and your own music", {
        slider("Volume", &S.volume, 0, 100, 5, "%", "Master volume of the game's sound, music and commentary."),
        choice("Sound", {"On", "Muted"}, [&] { return S.mute ? 1 : 0; }, [&](int v) { S.mute = v != 0; },
               "Mutes all game audio. Sound is automatically silent while fast-forwarding."),
        toggle("Music in the launcher", &S.menu_music, "The launcher plays the soundtrack chosen below: the game's menu songs, streamed from your disc, or your own."),
        choice("Soundtrack", {"The game's songs", "My music", "Both, mixed"}, [&] { return S.music; }, [&](int v) { S.music = v; },
               "What plays in the game's menus. My music replaces the game's nine menu songs with songs you imported (more than nine: a different nine each start); the game's own song display and jukebox show their names. Both keeps some of the game's songs. Takes effect at the next start."),
        info("My music", [] { const auto& v = my_music(); if (v.empty()) return std::string("nothing imported yet");
                              double sec = 0; for (const MusicTrack& t : v) sec += t.seconds;
                              return std::to_string(v.size()) + (v.size() == 1 ? " song, " : " songs, ") + std::to_string((int)(sec / 60 + 0.5)) + " min"; }),
        button("Import music...", ACT_MUSIC_IMPORT,
#if defined(__APPLE__)
               "Choose songs from your Mac: MP3, M4A/AAC, FLAC, AIFF or WAV. You can also drag them onto this window. Each song is converted once to the game's own music format and kept in the music folder; name a file \"Artist - Title\" to get both shown in the game."),
#else
               "Choose songs: WAV always works; MP3, M4A, FLAC and OGG need ffmpeg installed. You can also drag them onto this window. Each song is converted once to the game's own music format and kept in the music folder; name a file \"Artist - Title\" to get both shown in the game."),
#endif
        button("Remove my music", ACT_MUSIC_CLEAR, "Deletes the converted copies in the music folder. Your original files are not touched."),
        info("Output", [] { extern const char* snd_status(); return std::string(snd_status()); }),
    }});
    s_pages.back().items[3].restart = true;
    s_pages.push_back({"Game", "rosters, uniforms, ballparks and CPU play", {
        toggle("Commentary for new players", &S.commentary, "With modern rosters and eras: the commentators say the surname of about 850 new players, using their real recordings of that surname (the same list the game uses for its generated rookies). Takes effect at the next start."),
        choice("Rosters", roster_options(), [&] { return roster_get(); }, [&](int v) { roster_set(v); },
               "Which player database the game loads. Modern uses mods/rosters-2026 (current MLB players and ratings); league packs (mods/pack-*, made with tools/leaguepack.py) bring their own clubs, players and first season. Takes effect the next time the game starts: save settings and restart. Franchise and roster saves on your memory card keep the rosters they were made with."),
        choice("CPU play", {"Original (2005)", "2026 rules"}, [&] { return S.ai; }, [&](int v) { S.ai = v; },
               "How computer-controlled teams play. 2026 rules: no extreme infield shifts (banned in 2023), a quicker pace between pitches (pitch clock), more stolen-base attempts by fast runners (bigger bases, limited pickoffs), fewer hit-and-run calls, and the designated hitter in every park (universal DH; the game's own DH Rule option can still turn it off). Pitching changes, pinch hitters and bunts are decided as in 2005. Uses mods/ai-2026. Under an era the rules follow the season: the DH as it was then (none before 1973, the AL's until 2022), earlier hooks only from 2015, more steals in the running years (1975-1992) and from 2023. Takes effect at the next start."),
        choice("Uniforms", {"Original (2005)", "Today's looks"}, [&] { return S.uniforms; }, [&](int v) { S.uniforms = v; },
               "Each club's default home and road uniforms become the closest match to what it wears now, from the game's own wardrobe: throwback sets that came back, new colours (Rays, Padres, Diamondbacks, Marlins) and jerseys that fit the club's name today. No new logos are drawn. Uses mods/uniforms-2026; left out under an era (the season's own uniforms are used). Takes effect at the next start."),
        choice("Ballparks", {"Original (2005)", "Today's names and fences"}, [&] { return S.stadiums; }, [&](int v) { S.stadiums = v; },
               "Today's park names, and the outfield reshaped to today's fence distances where the fences moved or the club moved to a new park. The buildings are still the ones the game shipped with. Uses mods/stadiums-2026; left out under an era (the season's own parks are used). Takes effect at the next start."),
        choice("Simulated games", {"Original (2005)", "Real league of each season"}, [&] { return S.sim_league; }, [&](int v) { S.sim_league = v; },
               "How simulated games play out. Real league: strikeouts, walks, hit-by-pitches, singles, doubles, triples and home runs come out at the rates the major leagues had in the season being played (1977 plays like 1977, 2027 like today's game), steered by the season's own statistics; who is good at what still comes from the ratings. Original: the game's own numbers, about the same league in every year. Takes effect at the next start."),
        toggle("Varied head shapes", &S.head_shapes, "The game's 15 generic heads differ mostly in their skin and features; with this on each also has its own jaw width, cheekbones, lower-face length and chin (the cranium and neck are as they were, so caps and helmets still fit). Uses mods/heads (tools/heads/head_shapes.py). Takes effect at the next start."),
        toggle("Player portraits", &S.portraits, "Portraits rendered from each player's own face and uniform, in place of the generic heads: the modern rosters' (mods/portraits-2026) and each era's own. Takes effect at the next start."),
        info(s_launcher ? "Files" : "Loaded", [] {
            if (s_launcher) { std::error_code ec; bool r = std::filesystem::is_directory("mods/rosters-2026", ec), c = std::filesystem::is_directory("mods/commentary-2026", ec);
                              return std::string(r ? "rosters found" : "mods/rosters-2026 missing") + (r ? (c ? ", commentary found" : ", commentary missing") : ""); }
            extern std::string g_roster_status; return g_roster_status; }),
    }});
    s_pages.push_back({"Controls", "keyboard and gamepad", {
        info("D-pad", [] { return std::string("Arrow keys  |  D-pad"); }),
        info("Left stick", [] { return std::string("I J K L  |  Left stick"); }),
        info("Cross / Circle", [] { return std::string("Z / X  |  A / B"); }),
        info("Square / Triangle", [] { return std::string("A / S  |  X / Y"); }),
        info("L1 R1 / L2 R2", [] { return std::string("Q W / 1 2  |  Bumpers / Triggers"); }),
        info("Start / Select", [] { return std::string("Enter / Right Shift  |  Start / Back"); }),
        info("Menu", [] { return std::string("F1 or Esc  |  Back + Start or Guide"); }),
        info("Fast forward", [] { return std::string("Hold Tab"); }),
        info("Screenshot / FPS", [] { return std::string("F12 / F2"); }),
    }});
    s_pages.push_back({"System", "save, reset and quit", {
        button("Save settings", ACT_SAVE, "Writes all settings to mvp2005.ini next to your memory cards."),
        toggle("Intro movies", &S.intro_movies, "The EA Sports logo and the game's minute-long intro movie at every start. Off: straight to the title screen (the movie is still in EA Sports Extras). Applies at the next start."),
        toggle("Show the launcher at start", &S.launcher, "Off: the game starts straight away next time. Hold Shift while starting the game (or run it with --launcher) to get the launcher again."),
        toggle("Check for updates", &S.check_updates, "When the launcher starts it asks the download page whether there is a newer version of the game or of an installed feature, and says so on the start screen. Nothing is downloaded or changed without you choosing it."),
        info("Version", [] { GameUpdate u; return std::string(game_version()) + (update_available(&u) ? "  \xC2\xB7  version " + u.version + " is available" : std::string()); }),
        button("Reset all settings", ACT_RESET, "Restores every setting to its default."),
        button("Quit game", ACT_QUIT, "Closes the game. Unsaved progress in the game is lost."),
        info("Renderer", [] { return std::string(s_pres ? s_pres->name() : "none"); }),
    }});
    if (!s_launcher) s_pages.back().items.insert(s_pages.back().items.begin() + 1,
        button("Take screenshot", ACT_SHOT, "Saves the game picture as a PNG in the current folder (F12)."));
    for (size_t i = first_setting; i < s_pages.size(); i++) {
        Page& p = s_pages[i]; p.group = 1;
        p.preview = p.name == "Display" || p.name == "Graphics" || p.name == "Textures" || p.name == "Image";
    }
}

static bool s_pages_dirty = false;
void frontend_rebuild_pages() { s_pages_dirty = true; }        // done between frames (the pages are being drawn now)

// ---------------------------------------------------------------- layout helpers
struct Hit { float x, y, w, h; int kind, a, b; };   // kind: 0 sidebar page, 1 item row, 2 item left arrow, 3 item right arrow, 4 slider track, 5 close
static std::vector<Hit> s_hits;
static float s_scale = 1;
static bool inside(const Hit& h, float x, float y) { return x >= h.x && y >= h.y && x < h.x + h.w && y < h.y + h.h; }

static float ui_scale_for(int dw, int dh) {
    float s = std::min(dw / 1500.0f, dh / 900.0f);
    s = std::max(0.55f, std::min(s, 3.0f));
    if (g_settings.ui_scale) s *= g_settings.ui_scale / 100.0f;
    return s;
}

// ---------------------------------------------------------------- overlays (FPS, toast)
static double s_fps = 0, s_speed = 0, s_drawn_pct = 100;
static float s_ft_hist[120]; static int s_ft_pos = 0;
static void update_fps() {
    static u64 t0 = 0, f0 = 0, v0 = 0, d0 = 0, k0 = 0, lastf = 0, lastt = 0;
    u64 t = host_time_ns();
    if (g_game_frames != lastf) {
        if (lastt) { s_ft_hist[s_ft_pos++ % 120] = (float)((t - lastt) / 1e6 / std::max<u64>(1, g_game_frames - lastf)); }
        lastf = g_game_frames; lastt = t;
    }
    if (!t0) { t0 = t; f0 = g_game_frames; v0 = kernel_vblank_count(); d0 = g_frames_drawn; k0 = g_frames_skipped; return; }
    if (t - t0 < 500000000ull) return;
    double dt = (t - t0) / 1e9;
    s_fps = (g_game_frames - f0) / dt;
    extern u32 g_sim_dt_bits[2];
    float step; std::memcpy(&step, &g_sim_dt_bits[0], 4);
    s_speed = s_fps * step * 100.0;                   // game time per real second (sim step x frames)
    u64 dd = g_frames_drawn - d0, dk = g_frames_skipped - k0;
    s_drawn_pct = dd + dk ? 100.0 * dd / (dd + dk) : 100;
    t0 = t; f0 = g_game_frames; v0 = kernel_vblank_count(); d0 = g_frames_drawn; k0 = g_frames_skipped;
}
static void draw_toast(int dw, int dh, float s) {
    if (!s_toast.empty()) {
        double age = (host_time_ns() - s_toast_t) / 1e9;
        if (age < 3.2) {
            float a = (float)std::min(1.0, std::min(age / 0.15, (3.2 - age) / 0.4));
            float w = ui_text_width(s_toast, 18 * s) + 40 * s, h = 44 * s;
            float x = (dw - w) / 2, y = dh - h - 36 * s + (1 - a) * 12 * s;
            s_ui.shadow(x, y + 6 * s, w, h, 22 * s, 18 * s, rgba(0, 0, 0, (u8)(140 * a)));
            s_ui.rect(x, y, w, h, with_alpha(rgba(14, 19, 28, 240), a), 22 * s);
            s_ui.outline(x, y, w, h, with_alpha(rgba(255, 255, 255, 40), a), 22 * s, 1 * s);
            s_ui.text(x + 20 * s, y + (h - ui_line_height(18 * s)) / 2, s_toast, 18 * s, with_alpha(C_TEXT, a));
        }
    }
}
static u64 s_boot_t = 0;                             // when the game was started (the boot spinner)
static void draw_overlays(int dw, int dh) {
    float s = ui_scale_for(dw, dh);
    if (s_boot_t) {                                       // from Play Ball until the game shows its first picture
        static u64 first_frame_t = 0;
        if (!first_frame_t && g_game_frames > 2 && gs_display_version() > 2) first_frame_t = host_time_ns();
        float a = first_frame_t ? 1 - (float)((host_time_ns() - first_frame_t) / 1e9 / 0.35) : std::min(1.0f, (float)((host_time_ns() - s_boot_t) / 1e9 / 0.3));
        if (a <= 0) s_boot_t = 0;
        else {
            const float r = 13 * s, cx = dw - 46 * s, cy = dh - 46 * s; const double t = host_time_ns() / 1e9;
            for (int k = 0; k < 10; k++) {
                float ang = (float)(k * 6.2831853 / 10), ph = (float)std::fmod(t * 1.4 - k / 10.0 + 10, 1.0);
                s_ui.rect(cx + std::cos(ang) * r - 2.2f * s, cy + std::sin(ang) * r - 2.2f * s, 4.4f * s, 4.4f * s, rgba(242, 245, 248, (u8)(255 * a * (0.15f + 0.85f * (1 - ph)))), 2.2f * s);
            }
            const std::string m = "Starting the game";
            s_ui.text(cx - r - 14 * s - ui_text_width(m, 15 * s), cy - ui_line_height(15 * s) / 2, m, 15 * s, rgba(154, 167, 184, (u8)(255 * a)));
        }
    }
    if (g_settings.show_fps) {
        char a[32], b[96];
        snprintf(a, sizeof a, "%.0f", s_fps);
        snprintf(b, sizeof b, "FPS  |  speed %.0f%%%s", s_speed, s_drawn_pct < 99.5 ? "  |  skipping" : "");
        bool det = g_settings.show_fps >= 2;
        float w = std::max(ui_text_width(a, 30 * s, FONT_BOLD) + ui_text_width(b, 15 * s) + 40 * s, det ? 300 * s : 0.0f);
        float h = det ? 128 * s : 48 * s, x = 14 * s, y = 14 * s;
        if (g_settings.fps_corner & 1) x = dw - w - 14 * s;
        if (g_settings.fps_corner & 2) y = dh - h - 14 * s;
        s_ui.shadow(x, y + 4 * s, w, h, 12 * s, 14 * s, rgba(0, 0, 0, 120));
        s_ui.rect(x, y, w, h, rgba(9, 13, 21, 205), 12 * s);
        s_ui.outline(x, y, w, h, rgba(255, 255, 255, 30), 12 * s, 1 * s);
        u32 col = s_speed > 95 ? C_OK : s_speed > 80 ? C_ACCENT2 : C_ACCENT;
        float aw = s_ui.text(x + 14 * s, y + 5 * s, a, 30 * s, col, FONT_BOLD);
        s_ui.text(x + 22 * s + aw, y + 16 * s, b, 15 * s, C_DIM);
        if (det) {
            float gx = x + 14 * s, gy = y + 54 * s, gw = w - 28 * s, gh = 44 * s;
            s_ui.rect(gx, gy, gw, gh, rgba(255, 255, 255, 14), 4 * s);
            float target = 1000.0f / (settings_fps_limit() ? settings_fps_limit() : 60);
            s_ui.rect(gx, gy + gh - gh * std::min(1.0f, target / 50.0f), gw, 1 * s, rgba(255, 255, 255, 60));
            float bw = gw / 120.0f;
            float worst = 0;
            for (int i = 0; i < 120; i++) {
                float ft = s_ft_hist[(s_ft_pos + i) % 120];
                worst = std::max(worst, ft);
                float bh = std::min(1.0f, ft / 50.0f) * gh;
                s_ui.rect(gx + i * bw, gy + gh - bh, std::max(1.0f, bw - 1), bh, ft > target * 1.5f ? with_alpha(C_ACCENT, 0.9f) : with_alpha(C_OK, 0.8f));
            }
            char c[128];
            extern int g_effective_hz;
            snprintf(c, sizeof c, "frame %.1f ms  (worst %.1f)   %d Hz   %dx res   %s", s_ft_hist[(s_ft_pos + 119) % 120], worst, g_effective_hz, settings_internal_res(g_settings.renderer == 1), s_pres ? s_pres->name() : "");
            s_ui.text(gx, gy + gh + 6 * s, c, 13 * s, C_DIM);
        }
    }
    if (g_fast_forward) {
        std::string t = ">>  Fast forward";
        float w = ui_text_width(t, 18 * s, FONT_BOLD) + 28 * s;
        s_ui.rect(dw - w - 14 * s, 14 * s, w, 36 * s, rgba(9, 13, 21, 205), 18 * s);
        s_ui.outline(dw - w - 14 * s, 14 * s, w, 36 * s, rgba(255, 255, 255, 30), 18 * s, 1 * s);
        s_ui.text(dw - w, 20 * s, t, 18 * s, C_ACCENT2, FONT_BOLD);
    }
    draw_toast(dw, dh, s);
}

// ---------------------------------------------------------------- theme: a hand-operated ballpark scoreboard
// Painted steel plates in wall green, chalk-white lettering, lamps for state (green on, amber selected, red play).
static const u32 C_BOARD = rgba(23, 56, 43), C_RAIL = rgba(13, 34, 26), C_SLOT = rgba(9, 24, 18), C_ROW_SEL = rgba(36, 80, 62), C_ROW_HOV = rgba(29, 67, 52);
static const u32 C_CHALK = rgba(243, 239, 226), C_SLATE = rgba(152, 176, 163), C_RULE = rgba(58, 100, 80);
static const u32 C_AMBER = rgba(245, 184, 46), C_LAMP_RED = rgba(222, 56, 46), C_LAMP_GREEN = rgba(84, 208, 112), C_LAMP_OFF = rgba(40, 70, 57);

// ---------------------------------------------------------------- game art (from the user's disc)
// 30 clubs in the game's own order (the order of its logo files and of team.dat)
static const char* const kTeamCode[30] = {"ana", "oak", "sea", "tex", "cws", "cle", "det", "kc", "min", "bal", "bos", "nyy", "tb", "tor", "ari",
                                          "col", "la", "sd", "sf", "chc", "cin", "hou", "mil", "pit", "stl", "atl", "fla", "rfk", "nym", "phi"};
static const char* const kTeam2005[30] = {"Anaheim Angels", "Oakland Athletics", "Seattle Mariners", "Texas Rangers", "Chicago White Sox", "Cleveland Indians",
    "Detroit Tigers", "Kansas City Royals", "Minnesota Twins", "Baltimore Orioles", "Boston Red Sox", "New York Yankees", "Tampa Bay Devil Rays", "Toronto Blue Jays",
    "Arizona Diamondbacks", "Colorado Rockies", "Los Angeles Dodgers", "San Diego Padres", "San Francisco Giants", "Chicago Cubs", "Cincinnati Reds", "Houston Astros",
    "Milwaukee Brewers", "Pittsburgh Pirates", "St. Louis Cardinals", "Atlanta Braves", "Florida Marlins", "Washington Nationals", "New York Mets", "Philadelphia Phillies"};
static const char* const kTeam2026[30] = {"Los Angeles Angels", "Athletics", "Seattle Mariners", "Texas Rangers", "Chicago White Sox", "Cleveland Guardians",
    "Detroit Tigers", "Kansas City Royals", "Minnesota Twins", "Baltimore Orioles", "Boston Red Sox", "New York Yankees", "Tampa Bay Rays", "Toronto Blue Jays",
    "Arizona Diamondbacks", "Colorado Rockies", "Los Angeles Dodgers", "San Diego Padres", "San Francisco Giants", "Chicago Cubs", "Cincinnati Reds", "Houston Astros",
    "Milwaukee Brewers", "Pittsburgh Pirates", "St. Louis Cardinals", "Atlanta Braves", "Miami Marlins", "Washington Nationals", "New York Mets", "Philadelphia Phillies"};
static const char* team_name(int t) { return t < 0 || t >= 30 ? "No team" : g_settings.rosters == 1 ? kTeam2026[t] : kTeam2005[t]; }

enum { IMG_LOGO = 0, IMG_PARK = 1, IMG_PHOTO = 2, IMG_WORD = 8, IMG_BLUR = 48 };      // IMG_WORD + team, IMG_BLUR + a picture
static bool s_art_tried = false, s_img_ok[kUiImages];
static float s_img_aspect[kUiImages];
static u32 s_team_col[30][2];                       // primary, secondary (from the game's team colour bars)
static int s_park_team = -2;
static std::string s_art_iso;
// a soft copy of a picture for the frosted glass: a 1/8 box-filtered image, blurred twice more (the GPU's bilinear
// filtering smooths the rest)
static void art_blur(int id, const ArtImage& im) {
    const int f = 6, w = std::max(1, im.w / f), h = std::max(1, im.h / f);
    std::vector<float> c((size_t)w * h * 3, 0);
    for (int y = 0; y < h * f && y < im.h; y++) for (int x = 0; x < w * f && x < im.w; x++) {
        u32 p = im.px[(size_t)y * im.w + x]; float* d = &c[((size_t)(y / f) * w + x / f) * 3];
        d[0] += p & 255; d[1] += (p >> 8) & 255; d[2] += (p >> 16) & 255;
    }
    for (float& v : c) v /= f * f;
    for (int pass = 0; pass < 6; pass++) {                       // 3x3 box, six times (about a gaussian)
        std::vector<float> o(c.size());
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int k = 0; k < 3; k++) {
            float sum = 0; int n = 0;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) { int xx = x + dx, yy = y + dy; if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue; sum += c[((size_t)yy * w + xx) * 3 + k]; n++; }
            o[((size_t)y * w + x) * 3 + k] = sum / n;
        }
        c.swap(o);
    }
    std::vector<u32> px((size_t)w * h);
    for (size_t i = 0; i < px.size(); i++) px[i] = 0xff000000u | (u32)c[i * 3 + 2] << 16 | (u32)c[i * 3 + 1] << 8 | (u32)c[i * 3];
    s_pres->set_ui_image(IMG_BLUR + id, px.data(), w, h); s_img_ok[IMG_BLUR + id] = true; s_img_aspect[IMG_BLUR + id] = (float)im.w / im.h;
}
static std::vector<u32> s_park_px; static int s_park_w = 0, s_park_h = 0;   // the park picture (the launcher's preview)
static bool art_put(int id, const char* big, const char* file, const char* shape, ArtImage* keep = nullptr) {
    ArtImage im;
    if (!game_art_load(big, file, shape, im)) return false;
    s_pres->set_ui_image(id, im.px.data(), im.w, im.h);
    s_img_ok[id] = true; s_img_aspect[id] = (float)im.w / im.h;
    if (id >= IMG_PARK && id < IMG_PHOTO + 3) art_blur(id, im);
    if (id == IMG_PARK || (id == IMG_PHOTO && s_park_px.empty())) {               // cut to 16:9 for the preview card
        int w = im.w, h = std::min(im.h, im.w * 9 / 16);
        if (im.w * 9 > im.h * 16) { h = im.h; w = im.h * 16 / 9; }
        int x0 = (im.w - w) / 2, y0 = (im.h - h) / 2;
        s_park_px.resize((size_t)w * h); s_park_w = w; s_park_h = h;
        for (int y = 0; y < h; y++) std::memcpy(&s_park_px[(size_t)y * w], &im.px[(size_t)(y + y0) * im.w + x0], (size_t)w * 4);
    }
    if (id == IMG_PARK && s_launcher && s_pres) s_pres->set_game(s_park_px.data(), s_park_w, s_park_h);
    if (keep) *keep = std::move(im);
    return true;
}
static int local_hour() { time_t t = time(nullptr); struct tm* lt = localtime(&t); return lt ? lt->tm_hour : 12; }
// the home team's ballpark, as the game's team-select screen shows it: day or night by the clock
static void art_park(int team) {
    if (team == s_park_team) return;
    s_park_team = team; s_img_ok[IMG_PARK] = false;
    if (team < 0 || team >= 30 || !s_pres || s_art_iso.empty() || !game_art_open(s_art_iso.c_str())) return;
    bool night = local_hour() >= 18 || local_hour() < 6;
    std::string day = std::string(kTeamCode[team]) + ".ssh", nite = std::string(kTeamCode[team]) + "n.ssh";
    if (!art_put(IMG_PARK, "DATA/FRONTEND/STADIUMS.BIG", (night ? nite : day).c_str(), ""))
        art_put(IMG_PARK, "DATA/FRONTEND/STADIUMS.BIG", (night ? day : nite).c_str(), "");
    game_art_close();
}
static void art_load(const std::string& iso) {
    if (s_art_tried || !s_pres || iso.empty()) return;
    if (!game_art_open(iso.c_str())) return;
    s_art_tried = true; s_art_iso = iso;
    art_put(IMG_LOGO, "DATA/FRONTEND/FEONLY.BIG", "mainart.ssh", "tp03");
    static const char* const photos[3] = {"22bd.ssh", "40bd.ssh", "00br.ssh"};            // ballpark at night, the diamond, ball on the mound
    for (int k = 0; k < 3; k++) art_put(IMG_PHOTO + k, "DATA/FRONTEND/BKGNDS.BIG", photos[k], "");
    int marks = 0;
    for (int t = 0; t < 30; t++) {
        char file[16], shape[8]; snprintf(file, sizeof file, "l%03d.ssh", t);
        snprintf(shape, sizeof shape, "l%03d", t);
        if (art_put(IMG_WORD + t, "DATA/FRONTEND/LOGOS.BIG", file, shape)) marks++;
        ArtImage bar; snprintf(shape, sizeof shape, "e%03d", t);
        s_team_col[t][0] = s_team_col[t][1] = 0;
        if (game_art_load("DATA/FRONTEND/LOGOS.BIG", file, shape, bar)) {                 // two most used opaque colours
            u32 col[8]; int cnt[8]; int n = 0;
            for (u32 p : bar.px) {
                if ((p >> 24) < 200) continue;
                u32 c = rgba((u8)p, (u8)(p >> 8), (u8)(p >> 16)); int k = 0;
                for (; k < n; k++) if (col[k] == c) { cnt[k]++; break; }
                if (k == n && n < 8) { col[n] = c; cnt[n++] = 1; }
            }
            for (int pass = 0; pass < 2; pass++) { int best = -1; for (int k = 0; k < n; k++) if (cnt[k] > 0 && (best < 0 || cnt[k] > cnt[best])) best = k;
                                                   if (best >= 0) { s_team_col[t][pass] = col[best]; cnt[best] = 0; } }
            if (!s_team_col[t][1]) s_team_col[t][1] = s_team_col[t][0];
        }
    }
    game_art_close();
    rlog("ui", "art from the disc: logo %s, %d team wordmarks, %d menu photos", s_img_ok[IMG_LOGO] ? "yes" : "no", marks,
         (int)s_img_ok[IMG_PHOTO] + s_img_ok[IMG_PHOTO + 1] + s_img_ok[IMG_PHOTO + 2]);
    art_park(g_settings.team);
}
// image drawn to cover x,y,w,h (cropped to fit); zoom >= 1, focus = centre of the crop in 0..1
static void art_cover(int id, float x, float y, float w, float h, float zoom, float fx, float fy, u32 tint, float radius = 0) {
    float ia = s_img_aspect[id], ra = w / h, uw = 1, vh = 1;
    if (ra > ia) vh = ia / ra; else uw = ra / ia;
    uw /= zoom; vh /= zoom;
    float u0 = std::min(1 - uw, std::max(0.0f, fx - uw / 2)), v0 = std::min(1 - vh, std::max(0.0f, fy - vh / 2));
    s_ui.image(id, x, y, w, h, u0, v0, u0 + uw, v0 + vh, tint, radius);
}
static float luminance(u32 c) { return (0.2126f * ((c >> 16) & 255) + 0.7152f * ((c >> 8) & 255) + 0.0722f * (c & 255)) / 255.0f; }
static u32 lighten(u32 c, float k) { auto f = [&](int v) { return (u32)std::min(255.0f, v + (255 - v) * k); };
                                     return (c & 0xff000000u) | f((c >> 16) & 255) << 16 | f((c >> 8) & 255) << 8 | f(c & 255); }
// colours for the plate under PLAY and the stripes: the home team's, or scoreboard red
static void theme_colours(u32& primary, u32& secondary, u32& on_primary) {
    int t = g_settings.team;
    if (t >= 0 && t < 30 && s_team_col[t][0]) { primary = s_team_col[t][0]; secondary = s_team_col[t][1]; }
    else { primary = C_LAMP_RED; secondary = C_CHALK; }
    if (luminance(primary) < 0.10f) primary = lighten(primary, 0.22f);       // very dark navy / black still has to read on the board
    on_primary = luminance(primary) > 0.55f ? rgba(18, 26, 22) : C_CHALK;
}

#include "frontend_menu.inc"
#include "frontend_setup.inc"

// ---------------------------------------------------------------- video
static DstRect game_rect(int dw, int dh) {
    const double aspect = settings_aspect();
    int w, h;
    if (g_settings.integer_scale && s_game_h > 0) {
        // scale the native 448-line picture by a whole number (use the native height even at higher internal res)
        int native_h = 448;
        int k = std::max(1, dh / native_h);
        h = native_h * k; w = (int)(h * aspect + 0.5);
        while (w > dw && k > 1) { k--; h = native_h * k; w = (int)(h * aspect + 0.5); }
        if (w > dw) { w = dw; h = (int)(dw / aspect); }
    } else {
        w = dw; h = (int)(dw / aspect);
        if (h > dh) { h = dh; w = (int)(dh * aspect); }
    }
    return {(dw - w) / 2, (dh - h) / 2, w, h};
}
static PostParams post_params() {
    const Settings& S = g_settings;
    PostParams p;
    p.scale_filter = S.integer_scale && S.internal_res == 1 ? 0 : S.scale_filter;
    p.aa = S.aa; p.sharpen = S.sharpen / 100.0f;
    p.brightness = S.brightness / 100.0f; p.contrast = S.contrast / 100.0f; p.saturation = S.saturation / 100.0f;
    p.gamma = S.gamma / 100.0f; p.crt = S.crt; p.native_lines = 448;
    return p;
}
// the desktop the window is on: its shape (Match my display) and refresh rate (frame rate Match my display)
static void update_display_info() {
    int di = s_win ? SDL_GetWindowDisplayIndex(s_win) : 0; if (di < 0) di = 0;
    SDL_DisplayMode m{};
    if (SDL_GetDesktopDisplayMode(di, &m) == 0 && m.w > 0 && m.h > 0) {
        const double a = (double)m.w / m.h; const int hz = m.refresh_rate > 0 ? m.refresh_rate : 60;
        if (std::fabs(a - g_display_aspect) > 1e-6 || hz != g_display_hz) { g_display_aspect = a; g_display_hz = hz; settings_apply(); }
    }
}
static void resize_window_to_aspect() {
    if (!s_win || g_settings.fullscreen || (SDL_GetWindowFlags(s_win) & SDL_WINDOW_MAXIMIZED)) return;
    const int sc = g_settings.window_scale, wh = 448 * sc; int ww = (int)(wh * settings_aspect() + 0.5);
    SDL_Rect b{}; int di = SDL_GetWindowDisplayIndex(s_win);
    if (SDL_GetDisplayUsableBounds(di < 0 ? 0 : di, &b) == 0 && b.w > 0) {            // keep it on the screen
        const double k = std::min(1.0, std::min((b.w - 40) / (double)ww, (b.h - 60) / (double)wh));
        ww = (int)(ww * k); int h2 = (int)(wh * k); SDL_SetWindowSize(s_win, ww, h2);
    } else SDL_SetWindowSize(s_win, ww, wh);
}
static void apply_video() {
    const int nd = std::max(1, SDL_GetNumVideoDisplays());
    const int disp = std::min(g_settings.display, nd - 1);
    if (disp != s_cur_display) {                                              // move to the chosen monitor
        if (s_cur_display >= 0) {
            if (s_cur_fullscreen) SDL_SetWindowFullscreen(s_win, 0);
            SDL_SetWindowPosition(s_win, SDL_WINDOWPOS_CENTERED_DISPLAY(disp), SDL_WINDOWPOS_CENTERED_DISPLAY(disp));
            s_cur_fullscreen = false;
        }
        s_cur_display = disp; update_display_info();
    }
    if (g_settings.fullscreen != s_cur_fullscreen || (g_settings.fullscreen && g_settings.fs_exclusive != s_cur_excl)) {
        if (g_settings.fullscreen && g_settings.fs_exclusive) {
            SDL_DisplayMode m{};
            if (SDL_GetDesktopDisplayMode(disp, &m) == 0) SDL_SetWindowDisplayMode(s_win, &m);
            if (s_cur_fullscreen) SDL_SetWindowFullscreen(s_win, 0);
            SDL_SetWindowFullscreen(s_win, SDL_WINDOW_FULLSCREEN);
        } else {
            if (s_cur_fullscreen && s_cur_excl) SDL_SetWindowFullscreen(s_win, 0);
            SDL_SetWindowFullscreen(s_win, g_settings.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        }
        s_cur_fullscreen = g_settings.fullscreen; s_cur_excl = g_settings.fs_exclusive;
    }
    if (g_settings.vsync != s_cur_vsync) { s_pres->set_vsync(g_settings.vsync); s_cur_vsync = g_settings.vsync; }
    settings_apply();                    // widescreen flag, frame rate
}
static void screenshot_window(const char* path) {
    int dw, dh; s_pres->drawable_size(dw, dh);
    std::vector<u32> px; int w, h;
    if (!s_pres->read_window(px, w, h)) return;
    extern bool gs_write_png(const char*, const std::vector<u32>&, int, int);
    gs_write_png(path, px, w, h);
}
static void present(bool menu) {
    extern bool gs_get_display_gpu(uintptr_t& fbo, int& x0, int& y0, int& w, int& h);
    uintptr_t gfbo; int gx, gy, gw, gh;
    static bool shown_gpu = false;
    if (s_launcher) {}
    else if (gs_get_display_gpu(gfbo, gx, gy, gw, gh) && s_pres->set_game_gpu(gfbo, gx, gy, gw, gh)) {
        s_game_w = gw; s_game_h = gh; s_shown_version = gs_display_version(); shown_gpu = true;
    } else if (gs_display_version() != s_shown_version || s_px.empty() || shown_gpu) {
        shown_gpu = false;
        int w, h;
        if (gs_get_display(s_px, w, h)) { s_pres->set_game(s_px.data(), w, h); s_game_w = w; s_game_h = h; s_shown_version = gs_display_version(); }
    }
    int dw, dh; s_pres->drawable_size(dw, dh);
    {   // Auto internal resolution: the multiple of 448 lines nearest the picture's height in the window
        const DstRect gr = game_rect(dw, dh);
        g_auto_res = std::max(1, std::min(6, (int)std::lround(gr.h / 448.0)));
    }
    s_ui.clear();
    if (menu) { draw_menu(dw, dh); draw_toast(dw, dh, s_scale); } else draw_overlays(dw, dh);
    DstRect r = game_rect(dw, dh);
    if (menu && s_launcher) r = s_preview.w > 0 ? s_preview : DstRect{0, 0, 0, 0};           // the park picture, in its card
    else if (menu && s_preview_k > 0.01f && s_last_preview.w > 0) {                            // the game glides into the card
        const DstRect& p = s_preview.w > 0 ? s_preview : s_last_preview; const float k = s_preview_k;
        r = {(int)(r.x + (p.x - r.x) * k), (int)(r.y + (p.y - r.y) * k), (int)(r.w + (p.w - r.w) * k), (int)(r.h + (p.h - r.h) * k)};
    }
    extern double g_gpu_t_present;
    const auto tp = std::chrono::steady_clock::now();
    s_pres->frame(dw, dh, r, post_params(), s_ui);
    g_gpu_t_present += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tp).count();
}

// ---------------------------------------------------------------- init
void frontend_init() {
    if (g_headless) return;
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL init failed (%s) - running headless\n", SDL_GetError());
        g_headless = true;
        return;
    }
    {   // the shape of the desktop first: Match my display and the window's size depend on it
        SDL_DisplayMode m{}; const int di = std::min(g_settings.display, std::max(0, SDL_GetNumVideoDisplays() - 1));
        if (SDL_GetDesktopDisplayMode(di, &m) == 0 && m.w > 0 && m.h > 0) { g_display_aspect = (double)m.w / m.h; if (m.refresh_rate > 0) g_display_hz = m.refresh_rate; }
        settings_apply();
    }
    int sc = g_settings.window_scale;
    int wh = 448 * sc, ww = (int)(wh * settings_aspect() + 0.5);
    {   // fits the screen it opens on
        SDL_Rect b{}; const int di = std::min(g_settings.display, std::max(0, SDL_GetNumVideoDisplays() - 1));
        if (SDL_GetDisplayUsableBounds(di, &b) == 0 && b.w > 0) { const double k = std::min(1.0, std::min((b.w - 40) / (double)ww, (b.h - 60) / (double)wh)); ww = (int)(ww * k); wh = (int)(wh * k); }
    }
    // OpenGL 3.3 core (macOS: 3.2+ forward-compatible core gives 4.1)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    // graphics API: settings (Auto = Metal on Apple, else OpenGL); MVP_GPU_API=gl|metal|vulkan overrides
    int api = g_settings.gpu_api;
    if (const char* e = std::getenv("MVP_GPU_API")) api = !std::strcmp(e, "metal") ? 2 : !std::strcmp(e, "vulkan") ? 3 : !std::strcmp(e, "gl") ? 1 : 0;
#ifdef __APPLE__
    if (api == 0) api = 2;
#else
    if (api == 0 || api == 2) api = 1;
#endif
    const Uint32 wflags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    const int wpos = SDL_WINDOWPOS_CENTERED_DISPLAY(std::min(g_settings.display, std::max(0, SDL_GetNumVideoDisplays() - 1)));
#ifdef __APPLE__
    if (api == 2) {
        s_win = SDL_CreateWindow("MVP Baseball 2005", wpos, wpos, ww, wh, wflags | SDL_WINDOW_METAL);
        if (s_win && (s_pres = presenter_metal_create(s_win))) g_gpu_api = GPU_API_METAL;
        else if (s_win) { SDL_DestroyWindow(s_win); s_win = nullptr; }
        else fprintf(stderr, "[metal] no window (%s) - using OpenGL\n", SDL_GetError());
    }
#endif
#ifdef MVP_VULKAN
    if (api == 3 && !s_pres) {
#ifdef __APPLE__
        const Uint32 vkflag = SDL_WINDOW_METAL;            // MoltenVK draws into the window's CAMetalLayer
#else
        const Uint32 vkflag = SDL_WINDOW_VULKAN;
#endif
        s_win = SDL_CreateWindow("MVP Baseball 2005", wpos, wpos, ww, wh, wflags | vkflag);
        if (s_win && (s_pres = presenter_vk_create(s_win))) g_gpu_api = GPU_API_VULKAN;
        else if (s_win) { SDL_DestroyWindow(s_win); s_win = nullptr; }
        else fprintf(stderr, "[vulkan] no window (%s) - using OpenGL\n", SDL_GetError());
    }
#endif
    if (!s_pres) {
        s_win = SDL_CreateWindow("MVP Baseball 2005", wpos, wpos, ww, wh, wflags | SDL_WINDOW_OPENGL);
        if (s_win && (s_pres = presenter_gl_create(s_win))) g_gpu_api = GPU_API_GL;
    }
    if (!s_pres) {
        if (s_win) SDL_DestroyWindow(s_win);
        s_win = SDL_CreateWindow("MVP Baseball 2005", wpos, wpos, ww, wh, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        if (s_win) s_pres = presenter_sdl_create(s_win);
    }
    if (!s_win || !s_pres) { fprintf(stderr, "no window (%s) - running headless\n", SDL_GetError()); g_headless = true; return; }
    rlog("video", "presenter: %s", s_pres->name());
    build_pages();
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) { s_pad = SDL_GameControllerOpen(i); break; }
    s_cur_vsync = !g_settings.vsync;                  // force the first apply
    s_cur_display = std::min(g_settings.display, std::max(0, SDL_GetNumVideoDisplays() - 1));
    apply_video();
    toast("Press F1 (or Back + Start) for settings");
}

// ---------------------------------------------------------------- menu input
static int page_items() { return (int)s_pages[s_page].items.size(); }
static bool selectable(int i) { return i >= 0 && i < page_items() && s_pages[s_page].items[i].type != IT_INFO; }
static void move_sel(int d) {
    int n = page_items(); if (!n) return;
    for (int k = 0; k < n; k++) { s_sel = (s_sel + d + n) % n; if (selectable(s_sel)) return; }
}
static void change(Item& it, int d) {
    switch (it.type) {
    case IT_CHOICE: { int n = (int)it.options.size(); it.set((it.get() + d + n) % n); break; }
    case IT_TOGGLE: it.set(!it.get()); break;
    case IT_SLIDER: it.set(it.get() + d * it.step); break;
    case IT_BUTTON: if (d > 0) it.set(0); break;
    default: break;
    }
    apply_video();
}
static void open_page(int p) {
    s_page = (p + (int)s_pages.size()) % (int)s_pages.size();
    s_sel = 0; if (!selectable(0)) move_sel(1);
    view_changed();
}
static int page_named(const char* name) { for (size_t i = 0; i < s_pages.size(); i++) if (s_pages[i].name == name) return (int)i; return -1; }
static void open_page_named(const char* name) { int i = page_named(name); if (i >= 0) { open_page(i); s_view = V_PAGE; } }
static void run_menu();
static void set_team(int t) { g_settings.team = t; art_park(t); }
static void open_leagues() {
    int i = page_named("Leagues & Eras"); if (i < 0) return;
    open_page(i); s_view = V_PAGE;
    std::vector<Card> c = league_cards(); s_era_sel = 0;
    for (size_t k = 0; k < c.size(); k++) if (c[k].cur) s_era_sel = (int)k;
    s_car = (float)s_era_sel;
}
static void hub_activate(int i) {
    if (s_launcher) {
        switch (i) {
        case 0: s_action = ACT_PLAY; break;
        case 1: open_leagues(); break;
        case 2: open_page_named("Home team"); s_focus_side = false; break;
        case 3: { for (size_t k = 0; k < s_pages.size(); k++) if (s_pages[k].group == 1) { open_page((int)k); break; } s_view = V_PAGE; s_focus_side = true; break; }
        case 4: setup_open(s_launch_ok ? 1 : 0, false); break;
        }
    } else {
        switch (i) {
        case 0: s_action = ACT_RESUME; break;
        case 1: { for (size_t k = 0; k < s_pages.size(); k++) if (s_pages[k].group == 1) { open_page((int)k); break; } s_view = V_PAGE; s_focus_side = true; break; }
        case 2: open_leagues(); break;
        case 3: open_page_named("Home team"); s_focus_side = false; break;
        case 4: s_action = ACT_SHOT; break;
        case 5: s_action = ACT_QUIT; break;
        }
    }
}
static void go_hub() { s_view = V_HUB; view_changed(); }
static void next_section(int d) {                                  // Tab: the next settings section
    const Page& cur = s_pages[s_page];
    if (cur.group != 1) return;
    int p = s_page;
    for (size_t k = 0; k < s_pages.size(); k++) { p = (p + d + (int)s_pages.size()) % (int)s_pages.size(); if (s_pages[p].group == 1) break; }
    open_page(p);
}
static void menu_nav(int key) {    // 0 up 1 down 2 left 3 right 4 ok 5 back 6 next page 7 prev page
    if (s_view == V_SETUP) { setup_nav(key); return; }
    if (s_view == V_HUB) {
        const int n = (int)hub_tiles().size();
        switch (key) {
        case 2: s_hub_sel = (s_hub_sel + n - 1) % n; break;
        case 3: s_hub_sel = (s_hub_sel + 1) % n; break;
        case 4: hub_activate(s_hub_sel); break;
        case 5: if (!s_launcher) s_action = ACT_RESUME; break;
        default: break;
        }
        return;
    }
    const Page& pg = s_pages[s_page];
    if (pg.kind == 2) {                                            // the leagues carousel
        std::vector<Card> c = league_cards(); const int n = (int)c.size();
        switch (key) {
        case 2: if (s_era_sel > 0) s_era_sel--; break;
        case 3: if (s_era_sel < n - 1) s_era_sel++; break;
        case 0: case 1: if (s_era_sel < n && c[s_era_sel].kind == 4) s_build_year = std::min(2025, std::max(1901, s_build_year + (key == 0 ? 1 : -1))); break;
        case 4: if (s_era_sel < n) choose_card(c[s_era_sel]); break;
        case 5: go_hub(); break;
        default: break;
        }
        return;
    }
    if (pg.kind == 1 && key < 5) {                                 // the 30 wordmarks, then "no home team" below them
        int t = g_settings.team, cols = s_grid_cols;
        switch (key) {
        case 0: if (t < 0) set_team(30 - cols); else if (t >= cols) set_team(t - cols); break;
        case 1: if (t >= 0) set_team(t + cols < 30 ? t + cols : -1); break;
        case 2: if (t < 0) break; if (t % cols == 0) { if (pg.group == 1) s_focus_side = true; } else set_team(t - 1); break;
        case 3: if (t >= 0 && t < 29) set_team(t + 1); break;
        case 4: go_hub(); break;
        default: break;
        }
        return;
    }
    const bool side = pg.group == 1;
    if (!side) s_focus_side = false;
    const bool has = page_items() > 0 && s_sel < page_items();
    switch (key) {
    case 0: if (s_focus_side) next_section(-1); else move_sel(-1); break;
    case 1: if (s_focus_side) next_section(1); else move_sel(1); break;
    case 2: if (s_focus_side) break; if (has) change(s_pages[s_page].items[s_sel], -1); break;
    case 3: if (s_focus_side) { s_focus_side = false; if (!selectable(s_sel)) move_sel(1); break; } if (has) change(s_pages[s_page].items[s_sel], +1); break;
    case 4: if (s_focus_side) { s_focus_side = false; if (!selectable(s_sel)) move_sel(1); } else if (has) change(s_pages[s_page].items[s_sel], +1); break;
    case 5: if (side && !s_focus_side) s_focus_side = true; else go_hub(); break;
    case 6: next_section(1); break;
    case 7: next_section(-1); break;
    }
}
static int s_drag_item = -1;
static void menu_mouse(float x, float y, bool click, bool drag) {
    if (!click && !drag) {                                          // hover: tiles and cards follow the pointer
        for (auto it = s_hits.rbegin(); it != s_hits.rend(); ++it) if (inside(*it, x, y)) { if (it->kind == 7) s_hub_sel = it->a; break; }
        return;
    }
    for (auto it = s_hits.rbegin(); it != s_hits.rend(); ++it) {
        const Hit& h = *it;
        if (!inside(h, x, y) && !(drag && h.kind == 4 && h.a == s_drag_item)) continue;
        if (h.kind == 4 && (click || drag)) {                      // slider: set from position
            Item& item = s_pages[s_page].items[h.a];
            float sw = (float)h.b, sx = h.x + 11 * s_scale;
            float f = std::min(1.0f, std::max(0.0f, (x - sx) / sw));
            int v = item.lo + (int)std::lround(f * (item.hi - item.lo) / item.step) * item.step;
            item.set(v); apply_video();
            s_focus_side = false; s_sel = h.a; s_drag_item = h.a;
            return;
        }
        if (drag) continue;
        if (s_view == V_SETUP) { if (h.kind == 9) setup_nav(5); else if (h.kind == 10) s_action = ACT_QUIT; else setup_click(h); return; }
        switch (h.kind) {
        case 0: open_page(h.a); s_focus_side = true; return;
        case 6: set_team(h.a - 1); s_focus_side = false; return;
        case 7: s_hub_sel = h.a; hub_activate(h.a); return;
        case 8: { std::vector<Card> c = league_cards(); if (h.a == s_era_sel && h.a < (int)c.size()) choose_card(c[h.a]); else s_era_sel = h.a; return; }
        case 9: go_hub(); return;
        case 10: s_action = ACT_QUIT; return;
        case 12: s_action = ACT_GET_UPDATE; return;
        case 13: s_action = ACT_UPDATE_FEATURES; return;
        case 11: { Item& item = s_pages[s_page].items[h.a]; s_focus_side = false; s_sel = h.a; item.set(h.b); apply_video(); return; }
        case 2: s_focus_side = false; s_sel = h.a; change(s_pages[s_page].items[h.a], -1); return;
        case 3: s_focus_side = false; s_sel = h.a; change(s_pages[s_page].items[h.a], +1); return;
        case 1: if (!selectable(h.a)) return; s_focus_side = false; s_sel = h.a;
                if (s_pages[s_page].items[h.a].type == IT_TOGGLE || s_pages[s_page].items[h.a].type == IT_BUTTON) change(s_pages[s_page].items[h.a], +1);
                return;
        }
    }
}
static void to_drawable(int wx, int wy, float& x, float& y) {
    int ww, wh, dw, dh; SDL_GetWindowSize(s_win, &ww, &wh); s_pres->drawable_size(dw, dh);
    x = ww ? wx * (float)dw / ww : (float)wx; y = wh ? wy * (float)dh / wh : (float)wy;
}

// ---------------------------------------------------------------- game input
static void handle_common_key(int sc) {
    if (sc == SDL_SCANCODE_F11) { g_settings.fullscreen = !g_settings.fullscreen; apply_video(); }
    if (sc == SDL_SCANCODE_F2) g_settings.show_fps = (g_settings.show_fps + 1) % 3;
    if (sc == SDL_SCANCODE_F12) {
        static int n = 0; char path[64];
        snprintf(path, sizeof path, "screenshot_%03d.png", n++);
        if (gs_screenshot(path)) toast(std::string("Screenshot saved: ") + path);
    }
}
// Pause in the background: stay inside this vsync (the game, its timers and sound stop) until the window is back
static void pause_until_focus() {
    extern void snd_audio_pause(bool);
    snd_audio_pause(true); g_fast_forward = false;
    rlog("ui", "window in the background: paused");
    for (;;) {
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 250)) continue;
        if (e.type == SDL_QUIT) { fprintf(stderr, "window closed\n"); std::exit(0); }
        if (e.type == SDL_WINDOWEVENT && (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)) break;
        if (e.type == SDL_WINDOWEVENT && (e.window.event == SDL_WINDOWEVENT_EXPOSED || e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)) present(false);
    }
    snd_audio_pause(false);
    rlog("ui", "window back: resumed");
}
static void poll_input() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: fprintf(stderr, "window closed\n"); std::exit(0);
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_MOVED || e.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED) update_display_info();
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST && g_settings.pause_unfocused && s_gui_test < 0 && s_win_shot_frame < 0 && !std::getenv("MVP_NO_PAUSE")) { pause_until_focus(); return; }
            break;
        case SDL_KEYDOWN:
            if (e.key.repeat) break;
            if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE || e.key.keysym.scancode == SDL_SCANCODE_F1) { run_menu(); return; }
            handle_common_key(e.key.keysym.scancode);
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            if (e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE ||
                (e.cbutton.button == SDL_CONTROLLER_BUTTON_START && s_pad && SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_BACK)) ||
                (e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK && s_pad && SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_START))) {
                run_menu(); return;
            }
            break;
        case SDL_CONTROLLERDEVICEADDED: if (!s_pad) s_pad = SDL_GameControllerOpen(e.cdevice.which); break;
        case SDL_CONTROLLERDEVICEREMOVED: if (s_pad) { SDL_GameControllerClose(s_pad); s_pad = nullptr; } break;
        }
    }
    const Uint8* k = SDL_GetKeyboardState(nullptr);
    g_fast_forward = k[SDL_SCANCODE_TAB] != 0;
    u16 b = 0;
    struct { int sc; u16 bit; } map[] = {
        {SDL_SCANCODE_UP, PAD_UP}, {SDL_SCANCODE_DOWN, PAD_DOWN}, {SDL_SCANCODE_LEFT, PAD_LEFT}, {SDL_SCANCODE_RIGHT, PAD_RIGHT},
        {SDL_SCANCODE_RETURN, PAD_START}, {SDL_SCANCODE_RSHIFT, PAD_SELECT},
        {SDL_SCANCODE_Z, PAD_CROSS}, {SDL_SCANCODE_X, PAD_CIRCLE}, {SDL_SCANCODE_A, PAD_SQUARE}, {SDL_SCANCODE_S, PAD_TRIANGLE},
        {SDL_SCANCODE_Q, PAD_L1}, {SDL_SCANCODE_W, PAD_R1}, {SDL_SCANCODE_1, PAD_L2}, {SDL_SCANCODE_2, PAD_R2}};
    for (auto& m : map) if (k[m.sc]) b |= m.bit;
    int lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    if (k[SDL_SCANCODE_J]) lx = 0x00; if (k[SDL_SCANCODE_L]) lx = 0xff;
    if (k[SDL_SCANCODE_I]) ly = 0x00; if (k[SDL_SCANCODE_K]) ly = 0xff;
    if (s_pad) {
        struct { SDL_GameControllerButton gb; u16 bit; } gm[] = {
            {SDL_CONTROLLER_BUTTON_A, PAD_CROSS}, {SDL_CONTROLLER_BUTTON_B, PAD_CIRCLE}, {SDL_CONTROLLER_BUTTON_X, PAD_SQUARE},
            {SDL_CONTROLLER_BUTTON_Y, PAD_TRIANGLE}, {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, PAD_L1}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PAD_R1},
            {SDL_CONTROLLER_BUTTON_START, PAD_START}, {SDL_CONTROLLER_BUTTON_BACK, PAD_SELECT},
            {SDL_CONTROLLER_BUTTON_LEFTSTICK, PAD_L3}, {SDL_CONTROLLER_BUTTON_RIGHTSTICK, PAD_R3},
            {SDL_CONTROLLER_BUTTON_DPAD_UP, PAD_UP}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, PAD_DOWN},
            {SDL_CONTROLLER_BUTTON_DPAD_LEFT, PAD_LEFT}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, PAD_RIGHT}};
        for (auto& m : gm) if (SDL_GameControllerGetButton(s_pad, m.gb)) b |= m.bit;
        if (SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) b |= PAD_L2;
        if (SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) b |= PAD_R2;
        auto ax = [&](SDL_GameControllerAxis a) { int v = SDL_GameControllerGetAxis(s_pad, a); return (v + 32768) >> 8; };
        int glx = ax(SDL_CONTROLLER_AXIS_LEFTX), gly = ax(SDL_CONTROLLER_AXIS_LEFTY);
        if (glx < 0x60 || glx > 0xa0 || gly < 0x60 || gly > 0xa0) { lx = glx; ly = gly; }
        rx = ax(SDL_CONTROLLER_AXIS_RIGHTX); ry = ax(SDL_CONTROLLER_AXIS_RIGHTY);
    }
    g_pad[0].connected = true;
    g_pad[0].buttons = b;
    g_pad[0].lx = (u8)lx; g_pad[0].ly = (u8)ly; g_pad[0].rx = (u8)rx; g_pad[0].ry = (u8)ry;
}

// ---------------------------------------------------------------- launcher
#include "iop.h"
static void launcher_check(const std::string& path, bool extract) {
    s_launch_iso = path; s_launch_ok = false;
    if (path.empty()) { s_launch_msg.clear(); return; }
    if (!file_exists(path)) { s_launch_msg = "The disc image " + path + " can't be found any more. Import it again."; return; }
    std::string err = iso_check_game(path.c_str(), extract || !file_exists(s_launch_elf) ? s_launch_elf.c_str() : nullptr);
    s_launch_ok = err.empty();
    s_launch_msg = s_launch_ok ? (extract ? "MVP Baseball 2005 (USA) imported. Press Play." : "") : err;
}
static void launcher_import(const std::string& p) {
    if (p.empty()) return;
    std::error_code ec; std::string abs = std::filesystem::absolute(p, ec).string();
    launcher_check(ec ? p : abs, true);
    if (s_launch_ok) { g_settings.iso = s_launch_iso; settings_save(settings_path()); toast("Game disc imported"); art_load(s_launch_iso); }
    else toast("That file can't be used: see what's wrong below");
    if (s_view != V_SETUP || s_setup_step != 0) setup_open(0, s_setup_first);
    s_setup_btn = s_launch_ok ? SB_NEXT : SB_CHOOSE;
}
// native "open file" dialog (blocks until closed). music: several audio files, otherwise the disc image.
static std::vector<std::string> pick_files(bool music) {
#if defined(__APPLE__)
    const char* cmd = music
        ? "osascript -e 'set fs to choose file with prompt \"Choose songs to import (MP3, M4A, FLAC, AIFF, WAV)\" with multiple selections allowed' "
          "-e 'set out to \"\"' -e 'repeat with f in fs' -e 'set out to out & POSIX path of f & linefeed' -e 'end repeat' -e 'return out' 2>/dev/null"
        : "osascript -e 'POSIX path of (choose file with prompt \"Choose your MVP Baseball 2005 disc image (.iso)\")' 2>/dev/null";
#elif defined(_WIN32)
    const char* cmd = music
        ? "powershell -NoProfile -Command \"Add-Type -AssemblyName System.Windows.Forms; $d = New-Object System.Windows.Forms.OpenFileDialog; "
          "$d.Title = 'Choose songs to import'; $d.Multiselect = $true; $d.Filter = 'Music (*.wav;*.mp3;*.m4a;*.flac;*.ogg;*.aiff)|*.wav;*.mp3;*.m4a;*.flac;*.ogg;*.aiff;*.aif|All files (*.*)|*.*'; "
          "if ($d.ShowDialog() -eq 'OK') { $d.FileNames }\""
        : "powershell -NoProfile -Command \"Add-Type -AssemblyName System.Windows.Forms; $d = New-Object System.Windows.Forms.OpenFileDialog; "
          "$d.Title = 'Choose your MVP Baseball 2005 disc image'; $d.Filter = 'Disc images (*.iso)|*.iso|All files (*.*)|*.*'; "
          "if ($d.ShowDialog() -eq 'OK') { $d.FileName }\"";
#else
    const char* cmd = music
        ? "zenity --file-selection --multiple --separator='\n' --title='Choose songs to import' 2>/dev/null || kdialog --getopenfilename . --multiple --separate-output 2>/dev/null"
        : "zenity --file-selection --title='Choose your MVP Baseball 2005 disc image' 2>/dev/null || kdialog --getopenfilename . 2>/dev/null";
#endif
#if defined(_WIN32)
    FILE* f = _popen(cmd, "r");
#else
    FILE* f = popen(cmd, "r");
#endif
    std::vector<std::string> files;
    if (!f) return files;
    std::string out; char buf[1024];
    while (fgets(buf, sizeof buf, f)) out += buf;
#if defined(_WIN32)
    _pclose(f);
#else
    pclose(f);
#endif
    for (size_t p = 0; p < out.size();) {
        size_t e = out.find('\n', p); if (e == std::string::npos) e = out.size();
        std::string line = out.substr(p, e - p); p = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) files.push_back(line);
    }
    if (files.empty() && !music) s_launch_msg = s_launch_ok ? s_launch_msg : "No file chosen. If no dialog appeared, drag your .iso onto this window instead.";
    return files;
}
static std::string pick_file() { std::vector<std::string> v = pick_files(false); return v.empty() ? std::string() : v[0]; }
// converts the chosen songs into music/ (a few seconds each: the menu shows which one it is working on)
static void present(bool menu);
static void music_import_files(const std::vector<std::string>& files) {
    if (files.empty()) return;
    int ok = 0; std::string last_err;
    for (size_t i = 0; i < files.size(); i++) {
        toast("Converting " + std::to_string(i + 1) + " of " + std::to_string(files.size()) + ": " + short_path(std::filesystem::path(files[i]).filename().string(), 48));
        s_toast_t = host_time_ns() - 200000000ull;                    // fully visible on the one frame drawn before the work
        present(true);
        std::string err = music_import(files[i]);
        if (err.empty()) ok++; else { last_err = err; rlog("music", "%s", err.c_str()); }
    }
    s_my_music_read = false;
    if (ok && g_settings.music == 0) g_settings.music = 1;            // imported to be heard
    menu_music_stop(); s_music_failed = false;                        // the launcher's playlist is rebuilt
    if (ok == (int)files.size()) toast(ok == 1 ? "1 song imported: it plays in the game's menus from the next start" : std::to_string(ok) + " songs imported: they play in the game's menus from the next start");
    else toast(ok ? std::to_string(ok) + " imported, " + std::to_string(files.size() - ok) + " failed. " + last_err : last_err);
}
static bool s_launch_play = false;

// screenshots (MVP_GUI_PAGE): "hub[:tile]" or a page name or number, MVP_GUI_SEL the row (or "side")
static void gui_test_page(const char* p) {
    std::string v = p;
    if (v.rfind("hub", 0) == 0) { s_view = V_HUB; s_hub_sel = v.size() > 4 ? std::atoi(v.c_str() + 4) : 0; return; }
    if (v.rfind("setup", 0) == 0) { int st = v.size() > 6 ? std::atoi(v.c_str() + 6) : 0; setup_open(std::min(st, 1), true);
                                    if (st == 1) { for (int w = 0; w < 200 && features_load_state() == 1; w++) SDL_Delay(50); }
                                    if (const char* sel = std::getenv("MVP_GUI_SEL")) { std::vector<FeatRow> r = feat_rows(); int k = std::atoi(sel); if (k < (int)r.size()) s_feat_sel = k; }
                                    if (st == 2) { features_load_async(); for (int w = 0; w < 200 && features_load_state() == 1; w++) SDL_Delay(50); s_setup_step = 1; setup_press(SB_INSTALL); }
                                    return; }
    int i = std::isdigit((unsigned char)v[0]) ? std::atoi(v.c_str()) : page_named(v.c_str());
    if (i < 0 || i >= (int)s_pages.size()) return;
    if (s_pages[i].kind == 2) open_leagues(); else { open_page(i); s_view = V_PAGE; }
    const char* sel = std::getenv("MVP_GUI_SEL");
    s_focus_side = sel && std::string(sel) == "side";
    if (sel && !s_focus_side) { if (s_pages[i].kind == 2) { s_era_sel = std::atoi(sel); s_car = (float)s_era_sel; } else s_sel = std::atoi(sel); }
}
// The menu pauses the game: we stay inside this vsync until it's closed.
static void run_menu() {
    // started without the launcher (--play, or the launcher turned off): the menu's art is read on first use
    if (!s_launcher && !s_art_tried) art_load(!g_settings.iso.empty() ? g_settings.iso : std::string("(game disc)"));
    s_menu = true; s_action = -1; s_anim = 0;
    if (!std::getenv("MVP_GUI_PAGE")) { s_focus_side = true; s_view = V_HUB; s_hub_sel = s_launcher && !s_launch_ok ? 4 : 0;
        if (s_launcher && (!g_settings.setup_done || !s_launch_ok)) setup_open(s_launch_ok ? 1 : 0, !g_settings.setup_done); }
    view_changed();
    g_pad[0].buttons = 0; g_fast_forward = false;
    u64 opened = host_time_ns();
    bool closing = false; u64 close_t = 0;
    while (s_menu) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            int nav = -1;
            if (e.type == SDL_QUIT) std::exit(0);
            if (e.type == SDL_KEYDOWN) {
                switch (e.key.keysym.scancode) {
                case SDL_SCANCODE_UP: nav = 0; break;
                case SDL_SCANCODE_DOWN: nav = 1; break;
                case SDL_SCANCODE_LEFT: nav = 2; break;
                case SDL_SCANCODE_RIGHT: nav = 3; break;
                case SDL_SCANCODE_RETURN: case SDL_SCANCODE_Z: case SDL_SCANCODE_SPACE: nav = 4; break;
                case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_X: case SDL_SCANCODE_BACKSPACE: nav = 5; break;
                case SDL_SCANCODE_F1: s_action = ACT_RESUME; break;
                case SDL_SCANCODE_TAB: nav = 6; break;
                default: handle_common_key(e.key.keysym.scancode);
                }
            } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                switch (e.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP: nav = 0; break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: nav = 1; break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: nav = 2; break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: nav = 3; break;
                case SDL_CONTROLLER_BUTTON_A: nav = 4; break;
                case SDL_CONTROLLER_BUTTON_B: nav = 5; break;
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: nav = 6; break;
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: nav = 7; break;
                case SDL_CONTROLLER_BUTTON_START: case SDL_CONTROLLER_BUTTON_GUIDE: s_action = ACT_RESUME; break;
                default: break;
                }
            } else if (e.type == SDL_CONTROLLERDEVICEADDED) {
                if (!s_pad) s_pad = SDL_GameControllerOpen(e.cdevice.which);
            } else if (e.type == SDL_MOUSEMOTION) {
                to_drawable(e.motion.x, e.motion.y, s_mouse_x, s_mouse_y);
                if (s_mouse_down && s_drag_item >= 0) menu_mouse(s_mouse_x, s_mouse_y, false, true);
                else menu_mouse(s_mouse_x, s_mouse_y, false, false);
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                to_drawable(e.button.x, e.button.y, s_mouse_x, s_mouse_y);
                s_mouse_down = true; s_drag_item = -1;
                menu_mouse(s_mouse_x, s_mouse_y, true, false);
            } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
                s_mouse_down = false; s_drag_item = -1;
            } else if (e.type == SDL_DROPFILE) {
                if (e.drop.file) {
                    std::string f = e.drop.file;
                    if (music_is_audio_file(f)) { music_import_files({f}); open_page_named("Audio"); }
                    else if (s_launcher) launcher_import(f);
                    SDL_free(e.drop.file);
                }
            } else if (e.type == SDL_MOUSEWHEEL) {
                const bool sideways = s_view == V_HUB || (s_view == V_PAGE && s_pages[s_page].kind == 2);      // the hub's tiles, the leagues' cards
                if (e.wheel.y > 0) menu_nav(sideways ? 2 : 0); else if (e.wheel.y < 0) menu_nav(sideways ? 3 : 1);
            }
            if (nav >= 0) menu_nav(nav);
        }
        if (s_launcher) {                                    // the game's menu music, from the disc
            float vol = g_settings.mute ? 0.0f : g_settings.volume / 100.0f * 0.7f;
            if (s_music_mode != g_settings.music || !g_settings.menu_music) {          // soundtrack changed: rebuild the playlist
                if (menu_music_playing()) menu_music_stop();
                s_music_failed = false; s_music_mode = g_settings.music;
            }
            if (g_settings.menu_music && s_launch_ok && !menu_music_playing() && !closing && !s_music_failed) {
                if (game_art_open(s_launch_iso.c_str())) { s_music_failed = !menu_music_start(s_launch_iso.c_str(), vol, g_settings.music); game_art_close(); }
            }
            menu_music_update(closing ? 0.0f : vol);
        }
        switch (s_action) {
        case ACT_SAVE: toast(settings_save(settings_path()) ? "Settings saved to " + settings_path() : "Could not write " + settings_path()); break;
        case ACT_RESET: { Settings old = g_settings; g_settings = Settings(); g_settings.gs_threads = old.gs_threads; g_settings.iso = old.iso; g_settings.setup_done = old.setup_done; g_settings.features_url = old.features_url; apply_video(); toast("All settings reset to defaults"); break; }
        case ACT_RESET_IMAGE: g_settings.brightness = 0; g_settings.contrast = 100; g_settings.saturation = 100; g_settings.gamma = 100; toast("Image settings reset"); break;
        case ACT_SHOT: { static int n = 0; char p[64]; snprintf(p, sizeof p, "screenshot_%03d.png", n++); if (gs_screenshot(p)) toast(std::string("Screenshot saved: ") + p); break; }
        case ACT_QUIT: if (features_busy()) { features_cancel(); for (int i = 0; i < 60 && features_busy(); i++) SDL_Delay(100); } std::exit(0);
        case ACT_IMPORT: launcher_import(pick_file()); break;
        case ACT_GET_UPDATE: { GameUpdate u; if (update_available(&u) && !u.url.empty()) { if (SDL_OpenURL(u.url.c_str()) == 0) toast("The download page is open in your browser: get the file for your computer and replace this one (your settings, saves and features stay)"); else toast("Open " + u.url + " to download version " + u.version); } break; }
        case ACT_UPDATE_FEATURES: setup_open(1, false); setup_press(SB_INSTALL); break;
        case ACT_MUSIC_IMPORT: music_import_files(pick_files(true)); break;
        case ACT_MUSIC_CLEAR: { int n = music_remove_all(); s_my_music_read = false; menu_music_stop(); s_music_failed = false;
                                toast(n ? std::to_string(n) + (n == 1 ? " song removed" : " songs removed") : std::string("There is no imported music")); break; }
        case ACT_PLAY:
        case ACT_RESUME:
            if (s_launcher) {
                if (!s_launch_ok) { toast(s_launch_iso.empty() ? "Choose your game disc first" : "This disc image can't be used"); setup_open(0, s_setup_first); break; }
                if (features_busy()) { toast("Features are still installing"); setup_open(2, s_setup_first); break; }
                if (!closing) { settings_save(settings_path()); s_launch_play = true; closing = true; close_t = host_time_ns(); }
                break;
            }
            if (!closing) { closing = true; close_t = host_time_ns(); } break;
        }
        s_action = -1;
        if (s_pages_dirty) { s_pages_dirty = false; int p = s_page; build_pages(); s_page = std::min(p, (int)s_pages.size() - 1); s_sel = 0; }
        double t_open = (host_time_ns() - opened) / 1e9;
        const double d_open = s_launcher ? 0.55 : 0.18, d_close = s_launcher ? 0.45 : 0.14;   // the launcher fades from / to black
        s_anim = (float)std::min(1.0, t_open / d_open);
        if (closing) {
            double tc = (host_time_ns() - close_t) / 1e9;
            s_anim = std::min(s_anim, (float)std::max(0.0, 1.0 - tc / d_close));
            if (tc >= d_close) s_menu = false;
        }
        s_anim = 1 - (1 - s_anim) * (1 - s_anim);        // ease out
        const char* shot = std::getenv("MVP_GUI_SHOT");
        if (shot && t_open > 0.3) s_pres->request_capture();
        present(s_menu);
        if (shot && t_open > 0.3) { screenshot_window(shot); s_menu = false; }   // GUI self-test
        if (!g_settings.vsync) SDL_Delay(12);
    }
    s_mouse_x = s_mouse_y = -1;
}

// Shown before the game starts (settings + importing the disc). Returns false if the player quit.
// iso_path: in = --iso (may be empty), out = the disc to play.
bool frontend_launcher(std::string& iso_path, const std::string& elf_path, bool force) {
    if (g_headless || !s_win) return true;
    SDL_PumpEvents();
    bool shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
    s_launch_elf = elf_path;
    std::string cand = !iso_path.empty() ? iso_path : g_settings.iso;
    if (cand.empty() || !file_exists(cand)) {
        const char* guesses[] = {"game/MVP Baseball 2005 (USA).iso", "game/game.iso", "game/mvp2005.iso"};
        for (const char* g : guesses) if (file_exists(g)) { cand = g; break; }
    }
    launcher_check(cand, false);
    if (s_launch_ok) art_load(s_launch_iso);                  // menu photos for the launcher and the pause menu
    if (!force && !shift && !g_settings.launcher && s_launch_ok) { iso_path = s_launch_iso; return true; }
    s_launcher = true; build_pages(); open_page(0); s_toast.clear();
    features_load_async();                                       // the list of features, in the background
    update_check_async();                                        // a newer version of the game?
    s_view = V_HUB; s_focus_side = true;
    if (!s_park_px.empty()) s_pres->set_game(s_park_px.data(), s_park_w, s_park_h);       // the preview picture
    if (const char* pg = std::getenv("MVP_GUI_PAGE")) { gui_test_page(pg); }   // screenshots
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    s_launch_play = false;
    run_menu();
    menu_music_stop();
    s_launcher = false; build_pages(); open_page(0); s_focus_side = true; s_view = V_HUB;
    s_px.clear(); s_shown_version = ~0ull;
    if (!s_launch_play) return false;
    iso_path = s_launch_iso;
    s_boot_t = host_time_ns();
    toast("Press F1 (or Back + Start) for settings");
    return true;
}

void frontend_vsync() {
    if (g_headless || !s_win) return;
    poll_input();
    update_fps();
    static const bool gui_vb = std::getenv("MVP_GUI_VB") != nullptr;          // count vblanks instead (movies draw no frames)
    if (s_gui_test >= 0 && (long long)(gui_vb ? kernel_vblank_count() : g_game_frames) >= s_gui_test && !s_gui_test_done) {
        s_gui_test_done = true;
        if (const char* p = std::getenv("MVP_GUI_PAGE")) gui_test_page(p);
        run_menu();
    }
    // present when the picture changed, at most ~1 per display refresh, at least every 100 ms (overlays)
    u64 now = host_time_ns();
    bool changed = gs_display_version() != s_shown_version;
    u64 min_gap = g_settings.vsync ? 12000000ull : 4000000ull;
    if (g_fast_forward) min_gap = 16000000ull;
    if (now - s_last_present < min_gap) return;
    if (!changed && now - s_last_present < (s_boot_t ? 16000000ull : 100000000ull)) return;   // (the boot spinner moves smoothly)
    s_last_present = now;
    bool want_shot = s_win_shot_frame >= 0 && (long long)g_game_frames >= s_win_shot_frame;
    if (want_shot) s_pres->request_capture();
    present(false);
    if (want_shot) {             // MVP_WIN_SHOT=FRAME[:path]; a path with %d: one every MVP_WIN_SHOT_EVERY frames (default 600)
        if (s_win_shot_path.find("%d") != std::string::npos) {
            char p[512]; std::snprintf(p, sizeof p, s_win_shot_path.c_str(), (int)g_game_frames);
            screenshot_window(p);
            const char* e = std::getenv("MVP_WIN_SHOT_EVERY");
            s_win_shot_frame = (long long)g_game_frames + (e ? std::atoll(e) : 600);
        } else { screenshot_window(s_win_shot_path.c_str()); s_win_shot_frame = -1; }
    }
}

#else   // headless build

void frontend_init() { g_headless = true; }
void frontend_vsync() {}
bool frontend_launcher(std::string&, const std::string&, bool) { return true; }

#endif
