// render/dlss_pass -- the GPU side of the DLSS Super Resolution path: the render-resolution targets of one viewport, the camera motion vectors
// from depth, and the composite of the upscaled picture onto the swapchain image (with the depth the overlays need).
//
// Owns: DlssTargets (per viewport: scene colour RGBA16F + depth D32 at the render size, motion vectors RG16F, the DLSS output RGBA16F at the
// output size, their descriptor sets) and DlssPass (the shared pipelines: a compute pass depth -> motion vectors, a full-screen draw that
// copies the output to the swapchain format and writes the output-size depth). The upscale itself is Streamline (gpu/streamline.h).
// Image layouts: every frame starts these images in UNDEFINED (their content is rewritten each frame), so no layout is tracked between frames.
// Thread: the render thread. Spec: 11_NVIDIA_TECH.md §2.2, 08_MILESTONES.md T6.8.
#pragma once

#include <volk.h>

#include <cstdint>

#include "gpu/device.h"
#include "gpu/streamline.h"

namespace native::render {

struct DlssTargets {
    VkExtent2D render{}, out{};
    VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    VkFormat mvecFormat = VK_FORMAT_R16G16_SFLOAT;
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkImageUsageFlags usage = 0;
    };
    Image color, depth, mvec, output;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet mvecSet = VK_NULL_HANDLE, resolveSet = VK_NULL_HANDLE;
    bool valid() const { return color.image != VK_NULL_HANDLE; }
};

class DlssPass {
public:
    // `swapchainFormat` = the colour format of the pass the resolve draws into; `depthFormat` = the output-size depth buffer.
    DlssPass(const gpu::Device& device, VkFormat swapchainFormat, VkFormat depthFormat);
    ~DlssPass();
    DlssPass(const DlssPass&) = delete;
    DlssPass& operator=(const DlssPass&) = delete;

    bool valid() const { return mvecPipeline_ != VK_NULL_HANDLE && resolvePipeline_ != VK_NULL_HANDLE && sampler_ != VK_NULL_HANDLE; }
    VkFormat swapchainFormat() const { return swapchainFormat_; }

    // (Re)creates the targets for a render size and an output size. The GPU must be idle (or the old targets unused).
    bool createTargets(DlssTargets& t, VkExtent2D render, VkExtent2D out) const;
    void destroyTargets(DlssTargets& t) const;

    // Before the scene pass: the colour and depth targets go UNDEFINED -> attachment layouts.
    void beginScene(VkCommandBuffer cmd, const DlssTargets& t) const;
    // After the scene pass: colour -> shader read, depth -> depth read only, then the motion vectors (written into mvec, left in GENERAL).
    // `reproject` = previous unjittered viewProj * inverse(jittered viewProj of this frame), column-major; jitter in pixels of the render image.
    void endSceneAndMotion(VkCommandBuffer cmd, const DlssTargets& t, const float reproject[16], float jitterX, float jitterY) const;
    // The output image goes UNDEFINED -> GENERAL for the upscaler (the state DlssFrame says), and after the evaluate -> shader read for the resolve.
    void beforeUpscale(VkCommandBuffer cmd, const DlssTargets& t) const;
    void afterUpscale(VkCommandBuffer cmd, const DlssTargets& t) const;
    // Inside a render pass at the output size with the swapchain image and the output-size depth attached: copies the output and the depth.
    void resolve(VkCommandBuffer cmd, const DlssTargets& t) const;

    // The images as the upscaler sees them at the evaluate call (layouts as left by endSceneAndMotion / beforeUpscale).
    static gpu::DlssImage color(const DlssTargets& t);
    static gpu::DlssImage depth(const DlssTargets& t);
    static gpu::DlssImage mvec(const DlssTargets& t);
    static gpu::DlssImage output(const DlssTargets& t);

private:
    const gpu::Device& device_;
    VkFormat swapchainFormat_, depthFormat_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout mvecLayout_ = VK_NULL_HANDLE, resolveLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout mvecPipelineLayout_ = VK_NULL_HANDLE, resolvePipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline mvecPipeline_ = VK_NULL_HANDLE, resolvePipeline_ = VK_NULL_HANDLE;
};

} // namespace native::render
