// gpu/vma_impl -- the single translation unit that holds the Vulkan Memory Allocator implementation.
//
// Vulkan entry points come from volk at run time (no link-time dependency on vulkan-1.lib): VMA is told to fetch everything
// else itself through vkGetInstanceProcAddr / vkGetDeviceProcAddr (see Device::init).
#include <volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
