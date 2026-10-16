#pragma once
// Test oracle: the straightforward texel rasteriser as it was before TexelSampleCache (every surface re-rasterised, nothing cached).
// tests/test_texel_cache.cpp requires the cached builder to return exactly what this returns.
#include "core/lightbake.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace texel_reference {
using namespace core;

// ---- rasterisation --------------------------------------------------------------------------------------------------------
// World distance a conservatively rasterised sample is pulled in from the fragment edge.
constexpr double kEdgeInset = 0.005;

struct Tri2 {
    double x[3], y[3];
};

// The fragment (and fan triangle) that claims a texel, for its sub-texel positions.
struct SubCtx {
    const LightSurface* surface = nullptr;
    const std::vector<Vec2>* t = nullptr;  // texel coordinates of the fragment's vertices
    size_t k = 0;                          // the fan triangle (0, k, k + 1) that claimed the texel
};

// 12 floats, layout in TexelSamples::sub. All zero (mask 0) when there is no triangle to extrapolate from.
inline void subInfo(const SubCtx* c, int x, int y, std::vector<float>& out) {
    float v[12] = {};
    if (c && c->surface && c->t) {
        const auto& verts = c->surface->verts;
        const auto& t = *c->t;
        const size_t k = c->k, n = verts.size();
        const Vec2 a = t[0], b = t[k], cc = t[k + 1];
        const double den = (b.v - cc.v) * (a.u - cc.u) + (cc.u - b.u) * (a.v - cc.v);
        if (std::fabs(den) > 1e-12) {
            // The texel -> world map is affine on the chart's plane, so the claiming triangle also extrapolates to texel centres outside it.
            auto world = [&](double px, double py) {
                const double w0 = ((b.v - cc.v) * (px - cc.u) + (cc.u - b.u) * (py - cc.v)) / den;
                const double w1 = ((cc.v - a.v) * (px - cc.u) + (a.u - cc.u) * (py - cc.v)) / den;
                return verts[0] * w0 + verts[k] * w1 + verts[k + 1] * (1.0 - w0 - w1);
            };
            const double cx = x + 0.5, cy = y + 0.5;
            const Vec3 C = world(cx, cy), U = world(cx + 1, cy) - C, V = world(cx, cy + 1) - C;
            const double texelSize = 0.5 * (U.length() + V.length());
            const double margin = texelSize > 1e-9 ? kEdgeInset / texelSize : 0.0;  // texels: keeps sub positions off the polygon's border
            // is the texel-space point (qx, qy) inside the polygon (union of its fan triangles), clear of its border?
            auto insideAt = [&](double qx, double qy) {
                for (size_t kk = 1; kk + 1 < n; ++kk) {
                    const Vec2 p[3] = {t[0], t[kk], t[kk + 1]};
                    const double area = (p[1].u - p[0].u) * (p[2].v - p[0].v) - (p[1].v - p[0].v) * (p[2].u - p[0].u);
                    if (std::fabs(area) < 1e-12) continue;
                    const double sign = area > 0 ? 1.0 : -1.0;
                    // polygon edges get the margin, the internal diagonal does not
                    const bool polyEdge[3] = {kk == 1, true, kk + 2 == n};
                    bool in = true;
                    for (int e = 0; e < 3 && in; ++e) {
                        const Vec2 e0 = p[e], e1 = p[(e + 1) % 3];
                        const double ex = e1.u - e0.u, ey = e1.v - e0.v, len = std::sqrt(ex * ex + ey * ey);
                        if (len < 1e-12) { in = false; break; }
                        const double d = sign * (ex * (qy - e0.v) - ey * (qx - e0.u)) / len;  // distance inside, in texels
                        in = d >= (polyEdge[e] ? margin : -1e-9);
                    }
                    if (in) return true;
                }
                return false;
            };
            uint32_t mask = 0;
            for (int j = 0; j < kMaxSuperSamples; ++j)
                if (insideAt(cx + kSuperOffsets[j][0], cy + kSuperOffsets[j][1])) mask |= 1u << j;
            // the same test for the cells of the 2x2 and 4x4 grids the global illumination is gathered on (giGridOffset)
            uint32_t grid2 = 0, grid4 = 0;
            for (int j = 0; j < 4; ++j)
                if (insideAt(cx + giGridOffset(2, j % 2), cy + giGridOffset(2, j / 2))) grid2 |= 1u << j;
            for (int j = 0; j < 16; ++j)
                if (insideAt(cx + giGridOffset(4, j % 4), cy + giGridOffset(4, j / 4))) grid4 |= 1u << j;
            v[0] = static_cast<float>(C.x); v[1] = static_cast<float>(C.y); v[2] = static_cast<float>(C.z);
            std::memcpy(&v[3], &mask, sizeof(mask));
            v[4] = static_cast<float>(U.x); v[5] = static_cast<float>(U.y); v[6] = static_cast<float>(U.z);
            std::memcpy(&v[7], &grid2, sizeof(grid2));
            v[8] = static_cast<float>(V.x); v[9] = static_cast<float>(V.y); v[10] = static_cast<float>(V.z);
            std::memcpy(&v[11], &grid4, sizeof(grid4));
        }
    }
    out.insert(out.end(), v, v + 12);
}


inline TexelSamples buildTexelSamplesReference(const LightmapLayout& layout, const std::vector<LightSurface>& surfaces, const SolidSet* solids, size_t* hiddenOut) {
    size_t hiddenCount = 0;
    TexelSamples out;
    out.atlas = layout.settings.atlasSize;
    out.pageCount = layout.pageCount;
    const int atlas = out.atlas;
    std::vector<std::vector<int32_t>> owner(static_cast<size_t>(layout.pageCount), std::vector<int32_t>(static_cast<size_t>(atlas) * atlas, -1));

    auto claim = [&](int page, int x, int y, const Vec3& p, const Vec3& n, const SubCtx* ctx) {
        if (x < 0 || y < 0 || x >= atlas || y >= atlas) return false;
        int32_t& o = owner[static_cast<size_t>(page)][static_cast<size_t>(y) * atlas + x];
        if (o != -1) return false;  // already a sample (>= 0) or already known to be hidden (-2)
        if (solids && solids->inside(p + n * 0.02)) { o = -2; ++hiddenCount; return false; }  // buried in a solid: invisible, counted once
        o = static_cast<int32_t>(out.texel.size());
        out.texel.push_back(static_cast<uint32_t>((static_cast<size_t>(page) * atlas + y) * atlas + x));
        out.pos.insert(out.pos.end(), {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z), 1.f});
        out.nrm.insert(out.nrm.end(), {static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z), 0.f});
        subInfo(ctx, x, y, out.sub);
        return true;
    };

    // Two passes. Pass 0 bakes every texel whose centre lies inside a fragment. Pass 1 (conservative rasterisation) bakes the texels that
    // only partly overlap a fragment, at the nearest point inside it. Without pass 1 the outermost texels of a chart stay empty, the
    // gutter (one ring) does not reach them, and bilinear filtering at a polygon edge reads black / unrelated texels (dark or bright
    // edges at corners, light leaking between charts that touch in the atlas). Centre-inside texels always win over conservative ones.
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& s : surfaces) {
            if (s.verts.size() < 3) continue;
            const LightmapFace* f = layout.findForFragment(s.instance, s.entityId, s.brushInstance, s.brushId, s.faceId, s.verts);
            if (!f || f->chart < 0) continue;
            const int page = layout.charts[static_cast<size_t>(f->chart)].page;
            std::vector<Vec2> t;
            Vec3 centroid;
            for (const auto& v : s.verts) { t.push_back(f->texel(v)); centroid = centroid + v; }
            centroid = centroid * (1.0 / static_cast<double>(s.verts.size()));
            bool any = false;
            for (size_t k = 1; k + 1 < s.verts.size(); ++k) {
                const Vec2 a = t[0], b = t[k], c = t[k + 1];
                const double den = (b.v - c.v) * (a.u - c.u) + (c.u - b.u) * (a.v - c.v);
                if (std::fabs(den) < 1e-12) continue;
                const int x0 = static_cast<int>(std::floor(std::min({a.u, b.u, c.u}) - 0.5)) - pass;
                const int x1 = static_cast<int>(std::ceil(std::max({a.u, b.u, c.u}) - 0.5)) + pass;
                const int y0 = static_cast<int>(std::floor(std::min({a.v, b.v, c.v}) - 0.5)) - pass;
                const int y1 = static_cast<int>(std::ceil(std::max({a.v, b.v, c.v}) - 0.5)) + pass;
                auto bary = [&](double px, double py, double& w0, double& w1, double& w2) {
                    w0 = ((b.v - c.v) * (px - c.u) + (c.u - b.u) * (py - c.v)) / den;
                    w1 = ((c.v - a.v) * (px - c.u) + (a.u - c.u) * (py - c.v)) / den;
                    w2 = 1.0 - w0 - w1;
                };
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x) {
                        const double cx = x + 0.5, cy = y + 0.5;
                        double w0, w1, w2;
                        bary(cx, cy, w0, w1, w2);
                        constexpr double kTol = -1e-9;
                        Vec3 p;
                        if (w0 >= kTol && w1 >= kTol && w2 >= kTol) {
                            if (pass != 0) continue;  // handled by pass 0
                            p = s.verts[0] * w0 + s.verts[k] * w1 + s.verts[k + 1] * w2;
                        } else {
                            if (pass != 1) continue;
                            // nearest point of the triangle to the texel centre; the texel is covered when it lies within the texel square
                            const Vec2 pts[3] = {a, b, c};
                            double best = 1e300, qx = cx, qy = cy;
                            for (int e = 0; e < 3; ++e) {
                                const Vec2 e0 = pts[e], e1 = pts[(e + 1) % 3];
                                const double dx = e1.u - e0.u, dy = e1.v - e0.v, len2 = dx * dx + dy * dy;
                                const double u = len2 > 1e-18 ? std::clamp(((cx - e0.u) * dx + (cy - e0.v) * dy) / len2, 0.0, 1.0) : 0.0;
                                const double ex = e0.u + dx * u, ey = e0.v + dy * u;
                                const double d2 = (ex - cx) * (ex - cx) + (ey - cy) * (ey - cy);
                                if (d2 < best) { best = d2; qx = ex; qy = ey; }
                            }
                            if (std::fabs(qx - cx) > 0.5 - 1e-6 || std::fabs(qy - cy) > 0.5 - 1e-6) continue;  // the texel square does not overlap the triangle (touching only is not enough)
                            bary(qx, qy, w0, w1, w2);
                            w0 = std::max(0.0, w0); w1 = std::max(0.0, w1); w2 = std::max(0.0, w2);
                            const double sum = w0 + w1 + w2;
                            if (sum < 1e-12) continue;
                            p = (s.verts[0] * w0 + s.verts[k] * w1 + s.verts[k + 1] * w2) * (1.0 / sum);
                            // pull the point off the edge so the shadow ray origin does not sit on a crease / coplanar neighbour
                            const Vec3 toC = centroid - p;
                            const double dl = toC.length();
                            if (dl > 1e-9) p = p + toC * (std::min(dl, kEdgeInset) / dl);
                        }
                        const SubCtx ctx{&s, &t, k};
                        if (claim(page, x, y, p, s.normal, &ctx)) any = true;
                        else if (owner[static_cast<size_t>(page)][static_cast<size_t>(y) * atlas + x] >= 0) any = true;
                    }
            }
            if (pass == 0 && !any) {  // tiny piece: take the texel under its centroid
                double u = 0, v = 0;
                for (size_t i = 0; i < s.verts.size(); ++i) { u += t[i].u; v += t[i].v; }
                const double inv = 1.0 / static_cast<double>(s.verts.size());
                claim(page, static_cast<int>(std::floor(u * inv)), static_cast<int>(std::floor(v * inv)), centroid, s.normal, nullptr);
            }
        }
    }
    if (hiddenOut) *hiddenOut = hiddenCount;
    return out;
}

}  // namespace texel_reference
