# 02 — Build and tooling

## 1. Prerequisites (the user installs these; the implementer asks, never installs software itself)

| What | Version | Where it is found |
|---|---|---|
| Visual Studio 2022 (or Build Tools) with "Desktop development with C++" | 17.x | `vswhere` (as `build_core.bat` does) |
| Vulkan SDK | 1.4.363.0 (installed at `D:\VulkanSDK\1.4.363.0`) | env `VULKAN_SDK`; `glslangValidator` in `%VULKAN_SDK%\Bin` |
| Qt | 6.8 or newer (6.11.1 installed), component "MSVC 2022 64-bit" | env `QT_DIR` = the folder holding `lib\cmake\Qt6` (e.g. `C:\Qt\6.8.3\msvc2022_64`) |
| Ninja, CMake | the ones shipped with Visual Studio | PATH inside the vcvars environment |

Qt 6.11.1 (MSVC 2022 64-bit) is installed on the development machine at `D:\Qt\6.11.1\msvc2022_64`; `QT_DIR` is not set
there, so `build_native.bat` falls back to that folder (and to `C:\Qt\*\msvc2022_64`) when the variable is empty.

## 2. Third-party code added by this rewrite

All go into `native/third_party/` as source, pinned to a release, with their LICENSE file next to them and a line in
`native/third_party/README.md` (name, version, licence, URL, why).

| Library | Licence | Use | How |
|---|---|---|---|
| Qt Advanced Docking System (`githubuser0xFFFF/Qt-Advanced-Docking-System`) | LGPL-2.1 | dock panels, floating windows, saved layouts | built as a **shared** library (DLL) next to the exe (LGPL compliance); `add_subdirectory` |
| Vulkan Memory Allocator (`GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator`) | MIT | all buffer / image allocations | header-only; one `.cpp` with `VMA_IMPLEMENTATION` |
| volk (`zeux/volk`) | MIT | loads Vulkan entry points at run time (no link-time dependency on `vulkan-1.lib`, same idea as the Core) | one `.c` file |

Qt itself is linked dynamically (`Qt6::Widgets`, `Qt6::Gui`, `Qt6::Core`, `Qt6::Network` for the automation socket) and
deployed with `windeployqt`.

## 3. Vulkan headers

The Core vendors Vulkan headers version 275 (`core/vendored/vulkan`). They predate `VK_EXT_ray_tracing_invocation_reorder`,
`VK_EXT_descriptor_heap`, `VK_KHR_ray_tracing_position_fetch` and others the native app needs. Task T0.2 replaces
`core/vendored/vulkan/vulkan/*` (and `vk_video/*`) with the headers of SDK 1.4.363 (`%VULKAN_SDK%\Include\vulkan`,
`%VULKAN_SDK%\Include\vk_video`). The Core must still build and its GPU tests must still pass (newer headers are source
compatible; the Core loads the loader at run time). Native code uses the same vendored headers through `core_lib`'s include
path, so there is exactly one copy.

## 4. CMake layout

`native/CMakeLists.txt`:
- `cmake_minimum_required(VERSION 3.24)`, `project(mapeditor_native CXX C)`, C++20.
- `add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core ${CMAKE_BINARY_DIR}/core EXCLUDE_FROM_ALL)` — gives `core_lib`
  (and lets `core_tests` be built on request).
- `find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)` with `CMAKE_PREFIX_PATH=$ENV{QT_DIR}`.
- Static libraries per module (`native_base`, `native_doc`, `native_scene`, `native_gpu`, `native_light`, `native_render`,
  `native_tools`, `native_ui`) so the dependency rule in 01_ARCHITECTURE.md §4 is checked by the linker.
- `mapeditor_native` (WIN32 executable) links them all; `native_tests` (console) links everything below `ui` + doctest.
- Shader step: `native/shaders/manifest.txt` with the same line format as `core/shaders/manifest.txt`
  (`name | source | target-env | defines`), compiled with `glslangValidator -V --target-env vulkan1.3` and validated with
  `spirv-val`; outputs embedded with `core/tools/spv_to_header.cmake`. No checked-in SPIR-V for native shaders (the SDK is
  a prerequisite).
- Post-build: copy the Qt ADS DLL, run `windeployqt` (Release only), copy the OIDN DLLs from
  `core/third_party_bin/oidn-*/bin` (as `core/CMakeLists.txt` already does for its own targets).

## 5. Scripts

- `build_native.bat [Release|Debug]` at the repo root, modelled on `build_core.bat`: finds Visual Studio with `vswhere`,
  calls `vcvars64.bat`, checks `QT_DIR` and `VULKAN_SDK` (clear message when missing), configures
  `cmake -S native -B native/build -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG%`, builds `mapeditor_native` and `native_tests`.
  It does **not** run tests.
- `run_native.bat [assetsRoot] [map]` starts the exe.

## 6. Command line of the app

```
mapeditor_native.exe [--assets <folder>] [--map <assets-relative .map.json>] [--automation-port <port>]
                     [--no-lighting] [--validation] [--log <file>]
```
- `--validation` enables `VK_LAYER_KHRONOS_validation` and counts its messages (tests and debugging).
- `--automation-port` opens the local JSON socket (08_MILESTONES.md M9). Default: off.

## 7. Running tests

- Native: `native\build\native_tests.exe -tc="*<case>*"`. GPU cases are tagged `[gpu]` and skip themselves (with a message)
  when no ray-tracing device is found.
- Core (only when a task changed Core code): `cmake --build native\build --target core_tests`, then
  `native\build\core\core_tests.exe -tc="*<case>*"` with the cases the task names. Never the full suite.
- Export parity (09_TESTING_AND_PERF.md §2) uses `bsp_export_tool` (`cmake --build native\build --target bsp_export_tool`).

## 8. Debugging and profiling aids (build them early, T1.x)

- `log.h`: timestamped lines to `%LOCALAPPDATA%\mapeditor_native\log.txt` and the debugger.
- GPU timestamps around every pass (`VkQueryPool` per frame, results read one frame late) shown in a **Stats** panel and
  written to the log once a second while it is open.
- A `--trace <file>` option writing Chrome trace JSON (`chrome://tracing` / Perfetto) for jobs, frames and bake steps.
