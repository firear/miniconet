/*
 * hconfig.h — fallback defaults for platform/feature detection (not a cmake
 * generated file). Included by hplatform.h only when MINICONET_HAVE_GENERATED_CONFIG
 * is undefined (a raw -I source-tree build with no generated config). CMake builds
 * use the generated values instead. Tune per platform via -DHAVE_XXX=0/1.
 * Every macro is wrapped in #ifndef so the command line can override it.
 */
#ifndef HV_CONFIG_H_
#define HV_CONFIG_H_

/* ---- headers ---- */
#ifndef HAVE_STDBOOL_H
#define HAVE_STDBOOL_H 1
#endif
#ifndef HAVE_STDINT_H
#define HAVE_STDINT_H 1
#endif
#ifndef HAVE_INTTYPES_H
#define HAVE_INTTYPES_H 1
#endif
#ifndef HAVE_STDATOMIC_H
#define HAVE_STDATOMIC_H 1
#endif
#ifndef HAVE_SYS_TYPES_H
#define HAVE_SYS_TYPES_H 1
#endif
#ifndef HAVE_SYS_STAT_H
#define HAVE_SYS_STAT_H 1
#endif
#ifndef HAVE_SYS_TIME_H
#define HAVE_SYS_TIME_H 1
#endif
#ifndef HAVE_FCNTL_H
#define HAVE_FCNTL_H 1
#endif
#ifndef HAVE_PTHREAD_H
#define HAVE_PTHREAD_H 1
#endif
#ifndef HAVE_ENDIAN_H
#define HAVE_ENDIAN_H 1
#endif
#ifndef HAVE_SYS_ENDIAN_H
#define HAVE_SYS_ENDIAN_H 0   /* BSD only; Linux/glibc uses endian.h */
#endif
#ifndef HAVE_BYTESWAP_H
#define HAVE_BYTESWAP_H 1
#endif
#ifndef HAVE_IFADDRS_H
#define HAVE_IFADDRS_H 1
#endif

/* ---- functions ---- */
#ifndef HAVE_STRLCPY
#define HAVE_STRLCPY 1
#endif
#ifndef HAVE_STRLCAT
#define HAVE_STRLCAT 1
#endif
#ifndef HAVE_CLOCK_GETTIME
#define HAVE_CLOCK_GETTIME 1
#endif
#ifndef HAVE_GETTIMEOFDAY
#define HAVE_GETTIMEOFDAY 1
#endif
#ifndef HAVE_PTHREAD_SPIN_LOCK
#define HAVE_PTHREAD_SPIN_LOCK 1
#endif
#ifndef HAVE_PTHREAD_MUTEX_TIMEDLOCK
#define HAVE_PTHREAD_MUTEX_TIMEDLOCK 1
#endif
#ifndef HAVE_SEM_TIMEDWAIT
#define HAVE_SEM_TIMEDWAIT 1
#endif
#ifndef HAVE_PIPE
#define HAVE_PIPE 1
#endif
#ifndef HAVE_SOCKETPAIR
#define HAVE_SOCKETPAIR 1
#endif
#ifndef HAVE_EVENTFD
#define HAVE_EVENTFD 1
#endif
#ifndef HAVE_SETPROCTITLE
#define HAVE_SETPROCTITLE 0   /* needs libbsd, off by default */
#endif
#ifndef HAVE_GETTID
#define HAVE_GETTID 1         /* Linux, needs _GNU_SOURCE */
#endif
#ifndef HAVE_SCHED_GETCPU
#define HAVE_SCHED_GETCPU 1   /* Linux, needs _GNU_SOURCE */
#endif

/* ---- MSVC ----
 * Matches the three zeros forced in cmake/PlatformConfig.cmake's MSVC branch:
 *   - <stdatomic.h>: needs /std:c11+ on MSVC, not usable in this C90 project
 *   - sys/time.h  : the Windows SDK stub is not POSIX
 *   - pthread.h   : MSVC has no POSIX pthread (MinGW uses winpthreads -> 1)
 * Under cmake the probe wins; this fallback only serves non-cmake builds. */
#if defined(_MSC_VER)
#undef  HAVE_STDATOMIC_H
#define HAVE_STDATOMIC_H 0
#undef  HAVE_SYS_TIME_H
#define HAVE_SYS_TIME_H 0
#undef  HAVE_PTHREAD_H
#define HAVE_PTHREAD_H 0
#endif

/* ---- project feature switches (here because vendor hsocket.h/c pull this in via
 * hplatform.h). The generated config supplies the real values under cmake. ---- */
#ifndef ENABLE_UDS
#define ENABLE_UDS 0
#endif

#endif /* HV_CONFIG_H_ */
