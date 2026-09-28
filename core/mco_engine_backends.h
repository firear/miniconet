/*
 * mco_engine_backends.h — explicit per-backend ops getters, for callers that
 * must pin a backend; everyone else uses mco_engine_default().
 *
 * Only defined on their own platform; elsewhere a call is a link error.
 */
#ifndef MCO_ENGINE_BACKENDS_H
#define MCO_ENGINE_BACKENDS_H

#include "mco_engine.h"
#include "miniconet_config.h"   /* MINICONET_WITH_IO_URING: always 0/1 */

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__linux__)
const mco_engine_ops_t* mco_engine_epoll_ops(void);
/* io_uring backend: only built/declared when MINICONET_WITH_IO_URING=ON. */
#if MINICONET_WITH_IO_URING
const mco_engine_ops_t* mco_engine_uring_ops(void);
#endif
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
const mco_engine_ops_t* mco_engine_kqueue_ops(void);
#endif

#if defined(__sun)
const mco_engine_ops_t* mco_engine_evport_ops(void);
#endif

#if defined(_WIN32)
const mco_engine_ops_t* mco_engine_wepoll_ops(void);
#if MINICONET_WITH_IOCP
const mco_engine_ops_t* mco_engine_iocp_ops(void);
#endif
#endif

#if !defined(_WIN32)
const mco_engine_ops_t* mco_engine_select_ops(void); /* POSIX */
const mco_engine_ops_t* mco_engine_poll_ops(void);
#else
const mco_engine_ops_t* mco_engine_select_ops(void); /* Windows, see #ifdef _WIN32 */
#endif

#ifdef __cplusplus
}
#endif
#endif /* MCO_ENGINE_BACKENDS_H */
