#include "core/document.h"
#include <cmath>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <set>

using nlohmann::json;

namespace core {

namespace {

Vec3 vec3(const json& j, const std::string& what) {
    if (!j.is_array() || j.size() != 3 || !j[0].is_number() || !j[1].is_number() || !j[2].is_number())
        throw std::runtime_error(what + " must be an array of 3 numbers");
    return Vec3{j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

uint32_t uint32(const json& j, const std::string& what) {
    if (!j.is_number_integer() || j.get<int64_t>() < 0 || j.get<int64_t>() > 0xFFFFFFFFll)
        throw std::runtime_error(what + " must be a non-negative integer");
    return static_cast<uint32_t>(j.get<int64_t>());
}

std::string str(const json& j, const std::string& what) {
    if (!j.is_string()) throw std::runtime_error(what + " must be a string");
    return j.get<std::string>();
}

BrushFace faceFromJson(const json& j) {
    BrushFace f;
    f.id = uint32(j.at("id"), "face id");
    for (const auto& idx : j.at("loop")) f.loop.push_back(uint32(idx, "face loop index"));
    if (j.contains("uvs")) {
        for (const auto& uv : j["uvs"]) {
            if (!uv.is_array() || uv.size() != 2 || !uv[0].is_number() || !uv[1].is_number())
                throw std::runtime_error("face uv must be an array of 2 numbers");
            f.uvs.push_back({uv[0].get<double>(), uv[1].get<double>()});
        }
    }
    if (j.contains("texture")) f.texture = str(j["texture"], "face texture");
    {
        std::string look;
        for (const char* key : {"projection", "uvs", "repeat", "layout"})
            if (j.contains(key)) { look += key; look += '='; look += j[key].dump(); look += ';'; }
        if (!look.empty()) f.lookHash = static_cast<uint64_t>(std::hash<std::string>{}(look)) | 1u;  // never 0 when something is set
        json raw = json::object();
        for (const char* key : {"projection", "repeat", "layout"})
            if (j.contains(key) && !j[key].is_null()) raw[key] = j[key];
        if (!raw.empty()) f.look = std::make_shared<const json>(std::move(raw));
    }
    if (j.contains("lightmapScale") && j["lightmapScale"].is_number()) {  // lenient: a bad value keeps 1
        const double v = j["lightmapScale"].get<double>();
        if (std::isfinite(v) && v > 0) f.lightmapScale = std::clamp(v, kMinLightmapScale, kMaxLightmapScale);
    }
    if (j.contains("projection") && j["projection"].is_object()) {  // lenient like the Shell (projectionOf): bad parts keep the defaults
        const auto& pj = j["projection"];
        auto pair = [](const json& v, double& a, double& b, bool nonZero) {
            if (!v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number()) return;
            const double x = v[0].get<double>(), y = v[1].get<double>();
            if (!std::isfinite(x) || !std::isfinite(y)) return;
            a = nonZero && std::fabs(x) < 1e-6 ? a : x;
            b = nonZero && std::fabs(y) < 1e-6 ? b : y;
        };
        if (pj.contains("scale")) pair(pj["scale"], f.projection.scaleU, f.projection.scaleV, true);
        if (pj.contains("shift")) pair(pj["shift"], f.projection.shiftU, f.projection.shiftV, false);
        if (pj.contains("rotate") && pj["rotate"].is_number() && std::isfinite(pj["rotate"].get<double>())) f.projection.rotate = pj["rotate"].get<double>();
    }
    return f;
}

std::unordered_map<std::string, std::string> stringMap(const json& j, const std::string& what) {
    if (!j.is_object()) throw std::runtime_error(what + " must be an object");
    std::unordered_map<std::string, std::string> out;
    for (const auto& [k, v] : j.items()) out[k] = str(v, what + " value");
    return out;
}

// "refs": every key is one path or a non-empty array of paths
std::unordered_map<std::string, RefTargets> refMap(const json& j, const std::string& what) {
    if (!j.is_object()) throw std::runtime_error(what + " must be an object");
    std::unordered_map<std::string, RefTargets> out;
    for (const auto& [k, v] : j.items()) {
        if (v.is_string()) { out[k] = RefTargets(v.get<std::string>()); continue; }
        if (!v.is_array() || v.empty()) throw std::runtime_error(what + " value must be a path or a non-empty array of paths");
        std::vector<std::string> list;
        for (const auto& p : v) list.push_back(str(p, what + " value"));
        out[k] = RefTargets(std::move(list));
    }
    return out;
}

Entity entityFromJson(const json& j) {
    Entity e;
    e.id = str(j.at("id"), "entity id");
    if (j.contains("classname")) e.classname = str(j["classname"], "entity classname");
    if (j.contains("keyvalues")) e.keyvalues = stringMap(j["keyvalues"], "entity keyvalues");
    if (j.contains("origin")) e.origin = vec3(j["origin"], "entity origin");
    if (j.contains("angles")) e.angles = vec3(j["angles"], "entity angles");
    if (j.contains("refs")) e.refs = refMap(j["refs"], "entity refs");
    if (j.contains("brushes")) {
        for (const auto& bj : j["brushes"]) e.brushes.push_back(brushFromJson(bj));
    }
    return e;
}

EntityOverride overrideFromJson(const json& j) {
    if (!j.is_object()) throw std::runtime_error("prefab override must be an object");
    EntityOverride o;
    // Legacy form: every value is a string -> keyvalue overrides.
    const bool legacy = std::all_of(j.begin(), j.end(), [](const json& v) { return v.is_string(); });
    if (legacy) {
        o.keyvalues = stringMap(j, "prefab override");
        return o;
    }
    for (const auto& [k, v] : j.items()) {
        if (k == "keyvalues") o.keyvalues = stringMap(v, "override keyvalues");
        else if (k == "refs") o.refs = refMap(v, "override refs");
        else if (k == "origin") o.origin = vec3(v, "override origin");
        else if (k == "angles") o.angles = vec3(v, "override angles");
        else if (k == "scale") o.scale = vec3(v, "override scale");
        else if (k == "deleted") {
            if (!v.is_boolean()) throw std::runtime_error("override deleted must be a boolean");
            o.deleted = v.get<bool>();
        }
        // unknown keys are tolerated (Shell-owned data); Core never rewrites the file.
    }
    return o;
}

PrefabInstance prefabInstanceFromJson(const json& j) {
    PrefabInstance p;
    p.id = str(j.at("id"), "prefab instance id");
    p.file = str(j.at("file"), "prefab instance file");
    if (j.contains("name")) p.name = str(j["name"], "prefab instance name");
    if (j.contains("origin")) p.origin = vec3(j["origin"], "prefab instance origin");
    if (j.contains("angles")) p.angles = vec3(j["angles"], "prefab instance angles");
    if (j.contains("scale")) p.scale = vec3(j["scale"], "prefab instance scale");
    if (j.contains("overrides")) {
        if (!j["overrides"].is_object()) throw std::runtime_error("prefab overrides must be an object");
        for (const auto& [target, ov] : j["overrides"].items()) p.overrides[target] = overrideFromJson(ov);
    }
    return p;
}

}  // namespace

Brush brushFromJson(const json& j) {
    Brush b;
    b.id = str(j.at("id"), "brush id");
    const std::string op = j.contains("op") ? str(j["op"], "brush op") : "add";
    if (op != "add" && op != "subtract") throw std::runtime_error("brush op must be \"add\" or \"subtract\"");
    b.op = (op == "subtract") ? BrushOp::Subtract : BrushOp::Add;
    if (j.contains("carve")) {
        const json& c = j["carve"];
        if (!c.is_object()) throw std::runtime_error("brush carve must be an object");
        auto flag = [&](const char* key, bool& out) {
            if (!c.contains(key)) return;
            if (!c[key].is_boolean()) throw std::runtime_error(std::string("carve.") + key + " must be a boolean");
            out = c[key].get<bool>();
        };
        flag("self", b.carve.self);
        flag("structural", b.carve.structural);
        flag("nonStructural", b.carve.nonStructural);
    }
    for (const auto& v : j.at("vertices")) b.vertices.push_back(vec3(v, "brush vertex"));
    for (const auto& f : j.at("faces")) b.faces.push_back(faceFromJson(f));
    // smooth edges are a shading hint: anything malformed is skipped, never an error
    if (j.contains("smooth") && j["smooth"].is_array())
        for (const auto& e : j["smooth"]) {
            if (!e.is_array() || e.size() != 2 || !e[0].is_number_integer() || !e[1].is_number_integer()) continue;
            const int64_t fa = e[0].get<int64_t>(), fb = e[1].get<int64_t>();
            if (fa < 0 || fb < 0 || fa > UINT32_MAX || fb > UINT32_MAX) continue;
            b.smoothEdges.push_back({static_cast<uint32_t>(fa), static_cast<uint32_t>(fb)});
        }
    return b;
}

Entity* Document::findEntity(const Guid& id) {
    auto it = std::find_if(entities.begin(), entities.end(), [&](const Entity& e) { return e.id == id; });
    return it != entities.end() ? &*it : nullptr;
}

const Entity* Document::findEntity(const Guid& id) const {
    auto it = std::find_if(entities.begin(), entities.end(), [&](const Entity& e) { return e.id == id; });
    return it != entities.end() ? &*it : nullptr;
}

Document documentFromJson(const json& j) {
    if (!j.is_object()) throw std::runtime_error("document must be a JSON object");
    Document doc;
    doc.schemaVersion = str(j.at("schemaVersion"), "schemaVersion");
    if (doc.schemaVersion != kSchemaVersion)
        throw std::runtime_error("unsupported schemaVersion \"" + doc.schemaVersion + "\" (expected \"" +
                                 kSchemaVersion + "\")");
    if (j.contains("entities")) {
        for (const auto& ej : j["entities"]) doc.entities.push_back(entityFromJson(ej));
    }
    if (j.contains("prefabInstances")) {
        for (const auto& pj : j["prefabInstances"]) doc.prefabInstances.push_back(prefabInstanceFromJson(pj));
    }
    return doc;
}

namespace {
std::string baseName(const std::string& file) {
    std::string f = file;
    std::replace(f.begin(), f.end(), '\\', '/');
    auto slash = f.find_last_of('/');
    if (slash != std::string::npos) f = f.substr(slash + 1);
    for (const char* ext : {".map.json", ".json"}) {
        const std::string e = ext;
        if (f.size() > e.size() && f.compare(f.size() - e.size(), e.size(), e) == 0) {
            f.resize(f.size() - e.size());
            break;
        }
    }
    return f.empty() ? "prefab" : f;
}

bool badName(const std::string& n) { return n.empty() || n.find('|') != std::string::npos || n.find('/') != std::string::npos; }

std::optional<std::string> badPath(const std::string& what, const std::string& path) {
    if (path.empty()) return what + ": empty path";
    size_t start = 0;
    while (true) {
        auto slash = path.find('/', start);
        std::string seg = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (seg.empty() || seg.find('|') != std::string::npos) return what + ": malformed path '" + path + "'";
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return std::nullopt;
}
}  // namespace

std::vector<std::string> effectiveInstanceNames(const Document& doc) {
    std::set<std::string> taken;
    for (const auto& p : doc.prefabInstances)
        if (!p.name.empty()) taken.insert(p.name);
    std::vector<std::string> out;
    for (const auto& p : doc.prefabInstances) {
        if (!p.name.empty()) {
            out.push_back(p.name);
            continue;
        }
        const std::string base = baseName(p.file);
        std::string cand = base;
        for (int n = 2; taken.count(cand); ++n) cand = base + std::to_string(n);
        taken.insert(cand);
        out.push_back(cand);
    }
    return out;
}

std::optional<std::string> validateDocument(const Document& doc) {
    std::set<std::string> entityIds, brushIds, instanceIds, instanceNames;

    for (const auto& e : doc.entities) {
        if (e.id.empty()) return "entity with an empty id";
        if (!entityIds.insert(e.id).second) return "duplicate entity id '" + e.id + "'";
        for (const auto& [k, v] : e.refs)
            for (const auto& path : v.paths)
                if (auto err = badPath("entity '" + e.id + "' ref '" + k + "'", path)) return err;

        for (const auto& b : e.brushes) {
            const std::string where = "entity '" + e.id + "' brush '" + b.id + "'";
            if (b.id.empty()) return "entity '" + e.id + "' has a brush with an empty id";
            if (!brushIds.insert(b.id).second) return where + ": duplicate brush id";
            if (auto err = b.checkStructure()) return where + ": " + *err;
            // the convex rule is for SOLIDS: a subtractive brush is never compiled as a solid, so it may be concave anywhere
            if (EntityClassRegistry::requiresConvexBrushes(e.classname) && b.op != BrushOp::Subtract) {
                if (auto err = b.checkConvexSolid())
                    return where + ": " + e.classname + " brushes must be convex closed solids, but " + *err;
            }
        }
    }
    for (const auto& p : doc.prefabInstances) {
        if (p.id.empty()) return "prefab instance with an empty id";
        if (!instanceIds.insert(p.id).second) return "duplicate prefab instance id '" + p.id + "'";
        if (p.file.empty()) return "prefab instance '" + p.id + "' has an empty file";
        if (!p.name.empty() && badName(p.name))
            return "prefab instance '" + p.id + "': name '" + p.name + "' may not contain '|' or '/'";
        if (!p.name.empty() && !instanceNames.insert(p.name).second)
            return "two prefab instances are named '" + p.name + "'";
        for (const auto& [path, ov] : p.overrides) {
            const std::string where = "prefab instance '" + p.id + "' override '" + path + "'";
            if (auto err = badPath(where, path)) return err;
            for (const auto& [k, v] : ov.refs)
                for (const auto& rp : v.paths)
                    if (auto err = badPath(where + " ref '" + k + "'", rp)) return err;
        }
    }
    return std::nullopt;
}

}  // namespace core
