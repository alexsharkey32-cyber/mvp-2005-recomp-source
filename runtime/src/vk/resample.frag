#version 450
layout(push_constant) uniform Q { vec4 rect; vec4 uv; vec4 p0; ivec4 p1; } q;   // p0: src.xy dst.xy; p1.x mode
layout(set = 0, binding = 1) uniform sampler2D u_tex;
layout(location = 0) in vec2 v_uv; layout(location = 0) out vec4 o;
float cw(float x) { x = abs(x); return x < 1.0 ? 1.5*x*x*x - 2.5*x*x + 1.0 : x < 2.0 ? -0.5*x*x*x + 2.5*x*x - 4.0*x + 2.0 : 0.0; }
void main() {
    vec2 u_src = q.p0.xy, u_dst = q.p0.zw; int u_mode = q.p1.x;
    if (u_mode == 2) {
        vec2 p = v_uv * u_src - 0.5; vec2 f = fract(p); ivec2 b = ivec2(floor(p));
        vec3 acc = vec3(0.0); float ws = 0.0;
        for (int j = -1; j <= 2; j++) for (int i = -1; i <= 2; i++) {
            float w = cw(float(i) - f.x) * cw(float(j) - f.y);
            ivec2 c = clamp(b + ivec2(i, j), ivec2(0), ivec2(u_src) - 1);
            acc += w * texelFetch(u_tex, c, 0).rgb; ws += w;
        }
        o = vec4(acc / ws, 1.0);
    } else if (u_mode == 3) {
        vec2 fp = u_src / u_dst;
        vec2 n = clamp(ceil(fp), vec2(1.0), vec2(6.0));
        vec3 acc = vec3(0.0);
        for (float j = 0.0; j < 6.0; j++) for (float i = 0.0; i < 6.0; i++) {
            if (i >= n.x || j >= n.y) continue;
            vec2 off = ((vec2(i, j) + 0.5) / n - 0.5) * fp / u_src;
            acc += texture(u_tex, v_uv + off).rgb;
        }
        o = vec4(acc / (n.x * n.y), 1.0);
    } else o = vec4(texture(u_tex, v_uv).rgb, 1.0);
}
