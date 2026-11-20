// tools/edit_ops -- see edit_ops.h.
#include "tools/edit_ops.h"

#include <core/entity_class.h>

#include <algorithm>
#include <limits>
#include <set>

#include "doc/edit.h"
#include "tools/box_tool.h"

namespace native::tools {

namespace {

using doc::Json;
using doc::SelRef;
using Base = std::shared_ptr<const doc::DocumentVersion>;

core::Vec3 add(const core::Vec3& a, const core::Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }

core::Brush translated(core::Brush b, const core::Vec3& d) {
    for (core::Vec3& v : b.vertices) v = add(v, d);
    return b;
}

// What a list of selections stands for. Face selections stand for their brush; selections inside prefab instances are left alone.
struct Targets {
    std::vector<std::string> entities;                                  // whole entities, in selection order
    std::vector<std::pair<std::string, std::string>> brushes;           // (entity, brush) whose entity is not selected as a whole
    std::vector<std::string> instances;                                 // top-level prefab instances
};

Targets targetsOf(const std::vector<SelRef>& sels) {
    Targets t;
    std::set<std::string> whole;
    for (const SelRef& s : sels)
        if (s.instancePath.empty() && !s.entityId.empty() && s.brushId.empty()) whole.insert(s.entityId);
    std::set<std::string> seen;
    for (const SelRef& s : sels) {
        if (s.isInstance()) {
            if (s.instancePath.size() == 1 && seen.insert("i:" + s.instancePath[0]).second) t.instances.push_back(s.instancePath[0]);
            continue;
        }
        if (!s.instancePath.empty() || s.entityId.empty()) continue;
        if (s.brushId.empty()) {
            if (seen.insert("e:" + s.entityId).second) t.entities.push_back(s.entityId);
        } else if (!whole.count(s.entityId) && seen.insert("b:" + s.entityId + "|" + s.brushId).second) {
            t.brushes.push_back({s.entityId, s.brushId});
        }
    }
    return t;
}

// The brushes the selections cover: those of whole entities and the single brushes.
struct BrushTarget {
    std::string entityId;
    std::shared_ptr<const doc::BrushNode> node;
};
std::vector<BrushTarget> brushTargets(const doc::DocumentVersion& d, const std::vector<SelRef>& sels) {
    const Targets t = targetsOf(sels);
    std::vector<BrushTarget> out;
    for (const std::string& id : t.entities) {
        const int ei = d.findEntity(id);
        if (ei < 0) continue;
        for (const auto& b : d.entities[static_cast<size_t>(ei)]->brushes) out.push_back({id, b});
    }
    for (const auto& [eid, bid] : t.brushes) {
        const int bi = d.findBrush(eid, bid);
        const int ei = d.findEntity(eid);
        if (bi >= 0 && ei >= 0) out.push_back({eid, d.entities[static_cast<size_t>(ei)]->brushes[static_cast<size_t>(bi)]});
    }
    return out;
}

std::string worldspawnId(const doc::DocumentVersion& d) {
    for (const auto& e : d.entities)
        if (e->typed.classname == "worldspawn") return e->typed.id;
    return "";
}

// The world entity of the version being edited: made when the document has none.
bool ensureWorld(doc::VersionEditor& ed, const doc::DocumentVersion& base, std::string& worldId) {
    worldId = worldspawnId(base);
    if (!worldId.empty()) return true;
    core::Entity w;
    w.id = worldId = "e_worldspawn";
    w.classname = "worldspawn";
    return ed.addEntity(w);
}

core::Entity entityWithBrushes(const doc::EntityNode& n) {
    core::Entity e = n.typed;
    e.brushes.clear();
    for (const auto& b : n.brushes) e.brushes.push_back(b->brush);
    return e;
}

std::vector<Json> brushRaws(const doc::EntityNode& n) {
    std::vector<Json> out;
    for (const auto& b : n.brushes) out.push_back(b->raw);
    return out;
}

bool bounds(const std::vector<core::Brush>& brushes, core::Vec3& lo, core::Vec3& hi) {
    bool any = false;
    lo = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
    hi = {-lo.x, -lo.y, -lo.z};
    for (const core::Brush& b : brushes) {
        core::Vec3 a, c;
        if (!b.bounds(a, c)) continue;
        any = true;
        lo = {std::min(lo.x, a.x), std::min(lo.y, a.y), std::min(lo.z, a.z)};
        hi = {std::max(hi.x, c.x), std::max(hi.y, c.y), std::max(hi.z, c.z)};
    }
    return any;
}

EditResult refused(std::string message) {
    EditResult r;
    r.error = std::move(message);
    return r;
}

std::string prefabBaseName(const std::string& file) {
    std::string f = file;
    std::replace(f.begin(), f.end(), '\\', '/');
    f = f.substr(f.find_last_of('/') == std::string::npos ? 0 : f.find_last_of('/') + 1);
    for (const std::string ext : {".map.json", ".json"})
        if (f.size() > ext.size() && f.compare(f.size() - ext.size(), ext.size(), ext) == 0) {
            f.erase(f.size() - ext.size());
            break;
        }
    return f.empty() ? "prefab" : f;
}

std::set<std::string> instanceNames(const doc::DocumentVersion& d) {
    std::set<std::string> taken;
    for (const auto& p : d.instances)
        if (!p->typed.name.empty()) taken.insert(p->typed.name);
    std::set<std::string> all = taken;
    for (const auto& p : d.instances) {
        if (!p->typed.name.empty()) continue;
        const std::string base = prefabBaseName(p->typed.file);
        std::string cand = base;
        for (int n = 2; taken.count(cand); ++n) cand = base + std::to_string(n);
        taken.insert(cand);
        all.insert(cand);
    }
    return all;
}

// A new instance name that is also free among the names given out in the same edit.
std::string freshName(const doc::DocumentVersion& d, const std::string& file, std::set<std::string>& given) {
    const std::set<std::string> taken = instanceNames(d);
    const std::string base = prefabBaseName(file);
    std::string cand = base;
    for (int n = 2; taken.count(cand) || given.count(cand); ++n) cand = base + std::to_string(n);
    given.insert(cand);
    return cand;
}

} // namespace

std::string newInstanceName(const doc::DocumentVersion& d, const std::string& file) {
    const std::set<std::string> taken = instanceNames(d);
    const std::string base = prefabBaseName(file);
    std::string cand = base;
    for (int n = 2; taken.count(cand); ++n) cand = base + std::to_string(n);
    return cand;
}

std::vector<std::string> instanceDisplayNames(const doc::DocumentVersion& d) {
    std::set<std::string> taken;
    for (const auto& p : d.instances)
        if (!p->typed.name.empty()) taken.insert(p->typed.name);
    std::vector<std::string> out;
    for (const auto& p : d.instances) {
        if (!p->typed.name.empty()) {
            out.push_back(p->typed.name);
            continue;
        }
        const std::string base = prefabBaseName(p->typed.file);
        std::string cand = base;
        for (int n = 2; taken.count(cand); ++n) cand = base + std::to_string(n);
        taken.insert(cand);
        out.push_back(cand);
    }
    return out;
}

// ---- delete / duplicate / group -----------------------------------------------------------------------------------------------------

EditResult deleteSelection(const Base& base, const std::vector<SelRef>& sels) {
    if (!base) return refused("no document");
    const Targets t = targetsOf(sels);
    const size_t count = t.entities.size() + t.brushes.size() + t.instances.size();
    if (!count) return {};
    doc::VersionEditor ed(base);
    for (const auto& [eid, bid] : t.brushes) ed.removeBrush(eid, bid);
    for (const std::string& id : t.entities) ed.removeEntity(id);
    for (const std::string& id : t.instances) ed.removeInstance(id);
    EditResult r;
    r.label = count > 1 ? "Delete " + std::to_string(count) + " items" : (!t.instances.empty() ? "Delete prefab instance" : (!t.brushes.empty() ? "Delete brush" : "Delete entity"));
    r.version = ed.finish();
    if (!r.version) r.error = ed.error();
    return r;
}

EditResult duplicateSelection(const Base& base, const std::vector<SelRef>& sels) {
    if (!base) return refused("no document");
    const Targets t = targetsOf(sels);
    const size_t count = t.entities.size() + t.brushes.size() + t.instances.size();
    if (!count) return {};
    doc::VersionEditor ed(base);
    EditResult r;
    std::set<std::string> given;
    for (const std::string& id : t.instances) {
        for (const auto& p : base->instances) {
            if (p->typed.id != id) continue;
            core::PrefabInstance copy = p->typed;
            copy.id = newId("pi");
            copy.name = freshName(*base, p->typed.file, given);
            if (ed.addInstance(copy, p->raw)) r.select.push_back(SelRef::instance(copy.id));
        }
    }
    for (const std::string& id : t.entities) {
        const int ei = base->findEntity(id);
        if (ei < 0) continue;
        const doc::EntityNode& n = *base->entities[static_cast<size_t>(ei)];
        if (n.typed.classname == "worldspawn") continue;   // there is only one world
        core::Entity copy = entityWithBrushes(n);
        copy.id = newId("e");
        for (core::Brush& b : copy.brushes) b.id = newId("b");
        if (ed.addEntity(copy, n.raw, brushRaws(n))) r.select.push_back(SelRef::entity(copy.id));
    }
    for (const auto& [eid, bid] : t.brushes) {
        const int bi = base->findBrush(eid, bid);
        if (bi < 0) continue;
        const int ei = base->findEntity(eid);
        const auto& node = base->entities[static_cast<size_t>(ei)]->brushes[static_cast<size_t>(bi)];
        core::Brush copy = node->brush;
        copy.id = newId("b");
        if (ed.addBrush(eid, copy, node->raw)) r.select.push_back(SelRef::brush(eid, copy.id));
    }
    r.label = count > 1 ? "Duplicate " + std::to_string(count) + " items" : "Duplicate";
    r.version = ed.finish();
    if (!r.version) r.error = ed.error();
    else if (r.version == base) r.version = nullptr;
    return r;
}

EditResult makeEntityFromSelection(const Base& base, const std::vector<SelRef>& sels, const std::string& classname) {
    if (!base) return refused("no document");
    const std::vector<BrushTarget> targets = brushTargets(*base, sels);
    if (targets.empty()) return refused("Make entity: select brush(es) first");
    if (core::EntityClassRegistry::requiresConvexBrushes(classname))
        for (const BrushTarget& t : targets)
            if (t.node->brush.op != core::BrushOp::Subtract && t.node->brush.checkConvexSolid())
                return refused("Make entity: " + classname + " needs convex brushes - use func_detail for the concave one");
    doc::VersionEditor ed(base);
    core::Entity entity;
    entity.id = newId("e");
    entity.classname = classname;
    std::vector<Json> raws;
    for (const BrushTarget& t : targets) {
        ed.removeBrush(t.entityId, t.node->brush.id);
        entity.brushes.push_back(t.node->brush);
        raws.push_back(t.node->raw);
    }
    core::Vec3 lo, hi;
    if (bounds(entity.brushes, lo, hi)) entity.origin = {(lo.x + hi.x) / 2, (lo.y + hi.y) / 2, (lo.z + hi.z) / 2};   // the pivot is the centre of the selection's box
    if (!ed.addEntity(entity, Json(), raws)) return refused(ed.error());
    EditResult r;
    r.label = "Make " + classname;
    r.version = ed.finish();
    if (!r.version) return refused(ed.error());
    for (const core::Brush& b : entity.brushes) r.select.push_back(SelRef::brush(entity.id, b.id));
    return r;
}

EditResult moveSelectionToWorld(const Base& base, const std::vector<SelRef>& sels) {
    if (!base) return refused("no document");
    const std::string world = worldspawnId(*base);
    if (world.empty()) return refused("There is no worldspawn entity");
    std::vector<BrushTarget> targets = brushTargets(*base, sels);
    targets.erase(std::remove_if(targets.begin(), targets.end(), [&](const BrushTarget& t) { return t.entityId == world; }), targets.end());
    if (targets.empty()) return refused("Move to world: the selected brushes are already in the world");
    for (const BrushTarget& t : targets)
        if (t.node->brush.op != core::BrushOp::Subtract && t.node->brush.checkConvexSolid())
            return refused("Move to world: a concave brush cannot be solid world geometry - keep it in func_detail");
    doc::VersionEditor ed(base);
    for (const BrushTarget& t : targets) ed.removeBrush(t.entityId, t.node->brush.id);
    for (const BrushTarget& t : targets) ed.addBrush(world, t.node->brush, t.node->raw);
    EditResult r;
    r.label = "Move to world";
    r.version = ed.finish();
    if (!r.version) return refused(ed.error());
    for (const BrushTarget& t : targets) r.select.push_back(SelRef::brush(world, t.node->brush.id));
    return r;
}

// ---- the clipboard ----------------------------------------------------------------------------------------------------------------

std::optional<ClipboardPayload> buildClipboard(const doc::DocumentVersion& d, const std::vector<SelRef>& sels) {
    const Targets t = targetsOf(sels);
    ClipboardPayload p;
    for (const std::string& id : t.instances)
        for (const auto& inst : d.instances)
            if (inst->typed.id == id) p.items.push_back({{"kind", "instance"}, {"instance", inst->raw}});
    // the order of the selection decides nothing in the Shell either: instances, then entities / brushes as selected
    for (const SelRef& s : sels) {
        if (!s.instancePath.empty() || s.entityId.empty()) continue;
        const int ei = d.findEntity(s.entityId);
        if (ei < 0) continue;
        const doc::EntityNode& n = *d.entities[static_cast<size_t>(ei)];
        if (s.brushId.empty()) {
            if (std::find(t.entities.begin(), t.entities.end(), s.entityId) == t.entities.end()) continue;
            Json e = n.raw;
            if (!n.brushes.empty() || n.hasBrushesKey) {
                Json bs = Json::array();
                for (const auto& b : n.brushes) bs.push_back(b->raw);
                e["brushes"] = std::move(bs);
            }
            const Json item = {{"kind", "entity"}, {"entity", e}};
            if (std::find(p.items.begin(), p.items.end(), item) == p.items.end()) p.items.push_back(item);
        } else if (std::find(t.brushes.begin(), t.brushes.end(), std::make_pair(s.entityId, s.brushId)) != t.brushes.end()) {
            const int bi = d.findBrush(s.entityId, s.brushId);
            if (bi < 0) continue;
            const Json item = {{"kind", "brush"}, {"entity", {{"id", n.typed.id}, {"classname", n.typed.classname}}}, {"brush", n.brushes[static_cast<size_t>(bi)]->raw}};
            if (std::find(p.items.begin(), p.items.end(), item) == p.items.end()) p.items.push_back(item);
        }
    }
    if (p.items.empty()) return std::nullopt;
    return p;
}

std::string serializeClipboard(const ClipboardPayload& p) {
    return Json{{"format", kClipboardFormat}, {"version", kClipboardVersion}, {"items", p.items}}.dump();
}

std::optional<ClipboardPayload> parseClipboard(const std::string& text) {
    Json v = Json::parse(text, nullptr, false);
    if (!v.is_object() || v.value("format", std::string()) != kClipboardFormat) return std::nullopt;
    if (!v.contains("version") || !v["version"].is_number_integer() || v["version"].get<int>() > kClipboardVersion) return std::nullopt;
    if (!v.contains("items") || !v["items"].is_array()) return std::nullopt;
    ClipboardPayload p;
    for (const Json& it : v["items"]) {
        if (!it.is_object()) continue;
        const std::string kind = it.value("kind", std::string());
        const auto hasString = [](const Json& o, const char* key) { return o.is_object() && o.contains(key) && o[key].is_string(); };
        bool ok = false;
        if (kind == "brush") ok = it.contains("brush") && it["brush"].is_object() && it["brush"].contains("vertices") && it["brush"]["vertices"].is_array() && it["brush"].contains("faces") &&
                                  it["brush"]["faces"].is_array() && it.contains("entity") && hasString(it["entity"], "id");
        else if (kind == "entity") ok = it.contains("entity") && hasString(it["entity"], "id");
        else if (kind == "instance") ok = it.contains("instance") && hasString(it["instance"], "file");
        if (ok) p.items.push_back(it);
    }
    if (p.items.empty()) return std::nullopt;
    return p;
}

EditResult pasteInto(const Base& base, const ClipboardPayload& payload, const core::Vec3& offset) {
    if (!base) return refused("no document");
    doc::VersionEditor ed(base);
    EditResult r;
    std::string worldId;
    std::set<std::string> given;
    for (const Json& it : payload.items) {
        try {
            const std::string kind = it.value("kind", std::string());
            if (kind == "instance") {
                core::PrefabInstance inst = core::instanceFromJson(it["instance"]);
                inst.id = newId("pi");
                inst.name = freshName(*base, inst.file, given);
                inst.origin = add(inst.origin, offset);
                if (!ed.addInstance(inst, it["instance"])) return refused(ed.error());
                r.select.push_back(SelRef::instance(inst.id));
            } else if (kind == "entity") {
                core::Entity src = core::entityFromJson(it["entity"]);
                std::vector<Json> raws;
                if (it["entity"].contains("brushes") && it["entity"]["brushes"].is_array())
                    for (const Json& b : it["entity"]["brushes"]) raws.push_back(b);
                if (src.classname == "worldspawn") {   // there is only one worldspawn: its brushes join the existing one
                    if (!ensureWorld(ed, *base, worldId)) return refused(ed.error());
                    for (size_t i = 0; i < src.brushes.size(); ++i) {
                        core::Brush b = translated(src.brushes[i], offset);
                        b.id = newId("b");
                        if (!ed.addBrush(worldId, b, i < raws.size() ? raws[i] : Json())) return refused(ed.error());
                        r.select.push_back(SelRef::brush(worldId, b.id));
                    }
                    continue;
                }
                core::Entity e = src;
                e.id = newId("e");
                e.origin = add(e.origin, offset);
                for (core::Brush& b : e.brushes) {
                    b = translated(b, offset);
                    b.id = newId("b");
                }
                Json rawBase = it["entity"];
                rawBase.erase("brushes");
                if (!ed.addEntity(e, rawBase, raws)) return refused(ed.error());
                r.select.push_back(SelRef::entity(e.id));
            } else if (kind == "brush") {
                const std::string fromId = it["entity"].value("id", std::string()), fromClass = it["entity"].value("classname", std::string());
                std::string target;
                const int ei = base->findEntity(fromId);
                if (ei >= 0 && base->entities[static_cast<size_t>(ei)]->typed.classname == fromClass) target = fromId;   // back into the entity it came from
                if (target.empty()) {
                    if (!ensureWorld(ed, *base, worldId)) return refused(ed.error());
                    target = worldId;
                }
                core::Brush b = translated(core::brushFromJson(it["brush"]), offset);
                b.id = newId("b");
                if (!ed.addBrush(target, b, it["brush"])) return refused(ed.error());
                r.select.push_back(SelRef::brush(target, b.id));
            }
        } catch (const std::exception& e) {
            return refused(std::string("Paste: ") + e.what());
        }
    }
    r.label = payload.size() > 1 ? "Paste " + std::to_string(payload.size()) + " items" : "Paste";
    r.version = ed.finish();
    if (!r.version) return refused(ed.error());
    if (r.version == base) r.version = nullptr;
    return r;
}

} // namespace native::tools
