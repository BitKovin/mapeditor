// tools/ref_targets -- the entities a reference (target, killtarget, ...) can point at, for the entity picker of the Details panel (port of shell/src/model/refs.ts).
// A reference is a scope-relative PATH of ids, never a typed name (DESIGN 18.1). All paths here are relative to the document being edited.
// Thread: any (pure).
// Spec: 08_MILESTONES.md T4.4.
#pragma once

#include <string>
#include <vector>

#include "core/entdef.h"
#include "core/prefab.h"
#include "doc/document.h"
#include "doc/prefabs.h"

namespace native::tools {

struct RefTarget {
    std::string path;           // "entityId" for the document's own entities, "inst/child/entityId" for prefab content
    std::string label;          // what the user sees: the entity's name (or class) plus its id
    std::string group;          // "" for the document's own entities, otherwise the instance chain "door_frame|hinge"
    std::string resolvedName;   // the name the compiler gives it (group|targetname), "" when it has none
    bool operator==(const RefTarget& o) const { return path == o.path && label == o.label && group == o.group && resolvedName == o.resolvedName; }
};

// The names of the instance chain `ids` (instance ids below the document root); nested ones are named from the prefab files in `library` (null: their ids).
std::vector<std::string> chainNames(const doc::DocumentVersion& doc, doc::PrefabLibrary* library, const std::vector<std::string>& ids);

// Every entity a reference can point at: the document's own, then the prefab content grouped by instance.
std::vector<RefTarget> refTargets(const doc::DocumentVersion& doc, const std::vector<core::ResolvedEntity>& prefab, doc::PrefabLibrary* library = nullptr);

// The reference keys of an entity: target and killtarget, every key its class types as a target, and the ones in `present`.
std::vector<std::string> refKeysOf(const core::EntClass* def, const std::vector<std::string>& present = {});

} // namespace native::tools
