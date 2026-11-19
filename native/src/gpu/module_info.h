// gpu -- owns the Vulkan device, queues, VMA memory, descriptors, upload ring, arenas and acceleration structures; depends only on base.
//
// Threads: render and bake threads (each uses its own queue).
// Spec: 01_ARCHITECTURE.md §5, 05_RENDERER.md §2; tasks T1.2, T1.5
//
// This header only identifies the module (used by the skeleton test that checks every module library links).
#pragma once

namespace native::gpu {

const char* moduleName();

} // namespace native::gpu
