/*
 * mco_engine_iocp.c — Windows IOCP completion backend.
 * Two layers, mirroring mco_engine_uring.c:
 *   1) Readiness via WSAPoll (Vista+) for the mco_wait_io path.
 *   2) Completion via WSARecv/WSASend/ConnectEx/AcceptEx on IOCP; completions
 *      are dispatched inside poll(). Cancel goes through submit_cancel
 *      (CancelIoEx), and the coroutine resumes only after the op's completion,
 *      keeping its stack buffer alive.
 *
 * Unlike io_uring these calls may complete synchronously (return 0/TRUE, no IOCP
 * packet): submit_* then dispatches the cb itself and sets MCO_ASYNC_SYNC_DONE on
 * the cookie, so the caller returns without yielding (a re-read would EAGAIN).
 *
 * Readiness and completion are independent (WSAPoll vs IOCP): poll() reaps IOCP
 * packets non-blocking first, then WSAPoll; with ops in flight the WSAPoll
 * timeout drops to 1ms so completions are not missed.
 *
 * Verified on Win7+; all APIs are Vista+/Win7+. Key constraint: completion-style
 * accept must use AcceptEx (see mco_accept_uses_async()), because BSD accept()
 * yields a socket without WSA_FLAG_OVERLAPPED, unusable on IOCP. Every socket
 * used for completion io is created with WSA_FLAG_OVERLAPPED.
 */
#if defined(_WIN32)

#include "mco_engine.h"
#include "mco_engine_slot.h"
#include "../compat/hsocket.h"
#include <winsock2.h>
#include <mswsock.h>   /* AcceptEx / ConnectEx / SIO_GET_EXTENSION_FUNCTION_POINTER */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#ifdef _MSC_VER
#pragma comment(lib, "mswsock.lib")  /* AcceptEx / ConnectEx */
#pragma comment(lib, "ws2_32.lib")
#endif

#define IOCP_OP_READ    1
#define IOCP_OP_WRITE   2
#define IOCP_OP_CONNECT 3
#define IOCP_OP_ACCEPT  4

#define IOCP_SLOTS 1024

/* ---- readiness (WSAPoll) watches: compact array, fds[i] <-> watches[i] ---- */
typedef struct mco_iocp_watch {
    mco_engine_cb read_cb,  write_cb;
    void*         read_ud,  *write_ud;
    int           used;
} mco_iocp_watch_t;

/* ---- Engine-side record for a completion op. OVERLAPPED must be first:
 * GetQueuedCompletionStatusEx returns an OVERLAPPED*. One slot per in-flight op;
 * when the table is full submit_* returns 0.
 * Hot fields first (64B); READ/WRITE and ACCEPT state are mutually exclusive, so
 * a union saves ~96B per slot. */
typedef struct mco_iocp_slot {
    OVERLAPPED ov;
    int        in_use;
    int        fd;
    unsigned   gen;
    int        op;
    mco_engine_async_cb cb;
    void*      ud;
    union {
        WSABUF wsa;   /* READ/WRITE: buffer for WSARecv/WSASend */
        struct {
            SOCKET accept_sock;  /* ACCEPT: pre-created socket, handed back via res */
            char   accept_buf[2 * (sizeof(struct sockaddr_in6) + 16)]; /* AcceptEx addresses */
        } acc;
    } u;
} mco_iocp_slot_t;

struct mco_engine {
    HANDLE iocp;
    /* readiness (WSAPoll) — fd->slot table (swap mode: tight & indexable) */
    mco_slot_table_t rdy;
    mco_iocp_watch_t* watches;  /* parallel to rdy, grown by iocp_rdy_grow */
    WSAPOLLFD* pollfds;         /* parallel to rdy, grown by iocp_rdy_grow */
    int   wake_fd[2];
    /* async layer (IOCP) */
    mco_iocp_slot_t* slots;
    int   slots_cap;
    int*  slot_free;
    int   slot_free_top;
    int   nasync_pending;   /* in-flight ops; shrinks the WSAPoll timeout */
};

static uint64_t iocp_cookie(unsigned slot, unsigned gen) {
    /* Low 32 bits hold gen+1: cookie 0 means "submit failed", and slot 0 with
     * generation 0 would collide with it. The bias keeps cookies non-zero. */
    return ((uint64_t)slot << 32) | ((uint64_t)(gen + 1) & 0xFFFFFFFFu);
}

/* ---- readiness: fd->slot via the shared table (swap mode, so [0,rdy.n) is
 * tight and indexable; iocp_rdy_swap keeps watches/pollfds in sync). ---- */

static int iocp_rdy_grow(mco_slot_table_t* t, int newcap, void* ud) {
    mco_engine_t* e = (mco_engine_t*)ud;
    mco_iocp_watch_t* nw = (mco_iocp_watch_t*)realloc(e->watches,
                                                     (size_t)newcap * sizeof(mco_iocp_watch_t));
    WSAPOLLFD* np = (WSAPOLLFD*)realloc(e->pollfds, (size_t)newcap * sizeof(WSAPOLLFD));
    if (!nw || !np) return -1;
    e->watches = nw;
    e->pollfds = np;
    return 0;
}

static void iocp_rdy_swap(void* ud, int a, int b) {
    mco_engine_t* e = (mco_engine_t*)ud;
    e->watches[a] = e->watches[b];
    e->pollfds[a] = e->pollfds[b];
}

static void on_wake_drain(void* ud, int fd, int revents) {
    char buf[64];
    (void)ud; (void)revents;
    while (recv((SOCKET)fd, buf, sizeof(buf), 0) > 0) { /* drain */ }
}

/* ---- completion slot management ---- */

static int slot_alloc(mco_engine_t* e) {
    if (e->slot_free_top == 0) return -1;
    return e->slot_free[--e->slot_free_top];
}

static void slot_release(mco_engine_t* e, mco_iocp_slot_t* s) {
    int idx = (int)(s - e->slots);
    /* Only meaningful for ACCEPT: for READ/WRITE the union holds wsa. */
    if (s->op == IOCP_OP_ACCEPT && s->u.acc.accept_sock != INVALID_SOCKET) {
        closesocket(s->u.acc.accept_sock);
        s->u.acc.accept_sock = INVALID_SOCKET;
    }
    s->in_use = 0;
    s->cb = NULL;
    s->ud = NULL;
    s->gen++;   /* roll the generation: stale cookies are void */
    e->slot_free[e->slot_free_top++] = idx;
}

/* Associate the socket with IOCP (idempotent). Called by every submit_*. */
static int iocp_associate(mco_engine_t* e, int fd) {
    HANDLE h = CreateIoCompletionPort((HANDLE)(uintptr_t)fd, e->iocp, 0, 0);
    if (h == e->iocp) return 0;
    /* A handle can only be associated once; a repeat returns NULL +
     * ERROR_INVALID_PARAMETER(87), treated as success. Association dies with the
     * handle, so a reused handle value associates again. */
    if (GetLastError() == ERROR_INVALID_PARAMETER) return 0;
    return -1;
}

/* Compute and dispatch a completion result.
 * from_completion: 1 = async packet, use WSAGetOverlappedResult (a failed op
 * still posts). 0 = completed inside submit_*, bytes is the count and calling
 * WSAGetOverlappedResult would report a bogus failure. */
static void iocp_dispatch_slot(mco_engine_t* e, mco_iocp_slot_t* s, DWORD bytes, int from_completion) {
    int res;
    DWORD flags = 0;
    /* AcceptEx completes on accept_sock; the listen fd gives a wrong result. */
    SOCKET h = (s->op == IOCP_OP_ACCEPT) ? s->u.acc.accept_sock : (SOCKET)s->fd;
    BOOL ok = from_completion ? WSAGetOverlappedResult(h, &s->ov, &bytes, FALSE, &flags) : TRUE;
    if (ok) {
        switch (s->op) {
        case IOCP_OP_READ:
        case IOCP_OP_WRITE:
            res = (int)bytes;
            break;
        case IOCP_OP_CONNECT:
            res = 0;
            break;
        case IOCP_OP_ACCEPT:
            /* Inside if (ok): only a successful AcceptEx hands over accept_sock;
             * a cancel (995) is handled by the else branch. */
            if (!ok) { res = -(int)WSAGetLastError(); break; }
            /* AcceptEx requires SO_UPDATE_ACCEPT_CONTEXT, else getsockname and
             * friends fail. The value is the listen socket: pass sizeof(SOCKET)
             * bytes, it is 64-bit on x64. */
            {
                SOCKET listen_sock = (SOCKET)s->fd;
                setsockopt(s->u.acc.accept_sock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                           (char*)&listen_sock, sizeof(listen_sock));
            }
            /* accept_sock does not inherit non-blocking mode; set it explicitly so
             * it behaves like the readiness fallback path. */
            nonblocking((int)s->u.acc.accept_sock);
            res = (int)s->u.acc.accept_sock;
            s->u.acc.accept_sock = INVALID_SOCKET; /* ownership to the caller */
            break;
        default:
            res = 0;
            break;
        }
    } else {
        /* res<0 is a -WSA error; ECONNREFUSED &co are WSA codes (10061...). */
        res = -(int)WSAGetLastError();
    }
    if (s->cb) s->cb(s->ud, s->fd, res);
    slot_release(e, s);
    e->nasync_pending--;
}

/* Immediate failure inside submit_* (SOCKET_ERROR, not WSA_IO_PENDING): no IOCP
 * packet, dispatch -WSA directly. */
static void iocp_dispatch_fail(mco_engine_t* e, mco_iocp_slot_t* s) {
    int res = -(int)WSAGetLastError();
    if (s->cb) s->cb(s->ud, s->fd, res);
    slot_release(e, s);
    e->nasync_pending--;
}

/* ---- engine lifetime ---- */

static mco_engine_t* iocp_create_engine(void) {
    mco_engine_t* e = (mco_engine_t*)calloc(1, sizeof(mco_engine_t));
    int sv[2];
    int i;

    if (!e) return NULL;
    e->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!e->iocp) { free(e); return NULL; }
    mco_slot_table_init(&e->rdy, 0 /*swap*/, iocp_rdy_grow, iocp_rdy_swap, e);

    if (Socketpair(AF_INET, SOCK_STREAM, 0, sv) != 0) {
        CloseHandle(e->iocp); mco_slot_table_free(&e->rdy); free(e); return NULL;
    }
    e->wake_fd[0] = sv[0];
    e->wake_fd[1] = sv[1];
    nonblocking(e->wake_fd[0]);
    nonblocking(e->wake_fd[1]);
    {
        int slot, is_new = 0;
        if (mco_slot_ensure(&e->rdy, e->wake_fd[0], &slot, &is_new) != 0) {
            closesocket((SOCKET)e->wake_fd[0]); closesocket((SOCKET)e->wake_fd[1]);
            CloseHandle(e->iocp); free(e->watches); mco_slot_table_free(&e->rdy); free(e); return NULL;
        }
        memset(&e->watches[slot], 0, sizeof(e->watches[slot]));
        e->watches[slot].read_cb = on_wake_drain;
        e->watches[slot].used = 1;
        e->pollfds[slot].fd = (SOCKET)e->wake_fd[0];
        e->pollfds[slot].events = 0;
        e->pollfds[slot].revents = 0;
    }

    e->slots_cap = IOCP_SLOTS;
    e->slots = (mco_iocp_slot_t*)calloc(1, sizeof(mco_iocp_slot_t) * IOCP_SLOTS);
    e->slot_free = (int*)malloc(sizeof(int) * IOCP_SLOTS);
    if (!e->slots || !e->slot_free) goto fail;
    for (i = 0; i < IOCP_SLOTS; ++i) {
        e->slots[i].u.acc.accept_sock = INVALID_SOCKET;
        e->slot_free[i] = IOCP_SLOTS - 1 - i;
    }
    e->slot_free_top = IOCP_SLOTS;
    return (mco_engine_t*)e;

fail:
    closesocket((SOCKET)e->wake_fd[0]); closesocket((SOCKET)e->wake_fd[1]);
    CloseHandle(e->iocp);
    free(e->slots); free(e->slot_free); free(e->watches);
    free(e->pollfds); mco_slot_table_free(&e->rdy);
    free(e);
    return NULL;
}

static void iocp_destroy_engine(mco_engine_t* e) {
    int i;
    if (!e) return;
    closesocket((SOCKET)e->wake_fd[1]);
    closesocket((SOCKET)e->wake_fd[0]);
    for (i = 0; i < e->slots_cap; ++i) {
        if (e->slots[i].op == IOCP_OP_ACCEPT &&
            e->slots[i].u.acc.accept_sock != INVALID_SOCKET)
            closesocket(e->slots[i].u.acc.accept_sock);
    }
    free(e->slots);
    free(e->slot_free);
    free(e->watches);
    free(e->pollfds);
    mco_slot_table_free(&e->rdy);
    CloseHandle(e->iocp);
    free(e);
}

/* ---- readiness ctl/poll/wake (WSAPoll, as in the wepoll backend) ---- */

static int iocp_ctl_engine(mco_engine_t* e, int fd, int events, mco_engine_cb cb, void* ud) {
    int slot, is_new = 0;
    mco_iocp_watch_t* w;

    if (mco_slot_ensure(&e->rdy, fd, &slot, &is_new) != 0) return -1;
    if (is_new) {
        memset(&e->watches[slot], 0, sizeof(e->watches[slot]));
        e->pollfds[slot].fd = (SOCKET)fd;
        e->pollfds[slot].events = 0;
        e->pollfds[slot].revents = 0;
    }
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
    w->used = (w->read_cb != NULL) || (w->write_cb != NULL);
    /* events updated incrementally, not rebuilt per poll */
    e->pollfds[slot].events = (short)
        ((w->read_cb ? POLLRDNORM : 0) | (w->write_cb ? POLLWRNORM : 0));
    if (!w->used) mco_slot_release(&e->rdy, slot);
    return 0;
}

/* Dispatch WSAPoll readiness. Returns the event count. e->pollfds is maintained
 * incrementally by ctl (indexed by the rdy slot); revents is output-only. */
static int iocp_wsapoll(mco_engine_t* e, int timeout_ms) {
    int i, n, fired = 0;

    if (e->rdy.n == 0) return 0;
    n = WSAPoll(e->pollfds, (ULONG)e->rdy.n, timeout_ms);
    if (n == SOCKET_ERROR) return -1;
    for (i = 0; i < e->rdy.n && fired < n * 2; ++i) {
        mco_iocp_watch_t w;
        int is_r, is_w, err_flag;
        if (e->pollfds[i].revents == 0) continue;
        err_flag = (e->pollfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) ? MCO_EV_ERROR : 0;
        is_r = (e->pollfds[i].revents & (POLLRDNORM | POLLIN)) != 0;
        is_w = (e->pollfds[i].revents & (POLLWRNORM | POLLOUT)) != 0;
        w = e->watches[i];
        if (is_r && w.read_cb)  { w.read_cb(w.read_ud, e->rdy.fds[i], MCO_EV_READ | err_flag);  ++fired; }
        if (is_w && w.write_cb) { w.write_cb(w.write_ud, e->rdy.fds[i], MCO_EV_WRITE | err_flag); ++fired; }
    }
    return fired;
}

/* Reap IOCP packets non-blocking. Returns the dispatch count. */
static int iocp_drain_completions(mco_engine_t* e) {
    OVERLAPPED_ENTRY entries[64];
    ULONG got = 0;
    int i, fired = 0;
    BOOL ok = GetQueuedCompletionStatusEx(e->iocp, entries, 64, &got, 0, FALSE);
    if (!ok) return 0; /* no packet (timeout 0) or error */
    for (i = 0; i < (int)got; ++i) {
        OVERLAPPED* ov = entries[i].lpOverlapped;
        mco_iocp_slot_t* s;
        if (!ov) continue; /* wake packet from PostQueuedCompletionStatus */
        s = (mco_iocp_slot_t*)CONTAINING_RECORD(ov, mco_iocp_slot_t, ov);
        if (!s->in_use) continue; /* stale */
        iocp_dispatch_slot(e, s, (DWORD)entries[i].dwNumberOfBytesTransferred, 1);
        ++fired;
    }
    return fired;
}

/* Block for one IOCP packet (timeout_ms<0 forever). Returns the dispatch count. */
static int iocp_wait_completion(mco_engine_t* e, int timeout_ms) {
    OVERLAPPED_ENTRY entry;
    ULONG got = 0;
    DWORD ms = (timeout_ms < 0) ? INFINITE : (DWORD)timeout_ms;
    BOOL ok = GetQueuedCompletionStatusEx(e->iocp, &entry, 1, &got, ms, FALSE);
    if (!ok) return 0; /* timeout or error */
    if (entry.lpOverlapped) {
        mco_iocp_slot_t* s = (mco_iocp_slot_t*)CONTAINING_RECORD(entry.lpOverlapped, mco_iocp_slot_t, ov);
        if (s->in_use) {
            iocp_dispatch_slot(e, s, (DWORD)entry.dwNumberOfBytesTransferred, 1);
            return 1;
        }
    }
    return 0;
}

static int iocp_poll_engine(mco_engine_t* e, int timeout_ms) {
    int fired = 0;

    /* 1. reap IOCP packets non-blocking (async fast path) */
    fired += iocp_drain_completions(e);

    /* 2. pick the blocking mechanism based on whether any fd is registered */
    if (fired == 0 && timeout_ms != 0) {
        if (e->rdy.n > 0) {
            /* WSAPoll; with ops in flight the timeout drops to 1ms so no IOCP
             * completion is missed. */
            int wtimeout = timeout_ms;
            if (e->nasync_pending > 0 && (wtimeout < 0 || wtimeout > 1))
                wtimeout = 1;
            fired += iocp_wsapoll(e, wtimeout);
        } else if (e->nasync_pending > 0) {
            /* only async ops: block on IOCP */
            fired += iocp_wait_completion(e, timeout_ms);
        }
        /* nothing to wait for */
    }
    return fired;
}

static void iocp_wake_engine(mco_engine_t* e) {
    char b = 1;
    /* Wake both: the socket wakes WSAPoll, PostQueuedCompletionStatus wakes IOCP. */
    send((SOCKET)e->wake_fd[1], &b, 1, 0);
    PostQueuedCompletionStatus(e->iocp, 0, 0, NULL);
}

/* ---- completion-style async ops ---- */

static uint64_t iocp_submit_read(mco_engine_t* e, int fd, void* buf, int len,
                                 mco_engine_async_cb cb, void* ud) {
    mco_iocp_slot_t* s;
    int slot;
    DWORD bytes = 0, flags = 0;
    int rc;

    if (iocp_associate(e, fd) != 0) return 0;
    slot = slot_alloc(e);
    if (slot < 0) return 0;
    s = &e->slots[slot];
    memset(&s->ov, 0, sizeof(s->ov));
    s->in_use = 1;
    s->fd = fd;
    s->op = IOCP_OP_READ;
    s->cb = cb;
    s->ud = ud;
    s->u.wsa.buf = (char*)buf;
    s->u.wsa.len = (ULONG)len;
    e->nasync_pending++;

    rc = WSARecv((SOCKET)fd, &s->u.wsa, 1, &bytes, &flags, &s->ov, NULL);
    if (rc == 0) {
        /* Completed synchronously: dispatch directly and set MCO_ASYNC_SYNC_DONE
         * so the caller does not yield.
         * from_completion=0: bytes is the count, do not call WSAGetOverlappedResult. */
        iocp_dispatch_slot(e, s, bytes, 0);
        return iocp_cookie((uint32_t)slot, s->gen) | MCO_ASYNC_SYNC_DONE;
    }
    if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        /* immediate failure counts as sync completion: same flag */
        iocp_dispatch_fail(e, s);
        return iocp_cookie((uint32_t)slot, s->gen) | MCO_ASYNC_SYNC_DONE;
    }
    return iocp_cookie((uint32_t)slot, s->gen);
}

static uint64_t iocp_submit_write(mco_engine_t* e, int fd, const void* buf, int len,
                                  mco_engine_async_cb cb, void* ud) {
    mco_iocp_slot_t* s;
    int slot;
    DWORD bytes = 0;
    int rc;

    if (iocp_associate(e, fd) != 0) return 0;
    slot = slot_alloc(e);
    if (slot < 0) return 0;
    s = &e->slots[slot];
    memset(&s->ov, 0, sizeof(s->ov));
    s->in_use = 1;
    s->fd = fd;
    s->op = IOCP_OP_WRITE;
    s->cb = cb;
    s->ud = ud;
    s->u.wsa.buf = (char*)buf;
    s->u.wsa.len = (ULONG)len;
    e->nasync_pending++;

    rc = WSASend((SOCKET)fd, &s->u.wsa, 1, &bytes, 0, &s->ov, NULL);
    if (rc == 0) {
        iocp_dispatch_slot(e, s, bytes, 0);
        return iocp_cookie((uint32_t)slot, s->gen) | MCO_ASYNC_SYNC_DONE;
    }
    if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        iocp_dispatch_fail(e, s);
        return iocp_cookie((uint32_t)slot, s->gen) | MCO_ASYNC_SYNC_DONE;
    }
    return iocp_cookie((uint32_t)slot, s->gen);
}

static uint64_t iocp_submit_connect(mco_engine_t* e, int fd, const void* addr, unsigned addrlen,
                                    mco_engine_async_cb cb, void* ud) {
    /* mco_tcp_connect already started a non-blocking connect(), and ConnectEx
     * cannot attach to a socket with a connection in progress (unlike io_uring).
     * Fall back to readiness: WSAPoll for writable + SO_ERROR. */
    (void)e; (void)fd; (void)addr; (void)addrlen; (void)cb; (void)ud;
    return 0;
}

static uint64_t iocp_submit_accept(mco_engine_t* e, int fd, mco_engine_async_cb cb, void* ud) {
    mco_iocp_slot_t* s;
    int slot;
    struct sockaddr_storage ss;
    int sslen = sizeof(ss);
    int family;
    DWORD bytes = 0;
    BOOL rc;

    if (iocp_associate(e, fd) != 0) return 0;
    if (getsockname((SOCKET)fd, (struct sockaddr*)&ss, &sslen) != 0) return 0;
    family = ss.ss_family;

    slot = slot_alloc(e);
    if (slot < 0) return 0;
    s = &e->slots[slot];
    memset(&s->ov, 0, sizeof(s->ov));
    s->in_use = 1;
    s->fd = fd;
    s->op = IOCP_OP_ACCEPT;
    s->cb = cb;
    s->ud = ud;
    s->u.acc.accept_sock = WSASocket(family, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (s->u.acc.accept_sock == INVALID_SOCKET) {
        slot_release(e, s);
        e->nasync_pending--;
        return 0;
    }
    e->nasync_pending++;

    rc = AcceptEx((SOCKET)fd, s->u.acc.accept_sock, s->u.acc.accept_buf, 0,
                  (DWORD)(sizeof(struct sockaddr_in6) + 16),
                  (DWORD)(sizeof(struct sockaddr_in6) + 16),
                  &bytes, &s->ov);
    if (rc) {
        iocp_dispatch_slot(e, s, 0, 0);
        return iocp_cookie((uint32_t)slot, s->gen) | MCO_ASYNC_SYNC_DONE;
    }
    if (WSAGetLastError() != WSA_IO_PENDING) {
        iocp_dispatch_fail(e, s);
        return iocp_cookie((uint32_t)slot, s->gen) | MCO_ASYNC_SYNC_DONE;
    }
    return iocp_cookie((uint32_t)slot, s->gen);
}

static int iocp_submit_cancel(mco_engine_t* e, uint64_t cookie, void* ud) {
    unsigned slot;
    unsigned gen;
    mco_iocp_slot_t* s;

    /* An op that completed inside submit_* has nothing to cancel. */
    if (cookie & MCO_ASYNC_SYNC_DONE) return 0;

    slot = (unsigned)((cookie >> 32) & 0xFFFFFFFFu);
    gen  = (unsigned)((cookie & 0xFFFFFFFFu) - 1); /* undo the cookie +1 bias */

    if ((int)slot >= e->slots_cap) return 0;
    s = &e->slots[slot];
    /* no-op if the op already finished */
    if (!s->in_use || s->ud != ud || s->gen != gen) return 0;
    CancelIoEx((HANDLE)(uintptr_t)s->fd, &s->ov);
    return 0;
}

static const struct mco_engine_async_ops s_iocp_async_ops = {
    iocp_submit_read,
    iocp_submit_write,
    iocp_submit_connect,
    iocp_submit_accept,
    iocp_submit_cancel
};

static const mco_engine_ops_t s_iocp_ops = {
    "iocp",
    iocp_create_engine,
    iocp_destroy_engine,
    iocp_ctl_engine,
    iocp_poll_engine,
    iocp_wake_engine,
    &s_iocp_async_ops
};

const mco_engine_ops_t* mco_engine_iocp_ops(void) { return &s_iocp_ops; }

#endif /* _WIN32 */