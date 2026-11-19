# third_party/ (native editor)

Source copies, pinned to a release, each with its licence file next to it. Nothing here is edited; to update one, replace
the folder with the new release and change the line below.

| Folder | Name | Version | Licence | URL | Why |
|---|---|---|---|---|---|
| `qtads/` | Qt Advanced Docking System (`src/`, `cmake/`, top-level `CMakeLists.txt`; demo, examples, docs and the Python bindings are left out) | 5.1.1 | LGPL-2.1 (`qtads/LICENSE`, `qtads/gnu-lgpl-v2.1.md`) | https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System | dock panels, floating windows, saved layouts. Built as a **shared** library (DLL) next to the exe so the editor can relink against a modified copy (LGPL). 02_BUILD_AND_TOOLING.md named "4.x"; 5.1.1 was the latest release when T0.3 was done and supports Qt 6.8+ |
| `vma/` | Vulkan Memory Allocator (`vk_mem_alloc.h`) | 3.5.0 | MIT (`vma/LICENSE.txt`) | https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator | every buffer / image allocation. Header-only; exactly one `.cpp` defines `VMA_IMPLEMENTATION` |
| `volk/` | volk (`volk.h`, `volk.c`) | tag `vulkan-sdk-1.4.363.0` (header version 363) | MIT (`volk/LICENSE.md`) | https://github.com/zeux/volk | loads Vulkan entry points at run time (no link-time dependency on `vulkan-1.lib`) |

Vulkan API headers are not here: native code uses the Core's copy (`core/vendored/vulkan`, SDK 1.4.363 headers, T0.2).
