// gpu/swapchain -- see swapchain.h.
#include "gpu/swapchain.h"

#include <algorithm>

#include "base/log.h"

namespace native::gpu {

Swapchain::Swapchain(const Device& device, VkSurfaceKHR surface) : device_(device), surface_(surface) {}

Swapchain::~Swapchain() { destroy(); }

void Swapchain::destroyImages() {
    for (VkImageView v : views_) vkDestroyImageView(device_.device(), v, nullptr);
    for (VkSemaphore s : renderDone_) vkDestroySemaphore(device_.device(), s, nullptr);
    views_.clear();
    renderDone_.clear();
    images_.clear();
}

void Swapchain::destroy() {
    destroyImages();
    if (swapchain_) vkDestroySwapchainKHR(device_.device(), swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    extent_ = {0, 0};
}

bool Swapchain::create(uint32_t width, uint32_t height) {
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_.physicalDevice(), surface_, &caps) != VK_SUCCESS) return false;
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) return false;   // minimised

    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(device_.physicalDevice(), device_.graphicsFamily(), surface_, &supported);
    if (!supported) {
        base::logError("swapchain: the graphics queue family cannot present to this surface");
        return false;
    }

    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device_.physicalDevice(), surface_, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(nf);
    vkGetPhysicalDeviceSurfaceFormatsKHR(device_.physicalDevice(), surface_, &nf, formats.data());
    if (formats.empty()) return false;
    // Linear UNORM: shading is done in the textures' own space with no sRGB conversion (the engine's rule, DESIGN §6).
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; break; }

    uint32_t nm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device_.physicalDevice(), surface_, &nm, nullptr);
    std::vector<VkPresentModeKHR> modes(nm);
    vkGetPhysicalDeviceSurfacePresentModesKHR(device_.physicalDevice(), surface_, &nm, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()) mode = VK_PRESENT_MODE_MAILBOX_KHR;

    uint32_t count = std::max(3u, caps.minImageCount);
    if (caps.maxImageCount > 0) count = std::min(count, caps.maxImageCount);

    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    const bool capture = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (capture) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = count;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = usage;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                                                                            : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain_;

    VkSwapchainKHR created = VK_NULL_HANDLE;
    const VkResult r = vkCreateSwapchainKHR(device_.device(), &ci, nullptr, &created);
    if (r != VK_SUCCESS) {
        base::logError("vkCreateSwapchainKHR failed (" + std::to_string(r) + ")");
        return false;
    }
    destroyImages();                                    // views / semaphores of the old swapchain
    if (swapchain_) vkDestroySwapchainKHR(device_.device(), swapchain_, nullptr);
    swapchain_ = created;
    format_ = chosen.format;
    extent_ = extent;
    presentMode_ = mode;
    canCapture_ = capture;

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(device_.device(), swapchain_, &n, nullptr);
    images_.resize(n);
    vkGetSwapchainImagesKHR(device_.device(), swapchain_, &n, images_.data());
    for (VkImage img : images_) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        vkCreateImageView(device_.device(), &vi, nullptr, &view);
        views_.push_back(view);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore sem = VK_NULL_HANDLE;
        vkCreateSemaphore(device_.device(), &si, nullptr, &sem);
        renderDone_.push_back(sem);
    }
    return true;
}

VkResult Swapchain::acquire(VkSemaphore signalWhenReady, uint32_t* imageIndex) {
    return vkAcquireNextImageKHR(device_.device(), swapchain_, 2'000'000'000ull, signalWhenReady, VK_NULL_HANDLE, imageIndex);
}

VkResult Swapchain::present(VkQueue queue, uint32_t imageIndex) {
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderDone_[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex;
    return vkQueuePresentKHR(queue, &pi);
}

} // namespace native::gpu
