// render/mesh_pass -- see mesh_pass.h.
#include "render/mesh_pass.h"

#include <cstddef>
#include <cstring>

#include "base/log.h"
#include "native_shaders/label_frag.spv.h"
#include "native_shaders/label_vert.spv.h"
#include "native_shaders/mesh_frag.spv.h"
#include "native_shaders/mesh_gbuf_frag.spv.h"
#include "native_shaders/mesh_glass_frag.spv.h"
#include "native_shaders/mesh_vert.spv.h"
#include "native_shaders/overlay_frag.spv.h"
#include "native_shaders/overlay_line_frag.spv.h"
#include "native_shaders/overlay_line_vert.spv.h"
#include "native_shaders/overlay_vert.spv.h"
#include "native_shaders/sky_bg_vert.spv.h"
#include "render/glyph_atlas.h"
#include "scene/render_mesh.h"

namespace native::render {

namespace {

struct PushConstants {
    float viewProj[16];
    float eye[4];
    float misc[4];   // x = pass (0 opaque, 1 translucent, 2 volume, 3 sky background), y = sky texture index
    float lit[4];    // x = mode (0 unlit, 1 lit, 2 lighting only), y = exposure, z = brightness, w = texture lod bias
};
static_assert(sizeof(PushConstants) == 112);

constexpr float kLineWidthPx = 1.5f;   // the width of the overlay lines (anti-aliased, so a fraction of a pixel is fine)

VkShaderModule makeModule(VkDevice dev, const uint32_t* words, size_t sizeBytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = sizeBytes;
    ci.pCode = words;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, nullptr, &m);
    return m;
}

struct PipelineSpec {
    bool vertexInput = true;
    bool overlay = false;      // coloured vertices (LineVertex), 16 bytes a vertex
    bool triangles = false;    // overlay: triangles instead of lines
    bool aaLines = false;      // overlay: anti-aliased lines, one instance (two LineVertex) of six vertices a segment (shaders/overlay_line.vert)
    bool labels = false;       // text quads (LabelVertex), 32 bytes a vertex
    bool depthTest = true;
    bool depthBias = false;
    bool blend = false;
    bool depthWrite = true;
    bool gbuffer = false;      // three colour attachments: albedo (colorFormat), normal RGBA16F, distance R32F (the ray traced Lit path)
    VkCompareOp depthOp = VK_COMPARE_OP_LESS;
};

VkPipeline makePipeline(VkDevice dev, VkPipelineLayout layout, VkFormat colorFormat, VkFormat depthFormat, VkShaderModule vs, VkShaderModule fs,
                        const PipelineSpec& spec) {
    VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{0, sizeof(scene::RenderVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputBindingDescription lineBinding{0, sizeof(LineVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription lineAttrs[2] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 12}};
    VkVertexInputBindingDescription segBinding{0, 2 * sizeof(LineVertex), VK_VERTEX_INPUT_RATE_INSTANCE};
    VkVertexInputAttributeDescription segAttrs[4] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 12},
                                                    {2, 0, VK_FORMAT_R32G32B32_SFLOAT, 16}, {3, 0, VK_FORMAT_R8G8B8A8_UNORM, 28}};
    VkVertexInputAttributeDescription attrs[6] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(scene::RenderVertex, pos))},
        {1, 0, VK_FORMAT_R32_UINT, static_cast<uint32_t>(offsetof(scene::RenderVertex, normal))},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(scene::RenderVertex, uv))},
        {3, 0, VK_FORMAT_R32_UINT, static_cast<uint32_t>(offsetof(scene::RenderVertex, page))},   // page (low 16) and flags (high 16) as one word
        {4, 0, VK_FORMAT_R32_UINT, static_cast<uint32_t>(offsetof(scene::RenderVertex, texture))},
        {5, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(scene::RenderVertex, lm))},
    };
    VkVertexInputBindingDescription labelBinding{0, sizeof(LabelVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription labelAttrs[4] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 12}, {2, 0, VK_FORMAT_R32G32_SFLOAT, 20}, {3, 0, VK_FORMAT_R8G8B8A8_UNORM, 28}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (spec.labels) {
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &labelBinding;
        vi.vertexAttributeDescriptionCount = 4;
        vi.pVertexAttributeDescriptions = labelAttrs;
    } else if (spec.overlay && spec.aaLines) {
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &segBinding;
        vi.vertexAttributeDescriptionCount = 4;
        vi.pVertexAttributeDescriptions = segAttrs;
    } else if (spec.overlay) {
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &lineBinding;
        vi.vertexAttributeDescriptionCount = 2;
        vi.pVertexAttributeDescriptions = lineAttrs;
    } else if (spec.vertexInput) {
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = 6;
        vi.pVertexAttributeDescriptions = attrs;
    }
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = spec.overlay && !spec.triangles && !spec.labels && !spec.aaLines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;   // faces are seen from both sides (the backface culling setting comes later)
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    if (spec.depthBias) {
        rs.depthBiasEnable = VK_TRUE;
        rs.depthBiasConstantFactor = -4.0f;   // lines on a surface win the depth test against it
        rs.depthBiasSlopeFactor = -2.0f;
    }
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = spec.depthTest ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = spec.depthWrite ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = spec.depthOp;
    VkPipelineColorBlendAttachmentState att{};
    att.blendEnable = spec.blend ? VK_TRUE : VK_FALSE;
    att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    att.colorBlendOp = VK_BLEND_OP_ADD;
    att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    att.alphaBlendOp = VK_BLEND_OP_ADD;
    att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    const VkPipelineColorBlendAttachmentState attachments[3] = {att, att, att};
    cb.attachmentCount = spec.gbuffer ? 3 : 1;
    cb.pAttachments = attachments;
    const VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    const VkFormat gFormats[3] = {colorFormat, kGBufferNormalFormat, kGBufferDistFormat};
    rendering.colorAttachmentCount = spec.gbuffer ? 3 : 1;
    rendering.pColorAttachmentFormats = spec.gbuffer ? gFormats : &colorFormat;
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
        base::logError("MeshPass: pipeline creation failed");
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

} // namespace

MeshPass::MeshPass(const gpu::Device& device, VkFormat colorFormat, VkFormat depthFormat, VkDescriptorSetLayout textureLayout, VkDescriptorSetLayout lightmapLayout)
    : device_(device), colorFormat_(colorFormat), depthFormat_(depthFormat), textureLayout_(textureLayout), lightmapLayout_(lightmapLayout) {
    const VkDevice dev = device_.device();
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    const VkDescriptorSetLayout setLayouts[2] = {textureLayout, lightmapLayout};
    lci.setLayoutCount = 2;
    lci.pSetLayouts = setLayouts;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &range;
    vkCreatePipelineLayout(dev, &lci, nullptr, &layout_);

    VkShaderModule vs = makeModule(dev, native::shaders::k_mesh_vert_spv, sizeof(native::shaders::k_mesh_vert_spv));
    VkShaderModule bg = makeModule(dev, native::shaders::k_sky_bg_vert_spv, sizeof(native::shaders::k_sky_bg_vert_spv));
    VkShaderModule fs = makeModule(dev, native::shaders::k_mesh_frag_spv, sizeof(native::shaders::k_mesh_frag_spv));
    VkShaderModule ovs = makeModule(dev, native::shaders::k_overlay_vert_spv, sizeof(native::shaders::k_overlay_vert_spv));
    VkShaderModule ofs = makeModule(dev, native::shaders::k_overlay_frag_spv, sizeof(native::shaders::k_overlay_frag_spv));
    VkShaderModule alvs = makeModule(dev, native::shaders::k_overlay_line_vert_spv, sizeof(native::shaders::k_overlay_line_vert_spv));
    VkShaderModule alfs = makeModule(dev, native::shaders::k_overlay_line_frag_spv, sizeof(native::shaders::k_overlay_line_frag_spv));
    PipelineSpec world;
    world.overlay = true;
    world.blend = true;
    world.depthWrite = false;
    world.depthOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    world.depthBias = true;
    world.aaLines = true;
    worldLines_ = makePipeline(dev, layout_, colorFormat_, depthFormat_, alvs, alfs, world);
    PipelineSpec screen = world;
    screen.depthTest = false;
    screen.depthBias = false;
    screenLines_ = makePipeline(dev, layout_, colorFormat_, depthFormat_, alvs, alfs, screen);
    PipelineSpec plain = world;
    plain.depthBias = false;
    plainLines_ = makePipeline(dev, layout_, colorFormat_, depthFormat_, alvs, alfs, plain);
    PipelineSpec top = plain;
    top.depthTest = false;
    topLines_ = makePipeline(dev, layout_, colorFormat_, depthFormat_, alvs, alfs, top);
    PipelineSpec fillSpec = plain;
    fillSpec.triangles = true;
    fillSpec.aaLines = false;
    fills_ = makePipeline(dev, layout_, colorFormat_, depthFormat_, ovs, ofs, fillSpec);
    VkShaderModule lvs = makeModule(dev, native::shaders::k_label_vert_spv, sizeof(native::shaders::k_label_vert_spv));
    VkShaderModule lfs = makeModule(dev, native::shaders::k_label_frag_spv, sizeof(native::shaders::k_label_frag_spv));
    PipelineSpec labelSpec = plain;
    labelSpec.labels = true;
    labels_ = makePipeline(dev, layout_, colorFormat_, depthFormat_, lvs, lfs, labelSpec);
    PipelineSpec over = labelSpec;
    over.depthTest = false;
    labelsOver_ = makePipeline(dev, layout_, colorFormat_, depthFormat_, lvs, lfs, over);
    vkDestroyShaderModule(dev, lvs, nullptr);
    vkDestroyShaderModule(dev, lfs, nullptr);
    vkDestroyShaderModule(dev, ovs, nullptr);
    vkDestroyShaderModule(dev, ofs, nullptr);
    vkDestroyShaderModule(dev, alvs, nullptr);
    vkDestroyShaderModule(dev, alfs, nullptr);
    PipelineSpec opaque;
    opaque_ = makePipeline(dev, layout_, colorFormat_, depthFormat_, vs, fs, opaque);
    PipelineSpec blended;
    blended.blend = true;
    blended.depthWrite = false;
    translucent_ = makePipeline(dev, layout_, colorFormat_, depthFormat_, vs, fs, blended);
    volume_ = makePipeline(dev, layout_, colorFormat_, depthFormat_, vs, fs, blended);
    PipelineSpec sky;
    sky.vertexInput = false;
    sky.depthWrite = false;
    sky.depthOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    skyBg_ = makePipeline(dev, layout_, colorFormat_, depthFormat_, bg, fs, sky);
    {   // the G-buffer variants of the opaque and sky background draws (R2)
        VkShaderModule gfs = makeModule(dev, native::shaders::k_mesh_gbuf_frag_spv, sizeof(native::shaders::k_mesh_gbuf_frag_spv));
        PipelineSpec gOpaque;
        gOpaque.gbuffer = true;
        gBufferOpaque_ = makePipeline(dev, layout_, colorFormat_, depthFormat_, vs, gfs, gOpaque);
        PipelineSpec gSky = sky;
        gSky.gbuffer = true;
        gBufferSky_ = makePipeline(dev, layout_, colorFormat_, depthFormat_, bg, gfs, gSky);
        vkDestroyShaderModule(dev, gfs, nullptr);
    }
    vkDestroyShaderModule(dev, vs, nullptr);
    vkDestroyShaderModule(dev, bg, nullptr);
    vkDestroyShaderModule(dev, fs, nullptr);
    models_ = std::make_unique<ModelPass>(device_, layout_, colorFormat_, depthFormat_);
}

bool MeshPass::enableGlass(VkDescriptorSetLayout lit, VkDescriptorSetLayout tlas) {
    if (glassTranslucent_) return true;
    if (!device_.caps().bounceCache || lit == VK_NULL_HANDLE || tlas == VK_NULL_HANDLE) return false;
    const VkDevice dev = device_.device();
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
    const VkDescriptorSetLayout setLayouts[4] = {textureLayout_, lightmapLayout_, lit, tlas};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 4;
    lci.pSetLayouts = setLayouts;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(dev, &lci, nullptr, &glassLayout_) != VK_SUCCESS) return false;
    VkShaderModule vs = makeModule(dev, native::shaders::k_mesh_vert_spv, sizeof(native::shaders::k_mesh_vert_spv));
    VkShaderModule fs = makeModule(dev, native::shaders::k_mesh_glass_frag_spv, sizeof(native::shaders::k_mesh_glass_frag_spv));
    PipelineSpec blended;
    blended.blend = true;
    blended.depthWrite = false;
    glassTranslucent_ = makePipeline(dev, glassLayout_, colorFormat_, depthFormat_, vs, fs, blended);
    vkDestroyShaderModule(dev, vs, nullptr);
    vkDestroyShaderModule(dev, fs, nullptr);
    if (!glassTranslucent_) base::logWarning("MeshPass: the translucent surface pipeline failed; glass stays unlit");
    return glassTranslucent_ != VK_NULL_HANDLE;
}

MeshPass::~MeshPass() {
    models_.reset();   // before the layout it uses
    const VkDevice dev = device_.device();
    if (glassTranslucent_) vkDestroyPipeline(dev, glassTranslucent_, nullptr);
    if (glassLayout_) vkDestroyPipelineLayout(dev, glassLayout_, nullptr);
    for (VkPipeline p : {opaque_, translucent_, volume_, skyBg_, gBufferOpaque_, gBufferSky_, worldLines_, screenLines_, plainLines_, topLines_, fills_, labels_, labelsOver_})
        if (p) vkDestroyPipeline(dev, p, nullptr);
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
}

void MeshPass::record(VkCommandBuffer cmd, const ViewState& view, const GpuScene& scene, const DrawLists& lists, VkDescriptorSet textures,
                      uint32_t skyTexture, bool showVolumes, const FrameOverlays& ov, VkDescriptorSet lightmaps, const LitParams& lit, int parts, const GlassSets& glass) const {
    if (!valid()) return;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &textures, 0, nullptr);
    if (lightmaps) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 1, 1, &lightmaps, 0, nullptr);
    PushConstants pc{};
    pc.lit[0] = lightmaps && !(parts & (kMeshGBuffer | kMeshBlended)) ? static_cast<float>(lit.mode) : 0.f;
    pc.lit[1] = lit.exposure;
    pc.lit[2] = lit.brightness;
    pc.lit[3] = lit.lodBias;
    for (int i = 0; i < 16; ++i) pc.viewProj[i] = static_cast<float>(view.viewProj[i]);
    for (int i = 0; i < 3; ++i) pc.eye[i] = static_cast<float>(view.eye[i]);
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    const bool gbuf = (parts & kMeshGBuffer) != 0;
    if ((parts & (kMeshScene | kMeshGBuffer)) && skyTexture != 0 && view.perspective && !(parts & kMeshBlended)) {
        const Mat4 inv = mat4Invert(view.viewProj);
        PushConstants bgc = pc;
        for (int i = 0; i < 16; ++i) bgc.viewProj[i] = static_cast<float>(inv[i]);
        bgc.misc[0] = 3;
        bgc.misc[1] = static_cast<float>(skyTexture);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gbuf ? gBufferSky_ : skyBg_);
        vkCmdPushConstants(cmd, layout_, stages, 0, sizeof(bgc), &bgc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    const auto lines = [&](VkPipeline pipeline, const LineList& list, const Mat4& m) {
        if (list.count == 0) return;
        PushConstants p = pc;
        for (int i = 0; i < 16; ++i) p.viewProj[i] = static_cast<float>(m[static_cast<size_t>(i)]);
        p.eye[0] = ov.extent[0];
        p.eye[1] = ov.extent[1];
        p.misc[2] = kLineWidthPx;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdPushConstants(cmd, layout_, stages, 0, sizeof(p), &p);
        vkCmdBindVertexBuffers(cmd, 0, 1, &list.buffer, &list.offset);
        vkCmdDraw(cmd, 6, list.count / 2, 0, 0);   // a quad (instance) a segment, see overlay_line.vert
    };
    const auto text = [&](VkPipeline pipeline, const LineList& list) {
        if (list.count == 0 || ov.atlasTexture == 0) return;
        PushConstants p = pc;
        p.eye[0] = ov.extent[0];
        p.eye[1] = ov.extent[1];
        p.misc[1] = static_cast<float>(ov.atlasTexture);
        p.misc[2] = view.perspective ? 1.0f : 0.0f;   // label.vert: the near-only fade of the entity names needs a perspective view
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdPushConstants(cmd, layout_, stages, 0, sizeof(p), &p);
        vkCmdBindVertexBuffers(cmd, 0, 1, &list.buffer, &list.offset);
        vkCmdDraw(cmd, list.count, 1, 0, 0);
    };
    const auto overlays = [&] {
        if (ov.fills.count != 0) {
            PushConstants p = pc;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fills_);
            vkCmdPushConstants(cmd, layout_, stages, 0, sizeof(p), &p);
            vkCmdBindVertexBuffers(cmd, 0, 1, &ov.fills.buffer, &ov.fills.offset);
            vkCmdDraw(cmd, ov.fills.count, 1, 0, 0);
        }
        lines(plainLines_, ov.entityLines, view.viewProj);
        lines(worldLines_, ov.selection, view.viewProj);
        text(labels_, ov.labels);
        text(labelsOver_, ov.sizeLabels);
        lines(topLines_, ov.gizmo, view.viewProj);
        lines(screenLines_, ov.screen, mat4Identity());
    };
    // the models of entities go in with the opaque surfaces (not into the blended pass over a finished ray traced picture)
    const bool drawModels = models_ && models_->valid() && (parts & (kMeshScene | kMeshGBuffer)) && !(parts & kMeshBlended) && !lists.models.draws.empty();
    const float litPush[4] = {pc.lit[0], pc.lit[1], pc.lit[2], pc.lit[3]};
    if (!(parts & (kMeshScene | kMeshGBuffer | kMeshBlended)) || (lists.solid.count == 0 && lists.volume.count == 0 && lists.translucent.count == 0)) {
        if (drawModels) models_->record(cmd, view, litPush, lists.models, gbuf);
        if (parts & kMeshOverlays) overlays();
        return;
    }
    const VkDeviceSize zero = 0;
    const VkBuffer vb = scene.vertexBuffer();
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &zero);
    vkCmdBindIndexBuffer(cmd, scene.indexBuffer(), 0, VK_INDEX_TYPE_UINT32);
    const auto draw = [&](VkPipeline pipeline, float pass, const DrawList& list) {
        if (list.count == 0) return;
        PushConstants p = pc;
        p.misc[0] = pass;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdPushConstants(cmd, layout_, stages, 0, sizeof(p), &p);
        vkCmdDrawIndexedIndirect(cmd, list.buffer, list.offset, list.count, sizeof(VkDrawIndexedIndirectCommand));
    };
    if (!(parts & kMeshBlended)) draw(gbuf ? gBufferOpaque_ : opaque_, 0, lists.solid);
    if (drawModels) {
        models_->record(cmd, view, litPush, lists.models, gbuf);
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &zero);   // the scene's arenas again for the draws that follow
        vkCmdBindIndexBuffer(cmd, scene.indexBuffer(), 0, VK_INDEX_TYPE_UINT32);
    }
    if (!gbuf) {
        if ((parts & kMeshBlended) && glassTranslucent_ && glass.valid() && lightmaps && lists.translucent.count != 0) {   // the ray traced Lit view lights glass per fragment
            const VkDescriptorSet sets[4] = {textures, lightmaps, glass.lit, glass.tlas};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, glassLayout_, 0, 4, sets, 0, nullptr);
            PushConstants p = pc;
            p.misc[0] = 1;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, glassTranslucent_);
            vkCmdPushConstants(cmd, glassLayout_, stages, 0, sizeof(p), &p);
            vkCmdDrawIndexedIndirect(cmd, lists.translucent.buffer, lists.translucent.offset, lists.translucent.count, sizeof(VkDrawIndexedIndirectCommand));
        } else {
            draw(translucent_, 1, lists.translucent);
        }
        if (showVolumes) draw(volume_, 2, lists.volume);
    }
    if (parts & kMeshOverlays) overlays();
}

} // namespace native::render
