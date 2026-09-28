/*
 * mco_workpool.c — multi-vCPU work pool (see mco_workpool.h).
 * async(): round-robin a worker, enqueue on its own queue, signal its wake
 * channel. A dispatcher coroutine per worker waits on the channel, drains it
 * (happens-before with async's enqueue), then mco_go()s the tasks.
 * Backpressure: inflight under qlock; a full pool parks the submitter as a
 * waiter, and releasing a slot hands it to the head waiter. free() fires all
 * waiters with MCO_WP_WAIT_CLOSED (async() returns MCO_ERR_CLOSED).
 */
#include "mco_workpool.h"
#include "mco_vcpu.h"
#include "mco_engine.h"
#include "mco_oneshot.h"
#include "mco_wakefd.h"
#include "../compat/hthread.h"
#include "../compat/hmutex.h"
#include <stdlib.h>
#include <string.h>

/* ---- task queue node ---- */
typedef struct mco_wp_task {
    void (*fn)(void* arg);
    void* arg;
    mco_workpool_t* pool;   /* used to release the inflight slot after fn */
    struct mco_wp_task* next;
} mco_wp_task_t;

/* ---- waiter node for a full pool (stack-allocated) ---- */
#define MCO_WP_WAIT_PENDING 0
#define MCO_WP_WAIT_GRANTED 1  /* a slot was handed over; counts as occupied */
#define MCO_WP_WAIT_CLOSED  2  /* pool freed: submit failed */

typedef struct mco_wp_waiter {
    mco_oneshot_t* one;
    int status;
    struct mco_wp_waiter* next;
} mco_wp_waiter_t;

/* Per worker: own vcpu + wake channel read end + own queue. A shared queue would
 * be drained by whichever worker wakes first, defeating round-robin. */
typedef struct {
    mco_workpool_t* pool;
    mco_vcpu_t* vcpu;
    mco_wakefd_t* wake;   /* dispatcher waits on wake->rfd; NULL if not created */
    mco_wp_task_t* qhead;  /* this worker's queue, under qlock */
    mco_wp_task_t* qtail;
} mco_wp_worker_t;

struct mco_workpool {
    int             n;
    hthread_t*      threads;
    mco_wp_worker_t* workers;   /* n workers: vcpu + wake channel + own queue */
    mco_wakefd_t*   wakes;      /* n channels, used by async to signal */

    hmutex_t        qlock;       /* protects qhead/qtail, rr, inflight, waiters */
    int             rr;          /* round-robin cursor, under qlock */

    int             max_inflight;  /* <=0 = unlimited, inflight/waiters unused */
    int             inflight;      /* under qlock */
    mco_wp_waiter_t* waiters;      /* submitters waiting for a slot, under qlock */
    mco_wp_waiter_t* waiters_tail;

    int             shutdown;     /* under qlock */
};

/* Release a slot: hand it to the head waiter (count unchanged) if any, else
 * inflight--. Skipped when max_inflight<=0. */
static void mco_wp_release_slot(mco_workpool_t* pool) {
    mco_wp_waiter_t* w = NULL;
    if (pool->max_inflight <= 0) return;

    hmutex_lock(&pool->qlock);
    if (pool->waiters) {
        w = pool->waiters;
        pool->waiters = w->next;
        if (!pool->waiters) pool->waiters_tail = NULL;
        w->next = NULL;
    } else if (pool->inflight > 0) {
        pool->inflight--;
    }
    hmutex_unlock(&pool->qlock);

    if (w) {
        w->status = MCO_WP_WAIT_GRANTED;
        mco_oneshot_fire(w->one);
    }
}

/* Coroutine entry: run fn, then release the inflight slot. */
static void task_trampoline(void* arg) {
    mco_wp_task_t* node = (mco_wp_task_t*)arg;
    void (*fn)(void* arg) = node->fn;
    void* fn_arg = node->arg;
    mco_workpool_t* pool = node->pool;

    free(node);
    fn(fn_arg);
    mco_wp_release_slot(pool);
}

/* ---- dispatcher coroutine: one per worker vcpu ---- */
static void dispatcher_coro(void* arg) {
    mco_wp_worker_t* w = (mco_wp_worker_t*)arg;
    mco_workpool_t* pool = w->pool;

    for (;;) {
        int r = mco_wait_io(w->wake->rfd, MCO_EV_READ, -1);
        if (r == MCO_ERR_CLOSED) {
            /* cannot happen (this fd has no mco_socket_t) */
            break;
        }
        if (r < 0) continue;  /* no timeout (infinite wait); retry on error */

        /* Really drain the channel: happens-before with async's enqueue. With
         * eventfd one read resets the whole counter, so a burst costs one
         * syscall no matter how many submits it carries. */
        mco_wakefd_drain(w->wake);

        /* Take the queue and read shutdown under qlock: an unlocked read of a
         * plain int is still a data race. mco_go runs outside the lock. */
        {
            mco_wp_task_t* local;
            int shutting_down;
            hmutex_lock(&pool->qlock);
            local = w->qhead;
            w->qhead = NULL;
            w->qtail = NULL;
            shutting_down = pool->shutdown;
            hmutex_unlock(&pool->qlock);

            if (shutting_down) {
                /* shutting down: drop the rest and stop the vcpu */
                mco_wp_task_t* p = local;
                while (p) { mco_wp_task_t* nx = p->next; free(p); p = nx; }
                mco_vcpu_stop(w->vcpu);
                return;
            }

            while (local) {
                mco_wp_task_t* nx = local->next;
                /* safe: the dispatcher runs on the worker's own thread */
                mco_go(w->vcpu, task_trampoline, local);
                local = nx;
            }
        }
    }
}

/* ---- worker thread entry ---- */
static HTHREAD_ROUTINE(mco_wp_worker_main) {
    mco_wp_worker_t* w = (mco_wp_worker_t*)userdata;
    mco_vcpu_t* vcpu;

    vcpu = mco_vcpu_new(mco_engine_default());
    if (!vcpu) {
        /* malloc failed: this worker is lost, free() still joins it */
        return 0;
    }
    w->vcpu = vcpu;

    /* the dispatcher must be mco_go'ed before mco_vcpu_run */
    mco_go(vcpu, dispatcher_coro, w);

    mco_vcpu_run(vcpu);       /* blocks until the dispatcher stops the vcpu */
    mco_vcpu_free(vcpu);
    return 0;
}

/* ---- public API ---- */

mco_workpool_t* mco_workpool_new(int n_vcpus, int max_inflight) {
    mco_workpool_t* pool;
    int i;

    if (n_vcpus <= 0) n_vcpus = 1;

    pool = (mco_workpool_t*)calloc(1, sizeof(mco_workpool_t));
    if (!pool) return NULL;
    pool->n = n_vcpus;
    pool->rr = 0;
    pool->shutdown = 0;
    pool->max_inflight = max_inflight;
    pool->inflight = 0;
    pool->waiters = NULL;
    pool->waiters_tail = NULL;
    hmutex_init(&pool->qlock);

    pool->workers = (mco_wp_worker_t*)calloc((size_t)n_vcpus, sizeof(mco_wp_worker_t));
    pool->threads = (hthread_t*)calloc((size_t)n_vcpus, sizeof(hthread_t));
    pool->wakes = (mco_wakefd_t*)calloc((size_t)n_vcpus, sizeof(mco_wakefd_t));
    if (!pool->workers || !pool->threads || !pool->wakes) {
        free(pool->workers); free(pool->threads); free(pool->wakes);
        hmutex_destroy(&pool->qlock);
        free(pool);
        return NULL;
    }
    /* rfd < 0 is the "not created" marker, so set it before any init runs. */
    for (i = 0; i < n_vcpus; ++i) { pool->wakes[i].rfd = -1; pool->wakes[i].wfd = -1; }

    for (i = 0; i < n_vcpus; ++i) {
        if (mco_wakefd_init(&pool->wakes[i]) != 0) {
            /* failed: no channel, the thread never starts */
            continue;
        }
        pool->workers[i].pool = pool;
        pool->workers[i].wake = &pool->wakes[i];

        {
            uv_thread_options_t opt;
            opt.flags = UV_THREAD_NO_FLAGS;
            opt.stack_size = 0;
            pool->threads[i] = hthread_create(&opt, mco_wp_worker_main, &pool->workers[i]);
        }
    }

    return pool;
}

int mco_workpool_async(mco_workpool_t* pool, void (*fn)(void* arg), void* arg) {
    mco_wp_task_t* node;
    int target;

    if (!pool || !fn) return MCO_ERR_CLOSED;

    hmutex_lock(&pool->qlock);
    if (pool->shutdown) {
        hmutex_unlock(&pool->qlock);
        return MCO_ERR_CLOSED;
    }

    if (pool->max_inflight > 0 && pool->inflight >= pool->max_inflight) {
        /* Full: park as a waiter (stack-allocated, kept alive by the stack). */
        mco_wp_waiter_t waiter;
        waiter.one = mco_oneshot_new();
        waiter.status = MCO_WP_WAIT_PENDING;
        waiter.next = NULL;
        if (!waiter.one) {
            /* alloc failure: report it instead of retrying */
            hmutex_unlock(&pool->qlock);
            return MCO_ERR_CLOSED;
        }
        if (pool->waiters_tail) pool->waiters_tail->next = &waiter;
        else pool->waiters = &waiter;
        pool->waiters_tail = &waiter;
        hmutex_unlock(&pool->qlock);

        mco_oneshot_wait(waiter.one, -1);   /* coroutine-only, see the header */
        mco_oneshot_release(waiter.one);

        if (waiter.status == MCO_WP_WAIT_CLOSED) return MCO_ERR_CLOSED;
        /* GRANTED: the slot was handed over, inflight already counted */
    } else {
        if (pool->max_inflight > 0) pool->inflight++;  /* <=0: not tracked */
        hmutex_unlock(&pool->qlock);
    }

    node = (mco_wp_task_t*)malloc(sizeof(mco_wp_task_t));
    if (!node) {
        /* failure: give the slot back */
        mco_wp_release_slot(pool);
        return MCO_ERR_CLOSED;
    }
    node->fn = fn;
    node->arg = arg;
    node->pool = pool;
    node->next = NULL;

    hmutex_lock(&pool->qlock);
    if (pool->shutdown) {
        /* shutdown before enqueue: return the slot, drop the task */
        hmutex_unlock(&pool->qlock);
        mco_wp_release_slot(pool);
        free(node);
        return MCO_ERR_CLOSED;
    }

    /* round-robin picks the worker queue, so assignment matches the executor */
    target = pool->rr;
    pool->rr = (pool->rr + 1) % pool->n;
    {
        mco_wp_worker_t* w = &pool->workers[target];
        if (w->qtail) w->qtail->next = node; else w->qhead = node;
        w->qtail = node;
    }
    hmutex_unlock(&pool->qlock);

    /* Wake the worker: one signal on its channel, non-blocking. */
    mco_wakefd_signal(&pool->wakes[target]);

    return 0;
}

void mco_workpool_free(mco_workpool_t* pool) {
    int i;
    mco_wp_waiter_t* waiters;
    if (!pool) return;

    hmutex_lock(&pool->qlock);
    pool->shutdown = 1;
    waiters = pool->waiters;
    pool->waiters = NULL;
    pool->waiters_tail = NULL;
    hmutex_unlock(&pool->qlock);

    /* wake waiters so async() returns MCO_ERR_CLOSED instead of hanging */
    while (waiters) {
        mco_wp_waiter_t* nx = waiters->next;
        waiters->status = MCO_WP_WAIT_CLOSED;
        mco_oneshot_fire(waiters->one);
        waiters = nx;
    }

    /* wake every dispatcher so it sees shutdown and stops its vcpu */
    for (i = 0; i < pool->n; ++i) {
        if (pool->wakes[i].rfd >= 0) mco_wakefd_signal(&pool->wakes[i]);
    }

    /* join all workers; in-flight task coroutines are abandoned (documented). */
    for (i = 0; i < pool->n; ++i) {
        if (pool->workers[i].wake) {
            hthread_join(pool->threads[i]);
        }
    }

    /* Clean up the channels, any tasks left in the queues, and the struct. */
    for (i = 0; i < pool->n; ++i) {
        if (pool->wakes[i].rfd >= 0) mco_wakefd_fini(&pool->wakes[i]);

        {
            mco_wp_task_t* p = pool->workers[i].qhead;
            while (p) { mco_wp_task_t* nx = p->next; free(p); p = nx; }
        }
    }
    hmutex_destroy(&pool->qlock);
    free(pool->workers);
    free(pool->threads);
    free(pool->wakes);
    free(pool);
}
