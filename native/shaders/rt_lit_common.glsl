// The ray traced Lit path (R2, 12_RT_VIEWPORT.md): bindings and per frame parameters shared by rt_ao.comp, rt_filter.comp and rt_compose.comp.
// One descriptor set per frame in flight, written by render/rt_lit.cpp every frame (the history images swap their roles each frame).
//
// G-buffer (rasterised, render size): uAlbedo = the surface's colour (or the sky), uNormal = world normal + 1 (0 where there is no surface), uDist = distance along
// the view direction (the clip w). History (render size): uHist = (mean occlusion, mean of its square, sample count N, distance of that pixel).
#extension GL_KHR_shader_subgroup_basic : require

layout(set = 0, binding = 0) uniform sampler2D uDist;
layout(set = 0, binding = 1) uniform sampler2D uAlbedo;
layout(set = 0, binding = 2) uniform sampler2D uNormal;
layout(set = 0, binding = 3) uniform sampler2D uNormalPrev;
layout(set = 0, binding = 4, rgba32f) uniform image2D uHistPrev;
layout(set = 0, binding = 5, rgba32f) uniform image2D uHist;
layout(set = 0, binding = 7, rg16f) uniform image2D uFil0;
layout(set = 0, binding = 8, rg16f) uniform image2D uFil1;
layout(set = 0, binding = 9, rgba16f) uniform image2D uOut;
layout(set = 0, binding = 10, r16f) uniform image2D uSample;
layout(set = 0, binding = 11, std430) buffer Counters { uint traced; uint surface; } counters;
// R3: the direct light's history (mean D rgb, mean of the square of its luminance), the frame's D sample (for DLSS Ray Reconstruction) and the filtered D (rgb, variance of the mean luminance).
layout(set = 0, binding = 13, rgba32f) uniform image2D uHistDPrev;
layout(set = 0, binding = 14, rgba32f) uniform image2D uHistD;
layout(set = 0, binding = 15, rgba16f) uniform image2D uSampleD;
layout(set = 0, binding = 19, rgba16f) uniform image2D uFilD0;
layout(set = 0, binding = 20, rgba16f) uniform image2D uFilD1;
layout(set = 0, binding = 12, std140) uniform Params {
    vec4 rayA;       // the pixel's ray: direction = rayA + rayX * ndc.x + rayY * ndc.y (ndc of the pixel centre, jittered), not normalised
    vec4 rayX;
    vec4 rayY;
    vec4 eye;        // xyz
    vec4 fwd;        // xyz: the camera's forward axis
    vec4 size;       // render width, height
    mat4 prevViewProj;   // the previous frame's unjittered view-projection
    uvec4 u;         // x = frame number, y = flags (1 reset, 2 still, 4 filter ran, 8 radiance for DLSS RR), z = sample cap, w = target sample count
    vec4 ambient;    // rgb, w = 1 when the level's occlusion is active
    vec4 ao;         // radius, intensity, strength, power
    vec4 ao2;        // falloff, origin bias, exposure, brightness
    vec4 mode;       // x = render mode (1 lit, 2 lighting only), y = sample cap while the camera moves, z = samples at which the mean replaces the filter, w = same for RR
    vec4 gridOrigin; // xyz = the light grid's origin, w = the edge of a cell
    uvec4 gridDim;   // xyz = cells per axis
    uvec4 lightInfo; // x = lights, y = directional / sky lights (the first entries of the index list), z = 1 when direct light is traced
    vec4 look;       // x = lightmap texels per metre for the lightmap look (0 = off), y = shadow ray origin bias, z = far distance
    vec4 gi;         // bounce light (R6): x = the bake's surface albedo, y = the gather range, z = 1 when bounces are on, w = longest update path (vertices)
    uvec4 giB;       // x = the update samples one pixel of every giB.x * giB.x block
} P;

// The object records of render/rt_scene.h (RtRecord, 32 bytes = 2 uvec4 per instance slot = instanceCustomIndex): rec[2s] = (vertex address lo, hi, index address lo, hi),
// rec[2s + 1] = (triangle count, flags: bit 0 raw mesh, bit 1 the object receives baked light, vertex count, 0).
layout(set = 0, binding = 21, std430) readonly buffer Records { uvec4 rec[]; };

// The instance masks of render/rt_scene.h: only objects of a class that blocks light (the game profile's blockLight, core::LightingClasses) are seen by the shadow, occlusion and light rays.
const uint RT_MASK_SOLID = 1u;

const uint F_RESET = 1u, F_STILL = 2u, F_FILTER = 4u, F_RR = 8u;

bool hasSurface(ivec2 p) { return texelFetch(uNormal, p, 0).w > 0.5; }
// A surface that receives baked light (its class has lightmaps): G-buffer normal alpha 1; 0.6 = a surface drawn unlit (doors, lifts: lit from light volumes in the game).
bool receivesLight(ivec2 p) { return texelFetch(uNormal, p, 0).w > 0.8; }

// The world position of the surface seen through pixel p at the distance `dist` (rasterised exactly, so no depth buffer quantisation).
vec3 worldOf(ivec2 p, float dist) {
    vec2 ndc = (vec2(p) + 0.5) / P.size.xy * 2.0 - 1.0;
    vec3 v = P.rayA.xyz + P.rayX.xyz * ndc.x + P.rayY.xyz * ndc.y;
    return P.eye.xyz + v * (dist / dot(v, P.fwd.xyz));
}

// The shown factor of the level's ambient occlusion (core/lights.h): 1 - intensity * (1 - (1 - min(1, strength * occlusion))^power), 1 when it is not active.
float aoFactor(float occlusion) {
    if (P.ambient.w < 0.5) return 1.0;
    float t = clamp(1.0 - P.ao.z * occlusion, 0.0, 1.0);
    float p = P.ao.w == 1.0 ? t : pow(t, P.ao.w);
    return 1.0 - P.ao.y * (1.0 - p);
}
