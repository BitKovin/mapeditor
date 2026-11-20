// render/lightmap_view -- the descriptor set the Lit modes sample the display pages through.
//
// Owns: a descriptor set layout (set 1 of the mesh pipelines: one combined image sampler, fragment stage), a linear clamp-to-edge sampler and one descriptor set that
// points at the display page array. update() rewrites it when the image changed; the caller makes sure no frame in flight still uses the old image.
// Thread: render thread. Spec: 05_RENDERER.md §4 (Lit), 08_MILESTONES.md T6.4.
#pragma once

#include <volk.h>

#include "gpu/device.h"

namespace native::render {

class LightmapView {
public:
    explicit LightmapView(const gpu::Device& device);
    ~LightmapView();
    LightmapView(const LightmapView&) = delete;
    LightmapView& operator=(const LightmapView&) = delete;

    bool valid() const { return set_ != VK_NULL_HANDLE; }
    VkDescriptorSetLayout layout() const { return layout_; }
    VkDescriptorSet set() const { return set_; }
    // `view`: a 2D-array view of the display pages in VK_IMAGE_LAYOUT_GENERAL.
    void update(VkImageView view);

private:
    const gpu::Device& device_;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
};

} // namespace native::render
