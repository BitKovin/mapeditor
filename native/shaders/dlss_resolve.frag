#version 460
// The upscaled picture goes to the swapchain image, and the render-resolution depth (nearest sample) to the output-resolution depth buffer, so
// the overlays drawn next (grid, entity boxes, selection lines, gizmo, labels) are depth tested against the scene at the output size.
layout(set = 0, binding = 0) uniform sampler2D uColor;   // DLSS output, output size, 1:1 with the swapchain image
layout(set = 0, binding = 1) uniform sampler2D uDepth;   // render-size depth, nearest filtering

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(texelFetch(uColor, ivec2(gl_FragCoord.xy), 0).rgb, 1.0);
    gl_FragDepth = texture(uDepth, vUv).r;
}
