#version 460
// R7: translucent surfaces (glass) of the ray traced Lit viewport, lit per fragment in the blended pass (12_RT_VIEWPORT.md section 8). There is no G-buffer entry and no accumulation
// for them, so the light is deterministic and noise free instead of unbiased: the light of the surface's own side (the bake lights a face by its normal too)
//   * ambient, and every light's unshadowed contribution (evalLight of rt_light.glsl) with one shadow ray to the centre of the light for the four strongest local lights of the
//     grid cell, the sun's ray, and the sky's irradiance without a ray; the style layers are weighted by their animated values like the compose pass does;
//   * the bounce light: the mean of 8 fixed cosine weighted directions, each looking up the SHaRC cache at the surface it hits (the bake's gather: albedo * `albedo` * window * S).
// The colour is clamp(albedo * L * exposure + brightness), the alpha the texture's, as in mesh.frag. Bound sets: 0 = texture table, 1 = display pages (unused), 2 = the ray traced
// Lit set of RtLit (Params, lights, records), 3 = the acceleration structure.
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_ray_query : require
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#extension GL_EXT_shader_atomic_int64 : require
#extension GL_KHR_shader_subgroup_ballot : require
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_EXT_control_flow_attributes : require

#define SHARC_ENABLE_GLSL 1
#define SHARC_UPDATE 0
#define SHARC_FROM_PARAMS 1
#define RT_SET 2
#define TEX_SET 0

layout(set = 0, binding = 1, std430) readonly buffer Meta { vec4 meta[]; };   // x mode, y cutoff, z opacity, w has alpha

layout(push_constant) uniform PC {
    mat4 viewProj;
    vec4 eye;
    vec4 misc;
    vec4 lit;     // w = texture lod bias
} pc;

layout(location = 0) in vec3 vN;
layout(location = 1) in vec2 vUv;
layout(location = 2) flat in uint vTex;
layout(location = 3) flat in uint vFlags;
layout(location = 4) in vec3 vDir;
layout(location = 5) in vec2 vLm;
layout(location = 6) flat in uint vPage;
layout(location = 7) flat in uint vObj;
layout(location = 0) out vec4 outColor;

#include "rt_lit_common.glsl"
layout(set = 3, binding = 0) uniform accelerationStructureEXT tlas;

const float PI = 3.14159265358979323846;
uint pcg(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float rand01(inout uint seed) {
    seed = pcg(seed);
    return float(seed >> 8u) * (1.0 / 16777216.0);
}

#include "rt_light.glsl"
#include "rt_gi.glsl"
#include "rt_sharc_common.glsl"

// The direct light of one style layer at p (normal n): see the header.
vec3 layerLight(uint layer, vec3 p, vec3 n, vec3 o) {
    vec3 sum = vec3(0.0);
    for (uint g = 0u; g < P.layerCnt[layer]; ++g) {
        uint k = lightIndex[P.layerOff[layer] + g];
        int type = int(lt[k * LREC + 1u].w + 0.5);
        vec3 v = evalLight(k, p, n);
        if (type == 2 && lt[k * LREC + 5u].z > 0.5 && (v.x != 0.0 || v.y != 0.0 || v.z != 0.0)) v *= transmit(o, -lt[k * LREC + 2u].xyz, P.look.z);
        sum += v;
    }
    vec3 rel = (p - P.gridOrigin.xyz) / P.gridOrigin.w;
    ivec3 c = ivec3(floor(rel));
    if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, ivec3(P.gridDim.xyz)))) return sum;
    uvec2 cell = cells[layer * P.layerInfo.y + (uint(c.z) * P.gridDim.y + uint(c.y)) * P.gridDim.x + uint(c.x)];
    uint bestK[4] = uint[4](0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu);
    vec3 bestV[4] = vec3[4](vec3(0.0), vec3(0.0), vec3(0.0), vec3(0.0));
    float bestW[4] = float[4](0.0, 0.0, 0.0, 0.0);
    for (uint i = 0u; i < cell.y; ++i) {
        uint k = lightIndex[cell.x + i];
        vec3 v = evalLight(k, p, n);
        float w = abs(v.x) + abs(v.y) + abs(v.z);
        if (w == 0.0) continue;
        int slot = -1;                       // the weakest of the four kept, when this one beats it
        float weakest = w;
        for (int j = 0; j < 4; ++j)
            if (bestK[j] == 0xFFFFFFFFu) { slot = j; break; }
            else if (bestW[j] < weakest) { weakest = bestW[j]; slot = j; }
        if (slot < 0) continue;
        bestK[slot] = k;
        bestV[slot] = v;
        bestW[slot] = w;
    }
    for (int j = 0; j < 4; ++j) {
        if (bestK[j] == 0xFFFFFFFFu) continue;
        vec3 v = bestV[j];
        if (lt[bestK[j] * LREC + 5u].z > 0.5) v *= blocked(o, lt[bestK[j] * LREC + 0u].xyz);   // one ray to the centre of the light
        sum += v;
    }
    return sum;
}

// The bounce light at p: 8 fixed cosine weighted directions (the same on every fragment, so the result is smooth), each reading the cache at the surface it hits.
vec3 bounceLight(vec3 p, vec3 n) {
    vec3 t, b;
    frameOf(n, t, b);
    vec3 gi = vec3(0.0);
    [[unroll]] for (int i = 0; i < 8; ++i) {
        float u1 = (float(i) + 0.5) / 8.0, u2 = float(bitfieldReverse(uint(i))) * 2.3283064365386963e-10;
        float r = sqrt(u1), phi = 2.0 * PI * u2;
        vec3 dir = normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u1)));
        rayQueryEXT rq;
        rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, RT_MASK_SOLID, p + n * P.look.y, 0.0, dir, P.gi.y);
        while (rayQueryProceedEXT(rq)) {}
        if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) continue;
        GiHit hit = giHitOf(rq, dir);
        if (!hit.valid || hit.t >= P.gi.y) continue;
        SharcHitData hd;
        hd.positionWorld = hit.pos;
        hd.normalWorld = hit.n;
        vec3 s;
        if (SharcGetCachedRadiance(sharcParameters(), hd, s, false)) gi += s * hit.albedo * (P.gi.x * window(hit.t, P.gi.y));
    }
    return gi * (1.0 / 8.0);
}

void main() {
    vec3 n = normalize(vN);
    vec4 m = meta[vTex];
    vec4 t = texture(textures[nonuniformEXT(vTex)], vUv, pc.lit.w);
    float a = m.z * t.a;
    if ((vObj & 1u) == 0u) {   // a class without lightmaps: the unlit look of the other modes
        float s = 0.7 + 0.3 * abs(dot(n, normalize(vec3(0.35, 0.85, 0.4))));
        outColor = vec4((P.mode.x > 1.5 ? vec3(0.35) : t.rgb * 0.6) * s, a);
        return;
    }
    vec3 p = pc.eye.xyz + vDir;
    vec3 o = p + n * P.look.y;
    vec3 L = P.ambient.rgb;
    if (P.lightInfo.z != 0u)
        for (uint layer = 0u; layer < min(P.layerInfo.x, 4u); ++layer) {
            float w = styleWeight(layer);
            if (w != 0.0) L += w * layerLight(layer, p, n, o);
        }
    if (P.gi.z > 0.5) L += bounceLight(p, n);
    L = max(vec3(0.0), L);
    vec3 base = P.mode.x > 1.5 ? vec3(1.0) : t.rgb;
    outColor = vec4(clamp(base * L * P.ao2.z + P.ao2.w, 0.0, 1.0), a);
}
