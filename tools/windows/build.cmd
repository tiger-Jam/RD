@echo off
rem Build RD with Visual Studio 2022 (called from tools/win.sh)
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0\..\.."
if not exist build\RD.sln cmake -S . -B build -G "Visual Studio 17 2022" -A x64 > build_config.log 2>&1
rem Dev only: copy vcpkg FFmpeg DLLs next to the exe after the build (not for release)
set FFV=%USERPROFILE%\vcpkg\installed\x64-windows
cmake --build build --config RelWithDebInfo -- /m /v:minimal > build.log 2>&1
if errorlevel 1 (findstr /C:"error" build.log & exit /b 1)
if exist "%FFV%\bin\avcodec-*.dll" for %%f in ("%FFV%\bin\av*.dll" "%FFV%\bin\sw*.dll") do copy /y "%%f" build\RelWithDebInfo\ >nul
echo BUILD_OK
