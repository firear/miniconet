#include "../miniconet.h"
#include <stdio.h>
#include <string.h>

#define TEST_PORT 17321

static int g_server_done = 0;
static int g_client_done = 0;
static int g_client_ok = 0;

static void server_task(void* arg) {
    mco_socket_t* listener = (mco_socket_t*)arg;
    mco_socket_t* conn;
    char buf[128];
    int n;

    printf("[server] listening, waiting for connection...\n");
    conn = mco_tcp_accept(listener, 5000);
    if (!conn) { printf("[server] accept failed\n"); g_server_done = 1; return; }
    printf("[server] accepted fd=%d\n", mco_socket_fd(conn));

    n = mco_read(conn, buf, sizeof(buf) - 1, 5000);
    if (n <= 0) { printf("[server] read failed n=%d\n", n); }
    else {
        buf[n] = 0;
        printf("[server] got %d bytes: '%s', echoing back\n", n, buf);
        mco_write(conn, buf, n, 5000);
    }

    /* read again, expect EOF (n==0) */
    n = mco_read(conn, buf, sizeof(buf) - 1, 5000);
    printf("[server] second read returned %d (expect 0 = EOF)\n", n);

    mco_close(conn);
    mco_socket_destroy(conn);
    mco_close(listener);
    mco_socket_destroy(listener);
    g_server_done = 1;
}

static void client_task(void* arg) {
    mco_socket_t* conn;
    char buf[128];
    int n, err;
    (void)arg;

    /* cosmetic: let the server park in accept() first */
    mco_sleep(50);

    printf("[client] connecting...\n");
    conn = mco_tcp_connect("127.0.0.1", TEST_PORT, 3000, &err);
    if (!conn) { printf("[client] connect failed\n"); g_client_done = 1; return; }
    printf("[client] connected fd=%d\n", mco_socket_fd(conn));

    n = mco_write(conn, "hello-miniconet2", 16, 3000);
    printf("[client] wrote %d bytes\n", n);

    n = mco_read(conn, buf, sizeof(buf) - 1, 3000);
    if (n > 0) {
        buf[n] = 0;
        printf("[client] echo got %d bytes: '%s'\n", n, buf);
        g_client_ok = (n == 16 && memcmp(buf, "hello-miniconet2", 16) == 0);
    }

    mco_close(conn); /* so the server's second read() sees EOF */
    mco_socket_destroy(conn);
    g_client_done = 1;
}

int main(void) {
    mco_loop_t* loop = mco_loop_new();
    mco_socket_t* listener = mco_tcp_listen("0.0.0.0", TEST_PORT);

    if (!listener) { printf("listen failed\n"); return 1; }

    mco_spawn(loop, server_task, listener);
    mco_spawn(loop, client_task, NULL);

    mco_loop_run(loop);
    mco_loop_free(loop);

    printf("server_done=%d client_done=%d client_ok=%d\n",
           g_server_done, g_client_done, g_client_ok);

    if (g_server_done && g_client_done && g_client_ok) {
        printf("SMOKE TEST PASSED\n");
        return 0;
    }
    printf("SMOKE TEST FAILED\n");
    return 1;
}
