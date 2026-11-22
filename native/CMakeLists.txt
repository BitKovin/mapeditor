cmake_minimum_required(VERSION 3.24)
project(mapeditor_native CXX C)

# Native editor: one process, Qt 6 UI, Vulkan, the Core linked in as a library (native_rewrite/01_ARCHITECTURE.md).
# Module libraries follow the dependency rule of 01 Р’В§4:  base <- doc <- scene <- light, render <- tools <- ui <- app
# (gpu depends only on base). The linker therefore rejects an upward include that sneaks in.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Executables, tests and DLLs end up in one folder (windeployqt and the Qt ADS DLL land next to the exe).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_AUTOMOC ON)

# ---- Qt ---------------------------------------------------------------------------------------------------------------
# QT_DIR = the folder holding lib/cmake/Qt6 (02_BUILD_AND_TOOLING.md Р’В§1). build_native.bat finds it when the variable is unset.
if(DEFINED ENV{QT_DIR})
  list(PREPEND CMAKE_PREFIX_PATH "$ENV{QT_DIR}")
endif()
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network Test)
message(STATUS "Qt ${Qt6_VERSION} at ${Qt6_DIR}")

# ---- Qt Advanced Docking System (docking, floating windows, saved layouts; LGPL, shared library) ----------------------------
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/qtads EXCLUDE_FROM_ALL)
# ADS puts its DLL under <build>/x64/bin; the exe needs it next to itself.
set_target_properties(qtadvanceddocking-qt6 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# ---- the Core (same process, same memory) -------------------------------------------------------------------------------
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../core" "${CMAKE_BINARY_DIR}/core" EXCLUDE_FROM_ALL)

function(native_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8 /permissive- /bigobj /wd4100)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
  endif()
endfunction()

function(native_module name)
  cmake_parse_arguments(M "" "" "SOURCES;DEPENDS" ${ARGN})
  add_library(native_${name} STATIC ${M_SOURCES})
  target_include_directories(native_${name} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(native_${name} PUBLIC ${M_DEPENDS})
  native_warnings(native_${name})
  if(MSVC)
    # windows.h first, its `near` / `far` macros removed: the Core uses `near` as a member name and Vulkan / Qt headers pull windows.h in at any point
    target_compile_options(native_${name} PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/base/win_clean.h")
  endif()
endfunction()

# ---- shaders ------------------------------------------------------------------------------------------------------------
# native/shaders/manifest.txt (same line format as core/shaders/manifest.txt) -> glslangValidator -> spirv-val -> a header with a
# uint32_t array per shader in native::shaders, included as "native_shaders/<name>.spv.h". The Vulkan SDK is a prerequisite:
# no SPIR-V is checked in.
set(_sdk_hints)
if(DEFINED ENV{VULKAN_SDK})
  list(APPEND _sdk_hints "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
endif()
find_program(GLSLANG_VALIDATOR NAMES glslangValidator HINTS ${_sdk_hints})
find_program(SPIRV_VAL NAMES spirv-val HINTS ${_sdk_hints})
find_program(DXC NAMES dxc HINTS ${_sdk_hints})   # HLSL kernels (R1): `.hlsl` entries of the manifest are compiled with DXC to SPIR-V
if(NOT GLSLANG_VALIDATOR)
  message(FATAL_ERROR "glslangValidator not found: install the Vulkan SDK and set VULKAN_SDK")
endif()
set(NATIVE_SHADER_GEN_DIR "${CMAKE_BINARY_DIR}/shaders")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/manifest.txt" _native_manifest)
set(_native_shader_headers)
file(GLOB _native_glsl "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.glsl" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.hlsli" "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sharc/include/*.h")
foreach(_line IN LISTS _native_manifest)
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
  set(_spv "${NATIVE_SHADER_GEN_DIR}/${_name}.spv")
  set(_hdr "${NATIVE_SHADER_GEN_DIR}/native_shaders/${_name}.spv.h")
  set(_validate)
  if(SPIRV_VAL)
    set(_validate COMMAND "${SPIRV_VAL}" --target-env ${_env} "${_spv}")
  endif()
  # GLSL: glslangValidator, the 4th field = preprocessor defines. HLSL (.hlsl): DXC, the 4th field = the target profile (cs_6_5), entry point `main`.
  if(_source MATCHES "\.hlsl$")
    if(NOT DXC)
      message(FATAL_ERROR "dxc not found (needed for ${_source}): install the Vulkan SDK and set VULKAN_SDK")
    endif()
    set(_compile COMMAND "${DXC}" -spirv -fspv-target-env=${_env} -T ${_defs} -E main -fspv-extension=SPV_KHR_ray_query
                 -I "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "${_src}" -Fo "${_spv}")
  else()
    set(_compile COMMAND "${GLSLANG_VALIDATOR}" -V --target-env ${_env} ${_def_args} "${_src}" -o "${_spv}")
  endif()
  add_custom_command(
    OUTPUT "${_hdr}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${NATIVE_SHADER_GEN_DIR}/native_shaders"
    ${_compile}
    ${_validate}
    COMMAND "${CMAKE_COMMAND}" "-DSPV=${_spv}" "-DOUT=${_hdr}" "-DNAME=${_name}" "-DNS=native::shaders"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/../core/tools/spv_to_header.cmake"
    DEPENDS "${_src}" ${_native_glsl} "${CMAKE_CURRENT_SOURCE_DIR}/shaders/manifest.txt" "${CMAKE_CURRENT_SOURCE_DIR}/../core/tools/spv_to_header.cmake"
    COMMENT "Compiling shader ${_source}"
    VERBATIM)
  list(APPEND _native_shader_headers "${_hdr}")
endforeach()
add_custom_target(native_shaders DEPENDS ${_native_shader_headers})

# ---- module libraries ---------------------------------------------------------------------------------------------------
find_package(Threads REQUIRED)
native_module(base   SOURCES src/base/module_info.cpp src/base/hash.cpp src/base/jobs.cpp src/base/log.cpp src/base/trace.cpp
                     DEPENDS vendored Threads::Threads)
native_module(doc    SOURCES src/doc/module_info.cpp src/doc/document.cpp src/doc/document.h src/doc/io.cpp src/doc/io.h src/doc/json_write.cpp src/doc/json_write.h src/doc/changeset.cpp src/doc/changeset.h src/doc/commands.cpp src/doc/commands.h src/doc/level_settings.cpp src/doc/level_settings.h src/doc/edit.cpp src/doc/edit.h src/doc/selection.cpp src/doc/selection.h src/doc/prefabs.cpp src/doc/prefabs.h
                     DEPENDS native_base core_lib)
native_module(exporting SOURCES src/exporting/export_job.cpp src/exporting/export_job.h src/exporting/launch.cpp src/exporting/launch.h src/exporting/import_map.cpp src/exporting/import_map.h DEPENDS native_doc)
if(MSVC)
  target_compile_options(native_exporting PRIVATE /wd4828)   # core/lighting_service.h holds a character outside UTF-8 in a comment (a Core file: left as it is)
endif()
native_module(scene  SOURCES src/scene/module_info.cpp src/scene/model.cpp src/scene/model.h src/scene/model_entity.cpp src/scene/model_entity.h src/scene/model_store.cpp src/scene/model_store.h src/scene/gltf.cpp src/scene/gltf.h src/scene/builder.cpp src/scene/relief.cpp src/scene/relief.h src/scene/render_mesh.cpp src/scene/render_mesh.h src/scene/point_box.cpp src/scene/point_box.h src/scene/arrows.cpp src/scene/arrows.h src/scene/size_labels.cpp src/scene/size_labels.h src/scene/light_gizmo.cpp src/scene/light_gizmo.h src/scene/builder.h src/scene/cache.h src/scene/snapshot.h DEPENDS native_doc)
# volk (loader) and VMA (allocator) are third-party code: compiled without the project's warnings-as-errors.
add_library(native_vk STATIC third_party/volk/volk.c src/gpu/vma_impl.cpp)
target_include_directories(native_vk SYSTEM PUBLIC third_party/volk third_party/vma)
target_compile_definitions(native_vk PUBLIC VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR)
target_link_libraries(native_vk PUBLIC vendored)   # the Core's copy of the Vulkan headers (one copy, T0.2)
native_module(gpu    SOURCES src/gpu/module_info.cpp src/gpu/device.cpp src/gpu/streamline.cpp src/gpu/swapchain.cpp src/gpu/upload_ring.cpp src/gpu/arena.cpp src/gpu/gpu_timer.cpp DEPENDS native_base native_vk)
# NVIDIA Streamline (T6.7, 11_NVIDIA_TECH.md): optional. native/tools/fetch_streamline.ps1 unpacks the SDK into third_party/streamline
# (git-ignored). Without it gpu/streamline.cpp is a stub and the editor runs as before; with it the headers are used (the DLL is loaded
# at run time, never linked) and the interposer + plugins are copied next to the exe.
set(NATIVE_STREAMLINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/streamline")
if(EXISTS "${NATIVE_STREAMLINE_DIR}/include/sl.h")
  message(STATUS "Streamline SDK found: ${NATIVE_STREAMLINE_DIR}")
  target_compile_definitions(native_gpu PRIVATE NATIVE_HAS_STREAMLINE=1)
  target_include_directories(native_gpu SYSTEM PRIVATE "${NATIVE_STREAMLINE_DIR}/include")
  if(MSVC)
    target_compile_options(native_gpu PRIVATE /external:W0)
  endif()
  set(_sl_files sl.interposer.dll sl.common.dll sl.reflex.dll sl.pcl.dll NvLowLatencyVk.dll sl.dlss.dll nvngx_dlss.dll sl.dlss_d.dll nvngx_dlssd.dll)
  set(_sl_copy)
  foreach(_f IN LISTS _sl_files)
    list(APPEND _sl_copy COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${NATIVE_STREAMLINE_DIR}/bin/x64/${_f}" "${CMAKE_BINARY_DIR}/${_f}")
  endforeach()
  add_custom_target(native_streamline_dlls ${_sl_copy} COMMENT "Streamline DLLs next to the exe" VERBATIM)
  add_dependencies(native_gpu native_streamline_dlls)
else()
  message(STATUS "Streamline SDK not found (run native/tools/fetch_streamline.ps1 to enable Reflex and DLSS)")
endif()
native_module(light  SOURCES src/light/module_info.cpp src/light/page_allocator.cpp src/light/page_allocator.h src/light/layout.cpp src/light/layout.h src/light/page_images.cpp src/light/page_images.h src/light/samples.cpp src/light/samples.h src/light/accel.cpp src/light/accel.h
                     src/light/light_pack.cpp src/light/light_pack.h src/light/results.cpp src/light/results.h src/light/passes.cpp src/light/passes.h src/light/mailbox.cpp src/light/mailbox.h
                     src/light/lit_mesh.cpp src/light/lit_mesh.h src/light/light_link.cpp src/light/light_link.h src/light/lighting_service.cpp src/light/lighting_service.h
                     DEPENDS native_scene native_gpu)
add_dependencies(native_light native_shaders)
target_include_directories(native_light PRIVATE "${NATIVE_SHADER_GEN_DIR}")
native_module(render SOURCES src/render/module_info.cpp src/render/model_pass.cpp src/render/model_pass.h src/render/model_renderer.cpp src/render/model_renderer.h src/render/model_scene.cpp src/render/model_scene.h src/render/model_draws.h src/render/frustum.h src/render/render_thread.cpp src/render/scene_pass.cpp src/render/texture_table.cpp src/render/gpu_scene.cpp src/render/mesh_pass.cpp src/render/dlss_pass.cpp src/render/rt_scene.cpp src/render/rt_scene.h src/render/rt_lit.cpp src/render/rt_lit.h src/render/rt_lights.cpp src/render/rt_lights.h src/render/rt_lighting.cpp src/render/rt_lighting.h src/render/dlss_math.h src/render/picking.cpp src/render/picking.h src/render/selection_outline.cpp src/render/selection_outline.h src/render/line_vertex.h
                     src/render/viewport_window.cpp src/render/viewport_window.h src/render/lightmap_view.cpp src/render/lightmap_view.h
                     src/render/entity_overlay.cpp src/render/entity_overlay.h src/render/glyph_atlas.cpp src/render/glyph_atlas.h
                     src/render/camera.cpp src/render/camera_rig.cpp src/render/viewport_settings.cpp
                     DEPENDS native_scene native_light native_gpu Qt6::Gui)
add_dependencies(native_render native_shaders)
target_include_directories(native_render PRIVATE "${NATIVE_SHADER_GEN_DIR}")
# The action system (registry, contexts, keymap layers, input router) is Qt-free and sits next to base: the viewport glue (render),
# the Qt adapter and menus (ui) and the tools all use it. 10_UI_AND_INPUT.md.
native_module(actions SOURCES src/app/actions/input.cpp src/app/actions/registry.cpp src/app/actions/contexts.cpp
                      src/app/actions/keymap.cpp src/app/actions/keymap_edit.cpp src/app/actions/palette.cpp src/app/actions/router.cpp DEPENDS native_base)
native_module(tools  SOURCES src/tools/light_list.cpp src/tools/light_list.h src/tools/context_menu.cpp src/tools/context_menu.h src/tools/lighting_config.cpp src/tools/lighting_config.h src/tools/texturing/surface.cpp src/tools/texturing/surface.h src/tools/texturing/face_edit.cpp src/tools/texturing/inspector.cpp src/tools/texturing/inspector.h src/tools/texturing/browser.cpp src/tools/texturing/browser.h src/tools/texturing/props_edit.cpp src/tools/texturing/props_edit.h src/tools/texturing/group_tools.cpp src/tools/texturing/group_tools.h src/tools/texturing/tileset_edit.cpp src/tools/texturing/tileset_edit.h src/tools/relief/relief_edit.cpp src/tools/relief/relief_edit.h src/tools/parametric.cpp src/tools/parametric.h src/tools/prefabize.cpp src/tools/prefabize.h src/tools/prefab_preview.cpp src/tools/prefab_preview.h src/tools/terrain.cpp src/tools/terrain.h src/tools/texturing/face_edit.h src/tools/texturing/hotspots.cpp src/tools/texturing/hotspots.h src/tools/texturing/density.cpp src/tools/texturing/density.h src/tools/texturing/flow.cpp src/tools/texturing/flow.h src/tools/texturing/ftt.cpp src/tools/texturing/ftt.h src/tools/module_info.cpp src/tools/mesh/topology.cpp src/tools/mesh/topology.h src/tools/mesh/ops.cpp src/tools/mesh/ops_loop.cpp src/tools/mesh/ops.h src/tools/mesh/cut.cpp src/tools/mesh/cut.h src/tools/mesh/select.cpp src/tools/mesh/select.h src/tools/mesh/pick.cpp src/tools/mesh/pick.h src/tools/mesh/session.cpp src/tools/mesh/session.h src/tools/brush_edit.cpp src/tools/brush_edit.h src/tools/brush_ops.cpp src/tools/brush_ops.h src/tools/face_drag.cpp src/tools/face_drag.h src/tools/resize2d.cpp src/tools/resize2d.h src/tools/gizmo.cpp src/tools/gizmo.h src/tools/modal.cpp src/tools/modal.h src/tools/transform.cpp src/tools/transform.h src/tools/gizmo_overlay.cpp src/tools/gizmo_overlay.h src/tools/box_tool.cpp src/tools/box_tool.h src/tools/shapes.cpp src/tools/shapes.h src/tools/edit_ops.cpp src/tools/edit_ops.h src/tools/visibility.cpp src/tools/visibility.h src/tools/edit_ops.cpp src/tools/edit_ops.h src/tools/visibility.cpp src/tools/visibility.h src/tools/properties.cpp src/tools/properties.h src/tools/entity_edit.cpp src/tools/entity_edit.h src/tools/ref_targets.cpp src/tools/ref_targets.h src/tools/outliner.cpp src/tools/outliner.h src/tools/details.cpp src/tools/details.h src/tools/prefs.cpp src/tools/prefs.h src/tools/profile_form.cpp src/tools/profile_form.h src/tools/status_text.cpp src/tools/status_text.h src/tools/select_ops.cpp src/tools/select_ops.h src/tools/measure.cpp src/tools/measure.h
                     DEPENDS native_render native_light)
native_module(ui     SOURCES src/ui/main_window_facedrag.cpp src/ui/main_window_angle.cpp src/ui/main_window_export.cpp src/ui/main_window_mesh.cpp src/ui/main_window_brush.cpp src/ui/brush_actions.cpp src/ui/brush_actions.h src/ui/texture_actions.cpp src/ui/texture_actions.h src/ui/surface_panel.cpp src/ui/surface_panel.h src/ui/texture_browser_panel.cpp src/ui/texture_browser_panel.h src/ui/ftt_window.cpp src/ui/ftt_window.h src/ui/texture_props_window.cpp src/ui/texture_props_window.h src/ui/main_window_texprops.cpp src/ui/main_window_groups.cpp src/ui/texgroup_editor.cpp src/ui/texgroup_editor.h src/ui/level_settings_panel.cpp src/ui/hide_dialog.cpp src/ui/main_window_measure.cpp src/ui/theme.cpp src/ui/theme.h src/ui/main_window_bookmarks.cpp src/ui/history_panel.cpp src/ui/history_panel.h src/ui/messages_panel.cpp src/ui/messages_panel.h src/ui/adjust_panel.cpp src/ui/adjust_panel.h src/ui/lighting_panel.cpp src/ui/lighting_panel.h src/ui/main_window_lights.cpp src/ui/tool_options_panel.cpp src/ui/tool_options_panel.h src/ui/main_window_adjust.cpp src/ui/select_actions.cpp src/ui/select_actions.h src/ui/snap_actions.cpp src/ui/snap_actions.h src/ui/level_settings_panel.h src/ui/groups_panel.cpp src/ui/groups_panel.h src/ui/tileset_editor.cpp src/ui/tileset_editor.h src/ui/relief_editor.cpp src/ui/relief_editor.h src/ui/main_window_ftt.cpp src/ui/main_window_texture.cpp src/ui/mesh_actions.cpp src/ui/mesh_actions.h src/ui/module_info.cpp src/ui/main_window.cpp src/ui/main_window_document.cpp src/ui/main_window_transform.cpp src/ui/main_window_box.cpp src/ui/main_window_edit.cpp src/ui/input_probe.cpp src/ui/input_probe.h src/ui/main_window_edit.cpp src/ui/transform_session.h src/ui/box_drawing.h src/ui/view_helpers.h src/ui/main_window.h src/ui/stats_panel.cpp src/ui/stats_panel.h src/ui/main_window_panels.cpp src/ui/outliner_panel.cpp src/ui/outliner_panel.h src/ui/details_panel.cpp src/ui/details_panel.h src/ui/place_panel.cpp src/ui/place_panel.h src/ui/main_window_settings.cpp src/ui/settings_dialog.cpp src/ui/settings_dialog.h src/ui/chord_capture.cpp src/ui/chord_capture.h src/ui/keymap_page.cpp src/ui/keymap_page.h src/ui/status_bar.cpp src/ui/status_bar.h src/ui/main_window_help.cpp src/ui/command_palette.cpp src/ui/command_palette.h src/ui/key_help_overlay.cpp src/ui/key_help_overlay.h
                     src/ui/input_adapter.cpp src/ui/input_adapter.h src/ui/app_actions.cpp src/ui/app_actions.h
                     src/ui/registry_menus.cpp src/ui/registry_menus.h src/ui/glyph_atlas_qt.cpp src/ui/glyph_atlas_qt.h
                     DEPENDS native_tools native_actions native_exporting Qt6::Widgets ads::qtadvanceddocking)

# The automation socket (T9.4): JSON lines on 127.0.0.1 for outside scripts; off unless --automation-port is given.
native_module(automation SOURCES src/automation/server.cpp src/automation/server.h DEPENDS native_ui Qt6::Network)

# ---- the application ----------------------------------------------------------------------------------------------------
add_executable(mapeditor_native WIN32 src/app/main.cpp src/app/app_options.cpp src/app/app_options.h)
target_link_libraries(mapeditor_native PRIVATE native_automation native_ui Qt6::Widgets)
native_warnings(mapeditor_native)
# Shipped keymap presets next to the exe (<exe dir>/keymaps/default.json).
add_custom_command(TARGET mapeditor_native POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets/keymaps" "${CMAKE_BINARY_DIR}/keymaps"
  COMMENT "keymap presets")

# ---- tests (headless; everything below ui) ------------------------------------------------------------------------------
# Qt6::Test is not used for assertions (doctest is), but the offscreen platform lets the UI tests build real widgets.
add_executable(native_tests tests/test_main.cpp tests/test_modules.cpp tests/test_app_options.cpp tests/test_base.cpp tests/test_gpu_device.cpp tests/test_render_thread.cpp tests/test_camera.cpp tests/test_gpu_memory.cpp tests/test_actions.cpp tests/test_input_adapter.cpp tests/test_doc_io.cpp tests/test_doc_json_write.cpp tests/test_doc_history.cpp tests/test_doc_selection.cpp tests/test_doc_prefabs.cpp tests/test_scene_builder.cpp tests/test_exporting.cpp tests/test_ui_export.cpp tests/test_ui_mesh.cpp tests/test_automation.cpp tests/test_mesh_topology.cpp tests/test_mesh_ops.cpp tests/test_mesh_ops_loop.cpp tests/test_mesh_cut.cpp tests/test_mesh_select.cpp tests/test_mesh_session.cpp tests/test_tools_brush_ops.cpp tests/test_tools_surface.cpp tests/test_tools_texturing.cpp tests/test_tools_face_edit.cpp tests/test_ui_brush.cpp tests/test_ui_texture.cpp tests/test_tools_browser.cpp tests/test_ui_ftt.cpp tests/test_tools_props.cpp tests/test_ui_props.cpp tests/test_tools_groups.cpp tests/test_tools_tileset.cpp tests/test_tools_relief.cpp tests/test_tools_parametric.cpp tests/test_tools_prefabize.cpp tests/test_tools_prefab_preview.cpp tests/test_tools_terrain.cpp tests/test_ui_prefabize.cpp tests/test_scene_model.cpp tests/test_scene_model_store.cpp tests/test_render_model_scene.cpp tests/test_render_texture_table.cpp tests/test_level_settings.cpp tests/test_ui_level.cpp tests/test_ui_context_menu.cpp tests/test_ui_hide.cpp tests/test_tools_measure.cpp tests/test_ui_theme.cpp tests/test_ui_history.cpp tests/test_ui_messages.cpp tests/test_ui_adjust.cpp tests/test_ui_lighting.cpp tests/test_ui_select.cpp tests/test_tools_lighting_config.cpp tests/test_ui_settings_lighting.cpp tests/test_ui_tileset.cpp tests/test_ui_relief.cpp tests/test_ui_groups.cpp tests/test_scene_relief.cpp tests/test_picking.cpp tests/test_scene_overlays.cpp tests/test_tools_transform.cpp tests/test_tools_modal.cpp tests/test_tools_gizmo.cpp tests/test_tools_box.cpp tests/test_tools_clipboard.cpp tests/test_tools_visibility.cpp tests/test_tools_properties.cpp tests/test_tools_outliner.cpp tests/test_tools_details.cpp tests/test_tools_settings.cpp tests/test_keymap_edit.cpp tests/test_palette.cpp tests/test_light_layout.cpp tests/test_light_images.cpp tests/test_light_samples.cpp tests/test_light_accel.cpp tests/test_rt_scene.cpp tests/test_rt_lit.cpp tests/test_light_bake.cpp tests/test_light_compose.cpp tests/test_light_service.cpp tests/test_lit_view.cpp src/app/app_options.cpp)
target_include_directories(native_tests PRIVATE src "${NATIVE_SHADER_GEN_DIR}")
add_dependencies(native_tests native_shaders)
target_compile_definitions(native_tests PRIVATE NATIVE_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
                           NATIVE_CORE_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../core/assets")
if(MSVC)
  set_source_files_properties(src/ui/main_window_facedrag.cpp src/ui/main_window_export.cpp src/ui/main_window_mesh.cpp src/ui/main_window_brush.cpp src/ui/brush_actions.cpp src/ui/texture_actions.cpp src/ui/main_window_texture.cpp src/ui/mesh_actions.cpp src/ui/mesh_actions.h PROPERTIES COMPILE_OPTIONS /wd4828)
  set_source_files_properties(src/tools/lighting_config.cpp tests/test_tools_lighting_config.cpp tests/test_ui_settings_lighting.cpp PROPERTIES COMPILE_OPTIONS /wd4828)   # core/lighting_service.h, see above
  set_source_files_properties(tests/test_exporting.cpp PROPERTIES COMPILE_OPTIONS /wd4828)
  set_source_files_properties(tests/test_ui_export.cpp tests/test_ui_mesh.cpp PROPERTIES COMPILE_OPTIONS /wd4828)
endif()
target_link_libraries(native_tests PRIVATE native_automation native_ui Qt6::Widgets Qt6::Network Qt6::Test)
native_warnings(native_tests)

# Tests that need real windows and the GPU (Qt "windows" platform): viewports in docks, re-docking, resizing.
add_executable(native_gui_tests tests/gui_main.cpp tests/test_gui_viewport.cpp tests/test_gui_input.cpp tests/test_gui_document.cpp tests/test_gui_select.cpp tests/test_gui_transform.cpp tests/test_gui_box.cpp tests/test_gui_edit.cpp tests/test_gui_panels.cpp tests/test_gui_settings.cpp tests/test_gui_keymap.cpp tests/test_gui_context.cpp tests/test_gui_bookmarks.cpp tests/test_gui_snap.cpp tests/test_gui_tool_options.cpp tests/test_gui_measure.cpp tests/test_gui_palette.cpp tests/test_gui_gate_m4.cpp tests/test_gui_lit.cpp tests/test_gui_hardware_input.cpp tests/test_gui_bench_dlss.cpp)
target_include_directories(native_gui_tests PRIVATE src)
target_compile_definitions(native_gui_tests PRIVATE NATIVE_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
                           NATIVE_CORE_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../core/assets")
target_link_libraries(native_gui_tests PRIVATE native_ui Qt6::Widgets)
native_warnings(native_gui_tests)

enable_testing()
add_test(NAME native_tests COMMAND native_tests)
add_test(NAME native_gui_tests COMMAND native_gui_tests)

# ---- deployment (Release only): Qt DLLs and plugins next to the exe -----------------------------------------------------
if(WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Release")
  get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
  get_filename_component(_qt_bin "${_qmake}" DIRECTORY)
  find_program(WINDEPLOYQT windeployqt HINTS "${_qt_bin}")
  if(WINDEPLOYQT)
    add_custom_command(TARGET mapeditor_native POST_BUILD
      COMMAND "${WINDEPLOYQT}" --no-translations --no-system-d3d-compiler --no-opengl-sw
              --no-compiler-runtime "$<TARGET_FILE:mapeditor_native>"
      COMMENT "windeployqt")
    add_custom_command(TARGET native_tests POST_BUILD
      COMMAND "${WINDEPLOYQT}" --no-translations --no-system-d3d-compiler --no-opengl-sw
              --no-compiler-runtime "$<TARGET_FILE:native_tests>"
      COMMENT "windeployqt (tests)")
    add_custom_command(TARGET native_gui_tests POST_BUILD
      COMMAND "${WINDEPLOYQT}" --no-translations --no-system-d3d-compiler --no-opengl-sw
              --no-compiler-runtime "$<TARGET_FILE:native_gui_tests>"
      COMMENT "windeployqt (gui tests)")
    # The UI tests run on Qt's offscreen platform, which windeployqt does not deploy.
    add_custom_command(TARGET native_tests POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_qt_bin}/../plugins/platforms/qoffscreen.dll"
              "${CMAKE_BINARY_DIR}/platforms/qoffscreen.dll"
      COMMENT "qoffscreen.dll for the tests")
  else()
    message(WARNING "windeployqt not found: run the exe with Qt's bin folder on PATH")
  endif()
endif()
