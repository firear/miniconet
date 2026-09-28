/*
 * mco_engine_default.c — pick the backend per platform; each backend .c is
 * wrapped in #if so it compiles empty elsewhere.
 */
#include "mco_engine.h"
#include <stddef.h>
#include "miniconet_config.h"

#if defined(__linux__)
const mco_engine_ops_t* mco_engine_epoll_ops(void);
#if MINICONET_WITH_IO_URING
const mco_engine_ops_t* mco_engine_uring_ops(void);

const mco_engine_ops_t* mco_engine_default(void) {
    /* Lazy probe: create a uring engine once, cache io_uring on success and
     * epoll on failure (old kernel / seccomp / ENOSYS). */
    static const mco_engine_ops_t* cached = NULL;
    if (!cached) {
        mco_engine_t* probe = mco_engine_uring_ops()->create();
        if (probe) {
            mco_engine_uring_ops()->destroy(probe);
            cached = mco_engine_uring_ops();
        } else {
            cached = mco_engine_epoll_ops();
        }
    }
    return cached;
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
