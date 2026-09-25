@echo off
setlocal
rem Launch the Tartarus Engine editor, always from the latest build of whatever is
rem checked out. The desktop "Tartarus Engine" shortcut points here so a double-click
rem can never run a stale exe again.
cd /d "%~dp0"

rem Windows Terminal (the default console on Windows 11) ignores the launch screen's window
rem sizing, font and centring, so reopen in the classic console window. The desktop shortcut
rem starts conhost with "classic" itself, which skips the hop.
if /i not "%~1"=="classic" (
  start "" conhost.exe cmd /c ""%~f0" classic"
  exit /b 0
)

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

rem The launch screen (tools\launcher) plays while the Release build runs behind it, and
rem exits with the build's result. Output goes to build\last-build.log.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\launcher\launch-screen.ps1" -Build
if errorlevel 1 (
  echo.
  echo *** BUILD FAILED - last lines of build\last-build.log: ***
  powershell -NoProfile -Command "Get-Content build\last-build.log -Tail 25"
  echo.
  echo The previous build does NOT contain your latest changes.
  echo.
  choice /C YN /N /M "Launch the previous build anyway? [Y/N] "
  if errorlevel 2 exit /b 1
)

rem The engine resolves its shipped assets from the exe's own location (EnginePaths, audit
rem #355) and walks up for project/, so the working directory no longer matters - launch it
rem in place.
start "" "build\Release\TartarusEngine.exe"
timeout /t 3 >nul
