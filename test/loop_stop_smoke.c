/*
 * loop_stop_smoke.c — mco_loop_stop() must end mco_loop_run() promptly even
 * when another coroutine is parked on a long timer, and mco_loop_free() must
 * reclaim the coroutines the stop abandoned (no leak, no use-after-free).
 *
 * Regression for: loop teardown used to wait out the longest parked timer, so a
 * 5s sleep turned into a 5s shutdown. Run under ASan to catch the free path.
 */
#include "../miniconet.h"
#include "htime.h"
#include <stdio.h>

#define PARK_MS      5000   /* far longer than the 1s assertion below */
#define STOP_AFTER_MS  50   /* let the parked coroutine park first */

static int g_parked;   /* 1 = parked in the long sleep, 2 = resumed (must not happen) */
static int g_stopped;
static int g_spawned_after_stop;

static void parked_task(void* arg) {
    (void)arg;
    g_parked = 1;
    mco_sleep(PARK_MS);
    g_parked = 2;   /* reached only if the loop kept draining past the stop */
}

static void stopper_task(void* arg) {
    mco_loop_t* loop = (mco_loop_t*)arg;
    mco_sleep(STOP_AFTER_MS);
    g_stopped = 1;
    mco_loop_stop(loop);   /* must wake the loop instead of waiting PARK_MS */
}

int main(void) {
    mco_loop_t* loop;
    uint64_t t0, dt_ns;
    double elapsed_ms;
    int ok;

    loop = mco_loop_new();
    if (!loop) { printf("loop_stop_smoke: mco_loop_new failed\n"); return 1; }

    t0 = gethrtime_ns();
    mco_spawn(loop, parked_task, NULL);
    mco_spawn(loop, stopper_task, loop);
    mco_loop_run(loop);          /* returns as soon as the stop is requested */
    dt_ns = gethrtime_ns() - t0;
    elapsed_ms = (double)dt_ns / 1e6;

    g_spawned_after_stop = (g_parked == 2);

    /* Budget: the stop sleep plus slack. Waiting out PARK_MS (5000ms) fails. */
    ok = g_stopped && g_parked == 1 && !g_spawned_after_stop && elapsed_ms < 1000.0;

    printf("loop_stop_smoke: elapsed=%.1fms parked=%d stopped=%d resumed_past_stop=%d\n",
           elapsed_ms, g_parked, g_stopped, g_spawned_after_stop);

    /* Must reclaim parked_task's stack/task, not leak or touch freed memory. */
    mco_loop_free(loop);

    printf(ok ? "LOOP STOP TEST PASSED\n" : "LOOP STOP TEST FAILED\n");
    return ok ? 0 : 1;
}
