#version 450
layout(set = 0, binding = 1) uniform sampler2D t;
layout(location = 0) in vec2 v_uv; layout(location = 0) out vec4 o;
void main() { ivec2 s = textureSize(t, 0); o = texelFetch(t, min(ivec2(v_uv * vec2(s)), s - 1), 0); }
