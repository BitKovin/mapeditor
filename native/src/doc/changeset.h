// doc/changeset -- what differs between two versions of a document.
//
// Owns: ChangeKind bits, ChangedBrush / ChangedEntity / ChangeSet and diff(a, b). The diff compares node pointers first (an unchanged
// node is the same pointer in both versions), then the brush content hash, and descends only into replaced nodes, so its cost is
// O(changed objects) plus one pointer compare per entity / brush. The scene builder, the lighting and the panels react to a ChangeSet,
// never to "the document changed".
// Boxes are world-space bounds: a brush's vertices, an entity's or instance's origin (a point). Instance contents are not part of a
// DocumentVersion (T2.6 expands them), so ChangedBrush::instance is empty for now; an instance itself is reported as a ChangedEntity with
// `instance` set and an empty `entityId`.
// Thread: any (pure functions on immutable versions).
// Spec: 03_DOCUMENT.md §6, 08_MILESTONES.md T2.4.
#pragma once

#include <core/math3.h>

#include <cstdint>
#include <string>
#include <vector>

#include "doc/document.h"

namespace native::doc {

enum ChangeKind : uint32_t {
    Geometry = 1,        // vertices / faces / op / carve / smooth edges of a brush, brush added / removed
    Surface = 2,         // texture, projection, uvs, look, lightmap scale of faces (no geometry change)
    Keys = 4,            // entity keyvalues / refs (also set when an entity is added / removed)
    Transform = 8,       // entity origin / angles, instance transform
    Light = 16,          // a light entity changed (any of its keys or transform, added or removed)
    Class = 32,          // an entity's classname changed (its lighting class may change); also set when an entity is added / removed
    Prefab = 64,         // instance added / removed / file or overrides changed
};

// An axis-aligned box; invalid (empty) until a point is added. Lives here and not in core/live_bake.h (core::Aabb) to keep that header out.
struct Box {
    core::Vec3 mn{1e300, 1e300, 1e300}, mx{-1e300, -1e300, -1e300};
    bool valid() const { return mn.x <= mx.x; }
    void add(const core::Vec3& p);
    void add(const Box& o);
    bool operator==(const Box& o) const;
};

struct ChangedBrush {
    std::string instance;   // "" = a brush of the document itself
    std::string entityId;
    std::string brushId;
    uint32_t kinds = 0;
    Box before, after;      // invalid when the brush did not exist on that side
};

struct ChangedEntity {
    std::string instance;   // "" = an entity of the document; otherwise the instance id (entityId is then "")
    std::string entityId;
    uint32_t kinds = 0;
    Box before, after;
};

struct ChangeSet {
    uint64_t fromVersion = 0, toVersion = 0;
    std::vector<ChangedBrush> brushes;
    std::vector<ChangedEntity> entities;
    bool all = false;       // a new document, a changed root setting (level settings, game profile): everything is new

    bool empty() const { return !all && brushes.empty() && entities.empty(); }
};

// Pointer identity first, then the content hash. Changes of keys the editor does not model (raw only) are not reported.
ChangeSet diff(const DocumentVersion& a, const DocumentVersion& b);

// A change set that says "everything is new" (a document was opened).
ChangeSet allChanged(uint64_t fromVersion, uint64_t toVersion);

// World-space bounds of a brush node (invalid when it has no vertices).
Box brushBox(const core::Brush& brush);

} // namespace native::doc
