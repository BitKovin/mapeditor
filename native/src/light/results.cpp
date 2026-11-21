#include "light/results.h"

#include <algorithm>

namespace native::light {

namespace {
constexpr uint32_t kNaN = 0x7fc00000u;
}

Results::Results(gpu::Device& device, uint32_t slotCapacity)
    : slotCapacity_(slotCapacity),
      direct_{{device, static_cast<uint64_t>(slotCapacity) * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "light direct local"},
              {device, static_cast<uint64_t>(slotCapacity) * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "light direct sun"},
              {device, static_cast<uint64_t>(slotCapacity) * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "light direct sky"}},
      scratch_(device, static_cast<uint64_t>(slotCapacity) * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "light pass scratch"),
      occlusion_(device, static_cast<uint64_t>(slotCapacity) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "light occlusion") {
    if (!(direct_[0].valid() && direct_[1].valid() && direct_[2].valid() && scratch_.valid() && occlusion_.valid())) return;
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = device.computeFamily();
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(device.device(), &pi, nullptr, &pool) != VK_SUCCESS) return;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device.device(), &ai, &cmd) == VK_SUCCESS) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        reset(cmd, 0, slotCapacity);
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &di);
        vkEndCommandBuffer(cmd);
        VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbi.commandBuffer = cmd;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &cbi;
        valid_ = device.submitAndWait(device.computeQueue(), si, "result buffers");
    }
    vkDestroyCommandPool(device.device(), pool, nullptr);
}

Results::~Results() = default;

void Results::reset(VkCommandBuffer cmd, uint32_t first, uint32_t count) {
    if (count == 0 || first >= slotCapacity_) return;
    count = std::min(count, slotCapacity_ - first);
    for (const gpu::Arena& d : direct_) vkCmdFillBuffer(cmd, d.buffer(), static_cast<VkDeviceSize>(first) * 16, static_cast<VkDeviceSize>(count) * 16, 0);
    vkCmdFillBuffer(cmd, scratch_.buffer(), static_cast<VkDeviceSize>(first) * 16, static_cast<VkDeviceSize>(count) * 16, 0);
    vkCmdFillBuffer(cmd, occlusion_.buffer(), static_cast<VkDeviceSize>(first) * 4, static_cast<VkDeviceSize>(count) * 4, kNaN);
}

} // namespace native::light
