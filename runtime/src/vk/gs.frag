#version 450
#include "common.glsl"
// the GL renderer's fragment shader (gs_gpu.cpp kGsFS) with push constants
layout(set = 0, binding = 1) uniform sampler2D u_dst;
layout(set = 1, binding = 0) uniform sampler2D u_tex;
layout(location = 0) noperspective in vec4 v_col; layout(location = 1) flat in vec4 v_fcol;
layout(location = 2) noperspective in vec3 v_stq; layout(location = 3) noperspective in vec2 v_uv;
layout(location = 4) noperspective in float v_fog; layout(location = 5) noperspective in float v_z;
layout(location = 6) flat in vec4 v_clamp;
layout(location = 0, index = 0) out vec4 o0;
layout(location = 0, index = 1) out vec4 o1;
U u;
int wrapc(int c, int size, int mode, int mn, int mx) {
    if (mode == 0) return c & (size - 1);
    if (mode == 1) return clamp(c, 0, size - 1);
    if (mode == 2) return c < mn ? mn : (c > mx ? mx : c);
    return (c & mn) | mx;
}
ivec4 fetch(int x, int y) {
    x = wrapc(x, u.tsx, u.wrapx, u.rx0, u.rx1);
    y = wrapc(y, u.tsy, u.wrapy, u.ry0, u.ry1);
    if (x < 0 || y < 0 || x >= u.tsx || y >= u.tsy) return ivec4(0);
    return ivec4(texelFetch(u_tex, ivec2(x, y), 0) * 255.0 + 0.5);
}
ivec4 lerpc(ivec4 a, ivec4 b, int f) { return a + (((b - a) * f) >> 8); }
int blendsel(int sel, int cs, int cd) { return sel == 0 ? cs : (sel == 1 ? cd : 0); }
void main() {
    u = unpack();
    ivec4 c = ivec4(u.iip != 0 ? v_col : v_fcol);
    int r = c.r, g = c.g, b = c.b, a = c.a;
    if (u.tme != 0) {
        float tu, tv;
        if (u.fst != 0) { tu = v_uv.x * u.hsx; tv = v_uv.y * u.hsy; }
        else { float iq = v_stq.z != 0.0 ? 1.0 / v_stq.z : 0.0; tu = v_stq.x * iq * float(u.tsx); tv = v_stq.y * iq * float(u.tsy); }
        if (v_clamp.x <= v_clamp.z) tu = clamp(tu, v_clamp.x * u.hsx, v_clamp.z * u.hsx);
        if (v_clamp.y <= v_clamp.w) tv = clamp(tv, v_clamp.y * u.hsy, v_clamp.w * u.hsy);
        ivec4 t;
        if (u.hw != 0) {                  // anisotropic: the sampler repeats; clamp modes are clamped here
            vec2 tc = vec2(tu, tv) / vec2(u.tsx, u.tsy);
            vec2 h = 0.5 / vec2(u.tsx, u.tsy);
            if (u.wrapx != 0) tc.x = clamp(tc.x, h.x, 1.0 - h.x);
            if (u.wrapy != 0) tc.y = clamp(tc.y, h.y, 1.0 - h.y);
            t = ivec4(texture(u_tex, tc) * 255.0 + 0.5);
        } else if (u.bil != 0) {
            tu -= 0.5; tv -= 0.5;
            float fu = floor(tu), fv = floor(tv);
            int iu = int(fu), iv = int(fv);
            int wu = int((tu - fu) * 256.0), wv = int((tv - fv) * 256.0);
            t = lerpc(lerpc(fetch(iu, iv), fetch(iu + 1, iv), wu), lerpc(fetch(iu, iv + 1), fetch(iu + 1, iv + 1), wu), wv);
        } else t = fetch(int(floor(tu)), int(floor(tv)));
        if (u.tfx == 0) { r = (t.r * r) >> 7; g = (t.g * g) >> 7; b = (t.b * b) >> 7; if (u.tcc != 0) a = (t.a * a) >> 7; }
        else if (u.tfx == 1) { r = t.r; g = t.g; b = t.b; if (u.tcc != 0) a = t.a; }
        else if (u.tfx == 2) { r = ((t.r * r) >> 7) + a; g = ((t.g * g) >> 7) + a; b = ((t.b * b) >> 7) + a; if (u.tcc != 0) a = t.a + a; }
        else { r = ((t.r * r) >> 7) + a; g = ((t.g * g) >> 7) + a; b = ((t.b * b) >> 7) + a; if (u.tcc != 0) a = t.a; }
        r = clamp(r, 0, 255); g = clamp(g, 0, 255); b = clamp(b, 0, 255); a = clamp(a, 0, 255);
    }
    if (u.fge != 0) {
        int f = int(v_fog); ivec3 fc = ivec3(int(u.fogr), int(u.fogg), int(u.fogb));
        r = (r * f + fc.r * (255 - f)) >> 8; g = (g * f + fc.g * (255 - f)) >> 8; b = (b * f + fc.b * (255 - f)) >> 8;
    }
    if (u.atst >= 0) {
        bool pass;
        if (u.atst == 0) pass = false; else if (u.atst == 1) pass = true;
        else if (u.atst == 2) pass = a < u.aref; else if (u.atst == 3) pass = a <= u.aref;
        else if (u.atst == 4) pass = a == u.aref; else if (u.atst == 5) pass = a >= u.aref;
        else if (u.atst == 6) pass = a > u.aref; else pass = a != u.aref;
        if (pass != (u.apass != 0)) discard;
    }
    float z = min(floor(max(v_z, 0.0)), u.zmax);
    gl_FragDepth = z * (1.0 / 4294967296.0);
    ivec4 dst = ivec4(0);
    if (u.bmode == 2 || u.date != 0) dst = ivec4(texelFetch(u_dst, ivec2(gl_FragCoord.xy), 0) * 255.0 + 0.5);
    if (u.date != 0 && ((dst.a >> 7) & 1) != u.date - 1) discard;
    int oa = a & 255;
    if (u.fba != 0) oa |= 128;
    if (u.bmode == 1) {                                   // dual-source: o0 = Cs * |kS|, o1 = |kD|
        float f = min(float(u.bc == 0 ? a : u.fix) / 128.0, 1.0);
        float ks = (u.ba == 0 ? f : 0.0) - (u.bb == 0 ? f : 0.0) + (u.bd == 0 ? 1.0 : 0.0);
        float kd = (u.ba == 1 ? f : 0.0) - (u.bb == 1 ? f : 0.0) + (u.bd == 1 ? 1.0 : 0.0);
        o0 = vec4(vec3(r, g, b) / 255.0 * abs(ks), float(oa) / 255.0);
        o1 = vec4(vec3(abs(kd)), 0.0);
        return;
    }
    if (u.bmode == 2) {                                   // exact integer blend against the target copy
        int cc = u.bc == 0 ? a : (u.bc == 1 ? dst.a : u.fix);
        ivec3 cs = ivec3(r, g, b), cd = dst.rgb, o;
        for (int k = 0; k < 3; k++) {
            int va = blendsel(u.ba, cs[k], cd[k]), vb2 = blendsel(u.bb, cs[k], cd[k]), vd = blendsel(u.bd, cs[k], cd[k]);
            o[k] = (((va - vb2) * cc) >> 7) + vd;
        }
        if (u.colclamp != 0) o = clamp(o, 0, 255); else o = o & 255;
        r = o.r; g = o.g; b = o.b;
    }
    o0 = vec4(vec3(r, g, b) / 255.0, float(oa) / 255.0);
    o1 = vec4(0.0);
}
