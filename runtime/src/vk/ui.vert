#version 450
layout(push_constant) uniform Q { vec4 rect; vec4 uv; vec4 p0; } q;       // p0.xy = screen size
layout(std430, set = 0, binding = 0) readonly buffer V { float vb[]; };
layout(location = 0) out vec2 v_uv; layout(location = 1) out vec4 v_col; layout(location = 2) out vec2 v_local;
layout(location = 3) out vec4 v_shape; layout(location = 4) flat out int v_kind;
void main() {
    uint i = uint(gl_VertexIndex) * 15u;
    gl_Position = vec4(vb[i] / q.p0.x * 2.0 - 1.0, vb[i + 1] / q.p0.y * 2.0 - 1.0, 0.0, 1.0);
    v_uv = vec2(vb[i + 2], vb[i + 3]); v_col = vec4(vb[i + 4], vb[i + 5], vb[i + 6], vb[i + 7]);
    v_local = vec2(vb[i + 8], vb[i + 9]); v_shape = vec4(vb[i + 10], vb[i + 11], vb[i + 12], vb[i + 13]);
    v_kind = int(vb[i + 14] + 0.5);
}
