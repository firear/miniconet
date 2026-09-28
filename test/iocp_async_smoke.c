/*
 * iocp_async_smoke.c — Windows IOCP completion fast-path smoke (read/write/accept
 * via WSARecv/WSASend/AcceptEx, timeout/close via CancelIoEx + drain).
 * Without MINICONET_WITH_IOCP this degenerates to an empty passing test. When
 * enabled the vcpu is built explicitly with mco_engine_iocp_ops().
 * Scenarios (mirroring uring_async_smoke.c): a) 64KB echo round trip; b) read
 * timeout, and the connection still works after drain; c) mco_close from another
 * coroutine while a read is pending (MCO_ERR_CLOSED); d) mixed connect
 * failure/success with no bad state left behind.
 * Not run on real hardware: this sandbox is Linux; compile-verified only.
 */
#include "../miniconet.h"
#include "../core/mco_engine_backends.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if !MINICONET_WITH_IOCP

int main(void) {
    printf("skipped (built without MINICONET_WITH_IOCP)\n");
    return 0;
}

#else

#define PAYLOAD_LEN (64 * 1024)

static int g_port;
static int g_a_done, g_b_done, g_c_done, g_d_done, g_server_done;
static int g_a_ok, g_b_ok, g_c_ok, g_d_ok;

static mco_socket_t* g_conn;       /* scenario c: conn handed to the closer */
static int g_close_rd_result;      /* scenario c: read result after close */

/* echo server: handles the a/b/c connections, exits when all are done */
static void echo_server(void* arg) {
    mco_socket_t* l = (mco_socket_t*)arg;
    char buf[4096];

    for (;;) {
        mco_socket_t* c = mco_tcp_accept(l, 200);
        if (!c) {
            if (g_a_done && g_b_done && g_c_done && g_d_done) break;
            continue;
        }
        for (;;) {
            int n = mco_read(c, buf, sizeof(buf), 3000);
            if (n <= 0) break;
            if (mco_write(c, buf, n, 3000) != n) break;
        }
        mco_close(c); mco_socket_destroy(c);
    }
    mco_close(l); mco_socket_destroy(l);
    g_server_done = 1;
}

/* a) bulk echo round trip */
static void echo_client(void* arg) {
    static char out[PAYLOAD_LEN];
    static char in[PAYLOAD_LEN];
    int i, off, got;
    mco_socket_t* c;
    (void)arg;

    for (i = 0; i < PAYLOAD_LEN; ++i) out[i] = (char)(i * 31 + 7);

    c = mco_tcp_connect("127.0.0.1", g_port, 3000, NULL);
    if (!c) { g_a_done = 1; return; }

    off = 0;
    while (off < PAYLOAD_LEN) {
        int n = mco_write(c, out + off, PAYLOAD_LEN - off, 3000);
        if (n <= 0) break;
        off += n;
    }
    got = 0;
    while (got < PAYLOAD_LEN) {
        int n = mco_read(c, in + got, PAYLOAD_LEN - got, 3000);
        if (n <= 0) break;
        got += n;
    }
    g_a_ok = (off == PAYLOAD_LEN && got == PAYLOAD_LEN && memcmp(out, in, PAYLOAD_LEN) == 0);
    mco_close(c); mco_socket_destroy(c);
    g_a_done = 1;
}

/* b) connection/coroutine still usable after a read timeout */
static void timeout_client(void* arg) {
    mco_socket_t* c;
    char buf[16];
    (void)arg;

    c = mco_tcp_connect("127.0.0.1", g_port, 3000, NULL);
    if (!c) { g_b_done = 1; return; }

    /* peer is blocked in read and sends nothing: this read must time out */
    if (mco_read(c, buf, sizeof(buf), 120) != MCO_ERR_TIMEOUT) {
        mco_close(c); mco_socket_destroy(c);
        g_b_done = 1;
        return;
    }

    /* after the timeout (cancel + drain) the conn is alive: echo again */
    if (mco_write(c, "ping", 4, 3000) == 4 &&
        mco_read(c, buf, sizeof(buf), 3000) == 4 &&
        memcmp(buf, "ping", 4) == 0)
        g_b_ok = 1;

    mco_close(c); mco_socket_destroy(c);
    g_b_done = 1;
}

/* c) closer: wait for the reader to publish the conn, then close it */
static void closer_task(void* arg) {
    int spins = 0;
    (void)arg;
    while (!g_conn && spins < 500) { mco_sleep_ms(10); spins++; }
    if (g_conn) {
        mco_close(g_conn);  /* wake the reader's pending read (DRAIN + CancelIoEx) */
        g_c_ok = 1;
    }
}

static void reader_task(void* arg) {
    char buf[64];
    (void)arg;

    g_conn = mco_tcp_connect("127.0.0.1", g_port, 3000, NULL);
    if (!g_conn) { g_c_done = 1; return; }

    /* blocking read: woken by the closer's mco_close via mco_task_wake_closed */
    g_close_rd_result = mco_read(g_conn, buf, sizeof(buf), 8000);
    mco_socket_destroy(g_conn); /* close already done by the closer */
    g_conn = NULL;
    g_c_done = 1;
}

/* d) mixed connect failure/success: continue either way — the assertion is that
 * nothing is left in a bad state and the coroutine can still connect. */
static void connect_fail_client(void* arg) {
    mco_socket_t* c = mco_tcp_connect("127.0.0.1", 1, 200, NULL);
    (void)arg;
    if (c) { mco_close(c); mco_socket_destroy(c); }
    c = mco_tcp_connect("127.0.0.1", g_port, 3000, NULL);
    if (c) {
        g_d_ok = 1;
        mco_close(c); mco_socket_destroy(c);
    }
    g_d_done = 1;
}

int main(void) {
    mco_vcpu_t* vcpu;
    mco_socket_t* l;
    int ok;

    g_port = 17560;
    g_a_done = g_b_done = g_c_done = g_d_done = g_server_done = 0;
    g_a_ok = g_b_ok = g_c_ok = g_d_ok = 0;
    g_conn = NULL;
    g_close_rd_result = 0;

    l = mco_tcp_listen("127.0.0.1", g_port);
    if (!l) { printf("listen failed\n"); return 1; }

    vcpu = mco_vcpu_new(mco_engine_iocp_ops());
    if (!vcpu) { printf("[iocp] engine not available\n"); return 0; }

    mco_go(vcpu, echo_server, l);
    mco_go(vcpu, echo_client, NULL);
    mco_go(vcpu, timeout_client, NULL);
    mco_go(vcpu, closer_task, NULL);
    mco_go(vcpu, reader_task, NULL);
    mco_go(vcpu, connect_fail_client, NULL);
    mco_vcpu_run(vcpu);
    mco_vcpu_free(vcpu);

    ok = g_server_done && g_a_done && g_b_done && g_c_done && g_d_done &&
         g_a_ok && g_b_ok && g_c_ok && g_d_ok &&
         g_close_rd_result == MCO_ERR_CLOSED;

    printf("a_echo=%d b_timeout_then_echo=%d c_closed_rd=%d d_connect_fail=%d "
           "(server=%d a=%d b=%d c=%d d=%d) -> %s\n",
           g_a_ok, g_b_ok, g_close_rd_result == MCO_ERR_CLOSED, g_d_ok,
           g_server_done, g_a_done, g_b_done, g_c_done, g_d_done,
           ok ? "OK" : "FAIL");
    printf(ok ? "IOCP ASYNC TEST PASSED\n" : "IOCP ASYNC TEST FAILED\n");
    return ok ? 0 : 1;
}

#endif /* MINICONET_WITH_IOCP */