/*
 * mco_engine_evport.c — Solaris / illumos event ports
 *
 * Associations are one-shot: port_getn() removes the whole fd (unlike kqueue's
 * per-direction filters), so poll() must reassociate the still-wanted
 * directions. Cross-thread wake uses PORT_SOURCE_USER + port_send(), no pipe.
 */
#if defined(__sun)

#include "mco_engine.h"
#include <port.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct mco_evport_watch {
    mco_engine_cb read_cb,  write_cb;
    void*         read_ud,  *write_ud;
    /* The kernel drops the association on dispatch; 0 skips redundant
     * associate/dissociate syscalls. */
    int           associated;
    int           assoc_mask;
} mco_evport_watch_t;

struct mco_engine {
    int port;
    mco_evport_watch_t* watches; /* indexed by fd */
    int watches_cap;
};

static int watches_ensure(mco_engine_t* e, int fd) {
    if (fd < 0) return -1;
    if (fd >= e->watches_cap) {
        int newcap = e->watches_cap > 0 ? e->watches_cap : 64;
        mco_evport_watch_t* nw;
        while (newcap <= fd) newcap *= 2;
        nw = (mco_evport_watch_t*)realloc(e->watches, sizeof(mco_evport_watch_t) * newcap);
        if (!nw) return -1;
        memset(nw + e->watches_cap, 0, sizeof(mco_evport_watch_t) * (newcap - e->watches_cap));
        e->watches = nw;
        e->watches_cap = newcap;
    }
    return 0;
}

/* Re-associate the directions watches[fd] still wants; 0 means none. Skip when
 * already associated with the same mask. */
static int reassociate(mco_engine_t* e, int fd) {
    mco_evport_watch_t* w = &e->watches[fd];
    int pollevents = 0;
    if (w->read_cb)  pollevents |= POLLIN;
    if (w->write_cb) pollevents |= POLLOUT;

    if (pollevents == 0) {
        if (w->associated) {
            w->associated = 0;
            if (port_dissociate(e->port, PORT_SOURCE_FD, (uintptr_t)fd) != 0 &&
                errno != ENOENT) {
                /* ignore: cb cleared, stale events are harmless */
            }
        }
        return 0;
    }
    if (w->associated && w->assoc_mask == pollevents) return 0;
    {
        int rc = port_associate(e->port, PORT_SOURCE_FD, (uintptr_t)fd, pollevents, NULL);
        if (rc == 0) { w->associated = 1; w->assoc_mask = pollevents; }
        return rc;
    }
}

static mco_engine_t* evport_create(void) {
    mco_engine_t* e = (mco_engine_t*)calloc(1, sizeof(mco_engine_t));
    if (!e) return NULL;
    e->port = port_create();
    if (e->port < 0) { free(e); return NULL; }
    return e;
}

static void evport_destroy(mco_engine_t* e) {
    if (!e) return;
    close(e->port);
    free(e->watches);
    free(e);
}

static int evport_ctl(mco_engine_t* e, int fd, int events, mco_engine_cb cb, void* ud) {
    mco_evport_watch_t* w;
    int dirty = 0;
    if (watches_ensure(e, fd) != 0) return -1;
    w = &e->watches[fd];

    /* Identity check: a stale unregister must not clear the new registration; a
     * foreign overwrite must force port_associate. */
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
    if (dirty) { w->associated = 0; w->assoc_mask = 0; }

    if (reassociate(e, fd) != 0) return -1;
    return 0;
}

static int evport_poll(mco_engine_t* e, int timeout_ms) {
    port_event_t evs[256];
    struct timespec ts, *pts = NULL;
    uint_t nget = 256;
    int i, fired = 0;

    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
        pts = &ts;
    }

    if (port_getn(e->port, evs, 256, &nget, pts) != 0) {
        if (errno == EINTR || errno == ETIME) return 0;
        return -1;
    }

    for (i = 0; i < (int)nget; ++i) {
        int fd;
        mco_evport_watch_t w;

        if (evs[i].portev_source == PORT_SOURCE_USER) continue; /* cross-thread wake() */
        if (evs[i].portev_source != PORT_SOURCE_FD) continue;

        fd = (int)evs[i].portev_object;
        if (fd < 0 || fd >= e->watches_cap) continue;
        /* one-shot: the kernel dropped it; sync the accounting first */
        e->watches[fd].associated = 0;
        w = e->watches[fd]; /* snapshot: a cb may ctl() this fd */

        if ((evs[i].portev_events & POLLIN) && w.read_cb) {
            w.read_cb(w.read_ud, fd, MCO_EV_READ);
            ++fired;
        }
        if ((evs[i].portev_events & POLLOUT) && w.write_cb) {
            w.write_cb(w.write_ud, fd, MCO_EV_WRITE);
            ++fired;
        }
        if (evs[i].portev_events & (POLLERR | POLLHUP)) {
            if (w.read_cb)  { w.read_cb(w.read_ud, fd, MCO_EV_ERROR);  ++fired; }
            if (w.write_cb) { w.write_cb(w.write_ud, fd, MCO_EV_ERROR); ++fired; }
        }

        /* No reassociate here: the woken coroutine resumes before the next poll
         * and re-associates via ctl, saving a pair of syscalls. */
    }
    return fired;
}

static void evport_wake(mco_engine_t* e) {
    /* PORT_SOURCE_USER can be sent from any thread; no pipe needed. */
    port_send(e->port, 0, NULL);
}

static const mco_engine_ops_t s_evport_ops = {
    "evport",
    evport_create,
    evport_destroy,
    evport_ctl,
    evport_poll,
    evport_wake
};

const mco_engine_ops_t* mco_engine_evport_ops(void) { return &s_evport_ops; }

#endif /* __sun */
