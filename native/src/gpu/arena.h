// gpu/arena -- a big device-local buffer that is sub-allocated with a VMA virtual block.
//
// Owns: one VkBuffer (vertex / index / storage arenas of the renderer and the light sample arena) and the bookkeeping of which
// byte ranges are in use. Allocation and free are CPU-side only (no GPU work, no per-object buffers); contents are written
// through the UploadRing. Freeing a range that the GPU may still read is the caller's job to delay (retire by timeline value).
// Thread: allocate / free are internally locked, so the UI thread may free while the render thread allocates.
// Spec: 05_RENDERER.md §3 (arenas), 06_LIGHTING.md (sample arena), 08_MILESTONES.md T1.5.
#pragma once

#include <volk.h>

#include <vk_mem_alloc.h>

#include <cstdint>
#include <mutex>
#include <string>

#include "gpu/device.h"

namespace native::gpu {

class Arena {
public:
    struct Block {
        VkDeviceSize offset = ~VkDeviceSize(0);
        VkDeviceSize size = 0;
        bool ok() const { return size != 0; }
    private:
        friend class Arena;
        VmaVirtualAllocation handle = nullptr;
    };

    // usage is OR-ed with TRANSFER_DST (uploads) and TRANSFER_SRC (read-back in tests / capture).
    Arena(Device& device, VkDeviceSize capacity, VkBufferUsageFlags usage, std::string name);
    ~Arena();
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    bool valid() const { return buffer_ != VK_NULL_HANDLE && block_ != nullptr; }
    VkBuffer buffer() const { return buffer_; }
    VkDeviceSize capacity() const { return capacity_; }
    const std::string& name() const { return name_; }

    // Not ok() when no range of that size / alignment (power of two) is free.
    Block allocate(VkDeviceSize size, VkDeviceSize alignment = 16);
    // Releases the range and clears the block.
    void free(Block& block);

    VkDeviceSize usedBytes() const;
    uint32_t allocationCount() const;
    // Largest single allocation that would currently succeed with alignment 1 (fragmentation indicator).
    VkDeviceSize largestFreeRange() const;

private:
    Device& device_;
    std::string name_;
    VkDeviceSize capacity_ = 0;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    VmaVirtualBlock block_ = nullptr;
    mutable std::mutex mutex_;
};

} // namespace native::gpu
