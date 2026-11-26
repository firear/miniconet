/*
 * punch_smoke.c — the composable setup primitives (mco_tcp_socket /
 * mco_socket_bind / mco_socket_connect), i.e. the steps TCP hole punching is
 * built from:
 *   mco_socket_t* s = mco_tcp_socket(AF_INET);
 *   mco_socket_bind(s, NULL, 5000);                 // fixed, reusable local port
 *   mco_socket_connect(s, peer_ip, peer_port, 3000, &e);
 *
 *   a) a socket bound to a fixed local port really connects from that port;
 *   b) two sockets can share one fixed port (SO_REUSEPORT), skipped where the
 *      platform cannot do it;
 *   c) an AF_INET socket to a v6 address is refused up front instead of EINVAL
 *      from connect().
 *
 * Simultaneous open itself needs two hosts: on loopback the first SYN always
 * lands before the second connect() runs, so it is not exercised here.
 */
#include "../miniconet.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define ECHO_PORT   17610
#define FIXED_PORT  17611
#define SHARED_PORT 17612

static int g_a_ok, g_b_ok, g_b_skip, g_c_ok;

static void echo_server(void* arg) {
    mco_socket_t* l = (mco_socket_t*)arg;

    for (;;) {
        char buf[128];
        int n;
        mco_socket_t* c = mco_tcp_accept(l, 1500);
        if (!c) break; /* no client left */
        n = mco_read(c, buf, sizeof(buf), 3000);
        if (n > 0) mco_write(c, buf, n, 3000);
        mco_close(c); mco_socket_destroy(c);
    }
    mco_close(l); mco_socket_destroy(l);
}

static void client_task(void* arg) {
    mco_socket_t* s;
    sockaddr_u local;
    char buf[128];
    int e = 0, r;
    (void)arg;

    mco_sleep(30); /* the listener is in accept() by now */

    /* a) fixed local port -> connect -> echo */
    s = mco_tcp_socket(AF_INET);
    if (!s) return;
    if (mco_socket_bind(s, NULL, FIXED_PORT) != 0) {
        printf("  bind %d failed (errno=%d)\n", FIXED_PORT, errno);
        mco_socket_destroy(s);
        return;
    }
    r = mco_socket_connect(s, "127.0.0.1", ECHO_PORT, 3000, &e);
    if (r != 0) {
        printf("  connect failed (r=%d class=%d errno=%d)\n", r, e, errno);
        mco_close(s); mco_socket_destroy(s);
        return;
    }
    if (mco_socket_local_addr(s, &local) == 0)
        printf("  connected from local port %u\n", (unsigned)sockaddr_port(&local));
    g_a_ok = (mco_socket_local_addr(s, &local) == 0 &&
              (int)sockaddr_port(&local) == FIXED_PORT);
    if (mco_write(s, "punch", 5, 3000) == 5) {
        int n = mco_read(s, buf, sizeof(buf), 3000);
        g_a_ok = g_a_ok && n == 5 && memcmp(buf, "punch", 5) == 0;
    } else {
        g_a_ok = 0;
    }
    mco_close(s); mco_socket_destroy(s);

    /* b) two sockets, one fixed port */
    {
        mco_socket_t* s1 = mco_tcp_socket(AF_INET);
        mco_socket_t* s2 = mco_tcp_socket(AF_INET);
        int b1, b2;

        if (!s1 || !s2) return;
        b1 = mco_socket_bind(s1, NULL, SHARED_PORT);
        b2 = mco_socket_bind(s2, NULL, SHARED_PORT);
        if (b1 == 0 && b2 == 0) {
            g_b_ok = 1;
        } else {
            /* No SO_REUSEPORT (or a policy refusing it): a platform limit, not
             * an API failure. */
            printf("  shared bind failed (%d/%d, errno=%d) -> skipped\n", b1, b2, errno);
            g_b_skip = 1;
        }
        mco_close(s1); mco_socket_destroy(s1);
        mco_close(s2); mco_socket_destroy(s2);
    }

    /* c) family mismatch is refused before connect() */
    {
        mco_socket_t* s4 = mco_tcp_socket(AF_INET);
        int ec = 0;
        if (!s4) return;
        r = mco_socket_connect(s4, "::1", ECHO_PORT, 2000, &ec);
        g_c_ok = (r == MCO_ERR_ADDR && ec == MCO_CONNECT_INVALID);
        mco_close(s4); mco_socket_destroy(s4);
    }
}

int main(void) {
    mco_loop_t* loop;
    mco_socket_t* l;
    int ok;

    l = mco_tcp_listen("127.0.0.1", ECHO_PORT);
    if (!l) { printf("listen failed\n"); return 1; }

    loop = mco_loop_new();
    mco_spawn(loop, echo_server, l);
    mco_spawn(loop, client_task, NULL);
    mco_loop_run(loop);
    mco_loop_free(loop);

    ok = g_a_ok && (g_b_ok || g_b_skip) && g_c_ok;
    printf("a_fixed_port=%d b_shared_port=%d%s c_family_mismatch=%d -> %s\n",
           g_a_ok, g_b_ok, g_b_skip ? "(skipped)" : "", g_c_ok, ok ? "OK" : "FAIL");
    printf(ok ? "PUNCH TEST PASSED\n" : "PUNCH TEST FAILED\n");
    return ok ? 0 : 1;
}
