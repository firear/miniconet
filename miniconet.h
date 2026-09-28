/*
 * miniconet.h — coroutine-based network library API.
 *  1. mco_read()/mco_write() use the caller's buf directly, no buffering.
 *  2. >=0 = bytes transferred (0 = EOF), <0 = MCO_ERR_*.
 *  3. mco_close() wakes every waiter with MCO_ERR_CLOSED; nothing hangs.
 *  4. One waiter per socket direction; others get MCO_ERR_BUSY.
 */
#ifndef MINICONET_H
#define MINICONET_H

#include "compat/hsocket.h"
#include "core/mco_vcpu.h"
#ifdef MINICONET_HAVE_GENERATED_CONFIG
#include "miniconet_generated/miniconet_config.h"
#else
#include "miniconet_config.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mco_socket mco_socket_t;

#define MCO_ERR_IO       (-3)   /* real syscall error; check errno */
#define MCO_ERR_BUSY     (-4)   /* a coroutine already waits on that direction */
#define MCO_ERR_ADDR     (-5)   /* address resolve/bind failed */
/* MCO_ERR_TIMEOUT(-1) / MCO_ERR_CLOSED(-2) are in mco_vcpu.h */

/* ---- connect failure classes (out_errno of mco_tcp_connect()) ----
 * Only what callers treat differently: SOCKS5 maps them to REP (refused=5 /
 * unreachable=3,4 / other=1), an HTTP proxy to 502 vs 504. MCO_CONNECT_IO keeps
 * the real errno. Not written on success. */
#define MCO_CONNECT_OK        0
#define MCO_CONNECT_INVALID   1  /* bad args (empty host / port out of range) */
#define MCO_CONNECT_DNS       2  /* resolution failed or timed out */
#define MCO_CONNECT_REFUSED   3  /* ECONNREFUSED: nothing listening */
#define MCO_CONNECT_UNREACH   4  /* ENETUNREACH / EHOSTUNREACH */
#define MCO_CONNECT_TIMEOUT   5  /* resolve+connect budget exhausted */
#define MCO_CONNECT_TLS       6  /* TCP ok, TLS handshake failed */
#define MCO_CONNECT_IO        7  /* other syscall error; errno is valid */

/* ---- coroutines / event loop (thin wrappers over core/mco_vcpu) ---- */
typedef mco_vcpu_t mco_loop_t;

mco_loop_t* mco_loop_new(void);
void        mco_loop_run(mco_loop_t* loop);
void        mco_loop_stop(mco_loop_t* loop);
void        mco_loop_free(mco_loop_t* loop);

mco_task_t* mco_spawn(mco_loop_t* loop, void (*fn)(void* arg), void* arg);
#define mco_sleep(ms) mco_sleep_ms(ms)
/* ---- TCP ---- */

/* Listen; NULL on failure. Resolution is synchronous: call from main() before
 * mco_loop_run(), never inside a coroutine with a real hostname. */
mco_socket_t* mco_tcp_listen(const char* host, int port);

/* Accept one connection; timeout_ms<0 waits forever. NULL on failure. */
mco_socket_t* mco_tcp_accept(mco_socket_t* listener, int timeout_ms);

/* Accept without wrapping: returns the raw fd (>=0), no poller registration.
 * MCO_ERR_TIMEOUT/CLOSED/IO on failure. Adopt or close it soon — a raw fd is not
 * managed by any vcpu. TCP/UDS listeners only. */
int mco_tcp_accept_fd(mco_socket_t* listener, int timeout_ms);

/* Wrap a raw fd into an mco_socket_t owned by the current vcpu (set
 * non-blocking, register with its poller). Must be called in a coroutine of the
 * target thread. Works for any fd (TCP/UDP/UDS/pipe); NULL on failure, the
 * caller still owns fd. Limited to fd types the platform poller supports:
 * wepoll only accepts Winsock sockets. */
mco_socket_t* mco_socket_adopt_fd(int fd);

/* Connect; NULL on failure. Resolution is async (see core/mco_resolver.h) and
 * does not block the vcpu. Coroutine-only. timeout_ms covers resolve + connect.
 * out_errno may be NULL; otherwise the failure class (MCO_CONNECT_*) is written
 * there. */
mco_socket_t* mco_tcp_connect(const char* host, int port, int timeout_ms, int* out_errno);

/* >=0 = bytes transferred (0 = EOF), <0 = MCO_ERR_*. */
int mco_read(mco_socket_t* s, void* buf, int len, int timeout_ms);
int mco_write(mco_socket_t* s, const void* buf, int len, int timeout_ms);

/* ---- Read/write exactly N bytes (for headers and fixed-size fields) ----
 * mco_read/mco_write are one syscall each, so short transfers are normal (a TLS
 * record at a time). These loop internally:
 *   mco_read_exact: len = full; 0..len-1 = hit EOF midway (connection
 *                   half-closed, no more data); <0 = MCO_ERR_*.
 *   mco_write_all : len = all written; <0 = MCO_ERR_* (treat as failure).
 * timeout_ms is the budget for the whole operation, spread over each call. */
int mco_read_exact(mco_socket_t* s, void* buf, int len, int timeout_ms);
int mco_write_all(mco_socket_t* s, const void* buf, int len, int timeout_ms);

/* ---- Timeouts ----
 * timeout_ms only covers the current call. To share one budget across an
 * operation (resolve + connect + first read) use the deadline helpers in
 * core/mco_timer.h:
 *   mco_deadline_t dl = mco_deadline_after(3000);
 *   mco_socket_t* c = mco_tcp_connect(host, port, mco_deadline_ms(dl));
 *   mco_read(c, buf, len, mco_deadline_ms(dl));
 * Per-socket defaults: mco_socket_set_timeout() + mco_read_default(). */

/* Default timeout for this socket in ms; <0 = none (the initial value). */
void mco_socket_set_timeout(mco_socket_t* s, int timeout_ms);

/* TCP_NODELAY (proxies and interactive protocols want it); no-op on non-TCP. */
void mco_socket_set_nodelay(mco_socket_t* s, int on);
/* mco_read() with this socket's default timeout. */
int mco_read_default(mco_socket_t* s, void* buf, int len);
/* mco_write() with this socket's default timeout. */
int mco_write_default(mco_socket_t* s, const void* buf, int len);

/* ---- UDP ---- */

/* Bind a local UDP socket (receive side). Resolution is synchronous. NULL on
 * failure. */
mco_socket_t* mco_udp_bind(const char* host, int port);

/* Unbound UDP socket (send-first clients); the local port is kernel-assigned.
 * The family is decided lazily at the first mco_udp_sendto(), so dual stack
 * works. recvfrom before any sendto returns MCO_ERR_IO. Use mco_udp_bind() for
 * receive-only. */
mco_socket_t* mco_udp_socket(void);

/* Send a datagram (async resolution, coroutine-only). >=0 = bytes accepted by
 * the kernel, not delivered; <0 = MCO_ERR_*. */
int mco_udp_sendto(mco_socket_t* s, const void* buf, int len,
                    const char* host, int port, int timeout_ms);

/* Receive a datagram. >=0 = bytes (a 0-byte datagram is valid); <0 = MCO_ERR_*.
 * from_host/from_port are optional sender addresses. */
int mco_udp_recvfrom(mco_socket_t* s, void* buf, int len,
                      char* from_host, int from_host_len, int* from_port,
                      int timeout_ms);

/* Half-close; how is MCO_SHUT_RD/WR/RDWR (0/1/2, same as POSIX SHUT_* and
 * Winsock SD_*). Classic proxy use: after EOF on one direction, shutdown(WR) the
 * other end while still reading the trailing response.
 * Only does an fd-level shutdown: no waiter is woken and closed is not set, so
 * the caller must still mco_close(). 0 on success, <0 with errno set. */
#define MCO_SHUT_RD    0
#define MCO_SHUT_WR    1
#define MCO_SHUT_RDWR  2
int mco_shutdown(mco_socket_t* s, int how);

/* Close the fd and wake all waiters; the struct itself is not freed. */
void mco_close(mco_socket_t* s);
/* Free the struct; no coroutine may still wait on or use this socket (normally
 * right after mco_close(), by the owning coroutine). */
void mco_socket_destroy(mco_socket_t* s);

int mco_socket_fd(mco_socket_t* s);

/* Fill sockaddr_u (compat/hsocket.h) with the local/peer address. Returns a
 * struct rather than a string to keep the family and avoid buffer sizing; use
 * sockaddr_ip()/sockaddr_port()/sockaddr_str()/sockaddr_compare() for strings.
 * 0 on success, <0 on failure (peer address of an unconnected socket fails). */
int mco_socket_local_addr(mco_socket_t* s, sockaddr_u* out);
int mco_socket_peer_addr(mco_socket_t* s, sockaddr_u* out);

/* Non-zero if this connection is TLS. */
int mco_socket_is_ssl(mco_socket_t* s);

/* ---- Unix Domain Socket ----
 *
 * Requires ENABLE_UDS at build time (CMake: -DMINICONET_ENABLE_UDS=ON); without
 * it these stay linkable and return NULL. */
mco_socket_t* mco_tcp_listen_unix(const char* path);
mco_socket_t* mco_tcp_connect_unix(const char* path, int timeout_ms);

/* ---- SSL / TLS ----
 *
 * Wraps compat/ssl/hssl.h (only the openssl backend is built by default, see
 * MINICONET_WITH_OPENSSL). The handshake runs through mco_wait_io and does not
 * block other coroutines; after it, mco_read/mco_write carry ciphertext
 * transparently. TCP only (no DTLS).
 *
 * Client:
 *   mco_ssl_ctx_t ctx = mco_ssl_ctx_new_client(NULL, NULL, 0);
 *   mco_socket_t* s = mco_tls_connect("example.com", 443, ctx, 3000);
 *   mco_write(s, ...); mco_read(s, ...);
 * Server:
 *   mco_ssl_ctx_t ctx = mco_ssl_ctx_new_server("server.crt", "server.key", 0);
 *   mco_socket_t* conn = mco_tcp_accept(listener, -1);
 *   mco_ssl_server_handshake(conn, ctx, 3000);
 */
typedef void* mco_ssl_ctx_t;

/* verify_peer non-zero verifies the peer certificate; ca_file/ca_path may both be
 * NULL (falls back to the system trust store). NULL on failure, including when
 * the library has no SSL backend — never silently plaintext. */
mco_ssl_ctx_t mco_ssl_ctx_new_client(const char* ca_file, const char* ca_path, int verify_peer);
/* PEM certificate and key paths, both required for a server. NULL on failure. */
mco_ssl_ctx_t mco_ssl_ctx_new_server(const char* crt_file, const char* key_file, int verify_peer);
void mco_ssl_ctx_free(mco_ssl_ctx_t ctx);

/* Upgrade an established TCP connection to TLS (client side). ctx NULL uses the
 * process default (no verification, development only). sni_hostname may be NULL.
 * 0 on success, <0 = MCO_ERR_*. Coroutine-only. */
int mco_ssl_client_handshake(mco_socket_t* s, mco_ssl_ctx_t ctx, const char* sni_hostname, int timeout_ms);
/* Server side; ctx must come from mco_ssl_ctx_new_server(). */
int mco_ssl_server_handshake(mco_socket_t* s, mco_ssl_ctx_t ctx, int timeout_ms);

/* Resolve + connect + TLS handshake in one call (SNI from host). timeout_ms
 * covers everything. NULL on failure. out_errno uses the same MCO_CONNECT_* as
 * mco_tcp_connect(), with MCO_CONNECT_TLS when the handshake fails. */
mco_socket_t* mco_tls_connect(const char* host, int port, mco_ssl_ctx_t ctx, int timeout_ms, int* out_errno);

#ifdef __cplusplus
}
#endif
#endif /* MINICONET_H */
