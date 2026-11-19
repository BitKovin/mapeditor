// gpu/device -- the one VkInstance / VkDevice of the process, its queues, the VMA allocator and DeviceCaps.
//
// Owns: Vulkan start-up (volk, instance, physical device choice, feature chain, queues, VMA) and the DeviceCaps record that
// the rest of the code reads (nothing else queries Vulkan features, 05_RENDERER.md §2). Thread: create / destroy on the UI
// thread; the handles may then be used from any thread (queues are used by one thread each: graphics = render thread,
// compute = bake thread, 01_ARCHITECTURE.md §5).
// Spec: 05_RENDERER.md §2, 01_ARCHITECTURE.md §5, 08_MILESTONES.md T1.2.
//
// The Core's own GPU code (gpu_bake.cpp) will be handed this device later (T9.2); until then it keeps creating its own.
#pragma once

#include <volk.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <vk_mem_alloc.h>

namespace native::gpu {

struct DeviceOptions {
    bool validation = false;        // --validation: VK_LAYER_KHRONOS_validation + debug messenger (errors are counted)
    bool requireRayTracing = false; // fail instead of falling back to a device without ray query (Lit would be off)
    int forceDeviceIndex = -1;      // >= 0: use the n-th physical device (for tests / multi-GPU machines)
};

// Everything the rest of the program may ask about the device. Optional features are on only when the device exposes the
// feature bit AND the extension was enabled.
struct DeviceCaps {
    std::string deviceName;
    std::string driver;             // "NVIDIA 610.88" style when decodable, else the driver's own info string
    std::string apiVersion;         // "1.4.xxx" of the physical device
    bool discrete = false;

    // Required for Lit (05 §2). Lit is available when all of the first three are on.
    bool accelerationStructure = false;   // VK_KHR_acceleration_structure + deferred_host_operations
    bool rayQuery = false;                // VK_KHR_ray_query
    bool rayTracingPipeline = false;      // VK_KHR_ray_tracing_pipeline (gather pass; falls back to ray query without)
    // Optional.
    bool invocationReorder = false;       // VK_EXT_ray_tracing_invocation_reorder (SER)
    bool opacityMicromap = false;         // VK_EXT_opacity_micromap
    bool positionFetch = false;           // VK_KHR_ray_tracing_position_fetch
    bool globalPriority = false;          // VK_KHR_global_priority applied to at least one queue
    bool globalPriorityHigh = false;      // ... and the graphics queue got HIGH (needs rights the driver may withhold)
    bool hostImageCopy = false;           // VK_EXT_host_image_copy
    bool externalMemoryWin32 = false;     // VK_KHR_external_memory_win32
    bool shaderAtomicFloat = false;       // VK_EXT_shader_atomic_float (buffer float32 add)
    bool partitionedAccelerationStructure = false;  // VK_NV_partitioned_acceleration_structure (M12)
    bool clusterAccelerationStructure = false;      // VK_NV_cluster_acceleration_structure (M12)
    bool descriptorHeap = false;          // VK_EXT_descriptor_heap (not used in v1)

    bool dedicatedComputeQueue = false;   // the compute queue is from a different family than graphics
    bool lit() const { return accelerationStructure && rayQuery; }

    // Name / value rows for the Stats panel, in a fixed order.
    std::vector<std::pair<std::string, std::string>> rows() const;
};

struct AllocationStats {
    uint32_t allocationCount = 0;   // live VMA allocations
    uint32_t blockCount = 0;        // live VkDeviceMemory blocks
    uint64_t allocationBytes = 0;
};

class Device {
public:
    struct MessageCounts;   // validation message counters (defined in device.cpp)

    // Returns null and fills *error when no usable device exists (no loader, no Vulkan 1.3 device, ray tracing required but
    // absent ...).
    static std::unique_ptr<Device> create(const DeviceOptions& options, std::string* error);
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return physical_; }
    VkDevice device() const { return device_; }
    VmaAllocator allocator() const { return allocator_; }
    const DeviceCaps& caps() const { return caps_; }

    uint32_t graphicsFamily() const { return graphicsFamily_; }
    uint32_t computeFamily() const { return computeFamily_; }
    VkQueue graphicsQueue() const { return graphicsQueue_; }
    VkQueue computeQueue() const { return computeQueue_; }   // == graphicsQueue() when the device has no second queue

    // Validation layer messages seen so far (always 0 without options.validation).
    uint64_t validationErrors() const;
    uint64_t validationWarnings() const;
    bool validationActive() const { return messenger_ != VK_NULL_HANDLE; }

    AllocationStats allocationStats() const;

private:
    Device() = default;
    bool init(const DeviceOptions& options, std::string* error);

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = nullptr;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE, computeQueue_ = VK_NULL_HANDLE;
    uint32_t graphicsFamily_ = 0, computeFamily_ = 0;
    DeviceCaps caps_;
    std::unique_ptr<MessageCounts> counts_;
};

} // namespace native::gpu
