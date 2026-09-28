/* Verify verify_peer really takes effect: same self-signed cert, but the client
 * ctx sets verify_peer=1 with no matching CA, so the handshake must fail.
 */
#include "../miniconet.h"
#include "../compat/ssl/hssl.h"
#include <stdio.h>

#define TEST_PORT 17361

static mco_ssl_ctx_t g_server_ctx = NULL;
static mco_ssl_ctx_t g_client_ctx = NULL;
static int g_server_done = 0, g_client_done = 0;
static int g_handshake_rejected = 0;

static void server_task(void* arg) {
    mco_socket_t* listener = (mco_socket_t*)arg;
    mco_socket_t* conn = mco_tcp_accept(listener, 5000);
    if (conn) {
        int hs = mco_ssl_server_handshake(conn, g_server_ctx, 3000);
        printf("[server] handshake result=%d (client cert verification failed, server side usually sees handshake abort)\n", hs);
        mco_close(conn);
        mco_socket_destroy(conn);
    }
    mco_close(listener);
    mco_socket_destroy(listener);
    g_server_done = 1;
}

static void client_task(void* arg) {
    mco_socket_t* conn;
    (void)arg;
    mco_sleep(50);
    /* connect with a client ctx that has verify_peer=1 */
    conn = mco_tls_connect("127.0.0.1", TEST_PORT, g_client_ctx, 3000, NULL);
    if (!conn) {
        printf("[client] mco_tls_connect rejected (as expected: self-signed cert + verify_peer=1)\n");
        g_handshake_rejected = 1;
    } else {
        printf("[client] !!! handshake unexpectedly succeeded, verify_peer did NOT take effect !!!\n");
        mco_close(conn);
        mco_socket_destroy(conn);
    }
    g_client_done = 1;
}

int main(void) {
    mco_loop_t* loop;
    mco_socket_t* listener;
    int ok;

    if (!HV_WITH_SSL) {
        printf("current build has no real SSL backend (hssl_backend()=\"%s\"), skip TLS test.\n",
               hssl_backend());
        return 0;
    }

    g_server_ctx = mco_ssl_ctx_new_server("test/certs/server.crt", "test/certs/server.key", 0);
    /* verify_peer=1 with no CA: falls back to the system trust store */
    g_client_ctx = mco_ssl_ctx_new_client(NULL, NULL, 1);
    if (!g_server_ctx || !g_client_ctx) { printf("ctx create failed\n"); return 1; }

    listener = mco_tcp_listen("0.0.0.0", TEST_PORT);
    if (!listener) { printf("listen failed\n"); return 1; }

    loop = mco_loop_new();
    mco_spawn(loop, server_task, listener);
    mco_spawn(loop, client_task, NULL);
    mco_loop_run(loop);
    mco_loop_free(loop);

    mco_ssl_ctx_free(g_server_ctx);
    mco_ssl_ctx_free(g_client_ctx);

    printf("server_done=%d client_done=%d handshake_rejected=%d\n",
           g_server_done, g_client_done, g_handshake_rejected);

    ok = g_server_done && g_client_done && g_handshake_rejected;
    printf(ok ? "TLS VERIFY-REJECT TEST PASSED (cert verification works)\n" : "TLS VERIFY-REJECT TEST FAILED\n");
    return ok ? 0 : 1;
}
