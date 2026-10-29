@echo off
rem Release build: dev hooks off (BMJ_DEV=OFF), version %1. Called from tools/windows/release.ps1
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0\..\.."
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBMJ_DEV=OFF -DBMJ_VERSION=%1 > build_config.log 2>&1
if errorlevel 1 (type build_config.log & exit /b 1)
cmake --build build --config RelWithDebInfo -- /m /v:minimal > build.log 2>&1
if errorlevel 1 (findstr /C:"error" build.log & exit /b 1)
echo BUILD_OK
