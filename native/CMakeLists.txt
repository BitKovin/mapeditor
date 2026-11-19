cmake_minimum_required(VERSION 3.24)
project(mapeditor_native CXX C)

# Native editor: one process, Qt 6 UI, Vulkan, the Core linked in as a library (native_rewrite/01_ARCHITECTURE.md).
# Module libraries follow the dependency rule of 01 §4:  base <- doc <- scene <- light, render <- tools <- ui <- app
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
# QT_DIR = the folder holding lib/cmake/Qt6 (02_BUILD_AND_TOOLING.md §1). build_native.bat finds it when the variable is unset.
if(DEFINED ENV{QT_DIR})
  list(PREPEND CMAKE_PREFIX_PATH "$ENV{QT_DIR}")
endif()
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network Test)
message(STATUS "Qt ${Qt6_VERSION} at ${Qt6_DIR}")

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
endfunction()

# ---- module libraries ---------------------------------------------------------------------------------------------------
find_package(Threads REQUIRED)
native_module(base   SOURCES src/base/module_info.cpp src/base/hash.cpp src/base/jobs.cpp src/base/log.cpp src/base/trace.cpp
                     DEPENDS vendored Threads::Threads)
native_module(doc    SOURCES src/doc/module_info.cpp    DEPENDS native_base core_lib)
native_module(scene  SOURCES src/scene/module_info.cpp  DEPENDS native_doc)
# volk (loader) and VMA (allocator) are third-party code: compiled without the project's warnings-as-errors.
add_library(native_vk STATIC third_party/volk/volk.c src/gpu/vma_impl.cpp)
target_include_directories(native_vk SYSTEM PUBLIC third_party/volk third_party/vma)
target_compile_definitions(native_vk PUBLIC VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR)
target_link_libraries(native_vk PUBLIC vendored)   # the Core's copy of the Vulkan headers (one copy, T0.2)
native_module(gpu    SOURCES src/gpu/module_info.cpp src/gpu/device.cpp DEPENDS native_base native_vk)
native_module(light  SOURCES src/light/module_info.cpp  DEPENDS native_scene native_gpu)
native_module(render SOURCES src/render/module_info.cpp DEPENDS native_scene native_gpu)
native_module(tools  SOURCES src/tools/module_info.cpp  DEPENDS native_render native_light)
native_module(ui     SOURCES src/ui/module_info.cpp src/ui/main_window.cpp src/ui/main_window.h src/ui/stats_panel.cpp src/ui/stats_panel.h
                     DEPENDS native_tools Qt6::Widgets)

# ---- the application ----------------------------------------------------------------------------------------------------
add_executable(mapeditor_native WIN32 src/app/main.cpp src/app/app_options.cpp src/app/app_options.h)
target_link_libraries(mapeditor_native PRIVATE native_ui Qt6::Widgets)
native_warnings(mapeditor_native)

# ---- tests (headless; everything below ui) ------------------------------------------------------------------------------
# Qt6::Test is not used for assertions (doctest is), but the offscreen platform lets the UI tests build real widgets.
add_executable(native_tests tests/test_main.cpp tests/test_modules.cpp tests/test_app_options.cpp tests/test_base.cpp tests/test_gpu_device.cpp src/app/app_options.cpp)
target_include_directories(native_tests PRIVATE src)
target_link_libraries(native_tests PRIVATE native_ui Qt6::Widgets Qt6::Test)
native_warnings(native_tests)

enable_testing()
add_test(NAME native_tests COMMAND native_tests)

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
    # The UI tests run on Qt's offscreen platform, which windeployqt does not deploy.
    add_custom_command(TARGET native_tests POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_qt_bin}/../plugins/platforms/qoffscreen.dll"
              "${CMAKE_BINARY_DIR}/platforms/qoffscreen.dll"
      COMMENT "qoffscreen.dll for the tests")
  else()
    message(WARNING "windeployqt not found: run the exe with Qt's bin folder on PATH")
  endif()
endif()
