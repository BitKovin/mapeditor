@echo off
rem ============================================================================
rem  Installs dependencies, type-checks, tests and builds the frontend (Shell).
rem  Usage: build_shell.bat [notests]
rem  Needs: Node.js 20+ (https://nodejs.org)
rem  Output: shell\dist\   (the dev server, run.bat, does not need this build)
rem ============================================================================
setlocal
set "ROOT=%~dp0"
where node >nul 2>nul
if errorlevel 1 goto no_node

pushd "%ROOT%shell"
if not exist node_modules (
  echo === Installing dependencies ===
  call npm install
  if errorlevel 1 goto fail
)
echo === Type-checking ===
call npm run typecheck
if errorlevel 1 goto fail
if /i "%~1"=="notests" goto build
echo === Unit tests ===
call npm test
if errorlevel 1 goto fail
:build
echo === Building ===
call npm run build
if errorlevel 1 goto fail
popd
echo.
echo Shell built OK: %ROOT%shell\dist
exit /b 0

:no_node
echo ERROR: Node.js not found. Install Node 20+ from https://nodejs.org
exit /b 1
:fail
popd
echo.
echo BUILD FAILED
exit /b 1
