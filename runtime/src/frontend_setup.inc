// ---------------------------------------------------------------- setup: the game disc and the features
// Included by frontend.cpp after frontend_menu.inc. Three steps: 1 the disc (only the USA disc works), 2 the features to
// install or remove (sizes, free space, a preview of each), 3 installing. Shown at the first start, and from the hub's
// "Disc & Features" tile.
#include "installer.h"

static int s_setup_step = 0;                       // 0 disc, 1 features, 2 install
static int s_feat_sel = 0;                         // row in the features list
static float s_feat_scroll = 0;                    // first visible row (animated)
static int s_setup_btn = -1;                       // keyboard focus on a button of the footer (-1: the list)
static bool s_setup_first = false;                 // shown at the first start (Skip instead of Back)
static u64 s_setup_done_t = 0;                     // when an install finished (the done card animates in)

enum { SB_CHOOSE, SB_NEXT, SB_BACK, SB_INSTALL, SB_SKIP, SB_CANCEL, SB_PLAY, SB_RETRY, SB_FEATURES, SB_REFRESH };
struct FeatRow { bool header; int feat; std::string group; };
static std::vector<FeatRow> feat_rows() {
    std::vector<FeatRow> r; std::vector<Feature>& fs = features();
    std::vector<std::string> groups;
    for (const Feature& f : fs) if (std::find(groups.begin(), groups.end(), f.group) == groups.end()) groups.push_back(f.group);
    for (const std::string& g : groups) {
        r.push_back({true, -1, g});
        for (size_t i = 0; i < fs.size(); i++) if (fs[i].group == g) r.push_back({false, (int)i, g});
    }
    return r;
}
static void setup_open(int step, bool first) {
    s_setup_step = step; s_setup_first = first; s_setup_btn = -1; s_view = V_SETUP; view_changed();
    if (step >= 1 && features_load_state() == 0) features_load_async();
    if (step == 1) { std::vector<FeatRow> r = feat_rows(); s_feat_sel = 0; for (size_t i = 0; i < r.size(); i++) if (!r[i].header) { s_feat_sel = (int)i; break; } }
}
static void setup_finish() {                       // leaves the setup for the hub
    g_settings.setup_done = true; settings_save(settings_path()); s_pack_count = -1;
    s_view = V_HUB; s_hub_sel = 0; view_changed();
}
// ---- preview pictures: decoded once into UI image slots
static std::unordered_map<std::string, int> s_pv_slot;            // file path -> UI image id (0 = failed)
static int s_pv_next = 56;
static int preview_image(const std::string& path) {
    if (path.empty() || !s_pres) return 0;
    auto it = s_pv_slot.find(path); if (it != s_pv_slot.end()) return it->second;
    int id = 0;
    std::vector<u32> px; int w = 0, h = 0;
    extern bool png_load(const std::string&, std::vector<u32>&, int&, int&);
    if (s_pv_next < kUiImages && png_load(path, px, w, h) && w > 0 && h > 0) {
        id = s_pv_next++; s_pres->set_ui_image(id, px.data(), w, h); s_img_ok[id] = true; s_img_aspect[id] = (float)w / h;
    }
    s_pv_slot[path] = id;
    return id;
}

// ---- pieces
static void setup_steps(float x, float y, float s, float t, u32 acc, u32 on_acc) {        // ① DISC ─ ② FEATURES ─ ③ INSTALL
    static const char* names[3] = {"GAME DISC", "FEATURES", "INSTALL"};
    for (int i = 0; i < 3; i++) {
        const bool cur = i == s_setup_step, done = i < s_setup_step;
        const float r = 13 * s;
        s_ui.rect(x, y, r * 2, r * 2, with_alpha(cur ? acc : done ? G_GOOD : G_HI, (cur || done ? 1.0f : 0.14f) * t), r);
        std::string n = done ? "\xE2\x9C\x93" : std::to_string(i + 1);
        s_ui.text(x + r - ui_text_width(n, 13 * s, FONT_BOLD) / 2, y + r - ui_line_height(13 * s) / 2, n, 13 * s, with_alpha(cur ? on_acc : done ? rgba(8, 20, 12) : G_DIM, t), FONT_BOLD);
        x += r * 2 + 10 * s;
        x += s_ui.text(x, y + r - ui_line_height(12 * s) / 2, names[i], 12 * s, with_alpha(cur ? G_TEXT : G_DIM, t), FONT_BOLD, 1.6f * s);
        if (i < 2) { s_ui.rect(x + 14 * s, y + r, 34 * s, 1.5f * s, with_alpha(G_HI, 0.25f * t)); x += 62 * s; }
    }
}
// a pill button; kind: 0 plain, 1 primary (team colour), 2 danger. Returns its width.
static float setup_button(float x, float y, const std::string& label, int id, int kind, float s, float t, u32 acc, u32 on_acc, bool enabled = true) {
    const float h = 44 * s, w = ui_text_width(label, 16 * s, FONT_BOLD) + 50 * s;
    const bool hov = enabled && s_mouse_x >= x && s_mouse_x < x + w && s_mouse_y >= y && s_mouse_y < y + h;
    const bool foc = enabled && s_setup_btn == id;
    static char btn_keys[128]; float k = glide(&btn_keys[id & 127], hov || foc ? 1.0f : 0.0f, 0.3f);
    u32 fill = kind == 1 ? acc : kind == 2 && k > 0.5f ? G_BAD : G_HI;
    float fa = !enabled ? 0.06f : kind == 1 ? 1.0f : kind == 2 && k > 0.5f ? 0.9f : 0.10f + 0.10f * k;
    if (kind == 1 && enabled) s_ui.shadow(x, y + 6 * s, w, h, h / 2, 16 * s, with_alpha(acc, 0.35f * t * (0.5f + 0.5f * k)));
    s_ui.rect(x, y, w, h, with_alpha(fill, fa * t), h / 2);
    if (foc) s_ui.outline(x - 3 * s, y - 3 * s, w + 6 * s, h + 6 * s, with_alpha(G_HI, 0.9f * t), h / 2 + 3 * s, 2 * s);
    s_ui.text(x + 25 * s, y + (h - ui_line_height(16 * s)) / 2, label, 16 * s, with_alpha(!enabled ? G_FAINT : kind == 1 ? on_acc : G_TEXT, t), FONT_BOLD);
    if (enabled) s_hits.push_back({x, y, w, h, 21, id, 0});
    return w;
}
static float button_width(const std::string& label, float s) { return ui_text_width(label, 16 * s, FONT_BOLD) + 50 * s; }
static void status_dot(float cx, float cy, float r, int state, float t) {                 // 0 none 1 good 2 problem 3 busy
    u32 c = state == 1 ? G_GOOD : state == 2 ? G_WARN : G_HI;
    if (state == 3) {
        const double tt = now_s();
        for (int k = 0; k < 10; k++) { float ang = (float)(k * 6.2831853 / 10), ph = (float)std::fmod(tt * 1.4 - k / 10.0 + 10, 1.0);
            s_ui.rect(cx + std::cos(ang) * r * 0.7f - 2.2f * s_scale, cy + std::sin(ang) * r * 0.7f - 2.2f * s_scale, 4.4f * s_scale, 4.4f * s_scale, with_alpha(G_TEXT, t * (0.15f + 0.85f * (1 - ph))), 2.2f * s_scale); }
        return;
    }
    s_ui.rect(cx - r, cy - r, 2 * r, 2 * r, with_alpha(c, (state ? 0.18f : 0.08f) * t), r);
    s_ui.outline(cx - r, cy - r, 2 * r, 2 * r, with_alpha(c, (state ? 0.9f : 0.3f) * t), r, 2 * s_scale);
    const char* g = state == 1 ? "\xE2\x9C\x93" : state == 2 ? "!" : "\xE2\x97\x8B";
    const float fs = r * 1.1f;
    s_ui.text(cx - ui_text_width(g, fs, FONT_BOLD) / 2, cy - ui_line_height(fs) / 2, g, fs, with_alpha(state ? c : G_DIM, t), FONT_BOLD);
}

// ---- step 1: the disc
static void draw_setup_disc(int dw, int dh, float s, float a, float M, float top, float bottom, u32 acc, u32 on_acc) {
    const float gap = 22 * s, W = dw - 2 * M, LW = std::min(W * 0.60f, 980 * s), RW = W - LW - gap;
    const float x = M, y = top, h = bottom - top;
    glass(dw, dh, x, y, LW, h, 16 * s, a, s);
    float cx = x + 40 * s, cy = y + 36 * s, cw = LW - 80 * s;
    s_ui.text(cx, cy, "YOUR GAME DISC", 13 * s, with_alpha(s_acc_text, a), FONT_BOLD, 2 * s); cy += 30 * s;
    s_ui.text(cx, cy, "MVP BASEBALL 2005", 44 * s, with_alpha(G_TEXT, a), FONT_DISPLAY, 0.8f * s); cy += ui_line_height(44 * s) + 6 * s;
    // the requirement, plainly
    {
        const float bh = 112 * s;
        s_ui.rect(cx, cy, cw, bh, with_alpha(G_HI, 0.06f * a), 12 * s);
        s_ui.rect(cx, cy + 14 * s, 4 * s, bh - 28 * s, with_alpha(G_WARN, a), 2 * s);
        float tx = cx + 24 * s, ty = cy + 16 * s;
        float lw = s_ui.text(tx, ty, "USA version only", 18 * s, with_alpha(G_TEXT, a), FONT_BOLD);
        s_ui.text(tx + lw + 14 * s, ty + 2 * s, "NTSC-U  \xC2\xB7  SLUS-21135", 15 * s, with_alpha(G_WARN, a), FONT_BOLD, 1 * s);
        ui_text_wrapped(s_ui, tx, ty + 30 * s, cw - 48 * s, "The game's code was recompiled from that one disc, so European or Japanese discs, other printings and modified or "
                        "compressed images won't start. You need a 1:1 .iso of your own copy.", 15 * s, with_alpha(G_DIM, a));
        cy += bh + 28 * s;
    }
    // status
    {
        const int st = s_launch_ok ? 1 : s_launch_iso.empty() ? 0 : 2;
        status_dot(cx + 22 * s, cy + 22 * s, 22 * s, st, a);
        float tx = cx + 62 * s;
        std::string head = st == 1 ? "Disc checked: ready to play" : st == 2 ? "This disc image can't be used" : "No disc image chosen yet";
        s_ui.text(tx, cy, head, 21 * s, with_alpha(st == 1 ? G_GOOD : st == 2 ? G_WARN : G_TEXT, a), FONT_BOLD);
        float ly = cy + ui_line_height(21 * s) + 2 * s;
        if (st == 1) { s_ui.text(tx, ly, "MVP Baseball 2005 (USA)  \xC2\xB7  SLUS-21135  \xC2\xB7  game program verified", 14 * s, with_alpha(G_DIM, a)); ly += ui_line_height(14 * s) + 2 * s; }
        if (!s_launch_iso.empty()) { std::string p = s_launch_iso; while (p.size() > 8 && ui_text_width(p, 13 * s) > cw - 70 * s) p = "\xE2\x80\xA6" + p.substr(5); s_ui.text(tx, ly, p, 13 * s, with_alpha(G_FAINT, a)); ly += ui_line_height(13 * s) + 4 * s; }
        if (st == 2 && !s_launch_msg.empty()) ly += ui_text_wrapped(s_ui, tx, ly + 4 * s, cw - 70 * s, s_launch_msg, 15 * s, with_alpha(G_TEXT, a)) + 4 * s;
        if (st == 0) ly += ui_text_wrapped(s_ui, tx, ly, cw - 70 * s, "Choose the .iso, or drag it onto this window. It stays where it is; only the game program (8 MB) is copied into the game folder.", 15 * s, with_alpha(G_DIM, a));
    }
    // buttons
    const float by = y + h - 44 * s - 34 * s;
    float bx = cx;
    bx += setup_button(bx, by, s_launch_ok ? "Choose another disc..." : "Choose disc image...", SB_CHOOSE, s_launch_ok ? 0 : 1, s, a, acc, on_acc) + 14 * s;
    if (s_launch_ok) setup_button(bx, by, "Next: Features  \xE2\x80\xBA", SB_NEXT, 1, s, a, acc, on_acc);
    if (!s_launch_ok && s_setup_first) setup_button(x + LW - 40 * s - button_width("Quit", s), by, "Quit", 99, 0, s, a, acc, on_acc);
    // how to make an .iso
    glass(dw, dh, x + LW + gap, y, RW, h, 16 * s, a, s);
    float rx = x + LW + gap + 30 * s, ry = y + 36 * s, rw = RW - 60 * s;
    s_ui.text(rx, ry, "MAKING AN .ISO", 13 * s, with_alpha(s_acc_text, a), FONT_BOLD, 2 * s); ry += 34 * s;
    struct HowTo { const char* os; const char* text; };
    static const HowTo how[] = {
        {"Windows", "Put the disc in a DVD drive and use ImgBurn (Create image file from disc) or any ripping tool that saves .iso."},
        {"Mac", "Disk Utility: File > New Image > Image from the disc, format DVD/CD master, then rename the .cdr to .iso."},
        {"Linux", "dd if=/dev/sr0 of=mvp2005.iso bs=2048 status=progress"},
        {"Already have one?", "A .chd or .cso must be turned back into .iso first (chdman extractdvd, maxcso --decompress)."},
    };
    for (const HowTo& hw : how) {
        if (ry > y + h - 60 * s) break;
        s_ui.text(rx, ry, hw.os, 16 * s, with_alpha(G_TEXT, a), FONT_BOLD); ry += ui_line_height(16 * s) + 2 * s;
        ry += ui_text_wrapped(s_ui, rx, ry, rw, hw.text, 14 * s, with_alpha(G_DIM, a)) + 18 * s;
    }
}

// ---- step 2: the features
static void draw_preview_card(int dw, int dh, const Feature& f, float x, float y, float w, float h, float s, float a, u32 acc) {
    (void)dw; (void)dh;
    float iy = y, ih = std::min(h * 0.52f, w * 9 / 16.0f);
    int before = 0, after = 0; std::string caption;
    if (!f.previews.empty()) {
        const FeaturePreview& p = f.previews[0]; caption = p.caption;
        before = preview_image(features_preview_path(p, false));
        if (!p.after_file.empty()) after = preview_image(features_preview_path(p, true));
    }
    s_ui.rect(x, iy, w, ih, with_alpha(rgba(0, 0, 0), 0.55f * a), 12 * s);
    if (before) {
        art_cover(before, x, iy, w, ih, 1.0f, 0.5f, 0.5f, with_alpha(0xffffffffu, a), 12 * s);
        if (after) {                                            // before / after: a divider that sweeps (or follows the pointer)
            const bool hov = s_mouse_x >= x && s_mouse_x < x + w && s_mouse_y >= iy && s_mouse_y < iy + ih;
            float k = hov ? (s_mouse_x - x) / w : 0.5f + 0.32f * (float)std::sin(now_s() * 0.7);
            static char wipe_key; k = glide(&wipe_key, k, hov ? 0.5f : 0.08f);
            const float sx = x + w * k;
            // the after image over the right part: same crop as art_cover, cut at the divider
            float ia = s_img_aspect[after], ra = w / ih, uw = 1, vh = 1; if (ra > ia) vh = ia / ra; else uw = ra / ia;
            float u0 = (1 - uw) / 2, v0 = (1 - vh) / 2;
            s_ui.image(after, sx, iy, x + w - sx, ih, u0 + uw * k, v0, u0 + uw, v0 + vh, with_alpha(0xffffffffu, a));
            s_ui.rect(sx - 1.5f * s, iy, 3 * s, ih, with_alpha(G_HI, 0.9f * a));
            s_ui.rect(sx - 16 * s, iy + ih / 2 - 16 * s, 32 * s, 32 * s, with_alpha(G_HI, a), 16 * s);
            s_ui.text(sx - ui_text_width("\xE2\x80\xB9\xE2\x80\xBA", 18 * s, FONT_BOLD) / 2, iy + ih / 2 - ui_line_height(18 * s) / 2 - 1 * s, "\xE2\x80\xB9\xE2\x80\xBA", 18 * s, with_alpha(rgba(10, 14, 22), a), FONT_BOLD);
            auto tag = [&](float tx, const char* t) { float tw = ui_text_width(t, 11 * s, FONT_BOLD, 1.4f * s) + 18 * s;
                s_ui.rect(tx, iy + 12 * s, tw, 22 * s, with_alpha(rgba(0, 0, 0), 0.6f * a), 11 * s);
                s_ui.text(tx + 9 * s, iy + 12 * s + (22 * s - ui_line_height(11 * s)) / 2, t, 11 * s, with_alpha(G_TEXT, a), FONT_BOLD, 1.4f * s); return tw; };
            tag(x + 12 * s, "ORIGINAL"); float tw = ui_text_width("HD PACK", 11 * s, FONT_BOLD, 1.4f * s) + 18 * s; tag(x + w - 12 * s - tw, "HD PACK");
        }
    } else {                                                    // no picture (yet): the feature's group, big, on its colour
        s_ui.gradient(x, iy, w, ih, acc, 0.35f * a, 0.08f * a, true, 12 * s);
        std::string g = upper(f.era && f.season ? std::to_string(f.season) : f.group);
        float fs = std::min(ih * 0.42f, (w - 60 * s) / std::max(1.0f, ui_text_width(g, 1, FONT_DISPLAY)));
        s_ui.text(x + 28 * s, iy + ih - 24 * s - ui_line_height(fs), g, fs, with_alpha(G_TEXT, 0.9f * a), FONT_DISPLAY);
        if (!f.previews.empty()) s_ui.text(x + 28 * s, iy + 22 * s, "loading preview...", 13 * s, with_alpha(G_DIM, a));
    }
    s_ui.outline(x, iy, w, ih, with_alpha(G_HI, 0.14f * a), 12 * s, 1 * s);
    float ty = iy + ih + 14 * s;
    if (!caption.empty()) { s_ui.text(x, ty, caption, 13 * s, with_alpha(G_FAINT, a)); ty += ui_line_height(13 * s) + 10 * s; }
    s_ui.text(x, ty, upper(f.name), 26 * s, with_alpha(G_TEXT, a), FONT_DISPLAY, 0.5f * s); ty += ui_line_height(26 * s) + 6 * s;
    std::string sz = "Download " + human_bytes(f.download_size) + "   \xC2\xB7   Uses " + human_bytes(f.install_size) + " on disk";
    if (f.installed) sz = (f.installed_version.empty() ? std::string("Installed") : "Installed (v" + f.installed_version + ")") + "   \xC2\xB7   " + human_bytes(f.install_size) + " on disk";
    s_ui.text(x, ty, sz, 14 * s, with_alpha(s_acc_text, a), FONT_BOLD); ty += ui_line_height(14 * s) + 10 * s;
    float used = ui_text_wrapped(s_ui, x, ty, w, f.desc, 15 * s, with_alpha(G_DIM, a)); ty += used + 10 * s;
    std::vector<Feature>& fs = features();
    if (!f.requires_.empty()) {
        std::string r = "Needs: "; for (size_t i = 0; i < f.requires_.size(); i++) for (const Feature& g : fs) if (g.id == f.requires_[i]) r += (i ? ", " : "") + g.name;
        s_ui.text(x, ty, r, 13 * s, with_alpha(G_FAINT, a)); ty += ui_line_height(13 * s) + 4 * s;
    }
    if (!f.excludes.empty()) {
        std::string r = "Instead of: "; for (size_t i = 0; i < f.excludes.size(); i++) for (const Feature& g : fs) if (g.id == f.excludes[i]) r += (i ? ", " : "") + g.name;
        s_ui.text(x, ty, r, 13 * s, with_alpha(G_FAINT, a));
    }
}
static void draw_setup_features(int dw, int dh, float s, float a, float M, float top, float bottom, u32 acc, u32 on_acc) {
    const int ls = features_load_state();
    const float W = dw - 2 * M, foot_h = 84 * s, gap = 22 * s;
    const float body_b = bottom - foot_h - 16 * s;
    if (ls != 2) {                                          // getting the list / no list
        glass(dw, dh, M, top, W, body_b - top, 16 * s, a, s);
        const float cx = dw / 2.0f, cy = top + (body_b - top) * 0.42f;
        if (ls <= 1) {
            status_dot(cx, cy - 30 * s, 26 * s, 3, a);
            std::string m = "Getting the list of features...";
            s_ui.text(cx - ui_text_width(m, 20 * s, FONT_BOLD) / 2, cy + 10 * s, m, 20 * s, with_alpha(G_TEXT, a), FONT_BOLD);
        } else {
            status_dot(cx, cy - 30 * s, 26 * s, 2, a);
            std::string m = "No features available";
            s_ui.text(cx - ui_text_width(m, 22 * s, FONT_BOLD) / 2, cy + 10 * s, m, 22 * s, with_alpha(G_TEXT, a), FONT_BOLD);
            std::string e = features_load_error(); float ew = std::min(W - 80 * s, 760 * s);
            ui_text_wrapped(s_ui, cx - ew / 2, cy + 46 * s, ew, e + " The game plays without them: the 2005 rosters, uniforms and ballparks are on your disc.", 15 * s, with_alpha(G_DIM, a));
        }
    } else {
        std::vector<Feature>& fs = features();
        std::vector<FeatRow> rows = feat_rows();
        const float LW = std::min(W * 0.55f, 900 * s), RW = W - LW - gap;
        // the list
        glass(dw, dh, M, top, LW, body_b - top, 16 * s, a, s);
        const float lx = M + 22 * s, lw = LW - 44 * s, ly0 = top + 14 * s, lh_avail = body_b - top - 28 * s;
        const float RH = 54 * s, HH = 40 * s;
        // scroll so the selected row is visible
        float ysel = 0, ytot = 0; for (size_t i = 0; i < rows.size(); i++) { if ((int)i == s_feat_sel) ysel = ytot; ytot += rows[i].header ? HH : RH; }
        static float target = 0;
        if (ysel - target < 0) target = ysel - (s_feat_sel > 0 && rows[s_feat_sel - 1].header ? HH : 0);
        if (ysel + RH - target > lh_avail) target = ysel + RH - lh_avail;
        target = std::max(0.0f, std::min(target, std::max(0.0f, ytot - lh_avail)));
        s_feat_scroll += (target - s_feat_scroll) * 0.25f;
        float yy = ly0 - s_feat_scroll;
        for (size_t i = 0; i < rows.size(); i++) {
            const FeatRow& r = rows[i];
            const float rh = r.header ? HH : RH;
            if (yy + rh < ly0 - 2 || yy > ly0 + lh_avail) { yy += rh; continue; }
            float ra = a * std::min(1.0f, std::min((yy + rh - ly0) / (rh * 0.6f), (ly0 + lh_avail - yy) / (rh * 0.6f)));   // fade at the edges
            ra = std::max(0.0f, ra);
            if (r.header) {
                // the group, with how many of it are selected (click: all / none)
                int nsel = 0, n = 0; for (const Feature& f : fs) if (f.group == r.group) { n++; if (f.selected) nsel++; }
                s_ui.text(lx, yy + HH - 14 * s - ui_line_height(12 * s), upper(r.group), 12 * s, with_alpha(s_acc_text, ra), FONT_BOLD, 2 * s);
                std::string c = std::to_string(nsel) + " of " + std::to_string(n) + (nsel == n ? "  \xC2\xB7  clear" : "  \xC2\xB7  all");
                float cw2 = ui_text_width(c, 12 * s);
                s_ui.text(lx + lw - cw2, yy + HH - 14 * s - ui_line_height(12 * s), c, 12 * s, with_alpha(G_DIM, ra));
                s_hits.push_back({lx + lw - cw2 - 6 * s, yy + 6 * s, cw2 + 12 * s, HH - 6 * s, 23, (int)i, 0});
                yy += rh; continue;
            }
            const Feature& f = fs[r.feat];
            const bool sel = (int)i == s_feat_sel && s_setup_btn < 0;
            const bool hov = s_mouse_x >= lx - 10 * s && s_mouse_x < lx + lw + 10 * s && s_mouse_y >= yy && s_mouse_y < yy + rh;
            if (sel || (int)i == s_feat_sel) s_ui.rect(lx - 10 * s, yy + 3 * s, lw + 20 * s, rh - 6 * s, with_alpha(G_HI, (sel ? 0.09f : 0.05f) * ra), 10 * s);
            else if (hov) s_ui.rect(lx - 10 * s, yy + 3 * s, lw + 20 * s, rh - 6 * s, with_alpha(G_HI, 0.04f * ra), 10 * s);
            if (sel) s_ui.rect(lx - 10 * s, yy + 14 * s, 3.5f * s, rh - 28 * s, with_alpha(s_acc_text, ra), 2 * s);
            // checkbox
            const float cb = 24 * s, cbx = lx + 4 * s, cby = yy + (rh - cb) / 2;
            float k = glide(&f.selected, f.selected ? 1.0f : 0.0f, 0.3f);
            s_ui.rect(cbx, cby, cb, cb, with_alpha(G_HI, 0.10f * ra * (1 - k)), 7 * s);
            s_ui.outline(cbx, cby, cb, cb, with_alpha(G_HI, 0.35f * ra * (1 - k)), 7 * s, 1.5f * s);
            s_ui.rect(cbx, cby, cb, cb, with_alpha(f.required ? G_FAINT : acc, ra * k), 7 * s);
            if (k > 0.3f) s_ui.text(cbx + cb / 2 - ui_text_width("\xE2\x9C\x93", 16 * s, FONT_BOLD) / 2, cby + (cb - ui_line_height(16 * s)) / 2, "\xE2\x9C\x93", 16 * s, with_alpha(on_acc, ra * k), FONT_BOLD);
            // name, tag, size
            float tx = cbx + cb + 16 * s;
            float nw = s_ui.text(tx, yy + (rh - ui_line_height(17 * s)) / 2, f.name, 17 * s, with_alpha(G_TEXT, ra), sel ? FONT_BOLD : FONT_REGULAR);
            std::string tag; u32 tc = G_DIM;
            if (f.installed && !f.selected) { tag = "REMOVE"; tc = G_BAD; }
            else if (f.installed && !f.installed_version.empty() && f.installed_version != f.version) { tag = "UPDATE"; tc = G_WARN; }
            else if (f.installed) { tag = "INSTALLED"; tc = G_GOOD; }
            else if (f.selected) { tag = "NEW"; tc = s_acc_text; }
            else if (f.def) { tag = "RECOMMENDED"; tc = G_DIM; }
            if (!tag.empty()) {
                float tw = ui_text_width(tag, 10 * s, FONT_BOLD, 1 * s) + 14 * s, th = 19 * s, tyy = yy + (rh - th) / 2;
                if (tx + nw + 12 * s + tw < lx + lw - 110 * s) {
                    s_ui.rect(tx + nw + 12 * s, tyy, tw, th, with_alpha(tc, 0.16f * ra), th / 2);
                    s_ui.text(tx + nw + 19 * s, tyy + (th - ui_line_height(10 * s)) / 2, tag, 10 * s, with_alpha(tc, ra), FONT_BOLD, 1 * s);
                }
            }
            std::string sz = human_bytes(f.install_size);
            s_ui.text(lx + lw - ui_text_width(sz, 15 * s, FONT_BOLD), yy + (rh - ui_line_height(15 * s)) / 2, sz, 15 * s, with_alpha(f.selected ? G_TEXT : G_DIM, ra), FONT_BOLD);
            s_hits.push_back({lx - 10 * s, std::max(yy, ly0), lw + 20 * s, std::min(yy + rh, ly0 + lh_avail) - std::max(yy, ly0), 20, (int)i, 0});
            yy += rh;
        }
        if (ytot > lh_avail) {                               // scroll bar
            float bh = lh_avail * lh_avail / ytot, by = ly0 + (lh_avail - bh) * (s_feat_scroll / std::max(1.0f, ytot - lh_avail));
            s_ui.rect(M + LW - 9 * s, by, 4 * s, bh, with_alpha(G_HI, 0.22f * a), 2 * s);
        }
        // the preview of the selected feature
        glass(dw, dh, M + LW + gap, top, RW, body_b - top, 16 * s, a, s);
        if (s_feat_sel >= 0 && s_feat_sel < (int)rows.size() && !rows[s_feat_sel].header)
            draw_preview_card(dw, dh, fs[rows[s_feat_sel].feat], M + LW + gap + 26 * s, top + 26 * s, RW - 52 * s, body_b - top - 52 * s, s, a, acc);
    }
    // the footer: what it adds up to, and the buttons
    glass(dw, dh, M, bottom - foot_h, W, foot_h, 16 * s, a, s, 0.75f);
    const float fy = bottom - foot_h, fcy = fy + foot_h / 2;
    float bx = M + W - 24 * s;
    const bool pending = ls == 2 && features_changes_pending();
    uint64_t dl = 0, add = 0, fr = 0; if (ls == 2) features_plan(dl, add, fr);
    const uint64_t freeb = features_disk_free();
    uint64_t biggest = 0; if (ls == 2) for (const Feature& f : features()) if (f.selected && !f.installed) for (const FeaturePart& p : f.parts) biggest = std::max(biggest, p.size);
    const bool room = add + biggest <= freeb + fr || add == 0;
    {
        std::string lab = ls != 2 ? std::string("Continue") : pending ? (add || dl ? "Install" : "Apply changes") : std::string("Continue");
        bx -= button_width(lab, s); setup_button(bx, fcy - 22 * s, lab, ls == 2 && pending ? SB_INSTALL : SB_SKIP, 1, s, a, acc, on_acc, !pending || room);
        if (ls == 3) { bx -= button_width("Try again", s) + 12 * s; setup_button(bx, fcy - 22 * s, "Try again", SB_REFRESH, 0, s, a, acc, on_acc); }
        bx -= button_width("\xE2\x80\xB9  Disc", s) + 12 * s; setup_button(bx, fcy - 22 * s, "\xE2\x80\xB9  Disc", SB_BACK, 0, s, a, acc, on_acc);
    }
    float tx = M + 28 * s;
    if (ls == 2) {
        auto stat = [&](const std::string& k, const std::string& v, u32 c) {
            s_ui.text(tx, fy + 18 * s, k, 11 * s, with_alpha(G_DIM, a), FONT_BOLD, 1.6f * s);
            float w = s_ui.text(tx, fy + 36 * s, v, 22 * s, with_alpha(c, a), FONT_BOLD);
            tx += std::max(w, ui_text_width(k, 11 * s, FONT_BOLD, 1.6f * s)) + 40 * s;
        };
        stat("DOWNLOAD", dl ? human_bytes(dl) : std::string("nothing"), G_TEXT);
        stat("DISK SPACE", add ? "+" + human_bytes(add) : fr ? "-" + human_bytes(fr) : std::string("no change"), G_TEXT);
        stat("FREE ON THIS DRIVE", human_bytes(freeb), room ? G_GOOD : G_BAD);
        if (!room && tx < bx - 200 * s) ui_text_wrapped(s_ui, tx, fy + 18 * s, bx - tx - 30 * s, "Not enough space: unselect something, or free up " + human_bytes(add + biggest - freeb - fr) + ".", 14 * s, with_alpha(G_BAD, a), FONT_BOLD);
        else if (dl > 4000000000ull && tx < bx - 200 * s) {
            const double mins = dl / 12.5e6 / 60;                                    // at 100 Mbit/s
            char b[160]; snprintf(b, sizeof b, "About %s at 100 Mbit/s. You can stop and continue later: it picks up where it stopped.", mins >= 90 ? (std::to_string((int)(mins / 60 + 0.5)) + " hours").c_str() : (std::to_string((int)mins + 1) + " minutes").c_str());
            ui_text_wrapped(s_ui, tx, fy + 18 * s, bx - tx - 30 * s, b, 14 * s, with_alpha(G_DIM, a));
        }
    }
}

// ---- step 3: installing
static void draw_setup_install(int dw, int dh, float s, float a, float M, float top, float bottom, u32 acc, u32 on_acc) {
    FeatureProgress p = features_progress();
    const bool busy = features_busy();
    const float W = std::min(dw - 2 * M, 980 * s), x = (dw - W) / 2, h = std::min(bottom - top, 560 * s), y = top + (bottom - top - h) / 2;
    glass(dw, dh, x, y, W, h, 18 * s, a, s);
    const float cx = x + 44 * s, cw = W - 88 * s; float cy = y + 40 * s;
    const bool done = !busy && p.phase == FP_DONE, failed = !busy && (p.phase == FP_FAILED || p.phase == FP_CANCELLED);
    if (done && !s_setup_done_t) s_setup_done_t = host_time_ns();
    status_dot(cx + 26 * s, cy + 26 * s, 26 * s, busy ? 3 : done ? 1 : 2, a);
    std::string head = busy ? (p.phase == FP_REMOVING ? "Removing" : "Installing") : done ? "All set" : p.phase == FP_CANCELLED ? "Paused" : "Something went wrong";
    s_ui.text(cx + 72 * s, cy + 2 * s, upper(head), 34 * s, with_alpha(G_TEXT, a), FONT_DISPLAY, 0.6f * s);
    std::string sub = busy ? p.feature : done ? "Your features are installed. They're on whenever you play." : p.step;
    s_ui.text(cx + 72 * s, cy + 6 * s + ui_line_height(34 * s), sub, 16 * s, with_alpha(G_DIM, a));
    cy += 100 * s;
    // the bar
    const float frac = p.total ? (float)std::min(1.0, (double)p.done / p.total) : (done ? 1.0f : 0.0f);
    static char bar_key; float fk = glide(&bar_key, frac, 0.15f);
    s_ui.rect(cx, cy, cw, 12 * s, with_alpha(G_HI, 0.10f * a), 6 * s);
    s_ui.rect(cx, cy, std::max(12 * s, cw * fk), 12 * s, with_alpha(failed ? G_WARN : done ? G_GOOD : acc, a), 6 * s);
    if (busy && (p.phase == FP_VERIFYING || p.phase == FP_EXTRACTING)) {          // a moving sheen while it unpacks / checks
        float ph = (float)std::fmod(now_s() * 0.8, 1.0); s_ui.gradient(cx + cw * fk * ph - 40 * s, cy, 80 * s, 12 * s, G_HI, 0, 0.35f * a, false, 6 * s);
    }
    cy += 26 * s;
    {
        char b[200];
        if (p.total) snprintf(b, sizeof b, "%s of %s", human_bytes(p.done).c_str(), human_bytes(p.total).c_str()); else snprintf(b, sizeof b, "%s", "");
        s_ui.text(cx, cy, b, 15 * s, with_alpha(G_TEXT, a), FONT_BOLD);
        std::string r;
        if (busy && p.phase == FP_DOWNLOADING && p.speed > 1000) {
            r = human_bytes((uint64_t)p.speed) + "/s";
            double left = (p.total - p.done) / p.speed;
            if (left > 5) r += left > 5400 ? "   \xC2\xB7   about " + std::to_string((int)(left / 3600 + 0.5)) + " h left" : "   \xC2\xB7   about " + std::to_string((int)(left / 60) + 1) + " min left";
        } else if (busy) r = p.step;
        else if (p.features_total) r = std::to_string(p.features_done) + " of " + std::to_string(p.features_total) + " done";
        s_ui.text(cx + cw - ui_text_width(r, 15 * s), cy, r, 15 * s, with_alpha(G_DIM, a));
        cy += 40 * s;
    }
    if (failed && !p.error.empty()) { cy += ui_text_wrapped(s_ui, cx, cy, cw, p.error, 15 * s, with_alpha(G_WARN, a), FONT_BOLD) + 14 * s; }
    // the steps: done, now, still to come
    {
        std::vector<std::pair<std::string, bool>> job = features_job();
        const float by_limit = y + h - 44 * s - 36 * s - 54 * s, lh = 30 * s;
        const int fit = std::max(0, (int)((by_limit - cy) / lh));
        int first = std::max(0, std::min((int)job.size() - fit, p.features_done - 1));
        const float colw = cw / 2;
        int shown = 0;
        for (int i = first; i < (int)job.size() && shown < fit * 2; i++, shown++) {
            const float rx = cx + (shown >= fit ? colw : 0), ry = cy + (shown % std::max(1, fit)) * lh;
            const bool d = i < p.features_done, now = i == p.features_done && busy;
            if (now) status_dot(rx + 10 * s, ry + 10 * s, 11 * s, 3, a);
            else s_ui.text(rx + 10 * s - ui_text_width(d ? "\xE2\x9C\x93" : "\xE2\x97\x8B", 15 * s, FONT_BOLD) / 2, ry + 10 * s - ui_line_height(15 * s) / 2, d ? "\xE2\x9C\x93" : "\xE2\x97\x8B", 15 * s, with_alpha(d ? G_GOOD : G_FAINT, a), FONT_BOLD);
            std::string lab = (job[i].second ? "" : "Remove: ") + job[i].first;
            s_ui.text(rx + 30 * s, ry + 10 * s - ui_line_height(15 * s) / 2, lab, 15 * s, with_alpha(now ? G_TEXT : d ? G_DIM : G_FAINT, a), now ? FONT_BOLD : FONT_REGULAR);
        }
        if (job.empty() && done) s_ui.text(cx, cy, "Nothing to change.", 15 * s, with_alpha(G_DIM, a));
    }
    if (busy) ui_text_wrapped(s_ui, cx, y + h - 44 * s - 36 * s - 40 * s, cw, "You can leave this running. Stopping is safe: Install picks up where it stopped, and finished downloads are kept.", 14 * s, with_alpha(G_FAINT, a));
    // buttons
    const float by = y + h - 44 * s - 36 * s; float bx = cx;
    if (busy) setup_button(bx, by, "Stop", SB_CANCEL, 2, s, a, acc, on_acc);
    else if (done) { bx += setup_button(bx, by, "Play Ball", SB_PLAY, 1, s, a, acc, on_acc) + 14 * s; setup_button(bx, by, "Back to features", SB_FEATURES, 0, s, a, acc, on_acc); }
    else { bx += setup_button(bx, by, p.phase == FP_CANCELLED ? "Continue" : "Try again", SB_RETRY, 1, s, a, acc, on_acc) + 14 * s; setup_button(bx, by, "Back to features", SB_FEATURES, 0, s, a, acc, on_acc); }
}

static void draw_setup(int dw, int dh, float s, float t) {
    const float M = 56 * s;
    u32 acc, acc2, on_acc; accent_colours(acc, acc2, on_acc);
    const float k = view_k(), a = t * k;
    static const char* subs[3] = {"your MVP Baseball 2005 disc", "choose what to install", "downloading and unpacking"};
    const float hb = draw_header(dw, dh, s, t, M, "Setup", subs[s_setup_step], true);
    const float hints_y = dh - M * 0.55f - 26 * s;
    setup_steps(M, hb + 22 * s, s, a, acc, on_acc);
    const float top = hb + 22 * s + 26 * s + 22 * s, bottom = hints_y - 22 * s;
    if (s_setup_step == 0) draw_setup_disc(dw, dh, s, a, M, top, bottom, acc, on_acc);
    else if (s_setup_step == 1) draw_setup_features(dw, dh, s, a, M, top, bottom, acc, on_acc);
    else draw_setup_install(dw, dh, s, a, M, top, bottom, acc, on_acc);
    // hints
    float x = M;
    if (s_setup_step == 1) { x += hint(x, hints_y, "\xE2\x86\x91 \xE2\x86\x93", "Select", s, t); x += hint(x, hints_y, "Enter", "Turn on / off", s, t); x += hint(x, hints_y, "Tab", "Buttons", s, t); x += hint(x, hints_y, "Esc", "Back", s, t); }
    else { x += hint(x, hints_y, "\xE2\x86\x90 \xE2\x86\x92", "Buttons", s, t); x += hint(x, hints_y, "Enter", "Choose", s, t); if (s_setup_step == 0) x += hint(x, hints_y, "Drop", ".iso onto the window", s, t); x += hint(x, hints_y, "Esc", "Back", s, t); }
}

// ---- input
static std::vector<int> setup_buttons() {                       // the footer's buttons in order (keyboard focus)
    if (s_setup_step == 0) { std::vector<int> v{SB_CHOOSE}; if (s_launch_ok) v.push_back(SB_NEXT); return v; }
    if (s_setup_step == 1) {
        const int ls = features_load_state(); std::vector<int> v{SB_BACK};
        if (ls == 3) v.push_back(SB_REFRESH);
        v.push_back(ls == 2 && features_changes_pending() ? SB_INSTALL : SB_SKIP);
        return v;
    }
    const FeatureProgress p = features_progress();
    if (features_busy()) return {SB_CANCEL};
    if (p.phase == FP_DONE) return {SB_PLAY, SB_FEATURES};
    return {SB_RETRY, SB_FEATURES};
}
static void setup_press(int id) {
    switch (id) {
    case SB_CHOOSE: s_action = ACT_IMPORT; break;
    case SB_NEXT: setup_open(1, s_setup_first); break;
    case SB_BACK: setup_open(0, s_setup_first); break;
    case SB_REFRESH: features_load_async(true); break;
    case SB_SKIP: setup_finish(); break;
    case SB_INSTALL: case SB_RETRY: {
        uint64_t dl, add, fr; features_plan(dl, add, fr);
        s_setup_done_t = 0; features_apply(); s_setup_step = 2; s_setup_btn = -1; view_changed(); break; }
    case SB_CANCEL: features_cancel(); break;
    case SB_PLAY: setup_finish(); s_action = ACT_PLAY; break;
    case SB_FEATURES: setup_open(1, s_setup_first); break;
    case 99: s_action = ACT_QUIT; break;
    }
}
static void setup_nav(int key) {          // 0 up 1 down 2 left 3 right 4 ok 5 back 6 tab 7 shift-tab
    std::vector<int> b = setup_buttons();
    if (key == 5) {
        if (s_setup_step == 2) { if (!features_busy()) setup_open(1, s_setup_first); return; }
        if (s_setup_step == 1 && s_setup_btn >= 0) { s_setup_btn = -1; return; }
        if (s_setup_step == 1) { setup_open(0, s_setup_first); return; }
        if (s_setup_first && !s_launch_ok) return;                   // nowhere to go back to yet
        setup_finish(); return;
    }
    if (s_setup_step == 1 && s_setup_btn < 0 && features_load_state() == 2) {   // the list
        std::vector<FeatRow> rows = feat_rows(); const int n = (int)rows.size();
        auto step = [&](int d) { for (int i = s_feat_sel + d; i >= 0 && i < n; i += d) if (!rows[i].header) { s_feat_sel = i; return; } };
        switch (key) {
        case 0: step(-1); return;
        case 1: step(1); return;
        case 4: case 3: case 2: if (s_feat_sel < n && !rows[s_feat_sel].header) { const Feature& f = features()[rows[s_feat_sel].feat]; features_select(f.id, key == 2 ? false : key == 3 ? true : !f.selected); } return;
        case 6: s_setup_btn = b.back(); return;
        default: return;
        }
    }
    // buttons
    int i = (int)(std::find(b.begin(), b.end(), s_setup_btn) - b.begin());
    if (i >= (int)b.size()) i = (int)b.size() - 1;
    switch (key) {
    case 2: case 7: if (s_setup_btn < 0) s_setup_btn = b.back(); else if (i > 0) s_setup_btn = b[i - 1]; break;
    case 3: case 6: if (s_setup_btn < 0) s_setup_btn = b.back(); else if (i + 1 < (int)b.size()) s_setup_btn = b[i + 1]; break;
    case 0: if (s_setup_step == 1) s_setup_btn = -1; break;
    case 4: if (s_setup_btn < 0) s_setup_btn = b.back(); setup_press(s_setup_btn); break;
    default: break;
    }
}
static void setup_click(const Hit& h) {
    if (h.kind == 21) { setup_press(h.a); return; }
    if (h.kind == 20) {
        std::vector<FeatRow> rows = feat_rows();
        if (h.a < (int)rows.size() && !rows[h.a].header) {
            const Feature& f = features()[rows[h.a].feat];
            if (s_feat_sel == h.a || (s_mouse_x < h.x + 60 * s_scale)) features_select(f.id, !f.selected);     // the box, or a second click
            s_feat_sel = h.a; s_setup_btn = -1;
        }
        return;
    }
    if (h.kind == 23) {                                          // a group: all of it, or none
        std::vector<FeatRow> rows = feat_rows(); if (h.a >= (int)rows.size()) return;
        bool all = true; for (const Feature& f : features()) if (f.group == rows[h.a].group && !f.selected) all = false;
        std::vector<std::string> ids; for (const Feature& f : features()) if (f.group == rows[h.a].group) ids.push_back(f.id);
        for (const std::string& id : ids) features_select(id, !all);
    }
}
