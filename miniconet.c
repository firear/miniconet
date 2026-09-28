#include "miniconet.h"
#include "core/mco_resolver.h"
#include "compat/hsocket.h"
#include "compat/ssl/hssl.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>   /* fstat/S_ISREG/... to probe an adopted fd */

/* socket type: selects the syscall set for mco_read/mco_write. Defined in
 * core/mco_vcpu.h; the wait path must use the same values. 0 = STREAM, so a
 * calloc'ed socket needs no explicit assignment. */

struct mco_socket {
    int fd;
    uint8_t type;            /* mco_sock_type_e; 0 = STREAM */
    uint8_t closed;
    uint8_t rd_likely;           /* 1 = last read had data, try read() first */
    uint8_t wr_likely;           /* same, write direction */
    mco_task_t* rd_waiter;   /* only coroutine allowed to wait on read; NULL = free */
    mco_task_t* wr_waiter;   /* same, write direction */
    int default_timeout_ms;  /* used by mco_read/write_default; <0 = no timeout */
    hssl_t ssl;               /* non-NULL = TLS, io goes through hssl_read/write */
    /* Coroutine owning the SSL object during handshake or a cross-direction
     * (renegotiation) wait; others get MCO_ERR_BUSY, since an SSL* cannot be
     * used from two call sites at once. */
    mco_task_t* ssl_cross_owner;
};

/* ---- loop / coroutine wrappers ---- */

mco_loop_t* mco_loop_new(void) { return mco_vcpu_new(NULL); }
void mco_loop_run(mco_loop_t* loop) { mco_vcpu_run(loop); }
void mco_loop_stop(mco_loop_t* loop) { mco_vcpu_stop(loop); }
void mco_loop_free(mco_loop_t* loop) { mco_vcpu_free(loop); }

mco_task_t* mco_spawn(mco_loop_t* loop, void (*fn)(void* arg), void* arg) {
    return mco_go(loop, fn, arg);
}

/* ---- socket basics ---- */

static mco_socket_t* sock_wrap_impl(int fd, int close_on_fail) {
    mco_socket_t* s;
    if (fd < 0) return NULL;
    s = (mco_socket_t*)calloc(1, sizeof(mco_socket_t));
    if (!s) { if (close_on_fail) closesocket(fd); return NULL; }
    s->fd = fd;
    s->rd_likely = 1;  /* optimistic: the first request arrives right after connect */
    s->wr_likely = 1;  /* the send buffer is nearly always writable */
    s->default_timeout_ms = -1; /* calloc gives 0; set "no timeout" explicitly */
    nonblocking(fd);
    return s;
}

/* internal: closes fd if the allocation fails */
static mco_socket_t* sock_wrap(int fd) {
    return sock_wrap_impl(fd, 1);
}

int mco_socket_fd(mco_socket_t* s) { return s ? s->fd : -1; }

/* Shared by getsockname/getpeername: peer non-zero takes the peer address.
 * Returns sockaddr_u; stringifying is the caller's job (see hsocket.h). */
static int socket_addr_fill(int fd, int peer, sockaddr_u* out) {
    socklen_t addrlen;
    int ret;

    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    addrlen = (socklen_t)sizeof(*out);
    if (peer) ret = getpeername(fd, &out->sa, &addrlen);
    else      ret = getsockname(fd, &out->sa, &addrlen);
    return ret == 0 ? 0 : -1;
}

int mco_socket_local_addr(mco_socket_t* s, sockaddr_u* out) {
    if (!s || s->fd < 0) return -1;
    return socket_addr_fill(s->fd, 0, out);
}

int mco_socket_peer_addr(mco_socket_t* s, sockaddr_u* out) {
    if (!s || s->fd < 0) return -1;
    return socket_addr_fill(s->fd, 1, out);
}

int mco_socket_is_ssl(mco_socket_t* s) { return (s && s->ssl) ? 1 : 0; }

/* Clear the SSL ownership flag if the current coroutine holds it. */
static void ssl_cross_owner_release(mco_socket_t* s) {
    if (s->ssl_cross_owner == mco_task_self()) s->ssl_cross_owner = NULL;
}

int mco_shutdown(mco_socket_t* s, int how) {
    if (!s || s->fd < 0 || s->closed) return MCO_ERR_CLOSED;
    /* 0/1/2 match POSIX SHUT_* and Winsock SD_*, passed straight through. */
    if (shutdown(s->fd, how) != 0) return MCO_ERR_IO;
    /* No wakeup and no closed flag: the shutdown direction must still report
     * EOF/EPIPE to the caller. */
    return 0;
}

void mco_close(mco_socket_t* s) {
    if (!s || s->closed) return;
    s->closed = 1;
    /* wake waiters before closing: they see closed==1 and do not retry */
    mco_task_wake_closed(s->rd_waiter);
    mco_task_wake_closed(s->wr_waiter);
    if (s->ssl) {
        hssl_close(s->ssl);  /* best-effort close_notify; no wait for ack */
        hssl_free(s->ssl);
        s->ssl = NULL;
    }
    if (s->fd >= 0) closesocket(s->fd); /* a lazy UDP socket has fd -1 until first sendto */
}

void mco_socket_destroy(mco_socket_t* s) {
    free(s);
}

/* ---- read/write (TCP) ---- */

/* Dispatch io by socket type: stream -> recv/send, dgram -> recvfrom/sendto,
 * else read/write. The fd is non-blocking, EAGAIN is handled above. */
static inline ssize_t mco_sock_recv(mco_socket_t* s, void* buf, int len) {
    if (s->type == MCO_SOCK_DGRAM)
        return recvfrom(s->fd, buf, len, 0, NULL, NULL);
    if (s->type == MCO_SOCK_FILE)
        return MCO_RAW_READ(s->fd, buf, len);
    return recv(s->fd, buf, len, 0);
}

static inline ssize_t mco_sock_send(mco_socket_t* s, const void* buf, int len) {
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    if (s->type == MCO_SOCK_DGRAM)
        return sendto(s->fd, buf, len, flags, NULL, 0);
    /* Raw fds have no MSG_NOSIGNAL: the caller must ignore SIGPIPE itself. */
    if (s->type == MCO_SOCK_FILE)
        return MCO_RAW_WRITE(s->fd, buf, len);
    return send(s->fd, buf, len, flags);
}

int mco_read(mco_socket_t* s, void* buf, int len, int timeout_ms) {
    if (!s || s->closed) return MCO_ERR_CLOSED;
    if (s->rd_waiter) return MCO_ERR_BUSY;

    if (s->ssl) {
        /* TLS: hssl_read() does not say whether to wait for read or write
         * (WANT_WRITE-during-read), so ask hssl_get_error(). A cross-direction
         * wait means renegotiation, so hold ssl_cross_owner and give the other
         * direction MCO_ERR_BUSY. */
        for (;;) {
            int n, err, events;

            if (s->ssl_cross_owner && s->ssl_cross_owner != mco_task_self()) return MCO_ERR_BUSY;

            n = hssl_read(s->ssl, buf, len);
            if (n > 0) { ssl_cross_owner_release(s); return n; }

            err = hssl_get_error(s->ssl, n);
            if (err == HSSL_EOF) { ssl_cross_owner_release(s); return 0; } /* clean TLS shutdown */
            if (err == HSSL_WANT_READ) events = MCO_EV_READ;
            else if (err == HSSL_WANT_WRITE) { events = MCO_EV_WRITE; s->ssl_cross_owner = mco_task_self(); }
            else { ssl_cross_owner_release(s); return MCO_ERR_IO; }

            s->rd_waiter = mco_task_self();
            {
                int r = mco_wait_io(s->fd, events, timeout_ms);
                s->rd_waiter = NULL;
                if (r == MCO_ERR_TIMEOUT) { ssl_cross_owner_release(s); return MCO_ERR_TIMEOUT; }
                if (r == MCO_ERR_CLOSED) { ssl_cross_owner_release(s); return MCO_ERR_CLOSED; }
            }
            if (s->closed) { ssl_cross_owner_release(s); return MCO_ERR_CLOSED; }
        }
    }

    for (;;) {
        /* Speculative read: a hit saves a yield plus an event round trip, a miss
         * costs one ~300ns syscall. rd_likely only goes back to 1 after the wait
         * path gets data, so it never "learns" to stop. */
        if (s->rd_likely) {
            ssize_t n = mco_sock_recv(s, buf, len);
            int e;
            if (n >= 0) return (int)n; /* n==0 is an exact EOF, not an error */
            e = mco_io_errno((int)s->type);
            if (e == EINTR) continue;
            if (e != EAGAIN && e != EWOULDBLOCK) return MCO_ERR_IO;
            s->rd_likely = 0;
        }

        s->rd_waiter = mco_task_self();
        {
            /* Completion fast path: completes the read here (0 = EOF). */
            int r = mco_wait_recv(s->fd, buf, len, (int)s->type, timeout_ms);
            s->rd_waiter = NULL;
            if (r >= 0) s->rd_likely = 1;
            if (r == MCO_ERR_TIMEOUT) return MCO_ERR_TIMEOUT;
            if (r == MCO_ERR_CLOSED) return MCO_ERR_CLOSED;
            if (r == MCO_ERR_IO) return MCO_ERR_IO;
            if (s->closed) return MCO_ERR_CLOSED;
            return r; /* r>=0: data is in buf (0 = EOF) */
        }
    }
}

int mco_write(mco_socket_t* s, const void* buf, int len, int timeout_ms) {
    if (!s || s->closed) return MCO_ERR_CLOSED;
    if (s->wr_waiter) return MCO_ERR_BUSY;

    if (s->ssl) {
        /* Mirror of mco_read's TLS branch: a write may need to wait for READ. */
        for (;;) {
            int n, err, events;

            if (s->ssl_cross_owner && s->ssl_cross_owner != mco_task_self()) return MCO_ERR_BUSY;

            n = hssl_write(s->ssl, buf, len);
            if (n > 0) { ssl_cross_owner_release(s); return n; }

            err = hssl_get_error(s->ssl, n);
            if (err == HSSL_EOF) { ssl_cross_owner_release(s); return 0; }
            if (err == HSSL_WANT_WRITE) events = MCO_EV_WRITE;
            else if (err == HSSL_WANT_READ) { events = MCO_EV_READ; s->ssl_cross_owner = mco_task_self(); }
            else { ssl_cross_owner_release(s); return MCO_ERR_IO; }

            s->wr_waiter = mco_task_self();
            {
                int r = mco_wait_io(s->fd, events, timeout_ms);
                s->wr_waiter = NULL;
                if (r == MCO_ERR_TIMEOUT) { ssl_cross_owner_release(s); return MCO_ERR_TIMEOUT; }
                if (r == MCO_ERR_CLOSED) { ssl_cross_owner_release(s); return MCO_ERR_CLOSED; }
            }
            if (s->closed) { ssl_cross_owner_release(s); return MCO_ERR_CLOSED; }
        }
    }

    for (;;) {
        /* Speculative write, mirroring mco_read: EAGAIN means the send buffer is
         * full, so skip the probe and go straight to the wait path. */
        if (s->wr_likely) {
            ssize_t n = mco_sock_send(s, buf, len);
            int e;
            if (n >= 0) return (int)n;
            e = mco_io_errno((int)s->type);
            if (e == EINTR) continue;
            if (e != EAGAIN && e != EWOULDBLOCK) return MCO_ERR_IO;
            s->wr_likely = 0;
        }

        s->wr_waiter = mco_task_self();
        {
            /* Completion fast path as in mco_read; a partial write (0<n<len) is
             * returned as-is, matching write(). */
            int r = mco_wait_send(s->fd, buf, len, (int)s->type, timeout_ms);
            s->wr_waiter = NULL;
            if (r == len) s->wr_likely = 1;
            if (r == MCO_ERR_TIMEOUT) return MCO_ERR_TIMEOUT;
            if (r == MCO_ERR_CLOSED) return MCO_ERR_CLOSED;
            if (r == MCO_ERR_IO) return MCO_ERR_IO;
            if (s->closed) return MCO_ERR_CLOSED;
            return r;
        }
    }
}

void mco_socket_set_timeout(mco_socket_t* s, int timeout_ms) {
    if (s) s->default_timeout_ms = timeout_ms;
}

void mco_socket_set_nodelay(mco_socket_t* s, int on) {
    /* fails harmlessly on non-TCP sockets; the result is ignored */
    if (s && s->fd >= 0) tcp_nodelay(s->fd, on ? 1 : 0);
}

/* ---- Read/write exactly N bytes ----
 * timeout_ms is the budget for the whole operation: it becomes a deadline and
 * each underlying call only gets what is left. */
int mco_read_exact(mco_socket_t* s, void* buf, int len, int timeout_ms) {
    mco_deadline_t dl;
    char* p = (char*)buf;
    int got = 0;

    if (!s || len < 0) return MCO_ERR_CLOSED;
    dl = mco_deadline_after(timeout_ms);

    while (got < len) {
        int n = mco_read(s, p + got, len - got, mco_deadline_ms(dl));
        if (n > 0) { got += n; continue; }
        if (n == 0) return got; /* EOF: return what was read so far (may be 0) */
        return n;               /* MCO_ERR_*: partial data is useless */
    }
    return got;
}

int mco_write_all(mco_socket_t* s, const void* buf, int len, int timeout_ms) {
    mco_deadline_t dl;
    const char* p = (const char*)buf;
    int sent = 0;

    if (!s || len < 0) return MCO_ERR_CLOSED;
    dl = mco_deadline_after(timeout_ms);

    while (sent < len) {
        int n = mco_write(s, p + sent, len - sent, mco_deadline_ms(dl));
        if (n < 0) return n;
        /* n==0 with len>0 only happens as a TLS EOF; treat it as an io error. */
        if (n == 0) return MCO_ERR_IO;
        sent += n;
    }
    return sent;
}

int mco_read_default(mco_socket_t* s, void* buf, int len) {
    if (!s) return MCO_ERR_CLOSED;
    return mco_read(s, buf, len, s->default_timeout_ms);
}

int mco_write_default(mco_socket_t* s, const void* buf, int len) {
    if (!s) return MCO_ERR_CLOSED;
    return mco_write(s, buf, len, s->default_timeout_ms);
}

/* ---- TCP ---- */

mco_socket_t* mco_tcp_listen(const char* host, int port) {
    int fd = Listen(port, host ? host : "0.0.0.0");
    if (fd < 0) return NULL;
    return sock_wrap(fd);
}

/* Accept without wrapping: same wait as mco_tcp_accept, returns the raw fd. */
int mco_tcp_accept_fd(mco_socket_t* listener, int timeout_ms) {
    if (!listener || listener->closed) return MCO_ERR_CLOSED;
    if (listener->rd_waiter) return MCO_ERR_BUSY; /* no concurrent accepts */
#if MINICONET_WITH_IOCP
    /* IOCP must use AcceptEx: BSD accept() on Windows yields a socket without
     * WSA_FLAG_OVERLAPPED, unusable for WSARecv/WSASend. Other backends keep the
     * synchronous accept fast path. */
    if (mco_accept_uses_async())
        return mco_wait_accept(listener->fd, timeout_ms);
#endif
    for (;;) {
        int connfd = accept(listener->fd, NULL, NULL);
        int e;
        if (connfd >= 0) return connfd;
        e = socket_errno();
        if (e == EINTR) continue;
        if (e != EAGAIN && e != EWOULDBLOCK) return MCO_ERR_IO;

        listener->rd_waiter = mco_task_self();
        {
            int r = mco_wait_accept(listener->fd, timeout_ms);
            listener->rd_waiter = NULL;
            if (r >= 0) return r; /* the completion path yields the new fd */
            if (r == MCO_ERR_TIMEOUT) return MCO_ERR_TIMEOUT;
            if (r == MCO_ERR_CLOSED) return MCO_ERR_CLOSED;
            if (r == MCO_ERR_IO) return MCO_ERR_IO;
        }
        if (listener->closed) return MCO_ERR_CLOSED;
    }
}

/* Probe a raw fd's type (adopt path only): recv/send vs read/write. Falls back
 * to STREAM on failure. */
static uint8_t fd_detect_type(int fd) {
#ifdef OS_WIN
    /* a SOCKET handle is outside the CRT stat world; use SO_TYPE */
    int t = 0;
    int tl = (int)sizeof(t);
    if (getsockopt((SOCKET)fd, SOL_SOCKET, SO_TYPE, (char*)&t, &tl) == 0)
        return (t == SOCK_DGRAM) ? MCO_SOCK_DGRAM : MCO_SOCK_STREAM;
    return MCO_SOCK_FILE;
#else
    struct stat st;
    if (fstat(fd, &st) == 0) {
        if (S_ISREG(st.st_mode) || S_ISFIFO(st.st_mode) ||
            S_ISCHR(st.st_mode) || S_ISDIR(st.st_mode))
            return MCO_SOCK_FILE;
        if (S_ISSOCK(st.st_mode)) {
#ifdef SO_TYPE
            int t = 0;
            socklen_t tl = sizeof(t);
            if (getsockopt(fd, SOL_SOCKET, SO_TYPE, (void*)&t, &tl) == 0 && t == SOCK_DGRAM)
                return MCO_SOCK_DGRAM;
#endif
            return MCO_SOCK_STREAM;
        }
    }
    return MCO_SOCK_STREAM;
#endif
}

/* Like sock_wrap() but does not close(fd) on failure — the caller still owns it.
 * The fd's origin is unknown (pipe/file/tty), so its type must be probed:
 * recv/send on a raw fd is always ENOTSOCK. */
mco_socket_t* mco_socket_adopt_fd(int fd) {
    mco_socket_t* s = sock_wrap_impl(fd, 0);
    if (s) s->type = fd_detect_type(fd);
    return s;
}

mco_socket_t* mco_tcp_accept(mco_socket_t* listener, int timeout_ms) {
    int fd = mco_tcp_accept_fd(listener, timeout_ms);
    if (fd < 0) return NULL;
    return sock_wrap(fd); /* closes fd on failure, as before */
}

/* Map a connect errno to MCO_CONNECT_*; anything else is IO with errno kept.
 * err comes from socket_errno(): on Windows these are WSA* values, not POSIX
 * constants, so the cases are listed per platform. */
static int classify_connect_errno(int err) {
    switch (err) {
#ifdef OS_WIN
    case WSAECONNREFUSED:  return MCO_CONNECT_REFUSED;
    case WSAENETUNREACH:   return MCO_CONNECT_UNREACH;
    case WSAEHOSTUNREACH:  return MCO_CONNECT_UNREACH;
#else
    case ECONNREFUSED:  return MCO_CONNECT_REFUSED;
    case ENETUNREACH:   return MCO_CONNECT_UNREACH;
    case EHOSTUNREACH:  return MCO_CONNECT_UNREACH;
#endif
    default:            return MCO_CONNECT_IO;
    }
}

/* Finish a non-blocking connect (shared by TCP and UDS). fd already had
 * connect() issued (cret is its return value); if unfinished, wait for the
 * outcome and take the real errno (completion path: mco_wait_connect; readiness
 * path: SO_ERROR). Returns the socket, or NULL with fd already closed.
 * addr/addrlen feed the CONNECT fast path; out_errno may be NULL. */
static mco_socket_t* finish_nonblocking_connect(int fd, int cret, mco_deadline_t dl,
                                                const void* addr, unsigned addrlen,
                                                int* out_errno) {
    mco_socket_t* s;

    if (cret != 0) {
        int e = socket_errno();
#ifdef OS_WIN
        if( e  != WSAEWOULDBLOCK) {
#else
        if (e != EINPROGRESS) {
#endif
            if (out_errno) *out_errno = classify_connect_errno(e);
            closesocket(fd);
            return NULL;
        }
    }

    s = sock_wrap(fd);
    if (!s) {
        if (out_errno) *out_errno = MCO_CONNECT_IO;
        closesocket(fd);
        return NULL;
    }

    if (cret == 0) return s; /* rare: connected immediately (loopback / UDS) */

    {
        int conn_err = 0;
        int r = mco_wait_connect(fd, addr, addrlen, mco_deadline_ms(dl), &conn_err);
        int err = conn_err;
        socklen_t errlen = sizeof(err);

        if (r == MCO_ERR_TIMEOUT || r == MCO_ERR_CLOSED) {
            if (out_errno) *out_errno = MCO_CONNECT_TIMEOUT;
            mco_close(s); mco_socket_destroy(s); return NULL;
        }
        /* Trust the errno from the CQE: io_uring leaves sk_err unset, so SO_ERROR
         * would report a false 0. The readiness path uses SO_ERROR. */
        if (err == 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &errlen) != 0) err = socket_errno();
        if (err != 0) {
            if (out_errno) *out_errno = classify_connect_errno(err);
            mco_close(s); mco_socket_destroy(s); return NULL;
        }
        return s; /* connected */
    }
}

mco_socket_t* mco_tcp_connect(const char* host, int port, int timeout_ms, int* out_errno) {
    mco_deadline_t dl;
    sockaddr_u addr;
    int fd;
    int cret;

    if (out_errno) *out_errno = MCO_CONNECT_OK;
    if (!host || port <= 0 || port > 65535) {
        if (out_errno) *out_errno = MCO_CONNECT_INVALID;
        return NULL;
    }

    memset(&addr, 0, sizeof(addr));
    /* Async resolution (background pool, does not block the vcpu); dl is the
     * resolve+connect budget. */
    dl = mco_deadline_after(timeout_ms);
    if (mco_resolve(host, &addr, mco_deadline_ms(dl)) != MCO_RESOLVE_OK) {
        /* Both failure and timeout map to DNS: callers treat them the same. */
        if (out_errno) *out_errno = MCO_CONNECT_DNS;
        return NULL;
    }
    sockaddr_set_port(&addr, port);

    fd = hsocket(addr.sa.sa_family, SOCK_STREAM, 0);
    if (fd < 0) {
        if (out_errno) *out_errno = MCO_CONNECT_IO;
        return NULL;
    }
    nonblocking(fd);

    cret = connect(fd, &addr.sa, sockaddr_len(&addr));
    return finish_nonblocking_connect(fd, cret, dl, &addr.sa, (unsigned)sockaddr_len(&addr), out_errno);
}

/* ---- Unix Domain Socket ---- */

mco_socket_t* mco_tcp_listen_unix(const char* path) {
#if ENABLE_UDS
    int fd = ListenUnix(path);
    if (fd < 0) return NULL;
    return sock_wrap(fd);
#else
    (void)path;
    return NULL; /* built without ENABLE_UDS */
#endif
}

mco_socket_t* mco_tcp_connect_unix(const char* path, int timeout_ms) {
#if ENABLE_UDS
    mco_deadline_t dl = mco_deadline_after(timeout_ms);
    sockaddr_u addr;
    int fd, cret;

    memset(&addr, 0, sizeof(addr));
    sockaddr_set_path(&addr, path);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    nonblocking(fd);

    cret = connect(fd, &addr.sa, sockaddr_len(&addr));
    /* UDS does not classify failures: out_errno is NULL */
    return finish_nonblocking_connect(fd, cret, dl, &addr.sa, (unsigned)sockaddr_len(&addr), NULL);
#else
    (void)path; (void)timeout_ms;
    return NULL;
#endif
}

/* ---- UDP ---- */

mco_socket_t* mco_udp_bind(const char* host, int port) {
    int fd = Bind(port, host ? host : "0.0.0.0", SOCK_DGRAM);
    mco_socket_t* s;
    if (fd < 0) return NULL;
    s = sock_wrap(fd);
    if (s) s->type = MCO_SOCK_DGRAM;  /* dgram：recvfrom/sendto */
    return s;
}

mco_socket_t* mco_udp_socket(void) {
    /* Lazy: the socket is created at the first sendto, once the family is known
     * (dual stack for free). recvfrom before that fails. Use mco_udp_bind() for
     * receive-only. */
    mco_socket_t* s = (mco_socket_t*)calloc(1, sizeof(mco_socket_t));
    if (!s) return NULL;
    s->fd = -1;
    s->type = MCO_SOCK_DGRAM;          /* dgram：recvfrom/sendto */
    s->default_timeout_ms = -1;
    return s;
}

int mco_udp_sendto(mco_socket_t* s, const void* buf, int len,
                    const char* host, int port, int timeout_ms) {
    mco_deadline_t dl = mco_deadline_after(timeout_ms);
    sockaddr_u addr;

    if (!s || s->closed) return MCO_ERR_CLOSED;
    if (s->wr_waiter) return MCO_ERR_BUSY;

    memset(&addr, 0, sizeof(addr));
    if (mco_resolve(host, &addr, mco_deadline_ms(dl)) != MCO_RESOLVE_OK) return MCO_ERR_ADDR;
    sockaddr_set_port(&addr, port);

    if (s->fd < 0) {
        /* Lazy path: the family is only known now. */
        int fd = hsocket(addr.sa.sa_family, SOCK_DGRAM, 0);
        if (fd < 0) return MCO_ERR_IO;
        nonblocking(fd);
        s->fd = fd;
    }

    for (;;) {
        ssize_t n = sendto(s->fd, buf, (size_t)len, 0, &addr.sa, sockaddr_len(&addr));
        int e;
        if (n >= 0) return (int)n;
        e = socket_errno();
        if (e == EINTR) continue;
        /* also hit on a family mismatch: a UDP socket is fixed once created */
        if (e != EAGAIN && e != EWOULDBLOCK) return MCO_ERR_IO;

        s->wr_waiter = mco_task_self();
        {
            int r = mco_wait_io(s->fd, MCO_EV_WRITE, mco_deadline_ms(dl));
            s->wr_waiter = NULL;
            if (r == MCO_ERR_TIMEOUT) return MCO_ERR_TIMEOUT;
            if (r == MCO_ERR_CLOSED) return MCO_ERR_CLOSED;
        }
        if (s->closed) return MCO_ERR_CLOSED;
    }
}

int mco_udp_recvfrom(mco_socket_t* s, void* buf, int len,
                      char* from_host, int from_host_len, int* from_port,
                      int timeout_ms) {
    if (!s || s->closed) return MCO_ERR_CLOSED;
    if (s->rd_waiter) return MCO_ERR_BUSY;
    if (s->fd < 0) return MCO_ERR_IO; /* a lazy socket has no fd until first send */

    for (;;) {
        sockaddr_u addr;
        socklen_t addrlen = sizeof(addr);
        ssize_t n;
        memset(&addr, 0, sizeof(addr));

        n = recvfrom(s->fd, buf, (size_t)len, 0, &addr.sa, &addrlen);
        if (n >= 0) {
            if (from_host) sockaddr_ip(&addr, from_host, from_host_len);
            if (from_port) *from_port = (int)sockaddr_port(&addr);
            return (int)n; /* UDP has no EOF; a 0-byte datagram is valid */
        }
        {
            int e = socket_errno();
            if (e == EINTR) continue;
            if (e != EAGAIN && e != EWOULDBLOCK) return MCO_ERR_IO;
        }

        s->rd_waiter = mco_task_self();
        {
            int r = mco_wait_io(s->fd, MCO_EV_READ, timeout_ms);
            s->rd_waiter = NULL;
            if (r == MCO_ERR_TIMEOUT) return MCO_ERR_TIMEOUT;
            if (r == MCO_ERR_CLOSED) return MCO_ERR_CLOSED;
        }
        if (s->closed) return MCO_ERR_CLOSED;
    }
}

/* ---- SSL / TLS ---- */

mco_ssl_ctx_t mco_ssl_ctx_new_client(const char* ca_file, const char* ca_path, int verify_peer) {
    hssl_ctx_opt_t opt;
    if (!HV_WITH_SSL) return NULL; /* no real SSL backend: never silently plaintext */
    memset(&opt, 0, sizeof(opt));
    opt.endpoint = HSSL_CLIENT;
    opt.ca_file = ca_file;
    opt.ca_path = ca_path;
    opt.verify_peer = verify_peer ? 1 : 0;
    return (mco_ssl_ctx_t)hssl_ctx_new(&opt);
}

mco_ssl_ctx_t mco_ssl_ctx_new_server(const char* crt_file, const char* key_file, int verify_peer) {
    hssl_ctx_opt_t opt;
    if (!HV_WITH_SSL) return NULL;
    memset(&opt, 0, sizeof(opt));
    opt.endpoint = HSSL_SERVER;
    opt.crt_file = crt_file;
    opt.key_file = key_file;
    opt.verify_peer = verify_peer ? 1 : 0;
    return (mco_ssl_ctx_t)hssl_ctx_new(&opt);
}

void mco_ssl_ctx_free(mco_ssl_ctx_t ctx) {
    if (ctx) hssl_ctx_free((hssl_ctx_t)ctx);
}

/* Handshake driver shared by client and server: call step_fn (hssl_connect or
 * hssl_accept) and mco_wait_io in the requested direction until done. */
static int ssl_handshake_loop(mco_socket_t* s, int (*step_fn)(hssl_t), mco_deadline_t dl) {
    /* Own the SSL object for the whole handshake so no other coroutine touches
     * the underlying SSL API concurrently. */
    s->ssl_cross_owner = mco_task_self();

    for (;;) {
        int ret = step_fn(s->ssl);
        int events;

        if (ret == 0) { ssl_cross_owner_release(s); return 0; } /* handshake done */
        if (ret == HSSL_WANT_READ) events = MCO_EV_READ;
        else if (ret == HSSL_WANT_WRITE) events = MCO_EV_WRITE;
        else { ssl_cross_owner_release(s); return MCO_ERR_IO; } /* cert/protocol/peer */

        if (s->rd_waiter) { ssl_cross_owner_release(s); return MCO_ERR_BUSY; } /* handshake in progress */
        s->rd_waiter = mco_task_self();
        {
            int r = mco_wait_io(s->fd, events, mco_deadline_ms(dl));
            s->rd_waiter = NULL;
            if (r == MCO_ERR_TIMEOUT) { ssl_cross_owner_release(s); return MCO_ERR_TIMEOUT; }
            if (r == MCO_ERR_CLOSED) { ssl_cross_owner_release(s); return MCO_ERR_CLOSED; }
        }
        if (s->closed) { ssl_cross_owner_release(s); return MCO_ERR_CLOSED; }
    }
}

int mco_ssl_client_handshake(mco_socket_t* s, mco_ssl_ctx_t ctx, const char* sni_hostname, int timeout_ms) {
    mco_deadline_t dl = mco_deadline_after(timeout_ms);
    int result;

    if (!s || s->closed) return MCO_ERR_CLOSED;
    if (s->ssl) return MCO_ERR_BUSY; /* already handshaken */

    s->ssl = hssl_new((hssl_ctx_t)(ctx ? ctx : hssl_ctx_instance()), s->fd, 0 /* is_server */);
    if (!s->ssl) return MCO_ERR_IO;
    if (sni_hostname) hssl_set_sni_hostname(s->ssl, sni_hostname);

    result = ssl_handshake_loop(s, hssl_connect, dl);
    if (result != 0) { hssl_free(s->ssl); s->ssl = NULL; }
    return result;
}

int mco_ssl_server_handshake(mco_socket_t* s, mco_ssl_ctx_t ctx, int timeout_ms) {
    mco_deadline_t dl = mco_deadline_after(timeout_ms);
    int result;

    if (!s || s->closed) return MCO_ERR_CLOSED;
    if (s->ssl) return MCO_ERR_BUSY;
    if (!ctx) return MCO_ERR_ADDR; /* a server has no default cert; ctx required */

    s->ssl = hssl_new((hssl_ctx_t)ctx, s->fd, 1 /* is_server */);
    if (!s->ssl) return MCO_ERR_IO;

    result = ssl_handshake_loop(s, hssl_accept, dl);
    if (result != 0) { hssl_free(s->ssl); s->ssl = NULL; }
    return result;
}

mco_socket_t* mco_tls_connect(const char* host, int port, mco_ssl_ctx_t ctx, int timeout_ms, int* out_errno) {
    mco_deadline_t dl = mco_deadline_after(timeout_ms);
    mco_socket_t* s = mco_tcp_connect(host, port, mco_deadline_ms(dl), out_errno);
    int handshaken;

    if (!s) return NULL; /* connect failed; mco_tcp_connect set out_errno */

    handshaken = mco_ssl_client_handshake(s, ctx, host, mco_deadline_ms(dl));
    if (handshaken != 0) {
        /* TCP connected but the handshake failed; reported as MCO_CONNECT_TLS. */
        if (out_errno) *out_errno = MCO_CONNECT_TLS;
        mco_close(s);
        mco_socket_destroy(s);
        return NULL;
    }
    return s;
}

#if MINICONET_TEST_HOOKS
/* Test-only: force "another coroutine owns the SSL object" to exercise the
 * ssl_cross_owner guard. */
void mco_test_socket_set_ssl_cross_owner(mco_socket_t* s, void* owner) {
    if (s) s->ssl_cross_owner = (mco_task_t*)owner;
}
#endif
