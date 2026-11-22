#version 460
// Anti-aliased lines: every line segment is one instance of six vertices (a quad around the segment, expanded in screen space).
// The fragment shader turns the distance from the centre line into coverage, so the edges are smooth instead of stair-stepped.
layout(push_constant) uniform PC {
    mat4 viewProj;
    vec4 eye;     // xy = the viewport in pixels
    vec4 misc;    // z = the line width in pixels
} pc;

layout(location = 0) in vec3 aPos0;
layout(location = 1) in vec4 aColor0;
layout(location = 2) in vec3 aPos1;
layout(location = 3) in vec4 aColor1;
layout(location = 0) out vec4 vColor;
layout(location = 1) noperspective out vec3 vLocal;   // x = the distance from the centre line (px), y = the position along the segment (px), z = its length (px)

void main() {
    // the corner of the quad: which end, which side
    const vec2 kCorner[6] = vec2[6](vec2(0, -1), vec2(0, 1), vec2(1, -1), vec2(1, -1), vec2(0, 1), vec2(1, 1));
    const vec2 corner = kCorner[gl_VertexIndex % 6];

    vec4 c0 = pc.viewProj * vec4(aPos0, 1.0);
    vec4 c1 = pc.viewProj * vec4(aPos1, 1.0);
    vec4 col0 = aColor0;
    vec4 col1 = aColor1;
    // clip against the near plane (z >= 0) so a line that starts behind the camera still draws its visible part
    if (c0.z < 0.0 && c1.z < 0.0) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        vColor = vec4(0.0);
        vLocal = vec3(0.0);
        return;
    }
    if (c0.z < 0.0) {
        const float t = c0.z / (c0.z - c1.z);
        c0 = mix(c0, c1, t);
        col0 = mix(col0, col1, t);
    } else if (c1.z < 0.0) {
        const float t = c1.z / (c1.z - c0.z);
        c1 = mix(c1, c0, t);
        col1 = mix(col1, col0, t);
    }
    const vec2 half_px = pc.eye.xy * 0.5;
    const vec2 p0 = c0.xy / c0.w * half_px;
    const vec2 p1 = c1.xy / c1.w * half_px;
    const vec2 delta = p1 - p0;
    const float len = length(delta);
    const vec2 dir = len > 1e-4 ? delta / len : vec2(1.0, 0.0);
    const vec2 nrm = vec2(-dir.y, dir.x);

    const float halfWidth = max(pc.misc.z, 1.0) * 0.5;
    const float reach = halfWidth + 0.5;   // the quad covers the line and one more half pixel for the soft edge
    const bool atEnd = corner.x > 0.5;
    const vec4 c = atEnd ? c1 : c0;
    const vec2 base = atEnd ? p1 : p0;
    const float along = atEnd ? len + reach : -reach;   // the ends are pushed out by the reach so corners of connected lines close
    vec2 px = base + dir * (atEnd ? reach : -reach) + nrm * (corner.y * reach);
    gl_Position = vec4(px / half_px * c.w, c.z, c.w);
    vColor = atEnd ? col1 : col0;
    vLocal = vec3(corner.y * reach, along, len);
}
