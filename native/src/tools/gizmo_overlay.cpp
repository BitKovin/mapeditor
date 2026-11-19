// tools/gizmo_overlay -- see gizmo_overlay.h.
#include "tools/gizmo_overlay.h"

namespace native::tools {

namespace {

using render::LineVertex;
using render::packColor;

struct Rgb {
    float r, g, b;
};
constexpr Rgb kAxisColor[3] = {{1.0f, 0.3f, 0.3f}, {0.35f, 0.95f, 0.35f}, {0.4f, 0.55f, 1.0f}};
constexpr Rgb kActive = {1.0f, 1.0f, 0.2f};

void line(std::vector<LineVertex>& out, const Vec3& a, const Vec3& b, uint32_t c) {
    out.push_back({{static_cast<float>(a.x), static_cast<float>(a.y), static_cast<float>(a.z)}, c});
    out.push_back({{static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(b.z)}, c});
}

// Four lines from the tip back to a square base: a pyramid pointing along `d`.
void arrowHead(std::vector<LineVertex>& out, const Vec3& tip, const Vec3& d, double size, uint32_t c) {
    const Vec3 u = d.x != 0 ? Vec3{0, 1, 0} : Vec3{1, 0, 0}, v = d.z != 0 ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
    const Vec3 base = tip - d * (size * 2);
    const double signs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (const auto& s : signs) line(out, tip, base + (u * s[0] + v * s[1]) * size, c);
}

} // namespace

std::vector<LineVertex> gizmoLines(const Vec3& pivot, double length, render::GizmoMode mode, Axis active) {
    std::vector<LineVertex> out;
    const auto colorOf = [&](Axis a) {
        const Rgb c = active == a ? kActive : kAxisColor[a];
        return packColor(c.r, c.g, c.b, 1.0f);
    };
    for (Axis axis = 0; axis < 3; ++axis) {
        const Vec3 d = axisDir(axis), tip = pivot + d * length;
        const uint32_t c = colorOf(axis);
        if (mode == render::GizmoMode::Rotate) {
            const std::vector<double> ring = ringLines(pivot, axis, length);
            for (size_t i = 0; i + 5 < ring.size(); i += 6) line(out, {ring[i], ring[i + 1], ring[i + 2]}, {ring[i + 3], ring[i + 4], ring[i + 5]}, c);
        } else if (mode == render::GizmoMode::Scale) {
            line(out, pivot, tip, c);
            const double k = length * 0.06;   // a cube at the end: its 12 edges
            for (int bit = 0; bit < 4; ++bit) {
                for (int e = 0; e < 3; ++e) {
                    // the corner (sx, sy, sz) and its neighbour along axis e (only from the negative side, so each edge is made once)
                    const int s[3] = {(bit & 1) ? 1 : -1, (bit & 2) ? 1 : -1, 0};
                    int sign[3];
                    for (int i = 0, j = 0; i < 3; ++i) sign[i] = i == e ? -1 : s[j++];
                    int sign2[3] = {sign[0], sign[1], sign[2]};
                    sign2[e] = 1;
                    line(out, tip + Vec3{sign[0] * k, sign[1] * k, sign[2] * k}, tip + Vec3{sign2[0] * k, sign2[1] * k, sign2[2] * k}, c);
                }
            }
        } else {
            line(out, pivot, tip, c);
            arrowHead(out, tip, d, length * 0.14, c);
        }
    }
    return out;
}

std::vector<LineVertex> originHandleLines(const Vec3& origin, const Vec3& right, const Vec3& up, double half, bool active) {
    std::vector<LineVertex> out;
    const uint32_t c = active ? packColor(1.0f, 1.0f, 1.0f, 1.0f) : packColor(1.0f, 0.85f, 0.2f, 1.0f);
    const Vec3 r = right * half, u = up * half;
    const Vec3 p00 = origin - r - u, p10 = origin + r - u, p11 = origin + r + u, p01 = origin - r + u;
    line(out, p00, p10, c);
    line(out, p10, p11, c);
    line(out, p11, p01, c);
    line(out, p01, p00, c);
    line(out, origin - r * 0.5, origin + r * 0.5, c);
    line(out, origin - u * 0.5, origin + u * 0.5, c);
    return out;
}

} // namespace native::tools
