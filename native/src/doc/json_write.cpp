// doc/json_write -- see json_write.h.
#include "doc/json_write.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

namespace native::doc {

using Json = nlohmann::json;

namespace {

Json vec3Json(const core::Vec3& v) { return Json::array({numberJson(v.x), numberJson(v.y), numberJson(v.z)}); }

Json pairJson(double a, double b) { return Json::array({numberJson(a), numberJson(b)}); }

Json loopJson(const std::vector<uint32_t>& loop) {
    Json a = Json::array();
    for (uint32_t i : loop) a.push_back(i);
    return a;
}

Json uvsJson(const std::vector<core::Vec2>& uvs) {
    Json a = Json::array();
    for (const core::Vec2& uv : uvs) a.push_back(pairJson(uv.u, uv.v));
    return a;
}

Json projectionJson(const core::FaceProjection& p) {
    return Json{{"scale", pairJson(p.scaleU, p.scaleV)}, {"shift", pairJson(p.shiftU, p.shiftV)}, {"rotate", numberJson(p.rotate)}};
}

Json carveJson(const core::CarveScope& c) { return Json{{"self", c.self}, {"structural", c.structural}, {"nonStructural", c.nonStructural}}; }

Json smoothJson(const std::vector<std::pair<uint32_t, uint32_t>>& edges) {
    Json a = Json::array();
    for (const auto& e : edges) a.push_back(Json::array({e.first, e.second}));
    return a;
}

Json refJson(const core::RefTargets& r) {
    // The stored form (shell refValueOf): the plain string for one path, an array for several; duplicates dropped.
    std::vector<std::string> list;
    for (const std::string& p : r.paths)
        if (std::find(list.begin(), list.end(), p) == list.end()) list.push_back(p);
    if (list.size() == 1) return list[0];
    Json a = Json::array();
    for (const std::string& p : list) a.push_back(p);
    return a;
}

Json stringMapJson(const std::unordered_map<std::string, std::string>& m) {
    Json o = Json::object();
    for (const auto& [k, v] : m) o[k] = v;
    return o;
}

Json refsJson(const std::unordered_map<std::string, core::RefTargets>& m) {
    Json o = Json::object();
    for (const auto& [k, v] : m)
        if (!v.paths.empty()) o[k] = refJson(v);
    return o;
}

Json overrideJson(const core::EntityOverride& o) {
    Json j = Json::object();
    if (!o.keyvalues.empty()) j["keyvalues"] = stringMapJson(o.keyvalues);
    const Json refs = refsJson(o.refs);
    if (!refs.empty()) j["refs"] = refs;
    if (o.origin) j["origin"] = vec3Json(*o.origin);
    if (o.angles) j["angles"] = vec3Json(*o.angles);
    if (o.scale) j["scale"] = vec3Json(*o.scale);
    if (o.deleted) j["deleted"] = true;
    return j;
}

// Replaces / removes one key: an empty value is written only when `keepEmpty`, else the key goes away.
void setOrErase(Json& raw, const char* key, Json value, bool erase) {
    if (erase) raw.erase(key);
    else raw[key] = std::move(value);
}

// Patches an object-valued key (keyvalues, refs) so only the changed members are touched. `now` / `before` are the canonical JSON of
// the typed maps; before == null means there was no old value.
void patchObject(Json& raw, const char* key, const Json& now, const Json* before) {
    if (before && now == *before) return;
    if (now.empty()) {
        raw.erase(key);   // the last member went away: so does the (now useless) key
        return;
    }
    Json obj = (before && raw.contains(key) && raw[key].is_object()) ? raw[key] : Json::object();
    if (before)
        for (const auto& [k, v] : before->items())
            if (!now.contains(k)) obj.erase(k);
    for (const auto& [k, v] : now.items())
        if (!obj.contains(k) || obj[k] != v) obj[k] = v;
    raw[key] = std::move(obj);
}

void patchVec3(Json& raw, const char* key, const core::Vec3& now, const core::Vec3* before, bool alwaysWrite = false) {
    const Json n = vec3Json(now);
    if (before) {
        if (n != vec3Json(*before)) raw[key] = n;
    } else if (alwaysWrite || now.x != 0 || now.y != 0 || now.z != 0) {
        raw[key] = n;
    }
}

void patchOptionalVec3(Json& raw, const char* key, const std::optional<core::Vec3>& now, const std::optional<core::Vec3>* before) {
    const Json n = now ? vec3Json(*now) : Json(nullptr);
    const Json b = before && *before ? vec3Json(**before) : Json(nullptr);
    if (before && n == b) return;
    if (now) raw[key] = n;
    else raw.erase(key);
}

// ---- faces -----------------------------------------------------------------------------------------------------------------------

// The `projection`, `repeat` and `layout` keys the typed face keeps verbatim in `look`.
void patchLook(Json& raw, const nlohmann::json* now, const nlohmann::json* before) {
    for (const char* key : {"projection", "repeat", "layout"}) {
        const bool nowHas = now && now->is_object() && now->contains(key);
        const bool beforeHas = before && before->is_object() && before->contains(key);
        if (nowHas) {
            if (!beforeHas || (*now)[key] != (*before)[key]) raw[key] = (*now)[key];
        } else if (beforeHas) {
            raw.erase(key);
        }
    }
}

Json writeFace(const core::BrushFace& f, const Json& oldRaw, const core::BrushFace* old) {
    Json raw = old && oldRaw.is_object() ? oldRaw : Json::object();
    if (!old || f.id != old->id) raw["id"] = f.id;
    if (!old || loopJson(f.loop) != loopJson(old->loop)) raw["loop"] = loopJson(f.loop);
    if (!old || f.texture != old->texture) raw["texture"] = f.texture;
    if (!old || uvsJson(f.uvs) != uvsJson(old->uvs)) setOrErase(raw, "uvs", uvsJson(f.uvs), f.uvs.empty());
    if (!old || f.lightmapScale != old->lightmapScale) setOrErase(raw, "lightmapScale", numberJson(f.lightmapScale), f.lightmapScale == 1);
    // `look` holds the raw projection / repeat / layout; the typed projection (scale, shift, rotate) is patched into the projection
    // object on top of it, member by member, so a kind / basis / shear the typed view does not model stays.
    patchLook(raw, f.look.get(), old ? old->look.get() : nullptr);
    const core::FaceProjection def;
    const core::FaceProjection& before = old ? old->projection : def;
    if (projectionJson(f.projection) != projectionJson(before)) {
        Json pj = raw.contains("projection") && raw["projection"].is_object() ? raw["projection"] : Json::object();
        if (f.projection.scaleU != before.scaleU || f.projection.scaleV != before.scaleV) pj["scale"] = pairJson(f.projection.scaleU, f.projection.scaleV);
        if (f.projection.shiftU != before.shiftU || f.projection.shiftV != before.shiftV) pj["shift"] = pairJson(f.projection.shiftU, f.projection.shiftV);
        if (f.projection.rotate != before.rotate) pj["rotate"] = numberJson(f.projection.rotate);
        raw["projection"] = std::move(pj);
    }
    return raw;
}

} // namespace

Json numberJson(double value) {
    if (std::isfinite(value) && value == std::floor(value) && std::fabs(value) < 9e15) return static_cast<int64_t>(value);
    return value;
}

// ---- brush -----------------------------------------------------------------------------------------------------------------------

Json writeBrush(const core::Brush& b, const Json& oldRaw) {
    std::optional<core::Brush> old;
    if (oldRaw.is_object()) {
        try {
            old = core::brushFromJson(oldRaw);
        } catch (const std::exception&) {
            old.reset();   // not a brush we can read: written from scratch
        }
    }
    Json raw = old ? oldRaw : Json::object();
    if (!old || b.id != old->id) raw["id"] = b.id;
    if (!old ? b.op == core::BrushOp::Subtract : b.op != old->op) setOrErase(raw, "op", "subtract", b.op == core::BrushOp::Add);

    // carve: only the flags that changed are written into the carve object
    {
        const core::CarveScope def;
        const core::CarveScope& before = old ? old->carve : def;
        if (carveJson(b.carve) != carveJson(before)) {
            Json c = raw.contains("carve") && raw["carve"].is_object() ? raw["carve"] : Json::object();
            if (b.carve.self != before.self) c["self"] = b.carve.self;
            if (b.carve.structural != before.structural) c["structural"] = b.carve.structural;
            if (b.carve.nonStructural != before.nonStructural) c["nonStructural"] = b.carve.nonStructural;
            raw["carve"] = std::move(c);
        }
    }

    // vertices: with the same count only the moved ones are written (the output differs from the input exactly there)
    {
        const bool same = old && old->vertices.size() == b.vertices.size() && raw.contains("vertices") && raw["vertices"].is_array() &&
                          raw["vertices"].size() == b.vertices.size();
        if (same) {
            for (size_t i = 0; i < b.vertices.size(); ++i) {
                const Json n = vec3Json(b.vertices[i]);
                if (n != vec3Json(old->vertices[i])) raw["vertices"][i] = n;
            }
        } else {
            Json a = Json::array();
            for (const core::Vec3& v : b.vertices) a.push_back(vec3Json(v));
            raw["vertices"] = std::move(a);
        }
    }

    // faces: matched by id; a surviving face is patched, a new one is written from scratch, removed ones are dropped
    {
        std::map<uint32_t, size_t> oldIndex;
        const bool rawFaces = old && raw.contains("faces") && raw["faces"].is_array() && raw["faces"].size() == old->faces.size();
        if (rawFaces)
            for (size_t i = 0; i < old->faces.size(); ++i) oldIndex.emplace(old->faces[i].id, i);
        Json faces = Json::array();
        for (const core::BrushFace& f : b.faces) {
            auto it = oldIndex.find(f.id);
            if (it == oldIndex.end()) faces.push_back(writeFace(f, Json(), nullptr));
            else faces.push_back(writeFace(f, raw["faces"][it->second], &old->faces[it->second]));
        }
        raw["faces"] = std::move(faces);
    }

    // smooth edges
    if (!old || smoothJson(b.smoothEdges) != smoothJson(old->smoothEdges)) setOrErase(raw, "smooth", smoothJson(b.smoothEdges), b.smoothEdges.empty());
    return raw;
}

// ---- entity ----------------------------------------------------------------------------------------------------------------------

Json writeEntity(const core::Entity& e, const Json& oldRaw) {
    std::optional<core::Entity> old;
    if (oldRaw.is_object()) {
        try {
            Json noBrushes = oldRaw;
            noBrushes.erase("brushes");
            old = core::entityFromJson(noBrushes);
        } catch (const std::exception&) {
            old.reset();
        }
    }
    Json raw = old ? oldRaw : Json::object();
    if (!old || e.id != old->id) raw["id"] = e.id;
    if (!old || e.classname != old->classname) raw["classname"] = e.classname;
    {
        const Json now = stringMapJson(e.keyvalues);
        const Json before = old ? stringMapJson(old->keyvalues) : Json();
        patchObject(raw, "keyvalues", now, old ? &before : nullptr);
    }
    {
        const Json now = refsJson(e.refs);
        const Json before = old ? refsJson(old->refs) : Json();
        patchObject(raw, "refs", now, old ? &before : nullptr);
    }
    patchVec3(raw, "origin", e.origin, old ? &old->origin : nullptr);
    patchVec3(raw, "angles", e.angles, old ? &old->angles : nullptr);
    return raw;
}

// ---- prefab instance -------------------------------------------------------------------------------------------------------------

namespace {

Json writeOverride(const core::EntityOverride& o, const Json& oldRaw, const core::EntityOverride* old) {
    // The legacy flat form (every value a string) is replaced by the structured one as soon as the override is written.
    const bool structured = oldRaw.is_object() && !std::all_of(oldRaw.begin(), oldRaw.end(), [](const Json& v) { return v.is_string(); });
    if (!old || !structured) return overrideJson(o);
    Json raw = oldRaw;
    {
        const Json now = stringMapJson(o.keyvalues), before = stringMapJson(old->keyvalues);
        patchObject(raw, "keyvalues", now, &before);
    }
    {
        const Json now = refsJson(o.refs), before = refsJson(old->refs);
        patchObject(raw, "refs", now, &before);
    }
    patchOptionalVec3(raw, "origin", o.origin, &old->origin);
    patchOptionalVec3(raw, "angles", o.angles, &old->angles);
    patchOptionalVec3(raw, "scale", o.scale, &old->scale);
    if (o.deleted != old->deleted) setOrErase(raw, "deleted", true, !o.deleted);
    return raw;
}

} // namespace

Json writeInstance(const core::PrefabInstance& p, const Json& oldRaw) {
    std::optional<core::PrefabInstance> old;
    if (oldRaw.is_object()) {
        try {
            old = core::instanceFromJson(oldRaw);
        } catch (const std::exception&) {
            old.reset();
        }
    }
    Json raw = old ? oldRaw : Json::object();
    if (!old || p.id != old->id) raw["id"] = p.id;
    if (!old || p.file != old->file) raw["file"] = p.file;
    if (!old || p.name != old->name) setOrErase(raw, "name", p.name, p.name.empty());
    patchVec3(raw, "origin", p.origin, old ? &old->origin : nullptr);
    patchVec3(raw, "angles", p.angles, old ? &old->angles : nullptr);
    {
        const std::optional<core::Vec3>* before = old ? &old->scale : nullptr;
        patchOptionalVec3(raw, "scale", p.scale, before);
    }

    // overrides: untouched entries stay as they were, a changed one is patched, a new one is written, a removed one is dropped
    bool changed = !old || p.overrides.size() != old->overrides.size();
    if (old && !changed)
        for (const auto& [key, o] : p.overrides) {
            auto it = old->overrides.find(key);
            if (it == old->overrides.end() || overrideJson(o) != overrideJson(it->second)) {
                changed = true;
                break;
            }
        }
    if (changed) {
        Json ov = old && raw.contains("overrides") && raw["overrides"].is_object() ? raw["overrides"] : Json::object();
        if (old)
            for (const auto& [key, o] : old->overrides)
                if (!p.overrides.count(key)) ov.erase(key);
        for (const auto& [key, o] : p.overrides) {
            auto it = old ? old->overrides.find(key) : std::map<std::string, core::EntityOverride>::const_iterator();
            if (!old || it == old->overrides.end()) {
                ov[key] = writeOverride(o, Json(), nullptr);
            } else if (overrideJson(o) != overrideJson(it->second)) {
                ov[key] = writeOverride(o, ov.contains(key) ? ov[key] : Json(), &it->second);
            }
        }
        if (ov.empty()) raw.erase("overrides");
        else raw["overrides"] = std::move(ov);
    }
    return raw;
}

} // namespace native::doc
