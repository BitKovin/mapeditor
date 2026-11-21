// tools/prefabize -- see prefabize.h.
#include "tools/prefabize.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

#include "doc/edit.h"
#include "doc/json_write.h"
#include "tools/parametric.h"

namespace native::tools {

namespace {

using Json = doc::Json;

double r3(double n) { return std::round(n * 1000) / 1000; }

// refs of one entity after the move: only the targets that are part of the prefab stay.
void keepRefsTo(core::Entity& e, const std::set<std::string>& ids) {
    for (auto it = e.refs.begin(); it != e.refs.end();) {
        std::vector<std::string> inside;
        for (const std::string& p : it->second.paths)
            if (ids.count(p)) inside.push_back(p);
        if (inside.empty()) it = e.refs.erase(it);
        else {
            it->second.paths = std::move(inside);
            ++it;
        }
    }
}

} // namespace

std::optional<PrefabExtract> extractPrefab(const doc::DocumentVersion& d, const std::vector<doc::SelRef>& sels) {
    if (sels.empty()) return std::nullopt;
    const auto c = selectionPivot(d, sels);
    if (!c) return std::nullopt;
    const V3 pivot{r3((*c)[0]), r3((*c)[1]), r3((*c)[2])};
    const Affine back = translateBy({-pivot[0], -pivot[1], -pivot[2]});

    const Targets t = selectionTargets(sels);
    std::set<std::string> ids;   // the entities that go into the file (their ids stay, so references between them keep working)
    for (const auto& node : d.entities) {
        const std::string& id = node->typed.id;
        if (t.entities.count(id) || t.brushes.count(id)) ids.insert(id);
    }

    Json entities = Json::array();
    for (const auto& node : d.entities) {
        const std::string& id = node->typed.id;
        if (!ids.count(id)) continue;
        const bool all = t.entities.count(id) > 0;
        const auto picked = t.brushes.find(id);
        Json brushes = Json::array();
        for (const auto& b : node->brushes) {
            if (!all && !picked->second.count(b->brush.id)) continue;
            Json raw = doc::writeBrush(mapBrushPoints(b->brush, back, false, true), b->raw);
            if (b->raw.is_object() && b->raw.contains("shape")) {
                raw["shape"] = parametric::mapShapeRecord(b->raw["shape"], [&back](const Vec3& p) {
                    const V3 q = back({p.x, p.y, p.z});
                    return Vec3{q[0], q[1], q[2]};
                });
            }
            brushes.push_back(std::move(raw));
        }
        core::Entity typed = node->typed;
        keepRefsTo(typed, ids);
        const bool world = typed.classname == "worldspawn";
        if (!world) typed.origin = core::Vec3(r3(typed.origin.x - pivot[0]), r3(typed.origin.y - pivot[1]), r3(typed.origin.z - pivot[2]));
        Json out = doc::writeEntity(typed, node->raw);
        if (node->hasBrushesKey || !brushes.empty()) out["brushes"] = std::move(brushes);
        else out.erase("brushes");
        entities.push_back(std::move(out));
    }

    Json instances = Json::array();
    for (const auto& inst : d.instances) {
        if (!t.instances.count(inst->typed.id)) continue;
        core::PrefabInstance typed = inst->typed;
        typed.origin = core::Vec3(r3(typed.origin.x - pivot[0]), r3(typed.origin.y - pivot[1]), r3(typed.origin.z - pivot[2]));
        instances.push_back(doc::writeInstance(typed, inst->raw));
    }
    if (entities.empty() && instances.empty()) return std::nullopt;

    PrefabExtract out;
    out.pivot = pivot;
    out.file = d.rootRaw.is_object() ? d.rootRaw : Json::object();
    if (!out.file.contains("schemaVersion")) out.file["schemaVersion"] = core::kSchemaVersion;
    out.file["entities"] = std::move(entities);
    out.file["prefabInstances"] = std::move(instances);
    return out;
}

EditResult replaceWithInstance(const std::shared_ptr<const doc::DocumentVersion>& base, const std::vector<doc::SelRef>& sels, const std::string& file, const V3& pivot) {
    EditResult r;
    if (!base) return r;
    const Targets t = selectionTargets(sels);
    doc::VersionEditor ed(base);
    for (const std::string& id : t.instances) ed.removeInstance(id);
    for (const auto& [entityId, brushIds] : t.brushes)
        for (const std::string& b : brushIds) ed.removeBrush(entityId, b);
    for (const std::string& id : t.entities) {
        const int i = base->findEntity(id);
        if (i < 0) continue;
        const auto& node = base->entities[static_cast<size_t>(i)];
        if (node->typed.classname == "worldspawn") {   // the world stays, emptied of its brushes
            for (const auto& b : node->brushes) ed.removeBrush(id, b->brush.id);
        } else {
            ed.removeEntity(id);
        }
    }
    const std::shared_ptr<const doc::DocumentVersion> without = ed.finish();
    if (!without) {
        r.error = ed.error();
        return r;
    }
    core::PrefabInstance inst;
    inst.id = newId("pi");
    inst.name = newInstanceName(*without, file);
    inst.file = file;
    inst.origin = core::Vec3(pivot[0], pivot[1], pivot[2]);
    doc::VersionEditor add(without);
    if (!add.addInstance(inst)) {
        r.error = add.error();
        return r;
    }
    r.version = add.finish();
    if (!r.version) {
        r.error = add.error();
        return r;
    }
    r.select = {doc::SelRef::instance(inst.id)};
    r.label = "Turn into prefab";
    return r;
}

std::string prefabFileName(const std::string& prefabRoot, const std::string& typed, std::string& error) {
    error.clear();
    std::string name;
    for (char ch : typed) name += ch == '\\' ? '/' : ch;
    const auto trim = [](std::string& s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    };
    trim(name);
    if (name.empty()) {
        error = "Give the prefab a name";
        return {};
    }
    if (name.front() == '/' || (name.size() > 1 && name[1] == ':')) {
        error = "The prefab goes under the prefabs folder: give a name, not a full path";
        return {};
    }
    for (char ch : name)
        if (ch == '"' || ch == '*' || ch == '?' || ch == '<' || ch == '>' || ch == '|' || ch == ':' || static_cast<unsigned char>(ch) < 32) {
            error = "A prefab name may not contain  \" * ? < > | :";
            return {};
        }
    size_t start = 0;
    while (start <= name.size()) {
        const size_t end = name.find('/', start);
        const std::string part = name.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part == ".." || part == "." || part.empty()) {
            error = "That prefab name has an empty or relative folder";
            return {};
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    const auto ends = [&](const char* suffix) {
        const std::string s = suffix;
        return name.size() >= s.size() && name.compare(name.size() - s.size(), s.size(), s) == 0;
    };
    if (!ends(".json")) name += ".map.json";
    std::string root = prefabRoot;
    while (!root.empty() && root.back() == '/') root.pop_back();
    if (!root.empty() && name.rfind(root + "/", 0) != 0) name = root + "/" + name;
    return name;
}

} // namespace native::tools
