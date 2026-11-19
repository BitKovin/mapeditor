#version 460
#extension GL_EXT_nonuniform_qualifier : require
layout(set = 0, binding = 0) uniform sampler2D textures[];

layout(push_constant) uniform PC {
    mat4 viewProj;
    vec4 eye;
    vec4 misc;
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 t = texture(textures[nonuniformEXT(uint(pc.misc.y))], vUv);
    // r = the glyph, g = the glyph with its outline: dark outline, coloured fill
    float alpha = max(t.r, t.g * 0.85) * vColor.a;
    if (alpha < 0.01) discard;
    outColor = vec4(mix(vec3(0.0), vColor.rgb, t.r), alpha);
}
