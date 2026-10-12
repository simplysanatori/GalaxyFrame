#version 450
// Smoke test of the gq_compile_glsl CMake macro: the fullscreen triangle with
// the push constant layout of the Vulkan dialect (see gl_shadergen.h).
layout(push_constant, std430) uniform PushBlock {
    uint u_firstOrdinal;
    int u_copyMode;
    uint u_pad0;
    uint u_pad1;
    vec4 u_copyRect;
};
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 p = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    v_uv = mix(u_copyRect.xy, u_copyRect.zw, vec2(p.x, 1.0 - p.y));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
