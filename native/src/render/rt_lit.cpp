// render/rt_lit -- see rt_lit.h.
#include "render/rt_lit.h"

#include <algorithm>
#include <cstring>

#include "base/log.h"
#include "native_shaders/rt_ao_comp.spv.h"
#include "native_shaders/rt_compose_comp.spv.h"
#include "native_shaders/rt_filter_comp.spv.h"
#include "render/mesh_pass.h"

namespace native::render {

namespace {

// Matches `Params` of shaders/rt_lit_common.glsl (std140).
struct Params {
    float rayA[4], rayX[4], rayY[4], eye[4], fwd[4], size[4];
    float prevViewProj[16];
    uint32_t u[4];
    float ambient[4], ao[4], ao2[4], mode[4];
};
static_assert(sizeof(Params) == 240);

constexpr uint32_t kFlagReset = 1, kFlagStill = 2, kFlagFilter = 4, kFlagRr = 8;

VkShaderModule makeModule(VkDevice dev, const uint32_t* words, size_t sizeBytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = sizeBytes;
    ci.pCode = words;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, nullptr, &m);
    return m;
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

constexpr VkPipelineStageFlags2 kAll = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
constexpr VkAccessFlags2 kReadWrite = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = srcStage;
    mb.srcAccessMask = srcAccess;
    mb.dstStageMask = dstStage;
    mb.dstAccessMask = dstAccess;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
}

bool makeImage(const gpu::Device& device, RtLitTargets::Image& img, VkFormat format, VkExtent2D extent, VkImageUsageFlags usage) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {extent.width, extent.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateImage(device.allocator(), &ici, &aci, &img.image, &img.allocation, &info) != VK_SUCCESS) return false;
    img.memory = info.deviceMemory;
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vkCreateImageView(device.device(), &vci, nullptr, &img.view) == VK_SUCCESS;
}

void destroyImage(const gpu::Device& device, RtLitTargets::Image& img) {
    if (img.view) vkDestroyImageView(device.device(), img.view, nullptr);
    if (img.image) vmaDestroyImage(device.allocator(), img.image, img.allocation);
    img = {};
}

VkPipeline makeCompute(VkDevice dev, VkPipelineLayout layout, const uint32_t* words, size_t sizeBytes, const char* name) {
    VkShaderModule cs = makeModule(dev, words, sizeBytes);
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main", nullptr};
    cpi.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &p) != VK_SUCCESS) base::logError(std::string("RtLit: pipeline ") + name + " failed");
    vkDestroyShaderModule(dev, cs, nullptr);
    return p;
}

gpu::DlssImage describe(const RtLitTargets::Image& img, VkFormat format, VkExtent2D extent, VkImageLayout layout, VkImageUsageFlags usage) {
    gpu::DlssImage d;
    d.image = img.image;
    d.view = img.view;
    d.memory = img.memory;
    d.format = format;
    d.usage = usage;
    d.layout = layout;
    d.width = extent.width;
    d.height = extent.height;
    return d;
}

} // namespace

RtLit::RtLit(const gpu::Device& device) : device_(device) {
    const VkDevice dev = device_.device();
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(dev, &sci, nullptr, &sampler_);

    VkDescriptorSetLayoutBinding b[12]{};
    const VkShaderStageFlags cs = VK_SHADER_STAGE_COMPUTE_BIT;
    uint32_t n = 0;
    for (uint32_t i : {0u, 1u, 2u, 3u}) b[n++] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, cs, nullptr};
    for (uint32_t i : {4u, 5u, 7u, 8u, 9u, 10u}) b[n++] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, cs, nullptr};
    b[n++] = {11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs, nullptr};
    b[n++] = {12, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, cs, nullptr};
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = n;
    lci.pBindings = b;
    vkCreateDescriptorSetLayout(dev, &lci, nullptr, &setLayout_);
    VkDescriptorSetLayoutBinding tb{0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, cs, nullptr};
    VkDescriptorSetLayoutCreateInfo tci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    tci.bindingCount = 1;
    tci.pBindings = &tb;
    vkCreateDescriptorSetLayout(dev, &tci, nullptr, &tlasLayout_);

    const VkDescriptorSetLayout both[2] = {setLayout_, tlasLayout_};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 2;
    pli.pSetLayouts = both;
    vkCreatePipelineLayout(dev, &pli, nullptr, &aoLayout_);
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
    VkPipelineLayoutCreateInfo fli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    fli.setLayoutCount = 1;
    fli.pSetLayouts = &setLayout_;
    fli.pushConstantRangeCount = 1;
    fli.pPushConstantRanges = &range;
    vkCreatePipelineLayout(dev, &fli, nullptr, &filterLayout_);

    aoPipeline_ = makeCompute(dev, aoLayout_, native::shaders::k_rt_ao_comp_spv, sizeof(native::shaders::k_rt_ao_comp_spv), "rt_ao");
    filterPipeline_ = makeCompute(dev, filterLayout_, native::shaders::k_rt_filter_comp_spv, sizeof(native::shaders::k_rt_filter_comp_spv), "rt_filter");
    composePipeline_ = makeCompute(dev, filterLayout_, native::shaders::k_rt_compose_comp_spv, sizeof(native::shaders::k_rt_compose_comp_spv), "rt_compose");
}

RtLit::~RtLit() {
    const VkDevice dev = device_.device();
    for (VkPipeline p : {aoPipeline_, filterPipeline_, composePipeline_})
        if (p) vkDestroyPipeline(dev, p, nullptr);
    if (aoLayout_) vkDestroyPipelineLayout(dev, aoLayout_, nullptr);
    if (filterLayout_) vkDestroyPipelineLayout(dev, filterLayout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
    if (tlasLayout_) vkDestroyDescriptorSetLayout(dev, tlasLayout_, nullptr);
    if (sampler_) vkDestroySampler(dev, sampler_, nullptr);
}

bool RtLit::createTargets(RtLitTargets& t, VkExtent2D render, bool reconstruction) const {
    destroyTargets(t);
    t.render = render;
    constexpr VkImageUsageFlags kTransfer = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    constexpr VkImageUsageFlags kAttachment = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | kTransfer;
    constexpr VkImageUsageFlags kStorage = VK_IMAGE_USAGE_STORAGE_BIT | kTransfer;
    bool ok = true;
    for (int i = 0; i < 2; ++i) ok = ok && makeImage(device_, t.normal[i], kGBufferNormalFormat, render, kAttachment);
    ok = ok && makeImage(device_, t.dist, kGBufferDistFormat, render, kAttachment);
    for (int i = 0; i < 2; ++i) ok = ok && makeImage(device_, t.hist[i], VK_FORMAT_R32G32B32A32_SFLOAT, render, kStorage);
    for (int i = 0; i < 2; ++i) ok = ok && makeImage(device_, t.fil[i], VK_FORMAT_R16G16_SFLOAT, render, kStorage);
    ok = ok && makeImage(device_, t.sample, VK_FORMAT_R16_SFLOAT, render, kStorage);
    if (reconstruction) {
        ok = ok && makeImage(device_, t.radiance, VK_FORMAT_R16G16B16A16_SFLOAT, render, kStorage | VK_IMAGE_USAGE_SAMPLED_BIT);
        ok = ok && makeImage(device_, t.specAlbedo, VK_FORMAT_R8G8B8A8_UNORM, render, VK_IMAGE_USAGE_SAMPLED_BIT | kTransfer);
    }
    const auto buffer = [&](VkBuffer& buf, VmaAllocation& alloc, VkDeviceSize size, VkBufferUsageFlags usage, bool host, void** mapped) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = usage;
        VmaAllocationCreateInfo aci{};
        aci.usage = host ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (host) aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(device_.allocator(), &bci, &aci, &buf, &alloc, &info) != VK_SUCCESS) return false;
        if (mapped) *mapped = info.pMappedData;
        return true;
    };
    ok = ok && buffer(t.counters, t.countersAlloc, 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, nullptr);
    for (int i = 0; i < 2; ++i) {
        void* p = nullptr;
        ok = ok && buffer(t.readback[i], t.readbackAlloc[i], 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &p);
        t.readbackPtr[i] = static_cast<uint32_t*>(p);
        ok = ok && buffer(t.params[i], t.paramsAlloc[i], sizeof(Params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, &t.paramsPtr[i]);
    }
    if (ok) {
        const VkDescriptorPoolSize sizes[5] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 12}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
                                               {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 2}};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = 4;
        pci.poolSizeCount = 5;
        pci.pPoolSizes = sizes;
        ok = vkCreateDescriptorPool(device_.device(), &pci, nullptr, &t.pool) == VK_SUCCESS;
    }
    if (ok) {
        const VkDescriptorSetLayout layouts[4] = {setLayout_, setLayout_, tlasLayout_, tlasLayout_};
        VkDescriptorSet sets[4] = {};
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = t.pool;
        ai.descriptorSetCount = 4;
        ai.pSetLayouts = layouts;
        ok = vkAllocateDescriptorSets(device_.device(), &ai, sets) == VK_SUCCESS;
        t.sets[0] = sets[0];
        t.sets[1] = sets[1];
        t.tlasSets[0] = sets[2];
        t.tlasSets[1] = sets[3];
    }
    if (!ok) {
        base::logError("RtLit: the targets could not be created");
        destroyTargets(t);
    }
    return ok;
}

void RtLit::destroyTargets(RtLitTargets& t) const {
    if (t.pool) vkDestroyDescriptorPool(device_.device(), t.pool, nullptr);
    for (auto& i : t.normal) destroyImage(device_, i);
    for (auto& i : t.hist) destroyImage(device_, i);
    for (auto& i : t.fil) destroyImage(device_, i);
    destroyImage(device_, t.dist);
    destroyImage(device_, t.sample);
    destroyImage(device_, t.radiance);
    destroyImage(device_, t.specAlbedo);
    if (t.counters) vmaDestroyBuffer(device_.allocator(), t.counters, t.countersAlloc);
    for (int i = 0; i < 2; ++i) {
        if (t.readback[i]) vmaDestroyBuffer(device_.allocator(), t.readback[i], t.readbackAlloc[i]);
        if (t.params[i]) vmaDestroyBuffer(device_.allocator(), t.params[i], t.paramsAlloc[i]);
    }
    t = RtLitTargets{};
}

void RtLit::beginGBuffer(VkCommandBuffer cmd, RtLitTargets& t) const {
    if (!t.initialized) {
        for (int i = 0; i < 2; ++i) {
            imageBarrier(cmd, t.hist[i].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, kAll, 0, kAll, kReadWrite);
            imageBarrier(cmd, t.fil[i].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, kAll, 0, kAll, kReadWrite);
            imageBarrier(cmd, t.normal[i].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kAll, 0, kAll, kReadWrite);
        }
        imageBarrier(cmd, t.sample.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, kAll, 0, kAll, kReadWrite);
        t.initialized = true;
    }
    t.normalCur ^= 1u;
    imageBarrier(cmd, t.normal[t.normalCur].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kAll, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    imageBarrier(cmd, t.dist.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kAll, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
}

void RtLit::endGBuffer(VkCommandBuffer cmd, const RtLitTargets& t) const {
    imageBarrier(cmd, t.normal[t.normalCur].image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kAll, VK_ACCESS_2_MEMORY_READ_BIT);
    imageBarrier(cmd, t.dist.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kAll, VK_ACCESS_2_MEMORY_READ_BIT);
}

bool RtLit::record(VkCommandBuffer cmd, RtLitTargets& t, const DlssTargets& d, const RtLitFrame& f) const {
    if (!valid() || !t.valid() || f.tlas == VK_NULL_HANDLE) return false;
    const uint32_t slot = f.slot & 1u;
    const uint32_t w = t.render.width, h = t.render.height;

    // ---- the frame's parameters
    Params p{};
    {
        const Mat4 inv = mat4Invert(f.jitteredViewProj);
        const auto unproject = [&](double x, double y) {
            const double v[4] = {x, y, 0.5, 1.0};
            double q[4];
            for (int r = 0; r < 4; ++r) q[r] = inv[0 * 4 + r] * v[0] + inv[1 * 4 + r] * v[1] + inv[2 * 4 + r] * v[2] + inv[3 * 4 + r] * v[3];
            return Vec3{q[0] / q[3] - f.eye[0], q[1] / q[3] - f.eye[1], q[2] / q[3] - f.eye[2]};
        };
        const Vec3 a = unproject(0, 0), bx = unproject(1, 0), by = unproject(0, 1);
        const Vec3 vx = bx - a, vy = by - a;
        for (int i = 0; i < 3; ++i) {
            p.rayA[i] = static_cast<float>(a[i]);
            p.rayX[i] = static_cast<float>(vx[i]);
            p.rayY[i] = static_cast<float>(vy[i]);
            p.eye[i] = static_cast<float>(f.eye[i]);
            p.fwd[i] = static_cast<float>(f.forward[i]);
        }
        p.size[0] = static_cast<float>(w);
        p.size[1] = static_cast<float>(h);
        for (int i = 0; i < 16; ++i) p.prevViewProj[i] = static_cast<float>(f.prevViewProj[static_cast<size_t>(i)]);
        const bool filter = f.options.filter && !f.reconstruction && f.options.filterIterations > 0 && !f.skipTrace;
        p.u[0] = f.frameNumber;
        p.u[1] = (f.reset ? kFlagReset : 0u) | (f.still ? kFlagStill : 0u) | (filter ? kFlagFilter : 0u) | (f.reconstruction ? kFlagRr : 0u);
        p.u[2] = std::max(1u, f.options.sampleCap);
        p.u[3] = std::max(1u, f.options.targetSamples);
        for (int i = 0; i < 3; ++i) p.ambient[i] = f.lighting.ambient[i];
        p.ambient[3] = f.lighting.aoActive ? 1.0f : 0.0f;
        p.ao[0] = f.lighting.aoRadius;
        p.ao[1] = f.lighting.aoIntensity;
        p.ao[2] = f.lighting.aoStrength;
        p.ao[3] = f.lighting.aoPower;
        p.ao2[0] = f.lighting.aoFalloff;
        p.ao2[1] = 1e-3f;   // the bake's origin offset along the normal (kAoBias)
        p.ao2[2] = f.lighting.exposure;
        p.ao2[3] = f.lighting.brightness;
        p.mode[0] = static_cast<float>(f.mode);
        p.mode[1] = static_cast<float>(f.options.sampleCapMoving);
        p.mode[2] = std::max(1.0f, f.options.meanBlend);
        p.mode[3] = std::max(1.0f, f.options.meanBlendRr);
        std::memcpy(t.paramsPtr[slot], &p, sizeof(p));
        vmaFlushAllocation(device_.allocator(), t.paramsAlloc[slot], 0, sizeof(p));
    }

    // ---- descriptors (this slot's previous frame has finished)
    const uint32_t latest = t.latest, other = 1u - latest;
    const uint32_t histPrev = latest, histCur = f.skipTrace ? latest : other;
    const uint32_t normalPrev = 1u - t.normalCur;
    VkDescriptorImageInfo samplers[4] = {{sampler_, t.dist.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                         {sampler_, d.color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                         {sampler_, t.normal[t.normalCur].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                         {sampler_, t.normal[normalPrev].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    const VkImageView outView = f.reconstruction ? t.radiance.view : d.output.view;
    VkDescriptorImageInfo storage[6] = {{VK_NULL_HANDLE, t.hist[f.skipTrace ? other : histPrev].view, VK_IMAGE_LAYOUT_GENERAL},
                                        {VK_NULL_HANDLE, t.hist[histCur].view, VK_IMAGE_LAYOUT_GENERAL},
                                        {VK_NULL_HANDLE, t.fil[0].view, VK_IMAGE_LAYOUT_GENERAL},
                                        {VK_NULL_HANDLE, t.fil[1].view, VK_IMAGE_LAYOUT_GENERAL},
                                        {VK_NULL_HANDLE, outView, VK_IMAGE_LAYOUT_GENERAL},
                                        {VK_NULL_HANDLE, t.sample.view, VK_IMAGE_LAYOUT_GENERAL}};
    const VkDescriptorBufferInfo counterInfo{t.counters, 0, 8};
    const VkDescriptorBufferInfo paramInfo{t.params[slot], 0, sizeof(Params)};
    VkWriteDescriptorSetAccelerationStructureKHR asw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    asw.accelerationStructureCount = 1;
    asw.pAccelerationStructures = &f.tlas;
    VkWriteDescriptorSet writes[14]{};
    uint32_t n = 0;
    const auto write = [&](uint32_t binding, VkDescriptorType type) -> VkWriteDescriptorSet& {
        VkWriteDescriptorSet& wr = writes[n++];
        wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr.dstSet = t.sets[slot];
        wr.dstBinding = binding;
        wr.descriptorCount = 1;
        wr.descriptorType = type;
        return wr;
    };
    for (uint32_t i = 0; i < 4; ++i) write(i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER).pImageInfo = &samplers[i];
    const uint32_t storageBindings[6] = {4, 5, 7, 8, 9, 10};
    for (uint32_t i = 0; i < 6; ++i) write(storageBindings[i], VK_DESCRIPTOR_TYPE_STORAGE_IMAGE).pImageInfo = &storage[i];
    write(11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER).pBufferInfo = &counterInfo;
    write(12, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER).pBufferInfo = &paramInfo;
    {
        VkWriteDescriptorSet& wr = writes[n++];
        wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr.pNext = &asw;
        wr.dstSet = t.tlasSets[slot];
        wr.dstBinding = 0;
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    }
    vkUpdateDescriptorSets(device_.device(), n, writes, 0, nullptr);

    // ---- passes
    if (f.reconstruction) imageBarrier(cmd, t.radiance.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, kAll, 0, kAll, kReadWrite);
    vkCmdFillBuffer(cmd, t.counters, 0, 8, 0);
    memoryBarrier(cmd, kAll, kReadWrite, kAll, kReadWrite);
    const uint32_t gx = (w + 7) / 8, gy = (h + 7) / 8;
    if (!f.skipTrace) {
        const VkDescriptorSet both[2] = {t.sets[slot], t.tlasSets[slot]};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, aoPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, aoLayout_, 0, 2, both, 0, nullptr);
        vkCmdDispatch(cmd, gx, gy, 1);
        memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        t.latest = histCur;
    }
    uint32_t finalFil = 0;
    if ((p.u[1] & kFlagFilter) != 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, filterPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, filterLayout_, 0, 1, &t.sets[slot], 0, nullptr);
        for (uint32_t it = 0; it < f.options.filterIterations; ++it) {
            vkCmdPushConstants(cmd, filterLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &it);
            vkCmdDispatch(cmd, gx, gy, 1);
            memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            finalFil = it & 1u;
        }
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, filterLayout_, 0, 1, &t.sets[slot], 0, nullptr);
    vkCmdPushConstants(cmd, filterLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &finalFil);
    vkCmdDispatch(cmd, gx, gy, 1);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kAll, kReadWrite);

    // ---- the counters of this frame come back to the host (read when the slot is used again)
    VkBufferCopy copy{0, 0, 8};
    vkCmdCopyBuffer(cmd, t.counters, t.readback[slot], 1, &copy);
    VkBufferMemoryBarrier2 hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    hb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    hb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    hb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    hb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hb.buffer = t.readback[slot];
    hb.size = VK_WHOLE_SIZE;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &hb;
    vkCmdPipelineBarrier2(cmd, &dep);
    t.readbackValid[slot] = true;
    return true;
}

bool RtLit::readCounters(RtLitTargets& t, uint32_t slot, Counters* out) const {
    slot &= 1u;
    if (!t.readbackValid[slot] || !t.readbackPtr[slot]) return false;
    vmaInvalidateAllocation(device_.allocator(), t.readbackAlloc[slot], 0, 8);
    out->traced = t.readbackPtr[slot][0];
    out->surface = t.readbackPtr[slot][1];
    t.readbackValid[slot] = false;
    return true;
}

void RtLit::prepareReconstruction(VkCommandBuffer cmd, RtLitTargets& t) const {
    if (t.specAlbedoReady || t.specAlbedo.image == VK_NULL_HANDLE) return;
    imageBarrier(cmd, t.specAlbedo.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kAll, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkClearColorValue black{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, t.specAlbedo.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    imageBarrier(cmd, t.specAlbedo.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, kAll, kReadWrite);
    t.specAlbedoReady = true;
}

gpu::DlssImage RtLit::specularAlbedo(const RtLitTargets& t) {
    return describe(t.specAlbedo, VK_FORMAT_R8G8B8A8_UNORM, t.render, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
}

gpu::DlssImage RtLit::radiance(const RtLitTargets& t) {
    return describe(t.radiance, VK_FORMAT_R16G16B16A16_SFLOAT, t.render, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
}

gpu::DlssImage RtLit::normalForUpscaler(const RtLitTargets& t) {
    return describe(t.normal[t.normalCur], kGBufferNormalFormat, t.render, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
}

} // namespace native::render
