// tools/prefab_preview -- see prefab_preview.h.
#include "tools/prefab_preview.h"

#include <algorithm>
#include <cmath>

#include "doc/edit.h"
#include "doc/io.h"

namespace native::tools {

namespace {

using scene::V3;

constexpr double kYaw = 0.75, kPitch = 0.5;   // the Shell's icon camera

V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 unit(const V3& a) {
    const double l = std::sqrt(dot(a, a));
    return l > 1e-12 ? V3{a[0] / l, a[1] / l, a[2] / l} : V3{0, 1, 0};
}

// A polygon of the content in world space with the colour it is made of.
struct Poly {
    std::vector<V3> pts;
    std::array<double, 3> base;
};

V3 newell(const std::vector<V3>& p) {
    V3 n{0, 0, 0};
    for (size_t i = 0; i < p.size(); ++i) {
        const V3& a = p[i];
        const V3& b = p[(i + 1) % p.size()];
        n[0] += (a[1] - b[1]) * (a[2] + b[2]);
        n[1] += (a[2] - b[2]) * (a[0] + b[0]);
        n[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
    return n;
}

bool isPointEntity(const core::Entity& e) { return e.brushes.empty() && e.classname != "worldspawn"; }

// Near-black class colours would vanish on the dark panel (the lift the Shell's markers use).
std::array<double, 3> lifted(const std::optional<std::array<double, 3>>& c) {
    if (!c) return {1, 0.85, 0.25};
    if (std::max({(*c)[0], (*c)[1], (*c)[2]}) < 0.35) return {0.6, 0.6, 0.6};
    return *c;
}

std::vector<Poly> polysOf(const std::vector<core::ResolvedEntity>& content, const scene::BoxOf& boxOf, const ColorOf& colorOf) {
    static const std::array<double, 3> stone{0.70, 0.72, 0.76};
    std::vector<Poly> out;
    for (const core::ResolvedEntity& r : content) {
        const core::Entity& e = r.entity;
        for (const core::Brush& b : e.brushes) {
            if (b.op == core::BrushOp::Subtract) continue;
            for (const core::BrushFace& f : b.faces) {
                Poly p;
                p.base = stone;
                for (uint32_t i : f.loop)
                    if (i < b.vertices.size()) p.pts.push_back({b.vertices[i].x, b.vertices[i].y, b.vertices[i].z});
                if (p.pts.size() >= 3) out.push_back(std::move(p));
            }
        }
        if (isPointEntity(e)) {
            const std::array<V3, 8> c = scene::worldCorners(scene::pointBox(e, boxOf));
            const std::array<double, 3> color = lifted(colorOf ? colorOf(e.classname) : std::nullopt);
            V3 centre{0, 0, 0};
            for (const V3& p : c)
                for (int k = 0; k < 3; ++k) centre[k] += p[k] / 8;
            static const int quads[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
            for (const auto& q : quads) {
                Poly p;
                p.base = color;
                for (int i : q) p.pts.push_back(c[static_cast<size_t>(i)]);
                if (dot(newell(p.pts), sub(p.pts[0], centre)) < 0) std::reverse(p.pts.begin(), p.pts.end());   // outward
                out.push_back(std::move(p));
            }
        }
    }
    return out;
}

} // namespace

// ---- PrefabPreviews --------------------------------------------------------------------------------------------------------------------

PrefabPreviews::PrefabPreviews(std::filesystem::path assetsRoot) : root_(std::move(assetsRoot)), library_(std::make_shared<doc::PrefabLibrary>(root_)) {}

void PrefabPreviews::setAssetsRoot(const std::filesystem::path& root) {
    root_ = root;
    library_ = std::make_shared<doc::PrefabLibrary>(root_);
    built_.clear();
}

void PrefabPreviews::clear() { setAssetsRoot(root_); }

PreviewContent PrefabPreviews::get(const std::string& file) {
    const auto hit = built_.find(file);
    if (hit != built_.end()) return hit->second;
    PreviewContent out;
    const doc::LoadResult empty = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    doc::VersionEditor ed(empty.version);
    core::PrefabInstance inst;
    inst.id = "preview";
    inst.file = file;
    if (!ed.addInstance(inst)) {
        out.error = ed.error();
    } else if (const auto version = ed.finish()) {
        doc::PrefabResolver resolver(*library_);
        const std::shared_ptr<const doc::Expansion> x = resolver.expand(*version);
        if (x && !x->instances.empty() && x->instances[0]->entities && !x->instances[0]->entities->empty()) {
            out.entities = x->instances[0]->entities;
        } else {
            const std::vector<std::string> problems = x ? x->problems() : std::vector<std::string>();
            out.error = problems.empty() ? "The prefab has no content" : problems.front();
        }
    } else {
        out.error = ed.error();
    }
    built_[file] = out;
    return out;
}

// ---- the picture -----------------------------------------------------------------------------------------------------------------------

std::optional<scene::Box> contentBounds(const std::vector<core::ResolvedEntity>& content, const scene::BoxOf& boxOf) {
    scene::Box box{{1e300, 1e300, 1e300}, {-1e300, -1e300, -1e300}};
    bool any = false;
    const auto grow = [&](const V3& lo, const V3& hi) {
        any = true;
        for (int k = 0; k < 3; ++k) {
            box.min[static_cast<size_t>(k)] = std::min(box.min[static_cast<size_t>(k)], lo[static_cast<size_t>(k)]);
            box.max[static_cast<size_t>(k)] = std::max(box.max[static_cast<size_t>(k)], hi[static_cast<size_t>(k)]);
        }
    };
    for (const core::ResolvedEntity& r : content) {
        for (const core::Brush& b : r.entity.brushes) {
            if (b.op == core::BrushOp::Subtract) continue;
            for (const core::Vec3& v : b.vertices) grow({v.x, v.y, v.z}, {v.x, v.y, v.z});
        }
        if (isPointEntity(r.entity)) {
            const scene::Box bb = scene::boundsOfBox(scene::pointBox(r.entity, boxOf));
            grow(bb.min, bb.max);
        }
    }
    if (!any) return std::nullopt;
    return box;
}

IconModel iconOf(const std::vector<core::ResolvedEntity>& content, const scene::BoxOf& boxOf, const ColorOf& colorOf, int px, double margin) {
    IconModel model;
    const auto bounds = contentBounds(content, boxOf);
    if (!bounds) return model;
    model.bounds = *bounds;
    const std::vector<Poly> polys = polysOf(content, boxOf, colorOf);
    if (polys.empty()) return model;

    // an orthographic camera looking at the content from +x +y +z
    const V3 eye = unit({std::sin(kYaw) * std::cos(kPitch), std::sin(kPitch), std::cos(kYaw) * std::cos(kPitch)});
    const V3 right = unit(cross({0, 1, 0}, eye));
    const V3 up = cross(eye, right);
    const V3 light = unit({eye[0] * 0.7 + 0.3, eye[1] * 0.7 + 0.8, eye[2] * 0.7 + 0.2});

    struct Seen {
        std::vector<std::array<double, 2>> pts;
        double depth;
        std::array<double, 3> color;
    };
    std::vector<Seen> seen;
    double lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};
    for (const Poly& p : polys) {
        const V3 n = unit(newell(p.pts));
        if (dot(n, eye) <= 1e-6) continue;   // faces away
        Seen s;
        s.depth = 0;
        for (const V3& v : p.pts) {
            const std::array<double, 2> q{dot(v, right), -dot(v, up)};
            s.pts.push_back(q);
            s.depth += dot(v, eye) / static_cast<double>(p.pts.size());
            for (int k = 0; k < 2; ++k) {
                lo[k] = std::min(lo[k], q[static_cast<size_t>(k)]);
                hi[k] = std::max(hi[k], q[static_cast<size_t>(k)]);
            }
        }
        const double shade = 0.38 + 0.62 * std::max(0.0, dot(n, light));
        for (int k = 0; k < 3; ++k) s.color[static_cast<size_t>(k)] = std::min(1.0, p.base[static_cast<size_t>(k)] * shade);
        seen.push_back(std::move(s));
    }
    if (seen.empty()) return model;
    std::stable_sort(seen.begin(), seen.end(), [](const Seen& a, const Seen& b) { return a.depth < b.depth; });
    const double size = std::max({hi[0] - lo[0], hi[1] - lo[1], 1e-9});
    const double k = (px - 2 * margin) / size;
    const double ox = (px - (hi[0] - lo[0]) * k) / 2 - lo[0] * k, oy = (px - (hi[1] - lo[1]) * k) / 2 - lo[1] * k;
    for (const Seen& s : seen) {
        IconPoly p;
        p.color = s.color;
        for (const auto& q : s.pts) p.points.push_back({q[0] * k + ox, q[1] * k + oy});
        model.polys.push_back(std::move(p));
    }
    return model;
}

} // namespace native::tools
