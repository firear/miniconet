/* mco_engine.h — minimal io multiplexing engine interface (pluggable backends). */
#ifndef MCO_ENGINE_H
#define MCO_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mco_engine mco_engine_t;   /* opaque */

#define MCO_EV_READ   0x1
#define MCO_EV_WRITE  0x2
/* Error/hangup; read()/recv() once for the real errno or EOF. */
#define MCO_EV_ERROR  0x4

/* Readiness cb; ud from ctl(). Called inside poll(); must not block. */
typedef void (*mco_engine_cb)(void* ud, int fd, int revents);

typedef struct mco_engine_ops {
    const char*   name;
    mco_engine_t* (*create)(void);
    void          (*destroy)(mco_engine_t* e);

    /* Register/modify fd events. Read and write are independent slots, so two
     * coroutines can wait on one fd; only the directions in events change
     * (cb!=NULL sets, cb==NULL clears). 0 = ok, <0 = error.
     * ud is the registration identity: an unregister only applies on a ud match
     * (fd-reuse safe) and registering over a foreign ud forces a kernel resync. */
    int (*ctl)(mco_engine_t* e, int fd, int events, mco_engine_cb cb, void* ud);

    /* Wait for readiness (timeout_ms<0 forever, 0 poll once) and invoke each
     * ready cb. Returns the fired event count, <0 on error. */
    int (*poll)(mco_engine_t* e, int timeout_ms);

    /* Wake a poll()-blocked engine from another thread. Optional. */
    void (*wake)(mco_engine_t* e);

    /* Optional completion-style (io_uring/IOCP) extension; NULL elsewhere.
     * submit_* queues an op and returns a cookie (0 = not queued, use the
     * readiness path); on completion cb(ud, fd, res) runs inside poll() with
     * res>=0 bytes/new fd, <0 -errno. buf must stay valid until then.
     * submit_cancel cancels a pending op; it is async, the op still completes
     * once with -ECANCELED or its real result. */
    const struct mco_engine_async_ops* async;
} mco_engine_ops_t;

typedef void (*mco_engine_async_cb)(void* ud, int fd, int res);

/* submit_* cookie: 0 = not submitted, use the readiness path;
 * MCO_ASYNC_SYNC_DONE = completed inside submit_*, cb already ran, do not yield;
 * otherwise pending, result via cb. bit63 is free in both backends. */
#define MCO_ASYNC_SYNC_DONE ((uint64_t)1 << 63)

struct mco_engine_async_ops {
    uint64_t (*submit_read)(mco_engine_t* e, int fd, void* buf, int len,
                            mco_engine_async_cb cb, void* ud);
    uint64_t (*submit_write)(mco_engine_t* e, int fd, const void* buf, int len,
                             mco_engine_async_cb cb, void* ud);
    /* addr is copied at submit time */
    uint64_t (*submit_connect)(mco_engine_t* e, int fd, const void* addr, unsigned addrlen,
                               mco_engine_async_cb cb, void* ud);
    uint64_t (*submit_accept)(mco_engine_t* e, int fd, mco_engine_async_cb cb, void* ud);
    /* 0 = cancel accepted or op already done; <0 = submit failed */
    int (*submit_cancel)(mco_engine_t* e, uint64_t cookie, void* ud);
    /* Backend identity questions live in mco_accept_uses_async(), not here. */
};

/* Pick a backend per platform: Linux io_uring (else epoll), BSD/mac kqueue,
 * Windows wepoll, otherwise select. */
const mco_engine_ops_t* mco_engine_default(void);

#ifdef __cplusplus
}
#endif
#endif /* MCO_ENGINE_H */
