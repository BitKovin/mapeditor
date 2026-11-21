// R3: the direct light of the ray traced Lit path. The light model is the bake's (shaders/direct.comp: the same records from light/light_pack, the same evalLight maths for
// point / spot / directional / rect / sky lights, the same shadow rays and the same bias); what differs is how the shadow rays are spent. The bake gives every lightmap texel
// all lights with a fixed number of samples; here every pixel and frame gets ONE shadow ray per shadow casting light class:
//   * lights without a position (sun, sky): each has its own ray (a sample inside the sun's disc / a cosine weighted sky ray);
//   * local lights (point / spot / rect) of the light grid cell the point lies in: the unshadowed contributions are computed for all of them, one is picked with a probability
//     proportional to its luminance (resampled importance sampling with the candidates streamed through one reservoir), one ray tests it, and the estimate is divided by the
//     probability. Lights that do not cast shadows are added without a ray.
// Included by rt_ao.comp only (needs `tlas`, the pcg / rand01 helpers and the Params of rt_lit_common.glsl).

layout(set = RT_SET, binding = 16, std430) readonly buffer Lights { vec4 lt[]; };         // 8 x vec4 per light, then the sky tables
layout(set = RT_SET, binding = 17, std430) readonly buffer Cells { uvec2 cells[]; };      // (offset into the index list, count) per grid cell
layout(set = RT_SET, binding = 18, std430) readonly buffer LightIndex { uint lightIndex[]; };

const float MIN_DIST = 0.05;
const uint LREC = 8u;
const int SKY_LON = 32, SKY_LAT = 8, SKY_NLON = 16, SKY_NLAT = 9;

float window(float d, float r) {
    float q = d / r;
    float w = clamp(1.0 - q * q * q * q, 0.0, 1.0);
    return w * w;
}

// Fraction of the receiver's cosine-weighted hemisphere covered by the rectangle (Arvo), rectFormFactor() of direct.comp.
float rectFormFactor(uint k, vec3 p, vec3 n) {
    vec3 pos = lt[k * LREC + 0u].xyz;
    vec3 right = lt[k * LREC + 3u].xyz;
    vec3 up = lt[k * LREC + 4u].xyz;
    vec3 hr = right * (lt[k * LREC + 5u].x * 0.5);
    vec3 hu = up * (lt[k * LREC + 5u].y * 0.5);
    vec3 corner[4] = vec3[4](pos - hr - hu, pos + hr - hu, pos + hr + hu, pos - hr + hu);
    vec3 poly[8];
    int count = 0;
    for (int i = 0; i < 4; ++i) {
        vec3 a = corner[i] - p;
        vec3 b = corner[(i + 1) % 4] - p;
        float da = dot(n, a), db = dot(n, b);
        if (da >= 0.0) poly[count++] = a;
        if ((da >= 0.0) != (db >= 0.0)) poly[count++] = a + (b - a) * (da / (da - db));
    }
    if (count < 3) return 0.0;
    float sum = 0.0;
    for (int i = 0; i < count; ++i) {
        vec3 a = poly[i];
        vec3 b = poly[(i + 1) % count];
        float la = length(a), lb = length(b);
        if (la < 1e-12 || lb < 1e-12) continue;
        vec3 c = cross(a, b);
        float lc = length(c);
        if (lc < 1e-12) continue;
        float theta = acos(clamp(dot(a, b) / (la * lb), -1.0, 1.0));
        sum += theta * dot(c, n) / lc;
    }
    return abs(sum) / (2.0 * PI);
}

vec3 skyRadiance(uint k, vec3 dir) {
    if (dir.y <= 0.0) return vec3(0.0);
    uint base = uint(lt[k * LREC + 7u].x + 0.5);
    int i = clamp(int(floor((atan(dir.x, -dir.z) + PI) / (2.0 * PI) * float(SKY_LON))), 0, SKY_LON - 1);
    int j = clamp(int(floor(asin(clamp(dir.y / length(dir), -1.0, 1.0)) / (0.5 * PI) * float(SKY_LAT))), 0, SKY_LAT - 1);
    return lt[base + uint(j * SKY_LON + i)].rgb;
}
vec3 skyIrradiance(uint k, vec3 n) {
    uint base = uint(lt[k * LREC + 7u].x + 0.5) + uint(SKY_LON * SKY_LAT);
    float fu = (atan(n.x, -n.z) + PI) / (2.0 * PI) * float(SKY_NLON);
    float fv = (asin(clamp(n.y / length(n), -1.0, 1.0)) + 0.5 * PI) / PI * float(SKY_NLAT - 1);
    int i0 = int(floor(fu)), j0 = clamp(int(floor(fv)), 0, SKY_NLAT - 2);
    float a = fu - float(i0), b = clamp(fv - float(j0), 0.0, 1.0);
    int ia = ((i0 % SKY_NLON) + SKY_NLON) % SKY_NLON, ib = (ia + 1) % SKY_NLON;
    vec3 e00 = lt[base + uint(j0 * SKY_NLON + ia)].rgb, e10 = lt[base + uint(j0 * SKY_NLON + ib)].rgb;
    vec3 e01 = lt[base + uint((j0 + 1) * SKY_NLON + ia)].rgb, e11 = lt[base + uint((j0 + 1) * SKY_NLON + ib)].rgb;
    return mix(mix(e00, e10, a), mix(e01, e11, a), b);
}

// Unshadowed irradiance (pi absorbed) from light k.
vec3 evalLight(uint k, vec3 p, vec3 n) {
    vec4 a = lt[k * LREC + 0u];
    vec4 b = lt[k * LREC + 1u];
    int type = int(b.w + 0.5);
    if (type == 0 || type == 1) {
        vec3 d = a.xyz - p;
        float dist = length(d);
        if (dist >= a.w) return vec3(0.0);
        vec3 l = dist > 1e-9 ? d / dist : n;
        float cosT = max(0.0, dot(n, l));
        float dd = max(dist, MIN_DIST);
        float s = window(dist, a.w) / (dd * dd) * cosT;
        if (type == 1) {
            vec4 f = lt[k * LREC + 2u];
            float cosOuter = lt[k * LREC + 3u].w;
            float t = clamp((dot(f.xyz, -l) - cosOuter) / (f.w - cosOuter), 0.0, 1.0);
            s *= t * t;
        }
        return b.rgb * s;
    }
    if (type == 2) return b.rgb * max(0.0, dot(n, -lt[k * LREC + 2u].xyz));
    if (type == 3) {
        vec3 d = a.xyz - p;
        float dist = length(d);
        if (dist >= a.w || dot(p - a.xyz, lt[k * LREC + 2u].xyz) <= 0.0) return vec3(0.0);
        return b.rgb * (rectFormFactor(k, p, n) * window(dist, a.w));
    }
    if (type == 4) return lt[k * LREC + 7u].y > 0.5 ? b.rgb * skyIrradiance(k, n) : b.rgb * ((1.0 + n.y) * 0.5);
    return vec3(0.0);
}

void frameOf(vec3 n, out vec3 t, out vec3 b) {
    t = abs(n.y) < 0.99 ? normalize(cross(n, vec3(0.0, 1.0, 0.0))) : vec3(1.0, 0.0, 0.0);
    b = cross(n, t);
}

#ifndef TEX_SET
#define TEX_SET 2
#endif
layout(set = TEX_SET, binding = 0) uniform sampler2D textures[];   // the texture table of the renderer (global texture indices are what the vertices carry)
layout(set = TEX_SET, binding = 1, std430) readonly buffer TexMeta { vec4 texMeta[]; };   // x mode (0 opaque, 1 cutout, 2 translucent), y cutoff, z opacity, w 1 + tint (0 = not loaded)

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer WordBuf { uint w[]; };

// The material of a candidate triangle (an instance whose texture has a cutout or translucent mode is not forced opaque, so the shadow rays see its triangles as candidates) the way the
// Core's compiled bake sees it (raytrace.cpp surfaceAt): 0 = a surface that blocks (opaque, or a cutout texel that is solid), -1 = no surface here (a cutout hole, a clear texel of
// a translucent texture), 1 = a translucent surface: `tm` is what passes (the texel colour mixed with white by the tint, times 1 - opacity * alpha), counted once where the ray enters
// (front side), not where it leaves a closed brush.
int candidateSurface(rayQueryEXT rq, vec3 dir, out vec3 tm) {
    tm = vec3(1.0);
    uint slot = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false));
    uvec4 r0 = rec[slot * 2u];
    WordBuf vb = WordBuf(packUint2x32(uvec2(r0.x, r0.y)));
    WordBuf ib = WordBuf(packUint2x32(uvec2(r0.z, r0.w)));
    uint prim = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false));
    uvec3 idx = uvec3(ib.w[prim * 3u], ib.w[prim * 3u + 1u], ib.w[prim * 3u + 2u]);
    uint o0 = idx.x * 10u, o1 = idx.y * 10u, o2 = idx.z * 10u;
    uint tex = vb.w[o0 + 9u];
    vec4 m = texMeta[tex];
    if (m.x < 0.5) return 0;
    vec2 bc = rayQueryGetIntersectionBarycentricsEXT(rq, false);
    vec3 w3 = vec3(1.0 - bc.x - bc.y, bc.x, bc.y);
    vec2 uv = vec2(uintBitsToFloat(vb.w[o0 + 4u]), uintBitsToFloat(vb.w[o0 + 5u])) * w3.x + vec2(uintBitsToFloat(vb.w[o1 + 4u]), uintBitsToFloat(vb.w[o1 + 5u])) * w3.y +
                vec2(uintBitsToFloat(vb.w[o2 + 4u]), uintBitsToFloat(vb.w[o2 + 5u])) * w3.z;
    ivec2 sz = textureSize(textures[nonuniformEXT(tex)], 0);
    ivec2 px = min(sz - 1, ivec2(fract(uv) * vec2(sz)));   // the texel the Core picks (no filtering)
    vec4 t = texelFetch(textures[nonuniformEXT(tex)], px, 0);
    if (m.x < 1.5) return t.a < m.y ? -1 : 0;
    if (t.a * 255.0 < 16.0) return -1;
    vec3 p0 = vec3(uintBitsToFloat(vb.w[o0]), uintBitsToFloat(vb.w[o0 + 1u]), uintBitsToFloat(vb.w[o0 + 2u]));
    vec3 p1 = vec3(uintBitsToFloat(vb.w[o1]), uintBitsToFloat(vb.w[o1 + 1u]), uintBitsToFloat(vb.w[o1 + 2u]));
    vec3 p2 = vec3(uintBitsToFloat(vb.w[o2]), uintBitsToFloat(vb.w[o2 + 1u]), uintBitsToFloat(vb.w[o2 + 2u]));
    if (dot(cross(p1 - p0, p2 - p0), dir) >= 0.0) return 1;   // leaving the brush: no further dimming
    float tint = m.w >= 1.0 ? m.w - 1.0 : 1.0;
    tm = (vec3(1.0) + (t.rgb - vec3(1.0)) * tint) * (1.0 - m.z * t.a);
    return 1;
}

// What of the light passes along the segment o + t * d, t in [0, tmax], per colour channel: 1 = nothing in the way, 0 = a surface that blocks, in between through translucent surfaces
// (the Core's TriangleBvh::transmittance); cutout holes do not block.
vec3 transmit(vec3 o, vec3 d, float tmax) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, tlas, gl_RayFlagsTerminateOnFirstHitEXT, RT_MASK_SOLID, o, 0.0, d, tmax);
    vec3 T = vec3(1.0);
    while (rayQueryProceedEXT(rq)) {
        if (rayQueryGetIntersectionTypeEXT(rq, false) != gl_RayQueryCandidateIntersectionTriangleEXT) continue;
        vec3 tm;
        int s = candidateSurface(rq, d, tm);
        if (s == 0) {
            rayQueryConfirmIntersectionEXT(rq);
        } else if (s > 0) {
            T *= tm;
            if (max(T.x, max(T.y, T.z)) < 1e-3) {
                rayQueryTerminateEXT(rq);
                return vec3(0.0);
            }
        }
    }
    return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT ? vec3(0.0) : T;
}

vec3 blocked(vec3 o, vec3 target) {
    vec3 d = target - o;
    float dist = length(d);
    if (dist <= P.look.y) return vec3(1.0);
    return transmit(o, d / dist, dist - P.look.y);
}

// One sample of the light's visibility from o (per colour channel: translucent surfaces tint the light): a point of the light's source (disc for point / spot, rectangle, the sun's
// disc), one shadow ray.
vec3 visibilitySample(uint k, vec3 o, inout uint seed) {
    vec4 a = lt[k * LREC + 0u];
    int type = int(lt[k * LREC + 1u].w + 0.5);
    float size = lt[k * LREC + 4u].w;
    if (type == 0 || type == 1) {
        if (size <= 0.0) return blocked(o, a.xyz);
        vec3 l = normalize(a.xyz - o);
        vec3 t, b;
        frameOf(l, t, b);
        float u1 = rand01(seed), u2 = rand01(seed);
        float phi = 2.0 * PI * u2, r = size * sqrt(u1);
        return blocked(o, a.xyz + t * (cos(phi) * r) + b * (sin(phi) * r));
    }
    if (type == 2) {
        vec3 toLight = -lt[k * LREC + 2u].xyz;
        if (size <= 0.0) return transmit(o, toLight, P.look.z);
        vec3 t, b;
        frameOf(toLight, t, b);
        float u1 = rand01(seed), u2 = rand01(seed);
        float phi = 2.0 * PI * u2, r = tan(radians(size * 0.5)) * sqrt(u1);
        return transmit(o, normalize(toLight + t * (cos(phi) * r) + b * (sin(phi) * r)), P.look.z);
    }
    if (type == 3) {
        vec3 right = lt[k * LREC + 3u].xyz;
        vec3 up = lt[k * LREC + 4u].xyz;
        float w = lt[k * LREC + 5u].x, h = lt[k * LREC + 5u].y;
        float u1 = rand01(seed), u2 = rand01(seed);
        return blocked(o, a.xyz + right * ((u1 - 0.5) * w) + up * ((u2 - 0.5) * h));
    }
    return vec3(1.0);
}

// One sample of the sky light with occlusion: a cosine weighted ray; only an upward ray that escapes counts, weighted by the skybox's light from its direction.
vec3 skySample(uint k, vec3 o, vec3 n, inout uint seed) {
    vec3 t, b;
    frameOf(n, t, b);
    float u1 = rand01(seed), u2 = rand01(seed);
    float r = sqrt(u1), phi = 2.0 * PI * u2;
    vec3 dir = t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u1));
    if (dir.y <= 0.0) return vec3(0.0);
    return transmit(o, dir, P.look.z) * (lt[k * LREC + 7u].y > 0.5 ? skyRadiance(k, dir) : vec3(1.0));
}

float lum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// The bake's bounce source weight of a light at p (core/lightbake.cpp bounceNearFactor): the light's `bounce` power, a point / spot light stops growing closer than 1 m.
float bounceWeight(uint k, vec3 p) {
    float w = lt[k * LREC + 6u].x;
    int type = int(lt[k * LREC + 1u].w + 0.5);
    if (type == 0 || type == 1) {
        float dist = length(lt[k * LREC + 0u].xyz - p);
        float dd = max(dist, MIN_DIST), db = max(dist, 1.0);
        w *= (dd * dd) / (db * db);
    }
    return w;
}

// The direct light (sun, sky, local lights of the style layer `layer`) arriving at p (normal n): one unbiased sample of what the bake computes per lightmap texel. `bounce`: every
// light weighted by its bounce source weight (what the bake feeds into the bounces: Db). The layer's lights without the style value (the compose pass applies it).
vec3 directSampleL(vec3 p, vec3 n, inout uint seed, bool bounce, uint layer) {
    if (P.lightInfo.z == 0u) return vec3(0.0);
    vec3 o = p + n * P.look.y;
    vec3 sum = vec3(0.0);
    for (uint g = 0u; g < P.layerCnt[layer]; ++g) {
        uint k = lightIndex[P.layerOff[layer] + g];
        int type = int(lt[k * LREC + 1u].w + 0.5);
        bool casts = lt[k * LREC + 5u].z > 0.5;
        float bw = bounce ? bounceWeight(k, p) : 1.0;
        if (bw == 0.0) continue;
        if (type == 4) {
            sum += bw * (casts ? lt[k * LREC + 1u].rgb * skySample(k, o, n, seed) : evalLight(k, p, n));
        } else {
            vec3 v = evalLight(k, p, n);
            if (casts && (v.x != 0.0 || v.y != 0.0 || v.z != 0.0)) v *= visibilitySample(k, o, seed);
            sum += v * bw;
        }
    }
    // the local lights of the grid cell
    vec3 rel = (p - P.gridOrigin.xyz) / P.gridOrigin.w;
    ivec3 c = ivec3(floor(rel));
    if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, ivec3(P.gridDim.xyz)))) return sum;
    uvec2 cell = cells[layer * P.layerInfo.y + (uint(c.z) * P.gridDim.y + uint(c.y)) * P.gridDim.x + uint(c.x)];
    float wSum = 0.0, wSel = 0.0;
    uint sel = 0xFFFFFFFFu;
    vec3 cSel = vec3(0.0);
    for (uint i = 0u; i < cell.y; ++i) {
        uint k = lightIndex[cell.x + i];
        vec3 v = evalLight(k, p, n);
        if (bounce) v *= bounceWeight(k, p);
        if (v.x == 0.0 && v.y == 0.0 && v.z == 0.0) continue;
        if (lt[k * LREC + 5u].z < 0.5) {   // no shadow: exact
            sum += v;
            continue;
        }
        float w = abs(v.x) + abs(v.y) + abs(v.z);   // a dark light (negative brightness) is shadowed too
        wSum += w;
        if (rand01(seed) * wSum < w) {
            sel = k;
            wSel = w;
            cSel = v;
        }
    }
    if (sel != 0xFFFFFFFFu) sum += cSel * (visibilitySample(sel, o, seed) * wSum / wSel);
    return sum;
}

vec3 directSampleW(vec3 p, vec3 n, inout uint seed, bool bounce) { return directSampleL(p, n, seed, bounce, 0u); }
vec3 directSample(vec3 p, vec3 n, inout uint seed) { return directSampleL(p, n, seed, false, 0u); }
