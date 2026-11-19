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

class MeshPass {
public:
    MeshPass(const gpu::Device& device, VkFormat colorFormat, VkFormat depthFormat, VkDescriptorSetLayout textureLayout);
    ~MeshPass();
    MeshPass(const MeshPass&) = delete;
    MeshPass& operator=(const MeshPass&) = delete;

    bool valid() const { return opaque_ != VK_NULL_HANDLE && translucent_ != VK_NULL_HANDLE && volume_ != VK_NULL_HANDLE && skyBg_ != VK_NULL_HANDLE && worldLines_ != VK_NULL_HANDLE && screenLines_ != VK_NULL_HANDLE; }
    VkFormat colorFormat() const { return colorFormat_; }
    VkFormat depthFormat() const { return depthFormat_; }

    // Inside vkCmdBeginRendering (viewport and scissor already set). `skyTexture` = a sky entry of the texture table (TextureTable::skyIndex), 0 = none.
    void record(VkCommandBuffer cmd, const ViewState& view, const GpuScene& scene, const DrawLists& lists, VkDescriptorSet textures, uint32_t skyTexture,
                bool showVolumes, const LineList& selection, const LineList& screen) const;

private:
    const gpu::Device& device_;
    VkFormat colorFormat_, depthFormat_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline opaque_ = VK_NULL_HANDLE, translucent_ = VK_NULL_HANDLE, volume_ = VK_NULL_HANDLE, skyBg_ = VK_NULL_HANDLE;
    VkPipeline worldLines_ = VK_NULL_HANDLE, screenLines_ = VK_NULL_HANDLE;
};

} // namespace native::render
