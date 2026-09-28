/* TLS end-to-end: real handshake + encrypted io with a self-signed cert
 * (test/certs/). Server uses mco_ssl_server_handshake, client mco_tls_connect
 * with verification off (production should set verify_peer with a proper CA).
 * Also checks that io before the handshake fails and that plain mco_read/write
 * carry ciphertext transparently afterwards.
 */
#include "../miniconet.h"
#include "../compat/ssl/hssl.h"
#include <stdio.h>
#include <string.h>

#define TEST_PORT 17360

static mco_ssl_ctx_t g_server_ctx = NULL;
static mco_ssl_ctx_t g_client_ctx = NULL;
static int g_server_done = 0, g_client_done = 0, g_client_ok = 0;
static int g_eof_ok = 0;

static void server_task(void* arg) {
    mco_socket_t* listener = (mco_socket_t*)arg;
    mco_socket_t* conn;
    char buf[128];
    int n, hs;

    conn = mco_tcp_accept(listener, 5000);
    if (!conn) { printf("[server] accept failed\n"); g_server_done = 1; return; }

    hs = mco_ssl_server_handshake(conn, g_server_ctx, 5000);
    printf("[server] handshake result=%d (expect 0)\n", hs);
    if (hs != 0) { mco_close(conn); mco_socket_destroy(conn); g_server_done = 1; return; }

    n = mco_read(conn, buf, sizeof(buf) - 1, 3000); /* plain mco_read, as with TCP */
    if (n > 0) {
        buf[n] = 0;
        printf("[server] decrypted %d bytes: '%s', echoing back\n", n, buf);
        mco_write(conn, buf, n, 3000);
    }

    /* the client closes after the echo; this read must see a clean TLS EOF (0) */
    n = mco_read(conn, buf, sizeof(buf) - 1, 3000);
    printf("[server] second read after client close returned %d (expect 0 = EOF)\n", n);
    g_eof_ok = (n == 0);

    mco_close(conn);
    mco_socket_destroy(conn);
    mco_close(listener);
    mco_socket_destroy(listener);
    g_server_done = 1;
}

static void client_task(void* arg) {
    mco_socket_t* conn;
    char buf[128];
    int n;
    (void)arg;
    mco_sleep(50);

    conn = mco_tls_connect("127.0.0.1", TEST_PORT, g_client_ctx, 5000, NULL);
    if (!conn) { printf("[client] mco_tls_connect failed\n"); g_client_done = 1; return; }
    printf("[client] TLS handshake OK, fd=%d\n", mco_socket_fd(conn));

    n = mco_write(conn, "hello-over-tls", 14, 3000); /* plain mco_write */
    printf("[client] wrote %d bytes (encrypted, packets are ciphertext)\n", n);

    n = mco_read(conn, buf, sizeof(buf) - 1, 3000);
    if (n > 0) {
        buf[n] = 0;
        printf("[client] decrypted echo: '%s'\n", buf);
        g_client_ok = (n == 14 && memcmp(buf, "hello-over-tls", 14) == 0);
    }

    mco_close(conn); /* send close_notify, triggering the server's EOF read */
    mco_socket_destroy(conn);
    g_client_done = 1;
}

int main(void) {
    mco_loop_t* loop;
    mco_socket_t* listener;
    int ok;

    if (!HV_WITH_SSL) {
        printf("current build has no real SSL backend (hssl_backend()=\"%s\"), skip TLS test. "
               "install libssl-dev and let CMake/pkg-config detect OpenSSL to enable.\n",
               hssl_backend());
        return 0;
    }

    g_server_ctx = mco_ssl_ctx_new_server("test/certs/server.crt", "test/certs/server.key", 0);
    if (!g_server_ctx) { printf("server ctx create failed (SSL backend disabled?)\n"); return 1; }
    g_client_ctx = mco_ssl_ctx_new_client(NULL, NULL, 0); /* no verification */

    listener = mco_tcp_listen("0.0.0.0", TEST_PORT);
    if (!listener) { printf("listen failed\n"); return 1; }

    loop = mco_loop_new();
    mco_spawn(loop, server_task, listener);
    mco_spawn(loop, client_task, NULL);
    mco_loop_run(loop);
    mco_loop_free(loop);

    mco_ssl_ctx_free(g_server_ctx);
    mco_ssl_ctx_free(g_client_ctx);

    printf("server_done=%d client_done=%d client_ok=%d eof_ok=%d\n",
           g_server_done, g_client_done, g_client_ok, g_eof_ok);

    ok = g_server_done && g_client_done && g_client_ok && g_eof_ok;
    printf(ok ? "TLS ECHO TEST PASSED\n" : "TLS ECHO TEST FAILED\n");
    return ok ? 0 : 1;
}
