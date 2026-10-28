#version 450
layout(set = 0, binding = 1) uniform sampler2D u_font;
layout(set = 0, binding = 2) uniform sampler2D u_img;
layout(location = 0) in vec2 v_uv; layout(location = 1) in vec4 v_col; layout(location = 2) in vec2 v_local;
layout(location = 3) in vec4 v_shape; layout(location = 4) flat in int v_kind;
layout(location = 0) out vec4 o;
float sdbox(vec2 p, vec2 b, float r) { vec2 q = abs(p) - b + r; return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r; }
void main() {
    float a;
    if (v_kind == 1) a = texture(u_font, v_uv, -0.35).r;
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
}
