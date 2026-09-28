@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\vcvarsall.bat" amd64
cd /d c:\Users\firear\projs\miniconet
set CMAKE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
if "%1"=="cfg" (
    "%CMAKE%" -S . -B build-vs2010 -G Ninja -DCMAKE_MAKE_PROGRAM=c:\bin\ninja.exe -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMINICONET_WITH_IOCP=ON
    exit /b %ERRORLEVEL%
)
if "%1"=="clean" (
    rmdir /s /q build-vs2010
    exit /b 0
)
"%CMAKE%" --build build-vs2010 > build-vs2010.log 2>&1
set RC=%ERRORLEVEL%
type build-vs2010.log
if %RC% NEQ 0 echo ===== BUILD FAILED rc=%RC% =====
exit /b %RC%
