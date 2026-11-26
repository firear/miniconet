/* workpool: non-blocking submit + worker health (fixes 1 and 3)
 *
 *   1. try_async() at the inflight cap returns MCO_ERR_BUSY immediately instead
 *      of parking the caller (the acceptor must never stall).
 *   2. async() called from a PLAIN thread while the pool is full returns
 *      MCO_ERR_BUSY too — parking is coroutine-only, so it must not crash.
 *   3. worker_count() reports the workers whose thread really started, and
 *      active() tracks queued+running tasks until they finish.
 *
 * Only compiled when MINICONET_WITH_WORKPOOL=ON.
 */
#include "../miniconet.h"
#include "../core/mco_workpool.h"
#include "../compat/htime.h"
#include "../compat/hthread.h"
#include <stdio.h>
#include <string.h>

#define N_WORKERS   2
#define MAX_INFLIGHT 2
#define TASK_MS     120

static mco_loop_t* g_loop = NULL;
static int g_done = 0;
static int g_test_ok = 0;

/* result of the plain-thread submit */
static int g_thread_rc = 12345;

static void slow_task(void* arg) {
    (void)arg;
    mco_sleep(TASK_MS);          /* yields the vcpu; task stays "active" */
    g_done++;
}

/* plain OS thread (NOT a coroutine): the pool is full, so async() must report
 * MCO_ERR_BUSY rather than trying to park a thread that has no coroutine. */
static HTHREAD_ROUTINE(plain_submit_thread) {
    mco_workpool_t* pool = (mco_workpool_t*)userdata;
    g_thread_rc = mco_workpool_async(pool, slow_task, NULL);
    return 0;
}

static void main_task(void* arg) {
    mco_workpool_t* pool;
    int rc, i;
    int ok = 1;
    uv_thread_options_t opt;
    hthread_t th;

    (void)arg;

    pool = mco_workpool_new(N_WORKERS, MAX_INFLIGHT);
    if (!pool) {
        printf("[try_async] new failed\n");
        g_test_ok = 0;
        mco_loop_stop(g_loop);
        return;
    }

    if (mco_workpool_worker_count(pool) != N_WORKERS) {
        printf("[try_async] worker_count=%d, expected %d\n",
               mco_workpool_worker_count(pool), N_WORKERS);
        ok = 0;
    }

    /* fill the pool */
    for (i = 0; i < MAX_INFLIGHT; ++i) {
        rc = mco_workpool_try_async(pool, slow_task, NULL);
        if (rc != 0) {
            printf("[try_async] fill submit %d rc=%d\n", i, rc);
            ok = 0;
        }
    }

    /* full: must fail fast, must not queue */
    rc = mco_workpool_try_async(pool, slow_task, NULL);
    if (rc != MCO_ERR_BUSY) {
        printf("[try_async] expected MCO_ERR_BUSY, got %d\n", rc);
        ok = 0;
    }

    /* full + plain thread: must also report BUSY, not park/crash */
    opt.flags = UV_THREAD_NO_FLAGS;
    opt.stack_size = 0;
    th = hthread_create(&opt, plain_submit_thread, pool);
    if (th) hthread_join(th);
    if (g_thread_rc != MCO_ERR_BUSY) {
        printf("[try_async] plain-thread async rc=%d, expected MCO_ERR_BUSY\n", g_thread_rc);
        ok = 0;
    }

    /* still busy right away (tasks sleep TASK_MS) */
    if (mco_workpool_active(pool) != MAX_INFLIGHT) {
        printf("[try_async] active=%d, expected %d\n",
               mco_workpool_active(pool), MAX_INFLIGHT);
        ok = 0;
    }

    /* wait for completion through drain() */
    if (mco_workpool_drain(pool, 5000) != 0) {
        printf("[try_async] drain timed out\n");
        ok = 0;
    }
    if (g_done != MAX_INFLIGHT) {
        printf("[try_async] done=%d, expected %d\n", g_done, MAX_INFLIGHT);
        ok = 0;
    }
    if (mco_workpool_active(pool) != 0) {
        printf("[try_async] active after drain=%d\n", mco_workpool_active(pool));
        ok = 0;
    }

    /* slot freed again: try_async must succeed now */
    rc = mco_workpool_try_async(pool, slow_task, NULL);
    if (rc != 0) {
        printf("[try_async] submit after drain rc=%d\n", rc);
        ok = 0;
    }

    printf("[try_async] workers=%d done=%d active=%d\n",
           mco_workpool_worker_count(pool), g_done, mco_workpool_active(pool));

    mco_workpool_free(pool);
    g_test_ok = ok;
    mco_loop_stop(g_loop);
}

int main(void) {
    g_loop = mco_loop_new();
    mco_spawn(g_loop, main_task, NULL);
    mco_loop_run(g_loop);
    mco_loop_free(g_loop);

    printf(g_test_ok ? "WORKPOOL TRY_ASYNC TEST PASSED\n" : "WORKPOOL TRY_ASYNC TEST FAILED\n");
    return g_test_ok ? 0 : 1;
}
