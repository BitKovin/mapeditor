// light/page_images -- the GPU images behind the lightmap pages: one 2D-array image (RGBA16F, atlasSize x atlasSize x layers).
//
// Owns: the display pages' image (what the Lit view samples, 06 §6) and any other per-texel page array of the same shape (the bounce source S uses one too). The
// image stays in VK_IMAGE_LAYOUT_GENERAL (storage + sampled + copy): the passes of the bake write it, the viewport samples it, nothing is transitioned per frame.
// Growing (more layers) makes a new image, copies the old layers on the GPU and destroys the old one, so views / descriptors must be taken again after
// `ensureLayers` returned true (the `generation()` counter tells).
// Thread: the bake thread's (compute queue); the one-shot submits wait for the queue, so they never overlap the caller's own work.
// Spec: 06_LIGHTING.md §3.4 (grow by 8 layers, copy old layers on the GPU), §6, 08 T6.1.
#pragma once

#include <cstdint>
#include <vector>

#include "gpu/device.h"

namespace native::light {

class PageImages {
public:
    PageImages(gpu::Device& device, int atlasSize, VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT);
    ~PageImages();
    PageImages(const PageImages&) = delete;
    PageImages& operator=(const PageImages&) = delete;

    bool ok() const { return image_ != VK_NULL_HANDLE; }
    // At least `layers` layers (the content of the existing ones is kept). Returns true when the image was replaced.
    bool ensureLayers(int layers);
    int layers() const { return layers_; }
    int atlasSize() const { return atlas_; }
    VkImage image() const { return image_; }
    VkImageView view() const { return view_; }          // 2D-array view of all layers
    uint64_t generation() const { return generation_; } // +1 whenever image() / view() changed
    uint64_t bytes() const;                             // device memory of the image

    // Test / debug helpers: blocking copies through a staging buffer. A layer is atlasSize^2 texels of 4 half floats (as uint16 bits).
    bool writeLayer(int layer, const std::vector<uint16_t>& texels);
    bool readLayer(int layer, std::vector<uint16_t>& texels);

private:
    bool create(int layers, VkImage& image, VmaAllocation& alloc, VkImageView& view);
    void destroy();

    gpu::Device& device_;
    int atlas_;
    VkFormat format_;
    int layers_ = 0;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation alloc_ = nullptr;
    VkImageView view_ = VK_NULL_HANDLE;
    uint64_t generation_ = 0;
};

} // namespace native::light
