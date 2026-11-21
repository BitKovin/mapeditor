// render/rt_scene -- the ray tracing scene of the render thread: one BLAS per drawn solid object, a TLAS over them, the object records the kernels index.
//
// Owns: the BLASes (built from the GpuScene's vertex / index arenas, no geometry copy), their storage arena, the TLAS (rebuilt in the frame an object appeared, changed or
// went away), the record buffer (`RtRecord` per instance slot, `instanceCustomIndex` = slot), the idle policy (fast build first, a fast-trace rebuild with compaction
// once nothing changed for two seconds). What is traced is what is drawn: the objects of GpuScene::rtObjects() (hidden objects and volumes are not in it).
// Thread: the render thread, recorded into the upload command buffer of the frame loop (graphics queue) right after GpuScene::sync.
// Spec: 12_RT_VIEWPORT.md §3, 08_MILESTONES.md R1. Deviations kept for later tasks: every BLAS geometry is opaque (the cutout / translucent test of `surfaceAt` is
// R3), the instance mask is `kRtMaskSolid` for objects of a class that blocks light and `kRtMaskReceiver` for the others (R3; sky, translucent come later), instances are
// double sided.
#pragma once

#include <volk.h>

#include <vk_mem_alloc.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "gpu/arena.h"
#include "gpu/device.h"
#include "render/gpu_scene.h"

namespace native::render {

constexpr uint32_t kRtMaskSolid = 1;     // instances that block light: the shadow, occlusion and light rays use only this bit
constexpr uint32_t kRtMaskReceiver = 2;  // instances that only receive light (their class does not block it)

// What the kernels read per instance (`instanceCustomIndex` = slot): 32 bytes.
struct RtRecord {
    uint64_t vertexAddress;     // first vertex of the object (scene::RenderVertex, 40 bytes, position first)
    uint64_t indexAddress;      // first index (uint32, relative to the first vertex)
    uint32_t triangleCount;
    uint32_t flags;             // bit 0: drawn from the authored brush (no mesh yet)
    uint32_t vertexCount;
    uint32_t pad;
};
static_assert(sizeof(RtRecord) == 32);

struct RtOptions {
    uint64_t blasBytes = 512ull << 20;          // storage of all BLASes
    uint32_t maxObjects = 1u << 16;             // instance slots
    uint64_t scratchBudget = 64ull << 20;       // BLAS scratch of one update: the builds of a frame stop when it is used up
    double idleSeconds = 2.0;                   // no change for this long: the fast-trace rebuild with compaction starts
    uint32_t upgradesPerUpdate = 128;
};

class RtScene {
public:
    RtScene(gpu::Device& device, GpuScene& scene, const RtOptions& options = {});
    ~RtScene();   // waits for the device
    RtScene(const RtScene&) = delete;
    RtScene& operator=(const RtScene&) = delete;

    bool valid() const { return valid_; }

    // Records into `cmd` (outside a render pass, after GpuScene::sync recorded its uploads): the BLASes of new objects, the removal of gone ones, the TLAS and the
    // records when anything changed, and the idle upgrade steps. `completed` = timeline value the GPU reached, `submitted` = the last value submitted so far;
    // `cmd` itself will complete with `submitted + 1`. True when objects still wait for their BLAS (the build budget was used up): call again at once.
    bool update(VkCommandBuffer cmd, uint64_t completed, uint64_t submitted);

    // When the next update is due without any other reason (the idle upgrade); time_point::max() when none.
    std::chrono::steady_clock::time_point nextWake() const;

    VkAccelerationStructureKHR tlas() const { return tlas_; }   // null while the scene has no triangle
    VkBuffer recordBuffer() const { return records_; }          // RtRecord per slot
    uint64_t generation() const { return generation_; }         // +1 whenever tlas() changed
    struct Stats {
        uint32_t objects = 0;                                    // instances in the TLAS
        uint64_t triangles = 0;
        uint32_t lastBuilt = 0, lastRemoved = 0;                 // BLASes made / dropped by the update that changed the scene last
        uint64_t blasBuilds = 0, tlasBuilds = 0, compacted = 0;  // totals
        uint64_t blasBytes = 0;                                  // storage in use
        bool backlog = false;                                    // objects still waiting for their BLAS
        uint32_t fastObjects = 0;                                // BLASes still of the fast-build kind
    };
    const Stats& stats() const { return stats_; }

private:
    struct Blas {
        VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
        gpu::Arena::Block storage;
        VkDeviceAddress address = 0;
        uint32_t scratchBytes = 0;
    };
    enum class Quality : uint8_t { Fast, Upgrading, Final };
    struct Entry {
        RtObject object;
        uint32_t slot = 0;
        Blas blas;
        Quality quality = Quality::Fast;
    };
    struct Retired {
        uint64_t value;   // ~0 = the next update assigns `submitted`
        std::function<void()> release;
    };
    struct Upgrade {
        uint64_t serial = 0;
        Blas blas;                   // the fast-trace rebuild
        uint32_t query = 0;
        uint64_t value = 0;          // the timeline value after which its size can be read
        Blas compact;                // set once the compacting copy is recorded
        bool copied = false;
        uint64_t copyValue = 0;
    };

    bool makeBlas(const RtObject& o, VkBuildAccelerationStructureFlagsKHR flags, Blas& out, VkAccelerationStructureGeometryKHR& geom,
                  VkAccelerationStructureBuildGeometryInfoKHR& build, VkAccelerationStructureBuildRangeInfoKHR& range);
    void retireBlas(Blas& b);
    void retire(std::function<void()> f) { retired_.push_back({~uint64_t(0), std::move(f)}); }
    bool ensureScratch(uint64_t bytes);
    bool buildTlas(VkCommandBuffer cmd);
    void writeRecords(VkCommandBuffer cmd);
    void barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) const;

    gpu::Device& device_;
    GpuScene& scene_;
    RtOptions options_;
    bool valid_ = false;
    uint64_t scratchAlign_ = 128;
    gpu::Arena blasArena_;
    std::unordered_map<uint64_t, Entry> entries_;   // by RtObject::serial
    std::vector<uint32_t> freeSlots_;
    uint32_t slotCount_ = 0;
    uint64_t seenChanges_ = ~uint64_t(0);
    bool backlog_ = false;
    bool tlasDirty_ = false;
    std::chrono::steady_clock::time_point lastChange_ = std::chrono::steady_clock::now();
    std::vector<Retired> retired_;
    std::vector<Upgrade> upgrades_;
    VkQueryPool queryPool_ = VK_NULL_HANDLE;
    uint32_t queryCount_ = 0;
    uint32_t queriesUsed_ = 0;

    VkBuffer scratch_ = VK_NULL_HANDLE;
    VmaAllocation scratchAlloc_ = nullptr;
    uint64_t scratchSize_ = 0;
    VkBuffer instances_ = VK_NULL_HANDLE, records_ = VK_NULL_HANDLE;
    VmaAllocation instancesAlloc_ = nullptr, recordsAlloc_ = nullptr;
    VkAccelerationStructureKHR tlas_ = VK_NULL_HANDLE;
    VkBuffer tlasBuffer_ = VK_NULL_HANDLE;
    VmaAllocation tlasAlloc_ = nullptr;
    uint64_t generation_ = 0;
    Stats stats_;
};

} // namespace native::render
