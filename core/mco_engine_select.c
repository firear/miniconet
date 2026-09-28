/*
 * mco_engine_select.c — select() fallback backend.
 * POSIX: fds are capped by FD_SETSIZE (usually 1024) and rejected beyond it
 * (select is UB on out-of-range fds); fd_set is rebuilt and scanned O(n) per
 * poll. Windows differs (array+count, FD_SETSIZE caps the count, nfds ignored),
 * hence the #ifdef split. Slots: see mco_engine.h.
 */
#include "mco_engine.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include "../compat/hsocket.h"
  #include "mco_engine_slot.h"
  #define MCO_SELECT_CLOSE(fd) closesocket((SOCKET)(fd))
#else
  #include <sys/select.h>
  #include <unistd.h>
  #include <fcntl.h>
  #define MCO_SELECT_CLOSE(fd) close(fd)
#endif

typedef struct mco_sel_watch {
    mco_engine_cb read_cb,  write_cb;
    void*         read_ud,  *write_ud;
    int           used;   /* valid slot (Windows array management) */
} mco_sel_watch_t;

struct mco_engine {
    mco_sel_watch_t* watches;
#ifdef _WIN32
    /* Windows: compact fd->slot table (swap mode, indexable) */
    mco_slot_table_t slots;
#else
    int watches_cap;      /* POSIX: indexed by fd */
    /* Highest fd with used==1. Bounds the per-poll fd_set rebuild so a single
     * high fd seen once does not make every later poll scan up to watches_cap. */
    int max_fd;
#endif
    int wake_fd[2];        /* self-pipe (Windows: emulated socket pair) */
};

/* ---- POSIX: indexed by fd, same logic as epoll minus kernel state ---- */
#ifndef _WIN32

static int watches_ensure(mco_engine_t* e, int fd) {
    if (fd < 0 || fd >= FD_SETSIZE) return -1; /* reject rather than corrupt fd_set */
    if (fd >= e->watches_cap) {
        int newcap = e->watches_cap > 0 ? e->watches_cap : 64;
        mco_sel_watch_t* nw;
        while (newcap <= fd) newcap *= 2;
        if (newcap > FD_SETSIZE) newcap = FD_SETSIZE;
        nw = (mco_sel_watch_t*)realloc(e->watches, sizeof(mco_sel_watch_t) * newcap);
        if (!nw) return -1;
        memset(nw + e->watches_cap, 0, sizeof(mco_sel_watch_t) * (newcap - e->watches_cap));
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

static mco_engine_t* select_create(void) {
    mco_engine_t* e = (mco_engine_t*)calloc(1, sizeof(mco_engine_t));
    if (!e) return NULL;
    if (pipe(e->wake_fd) != 0) { free(e); return NULL; }
    fcntl(e->wake_fd[0], F_SETFL, fcntl(e->wake_fd[0], F_GETFL) | O_NONBLOCK);
    fcntl(e->wake_fd[1], F_SETFL, fcntl(e->wake_fd[1], F_GETFL) | O_NONBLOCK);
    if (watches_ensure(e, e->wake_fd[0]) != 0) {
        close(e->wake_fd[0]); close(e->wake_fd[1]); free(e); return NULL;
    }
    e->watches[e->wake_fd[0]].read_cb = on_wake_drain;
    e->watches[e->wake_fd[0]].used = 1;
    e->max_fd = e->wake_fd[0];
    return e;
}

static void select_destroy(mco_engine_t* e) {
    if (!e) return;
    close(e->wake_fd[0]);
    close(e->wake_fd[1]);
    free(e->watches);
    free(e);
}

static int select_ctl(mco_engine_t* e, int fd, int events, mco_engine_cb cb, void* ud) {
    mco_sel_watch_t* w;
    if (watches_ensure(e, fd) != 0) return -1;
    w = &e->watches[fd];

    /* Identity check: a stale unregister must not clear the new registration. */
    if (events & MCO_EV_READ) {
        if (cb) { w->read_cb = cb; w->read_ud = ud; }
        else if (w->read_ud == ud) { w->read_cb = NULL; w->read_ud = NULL; }
    }
    if (events & MCO_EV_WRITE) {
        if (cb) { w->write_cb = cb; w->write_ud = ud; }
        else if (w->write_ud == ud) { w->write_cb = NULL; w->write_ud = NULL; }
    }
    w->used = (w->read_cb != NULL) || (w->write_cb != NULL);
    if (w->used) {
        if (fd > e->max_fd) e->max_fd = fd;
    } else if (fd == e->max_fd) {
        /* Shrink the scan bound. Amortized O(1): each slot is walked at most
         * once per shrink, and max_fd only drops until a new high fd arrives. */
        while (e->max_fd >= 0 && !e->watches[e->max_fd].used) e->max_fd--;
    }
    return 0;
}

static int select_poll(mco_engine_t* e, int timeout_ms) {
    fd_set rfds, wfds;
    int maxfd = -1, i, n, fired = 0;
    struct timeval tv, *ptv = NULL;

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    for (i = 0; i <= e->max_fd; ++i) {
        if (!e->watches[i].used) continue;
        if (e->watches[i].read_cb)  FD_SET(i, &rfds);
        if (e->watches[i].write_cb) FD_SET(i, &wfds);
        if (i > maxfd) maxfd = i;
    }
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }

    n = select(maxfd + 1, &rfds, &wfds, NULL, ptv);
    if (n < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }
    for (i = 0; i <= maxfd && fired < n * 2; ++i) {
        mco_sel_watch_t w;
        int is_r, is_w;
        if (!e->watches[i].used) continue;
        is_r = FD_ISSET(i, &rfds);
        is_w = FD_ISSET(i, &wfds);
        if (!is_r && !is_w) continue;
        w = e->watches[i];
        if (is_r && w.read_cb)  { w.read_cb(w.read_ud, i, MCO_EV_READ);  ++fired; }
        if (is_w && w.write_cb) { w.write_cb(w.write_ud, i, MCO_EV_WRITE); ++fired; }
    }
    return fired;
}

static void select_wake(mco_engine_t* e) {
    char b = 1;
    (void)!write(e->wake_fd[1], &b, 1);
}

#else /* _WIN32: compact fd->slot table (swap mode); SOCKET values are not indices */

static int sel_grow(mco_slot_table_t* t, int newcap, void* ud) {
    mco_engine_t* e = (mco_engine_t*)ud;
    mco_sel_watch_t* nw = (mco_sel_watch_t*)realloc(e->watches,
                                                    (size_t)newcap * sizeof(mco_sel_watch_t));
    if (!nw) return -1;
    e->watches = nw;
    return 0;
}

static void sel_swap(void* ud, int a, int b) {
    mco_engine_t* e = (mco_engine_t*)ud;
    e->watches[a] = e->watches[b];
}

static mco_engine_t* select_create(void) {
    mco_engine_t* e = (mco_engine_t*)calloc(1, sizeof(mco_engine_t));
    int sv[2];
    if (!e) return NULL;
    mco_slot_table_init(&e->slots, 0 /*swap*/, sel_grow, sel_swap, e);
    if (Socketpair(AF_INET, SOCK_STREAM, 0, sv) != 0) {
        mco_slot_table_free(&e->slots); free(e); return NULL;
    }
    e->wake_fd[0] = sv[0];
    e->wake_fd[1] = sv[1];
    nonblocking(e->wake_fd[0]);
    nonblocking(e->wake_fd[1]);
    return e;
}

static void select_destroy(mco_engine_t* e) {
    if (!e) return;
    MCO_SELECT_CLOSE(e->wake_fd[0]);
    MCO_SELECT_CLOSE(e->wake_fd[1]);
    mco_slot_table_free(&e->slots);
    free(e->watches);
    free(e);
}

static int select_ctl(mco_engine_t* e, int fd, int events, mco_engine_cb cb, void* ud) {
    int slot, is_new = 0;
    mco_sel_watch_t* w;
    if (mco_slot_ensure(&e->slots, fd, &slot, &is_new) != 0) return -1;
    if (is_new) memset(&e->watches[slot], 0, sizeof(e->watches[slot]));
    w = &e->watches[slot];

    /* Identity check: a stale unregister must not clear the new registration. */
    if (events & MCO_EV_READ) {
        if (cb) { w->read_cb = cb; w->read_ud = ud; }
        else if (w->read_ud == ud) { w->read_cb = NULL; w->read_ud = NULL; }
    }
    if (events & MCO_EV_WRITE) {
        if (cb) { w->write_cb = cb; w->write_ud = ud; }
        else if (w->write_ud == ud) { w->write_cb = NULL; w->write_ud = NULL; }
    }

    if (!w->read_cb && !w->write_cb) mco_slot_release(&e->slots, slot);
    return 0;
}

static int select_poll(mco_engine_t* e, int timeout_ms) {
    fd_set rfds, wfds;
    int i, n, fired = 0;
    struct timeval tv, *ptv = NULL;

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    for (i = 0; i < e->slots.n; ++i) {
        if (e->watches[i].read_cb)  FD_SET((SOCKET)e->slots.fds[i], &rfds);
        if (e->watches[i].write_cb) FD_SET((SOCKET)e->slots.fds[i], &wfds);
    }
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }

    /* Windows select() ignores the first arg */
    n = select(0, &rfds, &wfds, NULL, ptv);
    if (n == SOCKET_ERROR) return -1;
    for (i = 0; i < e->slots.n && fired < n * 2; ++i) {
        mco_sel_watch_t w;
        SOCKET s = (SOCKET)e->slots.fds[i];
        int is_r = FD_ISSET(s, &rfds);
        int is_w = FD_ISSET(s, &wfds);
        if (!is_r && !is_w) continue;
        w = e->watches[i];
        if (is_r && w.read_cb)  { w.read_cb(w.read_ud, e->slots.fds[i], MCO_EV_READ);  ++fired; }
        if (is_w && w.write_cb) { w.write_cb(w.write_ud, e->slots.fds[i], MCO_EV_WRITE); ++fired; }
    }
    return fired;
}

static void select_wake(mco_engine_t* e) {
    char b = 1;
    send((SOCKET)e->wake_fd[1], &b, 1, 0);
}

#endif /* _WIN32 */

static const mco_engine_ops_t s_select_ops = {
    "select",
    select_create,
    select_destroy,
    select_ctl,
    select_poll,
    select_wake,
    NULL
};

const mco_engine_ops_t* mco_engine_select_ops(void) { return &s_select_ops; }
