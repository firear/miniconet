@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
cd /d c:\Users\firear\projs\miniconet
set CB=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin
echo ===== configure (IOCP ON) =====
"%CB%\cmake.exe" -S . -B build -G Ninja --fresh -DMINICONET_WITH_IOCP=ON
if %ERRORLEVEL% NEQ 0 echo CONFIGURE FAILED
echo ===== build =====
"%CB%\cmake.exe" --build build
if %ERRORLEVEL% NEQ 0 echo BUILD FAILED
echo ===== iocp_async_smoke =====
build\iocp_async_smoke.exe
echo ===== full ctest =====
"%CB%\ctest.exe" --test-dir build --timeout 60 --output-on-failure
