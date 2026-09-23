@echo off
setlocal
set "WT=%CD%\build\_deps"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if "%~1"=="" ( echo usage: build_probe ^<name^>  ^(builds work\%~1.cpp^) & exit /b 2 )
cl /nologo /EHsc /std:c++17 /MD /O2 ^
  /I"%WT%\assimp-src\include" /I"%WT%\assimp-build\include" /I"%WT%\glm-src" ^
  work\%~1.cpp /Fo:work\%~1.obj /Fe:work\%~1.exe ^
  /link /LIBPATH:"%WT%\assimp-build\lib\Release" /LIBPATH:"%WT%\assimp-build\contrib\zlib\Release" ^
  assimp-vc145-mt.lib zlibstatic.lib
