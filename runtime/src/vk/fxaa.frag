#version 450
layout(push_constant) uniform Q { vec4 rect; vec4 uv; vec4 p0; } q;       // p0: rcp.xy, span, reduce
layout(set = 0, binding = 1) uniform sampler2D u_tex;
layout(location = 0) in vec2 v_uv; layout(location = 0) out vec4 o;
void main() {
    vec2 u_rcp = q.p0.xy; float u_span = q.p0.z, u_reduce = q.p0.w;
    vec3 L = vec3(0.299, 0.587, 0.114);
    vec3 nw = texture(u_tex, v_uv + vec2(-1.0, -1.0) * u_rcp).rgb, ne = texture(u_tex, v_uv + vec2(1.0, -1.0) * u_rcp).rgb;
    vec3 sw = texture(u_tex, v_uv + vec2(-1.0, 1.0) * u_rcp).rgb, se = texture(u_tex, v_uv + vec2(1.0, 1.0) * u_rcp).rgb;
    vec3 m = texture(u_tex, v_uv).rgb;
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
}
