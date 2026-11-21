// render/rt_lit -- see rt_lit.h.
#include "render/rt_lit.h"

#include <algorithm>
#include <cstring>

#include "base/log.h"
#include "native_shaders/rt_ao_comp.spv.h"
#include "native_shaders/rt_compose_comp.spv.h"
#include "native_shaders/rt_filter_comp.spv.h"
#include "native_shaders/rt_sharc_resolve_comp.spv.h"
#include "native_shaders/rt_sharc_update_comp.spv.h"
#include "render/mesh_pass.h"

namespace native::render {

namespace {

// Matches `Params` of shaders/rt_lit_common.glsl (std140).
struct Params {
    float rayA[4], rayX[4], rayY[4], eye[4], fwd[4], size[4];
    float prevViewProj[16];
    uint32_t u[4];
    float ambient[4], ao[4], ao2[4], mode[4];
    float gridOrigin[4];
    uint32_t gridDim[4], lightInfo[4];
    float look[4];
    float gi[4];
    uint32_t giB[4];
};
static_assert(sizeof(Params) == 336);

// Matches `SharcPush` of shaders/rt_sharc_common.glsl.
struct SharcPush {
    uint64_t hash, accum, resolved;
    uint32_t capacity;
    float radianceScale;
    uint32_t accumFrames, staleMax, frame, pad;
    float cam[4], camPrev[4];
};
static_assert(sizeof(SharcPush) == 80);
constexpr float kRadianceScale = 1000.0f;

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

RtLit::RtLit(const gpu::Device& device, VkDescriptorSetLayout textureLayout) : device_(device) {
    const VkDevice dev = device_.device();
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(dev, &sci, nullptr, &sampler_);

    VkDescriptorSetLayoutBinding b[21]{};
    const VkShaderStageFlags cs = VK_SHADER_STAGE_COMPUTE_BIT;
    uint32_t n = 0;
    for (uint32_t i : {0u, 1u, 2u, 3u}) b[n++] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, cs, nullptr};
    for (uint32_t i : {4u, 5u, 7u, 8u, 9u, 10u, 13u, 14u, 15u, 19u, 20u}) b[n++] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, cs, nullptr};
    for (uint32_t i : {11u, 16u, 17u, 18u, 21u}) b[n++] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs, nullptr};
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

    const VkDescriptorSetLayout three[3] = {setLayout_, tlasLayout_, textureLayout};
    const VkPushConstantRange sharcRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SharcPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = textureLayout ? 3 : 2;
    pli.pSetLayouts = three;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &sharcRange;
    vkCreatePipelineLayout(dev, &pli, nullptr, &aoLayout_);
    VkPipelineLayoutCreateInfo rli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    rli.pushConstantRangeCount = 1;
    rli.pPushConstantRanges = &sharcRange;
    vkCreatePipelineLayout(dev, &rli, nullptr, &resolveLayout_);
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
    if (device_.caps().bounceCache) {
        updatePipeline_ = makeCompute(dev, aoLayout_, native::shaders::k_rt_sharc_update_comp_spv, sizeof(native::shaders::k_rt_sharc_update_comp_spv), "rt_sharc_update");
        resolvePipeline_ = makeCompute(dev, resolveLayout_, native::shaders::k_rt_sharc_resolve_comp_spv, sizeof(native::shaders::k_rt_sharc_resolve_comp_spv), "rt_sharc_resolve");
    }
}

RtLit::~RtLit() {
    const VkDevice dev = device_.device();
    for (VkPipeline p : {aoPipeline_, filterPipeline_, composePipeline_, updatePipeline_, resolvePipeline_})
        if (p) vkDestroyPipeline(dev, p, nullptr);
    if (resolveLayout_) vkDestroyPipelineLayout(dev, resolveLayout_, nullptr);
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
    for (int i = 0; i < 2; ++i) ok = ok && makeImage(device_, t.histD[i], VK_FORMAT_R32G32B32A32_SFLOAT, render, kStorage);
    for (int i = 0; i < 2; ++i) ok = ok && makeImage(device_, t.filD[i], VK_FORMAT_R16G16B16A16_SFLOAT, render, kStorage);
    ok = ok && makeImage(device_, t.sampleD, VK_FORMAT_R16G16B16A16_SFLOAT, render, kStorage);
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
        const VkDescriptorPoolSize sizes[5] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 24}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 12},
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

void RtLit::destroyCache(RtLitTargets& t) const {
    for (int i = 0; i < 3; ++i)
        if (t.cache.buffer[i]) vmaDestroyBuffer(device_.allocator(), t.cache.buffer[i], t.cache.alloc[i]);
    t.cache = RtLitTargets::Cache{};
}

// The three SHaRC buffers of a viewport (all zero at the start). False when the device cannot run it or the allocation failed (bounces stay off).
bool RtLit::ensureCache(RtLitTargets& t, const RtLitOptions& o) const {
    if (!o.bounces || !updatePipeline_ || !resolvePipeline_) {
        if (t.cache.valid()) destroyCache(t);
        return false;
    }
    const uint32_t capacity = 1u << std::clamp(o.giCacheLog2, 16u, 24u);
    if (t.cache.capacity == capacity) return true;
    destroyCache(t);
    const VkDeviceSize sizes[3] = {VkDeviceSize(capacity + 16) * 8, VkDeviceSize(capacity) * 16, VkDeviceSize(capacity) * 16};
    for (int i = 0; i < 3; ++i) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = sizes[i];
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateBuffer(device_.allocator(), &bci, &aci, &t.cache.buffer[i], &t.cache.alloc[i], nullptr) != VK_SUCCESS) {
            base::logWarning("RtLit: the bounce cache could not be allocated; bounce light is off for this viewport");
            destroyCache(t);
            return false;
        }
        VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        ai.buffer = t.cache.buffer[i];
        t.cache.address[i] = vkGetBufferDeviceAddress(device_.device(), &ai);
    }
    t.cache.capacity = capacity;
    t.cache.clear = true;
    t.cache.havePrev = false;
    return true;
}

void RtLit::destroyTargets(RtLitTargets& t) const {
    destroyCache(t);
    if (t.pool) vkDestroyDescriptorPool(device_.device(), t.pool, nullptr);
    for (auto& i : t.normal) destroyImage(device_, i);
    for (auto& i : t.hist) destroyImage(device_, i);
    for (auto& i : t.fil) destroyImage(device_, i);
    for (auto& i : t.histD) destroyImage(device_, i);
    for (auto& i : t.filD) destroyImage(device_, i);
    destroyImage(device_, t.sampleD);
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
            imageBarrier(cmd, t.histD[i].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, kAll, 0, kAll, kReadWrite);
            imageBarrier(cmd, t.filD[i].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, kAll, 0, kAll, kReadWrite);
            imageBarrier(cmd, t.normal[i].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kAll, 0, kAll, kReadWrite);
        }
        imageBarrier(cmd, t.sample.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, kAll, 0, kAll, kReadWrite);
        imageBarrier(cmd, t.sampleD.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, kAll, 0, kAll, kReadWrite);
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
    if (!valid() || !t.valid() || f.tlas == VK_NULL_HANDLE || !f.lights) return false;
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
        const RtLightData& li = f.lights->info();
        for (int i = 0; i < 3; ++i) {
            p.gridOrigin[i] = li.origin[i];
            p.gridDim[i] = li.dim[i];
        }
        p.gridOrigin[3] = li.cell;
        p.lightInfo[0] = li.lightCount;
        p.lightInfo[1] = li.globalCount;
        p.lightInfo[2] = li.lightCount > 0 ? 1u : 0u;
        p.look[0] = std::max(0.0f, f.options.lightmapTexelsPerMeter);
        p.look[1] = 0.01f;     // the bake shadow ray offset (light/passes.cpp)
        p.look[2] = 10000.f;   // the bake far distance
        const bool gi = ensureCache(t, f.options) && f.records != VK_NULL_HANDLE && f.textures != VK_NULL_HANDLE;
        p.gi[0] = f.options.giAlbedo;
        p.gi[1] = std::max(1.0f, f.options.giRange);
        p.gi[2] = gi ? 1.0f : 0.0f;
        p.gi[3] = static_cast<float>(std::clamp(f.options.giPathLength, 2u, 8u));
        p.giB[0] = std::clamp(f.options.giBlock, 1u, 16u);
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
    VkDescriptorImageInfo storage[11] = {{VK_NULL_HANDLE, t.hist[f.skipTrace ? other : histPrev].view, VK_IMAGE_LAYOUT_GENERAL},
                                        {VK_NULL_HANDLE, t.hist[histCur].view, VK_IMAGE_LAYOUT_GENERAL},
                                        {VK_NULL_HANDLE, t.fil[0].view, VK_IMAGE_LAYOUT_GENERAL},
                                        {VK_NULL_HANDLE, t.fil[1].view, VK_IMAGE_LAYOUT_GENERAL},
                                        {VK_NULL_HANDLE, outView, VK_IMAGE_LAYOUT_GENERAL},
                                        {VK_NULL_HANDLE, t.sample.view, VK_IMAGE_LAYOUT_GENERAL},
                                        {VK_NULL_HANDLE, t.histD[f.skipTrace ? other : histPrev].view, VK_IMAGE_LAYOUT_GENERAL},
                                        {VK_NULL_HANDLE, t.histD[histCur].view, VK_IMAGE_LAYOUT_GENERAL},
                                        {VK_NULL_HANDLE, t.sampleD.view, VK_IMAGE_LAYOUT_GENERAL},
                                        {VK_NULL_HANDLE, t.filD[0].view, VK_IMAGE_LAYOUT_GENERAL},
                                        {VK_NULL_HANDLE, t.filD[1].view, VK_IMAGE_LAYOUT_GENERAL}};
    const VkDescriptorBufferInfo lightInfos[3] = {{f.lights->records(), 0, f.lights->recordsSize()}, {f.lights->cells(), 0, f.lights->cellsSize()}, {f.lights->index(), 0, f.lights->indexSize()}};
    const VkDescriptorBufferInfo recordInfo{f.records != VK_NULL_HANDLE ? f.records : t.counters, 0, f.records != VK_NULL_HANDLE ? VK_WHOLE_SIZE : 8};
    const VkDescriptorBufferInfo counterInfo{t.counters, 0, 8};
    const VkDescriptorBufferInfo paramInfo{t.params[slot], 0, sizeof(Params)};
    VkWriteDescriptorSetAccelerationStructureKHR asw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    asw.accelerationStructureCount = 1;
    asw.pAccelerationStructures = &f.tlas;
    VkWriteDescriptorSet writes[26]{};
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
    const uint32_t storageBindings[11] = {4, 5, 7, 8, 9, 10, 13, 14, 15, 19, 20};
    for (uint32_t i = 0; i < 11; ++i) write(storageBindings[i], VK_DESCRIPTOR_TYPE_STORAGE_IMAGE).pImageInfo = &storage[i];
    write(11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER).pBufferInfo = &counterInfo;
    write(12, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER).pBufferInfo = &paramInfo;
    for (uint32_t i = 0; i < 3; ++i) write(16 + i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER).pBufferInfo = &lightInfos[i];
    write(21, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER).pBufferInfo = &recordInfo;
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
        const VkDescriptorSet sets3[3] = {t.sets[slot], t.tlasSets[slot], f.textures};
        const bool gi = p.gi[2] > 0.5f;
        SharcPush sp{};
        if (gi) {   // bounce light: the cache is zeroed when it is new or the light changed, then update -> resolve; the trace kernel below only reads it
            if (t.cache.clear || f.reset) {
                for (int i = 0; i < 3; ++i) vkCmdFillBuffer(cmd, t.cache.buffer[i], 0, VK_WHOLE_SIZE, 0);
                memoryBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, kAll, kReadWrite);
                t.cache.clear = false;
            }
            sp.hash = t.cache.address[0];
            sp.accum = t.cache.address[1];
            sp.resolved = t.cache.address[2];
            sp.capacity = t.cache.capacity;
            sp.radianceScale = kRadianceScale;
            sp.accumFrames = f.options.giAccumFrames;
            sp.staleMax = f.options.giStaleFrames;
            sp.frame = t.cache.frame++;
            for (int i = 0; i < 3; ++i) {
                sp.cam[i] = static_cast<float>(f.eye[i]);
                sp.camPrev[i] = t.cache.havePrev ? t.cache.camPrev[i] : static_cast<float>(f.eye[i]);
                t.cache.camPrev[i] = static_cast<float>(f.eye[i]);
            }
            sp.cam[3] = f.options.giSceneScale;
            t.cache.havePrev = true;
            const uint32_t block = std::clamp(f.options.giBlock, 1u, 16u);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, updatePipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, aoLayout_, 0, 3, sets3, 0, nullptr);
            vkCmdPushConstants(cmd, aoLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(sp), &sp);
            vkCmdDispatch(cmd, ((w + block - 1) / block + 7) / 8, ((h + block - 1) / block + 7) / 8, 1);
            memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resolvePipeline_);
            vkCmdPushConstants(cmd, resolveLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(sp), &sp);
            vkCmdDispatch(cmd, (t.cache.capacity + 63) / 64, 1, 1);
            memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, aoPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, aoLayout_, 0, 3, sets3, 0, nullptr);
        vkCmdPushConstants(cmd, aoLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(sp), &sp);
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
