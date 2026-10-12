@echo off
setlocal
rem Launch the Tartarus Engine editor, always from the latest build of whatever is
rem checked out. The desktop "Tartarus Engine" shortcut points here so a double-click
rem can never run a stale exe again. The build and the editor's load log show in this
rem console, which closes once the editor window is up.
cd /d "%~dp0"
title Tartarus Engine

echo Closing any running Tartarus Engine instances...
taskkill /F /IM TartarusEngine.exe >nul 2>&1

rem A fresh clone has no build folder yet; configure it once (the first build is slow).
if not exist "build\CMakeCache.txt" (
  echo First run: configuring the build - this can take a couple of minutes...
  cmake -S . -B build -A x64
  if errorlevel 1 (
    echo.
    echo *** CMAKE CONFIGURE FAILED - see the error above. ***
    pause
    exit /b 1
  )
)

echo.
echo Building (Release)...
cmake --build build --config Release --parallel
if errorlevel 1 (
  echo.
  echo *** BUILD FAILED - see the errors above. ***
  echo The previous build does NOT contain your latest changes.
  echo.
  choice /C YN /N /M "Launch the previous build anyway? [Y/N] "
  if errorlevel 2 exit /b 1
)

if not exist "build\Release\TartarusEngine.exe" (
  echo.
  echo *** No build\Release\TartarusEngine.exe to launch. ***
  pause
  exit /b 1
)

rem The engine resolves its shipped assets from the exe's own location (EnginePaths, audit
rem #355) and walks up for project/, so the working directory doesn't matter. The Release
rem editor has no console of its own: follow its log file here while it loads.
echo.
echo Starting the editor...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\follow-editor-log.ps1" -Exe "%~dp0build\Release\TartarusEngine.exe"
if errorlevel 1 pause
