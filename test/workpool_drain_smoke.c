/* workpool: two-phase shutdown (fix 2)
 *
 *   1. stop() refuses new work (try_async -> MCO_ERR_CLOSED) but tasks already
 *      accepted still run to completion; drain() waits for all of them.
 *   2. drain(timeout) reports MCO_ERR_TIMEOUT while a slow task is still running
 *      (instead of silently abandoning it).
 *   3. free() waits for in-flight work; free_now() drops it (escape hatch).
 *
 * Only compiled when MINICONET_WITH_WORKPOOL=ON.
 */
#include "../miniconet.h"
#include "../core/mco_workpool.h"
#include "../compat/htime.h"
#include <stdio.h>
#include <string.h>

#define N_TASKS   8
#define TASK_MS   40
#define SLOW_MS   300

static mco_loop_t* g_loop = NULL;
static int g_done = 0;
static int g_test_ok = 0;

static void counted_task(void* arg) {
    (void)arg;
    mco_sleep(TASK_MS);
    g_done++;
}

static void slow_task(void* arg) {
    (void)arg;
    mco_sleep(SLOW_MS);
    g_done++;
}

static void main_task(void* arg) {
    mco_workpool_t* pool;
    int i, ok = 1, rc;

    (void)arg;

    /* ---- 1. stop + drain waits for accepted tasks ---- */
    g_done = 0;
    pool = mco_workpool_new(2, 0);
    if (!pool) {
        printf("[drain] new failed\n");
        g_test_ok = 0;
        mco_loop_stop(g_loop);
        return;
    }
    for (i = 0; i < N_TASKS; ++i) {
        if (mco_workpool_async(pool, counted_task, NULL) != 0) {
            printf("[drain] submit %d failed\n", i);
            ok = 0;
        }
    }

    mco_workpool_stop(pool);

    rc = mco_workpool_try_async(pool, counted_task, NULL);
    if (rc != MCO_ERR_CLOSED) {
        printf("[drain] try_async after stop rc=%d, expected MCO_ERR_CLOSED\n", rc);
        ok = 0;
    }

    rc = mco_workpool_drain(pool, 5000);
    if (rc != 0) {
        printf("[drain] drain rc=%d\n", rc);
        ok = 0;
    }
    if (g_done != N_TASKS) {
        printf("[drain] done=%d, expected %d (stop must not abandon queued work)\n",
               g_done, N_TASKS);
        ok = 0;
    }
    printf("[drain] stop+drain: done=%d/%d\n", g_done, N_TASKS);

    /* free() on an already stopped pool must be a no-op drain + join */
    mco_workpool_free(pool);

    /* ---- 2. drain timeout while a task is still running ---- */
    g_done = 0;
    pool = mco_workpool_new(1, 0);
    if (!pool) {
        printf("[drain] new(1) failed\n");
        ok = 0;
        g_test_ok = ok;
        mco_loop_stop(g_loop);
        return;
    }
    if (mco_workpool_async(pool, slow_task, NULL) != 0) {
        printf("[drain] slow submit failed\n");
        ok = 0;
    }
    rc = mco_workpool_drain(pool, 50);
    if (rc != MCO_ERR_TIMEOUT) {
        printf("[drain] expected MCO_ERR_TIMEOUT, got %d\n", rc);
        ok = 0;
    }
    /* free() must wait for the slow task instead of killing it */
    mco_workpool_free(pool);
    if (g_done != 1) {
        printf("[drain] free did not wait: done=%d\n", g_done);
        ok = 0;
    }
    printf("[drain] timeout + free-waits: done=%d\n", g_done);

    /* ---- 3. free_now drops in-flight work (no wait) ---- */
    g_done = 0;
    pool = mco_workpool_new(1, 0);
    if (!pool) {
        printf("[drain] new(1) failed\n");
        ok = 0;
        g_test_ok = ok;
        mco_loop_stop(g_loop);
        return;
    }
    mco_workpool_async(pool, slow_task, NULL);
    mco_workpool_free_now(pool);      /* returns without waiting SLOW_MS */
    if (g_done != 0) {
        printf("[drain] free_now ran the task: done=%d\n", g_done);
        ok = 0;
    }
    printf("[drain] free_now: done=%d (expected 0)\n", g_done);

    g_test_ok = ok;
    mco_loop_stop(g_loop);
}

int main(void) {
    g_loop = mco_loop_new();
    mco_spawn(g_loop, main_task, NULL);
    mco_loop_run(g_loop);
    mco_loop_free(g_loop);

    printf(g_test_ok ? "WORKPOOL DRAIN TEST PASSED\n" : "WORKPOOL DRAIN TEST FAILED\n");
    return g_test_ok ? 0 : 1;
}
