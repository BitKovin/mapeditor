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
