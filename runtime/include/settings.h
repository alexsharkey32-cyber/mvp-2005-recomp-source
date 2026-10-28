// User settings: loaded from mvp2005.ini (or --config / $MVP_CONFIG), edited in the in-game menu (F1),
// saved on request.
#pragma once
#include <string>

struct Settings {
    // display
    bool widescreen = false;      // EA's hidden widescreen mode (derived from aspect: anything but 4:3)
    int  aspect = 5;              // 0 4:3, 1 16:9, 2 16:10, 3 21:9, 4 32:9, 5 match the display
    bool hud_169 = false;         // wider than 16:9: keep the game's 2D sprites at 16:9 proportions (experimental)
    bool fullscreen = false;
    bool fs_exclusive = false;    // fullscreen: false = borderless at the desktop resolution, true = exclusive mode
    int  display = 0;             // which monitor the window opens on (fullscreen uses it)
    int  window_scale = 2;        // initial window size: 640x448 * scale
    int  scale_filter = 3;        // 0 nearest, 1 bilinear, 2 bicubic, 3 auto (area when shrinking, bicubic when enlarging)
    bool integer_scale = false;   // whole-number scaling only (sharpest pixels)
    bool vsync = false;           // present with the display's vsync (can cap the frame rate at the monitor's)
    int  ui_scale = 0;            // 0 auto, else percent (75..200)
    // graphics
    int  renderer = 1;            // 0 software, 1 GPU when available
    int  gpu_api = 0;             // 0 auto (Metal on Apple GPUs, else OpenGL), 1 OpenGL, 2 Metal, 3 Vulkan (applied at start)
    int  internal_res = 1;        // 0 auto (from the window height), 1-6: render the game's main 3D/UI target at this multiple of 512x448
    int  tex_filter = 1;          // 0 nearest everywhere, 1 bilinear where the game asks for it (as on PS2)
    int  aniso = 0;               // anisotropic filtering (GPU renderer): 0 off (PS2-exact), 2, 4, 8, 16
    int  aa = 0;                  // post anti-aliasing: 0 off, 1 FXAA, 2 FXAA high quality
    int  sharpen = 0;             // 0-100 contrast adaptive sharpening
    int  gs_threads = -1;         // rasterizer threads: -1 auto, 0 = main thread only (applied at start)
    // textures
    int  tex_enhance = 0;         // built-in HD upscaling: 0 off, 1 = 2x, 2 = 4x
    bool tex_replace = true;      // use texture packs in textures/replace/
    bool tex_dump = false;        // write every new texture to textures/dump/ (to make packs)
    // image
    int  brightness = 0;          // -50..50
    int  contrast = 100;          // 50..150 %
    int  saturation = 100;        // 0..200 %
    int  gamma = 100;             // 50..200 (100 = unchanged)
    int  crt = 0;                 // 0 off, 1 scanlines, 2 CRT (scanlines + curvature + vignette)
    // frame rate
    int  fps_limit = 60;          // vblank rate in Hz: 60 = normal; 0 = unlimited; -1 = the display's refresh rate
    int  sim_timing = 1;          // 0 classic (game speed follows frame rate, as on PS2), 1 real-time (fixed speed)
    int  frameskip = 1;           // 0 off, 1 auto, 2..4 = always draw 1 of n frames
    int  show_fps = 0;            // 0 off, 1 simple, 2 detailed (with frame-time graph)
    int  fps_corner = 0;          // where the counter sits: 0 top left, 1 top right, 2 bottom left, 3 bottom right
    bool pause_unfocused = true;  // pause the game (and its sound) while the window is in the background
    // game data (applied at start: the game reads its database when it boots)
    int  rosters = 1;             // 0 original 2005 rosters, 1 modern (mods/rosters-2026, if present), 2 a league pack (league)
    std::string league;           // rosters 2: the league pack's folder in mods/ (tools/leaguepack.py build: mods/pack-<name>)
    bool commentary = true;       // modern rosters: commentators say new players' surnames (mods/commentary-2026)
    bool launcher = true;         // show the launcher before the game starts
    bool intro_movies = false;    // play the EA Sports logo and the intro movie at boot (off: straight to the title)
    std::string iso;              // the disc image the launcher imported
    std::string features_url;     // where the launcher gets the list of features (empty: the release's, built in)
    bool setup_done = false;      // the first-start setup (disc, features) has been through once
    int  ai = 1;                  // CPU play: 0 original (2005), 1 2026 rules (mods/ai-2026)
    int  team = -1;               // home team for the launcher / menu theme (0..29 in the game's order, -1 none)
    bool menu_music = true;       // the launcher plays the game's menu music from the disc
    int  uniforms = 1;            // 0 the 2005 uniforms, 1 today's looks from the game's own wardrobe (mods/uniforms-2026)
    int  sim_league = 1;          // simulated games: 0 the game's own numbers, 1 the real league of the season being played
    bool head_shapes = true;      // the 15 generic heads with more varied jaws, cheekbones and chins (mods/heads)
    bool portraits = true;        // rendered portraits of today's players (mods/portraits-2026, with the modern rosters)
    int  stadiums = 1;            // 0 the 2005 ballparks, 1 today's names and outfield dimensions (mods/stadiums-2026)
    int  music = 0;               // 0 the game's soundtrack, 1 your music (music/*.asf), 2 both mixed (applied at start)
    // audio
    int  volume = 100;            // 0..100 %
    bool mute = false;
};
extern Settings g_settings;

bool settings_load(const std::string& path);   // missing file = defaults
bool settings_save(const std::string& path);
std::string settings_path();                    // --config, $MVP_CONFIG or ./mvp2005.ini
void settings_set_path(const std::string& path);
bool settings_set(const std::string& key, int value);
bool settings_get(const std::string& key, int& value);    // one setting by its name in the .ini (features turn theirs on/off)
void settings_apply();                          // push values into the runtime (vblank rate, widescreen flag)
double settings_aspect();                       // the picture's aspect ratio (width / height) from aspect
int settings_internal_res(bool gpu);            // the internal resolution in effect (auto resolved, capped)
int settings_fps_limit();                       // fps_limit with -1 (match the display) resolved
extern double g_display_aspect;                 // the desktop's aspect ratio and refresh rate (set by the window code)
extern int g_display_hz, g_auto_res;
