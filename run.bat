@echo off
rem ============================================================================
rem  Starts the Core (backend) and the Shell dev server (frontend), then opens the editor.
rem  Builds whatever is missing first. Close the two console windows to stop.
rem  Usage: run.bat [assetsFolder] [initialMap]
rem  With no arguments the Core uses the assets folder saved from the editor's Settings (the gear button;
rem  stored in settings.json next to mapeditor_core.exe), else core\assets, and reopens the last map.
rem ============================================================================
setlocal
set "ROOT=%~dp0"
set "ASSETS=%~1"
set "MAP=%~2"
set "CORE_ARGS=8787"
if not "%ASSETS%"=="" set CORE_ARGS=%CORE_ARGS% "%ASSETS%"
if not "%ASSETS%"=="" if not "%MAP%"=="" set CORE_ARGS=%CORE_ARGS% "%MAP%"

if not exist "%ROOT%core\build\mapeditor_core.exe" (
  echo Core is not built yet - building it first...
  call "%ROOT%build_core.bat"
  if errorlevel 1 exit /b 1
)
where node >nul 2>nul
if errorlevel 1 (
  echo ERROR: Node.js not found. Install Node 20+ from https://nodejs.org
  exit /b 1
)
if not exist "%ROOT%shell\node_modules" (
  echo Installing frontend dependencies...
  pushd "%ROOT%shell" & call npm install & popd
)

echo Starting Core on ws://127.0.0.1:8787 ...
start "Level Editor - Core" /D "%ROOT%core" cmd /k build\mapeditor_core.exe %CORE_ARGS%
echo Starting Shell on http://localhost:5173 ...
start "Level Editor - Shell" /D "%ROOT%shell" cmd /k npm run dev -- --port 5173 --open
exit /b 0
