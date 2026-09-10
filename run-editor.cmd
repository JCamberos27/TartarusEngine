@echo off
setlocal
rem Launch the Tartarus Engine editor, always from the latest build of whatever is
rem checked out. The desktop "Tartarus Engine" shortcut points here so a double-click
rem can never run a stale exe again.
cd /d "%~dp0"

echo Closing any running Tartarus Engine instances...
taskkill /F /IM TartarusEngine.exe >nul 2>&1

echo Building the latest (Release)...
cmake --build build --config Release
if errorlevel 1 (
  echo.
  echo *** BUILD FAILED - the editor below is the PREVIOUS build. Fix the error and rerun. ***
  echo.
  pause
)

rem The engine resolves its shipped assets from the exe's own location (EnginePaths, audit
rem #355) and walks up for project/, so the working directory no longer matters — launch it
rem in place.
start "" "build\Release\TartarusEngine.exe"
