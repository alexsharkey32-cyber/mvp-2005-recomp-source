#version 450
layout(push_constant) uniform Q { vec4 rect; vec4 uv; vec4 p0; vec4 color; ivec4 p2; } q;  // p0: size.xy sharp gamma; p2: crt, lines(as int)
layout(set = 0, binding = 1) uniform sampler2D u_tex;
layout(location = 0) in vec2 v_uv; layout(location = 0) out vec4 o;
vec3 px(vec2 uv, vec2 d) { return texture(u_tex, uv + d / q.p0.xy).rgb; }
void main() {
    vec2 u_size = q.p0.xy; float u_sharp = q.p0.z, u_gamma = q.p0.w; int u_crt = q.p2.x; float u_lines = float(q.p2.y);
    vec2 uv = v_uv;
    float vig = 1.0;
    if (u_crt == 2) {
        vec2 c = uv * 2.0 - 1.0;
        c *= 1.0 + 0.045 * dot(c.yx, c.yx);
        uv = c * 0.5 + 0.5;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) { o = vec4(0.0, 0.0, 0.0, 1.0); return; }
        vec2 e = uv * (1.0 - uv.yx); vig = clamp(pow(e.x * e.y * 18.0, 0.22), 0.0, 1.0);
    }
    vec3 col = px(uv, vec2(0.0));
    if (u_sharp > 0.0) {
        vec3 b = px(uv, vec2(0.0, -1.0)), d = px(uv, vec2(-1.0, 0.0)), f = px(uv, vec2(1.0, 0.0)), h = px(uv, vec2(0.0, 1.0));
        vec3 mn = min(min(min(d, col), min(f, b)), h), mx = max(max(max(d, col), max(f, b)), h);
        vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));
        vec3 w = amp * (-1.0 / mix(8.0, 5.0, u_sharp));
        col = clamp((b * w + d * w + f * w + h * w + col) / (1.0 + 4.0 * w), 0.0, 1.0);
    }
    col = (col - 0.5) * q.color.y + 0.5 + q.color.x;
    float l = dot(col, vec3(0.299, 0.587, 0.114));
    col = mix(vec3(l), col, q.color.z);
    col = pow(clamp(col, 0.0, 1.0), vec3(1.0 / u_gamma));
    if (u_crt >= 1) {
        float row = uv.y * u_lines;
        float ppl = u_size.y / u_lines;
        float amt = clamp((ppl - 1.6) / 1.4, 0.0, 1.0);
        float d = abs(fract(row) - 0.5) * 2.0;
        float s = 1.0 - smoothstep(0.35, 1.0, d);
        col *= mix(1.0, 0.62 + 0.38 * s, amt);
        col *= 1.0 + 0.14 * amt;
    }
    o = vec4(col * vig, 1.0);
}
