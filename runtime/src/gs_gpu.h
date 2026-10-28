// GPU (OpenGL 3.3) renderer for the game's main 3D/UI target. gs.cpp keeps the GS state machine, VRAM
// and the software rasterizer; primitives that draw into the main target are also (or instead) sent here
// and rendered at the internal resolution. All calls happen on the thread that owns the GL context.
#pragma once
#include "ee_context.h"
#include <vector>

struct GpuVtx { float x, y, z; float r, g, b, a; float s, t, q; float u, v; float f; };   // same layout as gs.cpp's Vtx

// A sprite drawn at a higher internal resolution: the pixels added between the console's own sample points
// run up to one texel past the last of them, to the right and downwards. On a font page or an icon sheet that
// texel belongs to the next glyph or to the filler between the cells, and it shows as a faint box around every
// letter. So the texture coordinate is held inside what the console sampled: [lo, hi] in native texels along one
// axis, from the pixel positions p0/p1 of the two corners and their texel coordinates c0/c1 (hs: how many
// texels of a replacement texture make one native texel; all of those of the outermost texels stay in reach).
// Large pieces (backdrops cut into tiles that continue each other) are left alone.
#include <cmath>
#include <algorithm>
static inline bool sprite_tex_range(float p0, float p1, float c0, float c1, float hs, float& lo, float& hi) {
    if (p0 == p1 || std::fabs(c1 - c0) > 64.0f) return false;
    float n0 = std::ceil(std::min(p0, p1)), n1 = std::ceil(std::max(p0, p1)) - 1;
    if (n1 < n0) return false;
    float k = (c1 - c0) / (p1 - p0), ca = c0 + (n0 - p0) * k, cb = c0 + (n1 - p0) * k, e = 0.5f - 0.5f / std::max(hs, 1.0f);
    lo = std::min(ca, cb) - e; hi = std::max(ca, cb) + e;
    return true;
}

// Everything that affects how a primitive is drawn. Zero-initialise (memset) before filling: batches are
// compared with memcmp.
struct GpuState {
    u8 kind;                         // 0 point, 1 line, 2 triangle, 3 sprite
    u8 tme, fst, iip, fge, abe, bil, tfx, tcc, wms, wmt, zmsk, fba, colclamp, zpsm, pad0;
    u16 minu, maxu, minv, maxv;
    u32 tex; u16 tex_w, tex_h;       // GL texture (decoded RGBA8, 2^tw x 2^th)
    u8 hsx, hsy, pad1, pad2;         // HD texture: GL texture is tex_w*hsx x tex_h*hsy (1 = native)
    u32 test, alpha, fbmsk, fogcol;
    int sx0, sy0, sx1, sy1;          // scissor, native pixels (inclusive)
};

// Graphics APIs. The window's presenter picks one (frontend_init); the renderer uses the same one.
enum GpuApi { GPU_API_GL = 0, GPU_API_METAL = 1, GPU_API_VULKAN = 2 };
extern int g_gpu_api;
const char* gpu_api_name(int api);

bool gpu_available();                           // the renderer was set up (gpu_init succeeded)
bool gpu_init();                                // needs the presenter's context/device; false if shaders fail
bool gpu_init_offscreen();                      // headless tools (--gs-replay): hidden SDL window + context
void gpu_set_target(int w_native, int h_native, int scale);   // (re)allocate the target; clears it
void gpu_upload_target(const u32* rgba, int w_native, int h_native);   // native VRAM image -> whole target (nearest)
void gpu_prim(const GpuState& st, const GpuVtx* v, int nv);
void gpu_flush();                               // execute queued batches
bool gpu_read(int x0, int y0, int w, int h, std::vector<u32>& out);   // target pixels (scaled coords), top-down RGBA
uintptr_t gpu_target_handle();                  // the target for the presenter: GL framebuffer id / id<MTLTexture> / VkImage
unsigned gpu_tex_create(const u32* rgba, int w, int h);
void gpu_tex_free(unsigned id);
void gpu_set_aniso(int level);                  // 0/1 off, 2..16 anisotropic filtering                 // deleted after the batches using it have run
void gpu_stats();
// native-resolution copies of the whole target (VRAM <-> GPU): colour RGBA8 and depth as z / 2^32
bool gpu_read_native(std::vector<u32>& col, std::vector<float>* depth);
void gpu_write_native(const u32* col, const float* depth);   // depth may be null (colour only)

// ---- backends: gs_gpu.cpp batches primitives (GpuBatch) and hands each flush to one of these
struct GpuBatch {
    GpuState st;
    int mode;                        // 0 points, 1 lines, 2 triangles
    u32 first, count;                // vertices (17 floats each: x y z(NDC xy, GS z) r g b a s t q u v fog clamp4)
    int date;                        // destination alpha test through the stencil: 0 off, 1 keep, 2 a pass clears the pixel's stencil
    int copy;                        // needs a copy of the target: 1 for the batch (DATE), 2 per primitive (exact blending)
    int bx0, by0, bx1, by1;          // scaled bounding box (copy/date)
};
enum { GPU_VTX_FLOATS = 17 };
struct GpuBackend {
    virtual ~GpuBackend() {}
    virtual const char* name() const = 0;
    // fbmsk at bit level, DATE against every earlier write, exact blending without copies (framebuffer fetch)
    virtual bool exact_fetch() const { return false; }
    // no dual-source blending (some Vulkan drivers): every blended primitive goes through the target copy
    virtual bool dual_source() const { return true; }
    virtual void set_target(int w, int h, int wn, int hn, int scale) = 0;   // allocate + clear (colour 0, depth 0)
    virtual void upload_target(const u32* rgba, int wn, int hn) = 0;        // native image -> target, nearest
    virtual void execute(const float* verts, size_t nverts, const GpuBatch* b, size_t nb) = 0;
    virtual void end_flush() {}                                             // free textures released since the last flush
    virtual bool read(int x0, int y0, int w, int h, std::vector<u32>& out) = 0;
    virtual bool read_native(std::vector<u32>& col, std::vector<float>* depth) = 0;
    virtual void write_native(const u32* col, const float* depth) = 0;
    virtual unsigned tex_create(const u32* rgba, int w, int h) = 0;
    virtual void tex_free(unsigned id) = 0;                                 // deferred to end_flush
    virtual void set_aniso(float level) = 0;
    virtual uintptr_t target_handle() = 0;
};
extern u64 g_gpu_stat_copies;                  // target copies made by a backend (stats)
GpuBackend* gpu_backend_gl();
GpuBackend* gpu_backend_metal();     // null unless built for Apple and the Metal presenter is running
GpuBackend* gpu_backend_vulkan();    // null unless built with Vulkan and the Vulkan presenter is running
