/*
 * mco_blockpool.c — blocking-call offload pool (see mco_blockpool.h).
 *
 * N plain worker threads (no vcpu) take tasks from a mutex-protected queue. The
 * submitter is a coroutine: it parks on a one-shot and the worker fires it after
 * fn returns, which is delivered through the oneshot's wake channel — so the
 * vcpu event loop keeps running other coroutines meanwhile.
 *
 * Refcounting: the oneshot starts with one ref (submitter). A second ref is
 * taken when the task is queued (worker). Both sides release after use, so a
 * timed-out submitter cannot free the oneshot while the worker still needs it.
 */
#include "mco_blockpool.h"
#include "mco_vcpu.h"
#include "mco_oneshot.h"
#include "../compat/hthread.h"
#include "../compat/hmutex.h"
#include <stdlib.h>
#include <string.h>

typedef struct mco_bp_task {
    void (*fn)(void* arg);
    void* arg;
    mco_oneshot_t* done;
    struct mco_bp_task* next;
} mco_bp_task_t;

struct mco_blockpool {
    int             n;
    hthread_t*      threads;
    int             n_alive;

    hmutex_t        lock;
    hcondvar_t      work_cond;   /* workers wait here for a task */
    hcondvar_t      done_cond;   /* drain waits here for pending == 0 */
    mco_bp_task_t*  head;
    mco_bp_task_t*  tail;
    int             pending;     /* queued + running */
    int             queue_cap;   /* <=0 unlimited */
    int             shutdown;
};

static HTHREAD_ROUTINE(mco_bp_worker_main) {
    mco_blockpool_t* bp = (mco_blockpool_t*)userdata;

    for (;;) {
        mco_bp_task_t* t;

        hmutex_lock(&bp->lock);
        while (!bp->head && !bp->shutdown) {
            hcondvar_wait(&bp->work_cond, &bp->lock);
        }
        if (!bp->head) {
            /* shutdown and nothing left */
            hmutex_unlock(&bp->lock);
            break;
        }
        t = bp->head;
        bp->head = t->next;
        if (!bp->head) bp->tail = NULL;
        hmutex_unlock(&bp->lock);

        t->fn(t->arg);

        /* fire first, then drop the worker ref: the submitter may wake up and
         * release its own ref immediately. */
        mco_oneshot_fire(t->done);
        mco_oneshot_release(t->done);
        free(t);

        hmutex_lock(&bp->lock);
        if (bp->pending > 0) bp->pending--;
        if (bp->pending == 0) hcondvar_broadcast(&bp->done_cond);
        hmutex_unlock(&bp->lock);
    }
    return 0;
}

mco_blockpool_t* mco_blockpool_new(int n_threads, int queue_cap) {
    mco_blockpool_t* bp;
    int i;

    if (n_threads <= 0) n_threads = 1;

    bp = (mco_blockpool_t*)calloc(1, sizeof(mco_blockpool_t));
    if (!bp) return NULL;

    bp->n = n_threads;
    bp->queue_cap = queue_cap;
    bp->pending = 0;
    bp->shutdown = 0;
    bp->n_alive = 0;
    bp->head = NULL;
    bp->tail = NULL;
    hmutex_init(&bp->lock);
    hcondvar_init(&bp->work_cond);
    hcondvar_init(&bp->done_cond);

    bp->threads = (hthread_t*)calloc((size_t)n_threads, sizeof(hthread_t));
    if (!bp->threads) {
        hcondvar_destroy(&bp->done_cond);
        hcondvar_destroy(&bp->work_cond);
        hmutex_destroy(&bp->lock);
        free(bp);
        return NULL;
    }

    for (i = 0; i < n_threads; ++i) {
        uv_thread_options_t opt;
        opt.flags = UV_THREAD_NO_FLAGS;
        opt.stack_size = 0;
        bp->threads[i] = hthread_create(&opt, mco_bp_worker_main, bp);
        if (bp->threads[i]) bp->n_alive++;
    }

    if (bp->n_alive == 0) {
        hcondvar_destroy(&bp->done_cond);
        hcondvar_destroy(&bp->work_cond);
        hmutex_destroy(&bp->lock);
        free(bp->threads);
        free(bp);
        return NULL;
    }

    return bp;
}

void mco_blockpool_stop(mco_blockpool_t* bp) {
    if (!bp) return;
    hmutex_lock(&bp->lock);
    bp->shutdown = 1;
    hcondvar_broadcast(&bp->work_cond);   /* wake every worker so it can exit */
    hmutex_unlock(&bp->lock);
}

int mco_blockpool_drain(mco_blockpool_t* bp, int timeout_ms) {
    int rc = 0;

    if (!bp) return 0;

    hmutex_lock(&bp->lock);
    if (timeout_ms < 0) {
        while (bp->pending > 0) {
            hcondvar_wait(&bp->done_cond, &bp->lock);
        }
    } else {
        while (bp->pending > 0) {
            if (!hcondvar_wait_for(&bp->done_cond, &bp->lock, (unsigned int)timeout_ms)) {
                rc = MCO_ERR_TIMEOUT;
                break;
            }
        }
    }
    hmutex_unlock(&bp->lock);
    return rc;
}

int mco_blockpool_run(mco_blockpool_t* bp, void (*fn)(void* arg), void* arg,
                      int timeout_ms) {
    mco_bp_task_t* t;
    mco_oneshot_t* one;
    int rc;

    if (!bp || !fn) return MCO_ERR_CLOSED;
    if (!mco_vcpu_self()) return MCO_ERR_ADDR;   /* coroutine-only */

    one = mco_oneshot_new();
    if (!one) return MCO_ERR_IO;

    t = (mco_bp_task_t*)malloc(sizeof(mco_bp_task_t));
    if (!t) {
        mco_oneshot_release(one);
        return MCO_ERR_IO;
    }
    t->fn = fn;
    t->arg = arg;
    t->done = one;
    t->next = NULL;

    hmutex_lock(&bp->lock);
    if (bp->shutdown || bp->n_alive <= 0) {
        hmutex_unlock(&bp->lock);
        free(t);
        mco_oneshot_release(one);
        return MCO_ERR_CLOSED;
    }
    if (bp->queue_cap > 0 && bp->pending >= bp->queue_cap) {
        hmutex_unlock(&bp->lock);
        free(t);
        mco_oneshot_release(one);
        return MCO_ERR_BUSY;
    }
    mco_oneshot_retain(one);      /* the worker now holds a ref too */
    if (bp->tail) bp->tail->next = t; else bp->head = t;
    bp->tail = t;
    bp->pending++;
    hcondvar_signal(&bp->work_cond);
    hmutex_unlock(&bp->lock);

    rc = mco_oneshot_wait(one, timeout_ms);
    mco_oneshot_release(one);     /* drop the submitter ref */
    return rc;
}

int mco_blockpool_pending(mco_blockpool_t* bp) {
    int v;
    if (!bp) return 0;
    hmutex_lock(&bp->lock);
    v = bp->pending;
    hmutex_unlock(&bp->lock);
    return v;
}

int mco_blockpool_worker_count(mco_blockpool_t* bp) {
    int v;
    if (!bp) return 0;
    hmutex_lock(&bp->lock);
    v = bp->n_alive;
    hmutex_unlock(&bp->lock);
    return v;
}

void mco_blockpool_free(mco_blockpool_t* bp) {
    int i;

    if (!bp) return;

    mco_blockpool_stop(bp);
    /* Blocking calls cannot be cancelled: wait for them (a stuck fn blocks here,
     * which is the documented trade-off of offloading). */
    mco_blockpool_drain(bp, 5000);

    for (i = 0; i < bp->n; ++i) {
        if (bp->threads[i]) hthread_join(bp->threads[i]);
    }

    /* any task left behind (drain timed out) is leaked on purpose: its fn may
     * still be running on a worker that we just joined. */
    hcondvar_destroy(&bp->done_cond);
    hcondvar_destroy(&bp->work_cond);
    hmutex_destroy(&bp->lock);
    free(bp->threads);
    free(bp);
}
