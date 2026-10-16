#pragma once
#include "core/brush.h"
#include "core/entity_class.h"
#include "core/math3.h"
#include <nlohmann/json_fwd.hpp>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace core {

inline constexpr const char* kSchemaVersion = "0.2";

using Guid = std::string;

// IMPORTANT: this typed model is Core's *read-only interpretation* of a
// document, used for validation, prefab resolution and (later) lighting and
// export. The Shell owns the document schema and will add editor-only data
// Core has never heard of (visgroups, UV projection params, selection sets...).
// Core therefore keeps the raw JSON as the canonical copy and never rebuilds
// it from these structs -- see Scene. Anything not modeled here survives.
struct Entity {
    Guid id;
    std::string classname = "point_entity";
    std::unordered_map<std::string, std::string> keyvalues;
    // Reference keys (target, killtarget, ...) -> scope-relative path "entityId" or "childInstanceId/entityId"
    // (DESIGN.md 18.1). Resolved to names at compile time; never typed by the user.
    std::unordered_map<std::string, std::string> refs;

    // Point entities: where they are. Brush entities: an independent pivot,
    // not baked into brush coordinates (DESIGN.md §5.2). Meters, Y-up.
    Vec3 origin{0, 0, 0};
    Vec3 angles{0, 0, 0};  // degrees; unused until prefab rotation lands

    std::vector<Brush> brushes;

    bool isBrushEntity() const { return classname == "worldspawn" || !brushes.empty(); }
    bool showsCustomOriginGizmo() const {
        return isBrushEntity() && EntityClassRegistry::showsCustomOriginGizmo(classname);
    }
};

// A reference to another map file. Prefabs ARE map files (DESIGN.md §5.4).
// Differences an instance stores against its prefab file for ONE entity (DESIGN.md 18.1a).
struct EntityOverride {
    std::unordered_map<std::string, std::string> keyvalues;
    std::unordered_map<std::string, std::string> refs;
    std::optional<Vec3> origin, angles, scale;
    bool deleted = false;
};

struct PrefabInstance {
    Guid id;
    std::string name;  // optional; empty = derived (effectiveInstanceNames)
    std::string file;  // relative to the assets root, forward slashes
    Vec3 origin{0, 0, 0};
    Vec3 angles{0, 0, 0};
    std::optional<Vec3> scale;
    // key = entity path relative to this instance: "entityId", "childInstanceId/entityId", ...
    std::map<std::string, EntityOverride> overrides;
};

struct Document {
    std::string schemaVersion = kSchemaVersion;
    std::vector<Entity> entities;
    std::vector<PrefabInstance> prefabInstances;

    Entity* findEntity(const Guid& id);
    const Entity* findEntity(const Guid& id) const;
};

// Strict: throws std::runtime_error (or a nlohmann exception, both derive from
// std::exception) on anything malformed. The input is untrusted.
Document documentFromJson(const nlohmann::json& j);
Brush brushFromJson(const nlohmann::json& j);  // exposed so shared test vectors can use it

// Names of the instances of one document, same order: the explicit `name`, or a derived
// `<basename>`, `<basename>2`, ... that avoids every explicit name and every name already given out.
std::vector<std::string> effectiveInstanceNames(const Document& doc);

// Semantic checks a syntactically valid document must also pass: unique ids,
// structurally valid brushes, and convexity for classes that require it.
// Returns a human-readable problem, or nullopt.
std::optional<std::string> validateDocument(const Document& doc);

}  // namespace core
