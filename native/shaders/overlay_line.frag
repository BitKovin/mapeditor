#version 460
layout(push_constant) uniform PC {
    mat4 viewProj;
    vec4 eye;
    vec4 misc;    // z = the line width in pixels
} pc;

layout(location = 0) in vec4 vColor;
layout(location = 1) noperspective in vec3 vLocal;
layout(location = 0) out vec4 outColor;

void main() {
    const float halfWidth = max(pc.misc.z, 1.0) * 0.5;
    // coverage of the pixel: one inside the line, falling to zero over one pixel across the edge and over the end
    const float side = clamp(halfWidth + 0.5 - abs(vLocal.x), 0.0, 1.0);
    const float ends = clamp(min(vLocal.y, vLocal.z - vLocal.y) + halfWidth + 0.5, 0.0, 1.0);
    const float coverage = side * ends;
    if (coverage <= 0.003) discard;
    outColor = vec4(vColor.rgb, vColor.a * coverage);
}
