@echo off
rem ============================================================================
rem  Builds the whole editor into ONE folder ready to publish: release\LevelEditor\
rem  (the Electron app: LevelEditor.exe + the Core, its tools and DLLs, the default assets, the Shell).
rem  Copy or zip that folder; the target PC needs nothing installed (Windows 10/11 x64, a GPU driver).
rem  Usage:  build_app.bat [tests]        (tests also runs the Core, Shell and desktop unit tests; the default skips them)
rem  Needs:  what build_core.bat needs (Visual Studio C++ workload) and Node.js 20+; internet the first time
rem          (npm packages and the Electron runtime are downloaded, then cached).
rem  The Core is built in core\build_release, so a Core running from core\build (run.bat) does not block it.
rem ============================================================================
setlocal
set "ROOT=%~dp0"
set "NOTESTS=notests"
if /i "%~1"=="tests" set "NOTESTS="
where node >nul 2>nul
if errorlevel 1 goto no_node

echo ##### 1/3 Core (Release) #####
set "CORE_BUILD_DIR=%ROOT%core\build_release"
call "%ROOT%build_core.bat" Release %NOTESTS%
if errorlevel 1 goto fail

echo ##### 2/3 Shell #####
call "%ROOT%build_shell.bat" %NOTESTS%
if errorlevel 1 goto fail

echo ##### 3/3 Desktop app #####
pushd "%ROOT%desktop"
if exist node_modules\@electron\packager goto have_modules
echo === Installing dependencies ===
call npm install
if errorlevel 1 goto fail_popd
:have_modules
if "%NOTESTS%"=="notests" goto package
echo === Unit tests ===
call npm test
if errorlevel 1 goto fail_popd
:package
call node package.mjs "%CORE_BUILD_DIR%"
if errorlevel 1 goto fail_popd
popd
echo.
echo Ready to publish: %ROOT%release\LevelEditor\   (run LevelEditor.exe)
exit /b 0

:no_node
echo ERROR: Node.js not found. Install Node 20+ from https://nodejs.org
exit /b 1
:fail_popd
popd
:fail
echo.
echo BUILD FAILED
exit /b 1
