// Immediate-mode UI drawing: a list of quads (rounded rectangles, soft shadows, text glyphs) that a
// presenter backend (OpenGL or SDL_Renderer fallback) draws on top of the game picture.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

enum UiKind : uint8_t { UI_RECT = 0, UI_GLYPH = 1, UI_SHADOW = 2, UI_IMAGE = 3, UI_GRADIENT = 4 };
enum UiFont : uint8_t { FONT_REGULAR = 0, FONT_BOLD = 1, FONT_DISPLAY = 2 };   // text, emphasis, condensed headings
struct UiQuad {
    float x, y, w, h;          // pixels (drawable)
    float u0, v0, u1, v1;      // glyph atlas coordinates (normalized) for UI_GLYPH
    uint32_t color;            // 0xAARRGGBB
    float radius;              // corner radius (UI_RECT / UI_SHADOW)
    float soft;                // shadow blur size (UI_SHADOW), border width (UI_RECT, 0 = filled)
    UiKind kind;
};
struct UiBatch {
    std::vector<UiQuad> q;
    size_t under = 0;          // the first `under` quads are drawn before (under) the game picture, the rest over it
    void clear() { q.clear(); under = 0; }
    void mark_under() { under = q.size(); }
    // colour with its alpha ramped from a0 to a1 across the rectangle (vertical: top to bottom, else left to right)
    void gradient(float x, float y, float w, float h, uint32_t color, float a0, float a1, bool vertical, float radius = 0);
    void rect(float x, float y, float w, float h, uint32_t color, float radius = 0);
    void outline(float x, float y, float w, float h, uint32_t color, float radius, float width);
    void shadow(float x, float y, float w, float h, float radius, float blur, uint32_t color);
    // image set with Presenter::set_ui_image(id, ...): the u/v sub-rectangle drawn into x,y,w,h, tinted by color
    void image(int id, float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t color = 0xffffffffu, float radius = 0);
    // text: returns the advance width. size = pixel height of the font's em.
    float text(float x, float y, const std::string& s, float size, uint32_t color, UiFont font = FONT_REGULAR, float spacing = 0);
};
float ui_text_width(const std::string& s, float size, UiFont font = FONT_REGULAR, float spacing = 0);
float ui_line_height(float size);
// word-wrapped text; returns the height used
float ui_text_wrapped(UiBatch& b, float x, float y, float maxw, const std::string& s, float size, uint32_t color, UiFont font = FONT_REGULAR);

// Font atlas (the fonts stacked vertically, 8-bit coverage), built once from the embedded RLE data.
struct UiAtlas { int w = 0, h = 0; std::vector<uint8_t> px; int bold_y = 0; };
const UiAtlas& ui_atlas();

inline uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) { return (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | b; }
inline uint32_t with_alpha(uint32_t c, float a) { uint32_t al = (uint32_t)((c >> 24) * (a < 0 ? 0 : a > 1 ? 1 : a)); return (c & 0xffffff) | al << 24; }
