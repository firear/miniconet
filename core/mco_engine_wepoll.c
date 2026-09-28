/*
 * mco_engine_wepoll.c — Windows backend on wepoll (epoll for Windows).
 * Signatures differ from Linux epoll: epoll_create1() returns a HANDLE,
 * epoll_ctl() takes a SOCKET, and cleanup is epoll_close(), hence a separate
 * file. int fd -> SOCKET assumes handles fit in 32 bits (see
 * fd_to_socket_checked). wepoll only accepts Winsock sockets; the wakeup uses an
 * emulated socket pair. fd->slot mapping and O(1) poll recovery live in
 * mco_engine_slot.h: the slot index is encoded in ev.data.u32, so poll() needs
 * no linear scan.
 */
#if defined(_WIN32)

#include "mco_engine.h"
#include "mco_engine_slot.h"
#include "../compat/wepoll/wepoll.h"
#include "../compat/hsocket.h"
#include <stdlib.h>
#include <string.h>

typedef struct mco_we_watch {
    mco_engine_cb read_cb,  write_cb;
    void*         read_ud,  *write_ud;
    /* Armed directions under EPOLLONESHOT (disarms on dispatch). */
    int           armed_mask;
} mco_we_watch_t;

struct mco_engine {
    HANDLE ephnd;
    mco_slot_table_t slots;       /* fd -> stable slot (free-list mode) */
    mco_we_watch_t*  watches;     /* parallel to slots, grown by wepoll_grow */
    int   wake_fd[2];
};

static int wepoll_grow(mco_slot_table_t* t, int newcap, void* ud) {
    mco_engine_t* e = (mco_engine_t*)ud;
    mco_we_watch_t* nw = (mco_we_watch_t*)realloc(e->watches,
                                                  (size_t)newcap * sizeof(mco_we_watch_t));
    if (!nw) return -1;
    e->watches = nw;
    return 0;
}

/* int fd -> SOCKET: reject handles that do not fit in 32 bits. */
static int fd_to_socket_checked(int fd, SOCKET* out) {
    if (fd < 0) return -1;
    *out = (SOCKET)(uintptr_t)(unsigned int)fd;
    return 0;
}

static void on_wake_drain(void* ud, int fd, int revents) {
    char buf[64];
    SOCKET s;
    (void)ud; (void)revents;
    if (fd_to_socket_checked(fd, &s) != 0) return;
    while (recv(s, buf, sizeof(buf), 0) > 0) { /* drain */ }
}

static mco_engine_t* wepoll_engine_create(void) {
    mco_engine_t* e = (mco_engine_t*)calloc(1, sizeof(mco_engine_t));
    int sv[2];
    int slot, is_new = 0;
    struct epoll_event ev;

    if (!e) return NULL;
    mco_slot_table_init(&e->slots, 1 /*freelist*/, wepoll_grow, NULL, e);

    e->ephnd = epoll_create1(0);
    if (!e->ephnd) { free(e); return NULL; }

    if (Socketpair(AF_INET, SOCK_STREAM, 0, sv) != 0) {
        epoll_close(e->ephnd); free(e); return NULL;
    }
    e->wake_fd[0] = sv[0];
    e->wake_fd[1] = sv[1];
    nonblocking(e->wake_fd[0]);
    nonblocking(e->wake_fd[1]);

    if (mco_slot_ensure(&e->slots, e->wake_fd[0], &slot, &is_new) != 0) {
        epoll_close(e->ephnd);
        closesocket((SOCKET)e->wake_fd[0]); closesocket((SOCKET)e->wake_fd[1]);
        mco_slot_table_free(&e->slots); free(e->watches); free(e); return NULL;
    }
    memset(&e->watches[slot], 0, sizeof(e->watches[slot]));
    e->watches[slot].read_cb = on_wake_drain;

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u32 = (uint32_t)slot;
    {
        SOCKET s;
        fd_to_socket_checked(e->wake_fd[0], &s);
        epoll_ctl(e->ephnd, EPOLL_CTL_ADD, s, &ev);
    }
    e->watches[slot].armed_mask = EPOLLIN; /* matches the bare ADD above */
    return e;
}

static void wepoll_engine_destroy(mco_engine_t* e) {
    if (!e) return;
    epoll_close(e->ephnd);
    closesocket((SOCKET)e->wake_fd[0]);
    closesocket((SOCKET)e->wake_fd[1]);
    mco_slot_table_free(&e->slots);
    free(e->watches);
    free(e);
}

static int wepoll_engine_ctl(mco_engine_t* e, int fd, int events, mco_engine_cb cb, void* ud) {
    int slot, is_new = 0;
    SOCKET s;
    struct epoll_event ev;
    mco_we_watch_t* w;
    int new_mask, desired, dirty;

    if (fd_to_socket_checked(fd, &s) != 0) return -1;
    if (mco_slot_ensure(&e->slots, fd, &slot, &is_new) != 0) return -1;
    if (is_new) memset(&e->watches[slot], 0, sizeof(e->watches[slot]));
    w = &e->watches[slot];

    /* Identity check: a stale unregister must not clear the new registration; a
     * foreign overwrite forces a resync. */
    dirty = 0;
    if (events & MCO_EV_READ) {
        if (cb) {
            if (w->read_ud != ud) dirty = 1;
            w->read_cb = cb; w->read_ud = ud;
        } else if (w->read_ud == ud) {
            w->read_cb = NULL; w->read_ud = NULL;
        }
    }
    if (events & MCO_EV_WRITE) {
        if (cb) {
            if (w->write_ud != ud) dirty = 1;
            w->write_cb = cb; w->write_ud = ud;
        } else if (w->write_ud == ud) {
            w->write_cb = NULL; w->write_ud = NULL;
        }
    }
    if (dirty) w->armed_mask = 0;

    new_mask = 0;
    if (w->read_cb)  new_mask |= EPOLLIN;
    if (w->write_cb) new_mask |= EPOLLOUT;

    /* Lazy unregister: no syscall when the kernel state already matches. */
    desired = new_mask;
    if (desired == w->armed_mask) return 0;

    if (desired == 0) {
        if (w->armed_mask) {
            epoll_ctl(e->ephnd, EPOLL_CTL_DEL, s, NULL);
            w->armed_mask = 0;
        }
        mco_slot_release(&e->slots, slot);
        return 0;
    }

    memset(&ev, 0, sizeof(ev));
    ev.events = desired | EPOLLONESHOT;
    ev.data.u32 = (uint32_t)slot;

    if (w->armed_mask == 0) {
        int rc = epoll_ctl(e->ephnd, EPOLL_CTL_ADD, s, &ev);
        if (rc != 0) {
            /* ADD failed: wepoll still holds a stale entry for this SOCKET value.
             * MOD would keep polling the closed socket, so DEL first. */
            epoll_ctl(e->ephnd, EPOLL_CTL_DEL, s, NULL);
            if (epoll_ctl(e->ephnd, EPOLL_CTL_ADD, s, &ev) != 0) return -1;
        }
    } else {
        if (epoll_ctl(e->ephnd, EPOLL_CTL_MOD, s, &ev) != 0) return -1;
    }
    w->armed_mask = desired;
    return 0;
}

static int wepoll_engine_poll(mco_engine_t* e, int timeout_ms) {
    struct epoll_event evs[256];
    int n, i, fired = 0;

    n = epoll_wait(e->ephnd, evs, 256, timeout_ms);
    if (n < 0) return -1;

    for (i = 0; i < n; ++i) {
        int slot = (int)evs[i].data.u32;   /* O(1): slot encoded at ctl time */
        int fd;
        int err_flag = (evs[i].events & (EPOLLERR | EPOLLHUP)) ? MCO_EV_ERROR : 0;
        mco_we_watch_t w;
        if (slot < 0 || slot >= e->slots.n) continue;  /* bounds guard */
        fd = e->slots.fds[slot];
        /* one-shot: the kernel disarmed it */
        e->watches[slot].armed_mask = 0;
        w = e->watches[slot];
        if (w.read_cb && ((evs[i].events & EPOLLIN) || err_flag)) {
            w.read_cb(w.read_ud, fd, MCO_EV_READ | err_flag);
            ++fired;
        }
        if (w.write_cb && ((evs[i].events & EPOLLOUT) || err_flag)) {
            w.write_cb(w.write_ud, fd, MCO_EV_WRITE | err_flag);
            ++fired;
        }
        /* No re-arm here: the woken coroutine re-arms via ctl, saving a MOD+DEL
         * pair. */
    }
    return fired;
}

static void wepoll_engine_wake(mco_engine_t* e) {
    char b = 1;
    send((SOCKET)e->wake_fd[1], &b, 1, 0);
}

static const mco_engine_ops_t s_wepoll_ops = {
    "wepoll",
    wepoll_engine_create,
    wepoll_engine_destroy,
    wepoll_engine_ctl,
    wepoll_engine_poll,
    wepoll_engine_wake
};

const mco_engine_ops_t* mco_engine_wepoll_ops(void) { return &s_wepoll_ops; }

#endif /* _WIN32 */
