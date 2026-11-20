// tools/entity_edit -- see entity_edit.h.
#include "tools/entity_edit.h"

#include "core/entity_class.h"

#include <algorithm>
#include <cmath>

#include "doc/edit.h"
#include "tools/box_tool.h"
#include "tools/properties.h"

namespace native::tools {

namespace {

EditResult refused(std::string why) {
    EditResult r;
    r.error = std::move(why);
    return r;
}

EditResult done(doc::VersionEditor& ed, std::string label, std::vector<doc::SelRef> select = {}) {
    EditResult r;
    r.label = std::move(label);
    if (!ed.changed()) return r;   // nothing to do
    r.version = ed.finish();
    if (!r.version) r.error = ed.error();
    r.select = std::move(select);
    return r;
}

double axisOf(const core::Vec3& v, int a) { return a == 0 ? v.x : a == 1 ? v.y : v.z; }
void setAxis(core::Vec3& v, int a, double x) { (a == 0 ? v.x : a == 1 ? v.y : v.z) = x; }

const doc::EntityNode* entityNode(const doc::DocumentVersion& d, const std::string& id) {
    const int i = d.findEntity(id);
    return i < 0 ? nullptr : d.entities[static_cast<size_t>(i)].get();
}

const doc::InstanceNode* instanceNode(const doc::DocumentVersion& d, const std::string& id) {
    for (const auto& p : d.instances)
        if (p->typed.id == id) return p.get();
    return nullptr;
}

// Edits the typed entity (no brushes) and replaces it; nothing changes when `fn` returns false.
template <class Fn>
EditResult editEntity(const BasePtr& base, const std::string& id, std::string label, Fn fn) {
    if (!base) return refused("no document");
    const doc::EntityNode* n = entityNode(*base, id);
    if (!n) return refused("unknown entity " + id);
    core::Entity e = n->typed;
    if (!fn(e)) return {};
    doc::VersionEditor ed(base);
    ed.replaceEntity(e);
    return done(ed, std::move(label));
}

template <class Fn>
EditResult editInstance(const BasePtr& base, const std::string& id, std::string label, Fn fn) {
    if (!base) return refused("no document");
    const doc::InstanceNode* n = instanceNode(*base, id);
    if (!n) return refused("unknown prefab instance " + id);
    core::PrefabInstance p = n->typed;
    if (!fn(p)) return {};
    doc::VersionEditor ed(base);
    ed.replaceInstance(p);
    return done(ed, std::move(label));
}

bool emptyOverride(const core::EntityOverride& o) { return !o.deleted && !o.origin && !o.angles && !o.scale && o.keyvalues.empty() && o.refs.empty(); }

// Edits (creates / removes) the override of `path`; empty overrides disappear.
template <class Fn>
EditResult editOverride(const BasePtr& base, const std::string& instanceId, const std::string& path, std::string label, Fn fn) {
    return editInstance(base, instanceId, std::move(label), [&](core::PrefabInstance& p) {
        const auto it = p.overrides.find(path);
        core::EntityOverride o = it == p.overrides.end() ? core::EntityOverride{} : it->second;
        if (!fn(o)) return false;
        if (emptyOverride(o)) p.overrides.erase(path);
        else p.overrides[path] = o;
        return true;
    });
}

std::string join3(const std::array<double, 3>& v) { return formatNumber(v[0]) + " " + formatNumber(v[1]) + " " + formatNumber(v[2]); }

} // namespace

EditResult setKeyvalue(const BasePtr& base, const std::string& entityId, const std::string& key, const std::optional<std::string>& value) {
    if (key.empty()) return {};
    return editEntity(base, entityId, value ? "Set '" + key + "'" : "Remove '" + key + "'", [&](core::Entity& e) {
        const auto it = e.keyvalues.find(key);
        if (!value) {
            if (it == e.keyvalues.end()) return false;
            e.keyvalues.erase(it);
            return true;
        }
        if (it != e.keyvalues.end() && it->second == *value) return false;
        e.keyvalues[key] = *value;
        return true;
    });
}

EditResult setClassname(const BasePtr& base, const std::string& entityId, const std::string& classname) {
    if (!base || classname.empty()) return {};
    const doc::EntityNode* n = entityNode(*base, entityId);
    if (!n) return refused("unknown entity " + entityId);
    if (n->typed.classname == classname) return {};
    if (core::EntityClassRegistry::requiresConvexBrushes(classname)) {
        for (const auto& b : n->brushes)
            if (const auto problem = b->brush.checkConvexSolid())
                return refused(classname + " needs convex closed solids, but brush '" + b->brush.id + "': " + *problem);
    }
    return editEntity(base, entityId, "Change classname", [&](core::Entity& e) {
        e.classname = classname;
        return true;
    });
}

EditResult setOrigin(const BasePtr& base, const std::string& entityId, int axis, double value) {
    if (!std::isfinite(value) || axis < 0 || axis > 2) return {};
    return editEntity(base, entityId, "Set origin", [&](core::Entity& e) {
        if (axisOf(e.origin, axis) == value) return false;
        setAxis(e.origin, axis, value);
        return true;
    });
}

EditResult setAngle(const BasePtr& base, const std::string& entityId, int axis, double value) {
    if (!std::isfinite(value) || axis < 0 || axis > 2) return {};
    return editEntity(base, entityId, "Set angles", [&](core::Entity& e) {
        if (axisOf(e.angles, axis) == value) return false;
        setAxis(e.angles, axis, value);
        return true;
    });
}

EditResult setScale(const BasePtr& base, const std::string& entityId, int axis, double value) {
    if (!std::isfinite(value) || value == 0) return refused("Scale cannot be 0");
    if (axis < 0 || axis > 2) return {};
    return editEntity(base, entityId, "Set scale", [&](core::Entity& e) {
        const auto it = e.keyvalues.find("modelscale_vec");
        std::array<double, 3> cur = parseScaleVec(it == e.keyvalues.end() ? std::string() : it->second);
        if (cur[static_cast<size_t>(axis)] == value) return false;
        cur[static_cast<size_t>(axis)] = value;
        if (cur[0] == 1 && cur[1] == 1 && cur[2] == 1) e.keyvalues.erase("modelscale_vec");
        else e.keyvalues["modelscale_vec"] = join3(cur);
        return true;
    });
}

EditResult setRef(const BasePtr& base, const std::string& entityId, const std::string& key, const std::vector<std::string>& paths) {
    return editEntity(base, entityId, paths.empty() ? "Clear " + key : "Set " + key, [&](core::Entity& e) {
        const auto it = e.refs.find(key);
        if (paths.empty()) {
            if (it == e.refs.end()) return false;
            e.refs.erase(it);
            return true;
        }
        if (it != e.refs.end() && it->second.paths == paths) return false;
        e.refs[key] = core::RefTargets(paths);
        return true;
    });
}

EditResult shareTargetName(const BasePtr& base, const std::vector<std::string>& paths) {
    if (!base || paths.empty()) return {};
    const doc::EntityNode* first = entityNode(*base, paths[0]);
    if (!first) return {};
    const auto it = first->typed.keyvalues.find("targetname");
    if (it == first->typed.keyvalues.end() || it->second.empty()) return {};
    const std::string name = it->second;
    doc::VersionEditor ed(base);
    for (size_t i = 1; i < paths.size(); ++i) {
        const doc::EntityNode* n = entityNode(*base, paths[i]);
        if (!n) continue;   // a target inside a prefab instance
        const auto t = n->typed.keyvalues.find("targetname");
        if (t != n->typed.keyvalues.end() && t->second == name) continue;
        core::Entity e = n->typed;
        e.keyvalues["targetname"] = name;
        ed.replaceEntity(e);
    }
    return done(ed, "Share name");
}

EditResult setBrushOp(const BasePtr& base, const std::string& entityId, const std::string& brushId, bool subtract) {
    if (!base) return refused("no document");
    const int ei = base->findEntity(entityId), bi = base->findBrush(entityId, brushId);
    if (ei < 0 || bi < 0) return refused("unknown brush " + brushId);
    core::Brush b = base->entities[static_cast<size_t>(ei)]->brushes[static_cast<size_t>(bi)]->brush;
    const core::BrushOp op = subtract ? core::BrushOp::Subtract : core::BrushOp::Add;
    if (b.op == op) return {};
    b.op = op;
    doc::VersionEditor ed(base);
    ed.replaceBrush(entityId, b);
    return done(ed, subtract ? "Brush -> subtract" : "Brush -> add");
}

EditResult setCarve(const BasePtr& base, const std::string& entityId, const std::string& brushId, CarveKey key, bool value) {
    if (!base) return refused("no document");
    const int ei = base->findEntity(entityId), bi = base->findBrush(entityId, brushId);
    if (ei < 0 || bi < 0) return refused("unknown brush " + brushId);
    core::Brush b = base->entities[static_cast<size_t>(ei)]->brushes[static_cast<size_t>(bi)]->brush;
    bool& flag = key == CarveKey::Self ? b.carve.self : key == CarveKey::Structural ? b.carve.structural : b.carve.nonStructural;
    if (flag == value) return {};
    flag = value;
    doc::VersionEditor ed(base);
    ed.replaceBrush(entityId, b);
    return done(ed, key == CarveKey::Self ? "Carve self" : key == CarveKey::Structural ? "Carve structural" : "Carve nonStructural");
}

std::optional<std::array<double, 3>> defaultLightAngles(const std::string& classname) {
    if (classname == "light_spot" || classname == "light_directional" || classname == "light_rect") return std::array<double, 3>{90, 0, 0};
    return std::nullopt;
}

EditResult addPointEntity(const BasePtr& base, const std::string& classname, const core::Vec3& origin, const std::vector<std::pair<std::string, std::string>>& keyvalues,
                          const std::optional<core::Vec3>& angles) {
    if (!base) return refused("no document");
    core::Entity e;
    e.id = newId("e");
    e.classname = classname;
    e.origin = origin;
    for (const auto& [k, v] : keyvalues) e.keyvalues[k] = v;
    if (angles) e.angles = *angles;
    else if (const auto a = defaultLightAngles(classname)) e.angles = {(*a)[0], (*a)[1], (*a)[2]};
    doc::VersionEditor ed(base);
    ed.addEntity(e);
    return done(ed, "Add " + classname, {doc::SelRef::entity(e.id)});
}

EditResult spawnInstance(const BasePtr& base, const std::string& file, const core::Vec3& origin) {
    if (!base) return refused("no document");
    core::PrefabInstance p;
    p.id = newId("pi");
    p.name = newInstanceName(*base, file);
    p.file = file;
    p.origin = origin;
    doc::VersionEditor ed(base);
    ed.addInstance(p);
    return done(ed, "Add prefab", {doc::SelRef::instance(p.id)});
}

EditResult renameInstance(const BasePtr& base, const std::string& instanceId, const std::string& rawName) {
    if (!base) return refused("no document");
    std::string name = rawName;
    const size_t a = name.find_first_not_of(" \t"), b = name.find_last_not_of(" \t");
    name = a == std::string::npos ? std::string() : name.substr(a, b - a + 1);
    if (name.empty()) return refused("Rename: a name cannot be empty");
    if (name.find_first_of("|/") != std::string::npos) return refused("Rename: a name may not contain '|' or '/'");
    const std::vector<std::string> names = instanceDisplayNames(*base);
    for (size_t i = 0; i < base->instances.size(); ++i)
        if (base->instances[i]->typed.id != instanceId && names[i] == name) return refused("Rename: another instance is already called '" + name + "'");
    return editInstance(base, instanceId, "Rename prefab instance", [&](core::PrefabInstance& p) {
        if (p.name == name) return false;
        p.name = name;
        return true;
    });
}

EditResult setInstanceVec(const BasePtr& base, const std::string& instanceId, InstanceVec field, int axis, double value) {
    if (!std::isfinite(value) || axis < 0 || axis > 2) return {};
    if (field == InstanceVec::Scale && value == 0) return refused("Scale cannot be 0");
    const char* names[] = {"origin", "angles", "scale"};
    return editInstance(base, instanceId, std::string("Set instance ") + names[static_cast<int>(field)], [&](core::PrefabInstance& p) {
        core::Vec3 cur = field == InstanceVec::Origin ? p.origin : field == InstanceVec::Angles ? p.angles : p.scale.value_or(core::Vec3{1, 1, 1});
        if (axisOf(cur, axis) == value) return false;
        setAxis(cur, axis, value);
        if (field == InstanceVec::Origin) p.origin = cur;
        else if (field == InstanceVec::Angles) p.angles = cur;
        else p.scale = cur;
        return true;
    });
}

EditResult setOverrideKeyvalue(const BasePtr& base, const std::string& instanceId, const std::string& path, const std::string& key, const std::optional<std::string>& value) {
    return editOverride(base, instanceId, path, value ? "Override keyvalue" : "Revert override", [&](core::EntityOverride& o) {
        const auto it = o.keyvalues.find(key);
        if (!value) {
            if (it == o.keyvalues.end()) return false;
            o.keyvalues.erase(it);
            return true;
        }
        if (it != o.keyvalues.end() && it->second == *value) return false;
        o.keyvalues[key] = *value;
        return true;
    });
}

EditResult setOverrideField(const BasePtr& base, const std::string& instanceId, const std::string& path, OverrideField field, const std::optional<std::array<double, 3>>& value) {
    return editOverride(base, instanceId, path, value ? "Override entity" : "Revert override", [&](core::EntityOverride& o) {
        const auto v = value ? std::optional<core::Vec3>(core::Vec3{(*value)[0], (*value)[1], (*value)[2]}) : std::nullopt;
        switch (field) {
            case OverrideField::Origin: if (o.origin.has_value() == v.has_value() && (!v || (o.origin->x == v->x && o.origin->y == v->y && o.origin->z == v->z))) return false; o.origin = v; return true;
            case OverrideField::Angles: if (o.angles.has_value() == v.has_value() && (!v || (o.angles->x == v->x && o.angles->y == v->y && o.angles->z == v->z))) return false; o.angles = v; return true;
            case OverrideField::Scale: if (o.scale.has_value() == v.has_value() && (!v || (o.scale->x == v->x && o.scale->y == v->y && o.scale->z == v->z))) return false; o.scale = v; return true;
            case OverrideField::Deleted: {
                const bool on = value.has_value();
                if (o.deleted == on) return false;
                o.deleted = on;
                return true;
            }
        }
        return false;
    });
}

EditResult setOverrideRef(const BasePtr& base, const std::string& instanceId, const std::string& path, const std::string& key, const std::vector<std::string>& paths) {
    return editOverride(base, instanceId, path, paths.empty() ? "Revert reference" : "Override " + key, [&](core::EntityOverride& o) {
        const auto it = o.refs.find(key);
        if (paths.empty()) {
            if (it == o.refs.end()) return false;
            o.refs.erase(it);
            return true;
        }
        if (it != o.refs.end() && it->second.paths == paths) return false;
        o.refs[key] = core::RefTargets(paths);
        return true;
    });
}

EditResult revertOverride(const BasePtr& base, const std::string& instanceId, const std::string& path) {
    return editInstance(base, instanceId, "Revert overrides", [&](core::PrefabInstance& p) {
        if (!p.overrides.count(path)) return false;
        p.overrides.erase(path);
        return true;
    });
}

} // namespace native::tools
