/*
 * mco_vcpu.h — symmetric scheduler: one vcpu per OS thread running minicoro
 * coroutines. Drains the ready queue before engine->poll().
 */
#ifndef MCO_VCPU_H
#define MCO_VCPU_H

#include "mco_engine.h"
#include "mco_timer.h"
#include "../minicoro/minicoro.h"
#include <errno.h>
#include "../compat/hsocket.h"   /* socket_errno() */

/* Feature switches (generated/fallback gate, always 0/1). */
#ifdef MINICONET_HAVE_GENERATED_CONFIG
#include "miniconet_generated/miniconet_config.h"
#else
#include "miniconet_config.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mco_vcpu mco_vcpu_t;

/* Hot fields (scheduler + io wait) packed into the first 64B cache line. */
typedef struct mco_task {
    mco_coro*          co;
    mco_vcpu_t*        vcpu;
    struct mco_task*   ready_next;
    uint64_t           async_cookie; /* engine cookie from submit_* */
    /* io wait state (shared by readiness and completion paths) */
    int                wait_fd;
    int                wait_events;
    int                wait_result;  /* MCO_EV_* mask or byte count, else MCO_ERR_* */
    int                wait_errno;   /* real errno from the completion (-res); uring
                                        CONNECT leaves sk_err at 0, so the CQE is the
                                        only source of ECONNREFUSED */
    int8_t             blocked;      /* counted in vcpu->nblocked */
    int8_t             in_ready;     /* guards double enqueue */
    int8_t             wait_kind;    /* MCO_WAIT_* */
    int8_t             wait_final;   /* conclusion recorded in DRAIN */
    /* "sync done" lives on the submit cookie, not here, so it cannot leak. */

    void (*entry)(void* arg);
    void* entry_arg;

    /* Embedded wait timer node, reused serially by wait_io/sleep. */
    mco_timer_t        wait_timer_obj;
} mco_task_t;

/* io wait kinds (wait_kind).
 * ASYNC: stay suspended until the CQE arrives, or the kernel may write to the
 * reused stack. Timeout/close go to DRAIN (cancel, resume on the CQE). */
#define MCO_WAIT_NONE  0   /* not waiting */
#define MCO_WAIT_IO    1   /* readiness path */
#define MCO_WAIT_ASYNC 2   /* completion op in flight (buf on stack) */
#define MCO_WAIT_DRAIN 3   /* decided, waiting for the CQE */

#define MCO_ERR_TIMEOUT  (-1)
#define MCO_ERR_CLOSED   (-2)   /* fd closed or cancelled */
#define MCO_ERR_IO       (-3)   /* kept in sync with miniconet.h */

/* Current thread's vcpu; lazily created with the default engine. */
mco_vcpu_t* mco_vcpu_current(void);

/* Current thread's vcpu, or NULL if this thread has none. Unlike
 * mco_vcpu_current() it never creates one, so it is safe to call from a plain
 * worker thread (creating an engine there would leak a poll fd per thread). */
mco_vcpu_t* mco_vcpu_self(void);

/* Explicit create/destroy (normally handled by mco_vcpu_current()). */
mco_vcpu_t* mco_vcpu_new(const mco_engine_ops_t* ops);
void        mco_vcpu_free(mco_vcpu_t* vcpu);

/* Max cached stacks (default 64); 0 disables pooling. */
void mco_vcpu_set_stack_pool_limit(mco_vcpu_t* vcpu, int max_free);

/* Total stack reuse count (tests/observability). */
long mco_vcpu_stack_pool_reuse_count(mco_vcpu_t* vcpu);

/* Zeroing policy on stack reuse:
 *   NONE (default): minicoro memsets coroutine + context, stack is write-first.
 *   FULL          : clear the whole block (safe, slower).
 * With NONE a coroutine can read the previous one's remains; use FULL if that
 * matters. */
#define MCO_STACK_ZERO_NONE  0
#define MCO_STACK_ZERO_FULL   1
void mco_vcpu_set_stack_zero_mode(mco_vcpu_t* vcpu, int mode);

/* Create a coroutine and enqueue it (runs when scheduled). */
mco_task_t* mco_go(mco_vcpu_t* vcpu, void (*fn)(void* arg), void* arg);

/* Run until no live coroutines remain or mco_vcpu_stop(). */
void mco_vcpu_run(mco_vcpu_t* vcpu);
void mco_vcpu_stop(mco_vcpu_t* vcpu);

/* Currently running task (coroutine context only). */
mco_task_t* mco_task_self(void);

/* ---- Blocking primitives for coroutines (yield; scheduler resumes) ---- */

/* Wait for events or timeout_ms (<0 = none). Returns the MCO_EV_* mask, or
 * MCO_ERR_TIMEOUT/MCO_ERR_CLOSED. */
int  mco_wait_io(int fd, int events, int timeout_ms);

/* ---- Completion-style io (io_uring fast path, else readiness + syscall).
 * timeout_ms==0 returns timeout at once: try a non-blocking syscall first. ---- */

/* fd type: selects the syscall set; must match the probe path in miniconet.c.
 *   STREAM (TCP/TLS/Unix stream)     -> recv()/send()
 *   DGRAM （UDP）                    → recvfrom()/sendto()
 *   FILE   (pipe/file/tty/raw fd)    -> read()/write()
 * Raw fds are ENOTSOCK for recv/send on every platform.
 * 0 = STREAM, so a calloc'ed mco_socket defaults to TCP. */
typedef enum {
    MCO_SOCK_STREAM = 0,
    MCO_SOCK_DGRAM  = 1,
    MCO_SOCK_FILE   = 2,
} mco_sock_type_e;

/* Raw fd (MCO_SOCK_FILE) io: read/write, or _read/_write on MSVC. */
#ifdef _MSC_VER
#  include <io.h>
#  define MCO_RAW_READ(fd, buf, n)  _read((fd), (buf), (unsigned)(n))
#  define MCO_RAW_WRITE(fd, buf, n) _write((fd), (buf), (unsigned)(n))
#else
#  include <unistd.h>
#  define MCO_RAW_READ(fd, buf, n)  read((fd), (buf), (size_t)(n))
#  define MCO_RAW_WRITE(fd, buf, n) write((fd), (buf), (size_t)(n))
#endif

/* Raw fd errno; Windows socket errors live in WSAGetLastError(). */
static inline int mco_io_errno(int type) {
    return (type == MCO_SOCK_FILE) ? errno : socket_errno();
}

/* Wait for one read: bytes read (0 = EOF) or MCO_ERR_*. */
int  mco_wait_recv(int fd, void* buf, int len, int type, int timeout_ms);
/* Wait for one write: bytes written (may be partial) or MCO_ERR_*. */
int  mco_wait_send(int fd, const void* buf, int len, int type, int timeout_ms);
/* Finish a connect() already in progress (EINPROGRESS); addr only feeds the
 * CONNECT SQE. 0 = settled: *out_errno (may be NULL) 0 = ok, else real errno.
 * MCO_ERR_* = the wait timed out; ignore *out_errno then. */
int  mco_wait_connect(int fd, const void* addr, unsigned addrlen, int timeout_ms, int* out_errno);
/* Wait for one accept: new fd (>=0) or MCO_ERR_*. */
int  mco_wait_accept(int fd, int timeout_ms);
/* Whether the engine needs completion-style accept (AcceptEx): IOCP only, so
 * mco_tcp_accept_fd skips BSD accept(). Constant 0 when MINICONET_WITH_IOCP=0;
 * otherwise a runtime check, since iocp and wepoll vcpus can coexist. */
#if !MINICONET_WITH_IOCP
#define mco_accept_uses_async() 0
#else
int  mco_accept_uses_async(void);
#endif

/* Wake an io-blocked task with MCO_ERR_CLOSED (on close); safe no-op. */
void mco_task_wake_closed(mco_task_t* task);

/* Requeue a parked task on its own vcpu. Same thread only: the ready queue is
 * plain pointers, so a foreign thread must signal the engine's wake channel
 * instead. Never call it on mco_task_self() — a running task has no saved
 * context to resume into and would be resumed twice. */
void mco_task_wake(mco_task_t* task);

/* Sleep ms; ms==0 requeues and yields. */
void mco_sleep_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif
#endif /* MCO_VCPU_H */
