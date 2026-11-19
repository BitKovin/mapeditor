#version 460
// Text quads: a world anchor projected to the screen, the corners of the glyph cell offset from it in pixels.
layout(push_constant) uniform PC {
    mat4 viewProj;
    vec4 eye;     // xy = the viewport in pixels
    vec4 misc;    // y = the atlas texture index
} pc;

layout(location = 0) in vec3 aAnchor;
layout(location = 1) in vec2 aOffset;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aColor;
layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;

void main() {
    vec4 clip = pc.viewProj * vec4(aAnchor, 1.0);
    if (clip.w <= 1e-4) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);   // behind the camera: off screen
    } else {
        clip.xy += aOffset * 2.0 / pc.eye.xy * clip.w;
        gl_Position = clip;
    }
    vUv = aUv;
    vColor = aColor;
}
