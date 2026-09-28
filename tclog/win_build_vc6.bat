@echo off
rem ============================================================================
rem  tclog — VC6 (Visual C++ 6.0) 构建脚本
rem
rem  用法：
rem    win_build_vc6.bat          构建（自动 configure + 编译）
rem    win_build_vc6.bat cfg      只重新 configure，不编译
rem    win_build_vc6.bat clean    删除构建目录
rem
rem  生成器：优先 Ninja（C:\bin\ninja.exe）。Ninja 的 configure 或 build 失败则
rem          自动回退到 "NMake Makefiles"；两个都不行才报错退出。
rem
rem  为什么 Ninja 要加 --parallel 1：
rem    VC6 的 cl.exe **不能多进程共用一个 /Fd PDB**（VS2010+ 有 /FS 才行）。
rem    Ninja 默认并行，于是第二个并发的 cl 立刻报
rem      fatal error C1033: cannot open program database '...\tclog.pdb'
rem    串行（-j1）就没这个问题 —— 实测 10/10 全过。NMake 本来就是串行。
rem
rem  为什么不用 VS 工程生成器：
rem    CMake 从 3.6 起移除了 "Visual Studio 6" 生成器（3.3 起弃用），所以 VC6
rem    只能走 Ninja / NMake 这类命令行生成器，生成不了 .dsw / .dsp。
rem
rem  两个 VC6 特有的坑：
rem    1) VC98\Bin\VCVARS32.BAT 会自己改当前目录 —— call 之后必须自己 cd 回来，
rem       否则后面所有相对路径都跑到 VC6 安装目录去了；
rem    2) VC6 只有 32 位编译器，产物是 x86，和 win_build_vs2010.bat 的 amd64
rem       不是同一份。
rem ============================================================================
setlocal
set SRC=%~dp0
rem %~dp0 结尾带反斜杠；直接放进引号里会变成 "...\tclog\" —— 反斜杠把结束引号转义掉，
rem CMake 会把后面的参数全当成同一个路径。必须去掉。
if "%SRC:~-1%"=="\" set SRC=%SRC:~0,-1%
set VC6=C:\Users\firear\projs\vc6ultimate\VC98
set CMAKEDIR=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin
set CMAKE=%CMAKEDIR%\cmake.exe
set CTEST=%CMAKEDIR%\ctest.exe
set NINJA=C:\bin\ninja.exe
set BUILD=build-vc6
set LOG=build-vc6.log

if "%1"=="clean" (
    rmdir /s /q "%BUILD%" 2>nul
    echo [info] 已删除 %BUILD%
    exit /b 0
)

if not exist "%VC6%\Bin\VCVARS32.BAT" (
    echo [error] 找不到 VC6：%VC6%\Bin\VCVARS32.BAT
    echo         请改脚本里的 VC6 变量。
    exit /b 1
)
call "%VC6%\Bin\VCVARS32.BAT" >nul
cd /d "%SRC%"

rem 换生成器必须换干净目录（CMake 缓存里记着生成器），所以先删。
rmdir /s /q "%BUILD%" 2>nul
if not exist "%NINJA%" goto :try_nmake

rem --------------------------------------------------------------- 1) Ninja
echo [info] 生成器：Ninja（%NINJA%，--parallel 1）...
set GEN=Ninja
"%CMAKE%" -S "%SRC%" -B "%BUILD%" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=RelWithDebInfo > "%LOG%" 2>&1
if errorlevel 1 goto :ninja_failed
if "%1"=="cfg" goto :cfg_done

"%CMAKE%" --build "%BUILD%" --parallel 1 >> "%LOG%" 2>&1
if errorlevel 1 goto :ninja_failed
goto :report

:ninja_failed
echo [warn] Ninja 不行，回退 NMake Makefiles。Ninja 的失败原因如下：
echo ----------------------------------------------------------------------
findstr /V /C:"C4819" "%LOG%"
echo ----------------------------------------------------------------------
echo.
rmdir /s /q "%BUILD%" 2>nul

rem --------------------------------------------------------------- 2) NMake
:try_nmake
if not exist "%NINJA%" echo [info] 没找到 ninja（%NINJA%），直接用 NMake Makefiles。
echo [info] 生成器：NMake Makefiles...
set GEN=NMake Makefiles
"%CMAKE%" -S "%SRC%" -B "%BUILD%" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo > "%LOG%" 2>&1
if errorlevel 1 goto :failed
if "%1"=="cfg" goto :cfg_done

"%CMAKE%" --build "%BUILD%" >> "%LOG%" 2>&1
if errorlevel 1 goto :failed

:report
echo.
echo ===== BUILD OK  (generator = %GEN%, target = x86) =====
echo 产物：
dir /b "%BUILD%\*.lib" 2>nul
dir /b /s "%BUILD%\tests\*.exe" 2>nul
dir /b /s "%BUILD%\bench\*.exe" 2>nul
echo.
echo 编译输出（完整日志见 %LOG%）：
findstr /V /C:"C4819" "%LOG%"
echo.
echo 跑测试：  "%CTEST%" --test-dir "%BUILD%" --output-on-failure
exit /b 0

:cfg_done
echo.
echo ===== CONFIGURE OK  (generator = %GEN%) =====
findstr /V /C:"C4819" "%LOG%"
exit /b 0

:failed
echo.
echo ===== BUILD FAILED =====
findstr /V /C:"C4819" "%LOG%"
exit /b 1
