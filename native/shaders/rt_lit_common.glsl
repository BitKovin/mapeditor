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
} P;

const uint F_RESET = 1u, F_STILL = 2u, F_FILTER = 4u, F_RR = 8u;

bool hasSurface(ivec2 p) { return texelFetch(uNormal, p, 0).w > 0.5; }

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
