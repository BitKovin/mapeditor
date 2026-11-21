// render/model_pass -- draws the models of entities into a viewport (T11.9c): flat shaded, textured, instanced over the entities that show a model.
//
// Owns: two graphics pipelines over the pipeline layout of the MeshPass (set 0 = the texture table, the 112 byte push-constant block of the mesh shaders): the forward one
// (colour + depth) and the G-buffer one of the ray traced Lit path (albedo, normal, distance). Vertex binding 0 = ModelVertex (posed, model space), binding 1 = the instance's
// model matrix. Thread: render thread.
// Spec: 08_MILESTONES.md T11.9c.
#pragma once

#include <volk.h>

#include "gpu/device.h"
#include "render/model_draws.h"
#include "render/view_state.h"

namespace native::render {

class ModelPass {
public:
    // `layout` = the MeshPass's pipeline layout (it outlives this pass).
    ModelPass(const gpu::Device& device, VkPipelineLayout layout, VkFormat colorFormat, VkFormat depthFormat);
    ~ModelPass();
    ModelPass(const ModelPass&) = delete;
    ModelPass& operator=(const ModelPass&) = delete;

    bool valid() const { return forward_ != VK_NULL_HANDLE && gbuffer_ != VK_NULL_HANDLE; }
    // Inside vkCmdBeginRendering, the texture table already bound at set 0 of the layout. `lit` as the mesh pass pushes it (mode, exposure, brightness, lod bias).
    void record(VkCommandBuffer cmd, const ViewState& view, const float lit[4], const ModelDrawList& models, bool gbuffer) const;

private:
    const gpu::Device& device_;
    VkPipelineLayout layout_;
    VkPipeline forward_ = VK_NULL_HANDLE, gbuffer_ = VK_NULL_HANDLE;
};

} // namespace native::render
