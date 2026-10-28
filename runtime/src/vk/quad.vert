#version 450
// a rectangle from push constants: rect = x0 y0 x1 y1 (Vulkan NDC, y down), uv = u0 v0 u1 v1
layout(push_constant) uniform Q { vec4 rect; vec4 uv; vec4 p0; vec4 p1; vec4 p2; } q;
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 c = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
    gl_Position = vec4(mix(q.rect.xy, q.rect.zw, c), 0.0, 1.0);
    v_uv = mix(q.uv.xy, q.uv.zw, c);
}
