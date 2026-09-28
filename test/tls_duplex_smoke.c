/* The ssl_cross_owner guard must not misfire on the normal case: a reader and a
 * writer coroutine share one handshaken TLS connection (no renegotiation) and
 * must work like the plaintext duplex smoke test. Completion counting
 * serializes close, avoiding a double-free.
 */
#include "../miniconet.h"
#include "../compat/ssl/hssl.h"
#include <stdio.h>
#include <string.h>

#define TEST_PORT 17362

static mco_loop_t* g_loop = NULL;
static mco_ssl_ctx_t g_server_ctx = NULL;
static mco_ssl_ctx_t g_client_ctx = NULL;
static int g_writer_ok = 0, g_reader_ok = 0, g_writer_done = 0, g_reader_done = 0;
static int g_conn_finishers = 0;

static void conn_task_finished(mco_socket_t* conn) {
    g_conn_finishers++;
    if (g_conn_finishers == 2) {
        mco_close(conn);
        mco_socket_destroy(conn);
    }
}

static void server_task(void* arg) {
    mco_socket_t* listener = (mco_socket_t*)arg;
    mco_socket_t* conn = mco_tcp_accept(listener, 5000);
    char buf[32];
    if (conn && mco_ssl_server_handshake(conn, g_server_ctx, 3000) == 0) {
        int n = mco_read(conn, buf, sizeof(buf), 3000);
        if (n > 0) mco_write(conn, buf, n, 3000);
        mco_sleep(300);
        mco_close(conn);
        mco_socket_destroy(conn);
    } else if (conn) {
        mco_close(conn);
        mco_socket_destroy(conn);
    }
    mco_close(listener);
    mco_socket_destroy(listener);
}

static void writer_task(void* arg) {
    mco_socket_t* conn = (mco_socket_t*)arg;
    int n;
    mco_sleep(50);
    n = mco_write(conn, "tls-duplex-ok", 13, 3000);
    printf("[writer] wrote %d bytes (expect 13, MCO_ERR_BUSY would be %d)\n", n, MCO_ERR_BUSY);
    g_writer_ok = (n == 13);
    g_writer_done = 1;
    conn_task_finished(conn);
}

static void reader_task(void* arg) {
    mco_socket_t* conn = (mco_socket_t*)arg;
    char buf[32];
    int n = mco_read(conn, buf, sizeof(buf) - 1, 3000);
    if (n > 0) {
        buf[n] = 0;
        printf("[reader] got %d bytes: '%s'\n", n, buf);
        g_reader_ok = (n == 13 && memcmp(buf, "tls-duplex-ok", 13) == 0);
    } else {
        printf("[reader] read failed n=%d (MCO_ERR_BUSY would be %d)\n", n, MCO_ERR_BUSY);
    }
    g_reader_done = 1;
    conn_task_finished(conn);
}

static void connector_task(void* arg) {
    mco_socket_t* conn;
    (void)arg;
    mco_sleep(20);
    conn = mco_tls_connect("127.0.0.1", TEST_PORT, g_client_ctx, 3000, NULL);
    if (!conn) { printf("connect failed\n"); g_reader_done = g_writer_done = 1; return; }
    /* one handshaken TLS connection, one read-only and one write-only coroutine */
    mco_spawn(g_loop, reader_task, conn);
    mco_spawn(g_loop, writer_task, conn);
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
    mco_spawn(g_loop, connector_task, NULL);
    mco_loop_run(g_loop);
    mco_loop_free(g_loop);

    mco_ssl_ctx_free(g_server_ctx);
    mco_ssl_ctx_free(g_client_ctx);

    printf("writer_done=%d writer_ok=%d reader_done=%d reader_ok=%d\n",
           g_writer_done, g_writer_ok, g_reader_done, g_reader_ok);

    ok = g_writer_done && g_writer_ok && g_reader_done && g_reader_ok;
    printf(ok ? "TLS DUPLEX TEST PASSED (read/write still fully independent)\n" : "TLS DUPLEX TEST FAILED\n");
    return ok ? 0 : 1;
}
