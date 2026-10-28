// UI batch building and text layout (see ui.h).
#include "ui.h"
#include "ui_font.h"
#include <cmath>
#include <cstring>

static UiAtlas s_atlas;
static bool s_atlas_built = false;
static void unrle(const unsigned char* rle, size_t n, uint8_t* out, size_t out_n) {
    size_t o = 0;
    for (size_t i = 0; i < n && o < out_n; i++) {
        if (rle[i] == 0 && i + 1 < n) { size_t run = rle[++i]; for (size_t k = 0; k < run && o < out_n; k++) out[o++] = 0; }
        else out[o++] = rle[i];
    }
}
struct FontDesc { const UiGlyph* g; size_t n; int y; };
static FontDesc s_font[3];
const UiAtlas& ui_atlas() {
    if (!s_atlas_built) {
        s_atlas_built = true;
        s_atlas.w = kFontRegularW;
        const int y1 = kFontRegularH, y2 = y1 + kFontBoldH;
        s_atlas.bold_y = y1;
        s_atlas.h = y2 + kFontDisplayH;
        s_atlas.px.assign((size_t)s_atlas.w * s_atlas.h, 0);
        unrle(kFontRegularRle, sizeof kFontRegularRle, s_atlas.px.data(), (size_t)kFontRegularW * kFontRegularH);
        unrle(kFontBoldRle, sizeof kFontBoldRle, s_atlas.px.data() + (size_t)s_atlas.w * y1, (size_t)kFontBoldW * kFontBoldH);
        unrle(kFontDisplayRle, sizeof kFontDisplayRle, s_atlas.px.data() + (size_t)s_atlas.w * y2, (size_t)kFontDisplayW * kFontDisplayH);
        s_font[FONT_REGULAR] = {kFontRegularGlyphs, sizeof kFontRegularGlyphs / sizeof *kFontRegularGlyphs, 0};
        s_font[FONT_BOLD] = {kFontBoldGlyphs, sizeof kFontBoldGlyphs / sizeof *kFontBoldGlyphs, y1};
        s_font[FONT_DISPLAY] = {kFontDisplayGlyphs, sizeof kFontDisplayGlyphs / sizeof *kFontDisplayGlyphs, y2};
    }
    return s_atlas;
}

static uint32_t next_cp(const std::string& s, size_t& i) {       // UTF-8
    unsigned char c = (unsigned char)s[i++];
    if (c < 0x80) return c;
    int n = (c >= 0xf0) ? 3 : (c >= 0xe0) ? 2 : 1;
    uint32_t cp = c & (0x3f >> n);
    for (int k = 0; k < n && i < s.size(); k++) cp = (cp << 6) | ((unsigned char)s[i++] & 0x3f);
    return cp;
}
static const UiGlyph* glyph(uint32_t cp, UiFont f) {
    ui_atlas();
    const FontDesc& d = s_font[f <= FONT_DISPLAY ? f : FONT_REGULAR];
    if (cp >= 32 && cp < 127) return &d.g[cp - 32];
    for (size_t i = 95; i < d.n; i++) if (d.g[i].cp == cp) return &d.g[i];
    return &d.g['?' - 32];
}

void UiBatch::rect(float x, float y, float w, float h, uint32_t color, float radius) {
    if ((color >> 24) == 0 || w <= 0 || h <= 0) return;
    q.push_back({x, y, w, h, 0, 0, 0, 0, color, radius, 0, UI_RECT});
}
void UiBatch::image(int id, float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t color, float radius) {
    if ((color >> 24) == 0 || w <= 0 || h <= 0) return;
    q.push_back({x, y, w, h, u0, v0, u1, v1, color, radius, (float)id, UI_IMAGE});
}
void UiBatch::gradient(float x, float y, float w, float h, uint32_t color, float a0, float a1, bool vertical, float radius) {
    if (w <= 0 || h <= 0 || (a0 <= 0 && a1 <= 0)) return;
    // the alpha ramp rides in the texture coordinates: u (horizontal) or v (vertical) runs a0 -> a1
    if (vertical) q.push_back({x, y, w, h, 0, a0, 0, a1, color, radius, 1, UI_GRADIENT});
    else q.push_back({x, y, w, h, a0, 0, a1, 0, color, radius, 0, UI_GRADIENT});
}
void UiBatch::outline(float x, float y, float w, float h, uint32_t color, float radius, float width) {
    if ((color >> 24) == 0) return;
    q.push_back({x, y, w, h, 0, 0, 0, 0, color, radius, width, UI_RECT});
}
void UiBatch::shadow(float x, float y, float w, float h, float radius, float blur, uint32_t color) {
    q.push_back({x - blur, y - blur, w + 2 * blur, h + 2 * blur, 0, 0, 0, 0, color, radius, blur, UI_SHADOW});
}
float UiBatch::text(float x, float y, const std::string& s, float size, uint32_t color, UiFont font, float spacing) {
    const UiAtlas& a = ui_atlas();
    const float scale = size / kFontRegularSize;
    const int yoff_atlas = s_font[font <= FONT_DISPLAY ? font : FONT_REGULAR].y;
    float pen = x;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = next_cp(s, i);
        const UiGlyph* g = glyph(cp, font);
        if (g->w > 0) {
            float gx = std::floor(pen + g->xoff * scale + 0.5f), gy = std::floor(y + g->yoff * scale + 0.5f);
            q.push_back({gx, gy, g->w * scale, g->h * scale,
                         (float)g->x / a.w, (float)(g->y + yoff_atlas) / a.h, (float)(g->x + g->w) / a.w, (float)(g->y + g->h + yoff_atlas) / a.h,
                         color, 0, 0, UI_GLYPH});
        }
        pen += g->adv * scale + spacing;
    }
    return pen - x;
}
float ui_text_width(const std::string& s, float size, UiFont font, float spacing) {
    float w = 0; int n = 0;
    for (size_t i = 0; i < s.size();) { w += glyph(next_cp(s, i), font)->adv; n++; }
    return w * size / kFontRegularSize + n * spacing;
}
float ui_line_height(float size) { return (kFontRegularAscent + kFontRegularDescent) * size / kFontRegularSize; }
float ui_text_wrapped(UiBatch& b, float x, float y, float maxw, const std::string& s, float size, uint32_t color, UiFont font) {
    std::string line; float yy = y;
    size_t p = 0;
    while (p <= s.size()) {
        size_t q = s.find(' ', p); if (q == std::string::npos) q = s.size();
        std::string word = s.substr(p, q - p);
        std::string cand = line.empty() ? word : line + " " + word;
        if (!line.empty() && ui_text_width(cand, size, font) > maxw) { b.text(x, yy, line, size, color, font); yy += ui_line_height(size); line = word; }
        else line = cand;
        p = q + 1;
    }
    if (!line.empty()) { b.text(x, yy, line, size, color, font); yy += ui_line_height(size); }
    return yy - y;
}
