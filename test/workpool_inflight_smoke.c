/* mco_workpool max_inflight blocking semantics (the test from Step 1):
 *   1. max_inflight=1: two coroutines submit a slow task concurrently; exactly one
 *      returns immediately and the other blocks until the first finishes.
 *   2. max_inflight<=0: both submissions return immediately (the "never block"
 *      usage must not regress).
 * Only compiled when MINICONET_WITH_WORKPOOL=ON.
 */
#include "../miniconet.h"
#include "../core/mco_workpool.h"
#include "../compat/htime.h"
#include <stdio.h>

#define SLOW_MS        200
#define FAST_MAX_MS    80    /* well under SLOW_MS = "not blocked" */
#define BLOCKED_MIN_MS 120   /* close to SLOW_MS = "blocked for a slot" */

static void slow_task(void* arg) {
    (void)arg;
    mco_sleep(SLOW_MS);
}

/* ---- Part 1: max_inflight=1, expect one fast and one slow ---- */

static mco_loop_t* g_loop1 = NULL;
static mco_workpool_t* g_pool1 = NULL;
static long g_elapsed1[2];
static int g_done1 = 0;

static void submitter1(void* arg) {
    int idx = *(int*)arg;
    uint64_t t0, t1;
    int r;

    t0 = gettimeofday_ms();
    r = mco_workpool_async(g_pool1, slow_task, NULL);
    t1 = gettimeofday_ms();
    g_elapsed1[idx] = (long)(t1 - t0);

    if (r != 0) printf("[inflight] part1 submitter[%d] async failed: %d\n", idx, r);

    if (++g_done1 == 2) {
        /* Both async() calls have returned, but the tasks may still be running.
         * Wait for them before free, so this test stays focused on max_inflight
         * rather than on force-finishing in-flight coroutines. */
        mco_sleep(SLOW_MS + 100);
        mco_workpool_free(g_pool1);
        mco_loop_stop(g_loop1);
    }
}

static int run_part1(void) {
    static int idx0 = 0, idx1 = 1;
    int a, b;

    g_loop1 = mco_loop_new();
    g_pool1 = mco_workpool_new(1, 1); /* 1 worker, max_inflight=1 */
    if (!g_pool1) { printf("[inflight] part1 workpool_new failed\n"); return 0; }

    /* both coroutines submit "at the same time" (real contention, not staged) */
    mco_spawn(g_loop1, submitter1, &idx0);
    mco_spawn(g_loop1, submitter1, &idx1);
    mco_loop_run(g_loop1);
    mco_loop_free(g_loop1);

    a = g_elapsed1[0]; b = g_elapsed1[1];
    printf("[inflight] part1 elapsed = [%d, %d]ms\n", a, b);

    /* exactly one fast, one slow — regardless of who grabbed the slot */
    return (a < FAST_MAX_MS && b >= BLOCKED_MIN_MS) ||
           (b < FAST_MAX_MS && a >= BLOCKED_MIN_MS);
}

/* ---- Part 2: max_inflight<=0, expect neither to block ---- */

static mco_loop_t* g_loop2 = NULL;
static mco_workpool_t* g_pool2 = NULL;
static long g_elapsed2[2];
static int g_done2 = 0;

static void submitter2(void* arg) {
    int idx = *(int*)arg;
    uint64_t t0, t1;
    int r;

    t0 = gettimeofday_ms();
    r = mco_workpool_async(g_pool2, slow_task, NULL);
    t1 = gettimeofday_ms();
    g_elapsed2[idx] = (long)(t1 - t0);

    if (r != 0) printf("[inflight] part2 submitter[%d] async failed: %d\n", idx, r);

    if (++g_done2 == 2) {
        /* as in Part 1: wait for both slow tasks before freeing */
        mco_sleep(SLOW_MS + 100);
        mco_workpool_free(g_pool2);
        mco_loop_stop(g_loop2);
    }
}

static int run_part2(void) {
    static int idx0 = 0, idx1 = 1;

    g_loop2 = mco_loop_new();
    g_pool2 = mco_workpool_new(1, 0); /* max_inflight<=0: unlimited */
    if (!g_pool2) { printf("[inflight] part2 workpool_new failed\n"); return 0; }

    mco_spawn(g_loop2, submitter2, &idx0);
    mco_spawn(g_loop2, submitter2, &idx1);
    mco_loop_run(g_loop2);
    mco_loop_free(g_loop2);

    printf("[inflight] part2 elapsed = [%ld, %ld]ms\n", g_elapsed2[0], g_elapsed2[1]);

    return g_elapsed2[0] < FAST_MAX_MS && g_elapsed2[1] < FAST_MAX_MS;
}

int main(void) {
    int ok1 = run_part1();
    int ok2 = run_part2();

    printf("part1_ok=%d part2_ok=%d\n", ok1, ok2);

    if (ok1 && ok2) { printf("SMOKE TEST PASSED\n"); return 0; }
    printf("SMOKE TEST FAILED\n");
    return 1;
}
