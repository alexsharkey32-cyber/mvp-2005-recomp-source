#version 450
layout(push_constant) uniform Q { vec4 rect; vec4 uv; ivec4 p0; } q;      // p0.x = DATM
layout(set = 0, binding = 1) uniform sampler2D t;
layout(location = 0) out vec4 o;
void main() { int a = int(texelFetch(t, ivec2(gl_FragCoord.xy), 0).a * 255.0 + 0.5); if (((a >> 7) & 1) != q.p0.x) discard; o = vec4(0.0); }
