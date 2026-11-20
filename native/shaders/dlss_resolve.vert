#version 460
// One triangle over the whole output: the DLSS result is copied to the swapchain image by dlss_resolve.frag.
layout(location = 0) out vec2 vUv;

void main() {
    vec2 c = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);   // (0,0) (2,0) (0,2)
    vUv = c;
    gl_Position = vec4(c * 2.0 - 1.0, 0.0, 1.0);
}
