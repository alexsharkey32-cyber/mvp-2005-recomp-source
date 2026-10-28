// ---------------------------------------------------------------- menu drawing: broadcast style
// Included by frontend.cpp. The launcher and the pause menu share one look: the home team's ballpark from the disc
// full-bleed behind frosted-glass panels (the launcher; in the game, the game itself), a hub of large tiles, then
// pages. Everything eases in: views and pages slide and fade, selections glide, switches slide.

static const u32 G_TEXT = rgba(242, 245, 248), G_DIM = rgba(154, 167, 184), G_FAINT = rgba(104, 117, 135);
static const u32 G_PANEL = rgba(9, 13, 21), G_LINE = rgba(255, 255, 255, 26), G_HI = rgba(255, 255, 255);
static const u32 G_GOOD = rgba(74, 222, 128), G_WARN = rgba(251, 191, 36), G_BAD = rgba(248, 82, 82);

enum { V_HUB, V_PAGE, V_SETUP };
static int s_view = V_HUB, s_hub_sel = 0;
static u64 s_view_t = 0;                          // when the view or page last changed (transitions)
static int s_era_sel = 0; static float s_car = 0; // leagues carousel: selected card, animated position
static DstRect s_preview{0, 0, 0, 0};             // where the game picture goes on a preview page (0 size: none)
static float s_preview_k = 0;                     // 0 = the game fills the window, 1 = it sits in the preview card
static bool s_bg_drawn = false;                   // a background picture is under the UI this frame
static bool s_under_mode = false;                 // the UI goes under the game picture (it only shows in the preview card)
static DstRect s_last_preview{0, 0, 0, 0};

static double now_s() { return host_time_ns() / 1e9; }
static float ease(float k) { k = std::min(1.0f, std::max(0.0f, k)); return 1 - (1 - k) * (1 - k) * (1 - k); }
static std::unordered_map<const void*, float> s_k;          // per-element animation values
static float glide(const void* key, float target, float rate = 0.22f) {
    auto it = s_k.find(key); if (it == s_k.end()) return s_k[key] = target;
    it->second += (target - it->second) * rate; if (std::fabs(target - it->second) < 0.001f) it->second = target;
    return it->second;
}
static void view_changed() { s_view_t = host_time_ns(); }
static float view_k() { return ease((float)((host_time_ns() - s_view_t) / 1e9 / 0.28)); }

static std::string value_text(const Item& it) {
    if (it.type == IT_CHOICE) { int v = it.get(); return v >= 0 && v < (int)it.options.size() ? it.options[v] : "?"; }
    if (it.type == IT_SLIDER) { return std::to_string(it.get()) + it.unit; }
    if (it.type == IT_INFO) return it.info ? it.info() : "";
    return "";
}
static std::string upper(std::string s) { for (char& c : s) c = (char)std::toupper((unsigned char)c); return s; }
// what is switched on (launcher: what Play will load; in game: what is loaded)
struct Status { const char* label; std::string detail; int state; };          // 0 off, 1 on, 2 problem
static int s_pack_count = -1;                     // textures in the HD pack and textures/replace (-1: count again)
static int count_pack_textures() {
    int& n = s_pack_count;
    if (n < 0) { n = 0; for (const char* d : {"textures/hd", "textures/replace"}) { std::error_code ec; for (auto it = std::filesystem::directory_iterator(d, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) n++; } }
    return n;
}
static std::vector<Status> status_lamps() {
    const Settings& S = g_settings; std::error_code ec;
    bool rost = std::filesystem::is_directory("mods/rosters-2026", ec), comm = std::filesystem::is_directory("mods/commentary-2026", ec), ai = std::filesystem::is_directory("mods/ai-2026", ec);
    bool uni = std::filesystem::is_directory("mods/uniforms-2026", ec), park = std::filesystem::is_directory("mods/stadiums-2026", ec);
    std::vector<Status> v;
    if (s_launcher) v.push_back({"Disc", s_launch_ok ? "ready" : s_launch_iso.empty() ? "not imported" : "problem", s_launch_ok ? 1 : 2});
    if (S.rosters == 2) { bool pk = std::filesystem::is_directory("mods/" + S.league, ec); std::string nm = S.league.substr(S.league.rfind("pack-", 0) == 0 ? 5 : 0); if (nm == "mine") nm = "My League";
                          v.push_back({"Rosters", pk ? nm : "pack missing", pk ? 1 : 2}); }
    else v.push_back({"Rosters", S.rosters == 1 && rost ? "2026" : "2005", S.rosters == 1 && rost ? 1 : 0});
    bool era = false;
    if (S.rosters == 2) for (const Era& e : s_eras) if (e.dir == S.league) era = true;
    if (era) { v.push_back({"Uniforms", "the era's", 1}); v.push_back({"Ballparks", "the era's", 1}); }
    else {
    v.push_back({"Uniforms", S.uniforms == 1 && uni ? "2026" : "2005", S.uniforms == 1 && uni ? 1 : 0});
    v.push_back({"Ballparks", S.stadiums == 1 && park ? "2026" : "2005", S.stadiums == 1 && park ? 1 : 0});
    }
    v.push_back({"CPU", S.ai == 1 && ai ? "2026 rules" : "2005", S.ai == 1 && ai ? 1 : 0});
    if (S.rosters == 2) comm = std::filesystem::is_directory("mods/" + S.league + "/commentary", ec);   // a pack's own
    v.push_back({"Names", S.rosters >= 1 && S.commentary && comm ? "on" : "off", S.rosters >= 1 && S.commentary && comm ? 1 : 0});
    int n = count_pack_textures();
    v.push_back({"HD", S.tex_replace ? (n ? std::to_string(n) : std::string("no pack")) : "off", S.tex_replace && n ? 1 : 0});
    if (S.music) { size_t m = my_music().size(); v.push_back({"Music", m ? std::to_string(m) + (m == 1 ? " song" : " songs") : std::string("none"), m ? 1 : 2}); }
    return v;
}
static std::string league_label() {
    const Settings& S = g_settings;
    if (S.rosters == 0) return "MLB 2005";
    if (S.rosters == 1) return "MLB 2026";
    for (const Era& e : s_eras) if (e.dir == S.league) return e.name.empty() ? e.dir : e.name;
    if (S.league == "pack-mine") return "My League";
    for (auto& p : s_packs) if (p.first == S.league) return p.second;
    return S.league;
}
static std::string league_detail() {
    const Settings& S = g_settings;
    if (S.rosters == 0) return "the original rosters";
    if (S.rosters == 1) return "today's rosters";
    for (const Era& e : s_eras) if (e.dir == S.league) return "a season from history";
    return "a league pack";
}

// ---- the background: the ballpark and the menu photos from the disc, slowly drifting, one fading into the next
static int s_bg_ids[4], s_bg_n = 0;
static void bg_list() {
    s_bg_n = 0;
    if (s_img_ok[IMG_PARK] && g_settings.team >= 0) s_bg_ids[s_bg_n++] = IMG_PARK;
    for (int k = 0; k < 3; k++) if (s_img_ok[IMG_PHOTO + k]) s_bg_ids[s_bg_n++] = IMG_PHOTO + k;
}
struct BgLayer { int id; float alpha, zoom, fx, fy; };
static int bg_layers(BgLayer out[2]) {
    bg_list(); if (!s_bg_n) return 0;
    const double t = now_s(), hold = 11.0, fade = 1.8;
    const double cyc = s_bg_n > 1 ? t / hold : 0; const int i = (int)cyc % s_bg_n; const double f = cyc - std::floor(cyc);
    auto lay = [&](int k, float a) {
        BgLayer L; L.id = s_bg_ids[k]; L.alpha = a;
        const float ph = (float)k * 1.7f;
        L.zoom = 1.10f + 0.06f * (float)std::sin(t * 0.045 + ph);
        L.fx = 0.5f + 0.06f * (float)std::sin(t * 0.031 + ph * 2); L.fy = (L.id == IMG_PARK ? 0.50f : 0.45f) + 0.04f * (float)std::cos(t * 0.027 + ph);
        return L; };
    int n = 0;
    const double fade_at = 1.0 - fade / hold;
    if (s_bg_n > 1 && f > fade_at) { float k = ease((float)((f - fade_at) / (1 - fade_at))); out[n++] = lay(i, 1); out[n++] = lay((i + 1) % s_bg_n, k); }
    else out[n++] = lay(i, 1);
    return n;
}
// the background (or its blurred copy) cropped to x,y,w,h as it sits behind it in the window dw x dh
static void bg_draw(int dw, int dh, float x, float y, float w, float h, bool blurred, u32 tint, float radius = 0) {
    BgLayer L[2]; int n = bg_layers(L);
    for (int k = 0; k < n; k++) {
        int id = blurred && s_img_ok[IMG_BLUR + L[k].id] ? IMG_BLUR + L[k].id : L[k].id;
        float ia = s_img_aspect[L[k].id], ra = (float)dw / dh, uw = 1, vh = 1;
        if (ra > ia) vh = ia / ra; else uw = ra / ia;
        uw /= L[k].zoom; vh /= L[k].zoom;
        float u0 = std::min(1 - uw, std::max(0.0f, L[k].fx - uw / 2)), v0 = std::min(1 - vh, std::max(0.0f, L[k].fy - vh / 2));
        float a0 = u0 + uw * x / dw, a1 = u0 + uw * (x + w) / dw, b0 = v0 + vh * y / dh, b1 = v0 + vh * (y + h) / dh;
        s_ui.image(id, x, y, w, h, a0, b0, a1, b1, with_alpha(tint, ((tint >> 24) / 255.0f) * L[k].alpha), radius);
    }
}
// a frosted-glass panel: the blurred background behind it (the launcher) or a dark veil (over the game)
static void glass(int dw, int dh, float x, float y, float w, float h, float r, float t, float s, float dark = 0.68f) {
    s_ui.shadow(x, y + 10 * s, w, h, r, 28 * s, rgba(0, 0, 0, (u8)(120 * t)));
    if (s_bg_drawn) { bg_draw(dw, dh, x, y, w, h, true, with_alpha(0xffffffffu, t), r); s_ui.rect(x, y, w, h, with_alpha(G_PANEL, dark * t), r); }
    else s_ui.rect(x, y, w, h, with_alpha(G_PANEL, std::min(0.92f, dark + 0.22f) * t), r);
    s_ui.gradient(x, y, w, h * 0.45f, G_HI, 0.07f * t, 0, true, r);
    s_ui.outline(x, y, w, h, with_alpha(G_HI, 0.10f * t), r, 1.2f * s);
}
static void keycap(float x, float y, const std::string& k, float s, float t) {
    float w = std::max(ui_text_width(k, 14 * s, FONT_BOLD) + 16 * s, 28 * s);
    s_ui.rect(x, y, w, 26 * s, with_alpha(G_HI, 0.14f * t), 6 * s);
    s_ui.outline(x, y, w, 26 * s, with_alpha(G_HI, 0.18f * t), 6 * s, 1 * s);
    s_ui.text(x + (w - ui_text_width(k, 14 * s, FONT_BOLD)) / 2, y + (26 * s - ui_line_height(14 * s)) / 2, k, 14 * s, with_alpha(G_TEXT, t), FONT_BOLD);
}
static float hint(float x, float y, const std::string& k, const std::string& label, float s, float t) {
    keycap(x, y, k, s, t);
    float w = std::max(ui_text_width(k, 14 * s, FONT_BOLD) + 16 * s, 28 * s);
    s_ui.text(x + w + 9 * s, y + (26 * s - ui_line_height(14 * s)) / 2, label, 14 * s, with_alpha(G_DIM, t));
    return w + 9 * s + ui_text_width(label, 14 * s) + 26 * s;
}
static void chip(float& x, float y, const Status& st, float s, float t) {     // right-aligned: x is the right edge, moves left
    const float fs = 13 * s;
    float w = ui_text_width(st.label, fs, FONT_BOLD) + ui_text_width(st.detail, fs) + 40 * s, h = 28 * s;
    x -= w;
    s_ui.rect(x, y, w, h, with_alpha(G_PANEL, 0.55f * t), h / 2);
    s_ui.outline(x, y, w, h, with_alpha(G_HI, 0.10f * t), h / 2, 1 * s);
    u32 c = st.state == 2 ? G_BAD : st.state == 1 ? G_GOOD : G_FAINT;
    s_ui.rect(x + 11 * s, y + h / 2 - 3.5f * s, 7 * s, 7 * s, with_alpha(c, t), 3.5f * s);
    if (st.state == 1) s_ui.shadow(x + 11 * s, y + h / 2 - 3.5f * s, 7 * s, 7 * s, 3.5f * s, 5 * s, with_alpha(c, 0.5f * t));
    float tx = x + 25 * s, ty = y + (h - ui_line_height(fs)) / 2;
    tx += s_ui.text(tx, ty, st.label, fs, with_alpha(G_TEXT, t), FONT_BOLD) + 5 * s;
    s_ui.text(tx, ty, st.detail, fs, with_alpha(G_DIM, t));
    x -= 8 * s;
}
static u32 s_acc_text = 0;                                       // the accent for lettering on the dark glass
static void accent_colours(u32& acc, u32& acc2, u32& on_acc) {
    int tm = g_settings.team;
    if (tm >= 0 && tm < 30 && s_team_col[tm][0]) { acc = s_team_col[tm][0]; acc2 = s_team_col[tm][1]; }
    else { acc = rgba(220, 38, 38); acc2 = rgba(30, 64, 175); }
    // lettering: whichever team colour reads on dark glass (Boston's red rather than its navy), else a lighter one
    auto reads = [](u32 c) { float l = luminance(c); int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
                             return l > 0.18f && l < 0.9f && std::max(r, std::max(g, b)) - std::min(r, std::min(g, b)) > 60; };
    s_acc_text = reads(acc) ? acc : reads(acc2) ? acc2 : lighten(acc, 0.5f);
    if (luminance(acc) < 0.16f) acc = lighten(acc, 0.30f);         // navy / black: still has to show on the dark glass
    on_acc = luminance(acc) > 0.6f ? rgba(10, 14, 22) : G_TEXT;
}

// ---- header: the game's badge and title (left), what is loaded (right)
static float draw_header(int dw, int dh, float s, float t, float M, const std::string& title, const std::string& sub, bool back) {
    const float y = M * 0.55f, ls = 58 * s;
    float x = M;
    if (back) {                                                            // a back chevron, then the page title
        float bw = 44 * s;
        bool hov = s_mouse_x >= x && s_mouse_x < x + bw && s_mouse_y >= y && s_mouse_y < y + bw;
        s_ui.rect(x, y + (ls - bw) / 2, bw, bw, with_alpha(G_HI, (hov ? 0.20f : 0.10f) * t), bw / 2);
        s_ui.text(x + bw / 2 - ui_text_width("\xE2\x80\xB9", 34 * s, FONT_BOLD) / 2, y + (ls - bw) / 2 + (bw - ui_line_height(34 * s)) / 2 - 2 * s, "\xE2\x80\xB9", 34 * s, with_alpha(G_TEXT, t), FONT_BOLD);
        s_hits.push_back({x, y, bw, ls, 9, 0, 0});
        x += bw + 18 * s;
        s_ui.text(x, y + ls / 2 - ui_line_height(40 * s) * 0.56f, upper(title), 40 * s, with_alpha(G_TEXT, t), FONT_DISPLAY, 0.6f * s);
        float tw = ui_text_width(upper(title), 40 * s, FONT_DISPLAY, 0.6f * s);
        if (!sub.empty()) s_ui.text(x + tw + 16 * s, y + ls / 2 - ui_line_height(16 * s) * 0.42f, sub, 16 * s, with_alpha(G_DIM, t));
        x += tw + 16 * s + (sub.empty() ? 0 : ui_text_width(sub, 16 * s));
    } else {
        if (s_img_ok[IMG_LOGO]) {
            s_ui.shadow(x + ls * 0.12f, y + ls * 0.14f, ls * 0.76f, ls * 0.76f, ls * 0.38f, 12 * s, rgba(0, 0, 0, (u8)(140 * t)));
            s_ui.image(IMG_LOGO, x, y, ls, ls, 0, 0, 1, 1, with_alpha(0xffffffffu, t));
            x += ls + 14 * s;
        }
        s_ui.text(x, y + 6 * s, "MVP BASEBALL 2005", 22 * s, with_alpha(G_TEXT, t), FONT_DISPLAY, 1.2f * s);
        s_ui.text(x, y + 6 * s + ui_line_height(22 * s) - 2 * s, "RECOMPILED  \xC2\xB7  NATIVE PC & MAC", 12 * s, with_alpha(G_DIM, t), FONT_BOLD, 1.6f * s);
    }
    // what is loaded, as status chips (right)
    float cx = dw - M; const float cy = y + (ls - 28 * s) / 2;
    if (s_launcher) {                                                       // quit, at the far right
        const std::string q = "Quit"; float qw = ui_text_width(q, 14 * s, FONT_BOLD) + 30 * s, qh = 28 * s;
        bool hov = s_mouse_x >= cx - qw && s_mouse_x < cx && s_mouse_y >= cy && s_mouse_y < cy + qh;
        s_ui.rect(cx - qw, cy, qw, qh, with_alpha(hov ? G_BAD : G_PANEL, (hov ? 0.9f : 0.55f) * t), qh / 2);
        s_ui.outline(cx - qw, cy, qw, qh, with_alpha(G_HI, 0.10f * t), qh / 2, 1 * s);
        s_ui.text(cx - qw + 15 * s, cy + (qh - ui_line_height(14 * s)) / 2, q, 14 * s, with_alpha(G_TEXT, t), FONT_BOLD);
        s_hits.push_back({cx - qw, cy, qw, qh, 10, 0, 0});
        cx -= qw + 14 * s;
    }
    const float left_limit = x + (back ? 30 : 330) * s;
    std::vector<Status> st = status_lamps();
    for (int i = (int)st.size() - 1; i >= 0; i--) {
        float need = ui_text_width(st[i].label, 13 * s, FONT_BOLD) + ui_text_width(st[i].detail, 13 * s) + 48 * s;
        if (cx - need < left_limit) break;
        chip(cx, cy, st[i], s, t);
    }
    return y + ls;
}
static void draw_hints(int dw, float s, float t, float y, float M, bool hub, bool page_with_side) {
    float x = M;
    if (hub) { x += hint(x, y, "\xE2\x86\x90 \xE2\x86\x92", "Select", s, t); x += hint(x, y, "Enter", s_launcher ? "Open" : "Open", s, t); if (!s_launcher) x += hint(x, y, "Esc", "Resume", s, t); }
    else if (s_pages[s_page].kind == 2) {
        x += hint(x, y, "\xE2\x86\x90 \xE2\x86\x92", "Browse", s, t);
        x += hint(x, y, "Enter", "Choose", s, t);
        x += hint(x, y, "Esc", "Back", s, t);
    } else {
        x += hint(x, y, "\xE2\x86\x91 \xE2\x86\x93", "Select", s, t);
        x += hint(x, y, "\xE2\x86\x90 \xE2\x86\x92", "Change", s, t);
        x += hint(x, y, "Enter", "Choose", s, t);
        if (page_with_side) x += hint(x, y, "Tab", "Next section", s, t);
        x += hint(x, y, "Esc", "Back", s, t);
    }
    if (s_launcher && menu_music_playing()) {
        std::string m = "\xE2\x99\xAA  " + menu_music_label();
        float mw = ui_text_width(m, 14 * s);
        if (dw - M - mw > x) s_ui.text(dw - M - mw, y + (26 * s - ui_line_height(14 * s)) / 2, m, 14 * s, with_alpha(G_DIM, t));
    }
}

// ---- the hub: a hero line and a row of tiles
struct Tile { std::string kicker, title, caption; int icon; };              // icon: 0 play 1 leagues 2 team 3 settings 4 disc 5 quit 6 camera
static std::vector<Tile> hub_tiles() {
    std::vector<Tile> v;
    const Settings& S = g_settings;
    static const char* kPresetNames[] = {"Custom look", "Original look", "Performance", "Balanced", "Quality", "Remastered"};
    std::string look = kPresetNames[preset_get()];
    look += std::string("  \xC2\xB7  ") + aspect_name(settings_aspect()) + "  \xC2\xB7  " + (S.internal_res ? std::to_string(S.internal_res) + "x" : std::string("auto res"));
    if (s_launcher) {
        v.push_back({"START", "Play Ball", s_launch_ok ? "Start the game" : "Import your game disc first", 0});
        v.push_back({"LEAGUES & ERAS", league_label(), "Today, 2005, or a season from history", 1});
        v.push_back({"HOME TEAM", g_settings.team >= 0 ? team_name(g_settings.team) : "No team", "Your club's ballpark and colours", 2});
        v.push_back({"SETTINGS", "Settings", look, 3});
        const int nu = features_load_state() == 2 ? features_update_count() : 0;
        v.push_back({"SETUP", "Disc & Features", !s_launch_ok ? (s_launch_iso.empty() ? std::string("Choose your game disc") : std::string("Disc problem: open to see"))
                                                 : nu ? (nu == 1 ? std::string("1 update ready to install") : std::to_string(nu) + " updates ready to install") : features_summary(), 4});
    } else {
        v.push_back({"PAUSED", "Resume", "Back to the game", 0});
        v.push_back({"SETTINGS", "Settings", look, 3});
        v.push_back({"LEAGUES & ERAS", league_label(), "Applies at the next start", 1});
        v.push_back({"HOME TEAM", g_settings.team >= 0 ? team_name(g_settings.team) : "No team", "Themes this menu", 2});
        v.push_back({"SCREENSHOT", "Screenshot", "Saves the game picture (F12)", 6});
        v.push_back({"QUIT", "Quit Game", "Unsaved progress is lost", 5});
    }
    return v;
}
static void draw_icon(int icon, float cx, float cy, float r, u32 col, float t) {
    const u32 c = with_alpha(col, t);
    switch (icon) {
    case 0: {                                                               // play triangle (from strips)
        for (int i = 0; i < 14; i++) { float f = i / 13.0f, x = cx - r * 0.55f + f * r * 1.2f, hh = r * 0.95f * (1 - f);
                                       s_ui.rect(x, cy - hh, r * 1.2f / 13 + 1, hh * 2, c); }
        break; }
    case 1: s_ui.text(cx - ui_text_width("\xE2\x98\x85", r * 2.1f, FONT_BOLD) / 2, cy - ui_line_height(r * 2.1f) * 0.55f, "\xE2\x98\x85", r * 2.1f, c, FONT_BOLD); break;
    case 3: s_ui.text(cx - ui_text_width("\xE2\x9A\x99", r * 2.3f, FONT_BOLD) / 2, cy - ui_line_height(r * 2.3f) * 0.56f, "\xE2\x9A\x99", r * 2.3f, c, FONT_BOLD); break;
    case 4: s_ui.outline(cx - r, cy - r, 2 * r, 2 * r, c, r, r * 0.22f); s_ui.rect(cx - r * 0.24f, cy - r * 0.24f, r * 0.48f, r * 0.48f, c, r * 0.24f); break;
    case 5: s_ui.text(cx - ui_text_width("\xE2\x9C\x95", r * 2.0f, FONT_BOLD) / 2, cy - ui_line_height(r * 2.0f) * 0.55f, "\xE2\x9C\x95", r * 2.0f, c, FONT_BOLD); break;
    case 6: s_ui.outline(cx - r, cy - r * 0.7f, 2 * r, r * 1.5f, c, r * 0.25f, r * 0.16f); s_ui.outline(cx - r * 0.4f, cy - r * 0.35f, r * 0.8f, r * 0.8f, c, r * 0.4f, r * 0.16f); break;
    default: break;
    }
}
// ---- updates: a newer game on the download page, newer versions of installed features (right, under the header)
static void draw_update_notices(int dw, int dh, float s, float a, float M, float y, u32 acc, u32 on_acc) {
    struct Notice { std::string text, button; int kind; };
    std::vector<Notice> v;
    GameUpdate u;
    if (update_available(&u)) v.push_back({"Version " + u.version + " is available  \xC2\xB7  you have " + game_version(), "Download", 12});
    if (features_load_state() == 2 && !features_busy()) {
        int n = features_update_count();
        if (n) v.push_back({n == 1 ? std::string("1 feature has an update") : std::to_string(n) + " features have updates", "Update", 13});
    }
    const float h = 44 * s, fs = 15 * s;
    for (const Notice& nt : v) {
        const float bw = ui_text_width(nt.button, 14 * s, FONT_BOLD) + 32 * s, tw = ui_text_width(nt.text, fs, FONT_BOLD);
        const float w = std::min((float)dw - 2 * M, 42 * s + tw + 18 * s + bw + 6 * s), x = dw - M - w;
        glass(dw, dh, x, y, w, h, h / 2, a, s);
        s_ui.rect(x + 18 * s, y + h / 2 - 4 * s, 8 * s, 8 * s, with_alpha(G_WARN, a), 4 * s);
        s_ui.shadow(x + 18 * s, y + h / 2 - 4 * s, 8 * s, 8 * s, 4 * s, 6 * s, with_alpha(G_WARN, 0.5f * a));
        s_ui.text(x + 38 * s, y + (h - ui_line_height(fs)) / 2, nt.text, fs, with_alpha(G_TEXT, a), FONT_BOLD);
        const float bx = x + w - bw - 6 * s, by = y + 6 * s, bh = h - 12 * s;
        const bool hov = s_mouse_x >= bx && s_mouse_x < bx + bw && s_mouse_y >= by && s_mouse_y < by + bh;
        s_ui.rect(bx, by, bw, bh, with_alpha(acc, (hov ? 1.0f : 0.85f) * a), bh / 2);
        s_ui.text(bx + 16 * s, by + (bh - ui_line_height(14 * s)) / 2, nt.button, 14 * s, with_alpha(on_acc, a), FONT_BOLD);
        s_hits.push_back({bx, by, bw, bh, nt.kind, 0, 0});
        y += h + 10 * s;
    }
}
static void draw_hub(int dw, int dh, float s, float t) {
    const float M = 56 * s;
    u32 acc, acc2, on_acc; accent_colours(acc, acc2, on_acc);
    const float k = view_k(), slide = (1 - k) * 26 * s, a = t * k;
    const float head_bottom = draw_header(dw, dh, s, t, M, "", "", false);
    if (s_launcher) draw_update_notices(dw, dh, s, a, M, head_bottom + 16 * s, acc, on_acc);
    std::vector<Tile> tiles = hub_tiles();
    const int n = (int)tiles.size();
    s_hub_sel = std::min(std::max(0, s_hub_sel), n - 1);
    const float hints_y = dh - M * 0.55f - 26 * s;
    const float gap = 16 * s, TW = (dw - 2 * M - gap * (n - 1)) / n, TH = std::min(210 * s, TW * 0.92f);
    const float ty = hints_y - 28 * s - TH;
    // hero
    {
        float hy = ty - 36 * s;
        std::string head = s_launcher ? "PLAY BALL" : "PAUSED";
        float hs = std::min(132 * s, (dw - 2 * M) / 6.5f);
        hy -= ui_line_height(hs) * 0.92f;
        std::string kick = upper(league_label()) + "   \xC2\xB7   " + upper(league_detail());
        s_ui.rect(M, hy - 30 * s + slide, 34 * s, 4 * s, with_alpha(s_acc_text, a), 2 * s);
        s_ui.text(M + 46 * s, hy - 38 * s + slide, kick, 15 * s, with_alpha(G_TEXT, a * 0.92f), FONT_BOLD, 2.2f * s);
        s_ui.text(M - 4 * s + 3 * s, hy + 4 * s + slide, head, hs, rgba(0, 0, 0, (u8)(110 * a)), FONT_DISPLAY, 1.5f * s);
        s_ui.text(M - 4 * s, hy + slide, head, hs, with_alpha(G_TEXT, a), FONT_DISPLAY, 1.5f * s);
        std::string sub = s_launcher ? (g_settings.team >= 0 ? std::string(team_name(g_settings.team)) + " at home" : std::string("Pick a home team for your colours"))
                                     : std::string("The game is paused");
        s_ui.text(M, hy + ui_line_height(hs) * 0.86f + slide, sub, 20 * s, with_alpha(G_DIM, a));
    }
    // tiles
    for (int i = 0; i < n; i++) {
        const Tile& tl = tiles[i];
        const bool sel = i == s_hub_sel;
        static char tile_key[16]; const float lift = glide(&tile_key[i & 15], sel ? 1.0f : 0.0f, 0.25f);
        const float stagger = ease((float)((host_time_ns() - s_view_t) / 1e9 / 0.32 - i * 0.05));
        float x = M + i * (TW + gap), y = ty - lift * 10 * s + (1 - stagger) * 40 * s;
        const float at = t * stagger, r = 14 * s;
        const bool primary = i == 0;
        if (lift > 0.01f) s_ui.shadow(x, y + 8 * s, TW, TH, r, 30 * s, with_alpha(primary ? acc : rgba(0, 0, 0), 0.45f * lift * at));
        if (primary) {
            const bool ready = !s_launcher || s_launch_ok;
            s_ui.rect(x, y, TW, TH, with_alpha(ready ? acc : rgba(60, 68, 80), at), r);
            s_ui.gradient(x, y, TW, TH, rgba(0, 0, 0), 0, 0.35f * at, true, r);
            s_ui.gradient(x, y, TW, TH * 0.5f, G_HI, 0.16f * at, 0, true, r);
        } else glass(dw, dh, x, y, TW, TH, r, at, s, 0.55f - 0.12f * lift);
        if (tl.icon == 2 && g_settings.team >= 0 && s_img_ok[IMG_WORD + g_settings.team]) {     // the wordmark
            float ih = TH * 0.36f, iw = std::min(ih * 2, TW - 36 * s); ih = iw / 2;
            s_ui.image(IMG_WORD + g_settings.team, x + TW - iw - 14 * s, y + 14 * s, iw, ih, 0, 0, 1, 1, with_alpha(0xffffffffu, at));
        } else draw_icon(tl.icon, x + TW - 38 * s, y + 38 * s, 15 * s, primary ? on_acc : (sel ? s_acc_text : G_DIM), at * (primary ? 0.95f : 0.85f));
        const u32 txt = primary ? on_acc : G_TEXT, dim = primary ? with_alpha(on_acc, 0.78f) : G_DIM;
        s_ui.text(x + 20 * s, y + 18 * s, tl.kicker, 12 * s, with_alpha(primary ? on_acc : s_acc_text, at * (primary ? 0.85f : 1)), FONT_BOLD, 1.8f * s);
        float fs = std::min(30 * s, (TW - 40 * s) / std::max(4.0f, ui_text_width(tl.title, 1, FONT_DISPLAY)) );
        s_ui.text(x + 20 * s, y + TH - 30 * s - ui_line_height(13 * s) - ui_line_height(fs), tl.title, fs, with_alpha(txt, at), FONT_DISPLAY, 0.4f * s);
        std::string cap = tl.caption; const float cs = 13 * s;
        while (cap.size() > 4 && ui_text_width(cap, cs) > TW - 40 * s) cap = cap.substr(0, cap.size() - 4) + "\xE2\x80\xA6";
        s_ui.text(x + 20 * s, y + TH - 22 * s - ui_line_height(cs), cap, cs, with_alpha(dim, at));
        if (lift > 0.01f) s_ui.outline(x - 2 * s, y - 2 * s, TW + 4 * s, TH + 4 * s, with_alpha(primary ? G_HI : acc, lift * at * 0.95f), r + 2 * s, 2.5f * s);
        s_hits.push_back({x, y, TW, TH, 7, i, 0});
    }
    draw_hints(dw, s, t, hints_y, M, true, false);
    if (s_launcher && !s_launch_msg.empty() && !s_launch_ok) {
        float w = std::min(dw - 2 * M, ui_text_width(s_launch_msg, 15 * s) + 40 * s);
        glass(dw, dh, (dw - w) / 2, M * 2.2f, w, 44 * s, 22 * s, t, s);
        s_ui.text((dw - w) / 2 + 20 * s, M * 2.2f + (44 * s - ui_line_height(15 * s)) / 2, s_launch_msg, 15 * s, with_alpha(G_WARN, t), FONT_BOLD);
    }
}

// ---- rows of a page
static bool is_preview_page(const Page& pg) { return pg.preview; }
static void draw_rows(int dw, int dh, const Page& pg, float s, float a, float CX, float CY, float CW, float bottom, u32 acc, u32 on_acc, bool focus) {
    const int n = (int)pg.items.size();
    const float RH = std::min(58 * s, (bottom - CY) / std::max(1, n));
    if (focus && s_sel < n) {
        float target = CY + s_sel * RH;
        float yy = glide(&s_sel, target, 0.3f);
        s_ui.rect(CX - 14 * s, yy + 3 * s, CW + 28 * s, RH - 6 * s, with_alpha(G_HI, 0.075f * a), 10 * s);
        s_ui.rect(CX - 14 * s, yy + 14 * s, 3.5f * s, RH - 28 * s, with_alpha(s_acc_text, a), 2 * s);
    }
    for (int i = 0; i < n; i++) {
        const Item& it = pg.items[i];
        const float ry = CY + i * RH;
        const bool sel = focus && i == s_sel;
        const bool hov = s_mouse_y >= ry && s_mouse_y < ry + RH && s_mouse_x >= CX - 14 * s && s_mouse_x < CX + CW + 14 * s;
        if (hov && !sel && it.type != IT_INFO) s_ui.rect(CX - 14 * s, ry + 3 * s, CW + 28 * s, RH - 6 * s, with_alpha(G_HI, 0.04f * a), 10 * s);
        s_hits.push_back({CX - 14 * s, ry, CW + 28 * s, RH, 1, i, 0});
        const float fs = 17 * s, ty = ry + (RH - ui_line_height(fs)) / 2;
        const float right = CX + CW, vh = std::min(36 * s, RH - 14 * s), vy = ry + (RH - vh) / 2;
        if (i > 0) s_ui.rect(CX, ry, CW, 1 * s, with_alpha(G_HI, 0.05f * a));
        if (it.type == IT_BUTTON) {
            const bool primary = it.label == "Play" || it.label.rfind("Import game", 0) == 0 || it.label == "Build it";
            const bool danger = it.label.find("Quit") != std::string::npos || it.label.find("Reset") != std::string::npos || it.label.find("Remove") != std::string::npos;
            float bw = ui_text_width(it.label, 15 * s, FONT_BOLD) + 44 * s;
            float k = glide(&it, sel || hov ? 1.0f : 0.0f, 0.3f);
            u32 fill = primary ? acc : danger && k > 0.5f ? G_BAD : G_HI;
            float fa = primary ? 1.0f : danger && k > 0.5f ? 0.9f : 0.10f + 0.10f * k;
            s_ui.rect(CX, vy, bw, vh, with_alpha(fill, fa * a), vh / 2);
            if (sel) s_ui.outline(CX - 3 * s, vy - 3 * s, bw + 6 * s, vh + 6 * s, with_alpha(G_HI, 0.85f * a), vh / 2 + 3 * s, 1.8f * s);
            s_ui.text(CX + 22 * s, vy + (vh - ui_line_height(15 * s)) / 2, it.label, 15 * s, with_alpha(primary ? on_acc : G_TEXT, a), FONT_BOLD);
            continue;
        }
        float lw = s_ui.text(CX, ty, it.label, fs, with_alpha(it.type == IT_INFO ? G_DIM : G_TEXT, a), sel ? FONT_BOLD : FONT_REGULAR);
        if (it.restart) {
            const std::string tag = "NEXT START"; float tw = ui_text_width(tag, 10 * s, FONT_BOLD, 1 * s) + 14 * s, th = 18 * s;
            s_ui.rect(CX + lw + 12 * s, ry + (RH - th) / 2, tw, th, with_alpha(G_WARN, 0.16f * a), th / 2);
            s_ui.text(CX + lw + 19 * s, ry + (RH - ui_line_height(10 * s)) / 2, tag, 10 * s, with_alpha(G_WARN, a), FONT_BOLD, 1 * s);
        }
        if (it.type == IT_TOGGLE) {
            const bool on = it.get() != 0;
            const float sw = 56 * s, sh = 30 * s, sx = right - sw, sy = ry + (RH - sh) / 2;
            float k = glide(&it, on ? 1.0f : 0.0f, 0.28f);
            s_ui.rect(sx, sy, sw, sh, with_alpha(G_HI, 0.16f * a * (1 - k)), sh / 2);
            s_ui.rect(sx, sy, sw, sh, with_alpha(acc, a * k), sh / 2);
            float kx = sx + 3 * s + k * (sw - sh);
            s_ui.shadow(kx, sy + 3 * s, sh - 6 * s, sh - 6 * s, (sh - 6 * s) / 2, 3 * s, rgba(0, 0, 0, (u8)(90 * a)));
            s_ui.rect(kx, sy + 3 * s, sh - 6 * s, sh - 6 * s, with_alpha(G_HI, a), (sh - 6 * s) / 2);
            s_ui.text(sx - 12 * s - ui_text_width(on ? "On" : "Off", 14 * s), ry + (RH - ui_line_height(14 * s)) / 2, on ? "On" : "Off", 14 * s, with_alpha(on ? G_TEXT : G_DIM, a));
        } else if (it.type == IT_CHOICE) {
            const int no = (int)it.options.size(), cur = it.get();
            float segw = 0; for (auto& o : it.options) segw += ui_text_width(o, 14 * s, FONT_BOLD) + 30 * s;
            const float maxseg = std::min(520 * s, CW * 0.6f);
            if (no >= 2 && no <= 4 && segw <= maxseg) {                                       // segmented control
                float x0 = right - segw;
                s_ui.rect(x0, vy, segw, vh, with_alpha(G_HI, 0.08f * a), vh / 2);
                s_ui.outline(x0, vy, segw, vh, with_alpha(G_HI, 0.10f * a), vh / 2, 1 * s);
                float xs[8], ws[8]; float xx = x0;
                for (int k = 0; k < no; k++) { ws[k] = ui_text_width(it.options[k], 14 * s, FONT_BOLD) + 30 * s; xs[k] = xx; xx += ws[k]; }
                float px = glide(&it.options, xs[std::max(0, std::min(no - 1, cur))], 0.3f), pw = glide(&it.label, ws[std::max(0, std::min(no - 1, cur))], 0.3f);
                s_ui.rect(px + 3 * s, vy + 3 * s, pw - 6 * s, vh - 6 * s, with_alpha(acc, a), (vh - 6 * s) / 2);
                for (int k = 0; k < no; k++) {
                    s_ui.text(xs[k] + 15 * s, vy + (vh - ui_line_height(14 * s)) / 2, it.options[k], 14 * s, with_alpha(k == cur ? on_acc : G_DIM, a), FONT_BOLD);
                    s_hits.push_back({xs[k], vy, ws[k], vh, 11, i, k});
                }
            } else {                                                                            // ‹ value ›
                std::string v = value_text(it);
                float vw = std::max(ui_text_width(v, 14 * s, FONT_BOLD) + 90 * s, 280 * s), vx = right - vw;
                s_ui.rect(vx, vy, vw, vh, with_alpha(G_HI, 0.08f * a), vh / 2);
                s_ui.outline(vx, vy, vw, vh, with_alpha(G_HI, sel ? 0.30f * a : 0.10f * a), vh / 2, 1 * s);
                u32 ac = sel ? s_acc_text : G_DIM;
                float cy2 = vy + (vh - ui_line_height(11 * s)) / 2;
                s_ui.text(vx + 15 * s, cy2, "\xE2\x97\x80", 11 * s, with_alpha(ac, a), FONT_BOLD);
                s_ui.text(vx + vw - 15 * s - ui_text_width("\xE2\x96\xB6", 11 * s, FONT_BOLD), cy2, "\xE2\x96\xB6", 11 * s, with_alpha(ac, a), FONT_BOLD);
                s_ui.text(vx + (vw - ui_text_width(v, 14 * s, FONT_BOLD)) / 2, vy + (vh - ui_line_height(14 * s)) / 2, v, 14 * s, with_alpha(G_TEXT, a), FONT_BOLD);
                if (no <= 12 && RH - vh >= 12 * s) for (int k = 0; k < no; k++) {
                    float dx = vx + vw / 2 - (no - 1) * 4.5f * s + k * 9 * s;
                    s_ui.rect(dx - 2 * s, vy + vh + 3 * s, 4 * s, 4 * s, with_alpha(k == cur ? acc : G_HI, (k == cur ? 1 : 0.18f) * a), 2 * s);
                }
                s_hits.push_back({vx, vy, vw / 2, vh, 2, i, 0});
                s_hits.push_back({vx + vw / 2, vy, vw / 2, vh, 3, i, 0});
            }
        } else if (it.type == IT_SLIDER) {
            int v = it.get();
            const float sw = std::min(300 * s, CW * 0.38f), th = 6 * s, sx = right - sw - 70 * s, sy = ry + RH / 2 - th / 2;
            float f = glide(&it, (float)(v - it.lo) / (it.hi - it.lo), 0.35f);
            s_ui.rect(sx, sy, sw, th, with_alpha(G_HI, 0.14f * a), th / 2);
            s_ui.rect(sx, sy, sw * f, th, with_alpha(acc, a), th / 2);
            float kr = 11 * s;
            s_ui.shadow(sx + sw * f - kr, sy + th / 2 - kr, kr * 2, kr * 2, kr, 4 * s, rgba(0, 0, 0, (u8)(100 * a)));
            s_ui.rect(sx + sw * f - kr, sy + th / 2 - kr, kr * 2, kr * 2, with_alpha(G_HI, a), kr);
            if (sel) s_ui.outline(sx + sw * f - kr - 4 * s, sy + th / 2 - kr - 4 * s, kr * 2 + 8 * s, kr * 2 + 8 * s, with_alpha(acc, 0.6f * a), kr + 4 * s, 2 * s);
            std::string vt = value_text(it);
            float bw = 56 * s; s_ui.rect(right - bw, vy, bw, vh, with_alpha(G_HI, 0.08f * a), 8 * s);
            s_ui.text(right - bw / 2 - ui_text_width(vt, 14 * s, FONT_BOLD) / 2, vy + (vh - ui_line_height(14 * s)) / 2, vt, 14 * s, with_alpha(G_TEXT, a), FONT_BOLD);
            s_hits.push_back({sx - kr, ry, sw + 2 * kr, RH, 4, i, (int)sw});
        } else if (it.type == IT_INFO) {
            std::string v = value_text(it); const float is = 15 * s;
            float maxw = CW - lw - 40 * s;
            while (v.size() > 4 && ui_text_width(v, is) > maxw) v = "\xE2\x80\xA6" + v.substr(v.size() - (v.size() - 4));
            s_ui.text(right - ui_text_width(v, is), ry + (RH - ui_line_height(is)) / 2, v, is, with_alpha(G_TEXT, a * 0.9f));
        }
    }
}
// ---- the leagues carousel
struct Card { std::string year, title, blurb; int kind; std::string dir; bool cur; };   // kind 0 era/pack 1 today 2 original 3 my league 4 build
static std::vector<Card> league_cards() {
    std::vector<Card> v; const Settings& S = g_settings;
    v.push_back({"2026", "Today's Game", "Today's clubs, players, ratings, faces and portraits, with 2026 rules if you like.", 1, "", S.rosters == 1});
    v.push_back({"2005", "The Original", "The game as it shipped: the 2005 rosters, uniforms and ballparks.", 2, "", S.rosters == 0});
    for (const Era& e : s_eras) {
        std::string y = e.season ? std::to_string(e.season) : e.name.substr(0, 4);
        v.push_back({y, e.name.empty() ? e.dir : e.name, e.blurb, 0, e.dir, S.rosters == 2 && S.league == e.dir});
    }
    std::error_code ec;
    if (std::filesystem::exists("mods/pack-mine/pack.json", ec))
        v.push_back({"MINE", "My League", "Today's rosters with the clubs you made in the Team Maker.", 3, "pack-mine", S.rosters == 2 && S.league == "pack-mine"});
    v.push_back({std::to_string(s_build_year), "Build a Season", "Any season from 1901 to 2025, made from the Lahman Baseball Database. \xE2\x86\x91\xE2\x86\x93 change the year, Enter builds it (a few minutes).", 4, "", false});
    return v;
}
static void choose_card(const Card& c) {
    if (c.kind == 4) { era_build_start(); return; }
    if (c.kind == 1) g_settings.rosters = 1;
    else if (c.kind == 2) g_settings.rosters = 0;
    else { g_settings.rosters = 2; g_settings.league = c.dir; }
    settings_save(settings_path()); frontend_rebuild_pages();
    toast(s_launcher ? c.title + " chosen: press Play Ball" : c.title + " chosen: it loads the next time the game starts (then New Dynasty)");
    if (s_launcher) { s_view = V_HUB; s_hub_sel = 0; view_changed(); }
}
static void draw_leagues(int dw, int dh, float s, float t, float M, float top, float bottom, u32 acc, u32 on_acc) {
    std::vector<Card> cards = league_cards();
    const int n = (int)cards.size();
    s_era_sel = std::min(std::max(0, s_era_sel), n - 1);
    const float CH = std::min(bottom - top - 40 * s, 470 * s), CW = std::min(330 * s, CH * 0.72f), gap = 22 * s;
    s_car += (s_era_sel - s_car) * 0.2f; if (std::fabs(s_era_sel - s_car) < 0.001f) s_car = (float)s_era_sel;
    const float cx0 = dw / 2.0f - CW / 2, y0 = top + (bottom - top - CH) / 2;
    for (int i = 0; i < n; i++) {
        const Card& c = cards[i];
        float d = i - s_car, x = cx0 + d * (CW + gap);
        if (x + CW < -20 * s || x > dw + 20 * s) continue;
        float sel = std::max(0.0f, 1 - std::fabs(d));
        float sc = 0.88f + 0.12f * sel, w = CW * sc, h = CH * sc, xx = x + (CW - w) / 2, yy = y0 + (CH - h) / 2;
        float edge = std::min(1.0f, std::max(0.0f, std::min(xx + w - M * 0.5f, dw - M * 0.5f - xx) / (CW * 0.6f)));
        float at = t * (0.55f + 0.45f * sel) * edge;
        const float r = 16 * s;
        glass(dw, dh, xx, yy, w, h, r, at, s, 0.58f - 0.14f * sel);
        if (c.cur) s_ui.gradient(xx, yy, w, h * 0.55f, acc, 0.35f * at, 0, true, r);
        // the year, big
        float ys = std::min(118 * s, w / 3.0f) * sc;
        s_ui.text(xx + 24 * s, yy + 22 * s, c.year, ys, with_alpha(c.cur ? G_TEXT : with_alpha(G_TEXT, 0.92f), at), FONT_DISPLAY, 1 * s);
        float ty = yy + 30 * s + ui_line_height(ys) * 0.95f;
        s_ui.rect(xx + 24 * s, ty, 40 * s, 3 * s, with_alpha(s_acc_text, at), 1.5f * s);
        ty += 16 * s;
        ui_text_wrapped(s_ui, xx + 24 * s, ty, w - 48 * s, upper(c.title), 22 * s * sc, with_alpha(G_TEXT, at), FONT_DISPLAY);
        ty += ui_line_height(22 * s * sc) * (ui_text_width(upper(c.title), 22 * s * sc, FONT_DISPLAY) > w - 48 * s ? 2 : 1) + 10 * s;
        // the blurb, cut to the card
        {
            std::string b = c.blurb; const float bs = 14 * s * sc, lh = ui_line_height(bs);
            int lines = std::max(1, (int)((yy + h - 70 * s - ty) / lh));
            std::vector<std::string> L; std::string line; size_t p = 0;
            while (p <= b.size()) { size_t q = b.find(' ', p); if (q == std::string::npos) q = b.size(); std::string wd = b.substr(p, q - p);
                std::string cand = line.empty() ? wd : line + " " + wd;
                if (!line.empty() && ui_text_width(cand, bs) > w - 48 * s) { L.push_back(line); line = wd; } else line = cand; p = q + 1; }
            if (!line.empty()) L.push_back(line);
            for (int k = 0; k < (int)L.size() && k < lines; k++) {
                std::string ln = L[k]; if (k == lines - 1 && (int)L.size() > lines) ln += "\xE2\x80\xA6";
                s_ui.text(xx + 24 * s, ty + k * lh, ln, bs, with_alpha(G_DIM, at));
            }
        }
        // foot: chosen / choose / build status
        std::string foot = c.cur ? "\xE2\x9C\x93  SELECTED" : c.kind == 4 ? (era_build_status().empty() ? "ENTER  \xC2\xB7  BUILD" : upper(era_build_status())) : "ENTER  \xC2\xB7  CHOOSE";
        const float fh = 32 * s, fy = yy + h - fh - 20 * s;
        float fw = std::min(w - 48 * s, ui_text_width(foot, 12 * s, FONT_BOLD, 1.4f * s) + 28 * s);
        s_ui.rect(xx + 24 * s, fy, fw, fh, with_alpha(c.cur ? acc : G_HI, (c.cur ? 1.0f : 0.10f) * at), fh / 2);
        s_ui.text(xx + 38 * s, fy + (fh - ui_line_height(12 * s)) / 2, foot, 12 * s, with_alpha(c.cur ? on_acc : G_TEXT, at), FONT_BOLD, 1.4f * s);
        if (i == s_era_sel) s_ui.outline(xx - 3 * s, yy - 3 * s, w + 6 * s, h + 6 * s, with_alpha(s_acc_text, at), r + 3 * s, 2.5f * s);
        if (c.kind == 4 && i == s_era_sel) {                    // year arrows
            s_ui.text(xx + w - 46 * s, yy + 26 * s, "\xE2\x96\xB2", 18 * s, with_alpha(G_DIM, at), FONT_BOLD);
            s_ui.text(xx + w - 46 * s, yy + 26 * s + ys * 0.62f, "\xE2\x96\xBC", 18 * s, with_alpha(G_DIM, at), FONT_BOLD);
        }
        s_hits.push_back({xx, yy, w, h, 8, i, 0});
    }
    // position dots
    const float dy = y0 + CH + 22 * s, dx0 = dw / 2.0f - (n - 1) * 7 * s;
    for (int i = 0; i < n; i++) s_ui.rect(dx0 + i * 14 * s - 3 * s, dy, i == s_era_sel ? 18 * s : 6 * s, 6 * s, with_alpha(i == s_era_sel ? acc : G_HI, (i == s_era_sel ? 1 : 0.25f) * t), 3 * s);
}
// ---- the home team: the 30 wordmarks from the disc
static int s_grid_cols = 6;
static void draw_team_grid(int dw, int dh, float s, float a, float CX, float CY, float CW, float bottom, u32 acc) {
    const int cols = s_grid_cols = CW > 900 * s ? 6 : 5, rows = (30 + cols - 1) / cols;
    const float gap = 10 * s, tw = (CW - gap * (cols - 1)) / cols;
    const float th = std::min(tw * 0.5f, (bottom - CY - 70 * s - gap * (rows - 1)) / rows);
    for (int k = 0; k < 30; k++) {
        float x = CX + (k % cols) * (tw + gap), y = CY + (k / cols) * (th + gap);
        bool cur = g_settings.team == k, hov = s_mouse_x >= x && s_mouse_x < x + tw && s_mouse_y >= y && s_mouse_y < y + th;
        float g = glide(&s_team_col[k], cur ? 1.0f : hov ? 0.5f : 0.0f, 0.3f);
        u32 tc = s_team_col[k][0] ? s_team_col[k][0] : rgba(60, 70, 85);
        s_ui.rect(x, y, tw, th, with_alpha(G_HI, (0.06f + 0.06f * g) * a), 10 * s);
        s_ui.gradient(x, y, tw, th, tc, 0.10f * a, (0.25f + 0.45f * g) * a, true, 10 * s);
        if (s_img_ok[IMG_WORD + k]) {
            float ih = th - 14 * s, iw = std::min(ih * 2, tw - 16 * s); ih = iw / 2;
            s_ui.image(IMG_WORD + k, x + (tw - iw) / 2, y + (th - ih) / 2, iw, ih, 0, 0, 1, 1, with_alpha(0xffffffffu, a));
        } else {
            std::string ab = upper(kTeamCode[k] == std::string("rfk") ? "was" : kTeamCode[k]);
            s_ui.text(x + (tw - ui_text_width(ab, 24 * s, FONT_DISPLAY)) / 2, y + (th - ui_line_height(24 * s)) / 2, ab, 24 * s, with_alpha(G_TEXT, a), FONT_DISPLAY);
        }
        if (cur) s_ui.outline(x - 2 * s, y - 2 * s, tw + 4 * s, th + 4 * s, with_alpha(acc, a), 12 * s, 2.5f * s);
        s_hits.push_back({x, y, tw, th, 6, k + 1, 0});
    }
    float ny = CY + rows * (th + gap) + 8 * s;
    std::string label = "No home team";
    float bw = ui_text_width(label, 15 * s, FONT_BOLD) + 40 * s, bh = 38 * s;
    bool cur = g_settings.team < 0;
    s_ui.rect(CX, ny, bw, bh, with_alpha(cur ? acc : G_HI, (cur ? 1.0f : 0.10f) * a), bh / 2);
    s_ui.text(CX + 20 * s, ny + (bh - ui_line_height(15 * s)) / 2, label, 15 * s, with_alpha(G_TEXT, a), FONT_BOLD);
    s_hits.push_back({CX, ny, bw, bh, 6, 0, 0});
    s_ui.text(CX + bw + 22 * s, ny + (bh - ui_line_height(26 * s)) / 2, upper(team_name(g_settings.team)), 26 * s, with_alpha(G_TEXT, a), FONT_DISPLAY, 0.8f * s);
}

// ---- a page: header, the settings list (settings pages), the sheet, the help, the live preview
static void draw_page(int dw, int dh, float s, float t) {
    const float M = 56 * s;
    u32 acc, acc2, on_acc; accent_colours(acc, acc2, on_acc);
    const Page& pg = s_pages[s_page];
    const float k = view_k(), a = t * k, slide = (1 - k) * 22 * s;
    const float hb = draw_header(dw, dh, s, t, M, pg.name, pg.subtitle, true);
    const float hints_y = dh - M * 0.55f - 26 * s;
    const float top = hb + 26 * s, bottom = hints_y - 22 * s;
    const bool side = pg.group == 1;
    draw_hints(dw, s, t, hints_y, M, false, side);
    if (pg.kind == 2) { draw_leagues(dw, dh, s, a, M, top, bottom, acc, on_acc); return; }
    float PX = M, PW = dw - 2 * M;
    if (side) {                                                              // the settings sections
        const float SW = std::min(300 * s, dw * 0.22f);
        glass(dw, dh, M, top, SW, bottom - top, 16 * s, t, s);
        float sy = top + 16 * s; int ns = 0; for (const Page& p : s_pages) if (p.group == 1) ns++;
        const float ph = std::min(50 * s, (bottom - top - 32 * s) / std::max(1, ns));
        int idx = 0;
        for (int i = 0; i < (int)s_pages.size(); i++) {
            if (s_pages[i].group != 1) continue;
            float bx = M + 12 * s, bw = SW - 24 * s, by = sy + idx * ph, bh = ph - 4 * s;
            bool sel = i == s_page, hov = s_mouse_x >= bx && s_mouse_x < bx + bw && s_mouse_y >= by && s_mouse_y < by + bh;
            if (sel) {
                float yy = glide(&s_page, by, 0.3f);
                s_ui.rect(bx, yy, bw, bh, with_alpha(s_focus_side ? acc : G_HI, (s_focus_side ? 0.95f : 0.12f) * t), 10 * s);
            } else if (hov) s_ui.rect(bx, by, bw, bh, with_alpha(G_HI, 0.06f * t), 10 * s);
            const float fs = std::min(17 * s, bh * 0.42f);
            s_ui.text(bx + 18 * s, by + (bh - ui_line_height(fs)) / 2, s_pages[i].name, fs, with_alpha(sel && s_focus_side ? on_acc : sel ? G_TEXT : G_DIM, t), sel ? FONT_BOLD : FONT_REGULAR);
            s_hits.push_back({bx, by, bw, bh, 0, i, 0});
            idx++;
        }
        PX = M + SW + 22 * s; PW = dw - M - PX;
    }
    // the sheet
    glass(dw, dh, PX, top + slide, PW, bottom - top, 16 * s, a, s);
    const float pad = 30 * s, help_h = 92 * s;
    float CX = PX + pad, CY = top + 18 * s + slide, CW = PW - 2 * pad, CB = bottom - help_h + slide;
    // live preview (display, graphics, textures, image): the picture in a card at the right
    s_preview = {0, 0, 0, 0};
    float pvx = 0, pvy = 0, pvw = 0, pvh = 0;
    if (pg.preview) {
        pvw = std::min(CW * 0.44f, 560 * s); pvh = pvw / (s_launcher ? 16.0f / 9 : (float)std::max(1.25, std::min(2.4, settings_aspect())));   // the game's own shape
        pvx = CX + CW - pvw; pvy = CY + 34 * s;
        s_ui.shadow(pvx, pvy + 6 * s, pvw, pvh, 12 * s, 20 * s, rgba(0, 0, 0, (u8)(150 * a)));
        s_ui.rect(pvx, pvy, pvw, pvh, with_alpha(rgba(0, 0, 0), a), 4 * s);
        s_preview = {(int)pvx, (int)pvy, (int)pvw, (int)pvh}; s_last_preview = s_preview;
        CW -= pvw + 28 * s;
    }
    if (pg.kind == 1) draw_team_grid(dw, dh, s, a, CX, CY, CW, CB, acc);
    else draw_rows(dw, dh, pg, s, a, CX, CY, CW, CB, acc, on_acc, !s_focus_side || !side);
    // what the focused setting does
    float hy = bottom - help_h + 14 * s + slide;
    s_ui.rect(PX + pad, hy - 8 * s, PW - 2 * pad, 1 * s, with_alpha(G_HI, 0.08f * a));
    const float hw = PW - 2 * pad;
    if (s_launcher && pg.name == "Game files" && !s_launch_msg.empty())
        hy += ui_text_wrapped(s_ui, PX + pad, hy, hw, s_launch_msg, 14 * s, with_alpha(s_launch_ok ? G_GOOD : G_WARN, a), FONT_BOLD) + 2 * s;
    if (pg.kind == 1) ui_text_wrapped(s_ui, PX + pad, hy, hw, "The launcher and this menu take your team's ballpark, wordmark and colours from your disc. It does not change which team you play as.", 14 * s, with_alpha(G_DIM, a));
    else if ((!s_focus_side || !side) && s_sel < (int)pg.items.size()) {
        const Item& it = pg.items[s_sel];
        float used = it.help.empty() ? 0 : ui_text_wrapped(s_ui, PX + pad, hy, hw, it.help, 14 * s, with_alpha(G_DIM, a));
        if (it.restart) s_ui.text(PX + pad, hy + used + 2 * s, "Applies the next time the game starts.", 13 * s, with_alpha(G_WARN, a), FONT_BOLD);
    } else ui_text_wrapped(s_ui, PX + pad, hy, hw, s_launcher ? "Pick a section. Settings are saved when you press Play Ball."
                                                             : "Pick a section. Changes apply straight away; System \xE2\x80\xBA Save settings keeps them.", 14 * s, with_alpha(G_DIM, a));
    if (pvw > 0) {                                                       // over the picture: its label and frame
        if (s_under_mode) s_ui.mark_under();
        s_ui.text(pvx, CY + 4 * s, "LIVE PREVIEW", 12 * s, with_alpha(s_acc_text, a), FONT_BOLD, 1.8f * s);
        s_ui.text(pvx + ui_text_width("LIVE PREVIEW", 12 * s, FONT_BOLD, 1.8f * s) + 12 * s, CY + 4 * s, s_launcher ? "your ballpark, with these settings" : "the game, with these settings", 12 * s, with_alpha(G_DIM, a));
        s_ui.outline(pvx - 1 * s, pvy - 1 * s, pvw + 2 * s, pvh + 2 * s, with_alpha(G_HI, 0.25f * a), 4 * s, 1.5f * s);
    }
}

static void draw_setup(int dw, int dh, float s, float t);
static void draw_menu(int dw, int dh) {
    float s = ui_scale_for(dw, dh); s_scale = s;
    s_hits.clear();
    const float t = s_anim;
    // the background: in the launcher always the disc's pictures; in the game, the game (and the pictures while the
    // game shrinks into a preview card)
    s_bg_drawn = false;
    const bool pv = s_view == V_PAGE && s_pages[s_page].preview;
    s_preview_k = glide(&s_preview_k, pv ? 1.0f : 0.0f, 0.2f);
    s_under_mode = s_launcher || pv;                                       // else the UI is drawn over the game
    if (s_under_mode) {
        bg_list();
        if (s_bg_n) { bg_draw(dw, dh, 0, 0, (float)dw, (float)dh, false, with_alpha(0xffffffffu, t)); s_bg_drawn = true;
                      bg_draw(dw, dh, 0, 0, (float)dw, (float)dh, true, with_alpha(0xffffffffu, 0.42f * t)); }   // softened: the pictures are 512 px
        else s_ui.rect(0, 0, (float)dw, (float)dh, with_alpha(rgba(14, 22, 34), t));
        // scrims so the type reads: left, bottom, top
        s_ui.gradient(0, 0, dw * 0.7f, (float)dh, rgba(4, 7, 12), 0.80f * t, 0, false);
        s_ui.gradient(0, dh * 0.42f, (float)dw, dh * 0.58f, rgba(4, 7, 12), 0, 0.88f * t, true);
        s_ui.gradient(0, 0, (float)dw, dh * 0.2f, rgba(4, 7, 12), 0.55f * t, 0, true);
    } else {                                                              // over the game: a veil
        s_ui.rect(0, 0, (float)dw, (float)dh, rgba(3, 6, 10, (u8)(150 * t)));
        s_ui.gradient(0, 0, dw * 0.6f, (float)dh, rgba(3, 6, 10), 0.6f * t, 0, false);
        s_ui.gradient(0, dh * 0.5f, (float)dw, dh * 0.5f, rgba(3, 6, 10), 0, 0.6f * t, true);
    }
    s_preview = {0, 0, 0, 0};
    if (s_view == V_HUB) draw_hub(dw, dh, s, t); else if (s_view == V_SETUP) draw_setup(dw, dh, s, t); else draw_page(dw, dh, s, t);
    if (s_under_mode && !s_ui.under) s_ui.mark_under();
    if (s_launcher && t < 1) s_ui.rect(0, 0, (float)dw, (float)dh, rgba(0, 0, 0, (u8)(255 * (1 - t))));   // fade from / to black
}
