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
  echo *** BUILD FAILED - fix the error above and rerun. ***
  echo The previous build does NOT contain your latest changes.
  echo.
  choice /C YN /N /M "Launch the previous build anyway? [Y/N] "
  if errorlevel 2 exit /b 1
)

rem The engine resolves its shipped assets from the exe's own location (EnginePaths, audit
rem #355) and walks up for project/, so the working directory no longer matters — launch it
rem in place.
start "" "build\Release\TartarusEngine.exe"
