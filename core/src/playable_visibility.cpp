#include "core/playable_visibility.h"
#include "core/raytrace.h"
#include <algorithm>
#include <cmath>

namespace core {

struct PlayableVisibility::Impl {
    OccluderMesh mesh;
    std::unique_ptr<TriangleBvh> bvh;
    std::vector<Vec3> eyes;
};

PlayableVisibility::PlayableVisibility(const PlayableArea& area, const std::vector<LightSurface>& surfaces, const LightingClasses& classes, size_t maxViewpoints)
    : d_(new Impl) {
    d_->mesh = buildOccluders(surfaces, classes);
    if (!d_->mesh.empty()) d_->bvh = std::make_unique<TriangleBvh>(d_->mesh);
    if (area.empty()) return;
    // Viewpoints: a regular grid through the bounds of the area, the points inside it, thinned evenly to `maxViewpoints`.
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
    for (size_t i = 0; i < n; ++i) d_->eyes.push_back(inside[i * inside.size() / n]);
}

PlayableVisibility::~PlayableVisibility() = default;

size_t PlayableVisibility::viewpointCount() const { return d_->eyes.size(); }

bool PlayableVisibility::seen(const std::vector<Vec3>& poly, const Vec3& normal) const {
    if (!d_->bvh || d_->eyes.empty() || poly.size() < 3) return true;  // nothing blocks, or nothing to judge by: seen
    // sample points: the centre and the vertices pulled a little towards it
    Vec3 c{0, 0, 0};
    for (const Vec3& v : poly) c = c + v;
    c = c * (1.0 / static_cast<double>(poly.size()));
    std::vector<Vec3> pts{c};
    for (size_t i = 0; i < poly.size() && pts.size() < 9; ++i) pts.push_back(poly[i] + (c - poly[i]) * 0.1);
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
