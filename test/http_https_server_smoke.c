/*
 * http_https_server_smoke.c — HTTPS support smoke test for the HTTP server.
 * Creates a server TLS ctx with mco_ssl_ctx_new_server() and attaches it via
 * mco_http_server_set_tls; verifies that a client using mco_tls_connect() gets
 * 200 + body over plain mco_read/mco_write, and that a plaintext client hitting
 * the HTTPS port is rejected during the handshake.
 * Skips gracefully when no real SSL backend is detected.
 */
#include "miniconet.h"
#include "miniconet_http.h"
#include "miniconet_http_server.h"
#include "../compat/ssl/hssl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PORT 17392

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, msg); g_failures++; } \
    else { fprintf(stderr, "  [ ok ] %s\n", msg); } \
} while (0)

static mco_http_server_t* g_srv = NULL;
static mco_ssl_ctx_t g_server_ctx = NULL;
static mco_ssl_ctx_t g_client_ctx = NULL;

static void on_hi(mco_http_request_t* req, mco_http_response_t* resp, void* ud) {
    (void)req; (void)ud;
    mco_http_resp_text(resp, 200, "hello over https");
}

/* client coroutine: positive case (HTTPS GET) + negative case (plaintext) */
static void client_task(void* arg) {
    int port = *(int*)arg;
    mco_socket_t* conn;
    char buf[4096];
    int n, total = 0;

    /* ---- positive case: TLS client over HTTPS ---- */
    g_client_ctx = mco_ssl_ctx_new_client(NULL, NULL, 0); /* self-signed, no verify */
    conn = mco_tls_connect("127.0.0.1", port, g_client_ctx, 5000, NULL);
    CHECK(conn != NULL, "tls_connect to https server");
    if (conn) {
        mco_write(conn, "GET /hi HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", 55, 3000);
        for (;;) {
            n = mco_read(conn, buf + total, (int)sizeof(buf) - total - 1, 3000);
            if (n <= 0) break;
            total += n;
            if (total >= (int)sizeof(buf) - 1) break;
        }
        buf[total] = 0;
        CHECK(total > 0, "got encrypted response bytes");
        CHECK(strstr(buf, "200 OK") != NULL, "HTTPS response status 200");
        CHECK(strstr(buf, "hello over https") != NULL, "HTTPS response body correct");
        mco_close(conn);
        mco_socket_destroy(conn);
    }
    if (g_client_ctx) { mco_ssl_ctx_free(g_client_ctx); g_client_ctx = NULL; }

    /* ---- negative case: plaintext into the HTTPS port ---- */
    {
        mco_socket_t* c = mco_tcp_connect("127.0.0.1", port, 3000, NULL);
        CHECK(c != NULL, "plain tcp_connect to https port");
        if (c) {
            const char* req = "GET /hi HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
            mco_write(c, req, (int)strlen(req), 1000);
            char rbuf[512];
            int rn = mco_read(c, rbuf, sizeof(rbuf) - 1, 1500);
            CHECK(rn <= 0, "plain client does NOT get HTTP 200 (rejected at handshake)");
            mco_close(c);
            mco_socket_destroy(c);
        }
    }

    mco_http_server_stop(g_srv);
}

static void run_server(void* arg) {
    mco_loop_t* loop = (mco_loop_t*)arg;
    mco_socket_t* listener = mco_tcp_listen("0.0.0.0", TEST_PORT);
    if (!listener) { fprintf(stderr, "listen failed\n"); return; }
    mco_http_server_serve(g_srv, listener, loop);
}

int main(void) {
    mco_loop_t* loop;

    if (!HV_WITH_SSL) {
        printf("current build has no real SSL backend (hssl_backend()=\"%s\"), skip HTTPS test. "
               "install libssl-dev and let CMake/pkg-config detect OpenSSL to enable.\n",
               hssl_backend());
        return 0;
    }

    g_server_ctx = mco_ssl_ctx_new_server("test/certs/server.crt", "test/certs/server.key", 0);
    if (!g_server_ctx) { printf("server ssl ctx create failed\n"); return 1; }

    g_srv = mco_http_server_new();
    mco_http_server_handle(g_srv, "GET", "/hi", on_hi, NULL);
    mco_http_server_set_tls(g_srv, g_server_ctx);

    loop = mco_loop_new();
    mco_spawn(loop, run_server, loop);
    {
        static int tp = TEST_PORT;
        mco_spawn(loop, client_task, &tp);
    }
    mco_loop_run(loop);

    mco_http_server_free(g_srv);
    mco_ssl_ctx_free(g_server_ctx);
    mco_loop_free(loop);

    if (g_failures == 0) {
        printf("\nALL HTTP HTTPS SERVER SMOKE TESTS PASSED\n");
        return 0;
    }
    printf("\n%d HTTP HTTPS SERVER SMOKE TESTS FAILED\n", g_failures);
    return 1;
}