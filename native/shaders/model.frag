#version 460
#extension GL_EXT_nonuniform_qualifier : require
// Shading of a model: the texture (or the material colour) with the same readability tint as the unlit scene. The lit modes show a model dimmed (a model has no lightmap);
// the ray traced Lit view takes the G-buffer variant (GBUFFER) and lights it per pixel like any surface.
layout(set = 0, binding = 0) uniform sampler2D textures[];
layout(set = 0, binding = 1, std430) readonly buffer Meta { vec4 meta[]; };   // x mode (0 opaque, 1 cutout, 2 translucent), y cutoff

layout(push_constant) uniform PC {
    mat4 viewProj;
    vec4 color;
    vec4 mat;     // x = texture index, y = 1 when the texture is used, z = alpha mode (0 opaque, 1 cutout), w = cutoff
    vec4 lit;     // x = mode (0 unlit, 1 lit, 2 lighting only), y = exposure, z = brightness, w = texture lod bias
} pc;

layout(location = 0) in vec3 vN;
layout(location = 1) in vec2 vUv;
layout(location = 2) flat in uint vLight;
layout(location = 0) out vec4 outColor;
#ifdef GBUFFER
layout(location = 1) out vec4 outNormal;
layout(location = 2) out float outDepth;
#endif

// The light volume's value for the entity (T10.1, see mesh.frag): the models are lit by it, like the game does, not per pixel.
const uint ENTITY_META = 4096u;
vec3 entityLight(uint entry, vec3 n) {
    uint b = ENTITY_META + 3u * (entry - 1u);
    return meta[b].rgb + meta[b + 1u].rgb * max(0.0, dot(n, meta[b + 2u].xyz));
}

void main() {
    vec3 n = normalize(vN);
    if (!gl_FrontFacing) n = -n;
    vec3 albedo = pc.color.rgb;
    float a = pc.color.a;
    if (pc.mat.y > 0.5) {
        uint ti = uint(pc.mat.x);
        vec4 t = texture(textures[nonuniformEXT(ti)], vUv, pc.lit.w);
        albedo *= t.rgb;
        a *= t.a;
        vec4 m = meta[ti];                      // the texture's own transparency (the .obj models' textures)
        if (m.x > 0.5 && m.x < 1.5 && t.a < m.y) discard;
        if (m.x > 1.5 && t.a < 0.5) discard;    // translucent textures are cut at one half (no blended model pass yet)
    }
    if (pc.mat.z > 0.5 && a < pc.mat.w) discard;
#ifdef GBUFFER
    if (vLight != 0u) {   // the finished colour goes into the albedo target (0.7 = shown as it is by the compose pass)
        outColor = vec4(clamp((pc.lit.x > 1.5 ? vec3(1.0) : albedo) * entityLight(vLight, n) * pc.lit.y + pc.lit.z, 0.0, 1.0), 1.0);
        outNormal = vec4(n, 0.7);
    } else {
        outColor = vec4(albedo, 1.0);
        outNormal = vec4(n, 1.0);
    }
    outDepth = 1.0 / gl_FragCoord.w;
    return;
#endif
    float s = 0.7 + 0.3 * abs(dot(n, normalize(vec3(0.35, 0.85, 0.4))));
    float mode = pc.lit.x;
    if (mode > 0.5 && vLight != 0u) outColor = vec4(clamp((mode > 1.5 ? vec3(1.0) : albedo) * entityLight(vLight, n) * pc.lit.y + pc.lit.z, 0.0, 1.0), 1.0);
    else if (mode > 1.5) outColor = vec4(vec3(0.35) * s, 1.0);
    else if (mode > 0.5) outColor = vec4(albedo * 0.6 * s, 1.0);
    else outColor = vec4(albedo * s, 1.0);
}
