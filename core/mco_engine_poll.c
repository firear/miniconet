/*
 * mco_engine_poll.c — POSIX poll() backend.
 * No FD_SETSIZE limit and no bitmap rebuild, but still an O(n) scan. Not on
 * Windows (WSAPoll differs; use wepoll/select). Slots: see mco_engine.h.
 */
#if !defined(_WIN32)

#include "mco_engine.h"
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

typedef struct mco_poll_watch {
    mco_engine_cb read_cb,  write_cb;
    void*         read_ud,  *write_ud;
} mco_poll_watch_t;

struct mco_engine {
    struct pollfd*    pfds;      /* compact array; pfds[i].fd is valid */
    mco_poll_watch_t* watches;   /* one-to-one with pfds */
    int               n;         /* slots in use */
    int               cap;
    int*              index_of_fd; /* fd -> pfds index, -1 = unregistered */
    int               index_cap;
    int               wake_fd[2];
};

static int index_ensure(mco_engine_t* e, int fd) {
    if (fd < 0) return -1;
    if (fd >= e->index_cap) {
        int newcap = e->index_cap > 0 ? e->index_cap : 64;
        int* ni;
        int old = e->index_cap;
        int i;
        while (newcap <= fd) newcap *= 2;
        ni = (int*)realloc(e->index_of_fd, sizeof(int) * newcap);
        if (!ni) return -1;
        for (i = old; i < newcap; ++i) ni[i] = -1;
        e->index_of_fd = ni;
        e->index_cap = newcap;
    }
    return 0;
}

static int slot_ensure(mco_engine_t* e) {
    if (e->n >= e->cap) {
        int newcap = e->cap > 0 ? e->cap * 2 : 64;
        struct pollfd* np = (struct pollfd*)realloc(e->pfds, sizeof(struct pollfd) * newcap);
        mco_poll_watch_t* nw = (mco_poll_watch_t*)realloc(e->watches, sizeof(mco_poll_watch_t) * newcap);
        if (!np || !nw) return -1;
        e->pfds = np; e->watches = nw; e->cap = newcap;
    }
    return 0;
}

static void remove_slot(mco_engine_t* e, int slot) {
    int last = e->n - 1;
    if (slot != last) {
        e->pfds[slot] = e->pfds[last];
        e->watches[slot] = e->watches[last];
        e->index_of_fd[e->pfds[slot].fd] = slot;
    }
    e->n--;
}

static void on_wake_drain(void* ud, int fd, int revents) {
    char buf[64];
    (void)ud; (void)revents;
    while (read(fd, buf, sizeof(buf)) > 0) { /* drain */ }
}

static mco_engine_t* poll_create(void) {
    mco_engine_t* e = (mco_engine_t*)calloc(1, sizeof(mco_engine_t));
    if (!e) return NULL;
    if (pipe(e->wake_fd) != 0) { free(e); return NULL; }
    fcntl(e->wake_fd[0], F_SETFL, fcntl(e->wake_fd[0], F_GETFL) | O_NONBLOCK);
    fcntl(e->wake_fd[1], F_SETFL, fcntl(e->wake_fd[1], F_GETFL) | O_NONBLOCK);

    if (index_ensure(e, e->wake_fd[0]) != 0 || slot_ensure(e) != 0) {
        close(e->wake_fd[0]); close(e->wake_fd[1]); free(e); return NULL;
    }
    e->pfds[e->n].fd = e->wake_fd[0];
    e->pfds[e->n].events = POLLIN;
    e->pfds[e->n].revents = 0;
    e->watches[e->n].read_cb = on_wake_drain;
    e->watches[e->n].read_ud = NULL;
    e->index_of_fd[e->wake_fd[0]] = e->n;
    e->n++;
    return e;
}

static void poll_destroy(mco_engine_t* e) {
    if (!e) return;
    close(e->wake_fd[0]);
    close(e->wake_fd[1]);
    free(e->pfds);
    free(e->watches);
    free(e->index_of_fd);
    free(e);
}

static int poll_ctl(mco_engine_t* e, int fd, int events, mco_engine_cb cb, void* ud) {
    int slot;
    mco_poll_watch_t* w;

    if (index_ensure(e, fd) != 0) return -1;
    slot = e->index_of_fd[fd];

    if (slot < 0) {
        if (events == 0) return 0; /* cancelling an unregistered fd: ignore */
        if (slot_ensure(e) != 0) return -1;
        slot = e->n++;
        e->index_of_fd[fd] = slot;
        e->pfds[slot].fd = fd;
        e->pfds[slot].revents = 0;
        memset(&e->watches[slot], 0, sizeof(mco_poll_watch_t));
    }
    w = &e->watches[slot];

    /* Identity check: a stale unregister must not clear pfds' mask, or the new
     * coroutine would never get events. */
    if (events & MCO_EV_READ) {
        if (cb) { w->read_cb = cb; w->read_ud = ud; }
        else if (w->read_ud == ud) { w->read_cb = NULL; w->read_ud = NULL; }
    }
    if (events & MCO_EV_WRITE) {
        if (cb) { w->write_cb = cb; w->write_ud = ud; }
        else if (w->write_ud == ud) { w->write_cb = NULL; w->write_ud = NULL; }
    }

    if (!w->read_cb && !w->write_cb) {
        remove_slot(e, slot);
        e->index_of_fd[fd] = -1;
        return 0;
    }

    e->pfds[slot].events = 0;
    if (w->read_cb)  e->pfds[slot].events |= POLLIN;
    if (w->write_cb) e->pfds[slot].events |= POLLOUT;
    return 0;
}

static int poll_poll(mco_engine_t* e, int timeout_ms) {
    int n, i, fired = 0;
    n = poll(e->pfds, e->n, timeout_ms);
    if (n < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }
    /* Callbacks may ctl() and mutate pfds/watches, so work from this snapshot and
     * stop scanning once fired. */
    for (i = 0; i < e->n && fired < n * 2; ++i) {
        mco_poll_watch_t w;
        int fd;
        int err_flag;
        int is_r, is_w;
        if (!e->pfds[i].revents) continue;
        err_flag = (e->pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) ? MCO_EV_ERROR : 0;
        is_r = e->pfds[i].revents & POLLIN;
        is_w = e->pfds[i].revents & POLLOUT;
        fd = e->pfds[i].fd;
        w = e->watches[i];
        e->pfds[i].revents = 0;
        if (w.read_cb  && (is_r || err_flag)) { w.read_cb(w.read_ud, fd, MCO_EV_READ | err_flag);   ++fired; }
        if (w.write_cb && (is_w || err_flag)) { w.write_cb(w.write_ud, fd, MCO_EV_WRITE | err_flag); ++fired; }
    }
    return fired;
}

static void poll_wake(mco_engine_t* e) {
    char b = 1;
    (void)!write(e->wake_fd[1], &b, 1);
}

static const mco_engine_ops_t s_poll_ops = {
    "poll",
    poll_create,
    poll_destroy,
    poll_ctl,
    poll_poll,
    poll_wake,
    NULL
};

const mco_engine_ops_t* mco_engine_poll_ops(void) { return &s_poll_ops; }

#endif /* !_WIN32 */
