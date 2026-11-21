// R6: the SHaRC cache of the ray traced Lit path (third_party/sharc, NVIDIA RTX SDKs licence): constants shared by the update, resolve and query kernels.
// The cache stores S = Db + I of the bake (core/lightbake.cpp), demodulated: Db = the direct light of the surface weighted by each light's bounce factor, I = the cosine-gathered
// S of what the surface sees, times the hit's albedo, the bake's `albedo` and window(distance). The path of an update sample feeds it: radiance at a vertex = Db, the throughput of
// a segment = albedo(next hit) * albedo * window(length) (SharcSetThroughput), so the resolved radiance of a voxel is that vertex's S.
#ifndef SHARC_COMMON_GLSL
#define SHARC_COMMON_GLSL

#ifndef SHARC_UPDATE
#define SHARC_UPDATE 0
#endif
#define SHARC_RADIANCE_SCALE_DEFAULT 1000.0
#include "../third_party/sharc/include/SharcGlslHelpers.h"
#include "../third_party/sharc/include/SharcCommon.h"

#ifdef SHARC_FROM_PARAMS
// The glass shader has no room for the push constants: the Params hold the same values (rt_lit_common.glsl).
SharcParameters sharcParameters() {
    SharcParameters sp;
    sp.hashGridParameters.cameraPosition = P.sharcCam.xyz;
    sp.hashGridParameters.logarithmBase = SHARC_GRID_LOGARITHM_BASE;
    sp.hashGridParameters.sceneScale = P.sharcCam.w;
    sp.hashGridParameters.levelBias = float(SHARC_GRID_LEVEL_BIAS);
    sp.hashGridData.capacity = P.sharcB.z;
    sp.hashGridData.hashEntriesBuffer = RWStructuredBuffer_uint64_t(packUint2x32(P.sharcA.xy));
    sp.radianceScale = uintBitsToFloat(P.sharcB.w);
    sp.accumulationBuffer = RWStructuredBuffer_SharcAccumulationData(packUint2x32(P.sharcA.zw));
    sp.resolvedBuffer = RWStructuredBuffer_SharcPackedData(packUint2x32(P.sharcB.xy));
    return sp;
}
#else
// Push constants of the three kernels.
layout(push_constant) uniform SharcPush {
    uint64_t hashAddr, accumAddr, resolvedAddr;
    uint capacity;
    float radianceScale;
    uint accumFrames;
    uint staleMax;
    uint frame;
    vec4 cam;       // xyz = the camera position of the grid's levels, w = the scene scale (voxel size at distance 1)
    vec4 camPrev;   // xyz = the previous frame's
} G;

SharcParameters sharcParameters() {
    SharcParameters sp;
    sp.hashGridParameters.cameraPosition = G.cam.xyz;
    sp.hashGridParameters.logarithmBase = SHARC_GRID_LOGARITHM_BASE;
    sp.hashGridParameters.sceneScale = G.cam.w;
    sp.hashGridParameters.levelBias = float(SHARC_GRID_LEVEL_BIAS);
    sp.hashGridData.capacity = G.capacity;
    sp.hashGridData.hashEntriesBuffer = RWStructuredBuffer_uint64_t(G.hashAddr);
    sp.radianceScale = G.radianceScale;
    sp.accumulationBuffer = RWStructuredBuffer_SharcAccumulationData(G.accumAddr);
    sp.resolvedBuffer = RWStructuredBuffer_SharcPackedData(G.resolvedAddr);
    return sp;
}

#endif

#endif
