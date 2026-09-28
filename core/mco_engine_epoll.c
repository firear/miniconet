/*
 * mco_engine_epoll.c — Linux epoll backend.
 * Empty outside __linux__ so every mco_engine_*.c can go into the build.
 * Read/write are independent slots, see ctl() in mco_engine.h.
 */
#if defined(__linux__)

#include "mco_engine.h"
#include <sys/epoll.h>
#define __USE_GNU
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

typedef struct mco_ev_watch {
    mco_engine_cb read_cb,  write_cb;
    void*         read_ud,  *write_ud;
    /* Directions armed in the kernel; 0 = none. EPOLLONESHOT disarms on
     * dispatch, so a wait costs one ADD/MOD and the wakeup unregister is free.
     * in_kernel says the fd is still in the epoll set after disarm: then MOD is
     * required, since ADD would EEXIST. */
    int           armed_mask;
    int           in_kernel;
} mco_ev_watch_t;

struct mco_engine {
    mco_ev_watch_t* watches; /* indexed by fd */
    int epfd;
    int wake_fd[2];          /* [0]=read end (in epoll), [1]=write end */
    int watches_cap;
};

static int watches_ensure(mco_engine_t* e, int fd) {
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

static void on_wake_drain(void* ud, int fd, int revents) {
    char buf[64];
    (void)ud; (void)revents;
    while (read(fd, buf, sizeof(buf)) > 0) { /* drain */ }
}

static mco_engine_t* epoll_create_engine(void) {
    mco_engine_t* e = (mco_engine_t*)calloc(1, sizeof(mco_engine_t));
    if (!e) return NULL;
    e->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (e->epfd < 0) { free(e); return NULL; }
    if (pipe2(e->wake_fd, O_NONBLOCK | O_CLOEXEC) != 0) {
        close(e->epfd); free(e); return NULL;
    }
    if (watches_ensure(e, e->wake_fd[0]) != 0) {
        close(e->epfd); close(e->wake_fd[0]); close(e->wake_fd[1]); free(e);
        return NULL;
    }
    {
        struct epoll_event ev; 
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.fd = e->wake_fd[0];
        epoll_ctl(e->epfd, EPOLL_CTL_ADD, e->wake_fd[0], &ev);
        e->watches[e->wake_fd[0]].read_cb = on_wake_drain;
        e->watches[e->wake_fd[0]].read_ud = NULL;
    }
    return e;
}

static void epoll_destroy_engine(mco_engine_t* e) {
    if (!e) return;
    close(e->epfd);
    close(e->wake_fd[0]);
    close(e->wake_fd[1]);
    free(e->watches);
    free(e);
}

static int epoll_ctl_engine(mco_engine_t* e, int fd, int events, mco_engine_cb cb, void* ud) {
    mco_ev_watch_t* w;
    int new_mask, desired, dirty;
    struct epoll_event ev;

    if (watches_ensure(e, fd) != 0) return -1;
    w = &e->watches[fd];

    /* Identity check (by ud): the unregister happens after the coroutine
     * resumes, by which time the fd may have been closed and reused. A stale
     * unregister must not clear the new registration, and armed_mask carried
     * over is untrustworthy. */
    dirty = 0;
    if (events & MCO_EV_READ) {
        if (cb) {
            if (w->read_ud != ud) dirty = 1; /* foreign overwrite */
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
        /* MOD suffices while the fd is in the set; ENOENT falls back to ADD. Do
         * not clear in_kernel blindly, or every wait pays ADD/EEXIST + MOD. */
        w->armed_mask = 0;
    }

    new_mask = 0;
    if (w->read_cb)  new_mask |= EPOLLIN;
    if (w->write_cb) new_mask |= EPOLLOUT;

    /* Lazy unregister: no syscall when the kernel state already matches (after a
     * one-shot event armed_mask is 0). */
    desired = new_mask;
    if (desired == w->armed_mask) return 0;

    if (desired == 0) {
        /* DEL only while armed; ENOENT after close is ignored. */
        if (w->armed_mask) {
            epoll_ctl(e->epfd, EPOLL_CTL_DEL, fd, NULL);
            w->armed_mask = 0;
            w->in_kernel = 0;
        }
        return 0;
    }

    memset(&ev, 0, sizeof(ev));
    ev.events = desired | EPOLLONESHOT;
    ev.data.fd = fd;

    /* in_kernel picks ADD vs MOD so a wait costs one epoll_ctl instead of
     * ADD/EEXIST + MOD. Only a hint: after close/reuse MOD gets ENOENT and falls
     * back to ADD, so no event is lost. */
    if (!w->in_kernel) {
        int rc = epoll_ctl(e->epfd, EPOLL_CTL_ADD, fd, &ev);
        if (rc != 0 && errno == EEXIST)
            rc = epoll_ctl(e->epfd, EPOLL_CTL_MOD, fd, &ev);
        if (rc != 0) return -1;
        w->in_kernel = 1;
    } else {
        int rc = epoll_ctl(e->epfd, EPOLL_CTL_MOD, fd, &ev);
        if (rc != 0 && errno == ENOENT) {
            rc = epoll_ctl(e->epfd, EPOLL_CTL_ADD, fd, &ev);
            if (rc == 0) w->in_kernel = 1;
        }
        if (rc != 0) return -1;
    }
    w->armed_mask = desired;
    return 0;
}

static int epoll_poll_engine(mco_engine_t* e, int timeout_ms) {
    struct epoll_event evs[256];
    int n, i, fired = 0;

    n = epoll_wait(e->epfd, evs, 256, timeout_ms);
    if (n < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }
    for (i = 0; i < n; ++i) {
        int fd = evs[i].data.fd;
        int err_flag = (evs[i].events & (EPOLLERR | EPOLLHUP)) ? MCO_EV_ERROR : 0;

        if (fd < 0 || fd >= e->watches_cap) continue;
        /* One-shot: the kernel disarmed it; sync armed before the cb. */
        e->watches[fd].armed_mask = 0;
        /* Snapshot both directions' cb and ud: a cb may re-ctl() this fd and
         * rewrite the slot. */
        {
            mco_engine_cb rcb = e->watches[fd].read_cb;
            mco_engine_cb wcb = e->watches[fd].write_cb;
            void*          rud = e->watches[fd].read_ud;
            void*          wud = e->watches[fd].write_ud;

            if (rcb && ((evs[i].events & EPOLLIN) || err_flag)) {
                rcb(rud, fd, MCO_EV_READ | err_flag);
                ++fired;
            }
            if (wcb && ((evs[i].events & EPOLLOUT) || err_flag)) {
                wcb(wud, fd, MCO_EV_WRITE | err_flag);
                ++fired;
            }
        }

        /* Deliberately no re-arm: the woken coroutine resumes before the next
         * poll and re-arms via ctl, saving a MOD+DEL pair. Nothing is lost,
         * epoll is level-triggered. */
    }
    return fired;
}

static void epoll_wake_engine(mco_engine_t* e) {
    char b = 1;
    /* failure is fine: a full pipe means a wake is pending */
    (void)!write(e->wake_fd[1], &b, 1);
}

static const mco_engine_ops_t s_epoll_ops = {
    "epoll",
    epoll_create_engine,
    epoll_destroy_engine,
    epoll_ctl_engine,
    epoll_poll_engine,
    epoll_wake_engine,
    NULL
};

const mco_engine_ops_t* mco_engine_epoll_ops(void) { return &s_epoll_ops; }

#endif /* __linux__ */
