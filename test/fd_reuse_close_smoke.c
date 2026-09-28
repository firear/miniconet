/*
 * fd_reuse_close_smoke.c — fd reuse/close window regression (engine ctl identity).
 * Single vcpu, no intermediate yields so the window is guaranteed:
 *   taskA connects to S, sends 'A', blocks on read (then woken by close, queued);
 *   taskC connects to listener CL (stays in backlog for B to accept);
 *   taskB sleeps until A/C settle, closes A's conn (wakes A, still queued),
 *   accepts(CL) which returns immediately reusing A's fd, then waits for 'Z'.
 * The fix (core/mco_engine_*.c ctl identity): A's post-resume unregister carries
 * its ud and is skipped if the slot changed owner; B's register over a foreign
 * slot forces a kernel resync. Covers epoll/select/poll.
 */
#include "../miniconet.h"
#include "../core/mco_engine_backends.h"
#include <stdio.h>
#include <string.h>

static int g_port_cl;             /* listener CL for B to accept the reused fd */

static mco_socket_t* g_c1;        /* A's connection (A opens, B closes) */
static mco_socket_t* g_cl;        /* client-side listener */
static int   g_fd1;               /* A's connection fd (before close) */
static int   g_fd2;               /* fd B accepted */
static int   g_a_result;          /* A's read result (expect <0 = woken by close) */
static int   g_b_got;             /* B's read byte (expect 'Z') */
static int   g_a_done, g_b_done, g_server_done;

/* server S: hold A's 'A' connection until the client closes */
static void server_task(void* arg) {
    mco_socket_t* listener = (mco_socket_t*)arg;
    char cmd, buf[16];

    for (;;) {
        mco_socket_t* c = mco_tcp_accept(listener, 200);
        if (!c) {
            if (g_a_done && g_b_done) break;
            continue;
        }
        if (mco_read(c, &cmd, 1, 3000) != 1) { mco_close(c); mco_socket_destroy(c); continue; }
        /* hold the connection until the client closes (early close spoils the window) */
        while (mco_read(c, buf, sizeof(buf), 3000) > 0) { /* drain */ }
        mco_close(c); mco_socket_destroy(c);
    }
    mco_close(listener); mco_socket_destroy(listener);
    g_server_done = 1;
}

/* A: connect to S, send 'A', block on read */
static void client_a_task(void* arg) {
    mco_socket_t* c1;
    char buf[8];
    (void)arg;
    /* connect/io must run inside a coroutine */
    c1 = mco_tcp_connect("127.0.0.1", g_port_cl + 1, 3000, NULL);
    if (!c1) { g_fd1 = -1; g_a_done = 1; return; }
    g_c1 = c1;
    g_fd1 = mco_socket_fd(c1);
    mco_write(c1, "A", 1, 3000);
    /* blocked read, woken by B's close via mco_task_wake_closed (queued) */
    g_a_result = mco_read(c1, buf, sizeof(buf), 8000);
    /* c1 was closed by B; A frees the socket struct */
    mco_socket_destroy(c1);
    g_a_done = 1;
}

/* C: connect to CL (stays in backlog), echo 'W' back as 'Z' */
static void echo_peer_task(void* arg) {
    mco_socket_t* c3;
    char buf[8];
    (void)arg;
    c3 = mco_tcp_connect("127.0.0.1", g_port_cl, 3000, NULL);
    if (!c3) return;
    if (mco_read(c3, buf, 1, 5000) == 1 && buf[0] == 'W')
        mco_write(c3, "Z", 1, 3000);
    mco_close(c3); mco_socket_destroy(c3);
}

/* B: close wakes A -> accept the queued conn (fd reuse) -> register */
static void client_b_task(void* arg) {
    mco_socket_t* c1;
    mco_socket_t* c2;
    char buf[8];
    (void)arg;

    mco_sleep_ms(100); /* let A block and C's conn enter backlog */
    c1 = g_c1;
    if (!c1) { g_fd2 = -1; g_b_done = 1; return; }

    /* close wakes A (queued). No yield until B registers: accept returns at
     * once with a pending conn */
    mco_close(c1);
    c2 = mco_tcp_accept(g_cl, 0);
    if (!c2) { g_fd2 = -1; g_b_done = 1; return; }
    g_fd2 = mco_socket_fd(c2);
    mco_write(c2, "W", 1, 3000);
    /* this register lands on A's old slot; pre-fix A's unregister would clear it */
    if (mco_read(c2, buf, 1, 1500) == 1) g_b_got = buf[0];
    mco_close(c2); mco_socket_destroy(c2);
    g_b_done = 1;
}

static int run_one(const char* name, const mco_engine_ops_t* ops) {
    mco_vcpu_t* vcpu;
    mco_socket_t* server_l;
    int ok;
    int reuse_ok;

    g_c1 = NULL; g_cl = NULL; g_fd1 = g_fd2 = -1; g_a_result = 0; g_b_got = 0;
    g_a_done = g_b_done = g_server_done = 0;
    g_port_cl += 2; /* new port per backend, avoid TIME_WAIT */

    server_l = mco_tcp_listen("127.0.0.1", g_port_cl + 1);
    g_cl     = mco_tcp_listen("127.0.0.1", g_port_cl);
    if (!server_l || !g_cl) { printf("[%s] listen failed\n", name); return 0; }

    vcpu = mco_vcpu_new(ops);
    if (!vcpu) { printf("[%s] engine not available\n", name); return 0; }

    mco_go(vcpu, server_task, server_l);
    mco_go(vcpu, client_a_task, NULL);
    mco_go(vcpu, echo_peer_task, NULL);
    mco_go(vcpu, client_b_task, NULL);
    mco_vcpu_run(vcpu);
    mco_vcpu_free(vcpu);
    mco_close(g_cl); mco_socket_destroy(g_cl);

    /* POSIX hands out the lowest free fd, so reuse proves the window formed;
     * Windows SOCKET handles do not guarantee reuse. The real assertion is the
     * last one: B's register was not cleared by A's post-resume unregister. */
#ifdef _WIN32
    reuse_ok = 1;
#else
    reuse_ok = (g_fd2 == g_fd1);
#endif
    ok = g_server_done && g_a_done && g_b_done &&
         g_a_result < 0 &&          /* A woken by close (not EOF/data) */
         reuse_ok &&
         g_b_got == 'Z';            /* B's register survived A's unregister */

    printf("[%s] reuse=%d a_result=%d b_got=%c server=%d a=%d b=%d -> %s\n",
           name, g_fd2 == g_fd1, g_a_result,
           g_b_got >= 32 ? g_b_got : '?', g_server_done, g_a_done, g_b_done,
           ok ? "OK" : "FAIL");
    return ok;
}

int main(void) {
    int ok = 1;
    g_port_cl = 17470;

    ok &= run_one("epoll",  mco_engine_default());
#if !defined(_WIN32)
    ok &= run_one("select", mco_engine_select_ops());
    ok &= run_one("poll",   mco_engine_poll_ops());
#endif
#if MINICONET_WITH_IO_URING && defined(__linux__)
    ok &= run_one("io_uring", mco_engine_uring_ops());
#endif

    printf(ok ? "FD REUSE CLOSE TEST PASSED\n" : "FD REUSE CLOSE TEST FAILED\n");
    return ok ? 0 : 1;
}
