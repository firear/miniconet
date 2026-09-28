/*
 * mco_engine_kqueue.c — macOS / *BSD kqueue backend.
 * kqueue registers EVFILT_READ/EVFILT_WRITE as two independent filters, mapping
 * directly onto the "separate read/write slots" contract. ctl() diffs the state
 * and issues incremental EV_ADD / EV_DELETE.
 */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)

#include "mco_engine.h"
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct mco_kq_watch {
    mco_engine_cb read_cb,  write_cb;
    void*         read_ud,  *write_ud;
    /* Per-direction arm state: EV_ONESHOT removes the knote on dispatch, so the
     * later EV_DELETE is a no-op. */
    unsigned char armed_read;
    unsigned char armed_write;
} mco_kq_watch_t;

struct mco_engine {
    mco_kq_watch_t* watches;   /* indexed by fd */
    int wake_fd[2];
    int kq;
    int watches_cap;
};

static int watches_ensure(mco_engine_t* e, int fd) {
    if (fd < 0) return -1;
    if (fd >= e->watches_cap) {
        int newcap = e->watches_cap > 0 ? e->watches_cap : 64;
        mco_kq_watch_t* nw;
        while (newcap <= fd) newcap *= 2;
        nw = (mco_kq_watch_t*)realloc(e->watches, sizeof(mco_kq_watch_t) * newcap);
        if (!nw) return -1;
        memset(nw + e->watches_cap, 0, sizeof(mco_kq_watch_t) * (newcap - e->watches_cap));
        e->watches = nw;
        e->watches_cap = newcap;
    }
    return 0;
}

static void on_wake_drain(void* ud, int fd, int revents) {
    char buf[64];
    (void)ud; (void)revents;
    while (read(fd, buf, sizeof(buf)) > 0) { /* drain */ }
}

static mco_engine_t* kq_create(void) {
    mco_engine_t* e = (mco_engine_t*)calloc(1, sizeof(mco_engine_t));
    struct kevent kev;
    if (!e) return NULL;
    e->kq = kqueue();
    if (e->kq < 0) { free(e); return NULL; }
    if (pipe(e->wake_fd) != 0) { close(e->kq); free(e); return NULL; }
    fcntl(e->wake_fd[0], F_SETFL, fcntl(e->wake_fd[0], F_GETFL) | O_NONBLOCK);
    fcntl(e->wake_fd[1], F_SETFL, fcntl(e->wake_fd[1], F_GETFL) | O_NONBLOCK);

    if (watches_ensure(e, e->wake_fd[0]) != 0) {
        close(e->kq); close(e->wake_fd[0]); close(e->wake_fd[1]); free(e);
        return NULL;
    }
    EV_SET(&kev, e->wake_fd[0], EVFILT_READ, EV_ADD, 0, 0, NULL);
    kevent(e->kq, &kev, 1, NULL, 0, NULL);
    e->watches[e->wake_fd[0]].read_cb = on_wake_drain;
    e->watches[e->wake_fd[0]].armed_read = 1; /* bare ADD is a standing registration */
    return e;
}

static void kq_destroy(mco_engine_t* e) {
    if (!e) return;
    close(e->kq);
    close(e->wake_fd[0]);
    close(e->wake_fd[1]);
    free(e->watches);
    free(e);
}

static int kq_ctl(mco_engine_t* e, int fd, int events, mco_engine_cb cb, void* ud) {
    struct kevent changes[2];
    int n = 0;
    mco_kq_watch_t* w;
    int force_read = 0, force_write = 0;

    if (watches_ensure(e, fd) != 0) return -1;
    w = &e->watches[fd];

    /* Identity check: a stale unregister must not EV_DELETE the new knote, and a
     * foreign overwrite must force a re-arm. */
    if (events & MCO_EV_READ) {
        if (cb) {
            if (w->read_ud != ud) force_read = 1;
            w->read_cb = cb; w->read_ud = ud;
        } else if (w->read_ud == ud) {
            w->read_cb = NULL; w->read_ud = NULL;
        }
    }
    if (events & MCO_EV_WRITE) {
        if (cb) {
            if (w->write_ud != ud) force_write = 1;
            w->write_cb = cb; w->write_ud = ud;
        } else if (w->write_ud == ud) {
            w->write_cb = NULL; w->write_ud = NULL;
        }
    }

    /* Touch the kernel only when the arm state differs; after a one-shot dispatch
     * the wakeup unregister is free. */
    if (w->read_cb && (!w->armed_read || force_read)) {
        EV_SET(&changes[n++], fd, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, NULL);
        w->armed_read = 1;
    } else if (!w->read_cb && w->armed_read) {
        EV_SET(&changes[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        w->armed_read = 0;
    }
    if (w->write_cb && (!w->armed_write || force_write)) {
        EV_SET(&changes[n++], fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, NULL);
        w->armed_write = 1;
    } else if (!w->write_cb && w->armed_write) {
        EV_SET(&changes[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        w->armed_write = 0;
    }

    if (n > 0) {
        /* ENOENT is normal: the kernel dropped the filter on EOF/close. */
        if (kevent(e->kq, changes, n, NULL, 0, NULL) != 0 && errno != ENOENT) {
            return -1;
        }
    }
    return 0;
}

static int kq_poll(mco_engine_t* e, int timeout_ms) {
    struct kevent evs[256];
    struct timespec ts, *pts = NULL;
    int n, i, fired = 0;

    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
        pts = &ts;
    }

    n = kevent(e->kq, NULL, 0, evs, 256, pts);
    if (n < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }
    for (i = 0; i < n; ++i) {
        int fd = (int)evs[i].ident;
        int err_flag = (evs[i].flags & (EV_EOF | EV_ERROR)) ? MCO_EV_ERROR : 0;
        mco_kq_watch_t w;
        if (fd < 0 || fd >= e->watches_cap) continue;
        /* EV_ONESHOT removes this direction's knote; the other direction is a
         * separate knote and needs no re-arm. */
        if (evs[i].filter == EVFILT_READ) e->watches[fd].armed_read = 0;
        else                              e->watches[fd].armed_write = 0;
        w = e->watches[fd];
        if (evs[i].filter == EVFILT_READ && w.read_cb) {
            w.read_cb(w.read_ud, fd, MCO_EV_READ | err_flag);
            ++fired;
        } else if (evs[i].filter == EVFILT_WRITE && w.write_cb) {
            w.write_cb(w.write_ud, fd, MCO_EV_WRITE | err_flag);
            ++fired;
        }
    }
    return fired;
}

static void kq_wake(mco_engine_t* e) {
    char b = 1;
    (void)!write(e->wake_fd[1], &b, 1);
}

static const mco_engine_ops_t s_kqueue_ops = {
    "kqueue",
    kq_create,
    kq_destroy,
    kq_ctl,
    kq_poll,
    kq_wake
};

const mco_engine_ops_t* mco_engine_kqueue_ops(void) { return &s_kqueue_ops; }

#endif /* BSD/macOS */
