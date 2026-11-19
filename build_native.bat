@echo off
rem ============================================================================
rem  Builds the native editor (native\, Qt 6 + Vulkan + the Core as a library) with CMake + Ninja and the Visual Studio toolchain.
rem  Usage:  build_native.bat [Release|Debug]            (default: Release)
rem          set NATIVE_BUILD_DIR to build somewhere else than native\build
rem  Needs:  Visual Studio 2022 (or Build Tools) with "Desktop development with C++", the Vulkan SDK (VULKAN_SDK, for
rem          glslangValidator) and Qt 6.8+ (MSVC 2022 64-bit). QT_DIR = the folder holding lib\cmake\Qt6; when it is unset the
rem          newest D:\Qt\*\msvc2022_64 or C:\Qt\*\msvc2022_64 is used.
rem  Output: native\build\mapeditor_native.exe and native\build\native_tests.exe. This script does not run the tests.
rem  See native_rewrite\02_BUILD_AND_TOOLING.md.
rem ============================================================================
setlocal enabledelayedexpansion
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
set "ROOT=%~dp0"
set "BUILD=%NATIVE_BUILD_DIR%"
if "%BUILD%"=="" set "BUILD=%ROOT%native\build"

rem ---- Qt ----
if defined QT_DIR goto have_qt
for %%D in (D:\Qt C:\Qt) do (
  if exist "%%D" (
    for /f "delims=" %%Q in ('dir /b /ad /o-n "%%D\6.*" 2^>nul') do (
      if not defined QT_DIR if exist "%%D\%%Q\msvc2022_64\lib\cmake\Qt6" set "QT_DIR=%%D\%%Q\msvc2022_64"
    )
  )
)
:have_qt
if not defined QT_DIR goto no_qt
if not exist "%QT_DIR%\lib\cmake\Qt6" goto no_qt
echo Using Qt at: %QT_DIR%

rem ---- Vulkan SDK ----
if not defined VULKAN_SDK goto no_vulkan
if not exist "%VULKAN_SDK%\Bin\glslangValidator.exe" goto no_vulkan

rem ---- Visual Studio ----
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
cmake -S "%ROOT%native" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG%
if errorlevel 1 goto fail
echo === Building ===
cmake --build "%BUILD%" --target mapeditor_native native_tests
if errorlevel 1 goto fail
echo.
echo Native editor built OK: %BUILD%\mapeditor_native.exe
exit /b 0

:no_qt
echo ERROR: Qt 6.8+ (MSVC 2022 64-bit) not found. Set QT_DIR to the folder that holds lib\cmake\Qt6, e.g. D:\Qt\6.11.1\msvc2022_64
exit /b 1
:no_vulkan
echo ERROR: Vulkan SDK not found. Install it from https://vulkan.lunarg.com and set VULKAN_SDK (glslangValidator is needed).
exit /b 1
:no_vs
echo ERROR: Visual Studio C++ tools not found. Install "Desktop development with C++" (Visual Studio or Build Tools),
echo        or run this from an "x64 Native Tools Command Prompt".
exit /b 1
:no_ninja
echo ERROR: ninja not found on PATH. The Visual Studio CMake component includes it; otherwise install from https://ninja-build.org
exit /b 1
:no_cmake
echo ERROR: cmake not found on PATH. The Visual Studio CMake component includes it.
exit /b 1
:fail
echo.
echo BUILD FAILED
exit /b 1
