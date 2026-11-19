#version 460
#extension GL_EXT_nonuniform_qualifier : require
// Unlit shading of the scene (the Shell's MESH_FS): the texture's colour times a readability tint from the normal. Sky faces are sampled by view
// direction as the engine does (shell/src/model/sky.ts cubemapLookup / panoramaLookup), subtractive volumes are translucent red, transparency
// comes from the texture's meta (0 opaque, 1 cutout, 2 translucent; skyboxes: 3 faces, 4 panorama).
layout(set = 0, binding = 0) uniform sampler2D textures[];
layout(set = 0, binding = 1, std430) readonly buffer Meta { vec4 meta[]; };   // x mode, y cutoff, z opacity, w has alpha

layout(push_constant) uniform PC {
    mat4 viewProj;
    vec4 eye;
    vec4 misc;    // x = pass (0 opaque, 1 translucent, 2 volume, 3 sky background)
} pc;

layout(location = 0) in vec3 vN;
layout(location = 1) in vec2 vUv;
layout(location = 2) flat in uint vTex;
layout(location = 3) flat in uint vFlags;
layout(location = 4) in vec3 vDir;
layout(location = 0) out vec4 outColor;

vec3 skyColor(uint base, float kind, vec3 dir) {
    if (kind > 3.5) {
        float u = atan(dir.x, -dir.z) / 6.28318530718 + 0.5;
        float v = 0.5 - asin(clamp(dir.y / max(length(dir), 1e-9), -1.0, 1.0)) / 3.14159265359;
        return textureLod(textures[nonuniformEXT(base)], vec2(u, v), 0.0).rgb;
    }
    vec3 d = vec3(-dir.x, dir.y, dir.z);   // the engine samples its cube with x negated
    float ax = abs(d.x), ay = abs(d.y), az = abs(d.z);
    uint face;
    float sc, tc, ma;
    if (ax >= ay && ax >= az) {
        ma = ax;
        if (d.x > 0.0) { face = 0u; sc = -d.z; tc = -d.y; } else { face = 1u; sc = d.z; tc = -d.y; }
    } else if (ay >= az) {
        ma = ay;
        if (d.y > 0.0) { face = 2u; sc = d.x; tc = d.z; } else { face = 3u; sc = d.x; tc = -d.z; }
    } else {
        ma = az;
        if (d.z > 0.0) { face = 4u; sc = d.x; tc = -d.y; } else { face = 5u; sc = -d.x; tc = -d.y; }
    }
    float u = 0.0, v = 0.0;
    if (ma > 0.0) { u = (sc / ma + 1.0) * 0.5; v = (tc / ma + 1.0) * 0.5; }
    if (face == 2u) { float t = u; u = v; v = 1.0 - t; }          // _up is turned 90 degrees clockwise on load
    else if (face == 3u) { float t = u; u = 1.0 - v; v = t; }     // _dn counter-clockwise
    return textureLod(textures[nonuniformEXT(base + face)], vec2(u, v), 0.0).rgb;
}

void main() {
    float pass = pc.misc.x;
    vec3 n = normalize(vN);
    float s = 0.7 + 0.3 * abs(dot(n, normalize(vec3(0.35, 0.85, 0.4))));
    if (pass > 1.5 && pass < 2.5) {
        outColor = vec4(vec3(0.95, 0.25, 0.25) * s, 0.28);
        return;
    }
    vec4 m = meta[vTex];
    if (m.x > 2.5 && ((vFlags & 1u) != 0u || pass > 2.5)) {
        outColor = vec4(skyColor(vTex, m.x, vDir), 1.0);
        return;
    }
    vec4 t = texture(textures[nonuniformEXT(vTex)], vUv);
    float a = 1.0;
    if (m.x > 1.5) {                 // translucent
        if (pass < 0.5) discard;     // drawn in the blended pass
        a = m.z * t.a;
    } else {
        if (pass > 0.5) discard;
        if (m.x > 0.5 && t.a < m.y) discard;   // cutout
    }
    outColor = vec4(t.rgb * s, a);
}
