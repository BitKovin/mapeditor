// scene/snapshot -- the immutable picture of the scene that the render and bake threads read.
//
// Owns: ObjectKey (the identity of a brush in the scene), ObjectProducts (what is derived for it) and SceneSnapshot. A snapshot is published
// by the scene builder by swapping a shared pointer; readers keep the one they started their frame with. Products of objects that were
// not rebuilt are shared with the previous snapshot.
// Products: the carved surface pieces (core::LightSurface) of a brush and its bounds (T3.2), the relief meshes of its faces (T3.3); the render
// mesh joins them in T3.4 (`render` is null until its geometry job finished: the renderer draws `raw`, the authored brush).
// Thread: immutable once published, readable from any thread.
// Spec: 04_SCENE_AND_DERIVED.md §5, 08_MILESTONES.md T3.2.
#pragma once

// windows.h (pulled in by volk.h) defines `near` / `far` as macros; the Core uses `near` as a member name.
#pragma push_macro("near")
#pragma push_macro("far")
#undef near
#undef far
#include <core/derived.h>
#pragma pop_macro("far")
#pragma pop_macro("near")

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
#include "scene/relief.h"

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
    // The relief meshes of the object's pieces (T3.3), one per authored face side, sorted by key; null = no relief environment was set when the
    // object was built. `reliefHash` = what they were made from (the surfaces' hash, the neighbours', the texture environment's revision); a
    // partial snapshot can hold new surfaces with the previous relief until the relief of the object is rebuilt.
    std::shared_ptr<const ReliefList> relief;
    uint64_t reliefHash = 0;
    std::shared_ptr<const RenderMesh> render;   // null = not built yet: draw `raw`
    std::shared_ptr<const RenderMesh> raw;      // the authored brush as triangles (solid brushes: the stand-in until `render` exists)
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
