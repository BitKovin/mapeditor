// gpu/upload_ring -- see upload_ring.h.
#include "gpu/upload_ring.h"

#include <cstring>

#include "base/log.h"

namespace native::gpu {

UploadRing::UploadRing(Device& device, const UploadRingOptions& options) : device_(device) {
    frames_ = options.framesInFlight == 0 ? 1 : options.framesInFlight;
    slice_ = (options.bytesPerFrame + 255) & ~VkDeviceSize(255);   // keep every slice 256-byte aligned
    if (slice_ == 0) return;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = slice_ * frames_;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;   // indirect: the frame's draw commands are written straight into the slice
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(device_.allocator(), &bci, &aci, &buffer_, &allocation_, &info) != VK_SUCCESS) {
        base::logError("UploadRing: cannot allocate " + std::to_string(bci.size) + " bytes");
        buffer_ = VK_NULL_HANDLE;
        allocation_ = nullptr;
        return;
    }
    mapped_ = static_cast<uint8_t*>(info.pMappedData);
    retire_.assign(frames_, 0);
}

UploadRing::~UploadRing() {
    if (buffer_) vmaDestroyBuffer(device_.allocator(), buffer_, allocation_);
}

bool UploadRing::beginFrame(uint64_t completedValue) {
    if (!valid()) return false;
    if (retire_[next_] > completedValue) {
        ++refused_;
        return false;
    }
    current_ = next_;
    next_ = (next_ + 1) % frames_;
    head_ = 0;
    open_ = true;
    return true;
}

void UploadRing::endFrame(uint64_t retireValue) {
    if (!open_) return;
    retire_[current_] = retireValue;
    open_ = false;
}

UploadRing::Allocation UploadRing::allocate(VkDeviceSize size, VkDeviceSize alignment) {
    if (!open_ || size == 0) return {};
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return {};
    const VkDeviceSize start = (head_ + alignment - 1) & ~(alignment - 1);
    if (start > slice_ || size > slice_ - start) return {};
    head_ = start + size;
    const VkDeviceSize offset = static_cast<VkDeviceSize>(current_) * slice_ + start;
    Allocation a;
    a.buffer = buffer_;
    a.offset = offset;
    a.size = size;
    a.data = mapped_ + offset;
    return a;
}

void UploadRing::flush(const Allocation& a) {
    if (a.ok()) vmaFlushAllocation(device_.allocator(), allocation_, a.offset, a.size);
}

bool UploadRing::upload(VkCommandBuffer cmd, VkBuffer dst, VkDeviceSize dstOffset, const void* data, VkDeviceSize size) {
    const Allocation a = allocate(size, 16);
    if (!a.ok()) return false;
    std::memcpy(a.data, data, static_cast<size_t>(size));
    flush(a);
    const VkBufferCopy region{a.offset, dstOffset, size};
    vkCmdCopyBuffer(cmd, a.buffer, dst, 1, &region);
    return true;
}

} // namespace native::gpu
