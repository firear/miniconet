# 微基准：不注册进 CTest（耗时、输出非确定性），手工跑。
add_executable(bench_debug bench_debug.c)
target_link_libraries(bench_debug PRIVATE tclog)
target_compile_options(bench_debug PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>
)
