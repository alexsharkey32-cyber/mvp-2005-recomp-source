#version 450
layout(push_constant) uniform Q { vec4 rect; vec4 uv; ivec4 p0; } q;      // p0.x = scale
layout(set = 0, binding = 1) uniform sampler2D col;
layout(set = 0, binding = 2) uniform sampler2D dep;
layout(location = 0) out vec4 o; layout(location = 1) out float oz;
void main() { ivec2 p = ivec2(gl_FragCoord.xy) * q.p0.x + q.p0.x / 2; o = texelFetch(col, p, 0); oz = texelFetch(dep, p, 0).r; }
