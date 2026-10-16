#pragma once
#include "core/document.h"
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace core {

// Thrown for an actual inclusion cycle (A includes B includes A) or when
// nesting exceeds maxDepth. NOT thrown for a "diamond": the same prefab
// referenced twice from different places is fine and expected.
struct PrefabResolveError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// One entity after prefab flattening, tagged with enough provenance to
// eventually support editing-through-instances (routing an edit back to an
// override rather than mutating the prefab source). Not used for that yet
// -- this milestone only renders/selects the flattened result.
struct ResolvedEntity {
    Entity entity;                          // already transformed into root space
    std::vector<std::string> instancePath;  // e.g. {"root"} or {"root","door_01","hinge_01"}
    Guid sourceEntityId;                    // this entity's guid inside its own file
};

struct ResolveOptions {
    int maxDepth = 16;
};

// Returns the parsed Document for `path` (paths are whatever scheme the
// caller wants -- tests use an in-memory map, the real server resolves
// relative to an assets root on disk). Must return a stable reference,
// since resolvePrefabs may call it many times for the same path (diamonds).
using DocumentLoader = std::function<const Document&(const std::string& path)>;

// Recursively flattens `root`'s prefab_instances (and their prefab_instances,
// to arbitrary depth) into a single list of world-space entities.
//
// `rootPath`, if the root document itself has a known on-disk path, seeds
// cycle detection so a prefab that (directly or indirectly) includes the
// root map is also caught, not just cycles entirely among prefab files.
//
// Canonical form of an assets-relative path ("a/./b/../c" -> "a/c", forward
// slashes). Used as cache key and for cycle detection so aliases of the same
// file are recognised as the same file.
std::string normalizeAssetPath(const std::string& path);

// Applies, per instance: origin, angles (R = Ry(yaw)*Rx(pitch)*Rz(roll), the decal convention), scale and the
// structured overrides (DESIGN.md 18.1b). Entity `angles` come out composed in root space.
std::vector<ResolvedEntity> resolvePrefabs(const Document& root, const DocumentLoader& loader,
                                            const ResolveOptions& opts = {},
                                            const std::string& rootPath = "");

// ---- compile = flatten (N1 step b) ------------------------------------------------------------
struct FlattenOptions {
    int maxDepth = 16;
    std::string nameKey = "targetname";  // the keyvalue that holds an entity's own name
};

struct FlattenedEntity {
    Entity entity;                          // root space; keyvalues[nameKey] and every ref key hold RESOLVED names, refs is empty
    std::string resolvedName;               // "outer|inner|door"; empty if the entity is unnamed
    std::vector<std::string> instanceNames; // effective instance names from the root, e.g. {"outer","inner"}
    std::vector<std::string> instancePath;  // instance ids, starting with "root"
    Guid sourceEntityId;
};

struct FlattenResult {
    std::vector<FlattenedEntity> entities;  // the root document's own entities first, then every instance's
    std::vector<std::string> problems;      // warnings: unresolved refs, duplicate resolved names, ...
};

// Throws PrefabResolveError for inclusion cycles / excess depth, like resolvePrefabs.
FlattenResult flattenForCompile(const Document& root, const DocumentLoader& loader, const FlattenOptions& opts = {},
                                const std::string& rootPath = "");

}  // namespace core
