// gpu/upload_ring -- per-frame staging memory for CPU -> GPU copies.
//
// Owns: one persistently mapped, host-visible buffer divided into one slice per frame in flight (default 64 MB each, 2 slices).
// Each slice is a bump allocator for the frame being recorded; a slice is handed out again only when the timeline value it was
// tagged with at endFrame() has been reached (retire by timeline value, 01_ARCHITECTURE.md §5). Nothing is allocated per upload.
// Thread: one thread uses a ring (the one recording the frame); different rings are independent.
// Spec: 05_RENDERER.md §3 (deltas only through a per-frame upload ring), 08_MILESTONES.md T1.5.
//
// Usage per frame:   if (!ring.beginFrame(completedTimelineValue)) wait and retry;
//                    ring.upload(cmd, dstBuffer, dstOffset, data, size) ... ;  submit;  ring.endFrame(valueSignalledBySubmit);
#pragma once

#include <volk.h>

#include <vk_mem_alloc.h>

#include <cstdint>
#include <vector>

#include "gpu/device.h"

namespace native::gpu {

struct UploadRingOptions {
    VkDeviceSize bytesPerFrame = 64ull * 1024 * 1024;
    uint32_t framesInFlight = 2;
};

class UploadRing {
public:
    // A piece of the current frame's slice. data is writable until the frame is submitted; buffer / offset are for copy commands.
    struct Allocation {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;     // into buffer
        VkDeviceSize size = 0;
        void* data = nullptr;
        bool ok() const { return data != nullptr; }
    };

    UploadRing(Device& device, const UploadRingOptions& options = {});
    ~UploadRing();
    UploadRing(const UploadRing&) = delete;
    UploadRing& operator=(const UploadRing&) = delete;

    bool valid() const { return buffer_ != VK_NULL_HANDLE; }
    VkDeviceSize bytesPerFrame() const { return slice_; }
    uint32_t framesInFlight() const { return frames_; }

    // Starts recording the next frame: picks the next slice and resets it. Returns false (and changes nothing) when that slice is
    // still in use by the GPU, i.e. its retire value is greater than completedValue; wait for the timeline and call again.
    bool beginFrame(uint64_t completedValue);
    // Tags the current slice: it is free again once the timeline reaches retireValue.
    void endFrame(uint64_t retireValue);
    bool frameOpen() const { return open_; }

    // Staging space in the current slice (alignment: power of two). Not ok() when the slice is full or no frame is open.
    Allocation allocate(VkDeviceSize size, VkDeviceSize alignment = 16);
    // Makes CPU writes to an allocation visible to the GPU (needed on non-coherent memory; call before submit).
    void flush(const Allocation& a);
    // Copies data into the slice and records vkCmdCopyBuffer(staging -> dst). False when the slice is full. No barriers: the
    // caller orders the destination (transfer -> use).
    bool upload(VkCommandBuffer cmd, VkBuffer dst, VkDeviceSize dstOffset, const void* data, VkDeviceSize size);

    // Bytes used in the current slice and the number of times a slice was refused by beginFrame (statistics for the Stats panel).
    VkDeviceSize usedBytes() const { return open_ ? head_ : 0; }
    uint64_t refusedFrames() const { return refused_; }

private:
    Device& device_;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    uint8_t* mapped_ = nullptr;
    VkDeviceSize slice_ = 0;
    uint32_t frames_ = 0;
    std::vector<uint64_t> retire_;   // per slice
    uint32_t next_ = 0;              // slice beginFrame will use next
    uint32_t current_ = 0;
    bool open_ = false;
    VkDeviceSize head_ = 0;
    uint64_t refused_ = 0;
};

} // namespace native::gpu
