// render/scene_pass -- the test scene of a viewport (T1.4): an adaptive grid, the world axes and a 1 m box, all drawn from the
// viewport's view-projection matrix (dynamic rendering, no vertex buffers, no depth buffer).
//
// Owns: two graphics pipelines (lines, triangles) for one colour format. Thread: render thread. It proves the cameras and the
// render-thread plumbing and is replaced by the real passes (04/05 specs, T3.x).
#pragma once

#include <volk.h>

#include "gpu/device.h"
#include "render/view_state.h"

namespace native::render {

class ScenePass {
public:
    ScenePass(const gpu::Device& device, VkFormat colorFormat);
    ~ScenePass();
    ScenePass(const ScenePass&) = delete;
    ScenePass& operator=(const ScenePass&) = delete;

    bool valid() const { return gridPipeline_ != VK_NULL_HANDLE && boxPipeline_ != VK_NULL_HANDLE; }
    VkFormat format() const { return format_; }
    // Inside vkCmdBeginRendering: sets viewport / scissor and draws.
    void record(VkCommandBuffer cmd, VkExtent2D extent, const ViewState& view) const;

private:
    const gpu::Device& device_;
    VkFormat format_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline gridPipeline_ = VK_NULL_HANDLE;
    VkPipeline boxPipeline_ = VK_NULL_HANDLE;
};

} // namespace native::render
