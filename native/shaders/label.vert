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
        // the anchor lands on a whole pixel, so the glyph texels map 1:1 onto the screen (crisp text that does not shimmer while the camera moves)
        const vec2 ndc = clip.xy / clip.w;
        const vec2 snapped = floor((ndc * 0.5 + 0.5) * pc.eye.xy + 0.5) / pc.eye.xy * 2.0 - 1.0;
        clip.xy = (snapped + aOffset * 2.0 / pc.eye.xy) * clip.w;
        gl_Position = clip;
    }
    vUv = aUv;
    vColor = aColor;
    // the names of unselected entities (alpha 254 / 255, see render/entity_overlay.h) show only near the camera: full up to 6 m, gone at 8 m, never in an orthographic view
    if (aColor.a < 0.999) vColor.a *= pc.misc.z > 0.5 ? 1.0 - smoothstep(6.0, 8.0, clip.w) : 0.0;
}
