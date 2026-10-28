#version 450
#include "common.glsl"
layout(std430, set = 0, binding = 0) readonly buffer V { float vb[]; };
layout(location = 0) noperspective out vec4 v_col; layout(location = 1) flat out vec4 v_fcol;
layout(location = 2) noperspective out vec3 v_stq; layout(location = 3) noperspective out vec2 v_uv;
layout(location = 4) noperspective out float v_fog; layout(location = 5) noperspective out float v_z;
layout(location = 6) flat out vec4 v_clamp;
void main() {
    uint per = uint(bits(pc.f0, 28, 2));
    uint i = uint(gl_VertexIndex) * 17u;
    uint last = (pc.first + ((uint(gl_VertexIndex) - pc.first) / per) * per + (per - 1u)) * 17u;
    gl_Position = vec4(vb[i], vb[i + 1], 0.0, 1.0);   // Vulkan: NDC y -1 = row 0 = GS y 0
    gl_PointSize = 1.0;
    v_z = vb[i + 2];
    v_col = vec4(vb[i + 3], vb[i + 4], vb[i + 5], vb[i + 6]);
    v_fcol = vec4(vb[last + 3], vb[last + 4], vb[last + 5], vb[last + 6]);
    v_stq = vec3(vb[i + 7], vb[i + 8], vb[i + 9]);
    v_uv = vec2(vb[i + 10], vb[i + 11]);
    v_fog = vb[i + 12];
    v_clamp = vec4(vb[i + 13], vb[i + 14], vb[i + 15], vb[i + 16]);
}
