// render/model_pass -- see model_pass.h.
#include "render/model_pass.h"

#include <cstddef>

#include "base/log.h"
#include "native_shaders/model_frag.spv.h"
#include "native_shaders/model_gbuf_frag.spv.h"
#include "native_shaders/model_vert.spv.h"
#include "render/mesh_pass.h"

namespace native::render {

namespace {

struct ModelPush {
    float viewProj[16];
    float color[4];
    float mat[4];   // texture index, has texture, alpha mode, cutoff
    float lit[4];
};
static_assert(sizeof(ModelPush) == 112);

VkShaderModule makeModule(VkDevice dev, const uint32_t* words, size_t sizeBytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = sizeBytes;
    ci.pCode = words;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, nullptr, &m);
    return m;
}

VkPipeline makePipeline(VkDevice dev, VkPipelineLayout layout, VkFormat colorFormat, VkFormat depthFormat, VkShaderModule vs, VkShaderModule fs, bool gbuffer) {
    VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    const VkVertexInputBindingDescription bindings[2] = {{0, sizeof(ModelVertex), VK_VERTEX_INPUT_RATE_VERTEX}, {1, kModelInstanceBytes, VK_VERTEX_INPUT_RATE_INSTANCE}};
    const VkVertexInputAttributeDescription attrs[7] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(ModelVertex, pos))},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(ModelVertex, normal))},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(ModelVertex, uv))},
        {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
        {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
        {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
        {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 2;
    vi.pVertexBindingDescriptions = bindings;
    vi.vertexAttributeDescriptionCount = 7;
    vi.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;   // thin models and mirrored scales: both sides are drawn, the shader flips the normal of a back face
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    const VkPipelineColorBlendAttachmentState attachments[3] = {att, att, att};
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = gbuffer ? 3 : 1;
    cb.pAttachments = attachments;
    const VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    const VkFormat gFormats[3] = {colorFormat, kGBufferNormalFormat, kGBufferDistFormat};
    rendering.colorAttachmentCount = gbuffer ? 3 : 1;
    rendering.pColorAttachmentFormats = gbuffer ? gFormats : &colorFormat;
    rendering.depthAttachmentFormat = depthFormat;
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
    pi.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline) != VK_SUCCESS) {
        base::logError("ModelPass: pipeline creation failed");
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

} // namespace

ModelPass::ModelPass(const gpu::Device& device, VkPipelineLayout layout, VkFormat colorFormat, VkFormat depthFormat) : device_(device), layout_(layout) {
    const VkDevice dev = device_.device();
    VkShaderModule vs = makeModule(dev, native::shaders::k_model_vert_spv, sizeof(native::shaders::k_model_vert_spv));
    VkShaderModule fs = makeModule(dev, native::shaders::k_model_frag_spv, sizeof(native::shaders::k_model_frag_spv));
    VkShaderModule gfs = makeModule(dev, native::shaders::k_model_gbuf_frag_spv, sizeof(native::shaders::k_model_gbuf_frag_spv));
    forward_ = makePipeline(dev, layout_, colorFormat, depthFormat, vs, fs, false);
    gbuffer_ = makePipeline(dev, layout_, colorFormat, depthFormat, vs, gfs, true);
    vkDestroyShaderModule(dev, vs, nullptr);
    vkDestroyShaderModule(dev, fs, nullptr);
    vkDestroyShaderModule(dev, gfs, nullptr);
}

ModelPass::~ModelPass() {
    const VkDevice dev = device_.device();
    for (VkPipeline p : {forward_, gbuffer_})
        if (p) vkDestroyPipeline(dev, p, nullptr);
}

void ModelPass::record(VkCommandBuffer cmd, const ViewState& view, const float lit[4], const ModelDrawList& models, bool gbuffer) const {
    if (!valid() || models.draws.empty()) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gbuffer ? gbuffer_ : forward_);
    ModelPush pc{};
    for (int i = 0; i < 16; ++i) pc.viewProj[i] = static_cast<float>(view.viewProj[i]);
    for (int i = 0; i < 4; ++i) pc.lit[i] = lit[i];
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (const ModelDraw& d : models.draws) {
        if (d.indexCount == 0 || d.instanceCount == 0) continue;
        for (int i = 0; i < 4; ++i) pc.color[i] = d.color[i];
        pc.mat[0] = d.texture;
        pc.mat[1] = d.hasTexture;
        pc.mat[2] = d.alphaMode;
        pc.mat[3] = d.cutoff;
        vkCmdPushConstants(cmd, layout_, stages, 0, sizeof(pc), &pc);
        const VkBuffer buffers[2] = {d.vertexBuffer, d.instanceBuffer};
        const VkDeviceSize offsets[2] = {d.vertexOffset, d.instanceOffset};
        vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
        vkCmdBindIndexBuffer(cmd, d.indexBuffer, d.indexOffset, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, d.indexCount, d.instanceCount, 0, 0, 0);
    }
}

} // namespace native::render
