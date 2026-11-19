// render/scene_pass -- see scene_pass.h.
#include "render/scene_pass.h"

#include <cstring>

#include "base/log.h"
#include "native_shaders/viewport_box_vert.spv.h"
#include "native_shaders/viewport_grid_vert.spv.h"
#include "native_shaders/viewport_scene_frag.spv.h"

namespace native::render {

namespace {

constexpr int kGridHalf = 100;                                   // must match HALF in viewport_grid.vert
constexpr uint32_t kGridVertices = static_cast<uint32_t>(2 * (2 * kGridHalf + 1) * 2);

// Matches the push-constant block of the vertex shaders.
struct PushConstants {
    float viewProj[16];
    float grid[4];    // spacing, plane, centre a, centre b
    float misc[4];    // mode (0 grid, 1 axes)
};
static_assert(sizeof(PushConstants) == 96);

VkShaderModule makeModule(VkDevice dev, const uint32_t* words, size_t sizeBytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = sizeBytes;
    ci.pCode = words;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, nullptr, &m);
    return m;
}

VkPipeline makePipeline(VkDevice dev, VkPipelineLayout layout, VkFormat format, VkFormat depthFormat, VkShaderModule vs, VkShaderModule fs,
                        VkPrimitiveTopology topology, VkCullModeFlags cull, bool depthWrite) {
    VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                              {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = topology;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = cull;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = depthWrite ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.blendEnable = VK_TRUE;
    blendAtt.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAtt.colorBlendOp = VK_BLEND_OP_ADD;
    blendAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendAtt.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blendAtt;
    const VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;
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
        base::logError("ScenePass: pipeline creation failed");
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

} // namespace

ScenePass::ScenePass(const gpu::Device& device, VkFormat colorFormat, VkFormat depthFormat) : device_(device), format_(colorFormat) {
    const VkDevice dev = device_.device();
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &range;
    vkCreatePipelineLayout(dev, &lci, nullptr, &layout_);

    VkShaderModule gridVs = makeModule(dev, native::shaders::k_viewport_grid_vert_spv, sizeof(native::shaders::k_viewport_grid_vert_spv));
    VkShaderModule boxVs = makeModule(dev, native::shaders::k_viewport_box_vert_spv, sizeof(native::shaders::k_viewport_box_vert_spv));
    VkShaderModule fs = makeModule(dev, native::shaders::k_viewport_scene_frag_spv, sizeof(native::shaders::k_viewport_scene_frag_spv));
    gridPipeline_ = makePipeline(dev, layout_, format_, depthFormat, gridVs, fs, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_CULL_MODE_NONE, false);
    boxPipeline_ = makePipeline(dev, layout_, format_, depthFormat, boxVs, fs, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_CULL_MODE_BACK_BIT, true);
    vkDestroyShaderModule(dev, gridVs, nullptr);
    vkDestroyShaderModule(dev, boxVs, nullptr);
    vkDestroyShaderModule(dev, fs, nullptr);
}

ScenePass::~ScenePass() {
    if (gridPipeline_) vkDestroyPipeline(device_.device(), gridPipeline_, nullptr);
    if (boxPipeline_) vkDestroyPipeline(device_.device(), boxPipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(device_.device(), layout_, nullptr);
}

void ScenePass::record(VkCommandBuffer cmd, VkExtent2D extent, const ViewState& view, bool drawBox) const {
    if (!valid()) return;
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    PushConstants pc{};
    for (int i = 0; i < 16; ++i) pc.viewProj[i] = static_cast<float>(view.viewProj[i]);
    pc.grid[0] = static_cast<float>(view.gridSpacing);
    pc.grid[1] = static_cast<float>(view.gridPlane);
    pc.grid[2] = static_cast<float>(view.gridCenterA);
    pc.grid[3] = static_cast<float>(view.gridCenterB);

    if (view.showGrid) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gridPipeline_);
        pc.misc[0] = 0;
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        vkCmdDraw(cmd, kGridVertices, 1, 0, 0);
    }
    if (drawBox) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, boxPipeline_);
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        vkCmdDraw(cmd, 36, 1, 0, 0);
    }
    if (view.showAxes) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gridPipeline_);
        pc.misc[0] = 1;
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        vkCmdDraw(cmd, 6, 1, 0, 0);
    }
}

} // namespace native::render
