// GPU renderer for the main target (see gs_gpu.h). This file batches primitives for every backend and holds
// the OpenGL backend. The fragment shader reproduces the software rasterizer's per-pixel maths (texture wrap
// modes, integer bilinear, TFX, fog, alpha test) so the two can be compared on GS replays. GL blending uses
// dual-source blending for the GS formula ((A-B)*C>>7)+D; the cases fixed-function blending can't express
// (C = destination alpha, destination alpha test) read a copy of the target instead, one primitive at a time.
// Backends with framebuffer fetch (Metal on Apple GPUs) blend, mask and test in the shader instead.
#include "gs_gpu.h"
#include "ee_runtime.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <vector>

int g_gpu_api = GPU_API_GL;
const char* gpu_api_name(int api) { return api == GPU_API_METAL ? "Metal" : api == GPU_API_VULKAN ? "Vulkan" : "OpenGL"; }

// ---------------------------------------------------------------- shared batching
static GpuBackend* s_be = nullptr;
static int s_w = 0, s_h = 0, s_scale = 1, s_nw = 0, s_nh = 0;
static bool s_has_target = false;
static std::vector<float> s_verts;
static std::vector<GpuBatch> s_batches;
static u64 s_stat_prims = 0, s_stat_batches = 0, s_stat_flushes = 0, s_stat_reads = 0, s_stat_writes = 0;
u64 g_gpu_stat_copies = 0;
// MVP_GPU_PROF=1: every 5 s, where the time goes on the GPU side (wall clock on the emulation thread)
#include <chrono>
static const bool s_prof = std::getenv("MVP_GPU_PROF") != nullptr;
static double s_t_exec = 0, s_t_read = 0, s_t_tex = 0, s_t0 = -1; static u64 s_n_tex = 0, s_n_read = 0;
double g_gpu_t_present = 0;                     // frontend: presenter frame() time
static inline double prof_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static void prof_tick() {
    const double now = prof_ms();
    if (s_t0 < 0) s_t0 = now;
    if (now - s_t0 < 5000) return;
    rlog("gpu", "%.0f ms: flushes %.0f ms, read-backs %.0f ms (%llu), texture uploads %.0f ms (%llu), present %.0f ms", now - s_t0, s_t_exec, s_t_read,
         (unsigned long long)s_n_read, s_t_tex, (unsigned long long)s_n_tex, g_gpu_t_present);
    s_t0 = now; s_t_exec = s_t_read = s_t_tex = g_gpu_t_present = 0; s_n_tex = s_n_read = 0;
}
struct ProfScope { double& acc; double t; ProfScope(double& a) : acc(a), t(s_prof ? prof_ms() : 0) {} ~ProfScope() { if (s_prof) acc += prof_ms() - t; } };

bool gpu_available() { return s_be != nullptr; }
bool gpu_init() {
    if (s_be) return true;
    if (std::getenv("MVP_NO_GPU")) return false;
    GpuBackend* b = g_gpu_api == GPU_API_METAL ? gpu_backend_metal() : g_gpu_api == GPU_API_VULKAN ? gpu_backend_vulkan() : gpu_backend_gl();
    if (!b) return false;
    s_be = b;
    rlog("gpu", "GPU renderer ready (%s)", b->name());
    return true;
}
#ifdef MVP_SDL
#define SDL_MAIN_HANDLED
#include <SDL.h>
#endif
bool gpu_init_offscreen() {
    if (s_be) return true;
#ifdef MVP_SDL
    SDL_SetMainReady();
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "[gpu] SDL video: %s\n", SDL_GetError()); return false; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window* w = SDL_CreateWindow("gpu", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!w) { fprintf(stderr, "[gpu] window: %s\n", SDL_GetError()); return false; }
    if (!SDL_GL_CreateContext(w)) { fprintf(stderr, "[gpu] context: %s\n", SDL_GetError()); return false; }
    extern bool gl_load(void* (*)(const char*));
    if (!gl_load(SDL_GL_GetProcAddress)) return false;
    g_gpu_api = GPU_API_GL;
    return gpu_init();
#else
    return false;
#endif
}

void gpu_set_target(int wn, int hn, int scale) {
    if (!s_be) return;
    gpu_flush();
    s_scale = scale; s_nw = wn; s_nh = hn; s_w = wn * scale; s_h = hn * scale;
    s_be->set_target(s_w, s_h, wn, hn, scale);
    s_has_target = true;
    rlog("gpu", "main target %dx%d (%dx, %s)", s_w, s_h, scale, s_be->name());
}
void gpu_upload_target(const u32* px, int wn, int hn) { if (!s_be || !s_has_target) return; gpu_flush(); s_be->upload_target(px, wn, hn); }
bool gpu_read_native(std::vector<u32>& col, std::vector<float>* depth) {
    if (!s_be || !s_has_target) return false;
    gpu_flush(); s_stat_reads++;
    ProfScope ps(s_t_read); s_n_read++;
    return s_be->read_native(col, depth);
}
void gpu_write_native(const u32* col, const float* depth) {
    if (!s_be || !s_has_target) return;
    gpu_flush(); s_stat_writes++;
    s_be->write_native(col, depth);
}
unsigned gpu_tex_create(const u32* px, int w, int h) { if (!s_be) return 0; ProfScope ps(s_t_tex); s_n_tex++; return s_be->tex_create(px, w, h); }
void gpu_tex_free(unsigned id) { if (s_be && id) s_be->tex_free(id); }
void gpu_set_aniso(int level) { if (s_be) s_be->set_aniso(level > 1 ? (float)level : 0.0f); }
uintptr_t gpu_target_handle() { return s_be && s_has_target ? s_be->target_handle() : 0; }
bool gpu_read(int x0, int y0, int w, int h, std::vector<u32>& out) {
    if (!s_be || !s_has_target || w <= 0 || h <= 0) return false;
    gpu_flush();
    ProfScope ps(s_t_read); s_n_read++;
    return s_be->read(x0, y0, w, h, out);
}
void gpu_flush() {
    if (!s_be) return;
    if (!s_batches.empty()) {
        s_stat_flushes++; s_stat_batches += s_batches.size();
        ProfScope ps(s_t_exec);
        s_be->execute(s_verts.data(), s_verts.size() / GPU_VTX_FLOATS, s_batches.data(), s_batches.size());
        s_batches.clear(); s_verts.clear();
    }
    s_be->end_flush();
    if (s_prof) prof_tick();
}
void gpu_stats() {
    if (!s_be) return;
    fprintf(stderr, "GPU (%s): %llu primitives in %llu batches, %llu flushes, %llu target copies, %llu read-backs, %llu re-uploads\n", s_be->name(),
            (unsigned long long)s_stat_prims, (unsigned long long)s_stat_batches, (unsigned long long)s_stat_flushes,
            (unsigned long long)g_gpu_stat_copies, (unsigned long long)s_stat_reads, (unsigned long long)s_stat_writes);
}

// 0: no copy of the target needed. 1: destination alpha test, reads the alpha as it was before the batch
// (primitives in one batch don't see each other, like PCSX2's fast DATE). 2: blending that fixed-function
// hardware can't do (C = destination alpha, wrap-around colours), one primitive at a time.
static inline int needs_copy(const GpuState& st) {
    if (st.abe && ((st.alpha >> 4) & 3) == 1) return 2;
    if (st.abe && !st.colclamp) return 2;
    return 0;
}
// DATE: which stencil update keeps later primitives right. A written pixel whose new alpha bit 7 differs from
// DATM must fail the test for the rest of the batch (the GS "draw each pixel once" trick used for shadows).
// Returns 1 (alpha bit 7 stays as DATM wants), 2 (it flips), 0 (varies per pixel: one primitive per batch).
static int date_mode(const GpuState& st, const GpuVtx* v, int nv) {
    const u32 datm = (st.test >> 15) & 1;
    if ((st.fbmsk >> 24) == 0xff) return 1;                                  // alpha not written
    if (st.fba) return datm == 1 ? 1 : 2;
    if (st.tme && st.tcc) return 0;                                          // texture alpha
    u32 b = ((u32)v[0].a >> 7) & 1;
    for (int i = 1; i < nv; i++) if ((((u32)v[i].a >> 7) & 1) != b) return 0;
    return b == datm ? 1 : 2;
}
static const float kNoClamp[4] = {1, 1, 0, 0};
static const bool s_sprite_clamp = !std::getenv("MVP_NO_SPRITE_CLAMP");
static inline void push_vtx(float x, float y, float s, float t, float u, float vv, const GpuVtx& attr, float z, float f, const float* cl = kNoClamp) {
    const float S = (float)s_scale;
    float nx = (x * S + 0.5f) * 2.0f / (float)s_w - 1.0f, ny = (y * S + 0.5f) * 2.0f / (float)s_h - 1.0f;
    size_t o = s_verts.size();
    if (s_verts.capacity() < o + GPU_VTX_FLOATS) s_verts.reserve(std::max<size_t>(o * 2, GPU_VTX_FLOATS * 65536));
    s_verts.resize(o + GPU_VTX_FLOATS);
    float* d = &s_verts[o];
    d[0] = nx; d[1] = ny; d[2] = z; d[3] = attr.r; d[4] = attr.g; d[5] = attr.b; d[6] = attr.a;
    d[7] = s; d[8] = t; d[9] = attr.q; d[10] = u; d[11] = vv; d[12] = f;
    d[13] = cl[0]; d[14] = cl[1]; d[15] = cl[2]; d[16] = cl[3];
}
void gpu_prim(const GpuState& st, const GpuVtx* v, int nv) {
    if (!s_be || !s_has_target) return;
    s_stat_prims++;
    int mode = st.kind == 0 ? 0 : st.kind == 1 ? 1 : 2;
    const bool fetch = s_be->exact_fetch();
    int copy = fetch ? 0 : needs_copy(st);
    if (!fetch && !copy && st.abe && !s_be->dual_source()) copy = 2;
    int date = (st.test & (1 << 14)) ? (fetch ? 1 : date_mode(st, v, nv)) : 0;
    const bool date_single = (st.test & (1 << 14)) && date == 0;         // unknown: its own batch
    if (date_single) date = 2;
    u32 first = (u32)(s_verts.size() / GPU_VTX_FLOATS);
    if (st.kind == 3) {                                       // sprite -> two triangles, attributes of the 2nd vertex
        const GpuVtx& a = v[0]; const GpuVtx& b = v[1];
        const float xs[2] = {a.x, b.x}, ys[2] = {a.y, b.y}, ss[2] = {a.s, b.s}, ts[2] = {a.t, b.t}, us[2] = {a.u, b.u}, vs[2] = {a.v, b.v};
        static const int ci[6][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1}};
        float cl[4] = {1, 1, 0, 0};
        if (st.tme && s_scale > 1 && s_sprite_clamp) {
            float lo, hi;
            if (st.fst) {
                if (sprite_tex_range(a.x, b.x, a.u, b.u, (float)st.hsx, lo, hi)) { cl[0] = lo; cl[2] = hi; }
                if (sprite_tex_range(a.y, b.y, a.v, b.v, (float)st.hsy, lo, hi)) { cl[1] = lo; cl[3] = hi; }
            } else if (b.q != 0) {
                const float kw = (float)st.tex_w / b.q, kh = (float)st.tex_h / b.q;
                if (sprite_tex_range(a.x, b.x, a.s * kw, b.s * kw, (float)st.hsx, lo, hi)) { cl[0] = lo; cl[2] = hi; }
                if (sprite_tex_range(a.y, b.y, a.t * kh, b.t * kh, (float)st.hsy, lo, hi)) { cl[1] = lo; cl[3] = hi; }
            }
        }
        for (auto& c : ci) push_vtx(xs[c[0]], ys[c[1]], ss[c[0]], ts[c[1]], us[c[0]], vs[c[1]], b, b.z, b.f, cl);
    } else {
        for (int i = 0; i < nv; i++) push_vtx(v[i].x, v[i].y, v[i].s, v[i].t, v[i].u, v[i].v, v[i], v[i].z, v[i].f);
    }
    u32 count = (u32)(s_verts.size() / GPU_VTX_FLOATS) - first;
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    if (copy || (date && !fetch)) {
        float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
        for (int i = 1; i < nv; i++) { x0 = std::min(x0, v[i].x); x1 = std::max(x1, v[i].x); y0 = std::min(y0, v[i].y); y1 = std::max(y1, v[i].y); }
        x0 = std::max(x0, (float)st.sx0); y0 = std::max(y0, (float)st.sy0); x1 = std::min(x1, (float)st.sx1 + 1); y1 = std::min(y1, (float)st.sy1 + 1);
        bx0 = std::max(0, (int)(x0 * s_scale) - 1); by0 = std::max(0, (int)(y0 * s_scale) - 1);
        bx1 = std::min(s_w, (int)(x1 * s_scale) + 2); by1 = std::min(s_h, (int)(y1 * s_scale) + 2);
    }
    if (copy != 2 && !date_single && !s_batches.empty()) {
        GpuBatch& b = s_batches.back();
        if (b.copy == copy && b.date == date && b.mode == mode && b.first + b.count == first && std::memcmp(&b.st, &st, sizeof st) == 0) {
            b.count += count;
            if (copy || date) { b.bx0 = std::min(b.bx0, bx0); b.by0 = std::min(b.by0, by0); b.bx1 = std::max(b.bx1, bx1); b.by1 = std::max(b.by1, by1); }
            return;
        }
    }
    GpuBatch b; b.st = st; b.mode = mode; b.first = first; b.count = count; b.copy = copy; b.date = date;
    b.bx0 = bx0; b.by0 = by0; b.bx1 = bx1; b.by1 = by1;
    s_batches.push_back(b);
    if (s_verts.size() > (size_t)GPU_VTX_FLOATS * 400000) gpu_flush();
}

// ---------------------------------------------------------------- OpenGL backend
#ifdef MVP_SDL
#include "gl_funcs.h"
using namespace gl;

static const char* kGsVS = R"(#version 330 core
layout(location = 0) in vec3 a_pos; layout(location = 1) in vec4 a_col; layout(location = 2) in vec3 a_stq;
layout(location = 3) in vec2 a_uv; layout(location = 4) in float a_fog; layout(location = 5) in vec4 a_clamp;
noperspective out vec4 v_col; flat out vec4 v_fcol; noperspective out vec3 v_stq; noperspective out vec2 v_uv;
noperspective out float v_fog; noperspective out float v_z; flat out vec4 v_clamp;
void main() {
    gl_Position = vec4(a_pos.xy, 0.0, 1.0);
    v_col = a_col; v_fcol = a_col; v_stq = a_stq; v_uv = a_uv; v_fog = a_fog; v_z = a_pos.z; v_clamp = a_clamp;
})";

static const char* kGsFS = R"(#version 330 core
uniform sampler2D u_tex; uniform sampler2D u_dst;
uniform int u_tme, u_fst, u_iip, u_fge, u_tfx, u_tcc, u_bil;
uniform ivec2 u_tsize; uniform ivec2 u_wrap; uniform ivec4 u_region; uniform vec2 u_hs; uniform int u_hw;
uniform int u_atst, u_aref, u_apass;
uniform vec3 u_fogcol; uniform int u_fba;
uniform ivec4 u_blend; uniform int u_fix; uniform int u_bmode; uniform int u_date; uniform int u_colclamp;
uniform float u_zmax;
noperspective in vec4 v_col; flat in vec4 v_fcol; noperspective in vec3 v_stq; noperspective in vec2 v_uv;
noperspective in float v_fog; noperspective in float v_z; flat in vec4 v_clamp;
layout(location = 0, index = 0) out vec4 o0;
layout(location = 0, index = 1) out vec4 o1;

int wrapc(int c, int size, int mode, int mn, int mx) {
    if (mode == 0) return c & (size - 1);
    if (mode == 1) return clamp(c, 0, size - 1);
    if (mode == 2) return c < mn ? mn : (c > mx ? mx : c);
    return (c & mn) | mx;
}
ivec4 fetch(int u, int v) {
    u = wrapc(u, u_tsize.x, u_wrap.x, u_region.x, u_region.y);
    v = wrapc(v, u_tsize.y, u_wrap.y, u_region.z, u_region.w);
    if (u < 0 || v < 0 || u >= u_tsize.x || v >= u_tsize.y) return ivec4(0);
    return ivec4(texelFetch(u_tex, ivec2(u, v), 0) * 255.0 + 0.5);
}
ivec4 lerpc(ivec4 a, ivec4 b, int f) { return a + (((b - a) * f) >> 8); }
int blendsel(int sel, int cs, int cd) { return sel == 0 ? cs : (sel == 1 ? cd : 0); }

void main() {
    ivec4 c = ivec4(u_iip != 0 ? v_col : v_fcol);
    int r = c.r, g = c.g, b = c.b, a = c.a;
    if (u_tme != 0) {
        float tu, tv;
        // u_tsize is the size of the bound texture (HD textures: native size * u_hs), so STQ coordinates land
        // on its grid directly; UV coordinates are native texels and are scaled
        if (u_fst != 0) { tu = v_uv.x * u_hs.x; tv = v_uv.y * u_hs.y; }
        else { float iq = v_stq.z != 0.0 ? 1.0 / v_stq.z : 0.0; tu = v_stq.x * iq * float(u_tsize.x); tv = v_stq.y * iq * float(u_tsize.y); }
        // sprites: stay inside what the console sampled (sprite_tex_range; native texels, lo > hi = off)
        if (v_clamp.x <= v_clamp.z) tu = clamp(tu, v_clamp.x * u_hs.x, v_clamp.z * u_hs.x);
        if (v_clamp.y <= v_clamp.w) tv = clamp(tv, v_clamp.y * u_hs.y, v_clamp.w * u_hs.y);
        ivec4 t;
        if (u_hw != 0) {
            // anisotropic filtering: hardware trilinear/anisotropic sampling (repeat/clamp addressing only)
            t = ivec4(texture(u_tex, vec2(tu, tv) / vec2(u_tsize)) * 255.0 + 0.5);
        } else if (u_bil != 0) {
            tu -= 0.5; tv -= 0.5;
            float fu = floor(tu), fv = floor(tv);
            int iu = int(fu), iv = int(fv);
            int wu = int((tu - fu) * 256.0), wv = int((tv - fv) * 256.0);
            t = lerpc(lerpc(fetch(iu, iv), fetch(iu + 1, iv), wu), lerpc(fetch(iu, iv + 1), fetch(iu + 1, iv + 1), wu), wv);
        } else t = fetch(int(floor(tu)), int(floor(tv)));
        if (u_tfx == 0) { r = (t.r * r) >> 7; g = (t.g * g) >> 7; b = (t.b * b) >> 7; if (u_tcc != 0) a = (t.a * a) >> 7; }
        else if (u_tfx == 1) { r = t.r; g = t.g; b = t.b; if (u_tcc != 0) a = t.a; }
        else if (u_tfx == 2) { r = ((t.r * r) >> 7) + a; g = ((t.g * g) >> 7) + a; b = ((t.b * b) >> 7) + a; if (u_tcc != 0) a = t.a + a; }
        else { r = ((t.r * r) >> 7) + a; g = ((t.g * g) >> 7) + a; b = ((t.b * b) >> 7) + a; if (u_tcc != 0) a = t.a; }
        r = clamp(r, 0, 255); g = clamp(g, 0, 255); b = clamp(b, 0, 255); a = clamp(a, 0, 255);
    }
    if (u_fge != 0) {
        int f = int(v_fog); ivec3 fc = ivec3(u_fogcol);
        r = (r * f + fc.r * (255 - f)) >> 8; g = (g * f + fc.g * (255 - f)) >> 8; b = (b * f + fc.b * (255 - f)) >> 8;
    }
    if (u_atst >= 0) {
        bool pass;
        if (u_atst == 0) pass = false; else if (u_atst == 1) pass = true;
        else if (u_atst == 2) pass = a < u_aref; else if (u_atst == 3) pass = a <= u_aref;
        else if (u_atst == 4) pass = a == u_aref; else if (u_atst == 5) pass = a >= u_aref;
        else if (u_atst == 6) pass = a > u_aref; else pass = a != u_aref;
        if (pass != (u_apass != 0)) discard;
    }
    float z = min(floor(max(v_z, 0.0)), u_zmax);
    gl_FragDepth = z * (1.0 / 4294967296.0);
    ivec4 dst = ivec4(0);
    if (u_bmode == 2 || u_date != 0) dst = ivec4(texelFetch(u_dst, ivec2(gl_FragCoord.xy), 0) * 255.0 + 0.5);
    if (u_date != 0 && ((dst.a >> 7) & 1) != u_date - 1) discard;
    int oa = a & 255;
    if (u_fba != 0) oa |= 128;
    if (u_bmode == 1) {                                   // dual-source: o0 = Cs * |kS|, o1 = |kD|
        float f = min(float(u_blend.z == 0 ? a : u_fix) / 128.0, 1.0);
        float ks = (u_blend.x == 0 ? f : 0.0) - (u_blend.y == 0 ? f : 0.0) + (u_blend.w == 0 ? 1.0 : 0.0);
        float kd = (u_blend.x == 1 ? f : 0.0) - (u_blend.y == 1 ? f : 0.0) + (u_blend.w == 1 ? 1.0 : 0.0);
        o0 = vec4(vec3(r, g, b) / 255.0 * abs(ks), float(oa) / 255.0);
        o1 = vec4(vec3(abs(kd)), 0.0);
        return;
    }
    if (u_bmode == 2) {                                   // exact integer blend against the target copy
        int cc = u_blend.z == 0 ? a : (u_blend.z == 1 ? dst.a : u_fix);
        ivec3 cs = ivec3(r, g, b), cd = dst.rgb, o;
        for (int k = 0; k < 3; k++) {
            int va = blendsel(u_blend.x, cs[k], cd[k]), vb = blendsel(u_blend.y, cs[k], cd[k]), vd = blendsel(u_blend.w, cs[k], cd[k]);
            o[k] = (((va - vb) * cc) >> 7) + vd;
        }
        if (u_colclamp != 0) o = clamp(o, 0, 255); else o = o & 255;
        r = o.r; g = o.g; b = o.b;
    }
    o0 = vec4(vec3(r, g, b) / 255.0, float(oa) / 255.0);
    o1 = vec4(0.0);
})";

static const char* kCopyVS = R"(#version 330 core
uniform vec4 u_rect;
out vec2 v_uv;
void main() { vec2 c = vec2(gl_VertexID & 1, gl_VertexID >> 1); gl_Position = vec4(c * 2.0 - 1.0, 0.0, 1.0); v_uv = c; })";
static const char* kCopyFS = R"(#version 330 core
uniform sampler2D u_tex; uniform float u_depth;
in vec2 v_uv; out vec4 o;
void main() { o = texture(u_tex, v_uv); gl_FragDepth = u_depth; })";
static const char* kDateFS = R"(#version 330 core
uniform sampler2D u_tex; uniform int u_datm;
out vec4 o;
void main() { int a = int(texelFetch(u_tex, ivec2(gl_FragCoord.xy), 0).a * 255.0 + 0.5); if (((a >> 7) & 1) != u_datm) discard; o = vec4(0.0); })";
static const char* kDepthFS = R"(#version 330 core
uniform sampler2D u_tex;
in vec2 v_uv; out vec4 o;
void main() { o = vec4(0.0); gl_FragDepth = texture(u_tex, v_uv).r; })";

enum { U_TEX, U_DST, U_TME, U_FST, U_IIP, U_FGE, U_TFX, U_TCC, U_BIL, U_TSIZE, U_WRAP, U_REGION, U_HS, U_HW, U_ATST, U_AREF, U_APASS,
       U_FOGCOL, U_FBA, U_BLEND, U_FIX, U_BMODE, U_DATE, U_COLCLAMP, U_ZMAX, U_COUNT };
static const char* kUniNames[U_COUNT] = {"u_tex", "u_dst", "u_tme", "u_fst", "u_iip", "u_fge", "u_tfx", "u_tcc", "u_bil", "u_tsize", "u_wrap",
    "u_region", "u_hs", "u_hw", "u_atst", "u_aref", "u_apass", "u_fogcol", "u_fba", "u_blend", "u_fix", "u_bmode", "u_date", "u_colclamp", "u_zmax"};
#include <unordered_set>

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096]; glGetShaderInfoLog(s, sizeof log, nullptr, log); fprintf(stderr, "[gpu] shader error: %s\n", log); return 0; }
    return s;
}
static GLuint link(const char* vs, const char* fs) {
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[4096]; glGetProgramInfoLog(p, sizeof log, nullptr, log); fprintf(stderr, "[gpu] link error: %s\n", log); return 0; }
    return p;
}
static void tex_nearest(GLuint t) {
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

struct GLBackend : GpuBackend {
    GLuint date_prog = 0, depth_prog = 0, nfbo = 0, ncol = 0, ndepth = 0, nw_alloc = 0, nh_alloc = 0;
    GLuint prog = 0, copy_prog = 0, vao = 0, vbo = 0, fbo = 0, col = 0, depth = 0, dst = 0, empty_tex = 0;
    int w = 0, h = 0, scale = 1, nw = 0, nh = 0;
    std::vector<GLuint> free_later;
    GLint u[U_COUNT];
    float aniso = 0;
    std::unordered_set<GLuint> mipped;   // textures whose mipmaps / filter state are set up

    const char* name() const override { return "OpenGL"; }
    bool init() {
        if (!gl::glCreateShader) return false;               // no GL loaded (SDL renderer fallback)
        prog = link(kGsVS, kGsFS); copy_prog = link(kCopyVS, kCopyFS); depth_prog = link(kCopyVS, kDepthFS); date_prog = link(kCopyVS, kDateFS);
        if (!prog || !copy_prog || !depth_prog || !date_prog) return false;
        for (int i = 0; i < U_COUNT; i++) u[i] = glGetUniformLocation(prog, kUniNames[i]);
        glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        const GLsizei st = GPU_VTX_FLOATS * sizeof(float);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, 0, st, (void*)0);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 4, GL_FLOAT, 0, st, (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(2); glVertexAttribPointer(2, 3, GL_FLOAT, 0, st, (void*)(7 * sizeof(float)));
        glEnableVertexAttribArray(3); glVertexAttribPointer(3, 2, GL_FLOAT, 0, st, (void*)(10 * sizeof(float)));
        glEnableVertexAttribArray(4); glVertexAttribPointer(4, 1, GL_FLOAT, 0, st, (void*)(12 * sizeof(float)));
        glEnableVertexAttribArray(5); glVertexAttribPointer(5, 4, GL_FLOAT, 0, st, (void*)(13 * sizeof(float)));
        glBindVertexArray(0);
        u32 zero = 0;
        glGenTextures(1, &empty_tex);
        glBindTexture(GL_TEXTURE_2D, empty_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &zero);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
        return glGetError() == GL_NO_ERROR;
    }
    void set_target(int tw, int th, int wn, int hn, int sc) override {
        scale = sc; nw = wn; nh = hn; w = tw; h = th;
        if (!fbo) { glGenFramebuffers(1, &fbo); glGenTextures(1, &col); glGenTextures(1, &depth); glGenTextures(1, &dst); }
        glBindTexture(GL_TEXTURE_2D, col);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr); tex_nearest(col);
        glBindTexture(GL_TEXTURE_2D, depth);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH32F_STENCIL8, w, h, 0, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, nullptr); tex_nearest(depth);
        glBindTexture(GL_TEXTURE_2D, dst);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr); tex_nearest(dst);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, col, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fprintf(stderr, "[gpu] target framebuffer incomplete\n");
        glViewport(0, 0, w, h);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(1, 1, 1, 1); glDepthMask(1);
        glClearColor(0, 0, 0, 0); glClearDepth(0.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glClearDepth(1.0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    void upload_target(const u32* px, int wn, int hn) override {
        GLuint t; glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, wn, hn, 0, GL_RGBA, GL_UNSIGNED_BYTE, px); tex_nearest(t);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST);
        glColorMask(1, 1, 1, 1);
        glUseProgram(copy_prog);
        glUniform1i(glGetUniformLocation(copy_prog, "u_tex"), 0);
        glUniform1f(glGetUniformLocation(copy_prog, "u_depth"), 0.0f);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(vao);
        glViewport(0, 0, wn * scale, hn * scale);
        glBindTexture(GL_TEXTURE_2D, t);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0); glUseProgram(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteTextures(1, &t);
    }
    // native-size framebuffer for read-backs (the target is blitted down with nearest sampling)
    void native_fbo() {
        if (nfbo && (int)nw_alloc == nw && (int)nh_alloc == nh) return;
        if (!nfbo) { glGenFramebuffers(1, &nfbo); glGenTextures(1, &ncol); glGenTextures(1, &ndepth); }
        glBindTexture(GL_TEXTURE_2D, ncol);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, nw, nh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr); tex_nearest(ncol);
        glBindTexture(GL_TEXTURE_2D, ndepth);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH32F_STENCIL8, nw, nh, 0, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, nullptr); tex_nearest(ndepth);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, nfbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ncol, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, ndepth, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        nw_alloc = nw; nh_alloc = nh;
    }
    bool read_native(std::vector<u32>& c, std::vector<float>* d) override {
        GLuint src = fbo;
        if (scale > 1) {
            native_fbo();
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, nfbo);
            glBlitFramebuffer(0, 0, w, h, 0, 0, nw, nh, GL_COLOR_BUFFER_BIT | (d ? GL_DEPTH_BUFFER_BIT : 0), GL_NEAREST);
            src = nfbo;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, src);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        c.resize((size_t)nw * nh);
        glReadPixels(0, 0, nw, nh, GL_RGBA, GL_UNSIGNED_BYTE, c.data());
        if (d) { d->resize((size_t)nw * nh); glReadPixels(0, 0, nw, nh, GL_DEPTH_COMPONENT, GL_FLOAT, d->data()); }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return true;
    }
    void write_native(const u32* c, const float* d) override {
        upload_target(c, nw, nh);
        if (!d) return;
        GLuint t; glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, 0x822E /*GL_R32F*/, nw, nh, 0, GL_RED, GL_FLOAT, d); tex_nearest(t);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, w, h);
        glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST); glDepthFunc(GL_ALWAYS); glDepthMask(1);
        glColorMask(0, 0, 0, 0);
        glUseProgram(depth_prog);
        glUniform1i(glGetUniformLocation(depth_prog, "u_tex"), 0);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0); glUseProgram(0);
        glColorMask(1, 1, 1, 1); glDisable(GL_DEPTH_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteTextures(1, &t);
    }
    unsigned tex_create(const u32* px, int tw, int th) override {
        GLuint t; glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        tex_nearest(t);
        glBindTexture(GL_TEXTURE_2D, 0);
        return t;
    }
    void tex_free(unsigned id) override { free_later.push_back(id); mipped.erase(id); }
    void end_flush() override { for (GLuint t : free_later) glDeleteTextures(1, &t); free_later.clear(); }
    void set_aniso(float want) override {
        if (want > 0) {
            static float maxa = -1;
            if (maxa < 0) { maxa = 0; glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxa); while (glGetError() != GL_NO_ERROR) {} }
            if (maxa >= 1.0f) want = std::min(want, maxa);
        }
        if (want != aniso) { aniso = want; mipped.clear(); }   // re-apply sampler state to every texture
    }
    uintptr_t target_handle() override { return fbo; }
    bool read(int x0, int y0, int rw, int rh, std::vector<u32>& out) override {
        out.resize((size_t)rw * rh);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(x0, y0, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, out.data());   // row 0 = top (GS y), like our textures
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return true;
    }

    void apply_uniforms(const GpuState& st) {
        glUniform1i(u[U_TME], st.tme); glUniform1i(u[U_FST], st.fst); glUniform1i(u[U_IIP], st.iip || st.kind == 3 ? 1 : 0);
        glUniform1i(u[U_FGE], st.fge); glUniform1i(u[U_TFX], st.tfx); glUniform1i(u[U_TCC], st.tcc); glUniform1i(u[U_BIL], st.bil);
        const int hx = std::max<int>(st.hsx, 1), hy = std::max<int>(st.hsy, 1);
        glUniform2i(u[U_TSIZE], std::max<int>(st.tex_w, 1) * hx, std::max<int>(st.tex_h, 1) * hy);
        glUniform2i(u[U_WRAP], st.wms, st.wmt);
        glUniform2f(u[U_HS], (float)hx, (float)hy);
        // region clamp in the HD grid: whole native texels [min, max] cover [min*s, max*s + s-1]
        if (st.wms == 2 || st.wmt == 2)
            glUniform4i(u[U_REGION], st.wms == 2 ? st.minu * hx : st.minu, st.wms == 2 ? st.maxu * hx + hx - 1 : st.maxu,
                                     st.wmt == 2 ? st.minv * hy : st.minv, st.wmt == 2 ? st.maxv * hy + hy - 1 : st.maxv);
        else glUniform4i(u[U_REGION], st.minu, st.maxu, st.minv, st.maxv);
        glUniform3f(u[U_FOGCOL], (float)(st.fogcol & 0xff), (float)((st.fogcol >> 8) & 0xff), (float)((st.fogcol >> 16) & 0xff));
        glUniform1i(u[U_FBA], st.fba);
        glUniform4i(u[U_BLEND], st.alpha & 3, (st.alpha >> 2) & 3, (st.alpha >> 4) & 3, (st.alpha >> 6) & 3);
        glUniform1i(u[U_FIX], (st.alpha >> 8) & 0xff);
        glUniform1i(u[U_COLCLAMP], st.colclamp);
        const bool z16 = st.zpsm == 0x32 || st.zpsm == 0x3a;
        glUniform1f(u[U_ZMAX], z16 ? 65535.0f : st.zpsm == 0x31 ? 16777215.0f : 4294967295.0f);
        glUniform1i(u[U_DATE], (st.test & (1 << 14)) ? (int)((st.test >> 15) & 1) + 1 : 0);
    }
    // colour write mask from FBMSK (only whole bytes can be expressed)
    static void color_mask(const GpuState& st, bool alpha) {
        u32 m = st.fbmsk;
        glColorMask((m & 0xff) != 0xff, (m & 0xff00) != 0xff00, (m & 0xff0000) != 0xff0000, alpha && (m & 0xff000000u) != 0xff000000u);
    }
    // blend mode: 0 none, 1 dual-source, 2 shader (target copy)
    static int setup_blend(const GpuState& st, bool copy) {
        if (!st.abe) { glDisable(GL_BLEND); return 0; }
        if (copy) { glDisable(GL_BLEND); return 2; }
        u32 A = st.alpha & 3, B = (st.alpha >> 2) & 3, D = (st.alpha >> 6) & 3;
        bool neg_s = (B == 0 && A != 0 && D != 0), neg_d = (B == 1 && A != 1 && D != 1);
        GLenum eq = GL_FUNC_ADD;
        if (neg_s && !neg_d) eq = GL_FUNC_REVERSE_SUBTRACT;
        else if (!neg_s && neg_d) eq = GL_FUNC_SUBTRACT;
        glEnable(GL_BLEND);
        glBlendEquationSeparate(eq, GL_FUNC_ADD);
        if (neg_s && neg_d) glBlendFuncSeparate(GL_ZERO, GL_ZERO, GL_ONE, GL_ZERO);          // everything negative: 0
        else glBlendFuncSeparate(GL_ONE, GL_SRC1_COLOR, GL_ONE, GL_ZERO);
        return 1;
    }
    void execute(const float* verts, size_t nverts, const GpuBatch* batches, size_t nb) override {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, w, h);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nverts * GPU_VTX_FLOATS * sizeof(float)), verts, GL_STREAM_DRAW);
        glUseProgram(prog);
        glUniform1i(u[U_TEX], 0); glUniform1i(u[U_DST], 1);
        glEnable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        static const GLenum kModes[3] = {GL_POINTS, GL_LINES, GL_TRIANGLES};
        for (size_t bi = 0; bi < nb; bi++) {
            const GpuBatch& b = batches[bi];
            const GpuState& st = b.st;
            const GLenum mode = kModes[b.mode];
            apply_uniforms(st);
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, st.tme && st.tex ? st.tex : empty_tex);
            {   // anisotropic filtering for bilinear-filtered textures with plain repeat/clamp addressing
                const bool hw = aniso > 1.0f && st.kind == 2 && st.tme && st.tex && st.bil && st.wms < 2 && st.wmt < 2;
                glUniform1i(u[U_HW], hw ? 1 : 0);
                if (hw) {
                    if (mipped.insert(st.tex).second) {
                        glGenerateMipmap(GL_TEXTURE_2D);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, aniso);
                    }
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, st.wms == 0 ? GL_REPEAT : GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, st.wmt == 0 ? GL_REPEAT : GL_CLAMP_TO_EDGE);
                }
            }
            const bool bbox_ok = b.bx1 > b.bx0 && b.by1 > b.by0;
            if ((b.copy || b.date) && bbox_ok) {
                glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, dst);
                glCopyTexSubImage2D(GL_TEXTURE_2D, 0, b.bx0, b.by0, b.bx0, b.by0, b.bx1 - b.bx0, b.by1 - b.by0);
                glActiveTexture(GL_TEXTURE0);
                g_gpu_stat_copies++;
            }
            glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, dst); glActiveTexture(GL_TEXTURE0);
            int x0 = st.sx0 * scale, y0 = st.sy0 * scale;
            int x1 = std::min((st.sx1 + 1) * scale, w), y1 = std::min((st.sy1 + 1) * scale, h);
            if (x1 <= x0 || y1 <= y0) continue;
            if (b.date) {
                if (!bbox_ok) continue;
                // stencil = 1 where the destination alpha passes the test (from the copy taken above)
                glScissor(b.bx0, b.by0, b.bx1 - b.bx0, b.by1 - b.by0);
                glEnable(GL_STENCIL_TEST); glStencilMask(0xff);
                glClear(GL_STENCIL_BUFFER_BIT);
                glStencilFunc(GL_ALWAYS, 1, 0xff); glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                glColorMask(0, 0, 0, 0); glDepthMask(0); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
                glUseProgram(date_prog);
                glUniform1i(glGetUniformLocation(date_prog, "u_tex"), 1);
                glUniform1i(glGetUniformLocation(date_prog, "u_datm"), (int)((st.test >> 15) & 1));
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                glUseProgram(prog);
                glStencilFunc(GL_EQUAL, 1, 0xff);
                glUniform1i(u[U_DATE], 0);
            } else glDisable(GL_STENCIL_TEST);
            // pixels written with alpha: with DATE mode 2 they must fail from now on
            auto stencil_op = [&](bool writes_alpha) { if (b.date) glStencilOp(GL_KEEP, GL_KEEP, b.date == 2 && writes_alpha ? GL_ZERO : GL_KEEP); };
            glScissor(x0, y0, x1 - x0, y1 - y0);
            glUniform1i(u[U_BMODE], setup_blend(st, b.copy == 2));
            const bool zte = (st.test >> 16) & 1;
            if (zte) {
                static const GLenum zf[4] = {GL_NEVER, GL_ALWAYS, GL_GEQUAL, GL_GREATER};
                glEnable(GL_DEPTH_TEST); glDepthFunc(zf[(st.test >> 17) & 3]);
            } else glDisable(GL_DEPTH_TEST);
            const bool zw = zte && !st.zmsk;
            const bool ate = (st.test & 1) != 0;
            const u32 atst = (st.test >> 1) & 7, afail = (st.test >> 12) & 3;
            if (!ate || atst == 1) {
                glUniform1i(u[U_ATST], -1);
                color_mask(st, true); glDepthMask(zw); stencil_op(true);
                glDrawArrays(mode, (GLint)b.first, (GLsizei)b.count);
                continue;
            }
            glUniform1i(u[U_ATST], (int)atst); glUniform1i(u[U_AREF], (int)((st.test >> 4) & 0xff));
            if (atst != 0) {                                    // pixels that pass
                glUniform1i(u[U_APASS], 1);
                color_mask(st, true); glDepthMask(zw); stencil_op(true);
                glDrawArrays(mode, (GLint)b.first, (GLsizei)b.count);
            }
            if (afail != 0) {                                   // pixels that fail: partial writes
                glUniform1i(u[U_APASS], 0);
                if (afail == 1) { color_mask(st, true); glDepthMask(0); stencil_op(true); }
                else if (afail == 2) { glColorMask(0, 0, 0, 0); glDepthMask(zw); stencil_op(false); }
                else { color_mask(st, false); glDepthMask(0); stencil_op(false); }
                glDrawArrays(mode, (GLint)b.first, (GLsizei)b.count);
            }
        }
        // leave GL as the presenter expects it
        glDisable(GL_STENCIL_TEST); glStencilMask(0xff);
        glColorMask(1, 1, 1, 1); glDepthMask(1);
        glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, 0);
        glBindVertexArray(0); glUseProgram(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
};
GpuBackend* gpu_backend_gl() {
    static GLBackend* b = nullptr;
    if (b) return b;
    GLBackend* n = new GLBackend();
    if (!n->init()) { delete n; return nullptr; }
    return b = n;
}
#else
GpuBackend* gpu_backend_gl() { return nullptr; }
#endif
#if !defined(__APPLE__) || !defined(MVP_SDL)
GpuBackend* gpu_backend_metal() { return nullptr; }
#endif
#if !defined(MVP_VULKAN)
GpuBackend* gpu_backend_vulkan() { return nullptr; }
#endif
