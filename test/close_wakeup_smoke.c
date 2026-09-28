#include "../miniconet.h"
#include <stdio.h>

#define TEST_PORT 17322

static mco_loop_t* g_loop = NULL;
static int g_reader_woken = 0;
static int g_reader_result = -999;

/* Blocks in mco_read with a very long timeout and must be woken by another
 * coroutine's mco_close(), not run to timeout. */
static void blocked_reader_task(void* arg) {
    mco_socket_t* conn = (mco_socket_t*)arg;
    char buf[16];
    printf("[reader] blocking on read with a 60s timeout...\n");
    g_reader_result = mco_read(conn, buf, sizeof(buf), 60000);
    g_reader_woken = 1;
    printf("[reader] woken up, result=%d (expect %d = MCO_ERR_CLOSED)\n",
           g_reader_result, MCO_ERR_CLOSED);
    mco_socket_destroy(conn);
}

static void closer_task(void* arg) {
    mco_socket_t* conn = (mco_socket_t*)arg;
    mco_sleep(100); /* ensure the reader is parked in mco_wait_io */
    printf("[closer] closing the socket out from under the blocked reader...\n");
    mco_close(conn);
}

static void accept_task(void* arg) {
    mco_socket_t* listener = (mco_socket_t*)arg;
    mco_socket_t* conn = mco_tcp_accept(listener, 5000);
    mco_close(listener);
    mco_socket_destroy(listener);
    if (!conn) { printf("[server] accept failed\n"); return; }
    mco_spawn(g_loop, blocked_reader_task, conn);
    mco_spawn(g_loop, closer_task, conn);
}

static void connector_task(void* arg) {
    mco_socket_t* c;
    (void)arg;
    mco_sleep(20);
    c = mco_tcp_connect("127.0.0.1", TEST_PORT, 3000, NULL);
    if (c) {
        mco_sleep(500);
        mco_close(c);
        mco_socket_destroy(c);
    }
}

int main(void) {
    mco_socket_t* listener = mco_tcp_listen("0.0.0.0", TEST_PORT);
    if (!listener) { printf("listen failed\n"); return 1; }

    g_loop = mco_loop_new();
    mco_spawn(g_loop, accept_task, listener);
    mco_spawn(g_loop, connector_task, NULL);

    mco_loop_run(g_loop);
    mco_loop_free(g_loop);

    printf("reader_woken=%d reader_result=%d\n", g_reader_woken, g_reader_result);
    if (g_reader_woken && g_reader_result == MCO_ERR_CLOSED) {
        printf("CLOSE-WAKEUP TEST PASSED (no coroutine leak)\n");
        return 0;
    }
    printf("CLOSE-WAKEUP TEST FAILED\n");
    return 1;
}
