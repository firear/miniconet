# cmake/PlatformConfig.cmake
#
# Platform/feature detection + unified generation of the build switch macros.
#
# compat/hconfig.h used to be chv's generated artifact (written back into the
# source tree) kept as a hand-written file, which meant: values were guesses
# (hardcoded 1 on Linux), the artifact polluted the source tree across multi-config
# builds, and a misspelled macro name silently evaluated to 0 in #if.
#
# Now: detection + generation live here and write to the build dir
# (out-of-source); macros are always 0/1 (#cmakedefine01), so use #if only —
# never #ifdef/#ifndef. That gives: every name is centralized in the two
# templates, and a disabled switch is 0 rather than undefined, so a missing -D
# cannot silently pick another branch.
#
# Usage (top-level CMakeLists.txt):
#   include(PlatformConfig)
#   miniconet_detect_platform()      # detect, produce HAVE_* (before add_library)
#   include_directories(${MINICONET_GENERATED_DIR})
#   ...targets...
#   miniconet_configure_ssl_backend(miniconet2)
#   miniconet_generate_config()      # generate both headers (after SSL is settled)

include(CheckIncludeFile)
include(CheckSymbolExists)
include(CheckLibraryExists)

set(MINICONET_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated" CACHE INTERNAL "生成的配置头目录")

# ---- detection helpers ---------------------------------------------------
#
# Results are normalized to 0/1 (CMake check_* yields empty/true/false) and can be
# overridden with -DHAVE_XXX=0 (cross-compile escape hatch: defined vars skip
# detection).
macro(_mco_check_header header)
    string(TOUPPER "${header}" _u)
    string(REGEX REPLACE "[/.]" "_" _u "${_u}")
    set(_out HAVE_${_u})
    if(NOT DEFINED ${_out})
        check_include_file("${header}" ${_out})
    endif()
    if(${_out})
        set(${_out} 1)
    else()
        set(${_out} 0)
    endif()
    unset(_out)
    unset(_u)
endmacro()

macro(_mco_check_symbol sym header out)
    if(NOT DEFINED ${out})
        check_symbol_exists("${sym}" "${header}" ${out})
    endif()
    if(${out})
        set(${out} 1)
    else()
        set(${out} 0)
    endif()
endmacro()

# Zero a group of vars by name (none exist on Windows; skip needless probing)
macro(_mco_set_zero)
    foreach(_v ${ARGN})
        if(NOT DEFINED ${_v})
            set(${_v} 0)
        endif()
    endforeach()
    unset(_v)
endmacro()

# ---- detection: macros actually used by compat/ and core/ ----------------
#
# Listed by grepping HAVE_ across the tree, not copied from chv: chv's
# WITH_OPENSSL / USE_MULTIMAP / EVENT_IOCP / EVENT_IOURING / WITH_KCP are its own
# switches; ours are MINICONET_* and are not managed here.
function(miniconet_detect_platform)
    find_package(Threads QUIET)
    set(CMAKE_REQUIRED_LIBRARIES ${CMAKE_THREAD_LIBS_INIT})

    # --- headers ---
    _mco_check_header("stdbool.h")     # wepoll.c / hplatform.h
    _mco_check_header("stdint.h")      # hplatform.h
    _mco_check_header("inttypes.h")    # hplatform.h
    _mco_check_header("stdatomic.h")   # hatomic.h: C11 atomics vs GCC __sync fallback
    _mco_check_header("sys/types.h")   # hplatform.h
    _mco_check_header("sys/stat.h")    # hplatform.h / miniconet.c(fstat)
    _mco_check_header("sys/time.h")    # hplatform.h
    _mco_check_header("fcntl.h")       # hplatform.h
    _mco_check_header("pthread.h")     # hplatform.h / hthread.h / thrqueue_cs.c
    _mco_check_header("endian.h")      # hendian.h
    _mco_check_header("sys/endian.h")  # hendian.h（BSD）
    _mco_check_header("byteswap.h")    # hendian.h
    _mco_check_header("ifaddrs.h")     # currently unused, kept for vendor modules

    if(WIN32)
        # Windows: none of these exist; force 0 without probing.
        # HAVE_PTHREAD_H / HAVE_SYS_TIME_H are excluded: MinGW(w64) has
        # winpthreads and sys/time.h, so they go through normal detection.
        _mco_set_zero(HAVE_ENDIAN_H HAVE_SYS_ENDIAN_H
                      HAVE_BYTESWAP_H HAVE_IFADDRS_H
                      HAVE_GETTID HAVE_PTHREAD_SPIN_LOCK HAVE_PTHREAD_MUTEX_TIMEDLOCK
                      HAVE_SEM_TIMEDWAIT HAVE_PIPE HAVE_SOCKETPAIR HAVE_EVENTFD
                      HAVE_SETPROCTITLE HAVE_SCHED_GETCPU)
    else()
        # --- functions ---
        _mco_check_symbol("strlcpy"  "string.h"   HAVE_STRLCPY)          # hbase.h
        _mco_check_symbol("strlcat"  "string.h"   HAVE_STRLCAT)          # hbase.h
        _mco_check_symbol("gettimeofday" "sys/time.h" HAVE_GETTIMEOFDAY) # htime.c
        _mco_check_symbol("pthread_spin_lock"       "pthread.h"  HAVE_PTHREAD_SPIN_LOCK)       # hmutex.h
        _mco_check_symbol("pthread_mutex_timedlock" "pthread.h"  HAVE_PTHREAD_MUTEX_TIMEDLOCK) # hmutex.h
        _mco_check_symbol("sem_timedwait"           "semaphore.h" HAVE_SEM_TIMEDWAIT)          # hmutex.h
        _mco_check_symbol("pipe"       "unistd.h"     HAVE_PIPE)        # mco_oneshot.c
        _mco_check_symbol("socketpair" "sys/socket.h" HAVE_SOCKETPAIR)  # hsocket.c
        _mco_check_symbol("eventfd"    "sys/eventfd.h" HAVE_EVENTFD)
        _mco_check_symbol("setproctitle" "unistd.h" HAVE_SETPROCTITLE)

        # clock_gettime: in libc for glibc >= 2.17, in librt for older
        _mco_check_symbol("clock_gettime" "time.h" HAVE_CLOCK_GETTIME)  # htime.c/htime.h
        if(NOT HAVE_CLOCK_GETTIME)
            check_library_exists(rt clock_gettime "" MINICONET_CLOCK_GETTIME_IN_RT)
            if(MINICONET_CLOCK_GETTIME_IN_RT)
                set(HAVE_CLOCK_GETTIME 1 PARENT_SCOPE)
                set(MINICONET_NEED_LIBRT 1 PARENT_SCOPE)
            endif()
        endif()

        # these need _GNU_SOURCE (gettid is Linux-specific)
        set(_saved_defs ${CMAKE_REQUIRED_DEFINITIONS})
        set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
        _mco_check_symbol("gettid"       "unistd.h" HAVE_GETTID)         # hthread.h
        _mco_check_symbol("sched_getcpu" "sched.h"  HAVE_SCHED_GETCPU)   # hthread.c
        set(CMAKE_REQUIRED_DEFINITIONS ${_saved_defs})
        unset(_saved_defs)
    endif()

    # MSVC: three "header exists != usable" traps, forced to 0.
    #   - <stdatomic.h>: usable only with /std:c11+, while C_STANDARD here is 90.
    #   - sys/time.h: the Windows SDK stub has different struct timeval semantics,
    #     must not take the POSIX branch.
    #   - pthread.h: MSVC has no POSIX pthread (MinGW has winpthreads).
    # Override with -DHAVE_STDATOMIC_H=1 &c. (already-defined vars are skipped).
    if(MSVC)
        _mco_set_zero(HAVE_STDATOMIC_H HAVE_SYS_TIME_H HAVE_PTHREAD_H)
    endif()

    # propagate results to the caller scope (set() inside a function is local)
    set(_mco_vars
        HAVE_STDBOOL_H HAVE_STDINT_H HAVE_INTTYPES_H HAVE_STDATOMIC_H
        HAVE_SYS_TYPES_H HAVE_SYS_STAT_H HAVE_SYS_TIME_H HAVE_FCNTL_H HAVE_PTHREAD_H
        HAVE_ENDIAN_H HAVE_SYS_ENDIAN_H HAVE_BYTESWAP_H HAVE_IFADDRS_H
        HAVE_STRLCPY HAVE_STRLCAT HAVE_CLOCK_GETTIME HAVE_GETTIMEOFDAY
        HAVE_PTHREAD_SPIN_LOCK HAVE_PTHREAD_MUTEX_TIMEDLOCK HAVE_SEM_TIMEDWAIT
        HAVE_PIPE HAVE_SOCKETPAIR HAVE_EVENTFD HAVE_SETPROCTITLE
        HAVE_GETTID HAVE_SCHED_GETCPU)
    foreach(_v ${_mco_vars})
        set(${_v} ${${_v}} PARENT_SCOPE)
    endforeach()
endfunction()

# ---- generation ----------------------------------------------------------
#
# Both headers go to the build dir:
#   hconfig.h           platform/feature detection results (HAVE_*). The chv name
#                       is kept so the vendored hplatform.h needs no change.
#   miniconet_config.h  this project's switches (MINICONET_* / ENABLE_UDS),
#                       replacing scattered target_compile_definitions(...).
function(miniconet_generate_config)
    # A backend may be downgraded to none when its library is missing, so
    # MINICONET_HAVE_SSL must be decided after miniconet_configure_ssl_backend().
    if(MINICONET_SSL_BACKEND AND NOT MINICONET_SSL_BACKEND STREQUAL "none")
        set(MINICONET_HAVE_SSL 1)
    else()
        set(MINICONET_HAVE_SSL 0)
    endif()

    # ---- explicit option -> macro mapping ----
    #
    # Listed explicitly rather than relying on macro names matching option names.
    # A mismatch (ENABLE_UDS /
    # MINICONET_ENABLE_UDS、MINICONET_TEST_HOOKS / MINICONET_BUILD_TEST_HOOKS），
    # silently generates 0, dropping whole code paths and turning tests into green
    # no-ops, invisible in CI. New switches must appear here.
    if(MINICONET_ENABLE_UDS)
        set(ENABLE_UDS 1)                    # historical vendor name, no prefix
    else()
        set(ENABLE_UDS 0)
    endif()

    # the option is MINICONET_BUILD_TEST_HOOKS, the macro MINICONET_TEST_HOOKS
    # (historical name, unchanged).
    if(MINICONET_BUILD_TEST_HOOKS)
        set(MINICONET_TEST_HOOKS 1)
    else()
        set(MINICONET_TEST_HOOKS 0)
    endif()

    # The rest share the option name; reassign each so FORCEd options (UPNP forces
    # HTTP_PARSER) are reflected in the generated output.
    foreach(_sw
        MINICONET_WITH_IO_URING
        MINICONET_WITH_WORKPOOL
        MINICONET_WITH_PROTOCOL_CLIENTS
        MINICONET_WITH_HTTP_PARSER
        MINICONET_WITH_MQTT_PROTOCOL
        MINICONET_WITH_MINIUPNP
        MINICONET_WITH_KCP
        MINICONET_WITH_CRYPTO_UTILS
        MINICONET_WITH_IDNA
        MINICONET_WITH_WRAPPER_UTILS
        MINICONET_WITH_UPNP_CLIENT)
        if(${_sw})
            set(${_sw} 1)
        else()
            set(${_sw} 0)
        endif()
    endforeach()
    unset(_sw)

    # Generated into <build>/generated/miniconet_generated/: the extra subdir lets
    # the generated hconfig.h coexist with the in-tree fallback of the same name.
    # Which one wins is decided by MINICONET_HAVE_GENERATED_CONFIG (see the #ifdef
    # gates in hplatform.h and miniconet.h): external -I users take the fallback,
    # cmake builds the generated one.
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/hconfig.h.in
                   ${MINICONET_GENERATED_DIR}/miniconet_generated/hconfig.h)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/miniconet_config.h.in
                   ${MINICONET_GENERATED_DIR}/miniconet_generated/miniconet_config.h)

    message(STATUS "miniconet2: generated config    = ${MINICONET_GENERATED_DIR}/{hconfig.h, miniconet_config.h}")
    message(STATUS "miniconet2: stdatomic.h         = ${HAVE_STDATOMIC_H}")
    message(STATUS "miniconet2: clock_gettime       = ${HAVE_CLOCK_GETTIME}")
    message(STATUS "miniconet2: pthread spin/timed  = ${HAVE_PTHREAD_SPIN_LOCK}/${HAVE_PTHREAD_MUTEX_TIMEDLOCK}")
    message(STATUS "miniconet2: pipe/socketpair     = ${HAVE_PIPE}/${HAVE_SOCKETPAIR}")
    message(STATUS "miniconet2: gettid/sched_getcpu = ${HAVE_GETTID}/${HAVE_SCHED_GETCPU}")
endfunction()
