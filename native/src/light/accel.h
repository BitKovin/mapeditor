// light/accel -- acceleration structures of the live bake: one BLAS per object (brush / model / relief face set), a TLAS over them, the object records.
//
// Owns: occluder triangles grouped per object, their GPU copies (vertices and per-triangle lightmap attributes in arenas), one BLAS per object in a storage
// arena, the TLAS (rebuilt, not updated, whenever the object set changes) and the object record buffer the shaders index with `instanceCustomIndex`.
// Occluders are what core::buildOccluders makes (same rules: shadow-casting classes, a face's relief replaces its flat pieces, double sided, opaque), only
// split per object so an edit rebuilds a few small BLASes (M8) instead of one mesh of the level.
// Thread: the bake thread (compute queue). update() blocks until the builds are done (the scheduler of T6.5 / the BLAS policy of T8.3 make them asynchronous).
// Spec: 06_LIGHTING.md §5, 08 T6.3.
//
// Deviations kept for later tasks: every BLAS is built PREFER_FAST_TRACE (the "fast build first, compact after 2 s idle" policy is T8.3); transparent objects
// are flagged non-opaque but their cutout test (surfaceAt) and opacity micromaps come with T7 / T8.4.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/lightmap_uv.h"
#include "core/raytrace.h"
#include "gpu/arena.h"
#include "gpu/device.h"

namespace native::light {

struct AccelObject {
    std::string key;                // identity of the object across updates: "instance|entity|brushInstance|brush"
    uint64_t hash = 0;              // changes with the content (triangles, attributes, flags)
    std::vector<float> tris;        // 9 floats per triangle, world space
    std::vector<float> lightmap;    // 8 floats per triangle as core::OccluderMesh::lightmap (u0 v0 u1 v1 u2 v2 page valid), or empty
    bool transparent = false;       // cutout / translucent surfaces: not forced opaque
    size_t triangleCount() const { return tris.size() / 9; }
};

// What the shaders read per instance (`instanceCustomIndex` = slot): 16 bytes.
struct ObjectRecord {
    uint64_t attrAddress;           // device address of the object's lightmap attributes (8 floats per triangle), 0 when it has none
    uint32_t triangleCount;
    uint32_t flags;                 // bit 0: transparent
};
static_assert(sizeof(ObjectRecord) == 16);

// The occluders of `surfaces`, per object, with the same triangles (and order inside an object) buildOccluders makes. With `layout`, lightmap attributes too.
std::vector<AccelObject> buildAccelObjects(const std::vector<core::LightSurface>& surfaces, const core::LightingClasses& classes, const core::LightmapLayout* layout);

struct AccelOptions {
    uint64_t vertexBytes = 128ull << 20;   // arena of the BLAS inputs (kept: a rebuild needs them)
    uint64_t attrBytes = 128ull << 20;
    uint64_t blasBytes = 256ull << 20;     // storage of all BLASes
    uint32_t maxObjects = 1u << 16;        // rows of the object record buffer
};

class Accel {
public:
    Accel(gpu::Device& device, const AccelOptions& options = {});
    ~Accel();
    Accel(const Accel&) = delete;
    Accel& operator=(const Accel&) = delete;

    bool valid() const { return valid_; }
    // Makes the structures match `objects`: new or changed objects get a (re)built BLAS, objects that are gone are dropped, the TLAS is rebuilt when anything
    // changed. Returns false when an arena is full or a build failed (the previous state stays usable). Waits for the queue.
    bool update(const std::vector<AccelObject>& objects);

    VkAccelerationStructureKHR tlas() const { return tlas_; }          // null until the first update with a triangle
    VkBuffer objectBuffer() const { return objects_; }                 // ObjectRecord per slot
    uint64_t generation() const { return generation_; }                // +1 whenever tlas() changed
    struct Stats {
        size_t objects = 0, triangles = 0, rebuilt = 0, removed = 0;   // objects / triangles live; rebuilt and removed by the last update
        uint64_t blasBytes = 0, tlasBytes = 0;
        double seconds = 0;
    };
    const Stats& stats() const { return stats_; }
    uint32_t slotOf(const std::string& key) const;                     // ~0u when unknown

private:
    struct Entry {
        std::string key;
        uint64_t hash = 0;
        uint32_t triangles = 0;
        bool transparent = false;
        gpu::Arena::Block vertices, attrs, storage;
        VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
        VkDeviceAddress blasAddress = 0;
        bool live = false;
    };
    void destroyEntry(Entry& e);
    void destroyTlas();

    gpu::Device& device_;
    AccelOptions options_;
    bool valid_ = false;
    uint64_t scratchAlign_ = 128;
    gpu::Arena vertexArena_, attrArena_, blasArena_;
    std::vector<Entry> entries_;                   // by slot
    std::vector<uint32_t> freeSlots_;
    std::unordered_map<std::string, uint32_t> slots_;
    VkAccelerationStructureKHR tlas_ = VK_NULL_HANDLE;
    VkBuffer tlasBuffer_ = VK_NULL_HANDLE;
    VmaAllocation tlasAlloc_ = nullptr;
    VkBuffer objects_ = VK_NULL_HANDLE;
    VmaAllocation objectsAlloc_ = nullptr;
    uint64_t generation_ = 0;
    Stats stats_;
};

} // namespace native::light
