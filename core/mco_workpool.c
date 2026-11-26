/*
 * mco_workpool.c — multi-vCPU work pool (see mco_workpool.h).
 * async(): round-robin a worker, enqueue on its own queue, signal its wake
 * channel. A dispatcher coroutine per worker waits on the channel, drains it
 * (happens-before with async's enqueue), then mco_go()s the tasks.
 *
 * Two counters, on purpose:
 *   inflight — limiter only, active when max_inflight > 0
 *   active   — every accepted-but-unfinished task, always maintained; it is the
 *              drain() predicate and the "is it safe to stop this vcpu" test.
 *
 * Backpressure: inflight under qlock; a full pool parks the submitter as a
 * waiter, and releasing a slot hands it to the head waiter. try_async() never
 * parks: it reports MCO_ERR_BUSY instead, so an acceptor never stalls.
 *
 * Shutdown: stop() flips shutdown and wakes every dispatcher. A dispatcher keeps
 * executing what is already queued, and only stops its vcpu when the queue is
 * empty AND active == 0 (the last finishing task wakes all dispatchers). That is
 * what makes drain() able to wait for real completion instead of abandoning
 * running coroutines. free_now() sets free_now, which restores the old
 * "drop the queue and stop the vcpu" behaviour.
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
    hcondvar_t      done_cond;   /* signalled when active hits 0 (drain) */
    hcondvar_t      init_cond;   /* worker startup handshake (see new()) */
    int             n_init;      /* workers that finished init, ok or failed */
    int             n_started;   /* threads successfully created by new() */
    int             rr;          /* round-robin cursor, under qlock */

    int             max_inflight;  /* <=0 = unlimited, inflight/waiters unused */
    int             inflight;      /* under qlock */
    int             active;        /* unfinished tasks (queued + running) */
    mco_wp_waiter_t* waiters;      /* submitters waiting for a slot, under qlock */
    mco_wp_waiter_t* waiters_tail;

    int             shutdown;     /* under qlock: refuse new tasks */
    int             free_now;     /* under qlock: drop queues instead of draining */

    int             n_alive;      /* workers whose thread started */
    int*            alive;        /* indices of live workers, round-robin over it */
};

/* Wake every dispatcher so it re-checks shutdown/active. */
static void mco_wp_wake_all(mco_workpool_t* pool) {
    int i;
    for (i = 0; i < pool->n; ++i) {
        if (pool->wakes[i].rfd >= 0) mco_wakefd_signal(&pool->wakes[i]);
    }
}

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

/* Mark one task finished: active--, broadcast drain waiters, and when the pool is
 * shutting down wake every dispatcher so the idle ones can stop their vcpu. */
static void mco_wp_task_done(mco_workpool_t* pool) {
    int wake = 0;

    hmutex_lock(&pool->qlock);
    if (pool->active > 0) pool->active--;
    if (pool->active == 0) {
        hcondvar_broadcast(&pool->done_cond);
        if (pool->shutdown) wake = 1;
    }
    hmutex_unlock(&pool->qlock);

    if (wake) mco_wp_wake_all(pool);
    mco_wp_release_slot(pool);
}

/* Coroutine entry: run fn, then release the inflight slot. */
static void task_trampoline(void* arg) {
    mco_wp_task_t* node = (mco_wp_task_t*)arg;
    void (*fn)(void* arg) = node->fn;
    void* fn_arg = node->arg;
    mco_workpool_t* pool = node->pool;

    free(node);
    fn(fn_arg);
    mco_wp_task_done(pool);
}

static void mco_wp_free_tasks(mco_wp_task_t* p) {
    while (p) { mco_wp_task_t* nx = p->next; free(p); p = nx; }
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

        /* Take the queue and read the state under qlock: an unlocked read of a
         * plain int is still a data race. mco_go runs outside the lock. */
        {
            mco_wp_task_t* local;
            int shutting_down;
            int idle;
            int drop;

            hmutex_lock(&pool->qlock);
            local = w->qhead;
            w->qhead = NULL;
            w->qtail = NULL;
            shutting_down = pool->shutdown;
            idle = (pool->active == 0);
            drop = pool->free_now;
            hmutex_unlock(&pool->qlock);

            if (drop) {
                /* free_now: abandon the rest and stop the vcpu */
                mco_wp_free_tasks(local);
                mco_vcpu_stop(w->vcpu);
                return;
            }

            /* shutdown still runs what was already accepted (drain semantics) */
            while (local) {
                mco_wp_task_t* nx = local->next;
                /* Fallback: if the coroutine cannot be created the task will
                 * never run, so settle it right here — free the node and account
                 * for it, or active would never drop and drain() would stall.
                 * safe: the dispatcher runs on the worker's own thread */
                if (!mco_go(w->vcpu, task_trampoline, local)) {
                    free(local);
                    mco_wp_task_done(pool);
                }
                local = nx;
            }

            /* leave only when nothing is queued and nothing is running */
            if (shutting_down && idle) {
                mco_vcpu_stop(w->vcpu);
                return;
            }
        }
    }
}

/* ---- worker thread entry ---- */
static HTHREAD_ROUTINE(mco_wp_worker_main) {
    mco_wp_worker_t* w = (mco_wp_worker_t*)userdata;
    mco_workpool_t* pool = w->pool;
    mco_vcpu_t* vcpu;
    int ok = 0;

    vcpu = mco_vcpu_new(NULL);   /* NULL = default backend, and gains its readiness fallback */
    if (vcpu) {
        w->vcpu = vcpu;
        /* the dispatcher must be spawned before mco_vcpu_run */
        if (mco_go(vcpu, dispatcher_coro, w)) {
            ok = 1;
        } else {
            /* Fallback: no dispatcher means this worker can never run a task.
             * Leave wake NULL so new() drops it from alive[]; otherwise a task
             * routed here would never execute, active would never reach 0 and
             * drain() could only time out. */
            mco_vcpu_free(vcpu);
            w->vcpu = NULL;
        }
    }

    /* Startup handshake: new() waits for every started thread to reach this
     * point, so a failed worker is excluded BEFORE the pool is returned. */
    hmutex_lock(&pool->qlock);
    if (!ok) w->wake = NULL;
    pool->n_init++;
    hcondvar_broadcast(&pool->init_cond);
    hmutex_unlock(&pool->qlock);

    if (ok) {
        mco_vcpu_run(vcpu);   /* blocks until the dispatcher stops the vcpu */
        mco_vcpu_free(vcpu);
    }
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
    pool->free_now = 0;
    pool->max_inflight = max_inflight;
    pool->inflight = 0;
    pool->active = 0;
    pool->n_alive = 0;
    pool->waiters = NULL;
    pool->waiters_tail = NULL;
    hmutex_init(&pool->qlock);
    hcondvar_init(&pool->done_cond);
    hcondvar_init(&pool->init_cond);

    pool->workers = (mco_wp_worker_t*)calloc((size_t)n_vcpus, sizeof(mco_wp_worker_t));
    pool->threads = (hthread_t*)calloc((size_t)n_vcpus, sizeof(hthread_t));
    pool->wakes = (mco_wakefd_t*)calloc((size_t)n_vcpus, sizeof(mco_wakefd_t));
    pool->alive = (int*)calloc((size_t)n_vcpus, sizeof(int));
    if (!pool->workers || !pool->threads || !pool->wakes || !pool->alive) {
        free(pool->workers); free(pool->threads); free(pool->wakes); free(pool->alive);
        hcondvar_destroy(&pool->init_cond);
        hcondvar_destroy(&pool->done_cond);
        hmutex_destroy(&pool->qlock);
        free(pool);
        return NULL;
    }
    /* rfd < 0 is the "not created" marker, so set it before any init runs. */
    for (i = 0; i < n_vcpus; ++i) { pool->wakes[i].rfd = -1; pool->wakes[i].wfd = -1; }

    for (i = 0; i < n_vcpus; ++i) {
        uv_thread_options_t opt;
        if (mco_wakefd_init(&pool->wakes[i]) != 0) {
            /* failed: no channel, the thread never starts */
            continue;
        }
        pool->workers[i].pool = pool;
        pool->workers[i].wake = &pool->wakes[i];

        opt.flags = UV_THREAD_NO_FLAGS;
        opt.stack_size = 0;
        pool->threads[i] = hthread_create(&opt, mco_wp_worker_main, &pool->workers[i]);
        if (!pool->threads[i]) {
            /* thread did not start: drop this worker instead of queueing to it */
            mco_wakefd_fini(&pool->wakes[i]);
            pool->workers[i].wake = NULL;
            continue;
        }
        pool->n_started++;
    }

    /* Wait for every started thread to finish its init (vcpu + dispatcher). A
     * worker that failed leaves wake NULL, and only then do we build alive[] —
     * so no task is ever routed to a worker that cannot run it. */
    hmutex_lock(&pool->qlock);
    while (pool->n_init < pool->n_started) {
        hcondvar_wait(&pool->init_cond, &pool->qlock);
    }
    for (i = 0; i < n_vcpus; ++i) {
        if (pool->workers[i].wake) pool->alive[pool->n_alive++] = i;
    }
    hmutex_unlock(&pool->qlock);

    if (pool->n_alive == 0) {
        /* every worker failed: join the (already exiting) threads before free */
        for (i = 0; i < n_vcpus; ++i) {
            if (pool->threads[i]) hthread_join(pool->threads[i]);
            if (pool->wakes[i].rfd >= 0) mco_wakefd_fini(&pool->wakes[i]);
        }
        free(pool->workers); free(pool->threads); free(pool->wakes); free(pool->alive);
        hcondvar_destroy(&pool->init_cond);
        hcondvar_destroy(&pool->done_cond);
        hmutex_destroy(&pool->qlock);
        free(pool);
        return NULL;
    }

    return pool;
}

/* Common tail of async/try_async: caller already holds a slot (inflight++ when
 * tracked) and active++ was done under qlock. Enqueues on the target worker. */
static int mco_wp_enqueue(mco_workpool_t* pool, mco_wp_task_t* node) {
    int target;
    mco_wp_worker_t* w;

    hmutex_lock(&pool->qlock);
    if (pool->shutdown || pool->n_alive <= 0) {
        hmutex_unlock(&pool->qlock);
        return MCO_ERR_CLOSED;
    }
    target = pool->alive[pool->rr];
    pool->rr = (pool->rr + 1) % pool->n_alive;
    w = &pool->workers[target];
    if (w->qtail) w->qtail->next = node; else w->qhead = node;
    w->qtail = node;
    hmutex_unlock(&pool->qlock);

    /* Wake the worker: one signal on its channel, non-blocking. */
    mco_wakefd_signal(&pool->wakes[target]);
    return 0;
}

int mco_workpool_try_async(mco_workpool_t* pool, void (*fn)(void* arg), void* arg) {
    mco_wp_task_t* node;
    int rc;

    if (!pool || !fn) return MCO_ERR_CLOSED;

    node = (mco_wp_task_t*)malloc(sizeof(mco_wp_task_t));
    if (!node) return MCO_ERR_IO;

    hmutex_lock(&pool->qlock);
    if (pool->shutdown || pool->n_alive <= 0) {
        hmutex_unlock(&pool->qlock);
        free(node);
        return MCO_ERR_CLOSED;
    }
    if (pool->max_inflight > 0 && pool->inflight >= pool->max_inflight) {
        hmutex_unlock(&pool->qlock);
        free(node);
        return MCO_ERR_BUSY;   /* never parks, never queues */
    }
    if (pool->max_inflight > 0) pool->inflight++;
    pool->active++;
    hmutex_unlock(&pool->qlock);

    node->fn = fn;
    node->arg = arg;
    node->pool = pool;
    node->next = NULL;

    rc = mco_wp_enqueue(pool, node);
    if (rc != 0) {
        hmutex_lock(&pool->qlock);
        if (pool->active > 0) pool->active--;
        if (pool->max_inflight > 0 && pool->inflight > 0) pool->inflight--;
        hmutex_unlock(&pool->qlock);
        free(node);
    }
    return rc;
}

int mco_workpool_async(mco_workpool_t* pool, void (*fn)(void* arg), void* arg) {
    mco_wp_task_t* node;
    int rc;

    if (!pool || !fn) return MCO_ERR_CLOSED;

    hmutex_lock(&pool->qlock);
    if (pool->shutdown || pool->n_alive <= 0) {
        hmutex_unlock(&pool->qlock);
        return MCO_ERR_CLOSED;
    }

    if (pool->max_inflight > 0 && pool->inflight >= pool->max_inflight) {
        /* Full: park as a waiter (stack-allocated, kept alive by the stack).
         * Parking is coroutine-only; a plain thread must not be blocked here. */
        mco_wp_waiter_t waiter;

        if (!mco_vcpu_self()) {
            hmutex_unlock(&pool->qlock);
            return MCO_ERR_BUSY;
        }

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
        hmutex_lock(&pool->qlock);
        pool->active++;
        hmutex_unlock(&pool->qlock);
    } else {
        if (pool->max_inflight > 0) pool->inflight++;  /* <=0: not tracked */
        pool->active++;
        hmutex_unlock(&pool->qlock);
    }

    node = (mco_wp_task_t*)malloc(sizeof(mco_wp_task_t));
    if (!node) {
        /* failure: give the slot back */
        hmutex_lock(&pool->qlock);
        if (pool->active > 0) pool->active--;
        hmutex_unlock(&pool->qlock);
        mco_wp_release_slot(pool);
        return MCO_ERR_CLOSED;
    }
    node->fn = fn;
    node->arg = arg;
    node->pool = pool;
    node->next = NULL;

    rc = mco_wp_enqueue(pool, node);
    if (rc != 0) {
        hmutex_lock(&pool->qlock);
        if (pool->active > 0) pool->active--;
        hmutex_unlock(&pool->qlock);
        mco_wp_release_slot(pool);
        free(node);
    }
    return rc;
}

/* Fire every parked submitter with MCO_WP_WAIT_CLOSED. */
static void mco_wp_release_waiters(mco_workpool_t* pool) {
    mco_wp_waiter_t* waiters;

    hmutex_lock(&pool->qlock);
    waiters = pool->waiters;
    pool->waiters = NULL;
    pool->waiters_tail = NULL;
    hmutex_unlock(&pool->qlock);

    while (waiters) {
        mco_wp_waiter_t* nx = waiters->next;
        waiters->status = MCO_WP_WAIT_CLOSED;
        mco_oneshot_fire(waiters->one);
        waiters = nx;
    }
}

void mco_workpool_stop(mco_workpool_t* pool) {
    if (!pool) return;

    hmutex_lock(&pool->qlock);
    pool->shutdown = 1;
    hmutex_unlock(&pool->qlock);

    /* wake waiters so async() returns MCO_ERR_CLOSED instead of hanging */
    mco_wp_release_waiters(pool);

    /* wake every dispatcher so it sees shutdown and can finish + stop */
    mco_wp_wake_all(pool);
}

int mco_workpool_drain(mco_workpool_t* pool, int timeout_ms) {
    int rc = 0;

    if (!pool) return 0;

    hmutex_lock(&pool->qlock);
    if (timeout_ms < 0) {
        while (pool->active > 0) {
            hcondvar_wait(&pool->done_cond, &pool->qlock);
        }
    } else {
        while (pool->active > 0) {
            if (!hcondvar_wait_for(&pool->done_cond, &pool->qlock,
                                   (unsigned int)timeout_ms)) {
                rc = MCO_ERR_TIMEOUT;
                break;
            }
        }
    }
    hmutex_unlock(&pool->qlock);
    return rc;
}

int mco_workpool_active(mco_workpool_t* pool) {
    int v;
    if (!pool) return 0;
    hmutex_lock(&pool->qlock);
    v = pool->active;
    hmutex_unlock(&pool->qlock);
    return v;
}

int mco_workpool_worker_count(mco_workpool_t* pool) {
    int v;
    if (!pool) return 0;
    hmutex_lock(&pool->qlock);
    v = pool->n_alive;
    hmutex_unlock(&pool->qlock);
    return v;
}

/* Join every started worker and release all resources. Callers must have
 * stopped the pool (or set free_now) so the dispatchers exit on their own. */
static void mco_wp_join_and_free(mco_workpool_t* pool) {
    int i;

    for (i = 0; i < pool->n; ++i) {
        /* join every thread that started: a worker whose init failed also
         * returns, and leaving it unjoined leaks its thread resources. */
        if (pool->threads[i]) {
            hthread_join(pool->threads[i]);
        }
    }

    for (i = 0; i < pool->n; ++i) {
        if (pool->wakes[i].rfd >= 0) mco_wakefd_fini(&pool->wakes[i]);
        mco_wp_free_tasks(pool->workers[i].qhead);
    }

    hcondvar_destroy(&pool->init_cond);
    hcondvar_destroy(&pool->done_cond);
    hmutex_destroy(&pool->qlock);
    free(pool->alive);
    free(pool->workers);
    free(pool->threads);
    free(pool->wakes);
    free(pool);
}

void mco_workpool_free(mco_workpool_t* pool) {
    if (!pool) return;
    mco_workpool_stop(pool);
    if (mco_workpool_drain(pool, 5000) != 0) {
        /* a task is stuck: degrade to free_now so join() cannot hang forever */
        hmutex_lock(&pool->qlock);
        pool->free_now = 1;
        hmutex_unlock(&pool->qlock);
        mco_wp_wake_all(pool);
    }
    mco_wp_join_and_free(pool);
}

void mco_workpool_free_now(mco_workpool_t* pool) {
    if (!pool) return;

    hmutex_lock(&pool->qlock);
    pool->shutdown = 1;
    pool->free_now = 1;
    hmutex_unlock(&pool->qlock);

    mco_wp_release_waiters(pool);
    mco_wp_wake_all(pool);
    mco_wp_join_and_free(pool);
}
