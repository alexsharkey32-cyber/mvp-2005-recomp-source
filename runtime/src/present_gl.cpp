// OpenGL 3.3 core presenter: game picture -> [FXAA at source resolution] -> resample to the output size
// (nearest / bilinear / bicubic / area) -> sharpen (CAS) + colour + CRT -> window, then the UI.
#ifdef MVP_SDL
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "gl_funcs.h"
#include "present.h"
#include "ee_runtime.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#define GL_DEFINE(ret, name, args) PFN_##name name = nullptr;
namespace gl { GL_FUNCS(GL_DEFINE) }
#undef GL_DEFINE
bool gl_load(void* (*get_proc)(const char*)) {
    bool ok = true;
#define GL_LOAD(ret, name, args) gl::name = (gl::PFN_##name)get_proc(#name); if (!gl::name) { fprintf(stderr, "GL: missing %s\n", #name); ok = false; }
    GL_FUNCS(GL_LOAD)
#undef GL_LOAD
    return ok;
}
using namespace gl;

static const char* kVS = R"(#version 330 core
uniform vec4 u_rect;   // x0 y0 x1 y1 in NDC
uniform vec4 u_uv;     // u0 v0 u1 v1
out vec2 v_uv;
void main() {
    vec2 c = vec2(gl_VertexID & 1, gl_VertexID >> 1);
    gl_Position = vec4(mix(u_rect.xy, u_rect.zw, c), 0.0, 1.0);
    v_uv = mix(u_uv.xy, u_uv.zw, c);
})";

static const char* kFXAA = R"(#version 330 core
uniform sampler2D u_tex; uniform vec2 u_rcp; uniform float u_span; uniform float u_reduce;
in vec2 v_uv; out vec4 o;
void main() {
    vec3 L = vec3(0.299, 0.587, 0.114);
    vec3 nw = texture(u_tex, v_uv + vec2(-1.0, -1.0) * u_rcp).rgb, ne = texture(u_tex, v_uv + vec2(1.0, -1.0) * u_rcp).rgb;
    vec3 sw = texture(u_tex, v_uv + vec2(-1.0, 1.0) * u_rcp).rgb, se = texture(u_tex, v_uv + vec2(1.0, 1.0) * u_rcp).rgb;
    vec4 mc = texture(u_tex, v_uv); vec3 m = mc.rgb;
    float lnw = dot(nw, L), lne = dot(ne, L), lsw = dot(sw, L), lse = dot(se, L), lm = dot(m, L);
    float lmin = min(lm, min(min(lnw, lne), min(lsw, lse))), lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));
    vec2 dir = vec2(-((lnw + lne) - (lsw + lse)), ((lnw + lsw) - (lne + lse)));
    float red = max((lnw + lne + lsw + lse) * (0.25 * 0.125), u_reduce);
    float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + red);
    dir = clamp(dir * rcpMin, vec2(-u_span), vec2(u_span)) * u_rcp;
    vec3 a = 0.5 * (texture(u_tex, v_uv + dir * (1.0 / 3.0 - 0.5)).rgb + texture(u_tex, v_uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 b = a * 0.5 + 0.25 * (texture(u_tex, v_uv + dir * -0.5).rgb + texture(u_tex, v_uv + dir * 0.5).rgb);
    float lb = dot(b, L);
    o = vec4((lb < lmin || lb > lmax) ? a : b, 1.0);
})";

static const char* kResample = R"(#version 330 core
uniform sampler2D u_tex; uniform vec2 u_src; uniform vec2 u_dst; uniform int u_mode;
in vec2 v_uv; out vec4 o;
float cw(float x) { x = abs(x); return x < 1.0 ? 1.5*x*x*x - 2.5*x*x + 1.0 : x < 2.0 ? -0.5*x*x*x + 2.5*x*x - 4.0*x + 2.0 : 0.0; }
void main() {
    if (u_mode == 2) {                                     // bicubic (Catmull-Rom), 16 taps
        vec2 p = v_uv * u_src - 0.5; vec2 f = fract(p); ivec2 b = ivec2(floor(p));
        vec3 acc = vec3(0.0); float ws = 0.0;
        for (int j = -1; j <= 2; j++) for (int i = -1; i <= 2; i++) {
            float w = cw(float(i) - f.x) * cw(float(j) - f.y);
            ivec2 q = clamp(b + ivec2(i, j), ivec2(0), ivec2(u_src) - 1);
            acc += w * texelFetch(u_tex, q, 0).rgb; ws += w;
        }
        o = vec4(acc / ws, 1.0);
    } else if (u_mode == 3) {                              // area: average the source footprint of this pixel
        vec2 fp = u_src / u_dst;                           // source pixels per output pixel
        vec2 n = clamp(ceil(fp), vec2(1.0), vec2(6.0));
        vec3 acc = vec3(0.0);
        for (float j = 0.0; j < 6.0; j++) for (float i = 0.0; i < 6.0; i++) {
            if (i >= n.x || j >= n.y) continue;
            vec2 off = ((vec2(i, j) + 0.5) / n - 0.5) * fp / u_src;
            acc += texture(u_tex, v_uv + off).rgb;
        }
        o = vec4(acc / (n.x * n.y), 1.0);
    } else o = vec4(texture(u_tex, v_uv).rgb, 1.0);        // nearest / bilinear (sampler state)
})";

static const char* kFinal = R"(#version 330 core
uniform sampler2D u_tex; uniform vec2 u_size; uniform float u_sharp; uniform vec4 u_color; uniform float u_gamma;
uniform int u_crt; uniform float u_lines;
in vec2 v_uv; out vec4 o;
vec3 px(vec2 uv, vec2 d) { return texture(u_tex, uv + d / u_size).rgb; }
void main() {
    vec2 uv = v_uv;
    float vig = 1.0;
    if (u_crt == 2) {                                      // barrel curvature + vignette
        vec2 c = uv * 2.0 - 1.0;
        c *= 1.0 + 0.045 * dot(c.yx, c.yx);
        uv = c * 0.5 + 0.5;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) { o = vec4(0.0, 0.0, 0.0, 1.0); return; }
        vec2 e = uv * (1.0 - uv.yx); vig = clamp(pow(e.x * e.y * 18.0, 0.22), 0.0, 1.0);
    }
    vec3 col = px(uv, vec2(0.0));
    if (u_sharp > 0.0) {                                   // AMD FidelityFX CAS (simplified)
        vec3 b = px(uv, vec2(0.0, -1.0)), d = px(uv, vec2(-1.0, 0.0)), f = px(uv, vec2(1.0, 0.0)), h = px(uv, vec2(0.0, 1.0));
        vec3 mn = min(min(min(d, col), min(f, b)), h), mx = max(max(max(d, col), max(f, b)), h);
        vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));
        vec3 w = amp * (-1.0 / mix(8.0, 5.0, u_sharp));
        col = clamp((b * w + d * w + f * w + h * w + col) / (1.0 + 4.0 * w), 0.0, 1.0);
    }
    // colour: brightness (x), contrast (y), saturation (z); gamma
    col = (col - 0.5) * u_color.y + 0.5 + u_color.x;
    float l = dot(col, vec3(0.299, 0.587, 0.114));
    col = mix(vec3(l), col, u_color.z);
    col = pow(clamp(col, 0.0, 1.0), vec3(1.0 / u_gamma));
    if (u_crt >= 1) {                                      // scanlines at the original line spacing
        float row = uv.y * u_lines;
        float ppl = u_size.y / u_lines;                      // output pixels per original line
        float amt = clamp((ppl - 1.6) / 1.4, 0.0, 1.0);      // fade out when lines are too dense to show
        float d = abs(fract(row) - 0.5) * 2.0;               // 0 at line centre, 1 between lines
        float s = 1.0 - smoothstep(0.35, 1.0, d);
        col *= mix(1.0, 0.62 + 0.38 * s, amt);
        col *= 1.0 + 0.14 * amt;
    }
    o = vec4(col * vig, 1.0);
})";

static const char* kUiVS = R"(#version 330 core
layout(location = 0) in vec2 a_pos; layout(location = 1) in vec2 a_uv; layout(location = 2) in vec4 a_col;
layout(location = 3) in vec2 a_local; layout(location = 4) in vec4 a_shape;   // half-size xy, radius, soft/border
layout(location = 5) in float a_kind;
uniform vec2 u_screen;
out vec2 v_uv; out vec4 v_col; out vec2 v_local; out vec4 v_shape; flat out int v_kind;
void main() {
    gl_Position = vec4(a_pos.x / u_screen.x * 2.0 - 1.0, 1.0 - a_pos.y / u_screen.y * 2.0, 0.0, 1.0);
    v_uv = a_uv; v_col = a_col; v_local = a_local; v_shape = a_shape; v_kind = int(a_kind + 0.5);
})";
static const char* kUiFS = R"(#version 330 core
uniform sampler2D u_font; uniform sampler2D u_img;
in vec2 v_uv; in vec4 v_col; in vec2 v_local; in vec4 v_shape; flat in int v_kind;
out vec4 o;
float sdbox(vec2 p, vec2 b, float r) { vec2 q = abs(p) - b + r; return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r; }
void main() {
    float a;
    if (v_kind == 1) a = texture(u_font, v_uv).r;
    else if (v_kind == 2) {
        float blur = max(v_shape.w, 1.0);
        float d = sdbox(v_local, v_shape.xy - blur, v_shape.z);
        a = 1.0 - smoothstep(-blur, blur, d);
        a *= a;
    } else {
        float d = sdbox(v_local, v_shape.xy, min(v_shape.z, min(v_shape.x, v_shape.y)));
        a = clamp(0.5 - d, 0.0, 1.0);
        if (v_shape.w > 0.0 && v_kind == 0) a *= clamp(0.5 + d + v_shape.w, 0.0, 1.0);
    }
    if (v_kind == 3) { vec4 t = texture(u_img, v_uv); o = vec4(t.rgb * v_col.rgb, t.a * v_col.a * a); return; }
    if (v_kind == 4) { float k = v_shape.w > 0.5 ? v_uv.y : v_uv.x; o = vec4(v_col.rgb, v_col.a * a * k); return; }
    o = vec4(v_col.rgb, v_col.a * a);
})";

class GLPresenter : public Presenter {
public:
    SDL_Window* win = nullptr;
    SDL_GLContext ctx = nullptr;
    GLuint vao = 0, ui_vbo = 0, game_tex = 0, font_tex = 0;
    GLuint ui_img[kUiImages] = {};
    GLuint p_fxaa = 0, p_resample = 0, p_final = 0, p_ui = 0;
    GLuint fb1 = 0, fb1_tex = 0, fb2 = 0, fb2_tex = 0;
    int game_w = 0, game_h = 0, fb1_w = 0, fb1_h = 0, fb2_w = 0, fb2_h = 0;
    bool have_game = false;
    std::vector<u32> captured; int cap_w = 0, cap_h = 0;
    std::vector<float> uiv;

    const char* name() const override { return "OpenGL"; }

    static GLuint compile(GLenum type, const char* src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, nullptr, log); fprintf(stderr, "GL shader error: %s\n", log); return 0; }
        return s;
    }
    static GLuint program(const char* vs, const char* fs) {
        GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
        if (!v || !f) return 0;
        GLuint p = glCreateProgram();
        glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
        GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) { char log[2048]; glGetProgramInfoLog(p, sizeof log, nullptr, log); fprintf(stderr, "GL link error: %s\n", log); return 0; }
        return p;
    }
    static void tex_params(GLenum filter) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    static void make_target(GLuint& fb, GLuint& tex, int& cw, int& ch, int w, int h) {
        if (fb && cw == w && ch == h) return;
        if (!fb) { glGenFramebuffers(1, &fb); glGenTextures(1, &tex); }
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        tex_params(GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, fb);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        cw = w; ch = h;
    }
    bool init(SDL_Window* w) {
        win = w;
        ctx = SDL_GL_CreateContext(win);
        if (!ctx) { fprintf(stderr, "OpenGL context failed (%s)\n", SDL_GetError()); return false; }
        if (!gl_load(SDL_GL_GetProcAddress)) return false;
        p_fxaa = program(kVS, kFXAA); p_resample = program(kVS, kResample); p_final = program(kVS, kFinal); p_ui = program(kUiVS, kUiFS);
        if (!p_fxaa || !p_resample || !p_final || !p_ui) return false;
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &ui_vbo);
        glGenTextures(1, &game_tex);
        glGenTextures(1, &font_tex);
        const UiAtlas& a = ui_atlas();
        glBindTexture(GL_TEXTURE_2D, font_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, a.w, a.h, 0, GL_RED, GL_UNSIGNED_BYTE, a.px.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        tex_params(GL_LINEAR);
        glGenerateMipmap(GL_TEXTURE_2D);                               // the atlas is drawn at 56 px: small text samples the mips
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, 0x8501 /* GL_TEXTURE_LOD_BIAS */, -0.35f);
        rlog("gl", "presenter: %s | %s", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));
        return true;
    }
    void set_vsync(bool on) override { SDL_GL_SetSwapInterval(on ? 1 : 0); }
    void set_ui_image(int id, const u32* px, int w, int h) override {
        if (id < 0 || id >= kUiImages) return;
        if (!w) { if (ui_img[id]) glDeleteTextures(1, &ui_img[id]); ui_img[id] = 0; return; }
        if (!ui_img[id]) glGenTextures(1, &ui_img[id]);
        glBindTexture(GL_TEXTURE_2D, ui_img[id]);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    void drawable_size(int& w, int& h) override { SDL_GL_GetDrawableSize(win, &w, &h); }
    void set_game(const u32* px, int w, int h) override {
        glBindTexture(GL_TEXTURE_2D, game_tex);
        if (w != game_w || h != game_h) { glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px); game_w = w; game_h = h; }
        else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        have_game = true;
    }
    GLuint game_fb = 0;
    bool set_game_gpu(uintptr_t fbo, int x0, int y0, int w, int h) override {
        if (!fbo || w <= 0 || h <= 0) return false;
        glBindTexture(GL_TEXTURE_2D, game_tex);
        if (w != game_w || h != game_h) { glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr); game_w = w; game_h = h; }
        if (!game_fb) glGenFramebuffers(1, &game_fb);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, game_fb);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, game_tex, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glBlitFramebuffer(x0, y0, x0 + w, y0 + h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        have_game = true;
        return true;
    }
    void quad(GLuint prog, float x0, float y0, float x1, float y1, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1) {
        glUniform4f(glGetUniformLocation(prog, "u_rect"), x0, y0, x1, y1);
        glUniform4f(glGetUniformLocation(prog, "u_uv"), u0, v0, u1, v1);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    void frame(int dw, int dh, const DstRect& dst, const PostParams& pp, const UiBatch& ui) override {
        glBindVertexArray(vao);
        glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, dw, dh);
        glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
        if (ui.under) { draw_ui(dw, dh, ui, 0, ui.under); glBindVertexArray(vao); glDisable(GL_BLEND); }   // the UI under the picture
        glActiveTexture(GL_TEXTURE0);
        if (have_game && dst.w > 0 && dst.h > 0) {
            GLuint src = game_tex; int sw = game_w, sh = game_h;
            // 1. FXAA at the picture's own resolution. Our textures are top-down, GL's framebuffers bottom-up:
            //    every pass flips v, so after the three passes the picture is upright on screen.
            if (pp.aa) {
                make_target(fb1, fb1_tex, fb1_w, fb1_h, sw, sh);
                glBindFramebuffer(GL_FRAMEBUFFER, fb1); glViewport(0, 0, sw, sh);
                glUseProgram(p_fxaa);
                glBindTexture(GL_TEXTURE_2D, src); tex_params(GL_LINEAR);
                glUniform1i(glGetUniformLocation(p_fxaa, "u_tex"), 0);
                glUniform2f(glGetUniformLocation(p_fxaa, "u_rcp"), 1.0f / sw, 1.0f / sh);
                glUniform1f(glGetUniformLocation(p_fxaa, "u_span"), pp.aa >= 2 ? 16.0f : 8.0f);
                glUniform1f(glGetUniformLocation(p_fxaa, "u_reduce"), pp.aa >= 2 ? 1.0f / 256 : 1.0f / 128);
                quad(p_fxaa, -1, -1, 1, 1, 0, 0, 1, 1);          // no flip: fb1 stores rows like the source
                src = fb1_tex;
            }
            // 2. resample to the output size
            make_target(fb2, fb2_tex, fb2_w, fb2_h, dst.w, dst.h);
            glBindFramebuffer(GL_FRAMEBUFFER, fb2); glViewport(0, 0, dst.w, dst.h);
            glUseProgram(p_resample);
            glBindTexture(GL_TEXTURE_2D, src);
            tex_params(pp.scale_filter == 0 ? GL_NEAREST : GL_LINEAR);
            int mode = pp.scale_filter;
            if (mode == 3 && sw <= dst.w && sh <= dst.h) mode = 2;      // area only matters when shrinking; enlarge with bicubic
            glUniform1i(glGetUniformLocation(p_resample, "u_tex"), 0);
            glUniform2f(glGetUniformLocation(p_resample, "u_src"), (float)sw, (float)sh);
            glUniform2f(glGetUniformLocation(p_resample, "u_dst"), (float)dst.w, (float)dst.h);
            glUniform1i(glGetUniformLocation(p_resample, "u_mode"), mode);
            quad(p_resample, -1, -1, 1, 1, 0, 0, 1, 1);
            // 3. sharpen + colour + CRT into the window (flip: fb2 row 0 = picture top)
            glBindFramebuffer(GL_FRAMEBUFFER, 0); glViewport(0, 0, dw, dh);
            glUseProgram(p_final);
            glBindTexture(GL_TEXTURE_2D, fb2_tex); tex_params(GL_LINEAR);
            glUniform1i(glGetUniformLocation(p_final, "u_tex"), 0);
            glUniform2f(glGetUniformLocation(p_final, "u_size"), (float)dst.w, (float)dst.h);
            glUniform1f(glGetUniformLocation(p_final, "u_sharp"), pp.sharpen);
            glUniform4f(glGetUniformLocation(p_final, "u_color"), pp.brightness, pp.contrast, pp.saturation, 0);
            glUniform1f(glGetUniformLocation(p_final, "u_gamma"), pp.gamma);
            glUniform1i(glGetUniformLocation(p_final, "u_crt"), pp.crt);
            glUniform1f(glGetUniformLocation(p_final, "u_lines"), (float)pp.native_lines);
            float x0 = (float)dst.x / dw * 2 - 1, x1 = (float)(dst.x + dst.w) / dw * 2 - 1;
            float y1 = 1 - (float)dst.y / dh * 2, y0 = 1 - (float)(dst.y + dst.h) / dh * 2;
            quad(p_final, x0, y0, x1, y1, 0, 1, 1, 0);
        }
        draw_ui(dw, dh, ui, ui.under, ui.q.size());
        if (capture) {
            capture = false;
            cap_w = dw; cap_h = dh;
            std::vector<u32> tmp((size_t)dw * dh);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadPixels(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
            captured.resize(tmp.size());
            for (int y = 0; y < dh; y++) std::memcpy(&captured[(size_t)y * dw], &tmp[(size_t)(dh - 1 - y) * dw], (size_t)dw * 4);
        }
        SDL_GL_SwapWindow(win);
    }
    void draw_ui(int dw, int dh, const UiBatch& ui, size_t first, size_t last) {
        if (last <= first) return;
        uiv.clear();
        uiv.reserve((last - first) * 6 * 15);
        for (size_t qi = first; qi < last; qi++) { const UiQuad& q = ui.q[qi];
            float r = ((q.color >> 16) & 0xff) / 255.0f, g = ((q.color >> 8) & 0xff) / 255.0f, b = (q.color & 0xff) / 255.0f, a = (q.color >> 24) / 255.0f;
            float hw = q.w * 0.5f, hh = q.h * 0.5f;
            const float cx[6] = {0, 1, 0, 1, 1, 0}, cy[6] = {0, 0, 1, 0, 1, 1};
            for (int i = 0; i < 6; i++) {
                float px = q.x + cx[i] * q.w, py = q.y + cy[i] * q.h;
                float u = q.u0 + (q.u1 - q.u0) * cx[i], v = q.v0 + (q.v1 - q.v0) * cy[i];
                float lx = (cx[i] - 0.5f) * q.w, ly = (cy[i] - 0.5f) * q.h;
                float v15[15] = {px, py, u, v, r, g, b, a, lx, ly, hw, hh, q.radius, q.soft, (float)q.kind};
                uiv.insert(uiv.end(), v15, v15 + 15);
            }
        }
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(p_ui);
        glUniform2f(glGetUniformLocation(p_ui, "u_screen"), (float)dw, (float)dh);
        glUniform1i(glGetUniformLocation(p_ui, "u_font"), 0);
        glUniform1i(glGetUniformLocation(p_ui, "u_img"), 1);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, font_tex);
        glBindBuffer(GL_ARRAY_BUFFER, ui_vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(uiv.size() * sizeof(float)), uiv.data(), GL_STREAM_DRAW);
        const GLsizei st = 15 * sizeof(float);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, 0, st, (void*)0);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, 0, st, (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_FLOAT, 0, st, (void*)(4 * sizeof(float)));
        glEnableVertexAttribArray(3); glVertexAttribPointer(3, 2, GL_FLOAT, 0, st, (void*)(8 * sizeof(float)));
        glEnableVertexAttribArray(4); glVertexAttribPointer(4, 4, GL_FLOAT, 0, st, (void*)(10 * sizeof(float)));
        glEnableVertexAttribArray(5); glVertexAttribPointer(5, 1, GL_FLOAT, 0, st, (void*)(14 * sizeof(float)));
        // one draw per run of quads using the same image (most frames: a single run)
        size_t i = first, n = last;
        while (i < n) {
            int img = ui.q[i].kind == UI_IMAGE ? (int)ui.q[i].soft : -1;
            size_t j = i + 1;
            while (j < n && (ui.q[j].kind == UI_IMAGE ? (int)ui.q[j].soft : -1) == img) j++;
            if (img >= 0) { glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, img < kUiImages ? ui_img[img] : 0); glActiveTexture(GL_TEXTURE0); }
            if (img < 0 || (img < kUiImages && ui_img[img])) glDrawArrays(GL_TRIANGLES, (GLint)((i - first) * 6), (GLsizei)((j - i) * 6));
            i = j;
        }
        glDisable(GL_BLEND);
    }
    bool read_window(std::vector<u32>& px, int& w, int& h) override {
        if (captured.empty()) return false;
        px = captured; w = cap_w; h = cap_h; return true;
    }
};

Presenter* presenter_gl_create(SDL_Window* win) {
    if (std::getenv("MVP_NO_GL")) return nullptr;
    GLPresenter* p = new GLPresenter();
    if (!p->init(win)) { delete p; return nullptr; }
    return p;
}
#endif
