// gpu/arena -- see arena.h.
#include "gpu/arena.h"

#include "base/log.h"

namespace native::gpu {

Arena::Arena(Device& device, VkDeviceSize capacity, VkBufferUsageFlags usage, std::string name)
    : device_(device), name_(std::move(name)), capacity_(capacity) {
    if (capacity == 0) return;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = capacity;
    bci.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateBuffer(device_.allocator(), &bci, &aci, &buffer_, &allocation_, nullptr) != VK_SUCCESS) {
        base::logError("Arena '" + name_ + "': cannot allocate " + std::to_string(capacity) + " bytes");
        buffer_ = VK_NULL_HANDLE;
        allocation_ = nullptr;
        return;
    }
    VmaVirtualBlockCreateInfo vci{};
    vci.size = capacity;
    if (vmaCreateVirtualBlock(&vci, &block_) != VK_SUCCESS) block_ = nullptr;
}

Arena::~Arena() {
    if (block_) {
        vmaClearVirtualBlock(block_);
        vmaDestroyVirtualBlock(block_);
    }
    if (buffer_) vmaDestroyBuffer(device_.allocator(), buffer_, allocation_);
}

VkDeviceAddress Arena::deviceAddress() const {
    if (!buffer_) return 0;
    VkBufferDeviceAddressInfo i{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    i.buffer = buffer_;
    return vkGetBufferDeviceAddress(device_.device(), &i);
}

Arena::Block Arena::allocate(VkDeviceSize size, VkDeviceSize alignment) {
    Block b;
    if (!valid() || size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0) return b;
    VmaVirtualAllocationCreateInfo ci{};
    ci.size = size;
    ci.alignment = alignment;
    VkDeviceSize offset = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (vmaVirtualAllocate(block_, &ci, &b.handle, &offset) != VK_SUCCESS) {
        b.handle = nullptr;
        return b;
    }
    b.offset = offset;
    b.size = size;
    return b;
}

void Arena::free(Block& block) {
    if (block.handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        vmaVirtualFree(block_, block.handle);
    }
    block = Block{};
}

VkDeviceSize Arena::usedBytes() const {
    if (!valid()) return 0;
    VmaStatistics s{};
    std::lock_guard<std::mutex> lock(mutex_);
    vmaGetVirtualBlockStatistics(block_, &s);
    return s.allocationBytes;
}

uint32_t Arena::allocationCount() const {
    if (!valid()) return 0;
    VmaStatistics s{};
    std::lock_guard<std::mutex> lock(mutex_);
    vmaGetVirtualBlockStatistics(block_, &s);
    return s.allocationCount;
}

VkDeviceSize Arena::largestFreeRange() const {
    if (!valid()) return 0;
    VmaDetailedStatistics s{};
    std::lock_guard<std::mutex> lock(mutex_);
    vmaCalculateVirtualBlockStatistics(block_, &s);
    return s.unusedRangeSizeMax;
}

} // namespace native::gpu
