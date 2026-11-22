@echo off
rem ============================================================================
rem  Packages the native editor into release\native\ (M13, native_rewrite\08_MILESTONES.md): the exe, the Qt DLLs and plugins windeployqt put next to it
rem  (the build does that after every Release build), the Qt Advanced Docking DLL, the NVIDIA runtime DLLs (Streamline, DLSS, Reflex) and the default keymap.
rem  Usage:  package_native.bat            builds Release (build_native.bat) and then copies
rem          package_native.bat nobuild    copies what the last Release build left in native\build
rem          set NATIVE_BUILD_DIR to package another build folder.
rem  Needs the Microsoft Visual C++ 2015-2022 runtime on the target machine (the build does not ship it). Tests and their offscreen plugin are left out.
rem  Not touched: release\LevelEditor (the Electron app of build_app.bat).
rem ============================================================================
setlocal enabledelayedexpansion
set "ROOT=%~dp0"
set "BUILD=%NATIVE_BUILD_DIR%"
if "%BUILD%"=="" set "BUILD=%ROOT%native\build"
set "OUT=%ROOT%release\native"

if /i "%~1"=="nobuild" goto have_build
call "%ROOT%build_native.bat" Release
if errorlevel 1 goto fail
:have_build

if not exist "%BUILD%\mapeditor_native.exe" goto no_exe
findstr /c:"CMAKE_BUILD_TYPE:STRING=Release" "%BUILD%\CMakeCache.txt" >nul 2>nul
if errorlevel 1 goto not_release

echo === Packaging into %OUT% ===
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%"
if errorlevel 1 goto fail

copy /y "%BUILD%\mapeditor_native.exe" "%OUT%\" >nul
if errorlevel 1 goto fail
rem every DLL next to the exe: Qt, qtadvanceddocking, Streamline (sl.*), DLSS (nvngx_*), Reflex, dxcompiler / dxil
copy /y "%BUILD%\*.dll" "%OUT%\" >nul
if errorlevel 1 goto fail
rem the Qt plugin folders windeployqt made (the offscreen platform is the tests' own)
for %%D in (platforms styles imageformats iconengines generic tls networkinformation keymaps) do (
  if exist "%BUILD%\%%D" robocopy "%BUILD%\%%D" "%OUT%\%%D" /E /NFL /NDL /NJH /NJS /NP >nul
  if errorlevel 8 goto fail
)
if exist "%OUT%\platforms\qoffscreen.dll" del /q "%OUT%\platforms\qoffscreen.dll"
rem the Streamline manifest / plugin files, when the build placed any next to the exe
for %%F in ("%BUILD%\*.json" "%BUILD%\*.txt") do (
  echo %%~nxF | findstr /i /c:"sl." /c:"streamline" >nul && copy /y "%%F" "%OUT%\" >nul
)

echo.
echo Native editor packaged: %OUT%\mapeditor_native.exe
echo Start it with:  "%OUT%\mapeditor_native.exe" --assets ^<game folder^> --map ^<map.json^>
exit /b 0

:no_exe
echo ERROR: %BUILD%\mapeditor_native.exe not found. Run build_native.bat Release first, or run this script without "nobuild".
exit /b 1
:not_release
echo ERROR: %BUILD% is not a Release build (windeployqt only runs for Release). Run build_native.bat Release.
exit /b 1
:fail
echo.
echo PACKAGING FAILED
exit /b 1
