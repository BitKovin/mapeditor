// render/dlss_pass -- see dlss_pass.h.
#include "render/dlss_pass.h"

#include <cstring>

#include "base/log.h"
#include "native_shaders/dlss_mvec_comp.spv.h"
#include "native_shaders/dlss_resolve_frag.spv.h"
#include "native_shaders/dlss_resolve_vert.spv.h"

namespace native::render {

namespace {

struct PushConstants {
    float reproject[16];
    float info[4];   // render width, render height, jitter x, jitter y
};
static_assert(sizeof(PushConstants) == 80);

VkShaderModule makeModule(VkDevice dev, const uint32_t* words, size_t sizeBytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = sizeBytes;
    ci.pCode = words;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, nullptr, &m);
    return m;
}

void barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage,
             VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
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
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

constexpr VkPipelineStageFlags2 kAllStages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
constexpr VkAccessFlags2 kAnyRead = VK_ACCESS_2_MEMORY_READ_BIT;
constexpr VkAccessFlags2 kAnyAccess = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

bool makeImage(const gpu::Device& device, DlssTargets::Image& img, VkFormat format, VkExtent2D extent, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
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
    img.usage = usage;
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {aspect, 0, 1, 0, 1};
    return vkCreateImageView(device.device(), &vci, nullptr, &img.view) == VK_SUCCESS;
}

void destroyImage(const gpu::Device& device, DlssTargets::Image& img) {
    if (img.view) vkDestroyImageView(device.device(), img.view, nullptr);
    if (img.image) vmaDestroyImage(device.allocator(), img.image, img.allocation);
    img = {};
}

} // namespace

DlssPass::DlssPass(const gpu::Device& device, VkFormat swapchainFormat, VkFormat depthFormat)
    : device_(device), swapchainFormat_(swapchainFormat), depthFormat_(depthFormat) {
    const VkDevice dev = device_.device();
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(dev, &sci, nullptr, &sampler_);

    {   // motion vectors: depth in, motion vectors out
        VkDescriptorSetLayoutBinding b[2]{};
        b[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 2;
        lci.pBindings = b;
        vkCreateDescriptorSetLayout(dev, &lci, nullptr, &mvecLayout_);
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &mvecLayout_;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &range;
        vkCreatePipelineLayout(dev, &pli, nullptr, &mvecPipelineLayout_);
        VkShaderModule cs = makeModule(dev, native::shaders::k_dlss_mvec_comp_spv, sizeof(native::shaders::k_dlss_mvec_comp_spv));
        VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main", nullptr};
        cpi.layout = mvecPipelineLayout_;
        if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &mvecPipeline_) != VK_SUCCESS) base::logError("DlssPass: motion vector pipeline failed");
        vkDestroyShaderModule(dev, cs, nullptr);
    }
    {   // resolve: DLSS output + render-size depth in, the swapchain image + the output-size depth out
        VkDescriptorSetLayoutBinding b[2]{};
        b[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        b[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 2;
        lci.pBindings = b;
        vkCreateDescriptorSetLayout(dev, &lci, nullptr, &resolveLayout_);
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &resolveLayout_;
        vkCreatePipelineLayout(dev, &pli, nullptr, &resolvePipelineLayout_);
        VkShaderModule vs = makeModule(dev, native::shaders::k_dlss_resolve_vert_spv, sizeof(native::shaders::k_dlss_resolve_vert_spv));
        VkShaderModule fs = makeModule(dev, native::shaders::k_dlss_resolve_frag_spv, sizeof(native::shaders::k_dlss_resolve_frag_spv));
        VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = VK_TRUE;   // depth writes need the test on: it always passes, the shader's depth is stored
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        VkPipelineColorBlendAttachmentState att{};
        att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &att;
        const VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dynStates;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &swapchainFormat_;
        rendering.depthAttachmentFormat = depthFormat_;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pi.pNext = &rendering;
        pi.stageCount = 2;
        pi.pStages = stages;
        pi.pVertexInputState = &vi;
        pi.pInputAssemblyState = &ia;
        pi.pViewportState = &vp;
        pi.pRasterizationState = &rs;
        pi.pMultisampleState = &ms;
        pi.pDepthStencilState = &ds;
        pi.pColorBlendState = &cb;
        pi.pDynamicState = &dyn;
        pi.layout = resolvePipelineLayout_;
        if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr, &resolvePipeline_) != VK_SUCCESS) base::logError("DlssPass: resolve pipeline failed");
        vkDestroyShaderModule(dev, vs, nullptr);
        vkDestroyShaderModule(dev, fs, nullptr);
    }
}

DlssPass::~DlssPass() {
    const VkDevice dev = device_.device();
    if (mvecPipeline_) vkDestroyPipeline(dev, mvecPipeline_, nullptr);
    if (resolvePipeline_) vkDestroyPipeline(dev, resolvePipeline_, nullptr);
    if (mvecPipelineLayout_) vkDestroyPipelineLayout(dev, mvecPipelineLayout_, nullptr);
    if (resolvePipelineLayout_) vkDestroyPipelineLayout(dev, resolvePipelineLayout_, nullptr);
    if (mvecLayout_) vkDestroyDescriptorSetLayout(dev, mvecLayout_, nullptr);
    if (resolveLayout_) vkDestroyDescriptorSetLayout(dev, resolveLayout_, nullptr);
    if (sampler_) vkDestroySampler(dev, sampler_, nullptr);
}

bool DlssPass::createTargets(DlssTargets& t, VkExtent2D render, VkExtent2D out) const {
    destroyTargets(t);
    t.render = render;
    t.out = out;
    constexpr VkImageUsageFlags kTransfer = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    bool ok = makeImage(device_, t.color, t.colorFormat, render,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | kTransfer, VK_IMAGE_ASPECT_COLOR_BIT);
    ok = ok && makeImage(device_, t.depth, t.depthFormat, render, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | kTransfer,
                         VK_IMAGE_ASPECT_DEPTH_BIT);
    ok = ok && makeImage(device_, t.mvec, t.mvecFormat, render,
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | kTransfer, VK_IMAGE_ASPECT_COLOR_BIT);
    ok = ok && makeImage(device_, t.output, t.colorFormat, out,
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | kTransfer, VK_IMAGE_ASPECT_COLOR_BIT);
    if (ok) {
        VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = 2;
        pci.poolSizeCount = 2;
        pci.pPoolSizes = sizes;
        ok = vkCreateDescriptorPool(device_.device(), &pci, nullptr, &t.pool) == VK_SUCCESS;
    }
    if (ok) {
        const VkDescriptorSetLayout layouts[2] = {mvecLayout_, resolveLayout_};
        VkDescriptorSet sets[2] = {};
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = t.pool;
        ai.descriptorSetCount = 2;
        ai.pSetLayouts = layouts;
        ok = vkAllocateDescriptorSets(device_.device(), &ai, sets) == VK_SUCCESS;
        t.mvecSet = sets[0];
        t.resolveSet = sets[1];
    }
    if (ok) {
        const VkDescriptorImageInfo depthIn{sampler_, t.depth.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo mvecOut{VK_NULL_HANDLE, t.mvec.view, VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo colorIn{sampler_, t.output.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w[4]{};
        for (VkWriteDescriptorSet& x : w) {
            x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            x.descriptorCount = 1;
        }
        w[0].dstSet = t.mvecSet;
        w[0].dstBinding = 0;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &depthIn;
        w[1].dstSet = t.mvecSet;
        w[1].dstBinding = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[1].pImageInfo = &mvecOut;
        w[2].dstSet = t.resolveSet;
        w[2].dstBinding = 0;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[2].pImageInfo = &colorIn;
        w[3].dstSet = t.resolveSet;
        w[3].dstBinding = 1;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[3].pImageInfo = &depthIn;
        vkUpdateDescriptorSets(device_.device(), 4, w, 0, nullptr);
    }
    if (!ok) {
        base::logError("DlssPass: the render targets could not be created");
        destroyTargets(t);
    }
    return ok;
}

void DlssPass::destroyTargets(DlssTargets& t) const {
    if (t.pool) vkDestroyDescriptorPool(device_.device(), t.pool, nullptr);
    destroyImage(device_, t.color);
    destroyImage(device_, t.depth);
    destroyImage(device_, t.mvec);
    destroyImage(device_, t.output);
    t.pool = VK_NULL_HANDLE;
    t.mvecSet = t.resolveSet = VK_NULL_HANDLE;
    t.render = t.out = {0, 0};
}

void DlssPass::beginScene(VkCommandBuffer cmd, const DlssTargets& t) const {
    barrier(cmd, t.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kAllStages, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    barrier(cmd, t.depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, kAllStages, 0,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
}

void DlssPass::endSceneAndMotion(VkCommandBuffer cmd, const DlssTargets& t, const float reproject[16], float jitterX, float jitterY) const {
    barrier(cmd, t.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kAllStages, kAnyRead);
    barrier(cmd, t.depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            kAllStages, kAnyRead);
    barrier(cmd, t.mvec.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, kAllStages, 0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    PushConstants pc{};
    std::memcpy(pc.reproject, reproject, sizeof(pc.reproject));
    pc.info[0] = static_cast<float>(t.render.width);
    pc.info[1] = static_cast<float>(t.render.height);
    pc.info[2] = jitterX;
    pc.info[3] = jitterY;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mvecPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mvecPipelineLayout_, 0, 1, &t.mvecSet, 0, nullptr);
    vkCmdPushConstants(cmd, mvecPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (t.render.width + 7) / 8, (t.render.height + 7) / 8, 1);
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = kAllStages;
    mb.dstAccessMask = kAnyAccess;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void DlssPass::beforeUpscale(VkCommandBuffer cmd, const DlssTargets& t) const {
    barrier(cmd, t.output.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, kAllStages, 0, kAllStages, kAnyAccess);
}

void DlssPass::afterUpscale(VkCommandBuffer cmd, const DlssTargets& t) const {
    barrier(cmd, t.output.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kAllStages,
            VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void DlssPass::resolve(VkCommandBuffer cmd, const DlssTargets& t) const {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, resolvePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, resolvePipelineLayout_, 0, 1, &t.resolveSet, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

namespace {
gpu::DlssImage describe(const DlssTargets::Image& img, VkFormat format, VkExtent2D extent, VkImageLayout layout) {
    gpu::DlssImage d;
    d.image = img.image;
    d.view = img.view;
    d.memory = img.memory;
    d.format = format;
    d.usage = img.usage;
    d.layout = layout;
    d.width = extent.width;
    d.height = extent.height;
    return d;
}
} // namespace

gpu::DlssImage DlssPass::color(const DlssTargets& t) { return describe(t.color, t.colorFormat, t.render, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); }
gpu::DlssImage DlssPass::depth(const DlssTargets& t) { return describe(t.depth, t.depthFormat, t.render, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL); }
gpu::DlssImage DlssPass::mvec(const DlssTargets& t) { return describe(t.mvec, t.mvecFormat, t.render, VK_IMAGE_LAYOUT_GENERAL); }
gpu::DlssImage DlssPass::output(const DlssTargets& t) { return describe(t.output, t.colorFormat, t.out, VK_IMAGE_LAYOUT_GENERAL); }

} // namespace native::render
