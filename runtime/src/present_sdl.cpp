// Fallback presenter on SDL_Renderer (used when OpenGL 3.3 isn't available): bilinear/nearest scaling and
// the UI (square corners, no shadows). No anti-aliasing or post effects.
#ifdef MVP_SDL
#define SDL_MAIN_HANDLED
#include <algorithm>
#include <SDL.h>
#include "present.h"
#include <cstring>

class SDLPresenter : public Presenter {
public:
    SDL_Window* win = nullptr; SDL_Renderer* ren = nullptr;
    SDL_Texture* game = nullptr; SDL_Texture* font = nullptr;
    int gw = 0, gh = 0;
    std::vector<u32> captured; int cw = 0, ch = 0;
    const char* name() const override { return "SDL renderer"; }
    bool init(SDL_Window* w) {
        win = w;
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
        if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
        if (!ren) return false;
        const UiAtlas& a = ui_atlas();
        std::vector<u32> px((size_t)a.w * a.h);
        for (size_t i = 0; i < px.size(); i++) px[i] = 0x00ffffffu | ((u32)a.px[i] << 24);
        font = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, a.w, a.h);
        SDL_UpdateTexture(font, nullptr, px.data(), a.w * 4);
        SDL_SetTextureBlendMode(font, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(font, SDL_ScaleModeLinear);
        return true;
    }
    void set_vsync(bool on) override { SDL_RenderSetVSync(ren, on ? 1 : 0); }
    void drawable_size(int& w, int& h) override { SDL_GetRendererOutputSize(ren, &w, &h); }
    void set_game(const u32* px, int w, int h) override {
        if (!game || w != gw || h != gh) {
            if (game) SDL_DestroyTexture(game);
            game = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
            gw = w; gh = h;
        }
        SDL_UpdateTexture(game, nullptr, px, w * 4);
    }
    void frame(int dw, int dh, const DstRect& dst, const PostParams& pp, const UiBatch& ui) override {
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        draw_ui(ui, 0, ui.under);
        if (game) {
            SDL_SetTextureScaleMode(game, pp.scale_filter == 0 ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
            SDL_Rect r = {dst.x, dst.y, dst.w, dst.h};
            SDL_RenderCopy(ren, game, nullptr, &r);
        }
        draw_ui(ui, ui.under, ui.q.size());
        if (capture) {
            capture = false;
            cw = dw; ch = dh; captured.assign((size_t)dw * dh, 0);
            SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ABGR8888, captured.data(), dw * 4);
        }
        SDL_RenderPresent(ren);
    }
    void draw_ui(const UiBatch& ui, size_t first, size_t last) {
        const UiAtlas& a = ui_atlas();
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        for (size_t qi = first; qi < last; qi++) { const UiQuad& q = ui.q[qi];
            u8 r = (q.color >> 16) & 0xff, g = (q.color >> 8) & 0xff, b = q.color & 0xff, al = q.color >> 24;
            if (q.kind == UI_RECT) {
                SDL_SetRenderDrawColor(ren, r, g, b, al);
                if (q.soft > 0) {
                    SDL_Rect e[4] = {{(int)q.x, (int)q.y, (int)q.w, (int)q.soft}, {(int)q.x, (int)(q.y + q.h - q.soft), (int)q.w, (int)q.soft},
                                     {(int)q.x, (int)q.y, (int)q.soft, (int)q.h}, {(int)(q.x + q.w - q.soft), (int)q.y, (int)q.soft, (int)q.h}};
                    SDL_RenderFillRects(ren, e, 4);
                } else { SDL_Rect rr = {(int)q.x, (int)q.y, (int)(q.w + 0.5f), (int)(q.h + 0.5f)}; SDL_RenderFillRect(ren, &rr); }
            } else if (q.kind == UI_GLYPH) {
                SDL_SetTextureColorMod(font, r, g, b);
                SDL_SetTextureAlphaMod(font, al);
                SDL_Rect src = {(int)(q.u0 * a.w + 0.5f), (int)(q.v0 * a.h + 0.5f), (int)((q.u1 - q.u0) * a.w + 0.5f), (int)((q.v1 - q.v0) * a.h + 0.5f)};
                SDL_FRect d = {q.x, q.y, q.w, q.h};
                SDL_RenderCopyF(ren, font, &src, &d);
            } else if (q.kind == UI_GRADIENT) {                    // no shaders here: a few bands
                const bool vert = q.soft > 0.5f; const float a0 = vert ? q.v0 : q.u0, a1 = vert ? q.v1 : q.u1;
                for (int k = 0; k < 8; k++) {
                    float t = (k + 0.5f) / 8, al2 = al * (a0 + (a1 - a0) * t);
                    SDL_SetRenderDrawColor(ren, r, g, b, (u8)std::min(255.0f, al2));
                    SDL_FRect rr = vert ? SDL_FRect{q.x, q.y + q.h * k / 8, q.w, q.h / 8 + 1} : SDL_FRect{q.x + q.w * k / 8, q.y, q.w / 8 + 1, q.h};
                    SDL_RenderFillRectF(ren, &rr);
                }
            }
        }
    }
    bool read_window(std::vector<u32>& px, int& w, int& h) override {
        if (captured.empty()) return false;
        px = captured; w = cw; h = ch; return true;
    }
};

Presenter* presenter_sdl_create(SDL_Window* win) {
    SDLPresenter* p = new SDLPresenter();
    if (!p->init(win)) { delete p; return nullptr; }
    return p;
}
#endif
