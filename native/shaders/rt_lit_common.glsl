// The ray traced Lit path (R2, 12_RT_VIEWPORT.md): bindings and per frame parameters shared by rt_ao.comp, rt_filter.comp and rt_compose.comp.
// One descriptor set per frame in flight, written by render/rt_lit.cpp every frame (the history images swap their roles each frame).
//
// G-buffer (rasterised, render size): uAlbedo = the surface's colour (or the sky), uNormal = world normal + 1 (0 where there is no surface), uDist = distance along
// the view direction (the clip w). History (render size): uHist = (mean occlusion, mean of its square, sample count N, distance of that pixel).
#extension GL_KHR_shader_subgroup_basic : require
// RT_SET: the descriptor set of the ray traced Lit kernels (0 in the compute passes, 2 in the translucent surface shader mesh_glass.frag)
#ifndef RT_SET
#define RT_SET 0
#endif

layout(set = RT_SET, binding = 0) uniform sampler2D uDist;
layout(set = RT_SET, binding = 1) uniform sampler2D uAlbedo;
layout(set = RT_SET, binding = 2) uniform sampler2D uNormal;
layout(set = RT_SET, binding = 3) uniform sampler2D uNormalPrev;
layout(set = RT_SET, binding = 4, rgba32f) uniform image2D uHistPrev;
layout(set = RT_SET, binding = 5, rgba32f) uniform image2D uHist;
layout(set = RT_SET, binding = 7, rg16f) uniform image2D uFil0;
layout(set = RT_SET, binding = 8, rg16f) uniform image2D uFil1;
layout(set = RT_SET, binding = 9, rgba16f) uniform image2D uOut;
layout(set = RT_SET, binding = 10, r16f) uniform image2D uSample;
layout(set = RT_SET, binding = 11, std430) buffer Counters { uint traced; uint surface; } counters;
// R3: the direct light's history (mean D rgb, mean of the square of its luminance), the frame's D sample (for DLSS Ray Reconstruction) and the filtered D (rgb, variance of the mean luminance).
layout(set = RT_SET, binding = 13, rgba32f) uniform image2D uHistDPrev;
layout(set = RT_SET, binding = 14, rgba32f) uniform image2D uHistD;
layout(set = RT_SET, binding = 15, rgba16f) uniform image2D uSampleD;
layout(set = RT_SET, binding = 19, rgba16f) uniform image2D uFilD0;
layout(set = RT_SET, binding = 20, rgba16f) uniform image2D uFilD1;
// R7: the direct light of the style layers 1..3 (12 section 4): running means (rgb) of the previous frame and of this frame, one image per layer. The frame's total sample (all layers
// weighted by the style values) goes into uSampleD, the means are weighted by the compose pass.
layout(set = RT_SET, binding = 22, rgba32f) uniform image2D uHistSPrev0;
layout(set = RT_SET, binding = 23, rgba32f) uniform image2D uHistSPrev1;
layout(set = RT_SET, binding = 24, rgba32f) uniform image2D uHistSPrev2;
layout(set = RT_SET, binding = 25, rgba32f) uniform image2D uHistSCur0;
layout(set = RT_SET, binding = 26, rgba32f) uniform image2D uHistSCur1;
layout(set = RT_SET, binding = 27, rgba32f) uniform image2D uHistSCur2;
layout(set = RT_SET, binding = 12, std140) uniform Params {
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
    vec4 styleW;     // R7: the multipliers of the style layers 1..3 (layer 0 is always 1)
    uvec4 layerOff;  // per layer: the first entry of its sun / sky lights in the index list
    uvec4 layerCnt;  // per layer: how many of them
    uvec4 layerInfo; // x = style layers (1 without styled lights), y = grid cells per layer
    uvec4 sharcA;    // R7 (glass): the SHaRC buffers for a shader without the push constants: hash entries address (lo, hi), accumulation address (lo, hi)
    uvec4 sharcB;    // resolved address (lo, hi), capacity, the radiance scale as float bits
    vec4 sharcCam;   // xyz = the camera position of the grid levels, w = the scene scale
} P;

vec3 loadStyledPrev(uint l, ivec2 p) {
    if (l == 0u) return imageLoad(uHistSPrev0, p).rgb;
    if (l == 1u) return imageLoad(uHistSPrev1, p).rgb;
    return imageLoad(uHistSPrev2, p).rgb;
}
vec3 loadStyledCur(uint l, ivec2 p) {
    if (l == 0u) return imageLoad(uHistSCur0, p).rgb;
    if (l == 1u) return imageLoad(uHistSCur1, p).rgb;
    return imageLoad(uHistSCur2, p).rgb;
}
void storeStyled(uint l, ivec2 p, vec3 v) {
    if (l == 0u) imageStore(uHistSCur0, p, vec4(v, 0.0));
    else if (l == 1u) imageStore(uHistSCur1, p, vec4(v, 0.0));
    else imageStore(uHistSCur2, p, vec4(v, 0.0));
}
float styleWeight(uint layer) { return layer == 0u ? 1.0 : P.styleW[layer]; }

// The object records of render/rt_scene.h (RtRecord, 32 bytes = 2 uvec4 per instance slot = instanceCustomIndex): rec[2s] = (vertex address lo, hi, index address lo, hi),
// rec[2s + 1] = (triangle count, flags: bit 0 raw mesh, bit 1 the object receives baked light, vertex count, 0).
layout(set = RT_SET, binding = 21, std430) readonly buffer Records { uvec4 rec[]; };

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
