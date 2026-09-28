cmake_minimum_required(VERSION 3.10)
project(miniconet2 C)

set(CMAKE_C_STANDARD 90)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)  # gnu89: strict c90 rejects POSIX headers (pipe2/O_NONBLOCK)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# ---------------------------------------------------------------------------
# Build switches; every optional feature defaults OFF except BUILD_EXAMPLES.
# ---------------------------------------------------------------------------
option(MINICONET_BUILD_EXAMPLES    "构建 test/ 下的冒烟测试与 examples/ 下的示例"                   ON)
option(MINICONET_ENABLE_SANITIZERS "开发用：给 core 库和 test 加上 ASan+UBSan"                       OFF)
option(MINICONET_BUILD_TEST_HOOKS  "编译几个仅供 test/ 测试用的内部访问器；不是稳定公开 API"          ${MINICONET_BUILD_EXAMPLES})

option(MINICONET_WITH_OPENSSL "TLS 后端：OpenSSL(需要 libssl-dev)"                    OFF)
option(MINICONET_WITH_MBEDTLS "TLS 后端：mbedTLS(需要 libmbedtls-dev)"                OFF)
option(MINICONET_WITH_GNUTLS  "TLS 后端：GnuTLS(需要 libgnutls28-dev)"                OFF)
# The TLS backend options are mutually exclusive; see cmake/SSLBackends.cmake.
# With none enabled (non-Windows), mco_ssl_ctx_new_*() returns NULL at runtime
# (the nossl.c stub), never silently plaintext.

option(MINICONET_ENABLE_UDS "支持 Unix Domain Socket (mco_tcp_listen_unix/connect_unix)" OFF)

option(MINICONET_WITH_IO_URING
    "Linux: io_uring 引擎后端（裸 syscall，不依赖 liburing）。开启后成为 Linux 默认后端，\
运行期探测失败（内核过老/seccomp 禁止）自动回退 epoll；就绪模拟层始终可用，\
完成式快路径（read/write/connect/accept 直提交 SQE）按内核能力逐项探测" OFF)

option(MINICONET_WITH_IOCP
    "Windows: IOCP 完成式引擎后端（WSARecv/WSASend/ConnectEx/AcceptEx 直提交到\
完成端口）。开启后成为 Windows 默认后端；就绪模拟层用 WSAPoll 实现，供 TLS/UDP 等\
就绪路径使用。未真机跑通，仅编译级验证（同 wepoll 后端标注）" OFF)

option(MINICONET_WITH_PROTOCOL_CLIENTS
    "vendor 一批阻塞式协议客户端(DNS/FTP/SMTP/ICMP，来自 chv-master 项目)。\
注意：这些是同步阻塞实现，不要在协程里直接调用——会卡住所在 vcpu 的其它协程，\
目前只是先编译进来占位，异步化留待后续按 core/mco_resolver.c 的模式重新包一层" OFF)
option(MINICONET_WITH_HTTP_PARSER
    "vendor HTTP/multipart/websocket 的纯协议编解码器 + 定义(picohttpparser/httpdef/wsdef 等，\
来自 chv-master)。只是解析器本身，不含 hio_t 耦合的 HTTP/WebSocket server/client 封装\
(那部分要按协程 API 重写，见 miniconet2_websocket 的进度)" OFF)
option(MINICONET_WITH_MQTT_PROTOCOL
    "vendor MQTT 协议编解码器(mqtt_protocol.c，来自 chv-master)。同上，不含耦合 hio_t 的 mqtt_client.c" OFF)
option(MINICONET_WITH_MINIUPNP
    "vendor 一个 XML 解析器(minixml.c，来自 chv-master miniupnp 模块)。UPnP 发现逻辑本身耦合 hio_t，还没 vendor" OFF)
option(MINICONET_WITH_KCP
    "vendor KCP 协议核心算法(ikcp.c，来自 chv-master event/kcp)。只是算法本体，不含耦合 hio_t 的 hkcp.c 封装，\
和 mco_udp_* 的协程胶水层还没写" OFF)
option(MINICONET_WITH_CRYPTO_UTILS
    "vendor MD5/SHA1(来自 chv-master util/)。WebSocket 握手算 Sec-WebSocket-Accept 需要 SHA1" OFF)
option(MINICONET_WITH_IDNA
    "vendor 国际化域名支持(idna.c，来自 chv-master util/)，给 mco_resolve() 用" OFF)
option(MINICONET_WITH_WRAPPER_UTILS
    "vendor 几个零散工具：动态字节数组(charry.c) + hmemdup，来自 chv-master wrapper/" OFF)
option(MINICONET_WITH_WORKPOOL
    "可选：多 vCPU workpool 薄层(N 个 OS 线程各跑一个 vcpu + 跨线程任务队列)。\
给默认单 vcpu 模型补真并行能力；默认 OFF，不需要多核并行时不编译。见 core/mco_workpool.h" OFF)
option(MINICONET_WITH_UPNP_CLIENT
    "可选：UPnP IGD 客户端(协程同步式：SSDP 发现 + 设备描述解析 + SOAP 端口映射控制)。\
依赖 miniconet2_http_client(用 mco_http_get_ex/mco_http_request) 和 miniconet2_miniupnp。\
默认 OFF；打开时隐含 MINICONET_WITH_HTTP_PARSER=ON 和 MINICONET_WITH_MINIUPNP=ON。\
见 miniconet_upnp.h" OFF)

if(WIN32 OR MINGW)
    if(MSVC_VERSION GREATER_EQUAL 1700)
        add_compile_options(/utf-8)
    endif()
    add_definitions(-DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -D_WIN32_WINNT=0x0600 -DHV_STATICLIB)
    if(MSVC AND MSVC_VERSION LESS 1900)
        add_compile_options(/Dinline=__inline)
    endif()
endif()


# ---------------------------------------------------------------------------
# Platform detection + config header generation (cmake/PlatformConfig.cmake).
# Both headers land in the build dir; their macros are always 0/1, so use #if.
#
# Directory properties must be set before add_library.
# ---------------------------------------------------------------------------
include(PlatformConfig)
miniconet_detect_platform()
include_directories(${MINICONET_GENERATED_DIR})
# Gate macro: defined -> include the generated config headers; undefined -> the
# in-tree fallbacks (external -I source-tree builds). Directory scoped.
add_compile_definitions(MINICONET_HAVE_GENERATED_CONFIG=1)

# The UPnP client implies http_parser and miniupnp; FORCE them before the targets
# are created, or the upnp target fails to link.
if(MINICONET_WITH_UPNP_CLIENT)
    set(MINICONET_WITH_HTTP_PARSER ON CACHE BOOL "forced on by UPNP_CLIENT" FORCE)
    set(MINICONET_WITH_MINIUPNP   ON CACHE BOOL "forced on by UPNP_CLIENT" FORCE)
endif()

if(MINICONET_WITH_HTTP_PARSER)
    set(MINICONET_WITH_WRAPPER_UTILS ON CACHE BOOL "forced on by HTTP_PARSER" FORCE)
endif()

# ---------------------------------------------------------------------------
# Pick engine sources per platform instead of adding every backend and letting
# each #if compile to an empty file: same artifact, but the active backend is
# obvious and dead files are never preprocessed.
# ---------------------------------------------------------------------------
set(MINICONET_CORE_SOURCES
    compat/hbase.c
    compat/hsocket.c
    compat/htime.c
    compat/hthread.c
    tclog/debug.c
    tclog/thrqueue_cs.c
    core/mco_engine_default.c
    core/mco_timer.c
    core/mco_vcpu.c
    core/mco_resolver.c
    core/mco_oneshot.c
    core/mco_waitgroup.c
    core/mco_wakefd.c
    miniconet.c
    compat/ssl/hssl.c
    compat/ssl/openssl.c
    compat/ssl/mbedtls.c
    compat/ssl/gnutls.c
    compat/ssl/nossl.c
    compat/ssl/appletls.c
    compat/ssl/wintls.c
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND MINICONET_CORE_SOURCES core/mco_engine_epoll.c)
    if(MINICONET_WITH_IO_URING)
        list(APPEND MINICONET_CORE_SOURCES core/mco_uring.c core/mco_engine_uring.c)
    endif()
elseif(APPLE OR CMAKE_SYSTEM_NAME MATCHES "BSD")
    list(APPEND MINICONET_CORE_SOURCES core/mco_engine_kqueue.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "SunOS")
    list(APPEND MINICONET_CORE_SOURCES core/mco_engine_evport.c)
elseif(WIN32)
    list(APPEND MINICONET_CORE_SOURCES core/mco_engine_wepoll.c compat/wepoll/wepoll.c)
    if(MINICONET_WITH_IOCP)
        list(APPEND MINICONET_CORE_SOURCES core/mco_engine_iocp.c)
    endif()
endif()
if(NOT WIN32)
    list(APPEND MINICONET_CORE_SOURCES core/mco_engine_select.c core/mco_engine_poll.c)
else()
    list(APPEND MINICONET_CORE_SOURCES core/mco_engine_select.c)  # poll backend is Windows-only
endif()

if(WIN32)
    list(APPEND MINICONET_CORE_SOURCES compat/portable/winapi.c)
endif()

if(MINICONET_WITH_WORKPOOL)
    list(APPEND MINICONET_CORE_SOURCES core/mco_workpool.c)
endif()

add_library(miniconet2 STATIC ${MINICONET_CORE_SOURCES})

# PUBLIC include dirs need BUILD/INSTALL generator expressions: a bare
# source-tree path is baked into miniconet2-targets.cmake by install(EXPORT), so
# consumers would include the all-zero fallbacks. INSTALL_INTERFACE:include
# points installed packages at <prefix>/include.
target_include_directories(miniconet2
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/compat>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/compat/ssl>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/compat/portable>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/minicoro>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tclog>
        # dir holding the generated config; BUILD_INTERFACE only, since it does
        # not exist after install.
        $<BUILD_INTERFACE:${MINICONET_GENERATED_DIR}>
        $<INSTALL_INTERFACE:include>
)
# The gate macro propagates via BUILD_INTERFACE only, so neither integration
# style sees the all-zero in-tree fallbacks.
target_compile_definitions(miniconet2
    PUBLIC $<BUILD_INTERFACE:MINICONET_HAVE_GENERATED_CONFIG=1>)
# All PUBLIC on purpose: some test/*.c include internal headers for white-box
# checks; that is deliberate, not a leak of public API.

target_compile_options(miniconet2 PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>
    # -Wno-unused-result       # unchecked asprintf() in vendor code
    # -Wno-format-truncation   # false "format may be NULL" in vendor debug.c
)

# Vendored code carries helpers used only by feature paths we do not enable
# (e.g. openssl.c's setopt, whose only call site is commented out upstream).
# Silence -Wall for those files only, so our own sources still report dead code.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    set_source_files_properties(compat/ssl/openssl.c
        PROPERTIES COMPILE_FLAGS "-Wno-unused-function")
endif()

if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(miniconet2 PUBLIC Threads::Threads)
endif()

if(WIN32)
    target_link_libraries(miniconet2 PUBLIC ws2_32)
endif()

# These now come from the generated miniconet_config.h (always 0/1).

# clock_gettime needs explicit librt on glibc < 2.17
if(MINICONET_NEED_LIBRT)
    target_link_libraries(miniconet2 PUBLIC rt)
endif()

if(MINICONET_ENABLE_SANITIZERS)
    target_compile_options(miniconet2 PUBLIC -fsanitize=address,undefined -g)
    target_link_options(miniconet2 PUBLIC -fsanitize=address,undefined)
endif()

# TLS backend selection: see cmake/SSLBackends.cmake
include(SSLBackends)
miniconet_configure_ssl_backend(miniconet2)

# Generate hconfig.h / miniconet_config.h after the SSL backend is settled:
# MINICONET_HAVE_SSL must be the post-downgrade result.
miniconet_generate_config()

# ---------------------------------------------------------------------------
# Optional vendor modules: one static lib each, all OFF by default, kept out of
# the core artifact.
#
# crypto_utils (md5/sha1/base64) comes first: wsdef.c and smtp.c both need it.
# Both link the same lib — compiling the same source into two libs that one
# binary links gives duplicate symbols.
# ---------------------------------------------------------------------------
if(MINICONET_WITH_CRYPTO_UTILS OR MINICONET_WITH_HTTP_PARSER OR MINICONET_WITH_PROTOCOL_CLIENTS)
    add_library(miniconet2_crypto_utils STATIC
        compat/util/md5.c
        compat/util/sha1.c
        compat/util/base64.c
    )
    target_include_directories(miniconet2_crypto_utils PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/compat/util>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/compat/portable>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/compat>
        $<INSTALL_INTERFACE:include>
    )
    # target_compile_options(miniconet2_crypto_utils PRIVATE -Wno-unused-variable -Wno-unused-const-variable)
endif()

if(MINICONET_WITH_PROTOCOL_CLIENTS)
    add_library(miniconet2_protocol STATIC
        compat/herr.c
        compat/protocol/dns.c
        compat/protocol/ftp.c
        compat/protocol/smtp.c
        compat/protocol/icmp.c
    )
    target_include_directories(miniconet2_protocol PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/compat/protocol>
        $<INSTALL_INTERFACE:include>
    )
    target_link_libraries(miniconet2_protocol PUBLIC miniconet2 miniconet2_crypto_utils)  # base64 used by smtp.c
    # target_compile_options(miniconet2_protocol PRIVATE -Wno-unused-variable -Wno-unused-but-set-variable)
endif()

if(MINICONET_WITH_HTTP_PARSER)
    add_library(miniconet2_http_parser STATIC
        compat/http/picohttpparser.c
        compat/http/multipart_parser.c
        compat/http/websocket_parser.c
        compat/http/httpdef.c
        compat/http/wsdef.c
    )
    target_include_directories(miniconet2_http_parser PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/compat/http>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/compat/portable>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/compat>
        $<INSTALL_INTERFACE:include>
    )
    target_link_libraries(miniconet2_http_parser PUBLIC miniconet2_crypto_utils)  # sha1+base64 for wsdef.c

    # Coroutine glue layer: wraps the pure parsers into one-call coroutine APIs.
    add_library(miniconet2_websocket STATIC miniconet_websocket.c)
    target_link_libraries(miniconet2_websocket PUBLIC miniconet2 miniconet2_http_parser)

    add_library(miniconet2_http_client STATIC miniconet_http.c)
    target_link_libraries(miniconet2_http_client PUBLIC
        miniconet2 miniconet2_http_parser miniconet2_wrapper_utils)

    add_library(miniconet2_http_server STATIC miniconet_http_server.c)
    target_link_libraries(miniconet2_http_server PUBLIC miniconet2 miniconet2_http_parser miniconet2_websocket miniconet2_http_client)

    if(MINICONET_ENABLE_SANITIZERS)
        target_compile_options(miniconet2_http_client PUBLIC -fsanitize=address,undefined -g)
        target_link_options(miniconet2_http_client PUBLIC -fsanitize=address,undefined)
        target_compile_options(miniconet2_http_server PUBLIC -fsanitize=address,undefined -g)
        target_link_options(miniconet2_http_server PUBLIC -fsanitize=address,undefined)
    endif()
endif()

if(MINICONET_WITH_MQTT_PROTOCOL)
    add_library(miniconet2_mqtt_protocol STATIC
        compat/mqtt/mqtt_protocol.c
    )
    target_include_directories(miniconet2_mqtt_protocol PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/compat/mqtt>
        $<INSTALL_INTERFACE:include>
    )
    target_link_libraries(miniconet2_mqtt_protocol PUBLIC miniconet2)  # needs hendian.h
endif()

if(MINICONET_WITH_MINIUPNP)
    add_library(miniconet2_miniupnp STATIC
        compat/miniupnp/minixml.c
        compat/miniupnp/igd_desc_parse.c
        compat/miniupnp/upnpreplyparse.c
        compat/miniupnp/portlistingparse.c
    )
    target_include_directories(miniconet2_miniupnp PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/compat/miniupnp>
        $<INSTALL_INTERFACE:include>
    )
endif()

if(MINICONET_WITH_UPNP_CLIENT)
    add_library(miniconet2_upnp STATIC
        miniconet_upnp.c
    )
    target_include_directories(miniconet2_upnp PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/compat/miniupnp>
        $<INSTALL_INTERFACE:include>
    )
    # miniconet_upnp.c picks the real impl vs stub via #ifdef
    target_compile_definitions(miniconet2_upnp PRIVATE MINICONET_WITH_UPNP_CLIENT)
    # mco_http_* from miniconet2_http_client; igd/reply parse from miniupnp.
    target_link_libraries(miniconet2_upnp PUBLIC
        miniconet2_http_client miniconet2_miniupnp miniconet2)
endif()

if(MINICONET_WITH_KCP)
    add_library(miniconet2_kcp STATIC
        compat/kcp/ikcp.c
    )
    target_include_directories(miniconet2_kcp PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/compat/kcp>
        $<INSTALL_INTERFACE:include>
    )
endif()

if(MINICONET_WITH_IDNA)
    add_library(miniconet2_idna STATIC
        compat/util/idna.c
    )
    target_include_directories(miniconet2_idna PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/compat/util>
        $<INSTALL_INTERFACE:include>
    )
    target_include_directories(miniconet2_idna PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/compat
    )
endif()

if(MINICONET_WITH_WRAPPER_UTILS)
    add_library(miniconet2_wrapper_utils STATIC
        compat/wrapper/charry.c
        compat/hmemdup.c
    )
    target_include_directories(miniconet2_wrapper_utils PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/compat/wrapper>
        $<INSTALL_INTERFACE:include>
    )
    target_include_directories(miniconet2_wrapper_utils PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/compat
        ${CMAKE_CURRENT_SOURCE_DIR}/compat/portable
    )
endif()

# ---------------------------------------------------------------------------
# Smoke/regression tests (test/) + examples (examples/): each test/*.c becomes an
# executable named after the file and is registered with CTest.
#
# WORKING_DIRECTORY is the source root: test/tls_*.c reference test/certs/ by
# relative path.
# ---------------------------------------------------------------------------
if(MINICONET_BUILD_EXAMPLES)
    enable_testing()
    # Tests live in test/ (white-box integration tests); each *.c becomes one
    # CTest executable. examples/ keeps real demos, built but not registered.
    file(GLOB MINICONET_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test/*.c)

    # These need miniconet2_websocket / miniconet2_http_client, which exist only
    # when MINICONET_WITH_HTTP_PARSER=ON, so they cannot use the generic loop.
    set(MINICONET_HTTP_PARSER_TESTS
        ${CMAKE_CURRENT_SOURCE_DIR}/test/websocket_smoke.c
        ${CMAKE_CURRENT_SOURCE_DIR}/test/http_client_smoke.c
        ${CMAKE_CURRENT_SOURCE_DIR}/test/http_server_smoke.c
        ${CMAKE_CURRENT_SOURCE_DIR}/test/http_https_server_smoke.c
    )
    # needs core/mco_workpool.h symbols, only built when WORKPOOL=ON
    set(MINICONET_WORKPOOL_TESTS
        ${CMAKE_CURRENT_SOURCE_DIR}/test/workpool_smoke.c
        ${CMAKE_CURRENT_SOURCE_DIR}/test/workpool_inflight_smoke.c
        ${CMAKE_CURRENT_SOURCE_DIR}/test/accept_dispatch_smoke.c
        ${CMAKE_CURRENT_SOURCE_DIR}/test/workpool_backpressure_smoke.c
    )
    # needs miniconet2_upnp, only present when UPNP_CLIENT=ON
    set(MINICONET_UPNP_TESTS
        ${CMAKE_CURRENT_SOURCE_DIR}/test/upnp_smoke.c
    )
    # benchmark, not a regression test: registered separately, looser timeout,
    # no sanitizer (throughput under ASan is meaningless).
    set(MINICONET_BENCH_HTTP_TESTS
        ${CMAKE_CURRENT_SOURCE_DIR}/test/bench_http_server.c
    )
    # needs mco_engine_iocp_ops(), only built when IOCP=ON
    set(MINICONET_IOCP_TESTS
        ${CMAKE_CURRENT_SOURCE_DIR}/test/iocp_async_smoke.c
    )
    list(REMOVE_ITEM MINICONET_TEST_SOURCES
        ${MINICONET_HTTP_PARSER_TESTS} ${MINICONET_WORKPOOL_TESTS} ${MINICONET_UPNP_TESTS}
        ${MINICONET_BENCH_HTTP_TESTS} ${MINICONET_IOCP_TESTS})

    foreach(test_src ${MINICONET_TEST_SOURCES})
        get_filename_component(test_name ${test_src} NAME_WE)
        add_executable(${test_name} ${test_src})
        target_link_libraries(${test_name} PRIVATE miniconet2)
        add_test(NAME ${test_name} COMMAND ${test_name})
        set_tests_properties(${test_name} PROPERTIES
            TIMEOUT 30
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
    endforeach()

    if(MINICONET_WITH_HTTP_PARSER)
        foreach(test_src ${MINICONET_HTTP_PARSER_TESTS})
            if(EXISTS ${test_src})
                get_filename_component(test_name ${test_src} NAME_WE)
                add_executable(${test_name} ${test_src})
                target_link_libraries(${test_name} PRIVATE miniconet2_websocket miniconet2_http_client miniconet2_http_server)
                if(MINICONET_ENABLE_SANITIZERS)
                    target_compile_options(${test_name} PRIVATE -fsanitize=address,undefined -g)
                    target_link_options(${test_name} PRIVATE -fsanitize=address,undefined)
                endif()
                add_test(NAME ${test_name} COMMAND ${test_name})
                set_tests_properties(${test_name} PROPERTIES
                    TIMEOUT 30
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                )
            endif()
        endforeach()

        # HTTP benchmark: links http_server; no sanitizer; 120s timeout.
        foreach(test_src ${MINICONET_BENCH_HTTP_TESTS})
            if(EXISTS ${test_src})
                get_filename_component(test_name ${test_src} NAME_WE)
                add_executable(${test_name} ${test_src})
                target_link_libraries(${test_name} PRIVATE miniconet2 miniconet2_http_server)
                add_test(NAME ${test_name} COMMAND ${test_name})
                set_tests_properties(${test_name} PROPERTIES
                    TIMEOUT 120
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                )
            endif()
        endforeach()
    endif()

    if(MINICONET_WITH_WORKPOOL)
        foreach(test_src ${MINICONET_WORKPOOL_TESTS})
            if(EXISTS ${test_src})
                get_filename_component(test_name ${test_src} NAME_WE)
                add_executable(${test_name} ${test_src})
                target_link_libraries(${test_name} PRIVATE miniconet2)
                add_test(NAME ${test_name} COMMAND ${test_name})
                set_tests_properties(${test_name} PROPERTIES
                    TIMEOUT 30
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                )
            endif()
        endforeach()
    endif()

    if(MINICONET_WITH_IOCP)
        foreach(test_src ${MINICONET_IOCP_TESTS})
            if(EXISTS ${test_src})
                get_filename_component(test_name ${test_src} NAME_WE)
                add_executable(${test_name} ${test_src})
                target_link_libraries(${test_name} PRIVATE miniconet2)
                add_test(NAME ${test_name} COMMAND ${test_name})
                set_tests_properties(${test_name} PROPERTIES
                    TIMEOUT 30
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                )
            endif()
        endforeach()
    endif()

    if(MINICONET_WITH_UPNP_CLIENT)
        foreach(test_src ${MINICONET_UPNP_TESTS})
            if(EXISTS ${test_src})
                get_filename_component(test_name ${test_src} NAME_WE)
                add_executable(${test_name} ${test_src})
                target_link_libraries(${test_name} PRIVATE miniconet2_upnp)
                add_test(NAME ${test_name} COMMAND ${test_name})
                set_tests_properties(${test_name} PROPERTIES
                    TIMEOUT 30
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                )
            endif()
        endforeach()
    endif()

    # Examples: compiled but not registered with CTest. socks5_server.c is also
    # included by test/socks5_smoke.c via SOCKS5_EXCLUDE_MAIN.
    set(MINICONET_EXAMPLE_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/tcp_proxy_server.c
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/socks5_server.c
    )
    foreach(example_src ${MINICONET_EXAMPLE_SOURCES})
        if(EXISTS ${example_src})
            get_filename_component(example_name ${example_src} NAME_WE)
            add_executable(${example_name} ${example_src})
            target_link_libraries(${example_name} PRIVATE miniconet2)
        endif()
    endforeach()
endif()

message(STATUS "miniconet2: TLS backend           = ${MINICONET_SSL_BACKEND}")
message(STATUS "miniconet2: Unix Domain Socket     = ${MINICONET_ENABLE_UDS}")
message(STATUS "miniconet2: io_uring engine        = ${MINICONET_WITH_IO_URING}")
message(STATUS "miniconet2: iocp engine (Windows)   = ${MINICONET_WITH_IOCP}")
message(STATUS "miniconet2: protocol clients       = ${MINICONET_WITH_PROTOCOL_CLIENTS}")
message(STATUS "miniconet2: http parser            = ${MINICONET_WITH_HTTP_PARSER}")
message(STATUS "miniconet2: mqtt protocol          = ${MINICONET_WITH_MQTT_PROTOCOL}")
message(STATUS "miniconet2: miniupnp xml parser    = ${MINICONET_WITH_MINIUPNP}")
message(STATUS "miniconet2: kcp core algorithm     = ${MINICONET_WITH_KCP}")
message(STATUS "miniconet2: crypto utils(md5/sha1) = ${MINICONET_WITH_CRYPTO_UTILS} (http parser 或 protocol clients 打开时会自动一起编译)")
message(STATUS "miniconet2: idna                   = ${MINICONET_WITH_IDNA}")
message(STATUS "miniconet2: wrapper utils          = ${MINICONET_WITH_WRAPPER_UTILS}")
message(STATUS "miniconet2: workpool (multi-vcpu)  = ${MINICONET_WITH_WORKPOOL}")

# ---------------------------------------------------------------------------
# add_subdirectory support: one name for both integration styles.
#   - find_package(miniconet2)    -> config file provides miniconet2::xxx
#   - add_subdirectory(miniconet) -> aliases so consumers can use
#     `target_link_libraries(xxx miniconet2::miniconet2)` either way.
# Aliases are created only for existing targets; the export namespace does not
# clash (IMPORTED in the install tree vs ALIAS in the build tree).
foreach(_mc_t miniconet2 miniconet2_crypto_utils miniconet2_http_client
        miniconet2_http_parser miniconet2_http_server miniconet2_idna
        miniconet2_kcp miniconet2_miniupnp miniconet2_mqtt_protocol
        miniconet2_protocol miniconet2_upnp miniconet2_websocket
        miniconet2_wrapper_utils)
    if(TARGET ${_mc_t} AND NOT TARGET miniconet2::${_mc_t})
        add_library(miniconet2::${_mc_t} ALIAS ${_mc_t})
    endif()
endforeach()
unset(_mc_t)

# ---------------------------------------------------------------------------
# Install (cmake --install <build> [--prefix /usr/local])
#
# Consumers do not define MINICONET_HAVE_GENERATED_CONFIG and take the fallback
# headers, which are all-zero in the source tree. Installing copies the generated
# headers over those paths so the shipped ones reflect this build.
# ---------------------------------------------------------------------------
set(MINICONET_INSTALL_GEN_DIR ${MINICONET_GENERATED_DIR}/miniconet_generated)

# Core lib + optional libs (one EXPORT set). Transitive deps must be installed
# too, or cmake --install errors. Only existing targets are kept.
set(MINICONET_INSTALL_TARGETS miniconet2)
foreach(_mc_t
        miniconet2_crypto_utils miniconet2_http_client miniconet2_http_parser
        miniconet2_http_server miniconet2_idna miniconet2_kcp miniconet2_miniupnp
        miniconet2_mqtt_protocol miniconet2_protocol miniconet2_upnp
        miniconet2_websocket miniconet2_wrapper_utils)
    if(TARGET ${_mc_t})
        list(APPEND MINICONET_INSTALL_TARGETS ${_mc_t})
    endif()
endforeach()
unset(_mc_t)

install(TARGETS ${MINICONET_INSTALL_TARGETS}
        EXPORT miniconet2-targets
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)

# Public headers, aligned with miniconet2's PUBLIC include dirs:
#   - root: miniconet.h + miniconet_*.h (config overwritten below)
#   - compat/* and subdirs, minus the fallback hconfig.h
#   - core/* and minicoro/*
file(GLOB MINICONET_PUBLIC_H ${CMAKE_CURRENT_SOURCE_DIR}/miniconet*.h)
list(REMOVE_ITEM MINICONET_PUBLIC_H ${CMAKE_CURRENT_SOURCE_DIR}/miniconet_config.h)
install(FILES ${MINICONET_PUBLIC_H} DESTINATION include)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/compat
        DESTINATION include
        FILES_MATCHING PATTERN "*.h" PATTERN "hconfig.h" EXCLUDE)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/core
        DESTINATION include
        FILES_MATCHING PATTERN "*.h")
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/minicoro
        DESTINATION include
        FILES_MATCHING PATTERN "*.h")
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tclog
        DESTINATION include
        FILES_MATCHING PATTERN "*.h")

# overwrite the fallback paths with the generated config
install(FILES ${MINICONET_INSTALL_GEN_DIR}/hconfig.h
        DESTINATION include/compat)
install(FILES ${MINICONET_INSTALL_GEN_DIR}/miniconet_config.h
        DESTINATION include)

# make find_package(miniconet2) work: include dirs and deps
install(EXPORT miniconet2-targets
        NAMESPACE miniconet2::
        DESTINATION lib/cmake/miniconet2)
include(CMakePackageConfigHelpers)
# find_package pulls in Threads (and OpenSSL when enabled) for consumers.
set(_mc_config_deps "include(CMakeFindDependencyMacro)\nfind_dependency(Threads)")
if(MINICONET_WITH_OPENSSL)
    string(APPEND _mc_config_deps "\nfind_dependency(OpenSSL)")
endif()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/miniconet2-config.cmake
"${_mc_config_deps}\ninclude(\"\${CMAKE_CURRENT_LIST_DIR}/miniconet2-targets.cmake\")\n")
unset(_mc_config_deps)
install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/miniconet2-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/miniconet2-config-version.cmake
        DESTINATION lib/cmake/miniconet2)
unset(MINICONET_INSTALL_GEN_DIR)
unset(MINICONET_INSTALL_TARGETS)
