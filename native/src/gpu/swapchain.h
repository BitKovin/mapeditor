// gpu/swapchain -- one VkSwapchainKHR for one surface: creation, recreation on resize, acquire and present.
//
// Owns: the swapchain, its image views and one "render finished" binary semaphore per image (presentation waits on it).
// Thread: the render thread only (01_ARCHITECTURE.md §2); the surface itself belongs to Qt (render/viewport_window).
// Spec: 05_RENDERER.md §3: present mode MAILBOX when available else FIFO, 3 images, recreated by the render thread
// before its next frame after a resize.
#pragma once

#include <volk.h>

#include <cstdint>
#include <vector>

#include "gpu/device.h"

namespace native::gpu {

class Swapchain {
public:
    Swapchain(const Device& device, VkSurfaceKHR surface);
    ~Swapchain();
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // (Re)creates the swapchain for a client area of w x h pixels. Returns false when the area is empty (minimised window)
    // or Vulkan refuses; the old swapchain, if any, stays usable only after a successful call. The caller must make sure no
    // work using the old images is in flight (the render thread waits for its queue before calling).
    bool create(uint32_t width, uint32_t height);
    void destroy();
    bool valid() const { return swapchain_ != VK_NULL_HANDLE; }

    // VK_SUCCESS, VK_SUBOPTIMAL_KHR (usable, recreate soon) or an error such as VK_ERROR_OUT_OF_DATE_KHR.
    VkResult acquire(VkSemaphore signalWhenReady, uint32_t* imageIndex);
    // Waits for renderDone(imageIndex) on the GPU side.
    VkResult present(VkQueue queue, uint32_t imageIndex);

    VkSwapchainKHR handle() const { return swapchain_; }
    VkFormat format() const { return format_; }
    VkExtent2D extent() const { return extent_; }
    size_t imageCount() const { return images_.size(); }
    VkImage image(uint32_t i) const { return images_[i]; }
    VkImageView view(uint32_t i) const { return views_[i]; }
    VkSemaphore renderDone(uint32_t i) const { return renderDone_[i]; }
    VkPresentModeKHR presentMode() const { return presentMode_; }
    bool canCapture() const { return canCapture_; }   // images have TRANSFER_SRC usage

private:
    void destroyImages();

    const Device& device_;
    VkSurfaceKHR surface_;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{0, 0};
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    bool canCapture_ = false;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkSemaphore> renderDone_;
};

} // namespace native::gpu
