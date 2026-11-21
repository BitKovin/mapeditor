// R6: bounce light, the part that needs the surface a gather ray hit: where it is, which way it faces, whether it takes part (its class has lightmaps), its albedo
// (the texture at the hit point, as the bake's gatherCpu does). Included by rt_ao.comp and rt_sharc_update.comp after rt_light.glsl (needs `tlas`, P and rec).
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require

#ifndef TEX_SET
#define TEX_SET 2
#endif
layout(set = TEX_SET, binding = 0) uniform sampler2D textures[];   // the texture table of the renderer (global texture indices are what the vertices carry)

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer WordBuf { uint w[]; };

struct GiHit {
    bool valid;        // a surface that takes part
    float t;
    vec3 pos;
    vec3 n;            // facing the ray's origin
    vec3 albedo;
};

// The closest hit of the ray (mask: casters) and its material. `rq` holds the committed hit.
GiHit giHitOf(rayQueryEXT rq, vec3 dir) {
    GiHit h;
    h.valid = false;
    h.t = rayQueryGetIntersectionTEXT(rq, true);
    h.pos = vec3(0.0);
    h.n = -dir;
    h.albedo = vec3(1.0);
    uint slot = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true));
    uvec4 r0 = rec[slot * 2u], r1 = rec[slot * 2u + 1u];
    if ((r1.y & 2u) == 0u) return h;   // a caster whose class has no lightmaps absorbs the light (the bake's `m[7] < 0.5`)
    WordBuf vb = WordBuf(packUint2x32(uvec2(r0.x, r0.y)));
    WordBuf ib = WordBuf(packUint2x32(uvec2(r0.z, r0.w)));
    uint prim = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true));
    uvec3 idx = uvec3(ib.w[prim * 3u], ib.w[prim * 3u + 1u], ib.w[prim * 3u + 2u]);
    vec3 p0, p1, p2;
    vec2 uv0, uv1, uv2;
    uint tex = 0u;
    {
        uint o = idx.x * 10u;
        p0 = vec3(uintBitsToFloat(vb.w[o]), uintBitsToFloat(vb.w[o + 1u]), uintBitsToFloat(vb.w[o + 2u]));
        uv0 = vec2(uintBitsToFloat(vb.w[o + 4u]), uintBitsToFloat(vb.w[o + 5u]));
        tex = vb.w[o + 9u];
        o = idx.y * 10u;
        p1 = vec3(uintBitsToFloat(vb.w[o]), uintBitsToFloat(vb.w[o + 1u]), uintBitsToFloat(vb.w[o + 2u]));
        uv1 = vec2(uintBitsToFloat(vb.w[o + 4u]), uintBitsToFloat(vb.w[o + 5u]));
        o = idx.z * 10u;
        p2 = vec3(uintBitsToFloat(vb.w[o]), uintBitsToFloat(vb.w[o + 1u]), uintBitsToFloat(vb.w[o + 2u]));
        uv2 = vec2(uintBitsToFloat(vb.w[o + 4u]), uintBitsToFloat(vb.w[o + 5u]));
    }
    vec2 bc = rayQueryGetIntersectionBarycentricsEXT(rq, true);
    vec3 w3 = vec3(1.0 - bc.x - bc.y, bc.x, bc.y);
    h.pos = p0 * w3.x + p1 * w3.y + p2 * w3.z;
    vec3 gn = cross(p1 - p0, p2 - p0);
    float worldArea = 0.5 * length(gn);
    gn = gn / max(length(gn), 1e-20);
    h.n = dot(gn, dir) > 0.0 ? -gn : gn;
    vec2 uv = uv0 * w3.x + uv1 * w3.y + uv2 * w3.z;
    // the texture at the hit, filtered over a cone of the diffuse gather (the mip level follows the footprint)
    vec2 ts = vec2(textureSize(textures[nonuniformEXT(tex)], 0));
    float uvArea = 0.5 * abs((uv1.x - uv0.x) * (uv2.y - uv0.y) - (uv2.x - uv0.x) * (uv1.y - uv0.y));
    float texelsPerMetre = sqrt(uvArea / max(worldArea, 1e-12)) * max(ts.x, ts.y);
    float lod = clamp(log2(max(1.0, max(h.t, 1.0) * 0.15 * texelsPerMetre)), 0.0, 12.0);
    h.albedo = textureLod(textures[nonuniformEXT(tex)], uv, lod).rgb;
    h.valid = true;
    return h;
}

// A cosine weighted direction around n.
vec3 cosineDir(vec3 n, inout uint seed) {
    vec3 helper = abs(n.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    vec3 tu = normalize(cross(n, helper)), tv = cross(n, tu);
    float u1 = rand01(seed), u2 = rand01(seed);
    float r = sqrt(u1), phi = 2.0 * PI * u2;
    return normalize(tu * (r * cos(phi)) + tv * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u1)));
}
