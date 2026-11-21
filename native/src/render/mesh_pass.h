// render/mesh_pass -- draws the scene's meshes (Unlit) into a viewport: sky background, opaque + cutout, translucent, volumes.
//
// Owns: four graphics pipelines over one layout (set 0 = the texture table, 96 bytes of push constants) for one colour format and a depth format.
// Draws are indirect, from the lists GpuScene::buildDraws wrote into the upload ring: one vkCmdDrawIndexedIndirect per list.
// Thread: render thread. Spec: 05_RENDERER.md section 4, 08_MILESTONES.md T3.4.
#pragma once

#include <volk.h>

#include <cstdint>

#include "gpu/device.h"
#include "render/gpu_scene.h"
#include "render/line_vertex.h"
#include "render/view_state.h"

namespace native::render {

// Line vertices already in the upload ring (two a line).
struct LineList {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    uint32_t count = 0;
};

// Everything a viewport draws over the scene (vertex lists already in the upload ring).
struct FrameOverlays {
    LineList selection;     // the selection outline: world lines, depth tested with a bias
    LineList entityLines;   // entity boxes, arrows, light gizmos: world lines, depth tested
    LineList fills;         // translucent entity boxes: LineVertex triangles, depth tested without writing
    LineList labels;        // entity class names: LabelVertex triangles, depth tested at the anchor
    LineList sizeLabels;    // selection sizes: LabelVertex triangles, over everything
    LineList gizmo;         // the transform gizmo: world lines over everything
    LineList screen;        // the rubber band: lines in clip space
    uint32_t atlasTexture = 0;   // the glyph atlas in the texture table
    float extent[2] = {1, 1};    // the viewport in pixels
};

// How a viewport shades the scene (RenderMode): 0 = Unlit, 1 = Lit (albedo x light), 2 = Lighting only.
struct LitParams {
    int mode = 0;
    float exposure = 1, brightness = 0;
    float lodBias = 0;   // added to every texture lookup (the DLSS path renders at a lower resolution: log2(render / output) - 1)
};

// What MeshPass::record draws: the scene (sky, opaque, translucent, volumes) and / or the overlays over it (entity lines, selection, labels, gizmo, rubber band).
// DLSS draws the scene at the render resolution and the overlays at the output resolution after the upscaler.
// kMeshGBuffer: the ray traced Lit path's G-buffer pass (sky background + opaque into three attachments: albedo, normal, distance; see mesh.frag GBUFFER),
// kMeshBlended: only the translucent and volume draws, Unlit, over the finished picture (depth tested, no writes).
enum MeshParts : int { kMeshScene = 1, kMeshOverlays = 2, kMeshAll = 3, kMeshGBuffer = 4, kMeshBlended = 8 };
constexpr VkFormat kGBufferNormalFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kGBufferDistFormat = VK_FORMAT_R32_SFLOAT;

// The descriptor sets the translucent surface shader needs (RtLit::litSet / tlasSet of this frame).
struct GlassSets {
    VkDescriptorSet lit = VK_NULL_HANDLE, tlas = VK_NULL_HANDLE;
    bool valid() const { return lit != VK_NULL_HANDLE && tlas != VK_NULL_HANDLE; }
};

class MeshPass {
public:
    // `lightmapLayout` = set 1 (the display pages, render/lightmap_view.h).
    MeshPass(const gpu::Device& device, VkFormat colorFormat, VkFormat depthFormat, VkDescriptorSetLayout textureLayout, VkDescriptorSetLayout lightmapLayout);
    ~MeshPass();
    MeshPass(const MeshPass&) = delete;
    MeshPass& operator=(const MeshPass&) = delete;

    bool valid() const { return opaque_ != VK_NULL_HANDLE && gBufferOpaque_ != VK_NULL_HANDLE && gBufferSky_ != VK_NULL_HANDLE && translucent_ != VK_NULL_HANDLE && volume_ != VK_NULL_HANDLE && skyBg_ != VK_NULL_HANDLE && worldLines_ != VK_NULL_HANDLE && screenLines_ != VK_NULL_HANDLE && plainLines_ != VK_NULL_HANDLE && fills_ != VK_NULL_HANDLE && topLines_ != VK_NULL_HANDLE && labels_ != VK_NULL_HANDLE && labelsOver_ != VK_NULL_HANDLE; }
    VkFormat colorFormat() const { return colorFormat_; }
    VkFormat depthFormat() const { return depthFormat_; }

    // The ray traced Lit viewport lights translucent surfaces per fragment (mesh_glass.frag): needs the set layouts of RtLit; false when the pipeline cannot be made (glass stays Unlit).
    bool enableGlass(VkDescriptorSetLayout lit, VkDescriptorSetLayout tlas);

    // Inside vkCmdBeginRendering (viewport and scissor already set). `skyTexture` = a sky entry of the texture table (TextureTable::skyIndex), 0 = none.
    void record(VkCommandBuffer cmd, const ViewState& view, const GpuScene& scene, const DrawLists& lists, VkDescriptorSet textures, uint32_t skyTexture,
                bool showVolumes, const FrameOverlays& overlays, VkDescriptorSet lightmaps = VK_NULL_HANDLE, const LitParams& lit = {}, int parts = kMeshAll, const GlassSets& glass = {}) const;

private:
    const gpu::Device& device_;
    VkFormat colorFormat_, depthFormat_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE, glassLayout_ = VK_NULL_HANDLE;
    VkPipeline glassTranslucent_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout textureLayout_ = VK_NULL_HANDLE, lightmapLayout_ = VK_NULL_HANDLE;
    VkPipeline gBufferOpaque_ = VK_NULL_HANDLE, gBufferSky_ = VK_NULL_HANDLE;
    VkPipeline opaque_ = VK_NULL_HANDLE, translucent_ = VK_NULL_HANDLE, volume_ = VK_NULL_HANDLE, skyBg_ = VK_NULL_HANDLE;
    VkPipeline worldLines_ = VK_NULL_HANDLE, screenLines_ = VK_NULL_HANDLE, plainLines_ = VK_NULL_HANDLE, fills_ = VK_NULL_HANDLE, topLines_ = VK_NULL_HANDLE;
    VkPipeline labels_ = VK_NULL_HANDLE, labelsOver_ = VK_NULL_HANDLE;
};

} // namespace native::render
