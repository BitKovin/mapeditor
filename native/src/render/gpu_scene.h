// render/gpu_scene -- the scene's triangles on the GPU: arenas, per-object ranges, frustum culling into indirect draw commands.
//
// Owns: one vertex arena and one index arena (device-local, sub-allocated, gpu/arena.h), the table of objects with their ranges, the raw-brush
// fallback meshes, and the per-frame draw lists. A snapshot published by the scene builder is handed in with setSnapshot() (any thread); the
// render thread calls sync() once per loop iteration: objects whose mesh pointer changed are uploaded through the frame's upload ring
// (deltas only), objects that left are freed once the frames that may read them retired. An object without a render mesh yet is drawn from its
// authored brush (buildRawMesh); a subtractive brush is a translucent volume.
// The draw lists are written per viewport and frame into the upload ring (VkDrawIndexedIndirectCommand, one per object): one
// vkCmdDrawIndexedIndirect per pass draws thousands of brushes.
// Thread: setSnapshot from any thread; everything else on the render thread.
// Spec: 05_RENDERER.md §4, 08_MILESTONES.md T3.4. Deviation: frustum culling is on the CPU (the objects are few thousand boxes); the spec's compute
// pass is for scenes that make the CPU loop show up in the frame time.
#pragma once

#include "scene/snapshot.h"   // first: core headers use `near` as a name, which windows.h (via volk) defines as a macro

#include <volk.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gpu/arena.h"
#include "gpu/device.h"
#include "gpu/upload_ring.h"
#include "light/light_link.h"
#include "render/math3d.h"
#include "render/texture_table.h"
#include "scene/render_mesh.h"
#include "scene/snapshot.h"

namespace native::render {

struct DrawList {
    VkBuffer buffer = VK_NULL_HANDLE;   // the upload ring's buffer
    VkDeviceSize offset = 0;
    uint32_t count = 0;
};

// What one viewport draws this frame: the objects inside its frustum, split by how they are drawn.
struct DrawLists {
    DrawList solid;         // every visible solid object (opaque and cutout triangles; the translucent ones are skipped by the shader)
    DrawList translucent;   // the solid objects that have a translucent texture (drawn again, blended, without depth writes)
    DrawList volume;        // subtractive brushes and other volumes: translucent, no depth writes
    uint32_t visible = 0;   // objects inside the frustum
};

struct SceneStats {
    uint32_t objects = 0;
    uint64_t vertices = 0, indices = 0;
    uint32_t rawObjects = 0;          // drawn from the authored brush (no mesh yet)
    uint32_t queued = 0;              // waiting for upload
    uint64_t vertexBytesUsed = 0, indexBytesUsed = 0;
    uint64_t uploads = 0;
    uint32_t litObjects = 0;          // objects of the snapshot drawn from a lit mesh (lightmap coordinates) of the bake
};

// One drawn solid object as the ray tracing scene (render/rt_scene) sees it: where its triangles sit in the arenas. `serial` identifies the uploaded content (a new
// mesh = a new serial), so a changed object is one that has a serial the RT scene does not know.
struct RtObject {
    uint64_t serial = 0;
    uint32_t vertexOffset = 0;      // in vertices, from the start of the vertex arena
    uint32_t firstIndex = 0;        // in indices, from the start of the index arena (indices are relative to vertexOffset)
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
    bool raw = false;               // drawn from the authored brush (no mesh yet)
};

using HiddenObjects = std::unordered_set<scene::ObjectKey, scene::ObjectKey::Hash>;

class GpuScene {
public:
    GpuScene(gpu::Device& device, TextureTable& textures);
    ~GpuScene();   // the GPU must be idle
    GpuScene(const GpuScene&) = delete;
    GpuScene& operator=(const GpuScene&) = delete;

    bool valid() const { return vertices_.valid() && indices_.valid(); }

    void setSnapshot(std::shared_ptr<const scene::SceneSnapshot> snapshot);
    // The objects that are not drawn (Hide); null = none. Takes effect at the next sync.
    void setHidden(std::shared_ptr<const HiddenObjects> hidden);
    // The lit copies of the objects' meshes (light/lit_mesh), made for the live layout; an object whose mesh is not the one its copy was made from keeps its plain
    // mesh. null = none. Takes effect at the next sync.
    void setLit(std::shared_ptr<const light::LitSet> lit);

    // Render thread: takes the pending snapshot and uploads what changed. `completed` = timeline value the GPU reached, `submitted` = the last
    // value submitted so far (ranges freed now may be read until then). True when work remains (the slice was full): call again next frame.
    bool sync(VkCommandBuffer cmd, gpu::UploadRing& ring, uint64_t completed, uint64_t submitted);

    // Render thread: writes the draw commands of the objects inside the frustum of `viewProj`.
    void buildDraws(const Mat4& viewProj, gpu::UploadRing& ring, DrawLists& out);

    VkBuffer vertexBuffer() const { return vertices_.buffer(); }
    VkBuffer indexBuffer() const { return indices_.buffer(); }
    VkDeviceAddress vertexAddress() const { return vertices_.deviceAddress(); }
    VkDeviceAddress indexAddress() const { return indices_.deviceAddress(); }
    // The solid objects that are drawn (not hidden, not volumes) with a mesh in the arenas: what the ray tracing scene holds. Valid until the next sync().
    const std::vector<RtObject>& rtObjects() const { return rt_; }
    bool hasObjects() const { return !draws_.empty(); }
    // Grows whenever what is drawn changed (an object uploaded, removed, hidden or shown): the DLSS history is not valid across such a change.
    uint64_t changeSerial() const { return changes_; }
    SceneStats stats() const;
    uint32_t litObjects_ = 0;

private:
    struct Object {
        std::weak_ptr<const scene::RenderMesh> mesh;     // what the ranges hold; the pointer says what is uploaded (an expired one is a changed one)
        const scene::RenderMesh* meshPtr = nullptr;
        std::vector<uint32_t> textures;                  // global texture indices the mesh uses
        gpu::Arena::Block vb, ib;
        uint64_t serial = 0;                             // changes with every upload of this object
        uint32_t vertexCount = 0;
        int32_t vertexOffset = 0;                        // in vertices
        uint32_t firstIndex = 0, indexCount = 0;
        doc::Box bounds;
        bool volume = false;
        bool raw = false;
    };
    struct Work {
        scene::ObjectKey key;
        std::shared_ptr<const scene::RenderMesh> mesh;
        doc::Box bounds;
        bool raw = false;
    };
    struct Retired {
        gpu::Arena::Block vb, ib;
        uint64_t value;
    };
    bool upload(VkCommandBuffer cmd, gpu::UploadRing& ring, const Work& w);
    void rebuildDraws();

    gpu::Device& device_;
    TextureTable& textures_;
    gpu::Arena vertices_, indices_;

    std::mutex mutex_;
    std::shared_ptr<const scene::SceneSnapshot> pending_;
    std::shared_ptr<const HiddenObjects> pendingHidden_;
    bool hiddenPending_ = false;
    std::shared_ptr<const HiddenObjects> hidden_;
    std::shared_ptr<const light::LitSet> pendingLit_, lit_;
    bool litPending_ = false;

    std::shared_ptr<const scene::SceneSnapshot> current_;
    std::unordered_map<scene::ObjectKey, Object, scene::ObjectKey::Hash> objects_;
    std::vector<Work> queue_;           // still to upload, back = next
    std::vector<Retired> retired_;
    bool barrierNeeded_ = false;
    bool drawsDirty_ = false;
    struct Draw {
        VkDrawIndexedIndirectCommand cmd;
        doc::Box bounds;
        bool volume;
        const std::vector<uint32_t>* textures;
    };
    std::vector<Draw> draws_;
    std::vector<RtObject> rt_;
    uint64_t serials_ = 0;
    std::vector<VkDrawIndexedIndirectCommand> scratchSolid_, scratchTranslucent_, scratchVolume_;
    uint32_t rawObjects_ = 0;
    uint64_t uploads_ = 0;
    uint64_t changes_ = 0;
    bool loggedFull_ = false, loggedRing_ = false;
};

} // namespace native::render
