// tools/select_ops -- see select_ops.h.
#include "tools/select_ops.h"

#include <cmath>
#include <set>
#include <string>

namespace native::tools {

using doc::SelRef;

namespace {

std::string keyOf(const SelRef& s) {
    std::string k;
    for (const std::string& p : s.instancePath) k += p + '/';
    return k + '\x1f' + s.entityId + '\x1f' + s.brushId;
}

V3 groundAxis(const V3& v) {
    if (std::fabs(v[0]) >= std::fabs(v[2])) return {v[0] < 0 ? -1.0 : 1.0, 0, 0};
    return {0, 0, v[2] < 0 ? -1.0 : 1.0};
}

} // namespace

std::vector<SelRef> selectableItems(const doc::DocumentVersion& d) {
    std::vector<SelRef> out;
    for (const auto& e : d.entities) {
        if (!e->brushes.empty()) {
            for (const auto& b : e->brushes) out.push_back(SelRef::brush(e->typed.id, b->brush.id));
        } else if (e->typed.classname != "worldspawn" && !e->hasBrushesKey) {
            out.push_back(SelRef::entity(e->typed.id));
        }
    }
    for (const auto& i : d.instances) out.push_back(SelRef::instance(i->typed.id));
    return out;
}

std::vector<SelRef> invertedItems(const doc::DocumentVersion& d, const std::vector<SelRef>& selected) {
    std::set<std::string> have;
    for (const SelRef& s : selected) {
        if (s.isInstance()) {
            have.insert(keyOf(s));
            continue;
        }
        if (!s.instancePath.empty()) continue;   // something inside an instance: the instance is not "selected" by it
        if (s.brushId.empty()) {
            const int ei = d.findEntity(s.entityId);
            if (ei >= 0 && !d.entities[static_cast<size_t>(ei)]->brushes.empty()) {
                for (const auto& b : d.entities[static_cast<size_t>(ei)]->brushes) have.insert(keyOf(SelRef::brush(s.entityId, b->brush.id)));
                continue;
            }
            have.insert(keyOf(SelRef::entity(s.entityId)));
            continue;
        }
        have.insert(keyOf(SelRef::brush(s.entityId, s.brushId)));
    }
    std::vector<SelRef> out;
    for (const SelRef& s : selectableItems(d))
        if (!have.count(keyOf(s))) out.push_back(s);
    return out;
}

SimilarSelect selectSimilar(const doc::DocumentVersion& d, const std::vector<SelRef>& selected, SimilarKind kind) {
    SimilarSelect out;
    const char* what = kind == SimilarKind::Class ? "class" : kind == SimilarKind::Texture ? "texture" : "prefab";
    const auto none = [&] {
        out.message = std::string("Select similar (") + what + "): select " + (kind == SimilarKind::Prefab ? "a prefab instance" : kind == SimilarKind::Texture ? "a brush or a face" : "something") + " first";
        return out;
    };
    auto plural = [](size_t n) { return std::to_string(n) + (n == 1 ? " item" : " items"); };

    if (kind == SimilarKind::Prefab) {
        std::set<std::string> files;
        for (const SelRef& s : selected) {
            if (!s.isInstance()) continue;
            for (const auto& i : d.instances)
                if (i->typed.id == s.instancePath[0]) files.insert(i->typed.file);
        }
        if (files.empty()) return none();
        for (const auto& i : d.instances)
            if (files.count(i->typed.file)) out.found.push_back(SelRef::instance(i->typed.id));
        out.message = "Selected " + plural(out.found.size()) + " of the same prefab";
        return out;
    }

    if (kind == SimilarKind::Class) {
        std::set<std::string> classes;
        for (const SelRef& s : selected) {
            if (!s.instancePath.empty()) continue;
            const int ei = d.findEntity(s.entityId);
            if (ei >= 0) classes.insert(d.entities[static_cast<size_t>(ei)]->typed.classname);
        }
        if (classes.empty()) return none();
        for (const SelRef& item : selectableItems(d)) {
            const int ei = d.findEntity(item.entityId);
            if (ei >= 0 && classes.count(d.entities[static_cast<size_t>(ei)]->typed.classname)) out.found.push_back(item);
        }
        out.message = "Selected " + plural(out.found.size()) + " of the same class";
        return out;
    }

    std::set<std::string> textures;
    for (const SelRef& s : selected) {
        if (!s.instancePath.empty()) continue;
        const int ei = d.findEntity(s.entityId);
        if (ei < 0) continue;
        const auto& ent = *d.entities[static_cast<size_t>(ei)];
        for (const auto& b : ent.brushes) {
            if (!s.brushId.empty() && b->brush.id != s.brushId) continue;
            for (const core::BrushFace& f : b->brush.faces)
                if (!s.faceId || f.id == *s.faceId) textures.insert(f.texture);
        }
    }
    if (textures.empty()) return none();
    for (const auto& e : d.entities)
        for (const auto& b : e->brushes)
            for (const core::BrushFace& f : b->brush.faces)
                if (textures.count(f.texture)) {
                    out.found.push_back(SelRef::brush(e->typed.id, b->brush.id));
                    break;
                }
    out.message = "Selected " + plural(out.found.size()) + " with the same texture";
    return out;
}

V3 nudgeDirection(const std::string& key, const V3& right, const V3& forward) {
    if (key == "left") return groundAxis({-right[0], 0, -right[2]});
    if (key == "right") return groundAxis(right);
    if (key == "up") return groundAxis(forward);
    if (key == "down") return groundAxis({-forward[0], 0, -forward[2]});
    if (key == "pageup") return {0, 1, 0};
    if (key == "pagedown") return {0, -1, 0};
    return {0, 0, 0};
}

} // namespace native::tools
