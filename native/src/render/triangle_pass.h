// render/triangle_pass -- the test triangle of T1.3 (dynamic rendering, no vertex buffers).
//
// Owns: one graphics pipeline for one colour format. Thread: render thread. It exists to prove the swapchain / render-thread
// plumbing and is replaced by the real passes (04/05 specs, T3.x).
#pragma once

#include <volk.h>

#include "gpu/device.h"

namespace native::render {

class TrianglePass {
public:
    TrianglePass(const gpu::Device& device, VkFormat colorFormat);
    ~TrianglePass();
    TrianglePass(const TrianglePass&) = delete;
    TrianglePass& operator=(const TrianglePass&) = delete;

    bool valid() const { return pipeline_ != VK_NULL_HANDLE; }
    VkFormat format() const { return format_; }
    // Inside vkCmdBeginRendering: sets viewport / scissor and draws.
    void record(VkCommandBuffer cmd, VkExtent2D extent) const;

private:
    const gpu::Device& device_;
    VkFormat format_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

} // namespace native::render
