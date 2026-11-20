#include "light/passes.h"

#include <algorithm>
#include <cstring>

#include "native_shaders/accumulate_comp.spv.h"
#include "native_shaders/ao_comp.spv.h"
#include "native_shaders/compose_comp.spv.h"
#include "native_shaders/direct_comp.spv.h"
#include "native_shaders/slotmap_comp.spv.h"

namespace native::light {

namespace {

bool makeMapped(gpu::Device& d, uint64_t bytes, VkBuffer& buffer, VmaAllocation& allocation, void*& mapped) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = std::max<uint64_t>(bytes, 16);
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(d.allocator(), &bci, &aci, &buffer, &allocation, &info) != VK_SUCCESS) {
        buffer = VK_NULL_HANDLE;
        allocation = nullptr;
        return false;
    }
    mapped = info.pMappedData;
    return mapped != nullptr;
}

void computeBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
}

} // namespace

// ---- job list / light buffer -----------------------------------------------------------------------------------------------------

JobList::JobList(gpu::Device& device, uint32_t capacity) : device_(device), capacity_(capacity) {
    makeMapped(device, static_cast<uint64_t>(capacity) * 16, buffer_, allocation_, mapped_);
}

JobList::~JobList() {
    if (buffer_) vmaDestroyBuffer(device_.allocator(), buffer_, allocation_);
}

bool JobList::set(const std::vector<PassJob>& jobs) {
    if (!valid() || jobs.size() > capacity_) return false;
    auto* out = static_cast<uint32_t*>(mapped_);
    uint32_t prefix = 0;
    for (size_t i = 0; i < jobs.size(); ++i) {
        out[i * 4] = jobs[i].slotBase;
        out[i * 4 + 1] = jobs[i].count;
        out[i * 4 + 2] = prefix;
        out[i * 4 + 3] = jobs[i].passes;
        prefix += jobs[i].count;
    }
    vmaFlushAllocation(device_.allocator(), allocation_, 0, std::max<VkDeviceSize>(jobs.size() * 16, 16));
    count_ = static_cast<uint32_t>(jobs.size());
    total_ = prefix;
    return true;
}

bool JobList::setCompose(const std::vector<ComposeJob>& rects) {
    if (!valid() || rects.size() > capacity_) return false;
    auto* out = static_cast<uint32_t*>(mapped_);
    uint32_t prefix = 0;
    for (size_t i = 0; i < rects.size(); ++i) {
        out[i * 4] = rects[i].x | (rects[i].y << 16);
        out[i * 4 + 1] = rects[i].w | (rects[i].h << 16);
        out[i * 4 + 2] = rects[i].page;
        out[i * 4 + 3] = prefix;
        prefix += rects[i].w * rects[i].h;
    }
    vmaFlushAllocation(device_.allocator(), allocation_, 0, std::max<VkDeviceSize>(rects.size() * 16, 16));
    count_ = static_cast<uint32_t>(rects.size());
    total_ = prefix;
    return true;
}

LightBuffer::LightBuffer(gpu::Device& device, uint32_t capacityLights) : device_(device), capacityBytes_(static_cast<uint64_t>(capacityLights) * 128) {
    void* mapped = nullptr;
    makeMapped(device, capacityBytes_, buffer_, allocation_, mapped);
    mapped_ = mapped;
}

LightBuffer::~LightBuffer() {
    if (buffer_) vmaDestroyBuffer(device_.allocator(), buffer_, allocation_);
}

bool LightBuffer::set(const std::vector<float>& records, uint32_t lightCount) {
    if (!valid() || records.size() * sizeof(float) > capacityBytes_) return false;
    if (!records.empty()) std::memcpy(mapped_, records.data(), records.size() * sizeof(float));
    vmaFlushAllocation(device_.allocator(), allocation_, 0, std::max<VkDeviceSize>(records.size() * sizeof(float), 16));
    lightCount_ = lightCount;
    return true;
}

// ---- passes ------------------------------------------------------------------------------------------------------------------------

bool Passes::makePass(Pass& p, const uint32_t* spv, size_t bytes, const std::vector<VkDescriptorType>& bindings, uint32_t pushBytes) {
    std::vector<VkDescriptorSetLayoutBinding> b(bindings.size());
    for (size_t i = 0; i < bindings.size(); ++i) b[i] = {static_cast<uint32_t>(i), bindings[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = static_cast<uint32_t>(b.size());
    li.pBindings = b.data();
    if (vkCreateDescriptorSetLayout(device_.device(), &li, nullptr, &p.setLayout) != VK_SUCCESS) return false;
    VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &p.setLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(device_.device(), &pl, nullptr, &p.layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = bytes;
    mi.pCode = spv;
    if (vkCreateShaderModule(device_.device(), &mi, nullptr, &p.module) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, p.module, "main", nullptr};
    pi.layout = p.layout;
    return vkCreateComputePipelines(device_.device(), VK_NULL_HANDLE, 1, &pi, nullptr, &p.pipeline) == VK_SUCCESS;
}

void Passes::destroyPass(Pass& p) {
    if (p.pipeline) vkDestroyPipeline(device_.device(), p.pipeline, nullptr);
    if (p.module) vkDestroyShaderModule(device_.device(), p.module, nullptr);
    if (p.layout) vkDestroyPipelineLayout(device_.device(), p.layout, nullptr);
    if (p.setLayout) vkDestroyDescriptorSetLayout(device_.device(), p.setLayout, nullptr);
    p = {};
}

Passes::Passes(gpu::Device& device) : device_(device) {
    const VkDescriptorType sb = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, as = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    // direct: 0 samples, 1 jobs, 2 sub-texel data, 3 lights, 4 out, 5 tlas
    // ao:     0 samples, 1 jobs, 2 out, 3 tlas
    // accumulate: 0 samples, 1 jobs, 2 pass values, 3 running mean
    // slotmap: 0 samples, 1 jobs, 2 slot map image;  compose: 0 slot map image, 1 compose jobs, 2..4 class means, 5 occlusion, 6 mailbox
    const VkDescriptorType si = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    valid_ = device.caps().lit() && makePass(direct_, shaders::k_direct_comp_spv, sizeof(shaders::k_direct_comp_spv), {sb, sb, sb, sb, sb, as}, 48) &&
             makePass(ao_, shaders::k_ao_comp_spv, sizeof(shaders::k_ao_comp_spv), {sb, sb, sb, as}, 32) &&
             makePass(accumulate_, shaders::k_accumulate_comp_spv, sizeof(shaders::k_accumulate_comp_spv), {sb, sb, sb, sb}, 16) &&
             makePass(slotMap_, shaders::k_slotmap_comp_spv, sizeof(shaders::k_slotmap_comp_spv), {sb, sb, si}, 16) &&
             makePass(compose_, shaders::k_compose_comp_spv, sizeof(shaders::k_compose_comp_spv), {si, sb, sb, sb, sb, sb, sb}, 48);
    if (!valid_) return;
    const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096}, {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 512}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 512}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 512;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes = sizes;
    valid_ = vkCreateDescriptorPool(device.device(), &dpi, nullptr, &pool_) == VK_SUCCESS;
}

Passes::~Passes() {
    vkDeviceWaitIdle(device_.device());
    if (pool_) vkDestroyDescriptorPool(device_.device(), pool_, nullptr);
    destroyPass(direct_);
    destroyPass(ao_);
    destroyPass(accumulate_);
    destroyPass(slotMap_);
    destroyPass(compose_);
}

void Passes::resetDescriptors() {
    if (pool_) vkResetDescriptorPool(device_.device(), pool_, 0);
}

VkDescriptorSet Passes::allocateSet(const Pass& p) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &p.setLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    return vkAllocateDescriptorSets(device_.device(), &ai, &set) == VK_SUCCESS ? set : VK_NULL_HANDLE;
}

namespace {

struct Writes {
    std::vector<VkDescriptorBufferInfo> infos;
    std::vector<VkWriteDescriptorSet> writes;
    VkWriteDescriptorSetAccelerationStructureKHR as{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    Writes(VkDescriptorSet s, size_t buffers) : set(s) {
        infos.reserve(buffers);
        writes.reserve(buffers + 1);
    }
    void buffer(uint32_t binding, VkBuffer b) {
        infos.push_back({b, 0, VK_WHOLE_SIZE});
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &infos.back();
        writes.push_back(w);
    }
    std::vector<VkDescriptorImageInfo> images;
    void image(uint32_t binding, VkImageView v) {
        images.reserve(4);
        images.push_back({VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL});
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w.pImageInfo = &images.back();
        writes.push_back(w);
    }
    void accel(uint32_t binding, VkAccelerationStructureKHR a) {
        tlas = a;
        as.accelerationStructureCount = 1;
        as.pAccelerationStructures = &tlas;
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.pNext = &as;
        w.dstSet = set;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        writes.push_back(w);
    }
    void apply(VkDevice d) { vkUpdateDescriptorSets(d, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr); }
};

uint32_t groupsFor(uint32_t total) { return (total + 63) / 64; }

} // namespace

bool Passes::recordDirect(VkCommandBuffer cmd, const SampleStore& store, Results& results, const Accel& accel, const LightBuffer& lights, const JobList& jobs,
                          LightClass cls, const DirectParams& params) {
    if (!valid_ || accel.tlas() == VK_NULL_HANDLE || !store.valid() || !lights.valid() || !jobs.valid()) return false;
    if (jobs.total() == 0) return true;
    VkDescriptorSet dset = allocateSet(direct_), aset = allocateSet(accumulate_);
    if (!dset || !aset) return false;
    Writes d(dset, 5);
    d.buffer(0, store.sampleBuffer());
    d.buffer(1, jobs.buffer());
    d.buffer(2, store.subBuffer());
    d.buffer(3, lights.buffer());
    d.buffer(4, results.scratch());
    d.accel(5, accel.tlas());
    d.apply(device_.device());
    Writes a(aset, 4);
    a.buffer(0, store.sampleBuffer());
    a.buffer(1, jobs.buffer());
    a.buffer(2, results.scratch());
    a.buffer(3, results.direct(cls));
    a.apply(device_.device());

    struct DirectPush {
        uint32_t jobCount, total, lightCount, areaSamples, skySamples, superSamples, classMask, perPass;
        float bias, farDistance;
        uint32_t pad0, pad1;
    } pc{jobs.count(), jobs.total(), lights.lightCount(), params.areaSamples, params.skySamples, params.superSamples, classBit(cls), params.perPass,
         0.01f, 10000.f, 0, 0};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, direct_.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, direct_.layout, 0, 1, &dset, 0, nullptr);
    vkCmdPushConstants(cmd, direct_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, groupsFor(jobs.total()), 1, 1);
    computeBarrier(cmd);

    const uint32_t apc[4] = {jobs.count(), jobs.total(), 0, 0};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, accumulate_.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, accumulate_.layout, 0, 1, &aset, 0, nullptr);
    vkCmdPushConstants(cmd, accumulate_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(apc), apc);
    vkCmdDispatch(cmd, groupsFor(jobs.total()), 1, 1);
    computeBarrier(cmd);
    return true;
}

bool Passes::recordSlotMap(VkCommandBuffer cmd, const SampleStore& store, const JobList& jobs, VkImageView slotMap) {
    if (!valid_ || !store.valid() || !jobs.valid() || slotMap == VK_NULL_HANDLE) return false;
    if (jobs.total() == 0) return true;
    VkDescriptorSet set = allocateSet(slotMap_);
    if (!set) return false;
    Writes w(set, 2);
    w.buffer(0, store.sampleBuffer());
    w.buffer(1, jobs.buffer());
    w.image(2, slotMap);
    w.apply(device_.device());
    const uint32_t pc[4] = {jobs.count(), jobs.total(), 0, 0};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, slotMap_.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, slotMap_.layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, slotMap_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
    vkCmdDispatch(cmd, groupsFor(jobs.total()), 1, 1);
    computeBarrier(cmd);
    return true;
}

bool Passes::recordCompose(VkCommandBuffer cmd, const Results& results, VkImageView slotMap, const JobList& composeJobs, VkBuffer mailbox, const ComposeParams& params) {
    if (!valid_ || !composeJobs.valid() || slotMap == VK_NULL_HANDLE || mailbox == VK_NULL_HANDLE) return false;
    if (composeJobs.total() == 0) return true;
    VkDescriptorSet set = allocateSet(compose_);
    if (!set) return false;
    Writes w(set, 6);
    w.image(0, slotMap);
    w.buffer(1, composeJobs.buffer());
    w.buffer(2, results.direct(LightClass::Local));
    w.buffer(3, results.direct(LightClass::Sun));
    w.buffer(4, results.direct(LightClass::Sky));
    w.buffer(5, results.occlusion());
    w.buffer(6, mailbox);
    w.apply(device_.device());
    struct ComposePush {
        uint32_t jobCount, total, atlas, aoActive;
        float ambient[4];
        float aoStrength, aoPower, aoIntensity;
        uint32_t pad;
    } pc{composeJobs.count(), composeJobs.total(), params.atlas, params.aoActive ? 1u : 0u, {params.ambient[0], params.ambient[1], params.ambient[2], 0.f},
         params.aoStrength, params.aoPower, params.aoIntensity, 0};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compose_.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compose_.layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, compose_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, groupsFor(composeJobs.total()), 1, 1);
    computeBarrier(cmd);
    return true;
}

bool Passes::recordOcclusion(VkCommandBuffer cmd, const SampleStore& store, Results& results, const Accel& accel, const JobList& jobs, uint32_t rays, float radius,
                             float falloff) {
    if (!valid_ || accel.tlas() == VK_NULL_HANDLE || !store.valid() || !jobs.valid()) return false;
    if (jobs.total() == 0) return true;
    VkDescriptorSet set = allocateSet(ao_);
    if (!set) return false;
    Writes w(set, 3);
    w.buffer(0, store.sampleBuffer());
    w.buffer(1, jobs.buffer());
    w.buffer(2, results.occlusion());
    w.accel(3, accel.tlas());
    w.apply(device_.device());
    struct AoPush {
        uint32_t jobCount, total, rays, pad0;
        float radius, bias, falloff;
        uint32_t pad1;
    } pc{jobs.count(), jobs.total(), rays, 0, radius, 1e-3f, falloff, 0};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ao_.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ao_.layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, ao_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, groupsFor(jobs.total()), 1, 1);
    computeBarrier(cmd);
    return true;
}

} // namespace native::light
