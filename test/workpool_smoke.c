/* workpool end-to-end (completion collected via mco_waitgroup):
 *   1. 8 tasks on a 4-worker pool, each doing CPU-bound work + mco_sleep. Sleep is
 *      multiplexed by the timer heap even on one vcpu, so a pure CPU loop (no
 *      yield) is what distinguishes single from multi vcpu; assert elapsed <
 *      threshold to catch "the pool degenerated to serial".
 *   2. Each task writes a checkable result to g_results[idx]; all must be correct.
 *   3. Completion via mco_waitgroup (Add -> async -> Wait), whose happens-before
 *      lets the main coroutine safely read every worker's result.
 * Only compiled when MINICONET_WITH_WORKPOOL=ON.
 */
#include "../miniconet.h"
#include "../core/mco_workpool.h"
#include "../core/mco_waitgroup.h"
#include "../compat/htime.h"
#include <stdio.h>
#include <string.h>

#define N_TASKS    8
#define N_WORKERS  4
#define TASK_MS    30
#define CPU_LOOP   8000000   /* pure CPU iterations, ~10-30ms, no yield */
#define EXPECTED_MAX_MS 250  /* 8 serial CPU chunks blow past this */

static mco_loop_t* g_loop = NULL;
static long g_results[N_TASKS];
static int  g_test_ok = 0;
static mco_waitgroup_t* g_wg = NULL;

typedef struct { int idx; } task_arg_t;
static task_arg_t g_args[N_TASKS];

/* task body on a worker vcpu: CPU work + sleep + write result + done */
static void worker_task(void* arg) {
    task_arg_t* a = (task_arg_t*)arg;
    int idx = a->idx;
    long sum = 0;
    int i;
    /* deterministic CPU work: sum i^2, result checkable */
    for (i = 0; i < 100000; ++i) sum += (long)i * (long)i;
    g_results[idx] = sum;

    /* extra CPU load (no yield) so multi-core parallelism shows up in wall time;
     * volatile stops the compiler deleting it */
    { volatile long busy = 0; int j; for (j = 0; j < CPU_LOOP; ++j) busy += j; (void)busy; }

    mco_sleep(TASK_MS);  /* simulate io wait, yields on the worker vcpu */

    mco_waitgroup_done(g_wg);  /* report completion; the last one releases the wait */
}

static void main_task(void* arg) {
    mco_workpool_t* pool;
    uint64_t t0, t1;
    int i;
    int ok;
    int r;
    (void)arg;

    pool = mco_workpool_new(N_WORKERS, 0);  /* max_inflight<=0 = unlimited */
    if (!pool) { printf("[workpool] new failed\n"); g_test_ok = 0; mco_loop_stop(g_loop); return; }

    g_wg = mco_waitgroup_new();
    if (!g_wg) { printf("[workpool] waitgroup_new failed\n"); mco_workpool_free(pool); g_test_ok = 0; mco_loop_stop(g_loop); return; }

    /* add before submit: an add/done race could hit 0 and fire early */
    mco_waitgroup_add(g_wg, N_TASKS);

    t0 = gettimeofday_ms();
    for (i = 0; i < N_TASKS; ++i) {
        g_args[i].idx = i;
        mco_workpool_async(pool, worker_task, &g_args[i]);
    }

    r = mco_waitgroup_wait(g_wg, 5000);
    t1 = gettimeofday_ms();

    if (r != 0) {
        /* timed out with tasks possibly in flight: per the waitgroup contract we
         * must not free; just report failure. */
        printf("[workpool] waitgroup_wait failed: %d\n", r);
        mco_workpool_free(pool);
        g_test_ok = 0;
        mco_loop_stop(g_loop);
        return;
    }

    mco_waitgroup_free(g_wg);  /* all done, safe to free */
    g_wg = NULL;
    mco_workpool_free(pool);

    /* check: sum i^2 = 99999*100000*199999/6 */
    {
        long expected = 99999L * 100000L * 199999L / 6;
        int results_ok = 1;
        for (i = 0; i < N_TASKS; ++i) {
            if (g_results[i] != expected) {
                results_ok = 0;
                printf("[workpool] result[%d]=%ld != %ld\n", i, g_results[i], expected);
            }
        }
        ok = results_ok && ((t1 - t0) < EXPECTED_MAX_MS);
    }

    printf("[workpool] done=%d/%d, elapsed=%lums (threshold<%d)\n",
           N_TASKS, N_TASKS, (unsigned long)(t1 - t0), EXPECTED_MAX_MS);

    g_test_ok = ok ? 1 : 0;
    mco_loop_stop(g_loop);
}

int main(void) {
    g_loop = mco_loop_new();
    mco_spawn(g_loop, main_task, NULL);
    mco_loop_run(g_loop);
    mco_loop_free(g_loop);

    printf(g_test_ok ? "WORKPOOL TEST PASSED\n" : "WORKPOOL TEST FAILED\n");
    return g_test_ok ? 0 : 1;
}
