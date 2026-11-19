// scene/snapshot -- the immutable picture of the scene that the render and bake threads read.
//
// Owns: ObjectKey (the identity of a brush in the scene), ObjectProducts (what is derived for it) and SceneSnapshot. A snapshot is published
// by the scene builder by swapping a shared pointer; readers keep the one they started their frame with. Products of objects that were
// not rebuilt are shared with the previous snapshot.
// T3.2 products: the carved surface pieces (core::LightSurface) of a brush and its bounds; the render mesh joins them in T3.4
// (`render` stays null until then: the renderer draws the raw brush).
// Thread: immutable once published, readable from any thread.
// Spec: 04_SCENE_AND_DERIVED.md §5, 08_MILESTONES.md T3.2.
#pragma once

#include <core/derived.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "doc/changeset.h"
#include "doc/document.h"
#include "doc/prefabs.h"

namespace native::scene {

struct ObjectKey {
    std::string instance;   // core::instanceString of the instance path; "" = the document itself
    std::string entityId;
    std::string brushId;
    bool operator==(const ObjectKey& o) const { return instance == o.instance && entityId == o.entityId && brushId == o.brushId; }
    bool operator<(const ObjectKey& o) const {
        return std::tie(instance, entityId, brushId) < std::tie(o.instance, o.entityId, o.brushId);
    }
    struct Hash {
        size_t operator()(const ObjectKey& k) const {
            const std::hash<std::string> h;
            size_t r = h(k.instance);
            r = r * 1099511628211ull ^ h(k.entityId);
            r = r * 1099511628211ull ^ h(k.brushId);
            return r;
        }
    };
};

struct RenderMesh;   // T3.4

struct ObjectProducts {
    // The pieces collectSurfaces() makes for this brush (cut walls of its subtractors included); empty for subtractive brushes and
    // editor-only volumes. Never null.
    std::shared_ptr<const std::vector<core::LightSurface>> surfaces;
    std::shared_ptr<const RenderMesh> render;   // null = not built: draw the raw brush
    doc::Box bounds;                            // the brush's vertices (world space)
    uint64_t inputsHash = 0;                    // what the surfaces were made from (cache key)
    bool additive = false;                      // a solid brush (has surfaces); false for subtractive / editor-only
};

struct SceneSnapshot {
    uint64_t version = 0;                                   // the DocumentVersion it was built from
    std::shared_ptr<const doc::DocumentVersion> doc;
    std::shared_ptr<const doc::Expansion> prefabs;          // the prefab expansion it was built with (may be null)
    std::unordered_map<ObjectKey, ObjectProducts, ObjectKey::Hash> objects;
    doc::ChangeSet sincePrevious;                           // merged change set since the previous published snapshot
    bool complete = true;                                   // false: a partial publish, objects still building keep their previous products
};

} // namespace native::scene
