# tclog unit tests, registered with CTest by the parent tclog/CMakeLists.txt.
set(TCLOG_TEST_SOURCES
    test_thrqueue.c
    test_debug.c
)

foreach(tclog_test_src ${TCLOG_TEST_SOURCES})
    get_filename_component(tclog_test_name ${tclog_test_src} NAME_WE)
    add_executable(${tclog_test_name} ${tclog_test_src})
    target_link_libraries(${tclog_test_name} PRIVATE tclog)
    target_compile_options(${tclog_test_name} PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>
    )
    if(TCLOG_ENABLE_SANITIZERS)
        target_compile_options(${tclog_test_name} PRIVATE -fsanitize=address,undefined -g)
        target_link_options(${tclog_test_name} PRIVATE -fsanitize=address,undefined)
    endif()
    add_test(NAME ${tclog_test_name} COMMAND ${tclog_test_name})
    set_tests_properties(${tclog_test_name} PROPERTIES
        TIMEOUT 30
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )

    # 再注册一遍同一个测试，但强制走自带条件变量实现 —— 也就是 XP 上唯一的路径。
    # 开发机上没有 XP，这是让那条分支每次都被真正执行到的唯一办法
    # （否则它又是一段"从没跑过"的代码）。见 compat/tclog_win32.h。
    if(WIN32 OR MINGW)
        add_test(NAME ${tclog_test_name}_emu COMMAND ${tclog_test_name})
        set_tests_properties(${tclog_test_name}_emu PROPERTIES
            TIMEOUT 30
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            ENVIRONMENT "TCLOG_CV_EMU=1"
        )
    endif()
endforeach()
