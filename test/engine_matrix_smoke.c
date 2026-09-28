#include "../miniconet.h"
#include "../core/mco_engine_backends.h"
#include <stdio.h>
#include <string.h>

static int g_port;
static int g_server_done, g_client_done, g_client_ok;

static void server_task(void* arg) {
    mco_socket_t* listener = (mco_socket_t*)arg;
    mco_socket_t* conn;
    char buf[128];
    int n;

    conn = mco_tcp_accept(listener, 5000);
    if (!conn) { g_server_done = 1; return; }
    n = mco_read(conn, buf, sizeof(buf) - 1, 5000);
    if (n > 0) { buf[n] = 0; mco_write(conn, buf, n, 5000); }
    n = mco_read(conn, buf, sizeof(buf) - 1, 5000); /* expect 0 = EOF */
    mco_close(conn); mco_socket_destroy(conn);
    mco_close(listener); mco_socket_destroy(listener);
    g_server_done = 1;
}

static void client_task(void* arg) {
    mco_socket_t* conn;
    char buf[128];
    int n;
    (void)arg;
    mco_sleep(30);
    conn = mco_tcp_connect("127.0.0.1", g_port, 3000, NULL);
    if (!conn) { g_client_done = 1; return; }
    mco_write(conn, "hello-engine-matrix", 19, 3000);
    n = mco_read(conn, buf, sizeof(buf) - 1, 3000);
    if (n > 0) {
        buf[n] = 0;
        g_client_ok = (n == 19 && memcmp(buf, "hello-engine-matrix", 19) == 0);
    }
    mco_close(conn); mco_socket_destroy(conn);
    g_client_done = 1;
}

static int run_one(const char* name, const mco_engine_ops_t* ops) {
    mco_vcpu_t* vcpu;
    mco_socket_t* listener;

    g_server_done = g_client_done = g_client_ok = 0;
    g_port += 1; /* new port per backend, avoid TIME_WAIT */

    listener = mco_tcp_listen("0.0.0.0", g_port);
    if (!listener) { printf("[%s] listen failed\n", name); return 0; }

    vcpu = mco_vcpu_new(ops);
    if (!vcpu) { printf("[%s] engine not available on this platform\n", name); return 0; }

    mco_go(vcpu, server_task, listener);
    mco_go(vcpu, client_task, NULL);
    mco_vcpu_run(vcpu);
    mco_vcpu_free(vcpu);

    printf("[%s] server_done=%d client_done=%d client_ok=%d\n",
           name, g_server_done, g_client_done, g_client_ok);
    return g_server_done && g_client_done && g_client_ok;
}

int main(void) {
    int ok = 1;
    g_port = 17400;

#ifdef _WIN32
    /* Windows ships three backends: select, wepoll, and iocp (when built). */
    ok &= run_one("default", mco_engine_default());
    ok &= run_one("select",  mco_engine_select_ops());
    ok &= run_one("wepoll",  mco_engine_wepoll_ops());
#if MINICONET_WITH_IOCP
    ok &= run_one("iocp",    mco_engine_iocp_ops());
#endif
#else
    ok &= run_one("epoll",  mco_engine_default());
    ok &= run_one("select", mco_engine_select_ops());
    ok &= run_one("poll",   mco_engine_poll_ops());
  #if MINICONET_WITH_IO_URING && defined(__linux__)
    ok &= run_one("io_uring", mco_engine_uring_ops());
  #endif
#endif

    printf(ok ? "ENGINE MATRIX TEST PASSED\n" : "ENGINE MATRIX TEST FAILED\n");
    return ok ? 0 : 1;
}
