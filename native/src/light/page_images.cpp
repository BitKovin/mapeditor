#include "light/page_images.h"

#include <algorithm>
#include <cstring>
#include <functional>

namespace native::light {

namespace {

// One-shot command buffer on the compute queue, waited for.
bool submitOnce(gpu::Device& device, const std::function<void(VkCommandBuffer)>& record) {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = device.computeFamily();
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(device.device(), &pci, nullptr, &pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    bool ok = vkAllocateCommandBuffers(device.device(), &ai, &cmd) == VK_SUCCESS;
    if (ok) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        record(cmd);
        vkEndCommandBuffer(cmd);
        VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbi.commandBuffer = cmd;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &cbi;
        ok = vkQueueSubmit2(device.computeQueue(), 1, &si, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(device.computeQueue()) == VK_SUCCESS;
    }
    vkDestroyCommandPool(device.device(), pool, nullptr);
    return ok;
}

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, uint32_t layerCount, VkAccessFlags2 srcAccess, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layerCount};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

} // namespace

PageImages::PageImages(gpu::Device& device, int atlasSize, VkFormat format) : device_(device), atlas_(atlasSize), format_(format) {}

PageImages::~PageImages() { destroy(); }

void PageImages::destroy() {
    if (view_) vkDestroyImageView(device_.device(), view_, nullptr);
    if (image_) vmaDestroyImage(device_.allocator(), image_, alloc_);
    view_ = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
    alloc_ = nullptr;
    layers_ = 0;
}

bool PageImages::create(int layers, VkImage& image, VmaAllocation& alloc, VkImageView& view) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format_;
    ici.extent = {static_cast<uint32_t>(atlas_), static_cast<uint32_t>(atlas_), 1};
    ici.mipLevels = 1;
    ici.arrayLayers = static_cast<uint32_t>(layers);
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(device_.allocator(), &ici, &aci, &image, &alloc, nullptr) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vci.format = format_;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, static_cast<uint32_t>(layers)};
    if (vkCreateImageView(device_.device(), &vci, nullptr, &view) != VK_SUCCESS) {
        vmaDestroyImage(device_.allocator(), image, alloc);
        image = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool PageImages::ensureLayers(int layers) {
    layers = std::max(layers, 1);
    if (layers <= layers_) return false;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    if (!create(layers, image, alloc, view)) return false;
    const int keep = layers_;
    const bool done = submitOnce(device_, [&](VkCommandBuffer cmd) {
        barrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(layers), 0, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (keep > 0) {
            barrier(cmd, image_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<uint32_t>(keep), VK_ACCESS_2_MEMORY_WRITE_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
            VkImageCopy region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, static_cast<uint32_t>(keep)};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, static_cast<uint32_t>(keep)};
            region.extent = {static_cast<uint32_t>(atlas_), static_cast<uint32_t>(atlas_), 1};
            vkCmdCopyImage(cmd, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, static_cast<uint32_t>(layers), VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    });
    if (!done) {
        vkDestroyImageView(device_.device(), view, nullptr);
        vmaDestroyImage(device_.allocator(), image, alloc);
        return false;
    }
    destroy();
    image_ = image;
    alloc_ = alloc;
    view_ = view;
    layers_ = layers;
    ++generation_;
    return true;
}

uint64_t PageImages::bytes() const {
    if (!alloc_) return 0;
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(device_.allocator(), alloc_, &info);
    return info.size;
}

bool PageImages::writeLayer(int layer, const std::vector<uint16_t>& texels) {
    const size_t bytes = static_cast<size_t>(atlas_) * atlas_ * 8;
    if (layer < 0 || layer >= layers_ || texels.size() * 2 != bytes) return false;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation a = nullptr;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(device_.allocator(), &bci, &aci, &buf, &a, &info) != VK_SUCCESS) return false;
    std::memcpy(info.pMappedData, texels.data(), bytes);
    vmaFlushAllocation(device_.allocator(), a, 0, VK_WHOLE_SIZE);
    const bool ok = submitOnce(device_, [&](VkCommandBuffer cmd) {
        barrier(cmd, image_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(layers_), VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<uint32_t>(layer), 1};
        r.imageExtent = {static_cast<uint32_t>(atlas_), static_cast<uint32_t>(atlas_), 1};
        vkCmdCopyBufferToImage(cmd, buf, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        barrier(cmd, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, static_cast<uint32_t>(layers_), VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    });
    vmaDestroyBuffer(device_.allocator(), buf, a);
    return ok;
}

bool PageImages::readLayer(int layer, std::vector<uint16_t>& texels) {
    const size_t bytes = static_cast<size_t>(atlas_) * atlas_ * 8;
    if (layer < 0 || layer >= layers_) return false;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation a = nullptr;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(device_.allocator(), &bci, &aci, &buf, &a, &info) != VK_SUCCESS) return false;
    const bool ok = submitOnce(device_, [&](VkCommandBuffer cmd) {
        barrier(cmd, image_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<uint32_t>(layers_), VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<uint32_t>(layer), 1};
        r.imageExtent = {static_cast<uint32_t>(atlas_), static_cast<uint32_t>(atlas_), 1};
        vkCmdCopyImageToBuffer(cmd, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &r);
        barrier(cmd, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, static_cast<uint32_t>(layers_), VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    });
    if (ok) {
        vmaInvalidateAllocation(device_.allocator(), a, 0, VK_WHOLE_SIZE);
        texels.resize(bytes / 2);
        std::memcpy(texels.data(), info.pMappedData, bytes);
    }
    vmaDestroyBuffer(device_.allocator(), buf, a);
    return ok;
}

} // namespace native::light
