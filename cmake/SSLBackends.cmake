# cmake/SSLBackends.cmake
#
# SSL/TLS backend selection: openssl / mbedtls / gnutls / wintls (Windows only),
# or none (MINICONET_SSL_BACKEND stays "none" and mco_ssl_ctx_new_*() returns NULL
# at runtime). Beyond chv's "all off by default" philosophy:
#   1. Two or more backends at once is a FATAL_ERROR instead of duplicate symbols
#      at link time (each compat/ssl/*.c only guards itself with #ifdef WITH_XXX).
#   2. mbedtls/gnutls are probed with pkg-config, so a missing package errors out
#      with install instructions instead of failing at link time.
#
# Usage: include(cmake/SSLBackends.cmake) then
# miniconet_configure_ssl_backend(<target>).

function(miniconet_configure_ssl_backend target)
    set(_enabled_count 0)
    if(MINICONET_WITH_OPENSSL)
        math(EXPR _enabled_count "${_enabled_count}+1")
    endif()
    if(MINICONET_WITH_MBEDTLS)
        math(EXPR _enabled_count "${_enabled_count}+1")
    endif()
    if(MINICONET_WITH_GNUTLS)
        math(EXPR _enabled_count "${_enabled_count}+1")
    endif()
    if(_enabled_count GREATER 1)
        message(FATAL_ERROR
            "miniconet2: MINICONET_WITH_OPENSSL / MINICONET_WITH_MBEDTLS / "
            "MINICONET_WITH_GNUTLS 只能同时打开一个——compat/ssl/*.c 每个后端"
            "文件各自定义一整套 hssl_new/hssl_read/... 符号，两个后端一起编"
            "会在链接期重复定义，不是我们支持的用法。")
    endif()

    set(MINICONET_SSL_BACKEND "none" PARENT_SCOPE)

    if(MINICONET_WITH_OPENSSL)
        find_package(OpenSSL)
        if(OpenSSL_FOUND)
            target_compile_definitions(${target} PUBLIC WITH_OPENSSL)
            target_link_libraries(${target} PUBLIC OpenSSL::SSL OpenSSL::Crypto)
            set(MINICONET_SSL_BACKEND "openssl" PARENT_SCOPE)
        else()
            message(WARNING "miniconet2: MINICONET_WITH_OPENSSL=ON 但没找到 OpenSSL "
                             "开发库，降级为不带 TLS。Ubuntu/Debian 上装 libssl-dev 即可。")
        endif()
        return()
    endif()

    if(MINICONET_WITH_MBEDTLS)
        find_package(PkgConfig)
        if(PkgConfig_FOUND)
            pkg_check_modules(MBEDTLS mbedtls mbedcrypto mbedx509)
        endif()
        if(MBEDTLS_FOUND)
            target_compile_definitions(${target} PUBLIC WITH_MBEDTLS)
            target_link_libraries(${target} PUBLIC ${MBEDTLS_LIBRARIES})
            target_include_directories(${target} PUBLIC ${MBEDTLS_INCLUDE_DIRS})
            set(MINICONET_SSL_BACKEND "mbedtls" PARENT_SCOPE)
        else()
            message(WARNING "miniconet2: MINICONET_WITH_MBEDTLS=ON 但没找到 mbedTLS "
                             "开发库，降级为不带 TLS。Ubuntu/Debian 上装 libmbedtls-dev 即可。")
        endif()
        return()
    endif()

    if(MINICONET_WITH_GNUTLS)
        find_package(PkgConfig)
        if(PkgConfig_FOUND)
            pkg_check_modules(GNUTLS gnutls)
        endif()
        if(GNUTLS_FOUND)
            target_compile_definitions(${target} PUBLIC WITH_GNUTLS)
            target_link_libraries(${target} PUBLIC ${GNUTLS_LIBRARIES})
            target_include_directories(${target} PUBLIC ${GNUTLS_INCLUDE_DIRS})
            set(MINICONET_SSL_BACKEND "gnutls" PARENT_SCOPE)
        else()
            message(WARNING "miniconet2: MINICONET_WITH_GNUTLS=ON 但没找到 GnuTLS "
                             "开发库，降级为不带 TLS。Ubuntu/Debian 上装 libgnutls28-dev 即可。")
        endif()
        return()
    endif()

    if(WIN32)
        # As in chv: on Windows with no backend chosen, enable the built-in
        # SChannel (wintls); no third-party library needed.
        target_compile_definitions(${target} PUBLIC WITH_WINTLS)
        target_link_libraries(${target} PUBLIC secur32 crypt32)
        set(MINICONET_SSL_BACKEND "wintls" PARENT_SCOPE)
    endif()
endfunction()
