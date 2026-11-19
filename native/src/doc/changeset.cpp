// doc/changeset -- see changeset.h.
#include "doc/changeset.h"

#include <algorithm>
#include <string_view>
#include <unordered_map>

namespace native::doc {

void Box::add(const core::Vec3& p) {
    mn = {std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z)};
    mx = {std::max(mx.x, p.x), std::max(mx.y, p.y), std::max(mx.z, p.z)};
}

void Box::add(const Box& o) {
    if (!o.valid()) return;
    add(o.mn);
    add(o.mx);
}

bool Box::operator==(const Box& o) const {
    if (!valid() || !o.valid()) return valid() == o.valid();
    return mn.x == o.mn.x && mn.y == o.mn.y && mn.z == o.mn.z && mx.x == o.mx.x && mx.y == o.mx.y && mx.z == o.mx.z;
}

Box brushBox(const core::Brush& brush) {
    Box b;
    for (const core::Vec3& v : brush.vertices) b.add(v);
    return b;
}

ChangeSet allChanged(uint64_t fromVersion, uint64_t toVersion) {
    ChangeSet c;
    c.fromVersion = fromVersion;
    c.toVersion = toVersion;
    c.all = true;
    return c;
}

namespace {

bool same(const core::Vec3& a, const core::Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
bool same(const core::Vec2& a, const core::Vec2& b) { return a.u == b.u && a.v == b.v; }
bool same(const core::FaceProjection& a, const core::FaceProjection& b) {
    return a.scaleU == b.scaleU && a.scaleV == b.scaleV && a.shiftU == b.shiftU && a.shiftV == b.shiftV && a.rotate == b.rotate;
}
bool same(const core::CarveScope& a, const core::CarveScope& b) {
    return a.self == b.self && a.structural == b.structural && a.nonStructural == b.nonStructural;
}

template <class T>
bool sameList(const std::vector<T>& a, const std::vector<T>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!same(a[i], b[i])) return false;
    return true;
}

bool sameRefs(const std::unordered_map<std::string, core::RefTargets>& a, const std::unordered_map<std::string, core::RefTargets>& b) {
    if (a.size() != b.size()) return false;
    for (const auto& [k, v] : a) {
        auto it = b.find(k);
        if (it == b.end() || it->second.paths != v.paths) return false;
    }
    return true;
}

// Geometry bits: everything that changes the brush's mesh. Surface bits: how the faces look.
uint32_t brushKinds(const core::Brush& a, const core::Brush& b) {
    uint32_t kinds = 0;
    bool geometry = a.op != b.op || !same(a.carve, b.carve) || a.smoothEdges != b.smoothEdges || !sameList(a.vertices, b.vertices) ||
                    a.faces.size() != b.faces.size();
    if (!geometry)
        for (size_t i = 0; i < a.faces.size(); ++i)
            if (a.faces[i].id != b.faces[i].id || a.faces[i].loop != b.faces[i].loop) {
                geometry = true;
                break;
            }
    if (geometry) kinds |= Geometry;
    if (a.faces.size() == b.faces.size()) {
        for (size_t i = 0; i < a.faces.size(); ++i) {
            const core::BrushFace& fa = a.faces[i];
            const core::BrushFace& fb = b.faces[i];
            if (fa.texture != fb.texture || fa.lookHash != fb.lookHash || fa.lightmapScale != fb.lightmapScale || !same(fa.projection, fb.projection) ||
                !sameList(fa.uvs, fb.uvs)) {
                kinds |= Surface;
                break;
            }
        }
    } else {
        kinds |= Surface;   // faces came and went: the surfaces differ as well
    }
    return kinds;
}

bool isLightNode(const EntityNode& e) { return e.brushes.empty() && e.typed.classname.rfind("light_", 0) == 0; }

Box point(const core::Vec3& p) {
    Box b;
    b.add(p);
    return b;
}

// Matches the nodes of two vectors by id. The pointer-equal nodes at the same index are skipped without looking at them; only the
// rest is matched through a map, so the cost is O(size) pointer compares plus O(changed) hashing.
template <class Node, class Id, class Both, class OnlyA, class OnlyB>
void matchNodes(const std::vector<std::shared_ptr<const Node>>& a, const std::vector<std::shared_ptr<const Node>>& b, Id id, Both both,
                OnlyA onlyA, OnlyB onlyB) {
    std::vector<size_t> ua, ub;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i]) {
            ua.push_back(i);
            ub.push_back(i);
        }
    for (size_t i = n; i < a.size(); ++i) ua.push_back(i);
    for (size_t i = n; i < b.size(); ++i) ub.push_back(i);
    if (ua.empty() && ub.empty()) return;
    std::unordered_map<std::string_view, size_t> byId;
    byId.reserve(ua.size());
    for (size_t i : ua) byId.emplace(id(*a[i]), i);
    std::vector<size_t> added;
    for (size_t j : ub) {
        auto it = byId.find(id(*b[j]));
        if (it == byId.end()) {
            added.push_back(j);
            continue;
        }
        both(*a[it->second], *b[j]);
        byId.erase(it);
    }
    std::vector<size_t> removed;
    removed.reserve(byId.size());
    for (const auto& [key, i] : byId) removed.push_back(i);
    std::sort(removed.begin(), removed.end());
    for (size_t i : removed) onlyA(*a[i]);
    for (size_t j : added) onlyB(*b[j]);
}

void diffBrushes(const EntityNode& ea, const EntityNode& eb, ChangeSet& out) {
    const std::string& entityId = eb.typed.id;
    matchNodes<BrushNode>(
        ea.brushes, eb.brushes, [](const BrushNode& n) -> std::string_view { return n.brush.id; },
        [&](const BrushNode& x, const BrushNode& y) {
            if (&x == &y || x.contentHash == y.contentHash) return;
            const uint32_t kinds = brushKinds(x.brush, y.brush);
            if (!kinds) return;   // only keys the editor does not model changed
            out.brushes.push_back({"", entityId, y.brush.id, kinds, brushBox(x.brush), brushBox(y.brush)});
        },
        [&](const BrushNode& x) { out.brushes.push_back({"", entityId, x.brush.id, Geometry | Surface, brushBox(x.brush), Box()}); },
        [&](const BrushNode& y) { out.brushes.push_back({"", entityId, y.brush.id, Geometry | Surface, Box(), brushBox(y.brush)}); });
}

void diffEntity(const EntityNode& a, const EntityNode& b, ChangeSet& out) {
    if (&a == &b) return;
    uint32_t kinds = 0;
    if (a.typed.classname != b.typed.classname) kinds |= Class;
    if (a.typed.keyvalues != b.typed.keyvalues || !sameRefs(a.typed.refs, b.typed.refs)) kinds |= Keys;
    if (!same(a.typed.origin, b.typed.origin) || !same(a.typed.angles, b.typed.angles)) kinds |= Transform;
    if (kinds && (isLightNode(a) || isLightNode(b))) kinds |= Light;
    if (kinds) out.entities.push_back({"", b.typed.id, kinds, point(a.typed.origin), point(b.typed.origin)});
    diffBrushes(a, b, out);
}

void diffInstance(const InstanceNode& a, const InstanceNode& b, ChangeSet& out) {
    if (&a == &b) return;
    uint32_t kinds = 0;
    if (a.typed.file != b.typed.file) kinds |= Prefab;
    if (!same(a.typed.origin, b.typed.origin) || !same(a.typed.angles, b.typed.angles) || a.typed.scale.has_value() != b.typed.scale.has_value() ||
        (a.typed.scale && !same(*a.typed.scale, *b.typed.scale)))
        kinds |= Transform;
    // Overrides: compared through their JSON (a handful of small objects per instance).
    if (a.raw.value("overrides", Json()) != b.raw.value("overrides", Json())) kinds |= Prefab;
    if (kinds) out.entities.push_back({b.typed.id, "", kinds, point(a.typed.origin), point(b.typed.origin)});
}

} // namespace

ChangeSet diff(const DocumentVersion& a, const DocumentVersion& b) {
    ChangeSet out;
    out.fromVersion = a.version;
    out.toVersion = b.version;
    if (&a == &b) return out;

    if (a.rootRaw != b.rootRaw) out.all = true;

    matchNodes<EntityNode>(
        a.entities, b.entities, [](const EntityNode& n) -> std::string_view { return n.typed.id; },
        [&](const EntityNode& x, const EntityNode& y) { diffEntity(x, y, out); },
        [&](const EntityNode& x) {
            uint32_t kinds = Class | Keys | Transform;
            if (isLightNode(x)) kinds |= Light;
            out.entities.push_back({"", x.typed.id, kinds, point(x.typed.origin), Box()});
            for (const auto& bn : x.brushes) out.brushes.push_back({"", x.typed.id, bn->brush.id, Geometry | Surface, brushBox(bn->brush), Box()});
        },
        [&](const EntityNode& y) {
            uint32_t kinds = Class | Keys | Transform;
            if (isLightNode(y)) kinds |= Light;
            out.entities.push_back({"", y.typed.id, kinds, Box(), point(y.typed.origin)});
            for (const auto& bn : y.brushes) out.brushes.push_back({"", y.typed.id, bn->brush.id, Geometry | Surface, Box(), brushBox(bn->brush)});
        });

    matchNodes<InstanceNode>(
        a.instances, b.instances, [](const InstanceNode& n) -> std::string_view { return n.typed.id; },
        [&](const InstanceNode& x, const InstanceNode& y) { diffInstance(x, y, out); },
        [&](const InstanceNode& x) { out.entities.push_back({x.typed.id, "", Prefab, point(x.typed.origin), Box()}); },
        [&](const InstanceNode& y) { out.entities.push_back({y.typed.id, "", Prefab, Box(), point(y.typed.origin)}); });
    return out;
}

} // namespace native::doc
