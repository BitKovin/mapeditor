@echo off
rem ============================================================================
rem  Builds the Core (C++20) with CMake + Ninja using the Visual Studio toolchain.
rem  Usage:  build_core.bat [Release|Debug]        (default: Release)
rem  Needs:  Visual Studio 2019/2022 (or Build Tools) with the "Desktop development
rem          with C++" workload. It ships CMake and Ninja; nothing else to install.
rem  Output: core\build\mapeditor_core.exe, core\build\core_tests.exe and core\build\lightbake_tool.exe
rem  Shaders (core\shaders) are compiled as part of this build when glslangValidator is found (PATH, %VULKAN_SDK%\Bin or
rem  C:\VulkanSDK\*\Bin; the LunarG Vulkan SDK provides it). Without it the checked-in SPIR-V is used, and the build stops
rem  with a clear message if a shader was edited and the checked-in copy is stale. See the "Shaders:" line of the configure step.
rem ============================================================================
setlocal
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
set "ROOT=%~dp0"

where cl >nul 2>nul
if not errorlevel 1 goto have_cl

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_vs
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto no_vs
echo Using Visual Studio at: %VSPATH%
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto fail

:have_cl
where ninja >nul 2>nul
if errorlevel 1 goto no_ninja
where cmake >nul 2>nul
if errorlevel 1 goto no_cmake

echo === Configuring (%CONFIG%) ===
cmake -S "%ROOT%core" -B "%ROOT%core\build" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG%
if errorlevel 1 goto fail
echo === Building ===
cmake --build "%ROOT%core\build"
if errorlevel 1 goto fail
echo === Running unit tests ===
"%ROOT%core\build\core_tests.exe"
if errorlevel 1 goto fail
echo.
echo Core built OK: %ROOT%core\build\mapeditor_core.exe
exit /b 0

:no_vs
echo ERROR: Visual Studio C++ tools not found. Install "Desktop development with C++" (Visual Studio or Build Tools),
echo        or run this from an "x64 Native Tools Command Prompt".
exit /b 1
:no_ninja
echo ERROR: ninja not found on PATH. The Visual Studio CMake component includes it; otherwise install from https://ninja-build.org
exit /b 1
:no_cmake
echo ERROR: cmake not found on PATH. Install the Visual Studio CMake component or https://cmake.org
exit /b 1
:fail
echo.
echo BUILD FAILED
exit /b 1
