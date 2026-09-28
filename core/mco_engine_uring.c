/*
 * mco_engine_uring.c — Linux io_uring backend (raw syscalls, no liburing)
 *
 * Two layers:
 *   1) Readiness: POLL_ADD (one-shot) / POLL_REMOVE implement ctl/poll/wake.
 *      A per-watch gen voids POLL CQEs left from before fd reuse (io_uring does
 *      not remove polls on close, unlike epoll).
 *   2) Completion (ops->async): READ/WRITE/CONNECT/ACCEPT SQEs whose CQEs are
 *      dispatched inside poll(). The coroutine resumes only after the op's CQE,
 *      so its stack buffer stays alive.
 *
 * Requires 5.1+ (POLL_ADD/TIMEOUT) and IORING_FEAT_NODROP (5.5+, else creation
 * fails and the caller falls back to epoll). Completion also needs READ/WRITE
 * (5.6+) and ASYNC_CANCEL (5.5+); probed at create, else has_async=0.
 *
 * SQEs are buffered locally (events are only dispatched in poll()); sqe_alloc
 * flushes when the SQ is full. Before blocking, enter_wait submits and waits in
 * one syscall, with the timeout built in via EXT_ARG when available.
 */
#if defined(__linux__)

#include "mco_engine.h"
#include "mco_uring.h"
#include "../compat/hbase.h"

#include <poll.h>      /* POLLIN/...: POLL_ADD uses poll(2) flags */
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>     /* snprintf in the probes */
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>    /* abstract unix listener for the ACCEPT probe */
#include <netinet/in.h>
#include <arpa/inet.h>

#define URING_ENTRIES 1024

/* ---- SQE user_data tag: kind in bits 60..63 ---- */
#define URING_KIND_WATCH   1ULL  /* payload: fd(28b)<<24 | gen(16b)<<8 | dir(8b) */
#define URING_KIND_TIMEOUT 3ULL  /* payload: 0 */
#define URING_KIND_CANCEL  4ULL  /* payload: slot(22b)<<32 | gen(16b) */
/* kinds stay <= 5, so bit63 is 0 and never collides with MCO_ASYNC_SYNC_DONE. */
#define URING_KIND_ASYNC   5ULL  /* payload: slot(22b)<<32 | gen(16b) */

#define TAG_OF(kind, payload) (((uint64_t)(kind) << 60) | (uint64_t)(payload))

static uint64_t watch_tag(int fd, unsigned gen, int dir) {
    return TAG_OF(URING_KIND_WATCH,
                  ((uint64_t)(fd & 0xFFFFFFF) << 24) | ((gen & 0xFFFF) << 8) |
                  (uint64_t)(dir ? 1u : 0u));
}
static uint64_t async_tag(unsigned slot, unsigned gen) {
    return TAG_OF(URING_KIND_ASYNC, ((uint64_t)slot << 32) | (gen & 0xFFFF));
}
static uint64_t cancel_tag(unsigned slot, unsigned gen) {
    return TAG_OF(URING_KIND_CANCEL, ((uint64_t)slot << 32) | (gen & 0xFFFF));
}

typedef struct mco_ev_watch {
    mco_engine_cb read_cb, write_cb;
    void*         read_ud,  *write_ud;
    /* Directions armed in the kernel. POLL_ADD is one-shot, like EPOLLONESHOT. */
    int           armed_mask;
    /* Bumped when a foreign registration overwrites: stale POLL CQEs are void,
     * since io_uring does not remove polls on close. */
    unsigned      gen;
} mco_ev_watch_t;

/* Engine-side record for an async op; one slot per in-flight SQE (SQ-sized). */
typedef struct mco_async_slot {
    int      in_use;
    int      fd;
    unsigned gen;
    mco_engine_async_cb cb;
    void*    ud;
    void*    addr_copy;   /* CONNECT sockaddr copy (source may die while buffered) */
} mco_async_slot_t;

typedef struct mco_uring_engine {
    mco_uring_t       ring;
    int               wake_fd[2];
    mco_ev_watch_t*   watches;
    mco_async_slot_t* slots;
    int*              slot_free;    /* free slot index stack */
    uint64_t*         cancelq;      /* cancels staged when the SQ is full */
    struct mco_kernel_timespec poll_ts;  /* stable address for TIMEOUT/EXT_ARG */
    int               watches_cap;
    int               slots_cap;
    int               slot_free_top;
    int               cancelq_len;
    int               cancelq_cap;
    int               dispatch_count;
    int               has_ext_arg; /* EXT_ARG (5.11+): kernel-side timeout */
    int               has_async;    /* READ/WRITE + ASYNC_CANCEL available */
    int               has_accept;   /* kernel parks ACCEPT on non-blocking sockets */
    int               has_connect;  /* kernel parks CONNECT on non-blocking sockets */
} mco_uring_engine_t;

/* Probed once per process; a concurrent first race may repeat it harmlessly. */
static int g_uring_probe_async = -1;
static int g_uring_probe_accept = -1;
static int g_uring_probe_connect = -1;

static int watches_ensure(mco_uring_engine_t* e, int fd) {
    if (fd < 0) return -1;
    if (fd >= e->watches_cap) {
        int newcap = e->watches_cap > 0 ? e->watches_cap : 64;
        mco_ev_watch_t* nw;
        while (newcap <= fd) newcap *= 2;
        nw = (mco_ev_watch_t*)realloc(e->watches, sizeof(mco_ev_watch_t) * newcap);
        if (!nw) return -1;
        memset(nw + e->watches_cap, 0, sizeof(mco_ev_watch_t) * (newcap - e->watches_cap));
        e->watches = nw;
        e->watches_cap = newcap;
    }
    return 0;
}

/* Get an SQE; flush once when full, then NULL if still full. */
static struct mco_io_uring_sqe* sqe_alloc(mco_uring_engine_t* e) {
    struct mco_io_uring_sqe* sqe = mco_uring_sqe_get(&e->ring);
    if (!sqe) {
        mco_uring_submit(&e->ring);
        sqe = mco_uring_sqe_get(&e->ring);
    }
    return sqe;
}

static int slot_alloc(mco_uring_engine_t* e) {
    if (e->slot_free_top == 0) return -1;
    return e->slot_free[--e->slot_free_top];
}

static void slot_release(mco_uring_engine_t* e, mco_async_slot_t* s) {
    int idx = (int)(s - e->slots);
    if (s->addr_copy) { free(s->addr_copy); s->addr_copy = NULL; }
    s->in_use = 0;
    s->cb = NULL;
    s->ud = NULL;
    s->gen++;   /* roll the generation: stale tags (incl. cancels) are void */
    e->slot_free[e->slot_free_top++] = idx;
}

static int fill_poll_sqe(struct mco_io_uring_sqe* sqe, int fd, int dir, unsigned gen) {
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = MCO_IORING_OP_POLL_ADD;
    sqe->fd = fd;
    sqe->op_flags = dir ? POLLOUT : POLLIN;
    sqe->user_data = watch_tag(fd, gen, dir);
    return 0;
}

static void uring_destroy_engine(mco_engine_t* eng);  /* for create's cleanup path */

/* ---- CQE dispatch (reap cb, run inside poll()) ---- */

static void on_cqe_dispatch(void* ud, const struct mco_io_uring_cqe* cqe) {
    mco_uring_engine_t* e = (mco_uring_engine_t*)ud;
    uint64_t tag = cqe->user_data;
    unsigned kind = (unsigned)(tag >> 60);
    int res = cqe->res;

    switch (kind) {
    case URING_KIND_ASYNC: {
        unsigned slot = (unsigned)((tag >> 32) & 0x3FFFFF);
        unsigned gen = (unsigned)(tag & 0xFFFF);
        mco_async_slot_t* s;
        if (slot >= (unsigned)e->slots_cap) return;
        s = &e->slots[slot];
        if (!s->in_use || (s->gen & 0xFFFF) != gen) return;  /* stale */
        if (s->cb) s->cb(s->ud, s->fd, res);
        slot_release(e, s);
        e->dispatch_count++;
        return;
    }
    case URING_KIND_WATCH: {
        int fd = (int)((tag >> 24) & 0xFFFFFFF);
        unsigned gen = (unsigned)((tag >> 8) & 0xFFFF);
        int dir = (int)(tag & 0xFF);
        mco_ev_watch_t* w;
        int bit, ev;

        if (fd < 0 || fd >= e->watches_cap) return;
        w = &e->watches[fd];
        if ((w->gen & 0xFFFF) != gen) return;  /* poll from before fd reuse: void */
        bit = dir ? MCO_EV_WRITE : MCO_EV_READ;
        w->armed_mask &= ~bit;   /* one-shot consumed; the cb may re-ctl */
        if (res < 0) return;     /* cancelled by REMOVE: nothing to dispatch */
        ev = bit;
        if (res & (POLLERR | POLLHUP)) ev |= MCO_EV_ERROR;
        {
            mco_engine_cb cb = dir ? w->write_cb : w->read_cb;
            void* u = dir ? w->write_ud : w->read_ud;
            if (cb) { e->dispatch_count++; cb(u, fd, ev); }
        }
        return;
    }
    default:
        /* Ignore TIMEOUT (stale early wake) and CANCEL (receipt) CQEs. */
        return;
    }
}

/* ---- Capability probe (cached once per process) ----
 * Kernels differ on ACCEPT/CONNECT over non-blocking sockets: some park the op
 * (what we need), others complete at once with EAGAIN/EALREADY (fast path
 * useless). Each op is submitted with a 30ms TIMEOUT to see which happens. */

static void probe_cancel_helper(mco_uring_engine_t* e, uint64_t cookie) {
    struct mco_io_uring_sqe* sqe = sqe_alloc(e);
    if (!sqe) return;
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = MCO_IORING_OP_ASYNC_CANCEL;
    sqe->addr = cookie;
    sqe->user_data = cancel_tag((unsigned)((cookie >> 32) & 0x3FFFFF),
                                (unsigned)(cookie & 0xFFFF));
}

/* Read an empty pipe + ASYNC_CANCEL: READ must complete with -ECANCELED. */
static int probe_async_support(mco_uring_engine_t* e) {
    struct mco_io_uring_sqe* sqe;
    struct mco_io_uring_cqe cqe;
    char buf[16];
    int slot, got_op = 0, got_cancel = 0, op_res = -1, cancel_res = -1;
    uint64_t cookie;

    slot = slot_alloc(e);
    if (slot < 0) return 0;
    sqe = sqe_alloc(e);
    if (!sqe) { e->slot_free[e->slot_free_top++] = slot; return 0; }
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = MCO_IORING_OP_READ;
    sqe->fd = e->wake_fd[0];   /* empty pipe: READ parks, then is cancelled */
    sqe->addr = (uint64_t)(uintptr_t)buf;
    sqe->len = (uint32_t)sizeof(buf);
    sqe->user_data = async_tag((unsigned)slot, e->slots[slot].gen);
    e->slots[slot].in_use = 1;
    e->slots[slot].fd = e->wake_fd[0];
    e->slots[slot].cb = NULL;  /* probe bypasses on_cqe_dispatch */
    e->slots[slot].ud = NULL;
    cookie = async_tag((unsigned)slot, e->slots[slot].gen);

    probe_cancel_helper(e, cookie);
    if (mco_uring_submit(&e->ring) < 0) { slot_release(e, &e->slots[slot]); return 0; }

    while (got_op + got_cancel < 2) {
        if (mco_uring_wait_one(&e->ring, &cqe, -1) <= 0) break;
        if (cqe.user_data == cookie) { got_op = 1; op_res = cqe.res; }
        else if ((cqe.user_data >> 60) == URING_KIND_CANCEL) { got_cancel = 1; cancel_res = cqe.res; }
    }
    if (e->slots[slot].in_use) slot_release(e, &e->slots[slot]);
    return (got_op && op_res == -ECANCELED && got_cancel && cancel_res >= 0) ? 1 : 0;
}

/* Wait for the op CQE or the TIMEOUT CQE, whichever comes first.
 * 1 = the kernel parked the op (capability available); 0 = completed at once.
 * A second TIMEOUT guards against a broken ASYNC_CANCEL on very old kernels. */
static int probe_wait_for(mco_uring_engine_t* e, uint64_t cookie, int* out_connfd) {
    struct mco_io_uring_sqe* sqe;
    struct mco_io_uring_cqe cqe;
    int ok = 0, timeouts = 0;

    for (;;) {
        sqe = sqe_alloc(e);
        if (!sqe) return ok;
        memset(sqe, 0, sizeof(*sqe));
        sqe->opcode = MCO_IORING_OP_TIMEOUT;
        sqe->fd = -1;
        sqe->addr = (uint64_t)(uintptr_t)&e->poll_ts;
        sqe->user_data = TAG_OF(URING_KIND_TIMEOUT, 0);
        e->poll_ts.tv_sec = 0;
        e->poll_ts.tv_nsec = 30000000;  /* 30ms */
        sqe->len = 1;  /* len=0 for a pure timeout is -EINVAL on new kernels */
        if (mco_uring_submit(&e->ring) < 0) return ok;

        for (;;) {
            if (mco_uring_wait_one(&e->ring, &cqe, -1) <= 0) return ok;
            if (cqe.user_data == cookie) {
                if (out_connfd && cqe.res >= 0) *out_connfd = cqe.res;
                if (timeouts == 0) {
                    /* Completed before the timeout: res>=0 means the kernel
                     * really waited for readiness (poll-retry works). */
                    return (cqe.res >= 0) ? 1 : 0;
                }
                return 1;  /* cancelled or completed: it stayed parked */
            }
            if ((cqe.user_data >> 60) == URING_KIND_TIMEOUT) {
                if (timeouts == 0) {
                    /* not done in 30ms: the kernel parks it; cancel now */
                    probe_cancel_helper(e, cookie);
                    mco_uring_submit(&e->ring);
                    ok = 1;
                } else {
                    return ok;  /* cancel broken: stop waiting */
                }
                timeouts++;
                break;  /* re-arm a timeout */
            }
            /* Ignore the CANCEL receipt; wait for the op's -ECANCELED */
        }
    }
}

/* Submit ACCEPT on an idle non-blocking unix listener: it must not complete. */
static int probe_accept_support(mco_uring_engine_t* e) {
    struct mco_io_uring_sqe* sqe;
    struct sockaddr_un addr;
    int slot, lfd, connfd = -1, ok;

    lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (lfd < 0) return 0;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = 0;  /* abstract namespace: no filesystem entry */
    snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1, "mco-probe-a-%d", (int)getpid());
    if (bind(lfd, (struct sockaddr*)&addr, sizeof(addr.sun_family) + 1 + strlen(addr.sun_path + 1)) != 0 ||
        listen(lfd, 1) != 0) {
        close(lfd);
        return 0;
    }

    slot = slot_alloc(e);
    if (slot < 0) { close(lfd); return 0; }
    sqe = sqe_alloc(e);
    if (!sqe) { e->slot_free[e->slot_free_top++] = slot; close(lfd); return 0; }
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = MCO_IORING_OP_ACCEPT;
    sqe->fd = lfd;   /* addr/off left at 0 */
    sqe->user_data = async_tag((unsigned)slot, e->slots[slot].gen);
    e->slots[slot].in_use = 1;
    e->slots[slot].fd = lfd;
    e->slots[slot].cb = NULL;
    e->slots[slot].ud = NULL;

    ok = probe_wait_for(e, async_tag((unsigned)slot, e->slots[slot].gen), &connfd);
    if (e->slots[slot].in_use) slot_release(e, &e->slots[slot]);
    if (connfd >= 0) close(connfd);
    close(lfd);
    return ok;
}

/* TCP loopback: after a non-blocking connect() gives EINPROGRESS, submit
 * CONNECT — it must not complete at once with EALREADY/EINPROGRESS. */
static int probe_connect_support(mco_uring_engine_t* e) {
    struct mco_io_uring_sqe* sqe;
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    int slot, lfd, cfd, ret, ok;

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) return 0;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(lfd, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(lfd, 1) != 0 ||
        getsockname(lfd, (struct sockaddr*)&addr, &alen) != 0) {
        close(lfd);
        return 0;
    }

    cfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (cfd < 0) { close(lfd); return 0; }
    ret = connect(cfd, (struct sockaddr*)&addr, sizeof(addr));
    if (ret != 0 && errno != EINPROGRESS) {  /* cannot build the probe */
        close(cfd); close(lfd);
        return 0;
    }

    slot = slot_alloc(e);
    if (slot < 0) { close(cfd); close(lfd); return 0; }
    sqe = sqe_alloc(e);
    if (!sqe) {
        e->slot_free[e->slot_free_top++] = slot;
        close(cfd); close(lfd);
        return 0;
    }
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = MCO_IORING_OP_CONNECT;
    sqe->fd = cfd;
    sqe->addr = (uint64_t)(uintptr_t)&addr;
    sqe->off = (uint64_t)alen;
    sqe->user_data = async_tag((unsigned)slot, e->slots[slot].gen);
    e->slots[slot].in_use = 1;
    e->slots[slot].fd = cfd;
    e->slots[slot].cb = NULL;
    e->slots[slot].ud = NULL;

    ok = probe_wait_for(e, async_tag((unsigned)slot, e->slots[slot].gen), NULL);
    if (e->slots[slot].in_use) slot_release(e, &e->slots[slot]);
    close(cfd);
    close(lfd);
    return ok;
}

/* ---- engine lifetime ---- */

static mco_engine_t* uring_create_engine(void) {
    mco_uring_engine_t* e = (mco_uring_engine_t*)calloc(1, sizeof(mco_uring_engine_t));
    struct mco_io_uring_sqe* sqe;
    int i;

    if (!e) return NULL;
    if (mco_uring_create(&e->ring, URING_ENTRIES) != 0) {
        free(e);
        return NULL;
    }
    e->has_ext_arg = (e->ring.features & MCO_IORING_FEAT_EXT_ARG) != 0;

    if (pipe2(e->wake_fd, O_NONBLOCK | O_CLOEXEC) != 0) {
        mco_uring_destroy(&e->ring);
        free(e);
        return NULL;
    }
    if (watches_ensure(e, e->wake_fd[0]) != 0) goto fail;

    e->slots_cap = URING_ENTRIES;
    e->slots = (mco_async_slot_t*)calloc(1, sizeof(mco_async_slot_t) * URING_ENTRIES);
    e->slot_free = (int*)malloc(sizeof(int) * URING_ENTRIES);
    if (!e->slots || !e->slot_free) goto fail;
    for (i = 0; i < URING_ENTRIES; ++i) e->slot_free[i] = URING_ENTRIES - 1 - i;
    e->slot_free_top = URING_ENTRIES;

    e->cancelq_cap = 16;
    e->cancelq = (uint64_t*)malloc(sizeof(uint64_t) * e->cancelq_cap);
    if (!e->cancelq) goto fail;
    e->cancelq_len = 0;

    /* Standing POLL_ADD on the wake pipe (submit now, do not wait for poll).
     * wake() writes the pipe, which completes this POLL and wakes enter_wait();
     * the WATCH case consumes the CQE (read_cb is NULL, so it dispatches
     * nothing). It is one-shot, which suffices for the single mco_vcpu_stop(). */
    sqe = sqe_alloc(e);
    if (!sqe) goto fail;
    fill_poll_sqe(sqe, e->wake_fd[0], 0, 0);
    mco_uring_submit(&e->ring);

    /* Capability probes (cached per process, 30ms each) */
    if (g_uring_probe_async < 0) g_uring_probe_async = probe_async_support(e);
    if (g_uring_probe_accept < 0) g_uring_probe_accept = probe_accept_support(e);
    if (g_uring_probe_connect < 0) g_uring_probe_connect = probe_connect_support(e);
    e->has_async = g_uring_probe_async;
    e->has_accept = g_uring_probe_accept;
    e->has_connect = g_uring_probe_connect;
    return (mco_engine_t*)e;

fail:
    uring_destroy_engine((mco_engine_t*)e);
    return NULL;
}

static void uring_destroy_engine(mco_engine_t* eng) {
    mco_uring_engine_t* e = (mco_uring_engine_t*)eng;
    int i;

    if (!e) return;
    /* Close the pipe write end to unblock poll, then tear the ring down:
     * closing ring_fd cancels all in-flight requests. */
    close(e->wake_fd[1]);
    close(e->wake_fd[0]);
    for (i = 0; i < e->slots_cap; ++i) {
        if (e->slots[i].addr_copy) free(e->slots[i].addr_copy);
    }
    free(e->slots);
    free(e->slot_free);
    free(e->cancelq);
    free(e->watches);
    mco_uring_destroy(&e->ring);
    free(e);
}

/* ---- Readiness ctl/poll/wake (same semantics as the epoll backend) ---- */

static int uring_ctl_engine(mco_engine_t* eng, int fd, int events, mco_engine_cb cb, void* ud) {
    mco_uring_engine_t* e = (mco_uring_engine_t*)eng;
    mco_ev_watch_t* w;
    int dirty = 0, new_mask, d;

    if (watches_ensure(e, fd) != 0) return -1;
    w = &e->watches[fd];

    /* Registration identity check (by ud): a late unregister after fd reuse must
     * not clear the new registration; a foreign overwrite rolls gen and forces a
     * resync. */
    if (events & MCO_EV_READ) {
        if (cb) {
            if (w->read_ud != ud) dirty = 1;
            w->read_cb = cb; w->read_ud = ud;
        } else if (w->read_ud == ud) {
            w->read_cb = NULL; w->read_ud = NULL;
        }
        /* slot changed hands: skip entirely */
    }
    if (events & MCO_EV_WRITE) {
        if (cb) {
            if (w->write_ud != ud) dirty = 1;
            w->write_cb = cb; w->write_ud = ud;
        } else if (w->write_ud == ud) {
            w->write_cb = NULL; w->write_ud = NULL;
        }
    }
    if (dirty) {
        w->gen++;
        w->armed_mask = 0;
    }

    new_mask = (w->read_cb ? MCO_EV_READ : 0) | (w->write_cb ? MCO_EV_WRITE : 0);
    /* Lazy unregister: no SQE when the kernel state already matches (armed is 0
     * after a one-shot event). */
    if (new_mask == w->armed_mask) return 0;

    for (d = 0; d < 2; ++d) {
        int bit = d ? MCO_EV_WRITE : MCO_EV_READ;
        if ((w->armed_mask & bit) && !(new_mask & bit)) {
            /* POLL_REMOVE with the same tag as the poll; its receipt CQE just
             * clears armed. */
            struct mco_io_uring_sqe* sqe = sqe_alloc(e);
            if (!sqe) return -1;
            memset(sqe, 0, sizeof(*sqe));
            sqe->opcode = MCO_IORING_OP_POLL_REMOVE;
            sqe->fd = fd;
            sqe->addr = watch_tag(fd, w->gen, d);
            sqe->user_data = watch_tag(fd, w->gen, d);
            w->armed_mask &= ~bit;
        }
    }
    for (d = 0; d < 2; ++d) {
        int bit = d ? MCO_EV_WRITE : MCO_EV_READ;
        if ((new_mask & bit) && !(w->armed_mask & bit)) {
            struct mco_io_uring_sqe* sqe = sqe_alloc(e);
            if (!sqe) return -1;
            fill_poll_sqe(sqe, fd, d, w->gen);
            w->armed_mask |= bit;
        }
    }
    return 0;
}

static void flush_cancels(mco_uring_engine_t* e) {
    int had = e->cancelq_len;
    int i = 0;
    while (i < e->cancelq_len) {
        struct mco_io_uring_sqe* sqe = mco_uring_sqe_get(&e->ring);
        if (!sqe) {
            mco_uring_submit(&e->ring);  /* make room; a flush always progresses */
            continue;
        }
        memset(sqe, 0, sizeof(*sqe));
        sqe->opcode = MCO_IORING_OP_ASYNC_CANCEL;
        sqe->addr = e->cancelq[i];
        sqe->user_data = cancel_tag((unsigned)((e->cancelq[i] >> 32) & 0x3FFFFF),
                                    (unsigned)(e->cancelq[i] & 0xFFFF));
        i++;
    }
    e->cancelq_len = 0;
    /* cancel has no natural completion path (a DRAIN coroutine waits for its
     * -ECANCELED), so submit it immediately; this is a cold path anyway. */
    if (had) (void)mco_uring_submit(&e->ring);
}

static int uring_poll_engine(mco_engine_t* eng, int timeout_ms) {
    mco_uring_engine_t* e = (mco_uring_engine_t*)eng;
    int fired, n;

    flush_cancels(e);

    e->dispatch_count = 0;
    n = mco_uring_reap_all(&e->ring, on_cqe_dispatch, e);
    if (n < 0) return -1;
    fired = e->dispatch_count;

    if (fired == 0 && timeout_ms != 0) {
        struct mco_io_uring_cqe cqe;
        int r;
        const struct mco_kernel_timespec* ts = NULL;
        /* One enter does "submit + wait": buffered SQEs go down, then block for
         * one event. Timeout: EXT_ARG when the SQ is non-empty (built in, no
         * extra CQE), else a TIMEOUT SQE — some kernels EINVAL on
         * to_submit=0 with EXT_ARG. */
        if (timeout_ms >= 0) {
            e->poll_ts.tv_sec = timeout_ms / 1000;
            e->poll_ts.tv_nsec = (int64_t)(timeout_ms % 1000) * 1000000;
            if (e->has_ext_arg && mco_uring_pending(&e->ring) > 0) {
                ts = &e->poll_ts;
            } else {
                struct mco_io_uring_sqe* sqe = sqe_alloc(e);
                if (sqe) {
                    memset(sqe, 0, sizeof(*sqe));
                    sqe->opcode = MCO_IORING_OP_TIMEOUT;
                    sqe->fd = -1;
                    sqe->addr = (uint64_t)(uintptr_t)&e->poll_ts;
                    sqe->len = 1;  /* len=0 for a pure timeout is -EINVAL */
                    sqe->user_data = TAG_OF(URING_KIND_TIMEOUT, 0);
                }
                /* Cannot arm a timeout: wait forever for an event to wake us. */
            }
        }
        r = mco_uring_enter_wait(&e->ring, &cqe, timeout_ms, ts);
        if (r < 0) return -1;
        if (r == 1) on_cqe_dispatch(e, &cqe);
        n = mco_uring_reap_all(&e->ring, on_cqe_dispatch, e);
        if (n < 0) return -1;
        fired = e->dispatch_count;
    }
    return fired;
}

static void uring_wake_engine(mco_engine_t* eng) {
    mco_uring_engine_t* e = (mco_uring_engine_t*)eng;
    char b = 1;
    (void)!write(e->wake_fd[1], &b, 1);  /* failure is fine: pipe full = wake already pending */
}

/* ---- completion-style async ops ---- */

static uint64_t uring_submit_read(mco_engine_t* eng, int fd, void* buf, int len,
                                  mco_engine_async_cb cb, void* ud) {
    mco_uring_engine_t* e = (mco_uring_engine_t*)eng;
    struct mco_io_uring_sqe* sqe;
    int slot;

    if (!e->has_async) return 0;
    slot = slot_alloc(e);
    if (slot < 0) return 0;
    sqe = sqe_alloc(e);
    if (!sqe) { e->slot_free[e->slot_free_top++] = slot; return 0; }
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = MCO_IORING_OP_READ;
    sqe->fd = fd;
    sqe->addr = (uint64_t)(uintptr_t)buf;
    sqe->len = (uint32_t)len;
    sqe->user_data = async_tag((unsigned)slot, e->slots[slot].gen);
    e->slots[slot].in_use = 1;
    e->slots[slot].fd = fd;
    e->slots[slot].cb = cb;
    e->slots[slot].ud = ud;
    return async_tag((unsigned)slot, e->slots[slot].gen);
}

static uint64_t uring_submit_write(mco_engine_t* eng, int fd, const void* buf, int len,
                                   mco_engine_async_cb cb, void* ud) {
    mco_uring_engine_t* e = (mco_uring_engine_t*)eng;
    struct mco_io_uring_sqe* sqe;
    int slot;

    if (!e->has_async) return 0;
    slot = slot_alloc(e);
    if (slot < 0) return 0;
    sqe = sqe_alloc(e);
    if (!sqe) { e->slot_free[e->slot_free_top++] = slot; return 0; }
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = MCO_IORING_OP_WRITE;
    sqe->fd = fd;
    sqe->addr = (uint64_t)(uintptr_t)buf;
    sqe->len = (uint32_t)len;
    sqe->user_data = async_tag((unsigned)slot, e->slots[slot].gen);
    e->slots[slot].in_use = 1;
    e->slots[slot].fd = fd;
    e->slots[slot].cb = cb;
    e->slots[slot].ud = ud;
    return async_tag((unsigned)slot, e->slots[slot].gen);
}

static uint64_t uring_submit_connect(mco_engine_t* eng, int fd, const void* addr, unsigned addrlen,
                                     mco_engine_async_cb cb, void* ud) {
    mco_uring_engine_t* e = (mco_uring_engine_t*)eng;
    struct mco_io_uring_sqe* sqe;
    int slot;
    void* copy;

    if (!e->has_async || !e->has_connect) return 0;
    copy = malloc(addrlen);
    if (!copy) return 0;
    memcpy(copy, addr, addrlen);  /* copy: the source (coroutine stack) may die */
    slot = slot_alloc(e);
    if (slot < 0) { free(copy); return 0; }
    sqe = sqe_alloc(e);
    if (!sqe) { e->slot_free[e->slot_free_top++] = slot; free(copy); return 0; }
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = MCO_IORING_OP_CONNECT;
    sqe->fd = fd;
    sqe->addr = (uint64_t)(uintptr_t)copy;
    sqe->off = (uint64_t)addrlen;   /* CONNECT: off holds the address length */
    sqe->user_data = async_tag((unsigned)slot, e->slots[slot].gen);
    e->slots[slot].in_use = 1;
    e->slots[slot].fd = fd;
    e->slots[slot].cb = cb;
    e->slots[slot].ud = ud;
    e->slots[slot].addr_copy = copy;
    return async_tag((unsigned)slot, e->slots[slot].gen);
}

static uint64_t uring_submit_accept(mco_engine_t* eng, int fd, mco_engine_async_cb cb, void* ud) {
    mco_uring_engine_t* e = (mco_uring_engine_t*)eng;
    struct mco_io_uring_sqe* sqe;
    int slot;

    if (!e->has_async || !e->has_accept) return 0;
    slot = slot_alloc(e);
    if (slot < 0) return 0;
    sqe = sqe_alloc(e);
    if (!sqe) { e->slot_free[e->slot_free_top++] = slot; return 0; }
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = MCO_IORING_OP_ACCEPT;
    sqe->fd = fd;
    /* addr/off left at 0: accept4(fd, NULL, NULL, 0), no external sockaddr */
    sqe->user_data = async_tag((unsigned)slot, e->slots[slot].gen);
    e->slots[slot].in_use = 1;
    e->slots[slot].fd = fd;
    e->slots[slot].cb = cb;
    e->slots[slot].ud = ud;
    return async_tag((unsigned)slot, e->slots[slot].gen);
}

static int uring_submit_cancel(mco_engine_t* eng, uint64_t cookie, void* ud) {
    mco_uring_engine_t* e = (mco_uring_engine_t*)eng;
    unsigned slot = (unsigned)((cookie >> 32) & 0x3FFFFF);
    unsigned gen = (unsigned)(cookie & 0xFFFF);
    struct mco_io_uring_sqe* sqe;

    if ((cookie >> 60) != URING_KIND_ASYNC) return 0;
    if ((int)slot >= e->slots_cap) return 0;
    {
        mco_async_slot_t* s = &e->slots[slot];
        /* No-op if the op already finished (slot free/reused/gen rolled) */
        if (!s->in_use || s->ud != ud || (s->gen & 0xFFFF) != gen) return 0;
    }
    sqe = sqe_alloc(e);
    if (!sqe) {
        /* SQ full: queue it and flush at poll() start, before the next block. */
        if (e->cancelq_len == e->cancelq_cap) {
            int newcap = e->cancelq_cap * 2;
            uint64_t* nq = (uint64_t*)realloc(e->cancelq, sizeof(uint64_t) * newcap);
            if (!nq) return -1;
            e->cancelq = nq;
            e->cancelq_cap = newcap;
        }
        e->cancelq[e->cancelq_len++] = cookie;
        return 0;
    }
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = MCO_IORING_OP_ASYNC_CANCEL;
    sqe->addr = cookie;
    sqe->user_data = cancel_tag(slot, gen);
    return 0;
}

static const struct mco_engine_async_ops s_uring_async_ops = {
    uring_submit_read,
    uring_submit_write,
    uring_submit_connect,
    uring_submit_accept,
    uring_submit_cancel
};

static const mco_engine_ops_t s_uring_ops = {
    "io_uring",
    uring_create_engine,
    uring_destroy_engine,
    uring_ctl_engine,
    uring_poll_engine,
    uring_wake_engine,
    &s_uring_async_ops
};

const mco_engine_ops_t* mco_engine_uring_ops(void) { return &s_uring_ops; }

#endif /* __linux__ */
