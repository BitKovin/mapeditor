#include "gpu/gpu_timer.h"

#include <cstring>

namespace native::gpu {

GpuTimer::GpuTimer(Device& device, uint32_t slots) : device_(device), slots_(slots) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device_.physicalDevice(), &props);
    nsPerTick_ = static_cast<double>(props.limits.timestampPeriod);
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device_.physicalDevice(), &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device_.physicalDevice(), &familyCount, families.data());
    if (device_.graphicsFamily() < familyCount) validBits_ = families[device_.graphicsFamily()].timestampValidBits;
    if (validBits_ == 0 || nsPerTick_ <= 0.0) return;
    VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    ci.queryCount = kMaxStamps;
    for (uint32_t i = 0; i < slots; ++i) {
        VkQueryPool pool = VK_NULL_HANDLE;
        if (vkCreateQueryPool(device_.device(), &ci, nullptr, &pool) != VK_SUCCESS) break;
        pools_.push_back(pool);
    }
    if (pools_.size() != slots) {
        for (VkQueryPool p : pools_) vkDestroyQueryPool(device_.device(), p, nullptr);
        pools_.clear();
    }
    names_.resize(slots);
}

GpuTimer::~GpuTimer() {
    for (VkQueryPool p : pools_) vkDestroyQueryPool(device_.device(), p, nullptr);
}

void GpuTimer::begin(uint32_t slot) {
    if (pools_.empty() || slot >= slots_) return;
    vkResetQueryPool(device_.device(), pools_[slot], 0, kMaxStamps);
    names_[slot].clear();
}

void GpuTimer::stamp(VkCommandBuffer cmd, uint32_t slot, const char* name) {
    if (pools_.empty() || slot >= slots_ || names_[slot].size() >= kMaxStamps) return;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pools_[slot], static_cast<uint32_t>(names_[slot].size()));
    names_[slot].emplace_back(name);
}

bool GpuTimer::collect(uint32_t slot, std::vector<Interval>& out) {
    out.clear();
    if (pools_.empty() || slot >= slots_ || names_[slot].size() < 2) return false;
    const uint32_t count = static_cast<uint32_t>(names_[slot].size());
    uint64_t ticks[kMaxStamps];
    if (vkGetQueryPoolResults(device_.device(), pools_[slot], 0, count, sizeof(ticks), ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return false;
    const uint64_t mask = validBits_ >= 64 ? ~0ull : ((1ull << validBits_) - 1ull);
    for (uint32_t i = 1; i < count; ++i) {
        const uint64_t d = (ticks[i] - ticks[i - 1]) & mask;
        out.push_back({names_[slot][i], static_cast<double>(d) * nsPerTick_ * 1e-6});
    }
    return true;
}

} // namespace native::gpu
