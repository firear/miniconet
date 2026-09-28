/* Unix domain socket smoke test: mco_tcp_listen_unix/mco_tcp_connect_unix.
 * Needs ENABLE_UDS at build time; skips gracefully when off.
 */
#include "../miniconet.h"
#include <stdio.h>
#include <string.h>

#define UDS_PATH "/tmp/miniconet2_uds_smoke.sock"

static int g_server_done = 0, g_client_done = 0, g_client_ok = 0;

static void server_task(void* arg) {
    mco_socket_t* listener = (mco_socket_t*)arg;
    mco_socket_t* conn = mco_tcp_accept(listener, 5000);
    char buf[64];
    if (conn) {
        int n = mco_read(conn, buf, sizeof(buf) - 1, 3000);
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

    conn = mco_tcp_connect_unix(UDS_PATH, 3000);
    if (!conn) { printf("[client] connect_unix failed\n"); g_client_done = 1; return; }
    printf("[client] connected via UDS, fd=%d\n", mco_socket_fd(conn));

    mco_write(conn, "hello-uds", 9, 3000);
    n = mco_read(conn, buf, sizeof(buf) - 1, 3000);
    if (n > 0) {
        buf[n] = 0;
        g_client_ok = (n == 9 && memcmp(buf, "hello-uds", 9) == 0);
        printf("[client] echo got: '%s'\n", buf);
    }

    mco_close(conn);
    mco_socket_destroy(conn);
    g_client_done = 1;
}

int main(void) {
    mco_loop_t* loop;
    mco_socket_t* listener;
    int ok;

    unlink(UDS_PATH); /* stale socket file from an earlier run */
    listener = mco_tcp_listen_unix(UDS_PATH);
    if (!listener) {
        printf("mco_tcp_listen_unix returned NULL: current build has ENABLE_UDS off "
               "(CMake: -DMINICONET_ENABLE_UDS=ON), skip.\n");
        return 0;
    }

    loop = mco_loop_new();
    mco_spawn(loop, server_task, listener);
    mco_spawn(loop, client_task, NULL);
    mco_loop_run(loop);
    mco_loop_free(loop);
    unlink(UDS_PATH);

    printf("server_done=%d client_done=%d client_ok=%d\n",
           g_server_done, g_client_done, g_client_ok);

    ok = g_server_done && g_client_done && g_client_ok;
    printf(ok ? "UDS ECHO TEST PASSED\n" : "UDS ECHO TEST FAILED\n");
    return ok ? 0 : 1;
}
