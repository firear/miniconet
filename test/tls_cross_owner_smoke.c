/* Deterministically test the ssl_cross_owner guard itself. Triggering a real
 * OpenSSL renegotiation that lands in the cross-direction wait window depends on
 * version-specific internals and does not reproduce on fast loopback, so use
 * mco_test_socket_set_ssl_cross_owner() (test-only, needs
 * MINICONET_TEST_HOOKS) to force "another coroutine owns this SSL object":
 *   1. while owned, another coroutine's mco_write() returns MCO_ERR_BUSY at once;
 *   2. after release, normal io works again;
 *   3. io that never hits a cross-direction wait is unaffected (covered by
 *      tls_duplex_smoke.c).
 */
#include "../miniconet.h"
#include "../compat/ssl/hssl.h"
#include <stdio.h>
#include <string.h>

void mco_test_socket_set_ssl_cross_owner(mco_socket_t* s, void* owner); /* test only */

#define TEST_PORT 17364

static mco_loop_t* g_loop = NULL;
static mco_ssl_ctx_t g_server_ctx = NULL;
static mco_ssl_ctx_t g_client_ctx = NULL;
static mco_socket_t* g_conn = NULL;

static int g_owner_set = 0;
static int g_busy_result = -999;
static int g_after_release_result = -999;
static int g_done = 0;

/* "puppet" coroutine: pretend to renegotiate, hold ssl_cross_owner briefly */
static void owner_task(void* arg) {
    (void)arg;
    mco_test_socket_set_ssl_cross_owner(g_conn, mco_task_self());
    g_owner_set = 1;
    mco_sleep(150); /* pretend still busy */
    mco_test_socket_set_ssl_cross_owner(g_conn, NULL); /* release */
}

/* write while owner_task holds it; expect MCO_ERR_BUSY immediately */
static void probe_during_task(void* arg) {
    (void)arg;
    mco_sleep(30); /* ensure owner_task has set the flag */
    g_busy_result = mco_write(g_conn, "probe", 5, 1000);
    printf("[probe-during] mco_write while owned returned %d (expect %d = MCO_ERR_BUSY)\n",
           g_busy_result, MCO_ERR_BUSY);
}

/* retry after owner_task releases; expect a normal successful write */
static void probe_after_task(void* arg) {
    (void)arg;
    mco_sleep(250); /* owner_task releases at 150ms; wait a bit longer */
    g_after_release_result = mco_write(g_conn, "probe-after", 11, 1000);
    printf("[probe-after] mco_write after release returned %d (expect 11)\n", g_after_release_result);
    g_done = 1;
}

static void server_task(void* arg) {
    mco_socket_t* listener = (mco_socket_t*)arg;
    mco_socket_t* conn = mco_tcp_accept(listener, 5000);
    char buf[64];
    /* the server just drains the probe data; only the client's write result matters */
    if (conn && mco_ssl_server_handshake(conn, g_server_ctx, 3000) == 0) {
        int total = 0;
        while (total < 16 && mco_read(conn, buf, sizeof(buf), 2000) > 0) total++;
        mco_close(conn);
        mco_socket_destroy(conn);
    } else if (conn) {
        mco_close(conn);
        mco_socket_destroy(conn);
    }
    mco_close(listener);
    mco_socket_destroy(listener);
}

static void client_setup_task(void* arg) {
    (void)arg;
    mco_sleep(20);
    g_conn = mco_tls_connect("127.0.0.1", TEST_PORT, g_client_ctx, 3000, NULL);
    if (!g_conn) { printf("connect failed\n"); g_done = 1; return; }

    mco_spawn(g_loop, owner_task, NULL);
    mco_spawn(g_loop, probe_during_task, NULL);
    mco_spawn(g_loop, probe_after_task, NULL);
}

int main(void) {
    mco_socket_t* listener;
    int ok;

    if (!HV_WITH_SSL) {
        printf("current build has no real SSL backend, skip.\n");
        return 0;
    }

    g_server_ctx = mco_ssl_ctx_new_server("test/certs/server.crt", "test/certs/server.key", 0);
    g_client_ctx = mco_ssl_ctx_new_client(NULL, NULL, 0);
    if (!g_server_ctx || !g_client_ctx) { printf("ctx create failed\n"); return 1; }

    listener = mco_tcp_listen("0.0.0.0", TEST_PORT);
    if (!listener) { printf("listen failed\n"); return 1; }

    g_loop = mco_loop_new();
    mco_spawn(g_loop, server_task, listener);
    mco_spawn(g_loop, client_setup_task, NULL);
    mco_loop_run(g_loop);
    mco_loop_free(g_loop);

    mco_close(g_conn);
    mco_socket_destroy(g_conn);
    mco_ssl_ctx_free(g_server_ctx);
    mco_ssl_ctx_free(g_client_ctx);

    printf("owner_set=%d busy_result=%d after_release_result=%d done=%d\n",
           g_owner_set, g_busy_result, g_after_release_result, g_done);

    ok = g_owner_set && (g_busy_result == MCO_ERR_BUSY) && (g_after_release_result == 11) && g_done;
    printf(ok ? "TLS CROSS-OWNER GUARD TEST PASSED\n" : "TLS CROSS-OWNER GUARD TEST FAILED\n");
    return ok ? 0 : 1;
}
