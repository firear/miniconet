/* Backpressure check (Step 5): max_inflight=1, single worker, handle_conn sleeps
 * 300ms. The acceptor accepts 3 connections and submits each: the first returns
 * immediately, the next two block until the previous task finishes. Timing each
 * mco_workpool_async() call shows what "the acceptor coroutine gets blocked"
 * means: not accept() but the submit step, so new connections pile up in the
 * kernel backlog instead of queueing unbounded tasks.
 * Only compiled when MINICONET_WITH_WORKPOOL=ON.
 */
#include "../miniconet.h"
#include "../core/mco_workpool.h"
#include "../compat/htime.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#define TEST_PORT     17334
#define N_CONNS       3
#define SLOW_MS       300
/* threshold well under SLOW_MS, with slack for slow machines / jitter */
#define BLOCKED_MIN_MS 150
#define FAST_MAX_MS    100

static void slow_handle_conn(void* arg) {
    int fd = *(int*)arg;
    mco_socket_t* conn;
    free(arg);

    conn = mco_socket_adopt_fd(fd);
    if (!conn) { close(fd); return; }

    mco_sleep(SLOW_MS);  /* deliberately slow, holds the only inflight slot */

    mco_close(conn);
    mco_socket_destroy(conn);
}

static long g_submit_elapsed_ms[N_CONNS];
static int g_accept_ok = 1;
static int g_async_ok = 1;

static void acceptor_task(void* arg) {
    mco_socket_t* listener = (mco_socket_t*)arg;
    mco_workpool_t* pool;
    int i;

    pool = mco_workpool_new(1, 1);  /* 1 worker, max_inflight=1: deliberately tight */
    if (!pool) { printf("[backpressure] workpool_new failed\n"); g_accept_ok = 0; mco_close(listener); mco_socket_destroy(listener); return; }

    for (i = 0; i < N_CONNS; ++i) {
        int fd = mco_tcp_accept_fd(listener, 5000);
        int* boxed;
        uint64_t t0, t1;
        int r;
        if (fd < 0) { printf("[backpressure] accept_fd[%d] failed: %d\n", i, fd); g_accept_ok = 0; break; }

        boxed = (int*)malloc(sizeof(int));
        *boxed = fd;

        t0 = gettimeofday_ms();
        r = mco_workpool_async(pool, slow_handle_conn, boxed);
        t1 = gettimeofday_ms();
        g_submit_elapsed_ms[i] = (long)(t1 - t0);

        if (r != 0) { printf("[backpressure] async[%d] failed: %d\n", i, r); g_async_ok = 0; free(boxed); close(fd); }
        printf("[backpressure] submit[%d] took %ldms\n", i, g_submit_elapsed_ms[i]);
    }

    /* let the last task finish before free, to stay focused on backpressure */
    mco_sleep(SLOW_MS + 100);
    mco_workpool_free(pool);

    mco_close(listener);
    mco_socket_destroy(listener);
}

static int g_client_ok_count = 0;

static void client_conn_task(void* arg) {
    (void)arg;
    mco_socket_t* conn = mco_tcp_connect("127.0.0.1", TEST_PORT, 3000, NULL);
    if (!conn) return;
    /* no data exchange needed: just connect and hold an fd until the test ends */
    g_client_ok_count++;
    mco_sleep(2000);
    mco_close(conn);
    mco_socket_destroy(conn);
}

static void client_spawn_task(void* arg) {
    (void)arg;
    mco_sleep(60);
}

static void* client_thread_main(void* arg) {
    mco_loop_t* loop = mco_loop_new();
    int i;
    (void)arg;
    /* mco_sleep() must run inside a coroutine, not a raw pthread body */
    mco_spawn(loop, client_spawn_task, NULL);
    mco_loop_run(loop);
    for (i = 0; i < N_CONNS; ++i) mco_spawn(loop, client_conn_task, NULL);
    mco_loop_run(loop);
    mco_loop_free(loop);
    return NULL;
}

int main(void) {
    mco_loop_t* server_loop = mco_loop_new();
    mco_socket_t* listener = mco_tcp_listen("0.0.0.0", TEST_PORT);
    pthread_t client_th;
    int ok;

    if (!listener) { printf("listen failed\n"); return 1; }

    pthread_create(&client_th, NULL, client_thread_main, NULL);

    mco_spawn(server_loop, acceptor_task, listener);
    mco_loop_run(server_loop);
    mco_loop_free(server_loop);

    pthread_join(client_th, NULL);

    printf("[backpressure] client_ok=%d/%d accept_ok=%d async_ok=%d\n",
           g_client_ok_count, N_CONNS, g_accept_ok, g_async_ok);

    /* assert: 1st submit fast, 2nd/3rd blocked on the previous slow task */
    ok = g_accept_ok && g_async_ok && g_client_ok_count == N_CONNS &&
         g_submit_elapsed_ms[0] < FAST_MAX_MS &&
         g_submit_elapsed_ms[1] >= BLOCKED_MIN_MS &&
         g_submit_elapsed_ms[2] >= BLOCKED_MIN_MS;

    if (ok) { printf("SMOKE TEST PASSED\n"); return 0; }
    printf("SMOKE TEST FAILED\n");
    return 1;
}
