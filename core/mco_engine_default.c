/*
 * mco_engine_default.c — pick the backend per platform; each backend .c is
 * wrapped in #if so it compiles empty elsewhere.
 */
#include "mco_engine.h"
#include <stddef.h>
#ifdef MINICONET_HAVE_GENERATED_CONFIG
#include "miniconet_generated/miniconet_config.h"
#else
#include "miniconet_config.h"
#endif

#if defined(__linux__)
const mco_engine_ops_t* mco_engine_epoll_ops(void);
#if MINICONET_WITH_IO_URING
const mco_engine_ops_t* mco_engine_uring_ops(void);

/* No probing here. The io_uring availability check needs a live ring, so it
 * belongs to create(): selecting by building an engine and immediately
 * destroying it made every process pay for two rings instead of one. create()
 * failing is handled by mco_engine_default_fallback() below. */
const mco_engine_ops_t* mco_engine_default(void) { return mco_engine_uring_ops(); }

const mco_engine_ops_t* mco_engine_default_fallback(const mco_engine_ops_t* ops) {
    return (ops == mco_engine_uring_ops()) ? mco_engine_epoll_ops() : NULL;
}
#else
const mco_engine_ops_t* mco_engine_default(void) { return mco_engine_epoll_ops(); }
#endif

#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
const mco_engine_ops_t* mco_engine_kqueue_ops(void);
const mco_engine_ops_t* mco_engine_default(void) { return mco_engine_kqueue_ops(); }

#elif defined(__sun)
const mco_engine_ops_t* mco_engine_evport_ops(void);
const mco_engine_ops_t* mco_engine_default(void) { return mco_engine_evport_ops(); }

#elif defined(_WIN32)
const mco_engine_ops_t* mco_engine_wepoll_ops(void);
#if MINICONET_WITH_IOCP
const mco_engine_ops_t* mco_engine_iocp_ops(void);
const mco_engine_ops_t* mco_engine_default(void) { return mco_engine_iocp_ops(); }
#else
const mco_engine_ops_t* mco_engine_default(void) { return mco_engine_wepoll_ops(); }
#endif

#else
const mco_engine_ops_t* mco_engine_select_ops(void);
const mco_engine_ops_t* mco_engine_default(void) { return mco_engine_select_ops(); }

#endif

/* Only the Linux io_uring preference has a readiness fallback today; every other
 * platform's default backend is always creatable. */
#if !defined(__linux__) || !MINICONET_WITH_IO_URING
const mco_engine_ops_t* mco_engine_default_fallback(const mco_engine_ops_t* ops) {
    (void)ops;
    return NULL;
}
#endif
