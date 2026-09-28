#include "mco_vcpu.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#define MINICORO_IMPL
#include "minicoro.h"

/* Pooling is off under ASan: minicoro's fiber redzones are not reset on reuse,
 * false-reporting stack-buffer-underflow. Falls back to calloc/free. */
#if defined(__has_feature)
  #if __has_feature(address_sanitizer)
    #define MCO_VCPU_ASAN_BUILD 1
  #endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(MCO_VCPU_ASAN_BUILD)
  #define MCO_VCPU_ASAN_BUILD 1
#endif
#ifndef MCO_VCPU_ASAN_BUILD
  #define MCO_VCPU_ASAN_BUILD 0
#endif

#define MCO_STACK_SIZE 96*1024

/* Stack pool: reuse the "struct + stack" block after a coroutine ends. Only the
 * default size is cached; others use malloc/free. */
typedef struct mco_stack_pool {
    void*  free_list;   /* singly linked; next stored in the block itself */
    size_t block_size;  /* cached block size; 0 = not fixed yet */
    int    free_count;
    int    max_free;    /* cap; beyond it blocks are really freed */
    long   reuse_count; /* reuse count (tests/observability) */
    int    zero_mode;   /* MCO_STACK_ZERO_* */
} mco_stack_pool_t;

/* Zeroing on reuse: NONE (default) is correct, not just fast — minicoro memsets
 * the coroutine and context itself, storage/stack are write-first. FULL clears
 * the whole block (safe, slower). With NONE a coroutine can read the previous
 * one's remains; use FULL for sensitive data. (MCO_STACK_ZERO_* live in
 * mco_vcpu.h.) */

/* Hot members share the first cache line; heap/pool (cold) go last. */
struct mco_vcpu {
    const mco_engine_ops_t* ops;
    mco_engine_t*           engine;
    mco_task_t*             ready_head;
    mco_task_t*             ready_tail;
    mco_task_t*             current;   /* task being resumed; NULL outside coroutines */
    mco_task_t*             task_free;  /* dead task struct reuse list */
    int                     ntasks;    /* live coroutines */
    int                     ntask_free;
    int                     nblocked;   /* coroutines blocked on io/timers */
    int8_t                  stopping;
    mco_timer_heap_t        timers;
    mco_stack_pool_t        stack_pool;
};

#define MCO_STACK_POOL_DEFAULT_MAX_FREE 64
#define MCO_TASK_POOL_MAX_FREE          64

/* Max resumes per drain round; without it a self-requeuing coroutine (e.g.
 * mco_sleep_ms(0) spinning) would starve timers and io. */
#define MCO_DRAIN_BUDGET 64

/* ---- thread-local current vcpu ---- */
#if defined(_MSC_VER)
  #define MCO_THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
  #define MCO_THREAD_LOCAL __thread
#else
  #define MCO_THREAD_LOCAL /* plain global in single-threaded builds */
#endif

static MCO_THREAD_LOCAL mco_vcpu_t* g_cur_vcpu = NULL;

/* ---- ready queue ---- */

static void push_ready(mco_vcpu_t* vcpu, mco_task_t* task) {
    if (task->in_ready) return; /* already queued */
    if (task->blocked) { task->blocked = 0; vcpu->nblocked--; }
    task->in_ready = 1;
    task->ready_next = NULL;
    if (vcpu->ready_tail) vcpu->ready_tail->ready_next = task;
    else vcpu->ready_head = task;
    vcpu->ready_tail = task;
}

static mco_task_t* pop_ready(mco_vcpu_t* vcpu) {
    mco_task_t* task = vcpu->ready_head;
    if (!task) return NULL;
    vcpu->ready_head = task->ready_next;
    if (!vcpu->ready_head) vcpu->ready_tail = NULL;
    task->ready_next = NULL;
    task->in_ready = 0;
    return task;
}

/* ---- coroutine switch (scheduler hot path) ----
 *
 * Uses minicoro internals (_mco_prepare_jump* /_mco_switch), skipping the checks
 * meant for generic callers. The scheduler guarantees the contract: the outgoing
 * task is RUNNING and we always switch from the scheduler stack (prev_co NULL).
 * No stack-overflow probe: the entry point is fixed, so it would only cost a
 * volatile read and three compares per switch.
 *
 * blocked accounting decides whether the loop may skip poll: a plain yield
 * (already queued) is not blocked. push_ready clears it, and every wakeup path
 * goes through push_ready, so nothing is missed.
 */
static inline void task_yield(mco_vcpu_t* vcpu, mco_task_t* task) {
    if (!task->in_ready) { task->blocked = 1; vcpu->nblocked++; }
    _mco_jumpout(task->co);
}

static inline void task_resume(mco_task_t* task) {
    _mco_jumpin(task->co);
}

/* ---- vcpu lifetime ---- */

mco_vcpu_t* mco_vcpu_new(const mco_engine_ops_t* ops) {
    mco_vcpu_t* vcpu = (mco_vcpu_t*)calloc(1, sizeof(mco_vcpu_t));
    if (!vcpu) return NULL;
    vcpu->ops = ops ? ops : mco_engine_default();
    vcpu->engine = vcpu->ops->create();
    if (!vcpu->engine) { free(vcpu); return NULL; }
    mco_timer_heap_init(&vcpu->timers);
    vcpu->stack_pool.max_free = MCO_STACK_POOL_DEFAULT_MAX_FREE;
    vcpu->stack_pool.zero_mode = MCO_STACK_ZERO_NONE; /* see pool_alloc */
    return vcpu;
}

static void stack_pool_drain(mco_stack_pool_t* pool) {
    while (pool->free_list) {
        void* p = pool->free_list;
        pool->free_list = *(void**)p;
        free(p);
    }
    pool->free_count = 0;
}

/* Dead task structs are pooled for the next mco_go() (stacks go to the pool). */
static void task_pool_drain(mco_vcpu_t* vcpu) {
    while (vcpu->task_free) {
        mco_task_t* t = vcpu->task_free;
        vcpu->task_free = t->ready_next;
        free(t);
    }
    vcpu->ntask_free = 0;
}

void mco_vcpu_free(mco_vcpu_t* vcpu) {
    if (!vcpu) return;
    vcpu->ops->destroy(vcpu->engine);
    stack_pool_drain(&vcpu->stack_pool); /* free cached stacks to avoid leaks */
    task_pool_drain(vcpu);
    if (g_cur_vcpu == vcpu) g_cur_vcpu = NULL;
    free(vcpu);
}

void mco_vcpu_set_stack_pool_limit(mco_vcpu_t* vcpu, int max_free) {
    if (max_free < 0) max_free = 0;
    vcpu->stack_pool.max_free = max_free;
    /* Release the excess immediately when the cap is lowered. */
    while (vcpu->stack_pool.free_count > max_free && vcpu->stack_pool.free_list) {
        void* p = vcpu->stack_pool.free_list;
        vcpu->stack_pool.free_list = *(void**)p;
        free(p);
        vcpu->stack_pool.free_count--;
    }
}

long mco_vcpu_stack_pool_reuse_count(mco_vcpu_t* vcpu) {
    return vcpu->stack_pool.reuse_count;
}

void mco_vcpu_set_stack_zero_mode(mco_vcpu_t* vcpu, int mode) {
    /* NONE (default): minicoro clears what it needs; storage/stack are
     * write-first. FULL: clear the whole block (safe, slower). */
    if (mode != MCO_STACK_ZERO_NONE && mode != MCO_STACK_ZERO_FULL) return;
    vcpu->stack_pool.zero_mode = mode;
}

mco_vcpu_t* mco_vcpu_current(void) {
    if (!g_cur_vcpu) g_cur_vcpu = mco_vcpu_new(NULL);
    return g_cur_vcpu;
}

/* No lazy create: callers use this to ask "am I on the waiter's thread?", and
 * creating an engine on a plain worker thread would be a leak per thread. */
mco_vcpu_t* mco_vcpu_self(void) {
    return g_cur_vcpu;
}

mco_task_t* mco_task_self(void) {
    mco_vcpu_t* vcpu = g_cur_vcpu;
    return vcpu ? vcpu->current : NULL;
}

/* ---- coroutine creation ---- */

static void coro_trampoline(mco_coro* co) {
    mco_task_t* task = (mco_task_t*)mco_get_user_data(co);
    task->entry(task->entry_arg);
}

/* minicoro allocator hooked to the stack pool; size is "struct + stack". */
static void* pool_alloc(size_t size, void* allocator_data) {
    mco_stack_pool_t* pool = (mco_stack_pool_t*)allocator_data;
#if MCO_VCPU_ASAN_BUILD
    (void)pool;
    return calloc(1, size);
#else
    if (pool->block_size == size && pool->free_list) {
        void* p = pool->free_list;
        pool->free_list = *(void**)p;
        pool->free_count--;
        pool->reuse_count++;
        /* No zeroing: minicoro clears what it needs. Key to ~80ns creation. */
        if (pool->zero_mode == MCO_STACK_ZERO_FULL) memset(p, 0, size);
        return p;
    }
    /* malloc, not calloc: calloc would fault in the whole block (~68KB RSS per
     * coroutine); with malloc only written pages become resident (~8KB). */
    return malloc(size);
#endif
}

static void pool_dealloc(void* ptr, size_t size, void* allocator_data) {
    mco_stack_pool_t* pool = (mco_stack_pool_t*)allocator_data;
#if MCO_VCPU_ASAN_BUILD
    (void)pool; (void)size;
    free(ptr);
#else
    if (!ptr) return;
    if (pool->block_size == 0) pool->block_size = size;
    if (pool->block_size == size && pool->free_count < pool->max_free) {
        *(void**)ptr = pool->free_list;
        pool->free_list = ptr;
        pool->free_count++;
        return;
    }
    free(ptr);
#endif
}

mco_task_t* mco_go(mco_vcpu_t* vcpu, void (*fn)(void* arg), void* arg) {
    mco_task_t* task;
    mco_desc desc;
    mco_result res;

    if (vcpu->task_free) {
        task = vcpu->task_free;
        vcpu->task_free = task->ready_next;
        vcpu->ntask_free--;
        memset(task, 0, sizeof(*task)); /* reused struct, as with calloc */
    } else {
        task = (mco_task_t*)calloc(1, sizeof(mco_task_t));
        if (!task) return NULL;
    }

    desc = mco_desc_init(coro_trampoline, MCO_STACK_SIZE); /* 96KB stack: hot-path buffers stay on-stack */
    desc.user_data = task;
    desc.allocator_data = &vcpu->stack_pool;
    desc.alloc_cb = pool_alloc;
    desc.dealloc_cb = pool_dealloc;

    res = mco_create(&task->co, &desc);
    if (res != MCO_SUCCESS) { free(task); return NULL; }

    task->vcpu = vcpu;
    task->entry = fn;
    task->entry_arg = arg;
    task->wait_fd = -1;

    vcpu->ntasks++;
    push_ready(vcpu, task);
    return task;
}

/* ---- scheduler main loop ---- */

void mco_vcpu_run(mco_vcpu_t* vcpu) {
    mco_vcpu_t* saved_cur = g_cur_vcpu;
    g_cur_vcpu = vcpu;
    vcpu->stopping = 0;

    while (!vcpu->stopping) {
        mco_task_t* task;
        unsigned drained = 0;
        int budget_out;

        while (drained < MCO_DRAIN_BUDGET && (task = pop_ready(vcpu)) != NULL) {
            ++drained;
            vcpu->current = task;
            task_resume(task);
            vcpu->current = NULL;

            if (task->co->state == MCO_DEAD) {
                mco_destroy(task->co);
                /* recycle the task struct (stack handled by the pool) */
                if (vcpu->ntask_free < MCO_TASK_POOL_MAX_FREE) {
                    task->ready_next = vcpu->task_free;
                    vcpu->task_free = task;
                    vcpu->ntask_free++;
                } else {
                    free(task);
                }
                vcpu->ntasks--;
            }
            /* parked on the engine/timer heap; a callback will requeue it */

            if (vcpu->stopping) break;
        }

        if (vcpu->stopping) break;
        if (vcpu->ntasks == 0) break; /* no live coroutines */

        /* Budget exhausted with ready tasks left: must poll(0), the timer
         * timeout could be -1 (forever). Skip when nblocked==0: nothing is
         * parked, so poll would be a pure no-op syscall. */
        budget_out = (vcpu->ready_head != NULL);
        if (budget_out && vcpu->nblocked == 0) continue;

        {
            int timeout_ms = budget_out
                           ? 0
                           : mco_timer_heap_next_timeout_ms(&vcpu->timers);
            vcpu->ops->poll(vcpu->engine, timeout_ms);
            mco_timer_heap_process(&vcpu->timers);
        }
    }

    g_cur_vcpu = saved_cur;
}

void mco_vcpu_stop(mco_vcpu_t* vcpu) {
    vcpu->stopping = 1;
    if (vcpu->ops->wake) vcpu->ops->wake(vcpu->engine);
}

/* ---- io wait primitives ---- */

static void on_io_event(void* ud, int fd, int revents) {
    mco_task_t* task = (mco_task_t*)ud;
    /* Only accept an event for a live MCO_WAIT_IO wait on the same fd; stale or
     * mismatched events are ignored (the caller re-reads and re-waits). */
    if (task->wait_kind != MCO_WAIT_IO) return;
    if (task->wait_fd != fd) return;
    if (!(revents & (task->wait_events | MCO_EV_ERROR))) return;
    task->wait_result = revents;
    push_ready(task->vcpu, task);
}

static void on_io_timeout(void* ud) {
    mco_task_t* task = (mco_task_t*)ud;

    if (task->wait_kind == MCO_WAIT_NONE) {
        /* completion op already delivered a result this round */
        return;
    }
    if (task->wait_kind == MCO_WAIT_ASYNC) {
        /* Record the conclusion and go to DRAIN: buf is still referenced by the
         * in-flight op, so resume only after its CQE. */
        const struct mco_engine_async_ops* async = task->vcpu->ops->async;
        task->wait_final = MCO_ERR_TIMEOUT;
        task->wait_kind = MCO_WAIT_DRAIN;
        if (async) (void)async->submit_cancel(task->vcpu->engine, task->async_cookie, task);
        return;
    }
    if (task->wait_kind == MCO_WAIT_IO) {
        /* An io event already landed this round; do not overwrite it. */
        if (task->wait_result == 0)
            task->wait_result = MCO_ERR_TIMEOUT;
        push_ready(task->vcpu, task);
    }
    /* DRAIN: conclusion already recorded */
}

/* Completion cb dispatched inside poll(); res>=0 ok, <0 = -errno. */
static void on_async_complete(void* ud, int fd, int res) {
    mco_task_t* task = (mco_task_t*)ud;
    (void)fd;

    if (task->wait_kind == MCO_WAIT_ASYNC) {
        /* Keep the real errno in wait_errno: MCO_ERR_* is too coarse and under
         * io_uring SO_ERROR cannot give ECONNREFUSED. */
        task->wait_errno = (res < 0) ? -res : 0;
        task->wait_result = (res >= 0) ? res : MCO_ERR_IO;
        task->wait_kind = MCO_WAIT_NONE;
        push_ready(task->vcpu, task);
        return;
    }
    if (task->wait_kind == MCO_WAIT_DRAIN) {
        /* Final CQE after cancel: buf is free, resume with the recorded
         * conclusion and drop this CQE's errno. */
        task->wait_errno = 0;
        task->wait_result = task->wait_final;
        task->wait_kind = MCO_WAIT_NONE;
        push_ready(task->vcpu, task);
        return;
    }
    /* NONE/IO: the op completed inside submit_* (IOCP may post no completion),
     * so the cb ran before wait_kind became ASYNC. Record the result; the caller
     * reads it via MCO_ASYNC_SYNC_DONE without yielding. No push_ready: the task
     * is still running and would be resumed twice. */
    task->wait_errno = (res < 0) ? -res : 0;
    task->wait_result = (res >= 0) ? res : MCO_ERR_IO;
}

int mco_wait_io(int fd, int events, int timeout_ms) {
    mco_vcpu_t* vcpu = g_cur_vcpu;
    mco_task_t* task = vcpu->current;

    task->wait_fd = fd;
    task->wait_events = events;
    task->wait_result = 0;
    task->wait_kind = MCO_WAIT_IO;

    vcpu->ops->ctl(vcpu->engine, fd, events, on_io_event, task);
    if (timeout_ms >= 0) {
        /* Embedded timer node: no malloc */
        mco_timer_start(&vcpu->timers, &task->wait_timer_obj, (uint32_t)timeout_ms, on_io_timeout, task);
    }

    task_yield(vcpu, task);

    /* Unregister with the original direction bits: read and write are separate
     * slots and 0 would clear the whole fd, killing another coroutine's wait.
     * ud is the identity, so a stale unregister after fd reuse is skipped. */
    vcpu->ops->ctl(vcpu->engine, fd, events, NULL, task);
    if (mco_timer_active(&task->wait_timer_obj))
        mco_timer_stop(&vcpu->timers, &task->wait_timer_obj);
    task->wait_fd = -1;
    task->wait_kind = MCO_WAIT_NONE;
    return task->wait_result;
}

/* Completion-style wait: submit -> arm timer -> yield -> take the result. With
 * MCO_ASYNC_SYNC_DONE the op finished inside submit_*, so return directly
 * without yielding. */
static int async_wait_common(mco_vcpu_t* vcpu, mco_task_t* task, uint64_t cookie,
                             int fd, int timeout_ms) {
    if (cookie & MCO_ASYNC_SYNC_DONE) {
        task->wait_fd = -1;
        return task->wait_result;
    }
    task->wait_kind = MCO_WAIT_ASYNC;
    task->async_cookie = cookie;
    task->wait_fd = fd;
    task->wait_result = 0;
    task->wait_errno = 0;
    if (timeout_ms >= 0)
        mco_timer_start(&vcpu->timers, &task->wait_timer_obj, (uint32_t)timeout_ms, on_io_timeout, task);

    task_yield(vcpu, task);

    /* On resume wait_kind is already NONE; never return with DRAIN set. */
    if (mco_timer_active(&task->wait_timer_obj))
        mco_timer_stop(&vcpu->timers, &task->wait_timer_obj);
    task->wait_fd = -1;
    task->wait_kind = MCO_WAIT_NONE;
    return task->wait_result;
}

int mco_wait_recv(int fd, void* buf, int len, int type, int timeout_ms) {
    mco_vcpu_t* vcpu = g_cur_vcpu;
    mco_task_t* task = vcpu->current;
    const struct mco_engine_async_ops* async = vcpu->ops->async;

    if (timeout_ms == 0) return MCO_ERR_TIMEOUT;  /* caller already tried a read */

    /* Raw fds skip io_uring: submit_read is socket-oriented, files have no readiness. */
    if (async && type != MCO_SOCK_FILE) {
        uint64_t cookie = async->submit_read(vcpu->engine, fd, buf, len, on_async_complete, task);
        if (cookie) return async_wait_common(vcpu, task, cookie, fd, timeout_ms);
        /* SQ full: fall back to readiness */
    }

    for (;;) {
        int r = mco_wait_io(fd, MCO_EV_READ, timeout_ms);
        if (r == MCO_ERR_TIMEOUT) return MCO_ERR_TIMEOUT;
        if (r == MCO_ERR_CLOSED) return MCO_ERR_CLOSED;
        for (;;) {
            ssize_t n = (type == MCO_SOCK_FILE)
                      ? MCO_RAW_READ(fd, buf, len)
                      : recv(fd, buf, (size_t)len, 0);
            int e;
            if (n >= 0) return (int)n; /* re-read after READ/ERROR, 0 = EOF */
            e = mco_io_errno(type);
            if (e == EINTR) continue;
            if (e != EAGAIN && e != EWOULDBLOCK) return MCO_ERR_IO;
            break; /* spurious wakeup: keep waiting */
        }
    }
}

int mco_wait_send(int fd, const void* buf, int len, int type, int timeout_ms) {
    mco_vcpu_t* vcpu = g_cur_vcpu;
    mco_task_t* task = vcpu->current;
    const struct mco_engine_async_ops* async = vcpu->ops->async;

    if (timeout_ms == 0) return MCO_ERR_TIMEOUT;

    /* As in mco_wait_recv: raw fds skip io_uring. */
    if (async && type != MCO_SOCK_FILE) {
        uint64_t cookie = async->submit_write(vcpu->engine, fd, buf, len, on_async_complete, task);
        if (cookie) return async_wait_common(vcpu, task, cookie, fd, timeout_ms);
    }

    for (;;) {
        int r = mco_wait_io(fd, MCO_EV_WRITE, timeout_ms);
        if (r == MCO_ERR_TIMEOUT) return MCO_ERR_TIMEOUT;
        if (r == MCO_ERR_CLOSED) return MCO_ERR_CLOSED;
        for (;;) {
            ssize_t n;
            int e;
            if (type == MCO_SOCK_FILE) {
                n = MCO_RAW_WRITE(fd, buf, len);
            } else {
                int flags = 0;
#ifdef MSG_NOSIGNAL
                flags = MSG_NOSIGNAL; /* absent on macOS/BSD, macro-guarded */
#endif
                n = send(fd, buf, (size_t)len, flags);
            }
            if (n >= 0) return (int)n;
            e = mco_io_errno(type);
            if (e == EINTR) continue;
            if (e != EAGAIN && e != EWOULDBLOCK) return MCO_ERR_IO;
            break; /* spurious wakeup: keep waiting */
        }
    }
}

int mco_wait_connect(int fd, const void* addr, unsigned addrlen, int timeout_ms,
                     int* out_errno) {
    mco_vcpu_t* vcpu = g_cur_vcpu;
    mco_task_t* task = vcpu->current;
    const struct mco_engine_async_ops* async = vcpu->ops->async;

    if (out_errno) *out_errno = 0;
    if (timeout_ms == 0) return MCO_ERR_TIMEOUT;

    if (async) {
        uint64_t cookie = async->submit_connect(vcpu->engine, fd, addr, addrlen,
                                                on_async_complete, task);
        if (cookie) {
            int r = async_wait_common(vcpu, task, cookie, fd, timeout_ms);
            /* Trust the CQE res: io_uring CONNECT does not set sk_err, so
             * SO_ERROR would report a refused connection as success. */
            if (r >= 0 || task->wait_errno != 0) {
                if (out_errno) *out_errno = task->wait_errno;
                return 0;
            }
            return r; /* wait timed out or was cancelled */
        }
    }

    {
        int r = mco_wait_io(fd, MCO_EV_WRITE, timeout_ms);
        if (r == MCO_ERR_TIMEOUT) return MCO_ERR_TIMEOUT;
        if (r == MCO_ERR_CLOSED) return MCO_ERR_CLOSED;
        return 0; /* writable/error: caller uses SO_ERROR */
    }
}

#if MINICONET_WITH_IOCP
#include "mco_engine_backends.h"   /* mco_engine_iocp_ops(): backend identity */
/* Whether this vcpu runs on IOCP (which needs AcceptEx; BSD accept() on Windows
 * yields a non-overlapped socket). Decided by backend identity: one pointer
 * compare, no ABI slot. Absent when IOCP is not built (macro gives 0). */
int mco_accept_uses_async(void) {
    mco_vcpu_t* vcpu = g_cur_vcpu;
    if (!vcpu || !vcpu->ops) return 0;
    return vcpu->ops == mco_engine_iocp_ops();
}
#endif /* MINICONET_WITH_IOCP */

int mco_wait_accept(int fd, int timeout_ms) {
    mco_vcpu_t* vcpu = g_cur_vcpu;
    mco_task_t* task = vcpu->current;
    const struct mco_engine_async_ops* async = vcpu->ops->async;

    if (timeout_ms == 0) return MCO_ERR_TIMEOUT;

    if (async) {
        uint64_t cookie = async->submit_accept(vcpu->engine, fd, on_async_complete, task);
        if (cookie) return async_wait_common(vcpu, task, cookie, fd, timeout_ms);
    }

    for (;;) {
        int connfd = (int)accept(fd, NULL, NULL);
        int e;
        if (connfd >= 0) return connfd;
        e = socket_errno();
        if (e == EINTR) continue;
        if (e != EAGAIN && e != EWOULDBLOCK) return MCO_ERR_IO;
        {
            int r = mco_wait_io(fd, MCO_EV_READ, timeout_ms);
            if (r == MCO_ERR_TIMEOUT) return MCO_ERR_TIMEOUT;
            if (r == MCO_ERR_CLOSED) return MCO_ERR_CLOSED;
        }
    }
}

void mco_task_wake_closed(mco_task_t* task) {
    if (!task || task->wait_fd < 0) return; /* not waiting */

    if (task->wait_kind == MCO_WAIT_ASYNC) {
        /* Closed during a completion wait: same DRAIN machine as timeout —
         * record, cancel, resume on the CQE (the kernel holds the file ref). */
        const struct mco_engine_async_ops* async = task->vcpu->ops->async;
        task->wait_final = MCO_ERR_CLOSED;
        task->wait_kind = MCO_WAIT_DRAIN;
        if (async) (void)async->submit_cancel(task->vcpu->engine, task->async_cookie, task);
        return;
    }
    if (task->wait_kind == MCO_WAIT_DRAIN) return; /* already decided */

    task->wait_result = MCO_ERR_CLOSED;
    push_ready(task->vcpu, task);
}

/* Requeue without setting a result: the caller has already published whatever
 * the waiter needs (the mutex it did that under is the happens-before edge). */
void mco_task_wake(mco_task_t* task) {
    if (!task) return;
    push_ready(task->vcpu, task);
}

static void on_sleep_timer(void* ud) {
    mco_task_t* task = (mco_task_t*)ud;
    push_ready(task->vcpu, task);
}

void mco_sleep_ms(uint32_t ms) {
    mco_vcpu_t* vcpu = g_cur_vcpu;
    mco_task_t* task = vcpu->current;

    if (ms == 0) {
        /* Plain yield: skip the timer heap, just requeue. The drain budget keeps
         * the loop from starving on such spins. */
        push_ready(vcpu, task);
        task_yield(vcpu, task);
        return;
    }

    mco_timer_start(&vcpu->timers, &task->wait_timer_obj, ms, on_sleep_timer, task);
    task_yield(vcpu, task);
}
