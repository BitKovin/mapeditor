// Helpers of the [gpu] tests: a one-shot command buffer on a queue, waited for, and a device buffer read-back.
#pragma once

#include <doctest.h>

#include <cstring>
#include <functional>
#include <vector>

#include "gpu/device.h"

namespace native_test {

// A one-shot command buffer on the graphics queue (compute = false) or the compute queue, waited for.
class OneShot {
public:
    explicit OneShot(native::gpu::Device& d, bool compute = false) : d_(d), queue_(compute ? d.computeQueue() : d.graphicsQueue()) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = compute ? d.computeFamily() : d.graphicsFamily();
        vkCreateCommandPool(d.device(), &pi, nullptr, &pool_);
    }
    ~OneShot() { vkDestroyCommandPool(d_.device(), pool_, nullptr); }
    void run(const std::function<void(VkCommandBuffer)>& record) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(d_.device(), &ai, &cmd);
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
        vkQueueSubmit2(queue_, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue_);
        vkFreeCommandBuffers(d_.device(), pool_, 1, &cmd);
    }

private:
    native::gpu::Device& d_;
    VkQueue queue_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
};

// Copies the first `size` bytes of `src` (a device buffer with TRANSFER_SRC) to host memory.
inline std::vector<uint8_t> readBack(native::gpu::Device& d, OneShot& shot, VkBuffer src, uint64_t size) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation a = nullptr;
    VmaAllocationInfo info{};
    REQUIRE(vmaCreateBuffer(d.allocator(), &bci, &aci, &buf, &a, &info) == VK_SUCCESS);
    shot.run([&](VkCommandBuffer cmd) {
        VkBufferCopy r{0, 0, size};
        vkCmdCopyBuffer(cmd, src, buf, 1, &r);
    });
    vmaInvalidateAllocation(d.allocator(), a, 0, VK_WHOLE_SIZE);
    std::vector<uint8_t> out(size);
    std::memcpy(out.data(), info.pMappedData, size);
    vmaDestroyBuffer(d.allocator(), buf, a);
    return out;
}

// Transfer writes -> everything else (after uploads and fills).
inline void transferToAll(VkCommandBuffer cmd) {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
}

} // namespace native_test
