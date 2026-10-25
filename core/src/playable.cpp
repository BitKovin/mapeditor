#include "core/playable.h"
#include "core/bsp_compile.h"
#include "core/entity_class.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace core {

int PlayableLod::cascade(double d) const {
    if (levels <= 0 || d <= radius) return 0;
    const double r = std::max(1.0001, range);
    const int k = static_cast<int>(std::ceil(std::log(d / std::max(1e-6, radius)) / std::log(r) - 1e-9));
    return std::clamp(k, 0, levels);
}

double PlayableLod::densityAt(double d) const {
    const int k = cascade(d);
    return k == 0 ? 1.0 : std::pow(std::clamp(density, 0.05, 1.0), k);
}

void PlayableArea::add(const Brush& b) {
    for (const ConvexPiece& cp : convexPieces(b)) {
        Piece p;
        p.mn = {1e300, 1e300, 1e300};
        p.mx = {-1e300, -1e300, -1e300};
        for (const auto& w : convexWindings(cp.sides))
            for (const Vec3& v : w) {
                p.mn = {std::min(p.mn.x, v.x), std::min(p.mn.y, v.y), std::min(p.mn.z, v.z)};
                p.mx = {std::max(p.mx.x, v.x), std::max(p.mx.y, v.y), std::max(p.mx.z, v.z)};
            }
        for (const HalfSpace& h : cp.sides) p.planes.push_back({h.normal, h.dist});
        if (p.planes.size() >= 4 && p.mn.x <= p.mx.x) pieces_.push_back(std::move(p));
    }
}

void PlayableArea::finish() {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](double d) {
        uint64_t u;
        std::memcpy(&u, &d, sizeof u);
        h = (h ^ u) * 1099511628211ull;
    };
    mn_ = {1e300, 1e300, 1e300};
    mx_ = {-1e300, -1e300, -1e300};
    for (const Piece& p : pieces_) {
        mn_ = {std::min(mn_.x, p.mn.x), std::min(mn_.y, p.mn.y), std::min(mn_.z, p.mn.z)};
        mx_ = {std::max(mx_.x, p.mx.x), std::max(mx_.y, p.mx.y), std::max(mx_.z, p.mx.z)};
        for (const auto& pl : p.planes) for (double d : {pl.first.x, pl.first.y, pl.first.z, pl.second}) mix(d);
    }
    fp_ = pieces_.empty() ? 0 : h;
}

double PlayableArea::distance(const Vec3& p, double maxDist) const {
    double best = maxDist;
    for (const Piece& pc : pieces_) {
        // the distance to the piece's box is a cheap lower bound of the distance to the piece: skip pieces that cannot improve the answer
        const double dx = std::max({pc.mn.x - p.x, 0.0, p.x - pc.mx.x});
        const double dy = std::max({pc.mn.y - p.y, 0.0, p.y - pc.mx.y});
        const double dz = std::max({pc.mn.z - p.z, 0.0, p.z - pc.mx.z});
        const double box = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (box >= best) continue;
        double worst = 0;  // the largest violated plane: a lower bound of the distance to a convex region
        for (const auto& pl : pc.planes) worst = std::max(worst, pl.first.dot(p) - pl.second);
        worst = std::max(worst, box);
        best = std::min(best, worst);
        if (best <= 0) return 0;
    }
    return best;
}

double PlayableArea::distance(const std::vector<Vec3>& poly, double maxDist) const {
    if (poly.size() < 3 || pieces_.empty()) return maxDist;
    double best = maxDist;
    // whole-polygon reject: its box against the area's box
    Vec3 mn = poly[0], mx = poly[0];
    for (const Vec3& v : poly) {
        mn = {std::min(mn.x, v.x), std::min(mn.y, v.y), std::min(mn.z, v.z)};
        mx = {std::max(mx.x, v.x), std::max(mx.y, v.y), std::max(mx.z, v.z)};
    }
    const double bx = std::max({mn_.x - mx.x, 0.0, mn.x - mx_.x});
    const double by = std::max({mn_.y - mx.y, 0.0, mn.y - mx_.y});
    const double bz = std::max({mn_.z - mx.z, 0.0, mn.z - mx_.z});
    if (std::sqrt(bx * bx + by * by + bz * bz) >= best) return best;
    // fan triangles (polygon centre, edge): barycentric sample grid, about 2 m apart (8 steps at most per side)
    Vec3 c{0, 0, 0};
    for (const Vec3& v : poly) c = c + v;
    c = c * (1.0 / static_cast<double>(poly.size()));
    best = std::min(best, distance(c, best));
    for (size_t i = 0; i < poly.size() && best > 0; ++i) {
        const Vec3 &a = poly[i], &b = poly[(i + 1) % poly.size()];
        const double longest = std::max({(a - b).length(), (a - c).length(), (b - c).length()});
        const int n = std::clamp(static_cast<int>(std::ceil(longest / 2.0)), 1, 8);
        for (int u = 0; u <= n && best > 0; ++u)
            for (int v = 0; u + v <= n; ++v) {
                const double wa = static_cast<double>(u) / n, wb = static_cast<double>(v) / n, wc = 1.0 - wa - wb;
                best = std::min(best, distance(a * wa + b * wb + c * wc, best));
            }
    }
    return best;
}

std::shared_ptr<const PlayableArea> collectPlayableArea(const Document& doc, const std::vector<ResolvedEntity>& prefabs) {
    auto area = std::make_shared<PlayableArea>();
    auto visit = [&](const Entity& e) {
        if (!EntityClassRegistry::isPlayableArea(e.classname)) return;
        for (const Brush& b : e.brushes) if (b.op != BrushOp::Subtract) area->add(b);
    };
    for (const Entity& e : doc.entities) visit(e);
    for (const ResolvedEntity& r : prefabs) visit(r.entity);
    area->finish();
    return area;
}

}  // namespace core
