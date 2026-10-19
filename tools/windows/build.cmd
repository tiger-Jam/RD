@echo off
rem Build BMjoy with Visual Studio 2022 (called from tools/win.sh)
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0\..\.."
if not exist build\BMjoy.sln cmake -S . -B build -G "Visual Studio 17 2022" -A x64 > build_config.log 2>&1
rem Dev only: if vcpkg FFmpeg exists, build against its headers and copy its DLLs next to the exe (not for release)
set FFV=%USERPROFILE%\vcpkg\installed\x64-windows
if exist "%FFV%\include\libavcodec\avcodec.h" cmake -S . -B build -DBMJ_FFMPEG_INCLUDE="%FFV%\include" >> build_config.log 2>&1
cmake --build build --config RelWithDebInfo -- /m /v:minimal > build.log 2>&1
if errorlevel 1 (findstr /C:"error" build.log & exit /b 1)
if exist "%FFV%\bin\avcodec-*.dll" for %%f in ("%FFV%\bin\av*.dll" "%FFV%\bin\sw*.dll") do copy /y "%%f" build\RelWithDebInfo\ >nul
echo BUILD_OK
