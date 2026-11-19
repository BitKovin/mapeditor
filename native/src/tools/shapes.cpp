// tools/shapes -- see shapes.h.
#include "tools/shapes.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace native::tools {

namespace {

constexpr double kPi = 3.14159265358979323846;

core::Vec3 toCore(const Vec3& v) { return {v.x, v.y, v.z}; }
Vec3 at(const Frame& f, double x, double y, double z) { return f.o + f.u * x + f.v * y + f.n * z; }

void flipWinding(core::Brush& b) {
    for (core::BrushFace& f : b.faces) std::reverse(f.loop.begin(), f.loop.end());
}

core::BrushFace face(uint32_t id, std::vector<uint32_t> loop, const std::string& texture) {
    core::BrushFace f;
    f.id = id;
    f.loop = std::move(loop);
    f.texture = texture;
    return f;
}

// Prism from a convex polygon profile (in the local x / z plane, CCW seen from +y) extruded over y in [y0, y1].
core::Brush prism(const std::string& id, const Frame& f, const std::vector<std::pair<double, double>>& profile, double y0, double y1, const std::string& texture) {
    const uint32_t n = static_cast<uint32_t>(profile.size());
    core::Brush b;
    b.id = id;
    for (const auto& p : profile) b.vertices.push_back(toCore(at(f, p.first, y0, p.second)));
    for (const auto& p : profile) b.vertices.push_back(toCore(at(f, p.first, y1, p.second)));
    std::vector<uint32_t> bottom(n), top(n);
    for (uint32_t i = 0; i < n; ++i) {
        bottom[i] = n - 1 - i;
        top[i] = n + i;
    }
    b.faces.push_back(face(0, bottom, texture));   // y0 side
    b.faces.push_back(face(1, top, texture));      // y1 side
    for (uint32_t i = 0; i < n; ++i) b.faces.push_back(face(2 + i, {i, (i + 1) % n, n + (i + 1) % n, n + i}, texture));
    if (b.signedVolume() < 0) flipWinding(b);   // the frame may be left- or right-handed
    return b;
}

// A UV sphere filling the box (an ellipsoid when it is not a cube): poles on the height axis, `segments` faces around, `rings` from pole to pole.
core::Brush sphere(const std::string& id, const Frame& f, int segments, int rings, const std::string& texture) {
    const double rx = f.du / 2, ry = f.dv / 2, rz = f.h / 2;
    core::Brush b;
    b.id = id;
    b.vertices.push_back(toCore(at(f, rx, ry, 0)));
    for (int k = 1; k < rings; ++k) {
        const double t = kPi * k / rings, s = std::sin(t), z = rz * (1 - std::cos(t));
        for (int j = 0; j < segments; ++j) {
            const double a = 2 * kPi * j / segments;
            b.vertices.push_back(toCore(at(f, rx + rx * s * std::cos(a), ry + ry * s * std::sin(a), z)));
        }
    }
    const uint32_t top = static_cast<uint32_t>(b.vertices.size());
    b.vertices.push_back(toCore(at(f, rx, ry, f.h)));
    const auto ring = [&](int k, int j) { return static_cast<uint32_t>(1 + (k - 1) * segments + ((j % segments) + segments) % segments); };
    uint32_t fid = 0;
    for (int j = 0; j < segments; ++j) b.faces.push_back(face(fid++, {0, ring(1, j + 1), ring(1, j)}, texture));
    for (int k = 1; k + 1 < rings; ++k)
        for (int j = 0; j < segments; ++j) b.faces.push_back(face(fid++, {ring(k, j), ring(k, j + 1), ring(k + 1, j + 1), ring(k + 1, j)}, texture));
    for (int j = 0; j < segments; ++j) b.faces.push_back(face(fid++, {top, ring(rings - 1, j), ring(rings - 1, j + 1)}, texture));
    if (b.signedVolume() < 0) flipWinding(b);
    return b;
}

std::vector<uint32_t> faceIdsFrom(const core::Brush& b, uint32_t first) {
    std::vector<uint32_t> ids;
    for (const core::BrushFace& f : b.faces)
        if (f.id >= first) ids.push_back(f.id);
    return ids;
}

} // namespace

const char* shapeName(ShapeId s) {
    switch (s) {
        case ShapeId::Box: return "box";
        case ShapeId::Cylinder: return "cylinder";
        case ShapeId::Wedge: return "wedge";
        case ShapeId::Arch: return "arch";
        case ShapeId::Stairs: return "stairs";
        case ShapeId::Sphere: return "sphere";
    }
    return "box";
}

Frame frameOf(const DrawPlane& plane, const Rect& rect, double height) {
    // negative extents flip the axis so the local box is always positive; handedness is fixed by the winding fix of the builders
    const double su = rect.du < 0 ? -1 : 1, sv = rect.dv < 0 ? -1 : 1, sh = height < 0 ? -1 : 1;
    return {rect.origin, plane.u * su, plane.v * sv, plane.normal * sh, std::fabs(rect.du), std::fabs(rect.dv), std::fabs(height)};
}

Frame turned(const Frame& f, Turn turn) {
    const Vec3 U = f.u * f.du, V = f.v * f.dv;
    Frame r = f;
    switch (turn & 3) {
        case 1: r.o = f.o + U; r.u = f.v; r.v = f.u * -1.0; r.du = f.dv; r.dv = f.du; break;
        case 2: r.o = f.o + U + V; r.u = f.u * -1.0; r.v = f.v * -1.0; break;
        case 3: r.o = f.o + V; r.u = f.v * -1.0; r.v = f.u; r.du = f.dv; r.dv = f.du; break;
        default: break;
    }
    return r;
}

core::Brush smoothBetween(const core::Brush& b, const std::vector<uint32_t>& faceIds) {
    const std::set<uint32_t> ids(faceIds.begin(), faceIds.end());
    // the faces that meet at an edge (a directed vertex pair appears in one loop, the reverse in the neighbour)
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> edges;
    for (const core::BrushFace& f : b.faces)
        for (size_t i = 0; i < f.loop.size(); ++i) {
            const uint32_t p = f.loop[i], q = f.loop[(i + 1) % f.loop.size()];
            edges[{std::min(p, q), std::max(p, q)}].push_back(f.id);
        }
    std::set<std::pair<uint32_t, uint32_t>> pairs(b.smoothEdges.begin(), b.smoothEdges.end());
    const size_t before = pairs.size();
    for (const auto& e : edges) {
        if (e.second.size() != 2 || !ids.count(e.second[0]) || !ids.count(e.second[1])) continue;
        pairs.insert({std::min(e.second[0], e.second[1]), std::max(e.second[0], e.second[1])});
    }
    if (pairs.size() == before) return b;
    core::Brush out = b;
    out.smoothEdges.assign(pairs.begin(), pairs.end());
    return out;
}

ShapeOutcome buildShape(ShapeId shape, const DrawPlane& plane, const Rect& rect, double height, const std::string& texture, const ShapeParams& params,
                        const std::function<std::string()>& newId, Turn turn) {
    return buildInFrame(shape, turned(frameOf(plane, rect, height), turn), texture, params, newId);
}

ShapeOutcome buildInFrame(ShapeId shape, const Frame& f, const std::string& texture, const ShapeParams& params, const std::function<std::string()>& newId) {
    ShapeOutcome result;
    if (f.du < 1e-4 || f.dv < 1e-4 || f.h < 1e-4) {
        result.error = "Shape is too thin";
        return result;
    }
    std::vector<core::Brush>& out = result.brushes;
    switch (shape) {
        case ShapeId::Box: {
            const double x = f.du, y = f.dv, z = f.h;
            out.push_back(prism(newId(), f, {{0, 0}, {x, 0}, {x, z}, {0, z}}, 0, y, texture));
            break;
        }
        case ShapeId::Cylinder: {
            const int n = std::max(3, std::min(64, params.sides));
            std::vector<std::pair<double, double>> profile;
            for (int i = 0; i < n; ++i) {
                const double a = static_cast<double>(i) / n * kPi * 2;
                profile.push_back({f.du / 2 + std::cos(a) * f.du / 2, f.h / 2 + std::sin(a) * f.h / 2});
            }
            core::Brush b = prism(newId(), f, profile, 0, f.dv, texture);
            out.push_back(params.smooth ? smoothBetween(b, faceIdsFrom(b, 2)) : b);   // faces 2.. are the sides: smooth between them, the caps stay sharp
            break;
        }
        case ShapeId::Sphere: {
            core::Brush b = sphere(newId(), f, std::max(4, std::min(32, params.segments)), std::max(2, std::min(16, params.rings)), texture);
            out.push_back(params.smooth ? smoothBetween(b, faceIdsFrom(b, 0)) : b);
            break;
        }
        case ShapeId::Wedge:   // a ramp: full height at u = du, zero at u = 0
            out.push_back(prism(newId(), f, {{0, 0}, {f.du, 0}, {f.du, f.h}}, 0, f.dv, texture));
            break;
        case ShapeId::Stairs: {
            const int n = std::max(2, std::min(64, params.steps));
            for (int i = 0; i < n; ++i) {   // each step is a box from the floor up to its own height, so every brush stays convex
                const double x0 = static_cast<double>(i) / n * f.du, x1 = static_cast<double>(i + 1) / n * f.du, top = static_cast<double>(i + 1) / n * f.h;
                out.push_back(prism(newId(), f, {{x0, 0}, {x1, 0}, {x1, top}, {x0, top}}, 0, f.dv, texture));
            }
            break;
        }
        case ShapeId::Arch: {
            const int n = std::max(3, std::min(64, params.archSegments));
            const double t = params.archThickness;
            if (t <= 1e-3 || t >= std::min(f.du / 2, f.h) - 1e-3) {
                result.error = "Arch thickness must be smaller than half its width and its height";
                out.clear();
                return result;
            }
            const double rx = f.du / 2, rz = f.h;   // outer half-ellipse; the flat side sits on the bottom of the box
            const auto pt = [&](double a, double ox, double oz) { return std::pair<double, double>{rx + std::cos(a) * ox, std::sin(a) * oz}; };
            for (int i = 0; i < n; ++i) {
                const double a0 = kPi - static_cast<double>(i) / n * kPi, a1 = kPi - static_cast<double>(i + 1) / n * kPi;
                std::vector<std::pair<double, double>> profile = {pt(a0, rx, rz), pt(a0, rx - t, rz - t), pt(a1, rx - t, rz - t), pt(a1, rx, rz)};
                std::reverse(profile.begin(), profile.end());   // outer0, inner0, inner1, outer1 runs clockwise for the left-to-right sweep: reversed for CCW
                out.push_back(prism(newId(), f, profile, 0, f.dv, texture));
            }
            break;
        }
    }
    for (const core::Brush& b : out) {
        if (const auto problem = b.checkConvexSolid()) {
            result.error = std::string(shapeName(shape)) + ": " + *problem;
            out.clear();
            return result;
        }
    }
    return result;
}

std::vector<float> shapePreviewLines(ShapeId shape, const DrawPlane& plane, const Rect& rect, double height, const ShapeParams& params, Turn turn) {
    const ShapeOutcome r = buildShape(shape, plane, rect, height, "", params, [] { return std::string("p"); }, turn);
    std::vector<float> out;
    if (!r.ok()) return out;
    std::set<std::string> seen;
    const auto key = [](const core::Vec3& p) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%.5f,%.5f,%.5f", p.x, p.y, p.z);
        return std::string(buf);
    };
    for (const core::Brush& b : r.brushes)
        for (const core::BrushFace& f : b.faces)
            for (size_t i = 0; i < f.loop.size(); ++i) {
                const core::Vec3& p = b.vertices[f.loop[i]];
                const core::Vec3& q = b.vertices[f.loop[(i + 1) % f.loop.size()]];
                const std::string a = key(p), c = key(q), k = a < c ? a + "|" + c : c + "|" + a;
                if (!seen.insert(k).second) continue;
                for (const core::Vec3* v : {&p, &q}) {
                    out.push_back(static_cast<float>(v->x));
                    out.push_back(static_cast<float>(v->y));
                    out.push_back(static_cast<float>(v->z));
                }
            }
    return out;
}

} // namespace native::tools
