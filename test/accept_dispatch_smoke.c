/* End-to-end acceptor + workpool test (Step 4): the acceptor loops
 * mco_tcp_accept_fd() -> mco_workpool_async() to a worker vcpu, which adopts the
 * fd and echoes. The client opens N_CONNS connections; verify requests spread over
 * multiple OS threads instead of piling onto one.
 * Lives in test/ (gated by MINICONET_WITH_WORKPOOL) because asserting dispatch
 * balance is a test, while examples/ holds resident samples not registered in
 * CTest.
 */
#include "../miniconet.h"
#include "../core/mco_workpool.h"
#include "../core/mco_waitgroup.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#define TEST_PORT   17333
#define N_WORKERS   4
#define N_CONNS     16

/* ---- per-OS-thread count of connections handled by each worker ---- */
typedef struct { pthread_t tid; int count; } worker_stat_t;
static pthread_mutex_t g_stat_lock = PTHREAD_MUTEX_INITIALIZER;
static worker_stat_t g_stats[N_WORKERS];
static int g_n_distinct = 0;

static void record_hit(void) {
    pthread_t self = pthread_self();
    int i;
    pthread_mutex_lock(&g_stat_lock);
    for (i = 0; i < g_n_distinct; ++i) {
        if (pthread_equal(g_stats[i].tid, self)) { g_stats[i].count++; pthread_mutex_unlock(&g_stat_lock); return; }
    }
    if (g_n_distinct < N_WORKERS) {
        g_stats[g_n_distinct].tid = self;
        g_stats[g_n_distinct].count = 1;
        g_n_distinct++;
    }
    pthread_mutex_unlock(&g_stat_lock);
}

static mco_waitgroup_t* g_wg = NULL;

/* worker side: adopt fd -> one echo round -> record -> done */
static void handle_conn(void* arg) {
    int fd = *(int*)arg;
    mco_socket_t* conn;
    char buf[64];
    int n;
    free(arg);

    conn = mco_socket_adopt_fd(fd);
    if (!conn) { close(fd); mco_waitgroup_done(g_wg); return; }

    n = mco_read(conn, buf, sizeof(buf) - 1, 3000);
    if (n > 0) mco_write(conn, buf, n, 3000);

    record_hit();

    mco_close(conn);
    mco_socket_destroy(conn);
    mco_waitgroup_done(g_wg);
}

static int g_accept_ok = 1;

/* acceptor: loop accept_fd + workpool_async, stop after N_CONNS */
static void acceptor_task(void* arg) {
    mco_socket_t* listener = (mco_socket_t*)arg;
    mco_workpool_t* pool;
    int i;

    pool = mco_workpool_new(N_WORKERS, 0);  /* backpressure not tested here */
    if (!pool) { printf("[dispatch] workpool_new failed\n"); g_accept_ok = 0; mco_close(listener); mco_socket_destroy(listener); return; }

    g_wg = mco_waitgroup_new();
    mco_waitgroup_add(g_wg, N_CONNS);

    for (i = 0; i < N_CONNS; ++i) {
        int fd = mco_tcp_accept_fd(listener, 5000);
        int* boxed;
        int r;
        if (fd < 0) { printf("[dispatch] accept_fd[%d] failed: %d\n", i, fd); g_accept_ok = 0; break; }

        boxed = (int*)malloc(sizeof(int));
        *boxed = fd;
        r = mco_workpool_async(pool, handle_conn, boxed);
        if (r != 0) {
            printf("[dispatch] async[%d] failed: %d\n", i, r);
            g_accept_ok = 0;
            free(boxed);
            close(fd);
            mco_waitgroup_done(g_wg); /* add(N_CONNS) reserved a slot for this one */
        }
    }

    mco_waitgroup_wait(g_wg, 5000);
    mco_waitgroup_free(g_wg);
    g_wg = NULL;
    mco_workpool_free(pool);

    mco_close(listener);
    mco_socket_destroy(listener);
}

/* ---- client thread: N_CONNS concurrent connections ---- */
static int g_client_ok_count = 0;

static void client_conn_task(void* arg) {
    (void)arg;
    mco_socket_t* conn = mco_tcp_connect("127.0.0.1", TEST_PORT, 3000, NULL);
    char buf[64];
    int n;
    if (!conn) return;

    n = mco_write(conn, "ping", 4, 3000);
    if (n == 4) {
        n = mco_read(conn, buf, sizeof(buf) - 1, 3000);
        if (n == 4 && memcmp(buf, "ping", 4) == 0) g_client_ok_count++;
    }
    mco_close(conn);
    mco_socket_destroy(conn);
}

static void client_spawn_task(void* arg) {
    (void)arg;
    mco_sleep(60); /* let acceptor/workpool come up first */
}

static void* client_thread_main(void* arg) {
    mco_loop_t* loop = mco_loop_new();
    int i;
    (void)arg;
    /* mco_sleep() must run inside a coroutine, not in a raw pthread body */
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
    int i, min_count, max_count;

    if (!listener) { printf("listen failed\n"); return 1; }

    pthread_create(&client_th, NULL, client_thread_main, NULL);

    mco_spawn(server_loop, acceptor_task, listener);
    mco_loop_run(server_loop);
    mco_loop_free(server_loop);

    pthread_join(client_th, NULL);

    min_count = max_count = g_stats[0].count;
    for (i = 1; i < g_n_distinct; ++i) {
        if (g_stats[i].count < min_count) min_count = g_stats[i].count;
        if (g_stats[i].count > max_count) max_count = g_stats[i].count;
    }
    for (i = 0; i < g_n_distinct; ++i) {
        printf("[dispatch] worker#%d handled %d conns\n", i, g_stats[i].count);
    }
    printf("[dispatch] n_distinct_worker_threads=%d, client_ok=%d/%d, accept_ok=%d\n",
           g_n_distinct, g_client_ok_count, N_CONNS, g_accept_ok);

    /* Assert work spread over more than one OS thread, and round-robin roughly
     * even (per-worker counts differ by at most 1, absorbing startup jitter). */
    if (g_accept_ok && g_client_ok_count == N_CONNS &&
        g_n_distinct >= 2 && (max_count - min_count) <= 1) {
        printf("SMOKE TEST PASSED\n");
        return 0;
    }
    printf("SMOKE TEST FAILED\n");
    return 1;
}
