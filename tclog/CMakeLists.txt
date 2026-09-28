# Standalone CMake build for the tclog submodule.
#
# Builds libtclog (debug.c + thrqueue_cs.c) as a self-contained static library
# and registers its unit tests with CTest. This file is independent of the
#   mkdir build && cd build && cmake .. && cmake --build . && ctest --output-on-failure
#

cmake_minimum_required(VERSION 3.10)
project(tclog C)

set(CMAKE_C_STANDARD 90)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)   # gnu89: long long, __attribute__, // comments

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()


if(WIN32 OR MINGW)
    if(MSVC_VERSION GREATER_EQUAL 1700)
        add_compile_options(/utf-8)
    endif()
    add_definitions(-DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -D_WIN32_WINNT=0x0600 -DHV_STATICLIB)
    if(MSVC AND MSVC_VERSION LESS 1900)
        add_compile_options(/Dinline=__inline)
    endif()
endif()

add_library(tclog STATIC
    debug.c
    thrqueue_cs.c
)
target_include_directories(tclog
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
# Windows 条件变量后端（原生 / 自带实现）在运行期选，见 compat/tclog_win32.h。
# 因为它必须动态解析 kernel32 的符号，独立一个 .c 承载后端状态。
# 测试/bench 可用环境变量 TCLOG_CV_EMU=1 强制走自带实现（覆盖 XP 那条路径）。
if(WIN32 OR MINGW)
    target_sources(tclog PRIVATE compat/tclog_win32.c)
endif()
target_compile_definitions(tclog PRIVATE _GNU_SOURCE)
target_compile_options(tclog PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>
)

find_package(Threads REQUIRED)
target_link_libraries(tclog PUBLIC Threads::Threads)

# 分阶段插桩：PUBLIC 传播，让 bench 也看到同一宏。默认 OFF —— 关掉时
# tclog_perf.h 展开为空，库里不产生任何插桩代码。
option(TCLOG_PERF "Dev: phase instrumentation for the log hot path" OFF)
if(TCLOG_PERF)
    target_compile_definitions(tclog PUBLIC TCLOG_PERF)
endif()

option(TCLOG_ENABLE_SANITIZERS "Dev: ASan+UBSan for tclog and its tests" OFF)
if(TCLOG_ENABLE_SANITIZERS)
    target_compile_options(tclog PUBLIC -fsanitize=address,undefined -g)
    target_link_options(tclog PUBLIC -fsanitize=address,undefined)
endif()

# ---- Bench (not registered with CTest) ----
# 可选开关：换成老编译器（例如 VC6）时，bench 用到的 64 位整型/字面量后缀不保证都
# 支持，可以 -DTCLOG_BUILD_BENCH=OFF 只编库。
option(TCLOG_BUILD_BENCH "Build the micro-benchmark" ON)
if(TCLOG_BUILD_BENCH)
    add_subdirectory(bench)
endif()

# ---- Tests (CTest) ----
option(TCLOG_BUILD_TESTS "Build and register the unit tests" ON)
if(TCLOG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
