/* blockpool: blocking calls off the vcpu (fix 4)
 *
 * The main coroutine issues N blocking calls (usleep on a worker thread) through
 * mco_blockpool_run(). A heartbeat coroutine sleeps 5ms in a loop on the same
 * vcpu: if the blocking call had run on the vcpu, the heartbeat could not make
 * progress. So a healthy heartbeat count proves the offload works.
 *
 * Also checks: pending drops back to 0, run() after stop() returns
 * MCO_ERR_CLOSED, drain() reports completion.
 *
 * Only compiled when MINICONET_WITH_BLOCKPOOL=ON.
 */
#include "../miniconet.h"
#include "../core/mco_blockpool.h"
#include "../compat/htime.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define N_CALLS       20
#define BLOCK_US      5000      /* blocking work per call: 5ms */
#define HB_MS         5         /* heartbeat period */
#define MIN_HEARTBEATS 8        /* 20*5ms = 100ms => ~20 beats; 8 is a safe floor */

static mco_loop_t* g_loop = NULL;
static mco_blockpool_t* g_bp = NULL;
static int g_heartbeats = 0;
static int g_stop_heartbeat = 0;
static int g_test_ok = 0;

/* runs on a plain worker thread: MUST NOT touch vcpu state */
static void blocking_usleep(void* arg) {
    (void)arg;
    usleep(BLOCK_US);
}

static void heartbeat_task(void* arg) {
    (void)arg;
    while (!g_stop_heartbeat) {
        mco_sleep(HB_MS);
        g_heartbeats++;
    }
}

static void main_task(void* arg) {
    int i, rc, ok = 1;
    int64_t t0, t1;

    (void)arg;

    g_bp = mco_blockpool_new(2, 0);
    if (!g_bp) {
        printf("[blockpool] new failed\n");
        g_test_ok = 0;
        mco_loop_stop(g_loop);
        return;
    }
    if (mco_blockpool_worker_count(g_bp) != 2) {
        printf("[blockpool] worker_count=%d, expected 2\n",
               mco_blockpool_worker_count(g_bp));
        ok = 0;
    }

    mco_spawn(g_loop, heartbeat_task, NULL);

    t0 = gettimeofday_ms();
    for (i = 0; i < N_CALLS; ++i) {
        rc = mco_blockpool_run(g_bp, blocking_usleep, NULL, 5000);
        if (rc != 0) {
            printf("[blockpool] run %d rc=%d\n", i, rc);
            ok = 0;
            break;
        }
    }
    t1 = gettimeofday_ms();

    g_stop_heartbeat = 1;

    if (g_heartbeats < MIN_HEARTBEATS) {
        printf("[blockpool] heartbeats=%d (<%d): vcpu was blocked\n",
               g_heartbeats, MIN_HEARTBEATS);
        ok = 0;
    }
    if (mco_blockpool_pending(g_bp) != 0) {
        printf("[blockpool] pending=%d after completion\n", mco_blockpool_pending(g_bp));
        ok = 0;
    }
    if (mco_blockpool_drain(g_bp, 1000) != 0) {
        printf("[blockpool] drain failed\n");
        ok = 0;
    }

    /* after stop(): run must refuse */
    mco_blockpool_stop(g_bp);
    rc = mco_blockpool_run(g_bp, blocking_usleep, NULL, 1000);
    if (rc != MCO_ERR_CLOSED) {
        printf("[blockpool] run after stop rc=%d, expected MCO_ERR_CLOSED\n", rc);
        ok = 0;
    }

    printf("[blockpool] calls=%d elapsed=%lums heartbeats=%d pending=%d\n",
           N_CALLS, (unsigned long)(t1 - t0), g_heartbeats, mco_blockpool_pending(g_bp));

    mco_blockpool_free(g_bp);
    g_bp = NULL;

    g_test_ok = ok;
    mco_loop_stop(g_loop);
}

int main(void) {
    g_loop = mco_loop_new();
    mco_spawn(g_loop, main_task, NULL);
    mco_loop_run(g_loop);
    mco_loop_free(g_loop);

    printf(g_test_ok ? "BLOCKPOOL TEST PASSED\n" : "BLOCKPOOL TEST FAILED\n");
    return g_test_ok ? 0 : 1;
}
