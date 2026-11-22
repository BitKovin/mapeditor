// tools/measure -- see measure.h.
#include "tools/measure.h"

#include <cmath>
#include <cstdio>

namespace native::tools {

Measurement measure(const core::Vec3& a, const core::Vec3& b) {
    Measurement m;
    m.dx = b.x - a.x;
    m.dy = b.y - a.y;
    m.dz = b.z - a.z;
    m.distance = std::sqrt(m.dx * m.dx + m.dy * m.dy + m.dz * m.dz);
    m.ground = std::sqrt(m.dx * m.dx + m.dz * m.dz);
    return m;
}

std::string measureText(const Measurement& m) {
    char buf[200];
    // -0.00 would read oddly: a tiny negative delta is shown as 0.00
    const auto z = [](double v) { return std::fabs(v) < 0.005 ? 0.0 : v; };
    std::snprintf(buf, sizeof buf, "%.2f m   dX %.2f   dY %.2f (up)   dZ %.2f   ground %.2f m", m.distance, z(m.dx), z(m.dy), z(m.dz), m.ground);
    return buf;
}

} // namespace native::tools
