#include "core/playable_visibility.h"
#include "core/raytrace.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace core {

namespace {

// Destructive visibility culling must balance coverage and cost. The old test used only the centre and at most
// eight points near the vertices; a visible region between those points could be missed. Sample interiors on a
// regular 0.5 m grid, widening the spacing for large polygons so large, fully hidden faces can still be classified.
// Degenerate polygons are treated as uncertain and the caller keeps them.
constexpr double kVisibilitySampleSpacing = 0.25;
constexpr size_t kMaxVisibilitySamples = 192;

struct Point2 {
    double x = 0, y = 0;
};

double cross2(const Point2& a, const Point2& b, const Point2& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool visibilitySamplePoints(const std::vector<Vec3>& poly, std::vector<Vec3>& pts) {
    if (poly.size() < 3) return false;

    const Vec3 origin = poly[0];
    Vec3 planeNormal;
    for (size_t i = 1; i + 1 < poly.size() && planeNormal.length() < 1e-9; ++i)
        planeNormal = (poly[i] - origin).cross(poly[i + 1] - origin).normalized();
    if (planeNormal.length() < 0.5) return false;

    Vec3 u;
    for (size_t i = 1; i < poly.size() && u.length() < 0.5; ++i) u = (poly[i] - origin).normalized();
    if (u.length() < 0.5) return false;
    const Vec3 v = planeNormal.cross(u).normalized();
    if (v.length() < 0.5) return false;

    std::vector<Point2> p2;
    p2.reserve(poly.size());
    double minU = std::numeric_limits<double>::infinity(), maxU = -minU;
    double minV = minU, maxV = -minU;
    for (const Vec3& p : poly) {
        const Vec3 d = p - origin;
        const Point2 q{d.dot(u), d.dot(v)};
        p2.push_back(q);
        minU = std::min(minU, q.x); maxU = std::max(maxU, q.x);
        minV = std::min(minV, q.y); maxV = std::max(maxV, q.y);
    }

    double twiceArea = 0;
    for (size_t i = 0; i < p2.size(); ++i) {
        const Point2& a = p2[i];
        const Point2& b = p2[(i + 1) % p2.size()];
        twiceArea += a.x * b.y - b.x * a.y;
    }
    if (std::fabs(twiceArea) < 1e-8) return false;

    // Use an even-odd point-in-polygon test below rather than assuming the face outline is convex. Large, valid
    // concave authored faces must still be eligible for culling; treating them as "uncertain/visible" kept them all.

    const size_t seedCount = 1 + std::min<size_t>(poly.size(), 8);
    const size_t maxGridSamples = kMaxVisibilitySamples > seedCount ? kMaxVisibilitySamples - seedCount : 1;
    // Keep sampling large faces instead of treating their size as proof that they are visible. Increase the grid
    // spacing only as much as needed to fit the budget; small polygons retain the original 0.5 m coverage.
    double spacing = kVisibilitySampleSpacing;
    int stepsU = 1, stepsV = 1;
    size_t gridCount = 0;
    bool gridFitsBudget = false;
    for (int attempt = 0; attempt < 64; ++attempt) {
        const double su = std::max(1.0, std::ceil((maxU - minU) / spacing));
        const double sv = std::max(1.0, std::ceil((maxV - minV) / spacing));
        // Avoid integer overflow on malformed or extremely large coordinates. A larger spacing is tried below.
        if (su > 1000000.0 || sv > 1000000.0) {
            spacing *= 2.0;
            continue;
        }
        stepsU = static_cast<int>(su);
        stepsV = static_cast<int>(sv);
        gridCount = static_cast<size_t>(stepsU + 1) * static_cast<size_t>(stepsV + 1);
        if (gridCount <= maxGridSamples) { gridFitsBudget = true; break; }
        const double ratio = std::sqrt(static_cast<double>(gridCount) / static_cast<double>(maxGridSamples));
        spacing *= std::max(1.05, ratio * 1.01);
    }
    if (!gridFitsBudget) {
        // Still sample instead of declaring the face visible solely because it is huge. This fallback gives
        // approximately square coverage with no more than the point budget.
        const double extentU = std::max(1e-9, maxU - minU);
        const double extentV = std::max(1e-9, maxV - minV);
        const double cells = std::sqrt(static_cast<double>(maxGridSamples));
        stepsU = std::max(1, static_cast<int>(std::min(1000000.0, std::ceil(cells * extentU / std::max(extentU, extentV)))));
        stepsV = std::max(1, static_cast<int>(std::min(1000000.0, std::ceil(cells * extentV / std::max(extentU, extentV)))));
        while (static_cast<size_t>(stepsU + 1) * static_cast<size_t>(stepsV + 1) > maxGridSamples) {
            if (stepsU >= stepsV && stepsU > 1) --stepsU;
            else if (stepsV > 1) --stepsV;
            else break;
        }
        gridCount = static_cast<size_t>(stepsU + 1) * static_cast<size_t>(stepsV + 1);
    }

    Vec3 center{0, 0, 0};
    Point2 center2{};
    for (size_t i = 0; i < poly.size(); ++i) {
        center = center + poly[i];
        center2.x += p2[i].x;
        center2.y += p2[i].y;
    }
    center = center * (1.0 / static_cast<double>(poly.size()));
    center2.x /= static_cast<double>(poly.size());
    center2.y /= static_cast<double>(poly.size());

    auto inPolygon = [&](const Point2& q) {
        bool inside = false;
        for (size_t i = 0, j = p2.size() - 1; i < p2.size(); j = i++) {
            const Point2& a = p2[j];
            const Point2& b = p2[i];
            const double cross = cross2(a, b, q);
            if (std::fabs(cross) <= 1e-7 && q.x >= std::min(a.x, b.x) - 1e-7 && q.x <= std::max(a.x, b.x) + 1e-7 &&
                q.y >= std::min(a.y, b.y) - 1e-7 && q.y <= std::max(a.y, b.y) + 1e-7) return true;  // boundary
            if ((a.y > q.y) != (b.y > q.y)) {
                const double xCross = a.x + (q.y - a.y) * (b.x - a.x) / (b.y - a.y);
                if (q.x < xCross) inside = !inside;
            }
        }
        return inside;
    };

    auto addUnique = [&](const Vec3& p) {
        for (const Vec3& q : pts) if ((p - q).length() < 1e-5) return;
        pts.push_back(p);
    };
    pts.clear();
    pts.reserve(std::min(kMaxVisibilitySamples, gridCount + seedCount));
    if (inPolygon(center2)) addUnique(center);
    // Keep near-corner samples where their inward-shifted locations are actually inside a concave outline.
    for (size_t i = 0; i < poly.size() && pts.size() < 9; ++i) {
        const Point2 q{p2[i].x + (center2.x - p2[i].x) * 0.1, p2[i].y + (center2.y - p2[i].y) * 0.1};
        if (inPolygon(q)) addUnique(poly[i] + (center - poly[i]) * 0.1);
    }
    for (int j = 0; j <= stepsV; ++j) {
        const double y = minV + (maxV - minV) * (static_cast<double>(j) / stepsV);
        for (int i = 0; i <= stepsU; ++i) {
            const double x = minU + (maxU - minU) * (static_cast<double>(i) / stepsU);
            const Point2 q{x, y};
            if (inPolygon(q)) addUnique(origin + u * x + v * y);
        }
    }

    // When a large face had to use a coarser grid, supplement its regular samples with a deterministic low-discrepancy
    // set. This breaks up grid-alignment artefacts (a narrow visible patch falling between every row/column) while
    // keeping the total number of points bounded. Small faces already get 0.25 m coverage and need no sparse fill.
    if (spacing > kVisibilitySampleSpacing * 1.01) {
        auto radicalInverse = [](size_t index, size_t base) {
            double f = 1.0 / static_cast<double>(base), value = 0.0;
            while (index) {
                value += f * static_cast<double>(index % base);
                index /= base;
                f /= static_cast<double>(base);
            }
            return value;
        };
        for (size_t i = 1; pts.size() < kMaxVisibilitySamples && i < kMaxVisibilitySamples * 16; ++i) {
            const double x = minU + (maxU - minU) * radicalInverse(i, 2);
            const double y = minV + (maxV - minV) * radicalInverse(i, 3);
            const Point2 q{x, y};
            if (inPolygon(q)) addUnique(origin + u * x + v * y);
        }
    }
    return !pts.empty();
}

}  // namespace

struct PlayableVisibility::Impl {
    OccluderMesh mesh;
    std::unique_ptr<TriangleBvh> bvh;
    std::vector<Vec3> eyes;
};

PlayableVisibility::PlayableVisibility(const PlayableArea& area, const std::vector<LightSurface>& surfaces, const LightingClasses& classes, size_t maxViewpoints,
                                     std::function<bool(const LightSurface&)> blocks)
    : d_(new Impl) {
    if (blocks) {
        std::vector<LightSurface> occ;
        for (const LightSurface& s : surfaces) {
            if (!blocks(s)) continue;
            occ.push_back(s);
            occ.back().classname = "worldspawn";
        }
        LightingClasses only;
        only.castShadows = {"worldspawn"};
        d_->mesh = buildOccluders(occ, only);
    } else {
        d_->mesh = buildOccluders(surfaces, classes);
    }
    if (!d_->mesh.empty()) d_->bvh = std::make_unique<TriangleBvh>(d_->mesh);
    if (area.empty()) return;
    // Candidate viewpoints: a regular grid through the bounds of the area, filtered to points inside it and then
    // spatially thinned to `maxViewpoints` when needed.
    const Vec3 mn = area.mn(), mx = area.mx();
    const Vec3 ext = mx - mn;
    double step = 2.0;
    auto count = [&](double s) { return (std::floor(ext.x / s) + 1) * (std::floor(ext.y / s) + 1) * (std::floor(ext.z / s) + 1); };
    while (count(step) > 20000) step *= 1.5;
    std::vector<Vec3> inside;
    for (double x = mn.x + step * 0.5; x <= mx.x; x += step)
        for (double y = mn.y + step * 0.5; y <= mx.y; y += step)
            for (double z = mn.z + step * 0.5; z <= mx.z; z += step) {
                const Vec3 p{x, y, z};
                if (area.distance(p) <= 0) inside.push_back(p);
            }
    if (inside.empty()) inside.push_back((mn + mx) * 0.5);  // a volume thinner than the grid step
    const size_t n = std::min(inside.size(), std::max<size_t>(1, maxViewpoints));
    if (n == inside.size()) {
        d_->eyes = std::move(inside);
    } else {
        // The old evenly-spaced flat-array stride was not spatially even: because z was the innermost loop,
        // it could leave large gaps in x/y and miss narrow sight lines to small details. Farthest-point sampling
        // spreads the limited viewpoints over the entire playable volume, including separated sub-volumes.
        const Vec3 center = (mn + mx) * 0.5;
        size_t first = 0;
        double bestCenterDist = std::numeric_limits<double>::infinity();
        for (size_t i = 0; i < inside.size(); ++i) {
            const Vec3 d = inside[i] - center;
            const double dist2 = d.dot(d);
            if (dist2 < bestCenterDist) { bestCenterDist = dist2; first = i; }
        }
        d_->eyes.reserve(n);
        d_->eyes.push_back(inside[first]);
        std::vector<double> nearest2(inside.size(), std::numeric_limits<double>::infinity());
        std::vector<char> selected(inside.size(), 0);
        selected[first] = 1;
        for (size_t k = 1; k < n; ++k) {
            const Vec3& last = d_->eyes.back();
            size_t farthest = inside.size();
            double farthestDist2 = -1.0;
            for (size_t i = 0; i < inside.size(); ++i) {
                if (selected[i]) continue;
                const Vec3 delta = inside[i] - last;
                nearest2[i] = std::min(nearest2[i], delta.dot(delta));
                if (nearest2[i] > farthestDist2) { farthestDist2 = nearest2[i]; farthest = i; }
            }
            if (farthest == inside.size()) break;
            selected[farthest] = 1;
            d_->eyes.push_back(inside[farthest]);
        }
    }
}

PlayableVisibility::~PlayableVisibility() = default;

size_t PlayableVisibility::viewpointCount() const { return d_->eyes.size(); }

bool PlayableVisibility::seen(const std::vector<Vec3>& poly, const Vec3& normal) const {
    if (!d_->bvh || d_->eyes.empty() || poly.size() < 3) return true;  // nothing blocks, or nothing to judge by: seen
    std::vector<Vec3> pts;
    // A degenerate polygon is uncertain. Valid large faces are still sampled at a coarser spacing, rather than
    // automatically being declared visible just because a fine grid would exceed the budget.
    if (!visibilitySamplePoints(poly, pts)) return true;
    for (const Vec3& p : pts)
        for (const Vec3& eye : d_->eyes) {
            const Vec3 to = eye - p;
            if (to.length() < 1e-6) return true;
            const double side = normal.dot(to) >= 0 ? 1.0 : -1.0;  // lift off whichever side faces the eye (the face itself is in the mesh)
            const Vec3 from = p + normal * (0.03 * side);
            if (!d_->bvh->occluded(from, eye - from, 0.999)) return true;
        }
    return false;
}

}  // namespace core
