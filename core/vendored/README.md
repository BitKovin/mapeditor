# vendored/

Third-party, header-only dependencies, checked in so the Core builds with **no package manager
and no system libraries** (just a C++20 compiler, CMake and Ninja).

| Path | What | Version | License |
|---|---|---|---|
| `boost_include/boost/` | Boost.Beast + Asio + System (+ their transitive headers, extracted with `bcp`) | 1.83 | Boost Software License 1.0 |
| `nlohmann_json/nlohmann/` | JSON for Modern C++ | 3.11.3 | MIT |
| `miniz/miniz.h` | miniz (zlib-compatible deflate / inflate; single header, copied from the engine repo `SdlWasmEngine/source/Engine/Compression/zip/miniz_once.h`), used by the EXR lightmap writer `core/exr.h` | 3.1.0 | Unlicense (public domain) |
| `doctest/doctest.h` | doctest (tests only) | 2.4.11 | MIT |
| `vulkan/vulkan/`, `vulkan/vk_video/` | Vulkan API headers (types and constants only; the loader is opened at run time, no SDK needed) | 1.3.275 | Apache-2.0 |
| `oidn/OpenImageDenoise/` | Intel Open Image Denoise C API headers (`oidn.h`, generated `config.h`; only types and constants, the library is opened at run time: `core/denoise.h`). The binary release (OpenImageDenoise + its tbb / device libraries) is NOT shipped; place it next to the executable, point `MAPEDITOR_OIDN` at it, or unpack the release zip into `core/third_party_bin/` (CMake copies its DLLs next to the executables) | 2.5.1 | Apache-2.0 (`oidn/LICENSE.txt`) |

To update Boost: `bcp --boost=<boost-root> boost/beast.hpp boost/beast/websocket.hpp boost/asio.hpp
boost/system.hpp <out>` and copy `<out>/boost` over `boost_include/boost` (ignore the system
headers bcp also copies next to it).
