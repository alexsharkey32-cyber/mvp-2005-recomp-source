// Presenters draw the game picture (with scaling / anti-aliasing / post effects) and the UI batch into
// the window. OpenGL is preferred; SDL_Renderer is the fallback (no post effects).
#pragma once
#include "ee_context.h"
#include "ui.h"
#include <vector>

struct PostParams {
    int scale_filter = 1;      // 0 nearest, 1 bilinear, 2 bicubic, 3 area (best when shrinking a high-res image)
    int aa = 0;                // 0 off, 1 FXAA, 2 FXAA high quality
    float sharpen = 0;         // 0..1 (contrast adaptive sharpening)
    float brightness = 0;      // -0.5..0.5
    float contrast = 1;        // 0.5..1.5
    float saturation = 1;      // 0..2
    float gamma = 1;           // 0.5..2
    int crt = 0;               // 0 off, 1 scanlines, 2 CRT (scanlines + curvature + vignette)
    int native_lines = 448;    // lines of the original picture (scanline spacing)
};
struct DstRect { int x, y, w, h; };

enum { kUiImages = 96 };
class Presenter {
public:
    virtual ~Presenter() {}
    virtual const char* name() const = 0;
    virtual void set_game(const u32* rgba, int w, int h) = 0;      // new game picture (RGBA, top-down)
    virtual void set_vsync(bool on) = 0;
    // game picture from the GPU renderer's target (gpu_target_handle: GL framebuffer / MTLTexture / VkImage): rect in its pixels, row 0 = top
    virtual bool set_game_gpu(uintptr_t fbo, int x0, int y0, int w, int h) { (void)fbo; (void)x0; (void)y0; (void)w; (void)h; return false; }
    virtual void drawable_size(int& w, int& h) = 0;
    // images for UiBatch::image (id 0..kUiImages-1); rgba top-down, linear filtered. w = 0 frees it.
    virtual void set_ui_image(int id, const u32* rgba, int w, int h) { (void)id; (void)rgba; (void)w; (void)h; }
    // draws one window frame: game picture into dst (drawable pixels), then the UI
    virtual void frame(int draw_w, int draw_h, const DstRect& dst, const PostParams& pp, const UiBatch& ui) = 0;
    void request_capture() { capture = true; }                               // grab the next frame()
    virtual bool read_window(std::vector<u32>& rgba, int& w, int& h) = 0;   // the captured frame (RGBA, top-down)
protected:
    bool capture = false;
};
struct SDL_Window;
Presenter* presenter_gl_create(SDL_Window* win);    // null if OpenGL 3.3 core is unavailable
Presenter* presenter_sdl_create(SDL_Window* win);
Presenter* presenter_metal_create(SDL_Window* win);  // Apple only; null without an Apple GPU
Presenter* presenter_vk_create(SDL_Window* win);     // MVP_VULKAN builds; null without a Vulkan 1.1 device
