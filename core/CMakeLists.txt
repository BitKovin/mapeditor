cmake_minimum_required(VERSION 3.20)
project(mapeditor_core CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Everything third-party is header-only and lives in vendored/ -- no find_package, no vcpkg.
set(VENDORED "${CMAKE_CURRENT_SOURCE_DIR}/vendored")
add_library(vendored INTERFACE)
target_include_directories(vendored SYSTEM INTERFACE
    "${VENDORED}/boost_include" "${VENDORED}/nlohmann_json" "${VENDORED}/doctest" "${VENDORED}/vulkan" "${VENDORED}/oidn" "${VENDORED}/miniz")
target_compile_definitions(vendored INTERFACE BOOST_ALL_NO_LIB=1)
if(WIN32)
  # Asio needs a target Windows version; 0x0A00 = Windows 10.
  target_compile_definitions(vendored INTERFACE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

function(core_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive- /bigobj /wd4100)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra)
  endif()
endfunction()

# ---- shaders (compiled together with the Core) ----------------------------------------------------------------------------
# shaders/manifest.txt lists them. With glslangValidator available (PATH, %VULKAN_SDK%\Bin, C:\VulkanSDK\*\Bin) every build
# recompiles the ones whose source changed. Without it the checked-in SPIR-V (src/prebuilt) is used, but only when its recorded
# source hash still matches the shader source: a stale header is a configure error, never a silent old shader.
option(MAPEDITOR_COMPILE_SHADERS "Compile shaders with glslangValidator when it is found" ON)
set(SHADER_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(SHADER_PREBUILT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/prebuilt")
set(GLSLANG_VALIDATOR "")
if(MAPEDITOR_COMPILE_SHADERS)
  set(_sdk_hints)
  if(DEFINED ENV{VULKAN_SDK})
    list(APPEND _sdk_hints "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
  endif()
  if(WIN32)
    file(GLOB _sdk_dirs "C:/VulkanSDK/*/Bin")
    list(APPEND _sdk_hints ${_sdk_dirs})
  endif()
  find_program(GLSLANG_VALIDATOR_EXE NAMES glslangValidator HINTS ${_sdk_hints})
  set(GLSLANG_VALIDATOR "${GLSLANG_VALIDATOR_EXE}")
endif()

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/manifest.txt" _manifest)
set(_shader_outputs)
foreach(_line IN LISTS _manifest)
  string(STRIP "${_line}" _line)
  if(_line STREQUAL "" OR _line MATCHES "^#")
    continue()
  endif()
  string(REPLACE "|" ";" _f "${_line}")
  list(GET _f 0 _name)
  list(GET _f 1 _source)
  list(GET _f 2 _env)
  list(LENGTH _f _n)
  set(_defs "")
  if(_n GREATER 3)
    list(GET _f 3 _defs)
  endif()
  set(_src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_source}")
  set(_def_args)
  string(REPLACE "," ";" _def_list "${_defs}")
  foreach(_d IN LISTS _def_list)
    if(NOT _d STREQUAL "")
      list(APPEND _def_args "-D${_d}")
    endif()
  endforeach()
  if(GLSLANG_VALIDATOR)
    set(_spv "${SHADER_GEN_DIR}/${_name}.spv")
    set(_hdr "${SHADER_GEN_DIR}/core_shaders/${_name}.spv.h")
    add_custom_command(
      OUTPUT "${_hdr}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_GEN_DIR}/core_shaders"
      COMMAND "${GLSLANG_VALIDATOR}" -V --target-env ${_env} ${_def_args} "${_src}" -o "${_spv}"
      COMMAND ${CMAKE_COMMAND} "-DSPV=${_spv}" "-DOUT=${_hdr}" "-DNAME=${_name}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/spv_to_header.cmake"
      DEPENDS "${_src}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/manifest.txt" "${CMAKE_CURRENT_SOURCE_DIR}/tools/spv_to_header.cmake"
      COMMENT "Compiling shader ${_source}"
      VERBATIM)
    list(APPEND _shader_outputs "${_hdr}")
  else()
    file(READ "${_src}" _text)
    string(REPLACE "\r" "" _text "${_text}")
    string(SHA256 _hash "${_text}\n${_line}")
    set(_prebuilt "${SHADER_PREBUILT_DIR}/core_shaders/${_name}.spv.h")
    set(_ok FALSE)
    if(EXISTS "${_prebuilt}")
      file(STRINGS "${_prebuilt}" _hl REGEX "^// source-sha256: " LIMIT_COUNT 1)
      if(_hl STREQUAL "// source-sha256: ${_hash}")
        set(_ok TRUE)
      endif()
    endif()
    if(NOT _ok)
      message(FATAL_ERROR "Shader ${_source} changed (or its prebuilt SPIR-V is missing) and glslangValidator was not found.\n"
                          "Install the Vulkan SDK (https://vulkan.lunarg.com; it sets VULKAN_SDK) or put glslangValidator on PATH, "
                          "then re-run the build. Maintainers: python3 tools/compile_shaders.py refreshes src/prebuilt.")
    endif()
  endif()
endforeach()

if(GLSLANG_VALIDATOR)
  message(STATUS "Shaders: compiling with ${GLSLANG_VALIDATOR}")
  add_custom_target(core_shaders DEPENDS ${_shader_outputs})
  set(CORE_SHADER_INCLUDE "${SHADER_GEN_DIR}")
else()
  message(STATUS "Shaders: glslangValidator not found, using checked-in SPIR-V (source hashes verified)")
  set(CORE_SHADER_INCLUDE "${SHADER_PREBUILT_DIR}")
endif()

# core_lib: pure logic (no sockets), so it is unit-testable.
add_library(core_lib STATIC
    src/brush.cpp src/entity_class.cpp src/document.cpp src/prefab.cpp
    src/scene.cpp src/router.cpp src/origin.cpp src/profile.cpp src/textures.cpp src/sky.cpp src/texgroups.cpp
    src/entdef.cpp src/settings.cpp src/native_dialog.cpp src/csg.cpp src/decal.cpp src/derived.cpp
    src/lighting_classes.cpp src/lightmap_uv.cpp src/lights.cpp src/lightbake.cpp src/parallel.cpp src/gpu_bake.cpp src/raytrace.cpp src/live_bake.cpp src/lighting_service.cpp src/relief_geometry.cpp
    src/albedo.cpp src/lightvol.cpp src/light_styles.cpp src/light_style_analysis.cpp src/denoise.cpp
    src/exr.cpp src/bsp_convex.cpp src/bsp_compile.cpp src/bsp_export.cpp src/exporter.cpp
    src/texplan_props.cpp src/texplan_uvmesh.cpp src/texplan_layout.cpp src/texplan_relief.cpp)
target_include_directories(core_lib PUBLIC include)
target_link_libraries(core_lib PUBLIC vendored ${CMAKE_DL_LIBS})
core_warnings(core_lib)
if(WIN32)
  target_link_libraries(core_lib PUBLIC ole32 shell32 uuid)  # native file dialogs (native_dialog.cpp)
endif()
target_include_directories(core_lib PUBLIC "${CORE_SHADER_INCLUDE}")
if(GLSLANG_VALIDATOR)
  add_dependencies(core_lib core_shaders)
endif()
if(NOT MSVC)
  # the VkFoo x{VK_STRUCTURE_TYPE_FOO} idiom would otherwise flood -Wextra
  set_source_files_properties(src/gpu_bake.cpp PROPERTIES COMPILE_OPTIONS -Wno-missing-field-initializers)
endif()

add_executable(lightbake_tool src/lightbake_main.cpp)
target_link_libraries(lightbake_tool PRIVATE core_lib)
core_warnings(lightbake_tool)

add_executable(bsp_export_tool src/bsp_export_main.cpp)
target_link_libraries(bsp_export_tool PRIVATE core_lib)
core_warnings(bsp_export_tool)

add_executable(mapeditor_core src/main.cpp src/ws_server.cpp)
target_link_libraries(mapeditor_core PRIVATE core_lib)
if(WIN32)
  target_link_libraries(mapeditor_core PRIVATE ws2_32 mswsock)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(mapeditor_core PRIVATE Threads::Threads)
endif()
core_warnings(mapeditor_core)

enable_testing()
add_executable(core_tests
    tests/test_main.cpp tests/test_brush.cpp tests/test_document.cpp tests/test_prefab.cpp
    tests/test_scene.cpp tests/test_router.cpp tests/test_textures.cpp tests/test_texgroups.cpp
    tests/test_entdef.cpp tests/test_settings.cpp tests/test_config_ui.cpp tests/test_csg.cpp tests/test_derived.cpp
    tests/test_lightmap_uv.cpp tests/test_lightbake.cpp tests/test_raytrace.cpp tests/test_bounce.cpp tests/test_live_bake.cpp tests/test_lighting_service.cpp tests/test_denoise.cpp tests/test_lightvol.cpp tests/test_light_styles.cpp tests/test_albedo.cpp tests/test_relief_geometry.cpp tests/test_level_settings.cpp tests/test_transparency.cpp tests/test_bsp.cpp tests/test_texplan.cpp tests/test_exporters.cpp)
target_include_directories(core_tests PRIVATE tests src)
target_compile_definitions(core_tests PRIVATE TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../testdata")
target_link_libraries(core_tests PRIVATE core_lib)
core_warnings(core_tests)
add_test(NAME core_tests COMMAND core_tests)

# Copy nothing at build time: run from core/ (or pass the assets root) -- see run.bat.

# Latency benchmark of the live lighting pipeline (noise instead of light; see bench/bench_live.cpp). Not part of `all`:
#   cmake --build <dir> --target bench_live
add_executable(bench_live EXCLUDE_FROM_ALL bench/bench_live.cpp)
target_include_directories(bench_live PRIVATE tests src)
target_compile_definitions(bench_live PRIVATE BENCH_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
target_link_libraries(bench_live PRIVATE core_lib)
core_warnings(bench_live)

# Intel Open Image Denoise binaries (not shipped, see vendored/README.md): when a release is unpacked into third_party_bin/oidn-*,
# its DLLs are copied next to the executables at configure time (core/denoise.h opens the library at run time).
file(GLOB OIDN_BIN_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/third_party_bin/oidn-*/bin")
if(OIDN_BIN_DIRS)
  list(GET OIDN_BIN_DIRS 0 OIDN_BIN_DIR)
  file(GLOB OIDN_DLLS "${OIDN_BIN_DIR}/*.dll" "${OIDN_BIN_DIR}/*.so*")
  file(COPY ${OIDN_DLLS} DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
  message(STATUS "OpenImageDenoise: ${OIDN_BIN_DIR}")
endif()
