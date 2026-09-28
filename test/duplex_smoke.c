#include "../miniconet.h"
#include <stdio.h>
#include <string.h>

/* Regression test: on one fd, a coroutine waiting only for READ and another only
 * for WRITE must not clobber each other's engine registration. With a single-slot
 * ctl() the later registration wins and one coroutine times out. */

#define TEST_PORT 17323

static mco_loop_t* g_loop = NULL;
static int g_writer_ok = 0, g_reader_ok = 0, g_writer_done = 0, g_reader_done = 0;
static int g_conn_finishers = 0; /* close+free only once both directions are done */

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
    if (conn) {
        int n = mco_read(conn, buf, sizeof(buf), 3000);
        if (n > 0) mco_write(conn, buf, n, 3000); /* echo back for the reader */
        mco_sleep(300);
        mco_close(conn);
        mco_socket_destroy(conn);
    }
    mco_close(listener);
    mco_socket_destroy(listener);
}

/* Writer: sleep first so the reader parks on mco_wait_io(READ) first. */
static void writer_task(void* arg) {
    mco_socket_t* conn = (mco_socket_t*)arg;
    int n;
    mco_sleep(50);
    n = mco_write(conn, "duplex-write-ok", 15, 3000);
    printf("[writer] wrote %d bytes (expect 15)\n", n);
    g_writer_ok = (n == 15);
    g_writer_done = 1;
    conn_task_finished(conn);
}

/* Reader: starts waiting for read immediately, so the writer then registers a
 * write wait on the same fd — exactly the window under test. */
static void reader_task(void* arg) {
    mco_socket_t* conn = (mco_socket_t*)arg;
    char buf[32];
    int n = mco_read(conn, buf, sizeof(buf) - 1, 3000);
    if (n > 0) {
        buf[n] = 0;
        printf("[reader] got %d bytes: '%s'\n", n, buf);
        g_reader_ok = (n == 15 && memcmp(buf, "duplex-write-ok", 15) == 0);
    } else {
        printf("[reader] read failed n=%d\n", n);
    }
    g_reader_done = 1;
    conn_task_finished(conn);
}

static void connector_task(void* arg) {
    mco_socket_t* conn;
    (void)arg;
    mco_sleep(20);
    conn = mco_tcp_connect("127.0.0.1", TEST_PORT, 3000, NULL);
    if (!conn) { printf("connect failed\n"); g_reader_done = g_writer_done = 1; return; }
    /* same conn, two coroutines: one waits for read, one for write */
    mco_spawn(g_loop, reader_task, conn);
    mco_spawn(g_loop, writer_task, conn);
}

int main(void) {
    mco_socket_t* listener = mco_tcp_listen("0.0.0.0", TEST_PORT);
    if (!listener) { printf("listen failed\n"); return 1; }

    g_loop = mco_loop_new();
    mco_spawn(g_loop, server_task, listener);
    mco_spawn(g_loop, connector_task, NULL);
    mco_loop_run(g_loop);
    mco_loop_free(g_loop);

    printf("writer_done=%d writer_ok=%d reader_done=%d reader_ok=%d\n",
           g_writer_done, g_writer_ok, g_reader_done, g_reader_ok);

    if (g_writer_done && g_writer_ok && g_reader_done && g_reader_ok) {
        printf("DUPLEX TEST PASSED (read-waiter and write-waiter on the same fd coexist)\n");
        return 0;
    }
    printf("DUPLEX TEST FAILED\n");
    return 1;
}
