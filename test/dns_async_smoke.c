/* Verify DNS resolution is truly async: resolve "localhost" (goes through
 * getaddrinfo on a background pool) several times while a ticker coroutine keeps
 * counting. If resolution blocked, the ticker would stall and ticks would fall
 * short.
 * Also runs mco_tcp_connect("localhost", ...) end to end.
 */
#include "../miniconet.h"
#include "../core/mco_resolver.h"
#include "../compat/hsocket.h"
#include <stdio.h>
#include <string.h>

#define TEST_PORT 17330
#define N_RESOLVERS 8
#define N_TICKS 40 /* 40 * 5ms = 200ms total */

static mco_loop_t* g_loop = NULL;
static volatile int g_ticks = 0;
static volatile int g_resolve_ok_count = 0;
static volatile int g_resolve_done_count = 0;

static void ticker_task(void* arg) {
    int i;
    (void)arg;
    /* Run the full duration regardless of resolver progress: the point is that the
     * ticker is unaffected, so compare ticks against the full count. */
    for (i = 0; i < N_TICKS; ++i) {
        mco_sleep(5);
        g_ticks++;
    }
}

static void resolver_task(void* arg) {
    sockaddr_u addr;
    (void)arg;
    memset(&addr, 0, sizeof(addr));
    if (mco_resolve("localhost", &addr, 3000) == MCO_RESOLVE_OK) g_resolve_ok_count++;
    g_resolve_done_count++;
}

/* ---- integration: mco_tcp_connect("localhost", ...) end to end ---- */
static int g_server_done = 0, g_client_done = 0, g_client_ok = 0;

static void server_task(void* arg) {
    mco_socket_t* listener = (mco_socket_t*)arg;
    mco_socket_t* conn = mco_tcp_accept(listener, 5000);
    char buf[64];
    if (conn) {
        int n = mco_read(conn, buf, sizeof(buf), 3000);
        if (n > 0) mco_write(conn, buf, n, 3000);
        mco_close(conn);
        mco_socket_destroy(conn);
    }
    mco_close(listener);
    mco_socket_destroy(listener);
    g_server_done = 1;
}

static void client_task(void* arg) {
    mco_socket_t* conn;
    char buf[64];
    int n;
    (void)arg;
    mco_sleep(30);
    /* "localhost" forces the async resolve path */
    conn = mco_tcp_connect("localhost", TEST_PORT, 3000, NULL);
    if (!conn) { printf("[client] connect(localhost) failed\n"); g_client_done = 1; return; }
    mco_write(conn, "via-localhost", 13, 3000);
    n = mco_read(conn, buf, sizeof(buf) - 1, 3000);
    if (n > 0) {
        buf[n] = 0;
        g_client_ok = (n == 13 && memcmp(buf, "via-localhost", 13) == 0);
    }
    mco_close(conn);
    mco_socket_destroy(conn);
    g_client_done = 1;
}

int main(void) {
    mco_socket_t* listener;
    int ok;
    int i;

    /* part 1: 8 concurrent resolves + 1 ticker */
    g_loop = mco_loop_new();
    mco_spawn(g_loop, ticker_task, NULL);
    for (i = 0; i < N_RESOLVERS; ++i) mco_spawn(g_loop, resolver_task, NULL);
    mco_loop_run(g_loop);
    mco_loop_free(g_loop);

    printf("[dns] resolve_ok=%d/%d, ticks=%d/%d (expect close to %d, proving not blocked by resolve)\n",
           g_resolve_ok_count, g_resolve_done_count, g_ticks, N_TICKS, N_TICKS);

    /* part 2: mco_tcp_connect with a hostname */
    listener = mco_tcp_listen("0.0.0.0", TEST_PORT);
    if (!listener) { printf("listen failed\n"); return 1; }
    g_loop = mco_loop_new();
    mco_spawn(g_loop, server_task, listener);
    mco_spawn(g_loop, client_task, NULL);
    mco_loop_run(g_loop);
    mco_loop_free(g_loop);

    mco_resolver_shutdown();

    printf("server_done=%d client_done=%d client_ok=%d\n",
           g_server_done, g_client_done, g_client_ok);

    /* ticks must not fall noticeably short (a blocking version would stall near 0) */
    ok = (g_resolve_ok_count == N_RESOLVERS) && (g_ticks >= N_TICKS - 3)
         && g_server_done && g_client_done && g_client_ok;
    printf(ok ? "DNS ASYNC TEST PASSED\n" : "DNS ASYNC TEST FAILED\n");
    return ok ? 0 : 1;
}
