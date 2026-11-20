// light/passes -- the compute passes of the live bake: direct light per light class, ambient occlusion, accumulation.
//
// Owns: the pipelines (direct.comp, ao.comp, accumulate.comp from native/shaders), a descriptor pool the record calls allocate from, the job buffers and the
// light record buffer. The passes only RECORD into a command buffer the caller submits on the compute queue; the caller owns the timeline: `resetDescriptors()`
// once every earlier submission finished, a JobList / LightBuffer lives until its submission finished, and a barrier orders the uploads (transfer) before the
// passes and the passes before their readers.
// Thread: the bake thread. Spec: 06_LIGHTING.md §10, 08 T6.4.
#pragma once

#include <cstdint>
#include <vector>

#include "gpu/device.h"
#include "light/accel.h"
#include "light/results.h"
#include "light/samples.h"

namespace native::light {

// One chunk's samples in one pass: the slots [slotBase, slotBase + count) and how many passes of this class the chunk had before.
struct PassJob {
    uint32_t slotBase = 0, count = 0, passes = 0;
};

// The jobs of one dispatch in host-visible memory (uvec4 {slotBase, count, prefix, passes} each). Fill with set(), keep alive until the submission is done.
class JobList {
public:
    JobList(gpu::Device& device, uint32_t capacity);
    ~JobList();
    JobList(const JobList&) = delete;
    JobList& operator=(const JobList&) = delete;
    bool valid() const { return buffer_ != VK_NULL_HANDLE; }
    // False when more jobs than the capacity. Replaces the previous content (not while a submission that reads it is pending).
    bool set(const std::vector<PassJob>& jobs);
    VkBuffer buffer() const { return buffer_; }
    uint32_t count() const { return count_; }
    uint32_t total() const { return total_; }     // samples over all jobs

private:
    gpu::Device& device_;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    void* mapped_ = nullptr;
    uint32_t capacity_ = 0, count_ = 0, total_ = 0;
};

// Light records (light_pack.h) in host-visible memory.
class LightBuffer {
public:
    LightBuffer(gpu::Device& device, uint32_t capacityLights);
    ~LightBuffer();
    LightBuffer(const LightBuffer&) = delete;
    LightBuffer& operator=(const LightBuffer&) = delete;
    bool valid() const { return buffer_ != VK_NULL_HANDLE; }
    // `records` = packLights() of the layer, `lightCount` = how many lights it holds (the rest of the floats are sky tables). Not while a submission reads it.
    bool set(const std::vector<float>& records, uint32_t lightCount);
    VkBuffer buffer() const { return buffer_; }
    uint32_t lightCount() const { return lightCount_; }

private:
    gpu::Device& device_;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    void* mapped_ = nullptr;
    uint64_t capacityBytes_ = 0;
    uint32_t lightCount_ = 0;
};

struct DirectParams {
    uint32_t areaSamples = 8;     // LiveSettings::areaSamplesPerPass
    uint32_t skySamples = 16;     // LiveSettings::skySamplesPerPass
    uint32_t superSamples = 8;    // LiveSettings::directSupersample (1 = off, at most 8)
    uint32_t perPass = 16;        // random numbers a pass draws: max(area, sky, gi samples per pass), as LiveBaker::refineBatch
};

class Passes {
public:
    explicit Passes(gpu::Device& device);
    ~Passes();
    Passes(const Passes&) = delete;
    Passes& operator=(const Passes&) = delete;
    bool valid() const { return valid_; }

    // Frees the descriptor sets of every earlier record call (their submissions must be done).
    void resetDescriptors();

    // Direct light of one class for the jobs: evaluates the class's lights at every sample (shadow rays through the TLAS), merges the pass into
    // results.direct(cls) (running mean, a chunk's first pass replaces) and leaves a compute -> compute barrier behind. False when nothing can be traced yet
    // (no TLAS) or the pool is full.
    bool recordDirect(VkCommandBuffer cmd, const SampleStore& store, Results& results, const Accel& accel, const LightBuffer& lights, const JobList& jobs,
                      LightClass cls, const DirectParams& params);
    // Ambient occlusion of the jobs' samples into results.occlusion() (overwrites). Same barrier.
    bool recordOcclusion(VkCommandBuffer cmd, const SampleStore& store, Results& results, const Accel& accel, const JobList& jobs, uint32_t rays, float radius,
                         float falloff);

private:
    struct Pass {
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkShaderModule module = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
    };
    bool makePass(Pass& p, const uint32_t* spv, size_t bytes, const std::vector<VkDescriptorType>& bindings, uint32_t pushBytes);
    void destroyPass(Pass& p);
    VkDescriptorSet allocateSet(const Pass& p);

    gpu::Device& device_;
    Pass direct_, ao_, accumulate_;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    bool valid_ = false;
};

} // namespace native::light
