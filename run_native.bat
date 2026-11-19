@echo off
rem Starts the native editor.  Usage: run_native.bat [assetsRoot] [map]   (map is relative to the assets root)
rem Any further options go straight to mapeditor_native.exe (see native_rewrite\02_BUILD_AND_TOOLING.md section 6).
setlocal
set "ROOT=%~dp0"
set "BUILD=%NATIVE_BUILD_DIR%"
if "%BUILD%"=="" set "BUILD=%ROOT%native\build"
if not exist "%BUILD%\mapeditor_native.exe" (
  echo mapeditor_native.exe not found in %BUILD% - run build_native.bat first.
  exit /b 1
)
set "ARGS="
if not "%~1"=="" set "ARGS=--assets "%~1""
if not "%~2"=="" set "ARGS=%ARGS% --map "%~2""
shift
shift
"%BUILD%\mapeditor_native.exe" %ARGS% %1 %2 %3 %4 %5 %6 %7 %8 %9
