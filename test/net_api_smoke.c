/* net_api_smoke.c — smoke test for the APIs protocol code uses daily:
 *   mco_read_exact / mco_write_all  must absorb short reads/writes
 *   mco_shutdown                    must still drain the response
 *   mco_socket_local_addr/peer_addr address query
 *   mco_socket_set_nodelay
 *   mco_tcp_connect out_errno       failures must be distinguishable
 * TIMEOUT and UNREACH are untested on purpose: they need an address that is
 * unreachable but does not fail fast, which varies by environment. REFUSED, DNS
 * and INVALID are deterministic everywhere. */
#include "../miniconet.h"
#include <stdio.h>
#include <string.h>

#define PORT_EXACT  17401  /* short reads coalesced by mco_read_exact */
#define PORT_HALF   17402  /* full response after half-close */
#define PORT_BIG    17403  /* write_all bulk + address query + nodelay */
#define PORT_NONE   17409  /* unlistened port, for REFUSED */

#define BIG_SIZE 65536

static char g_big[BIG_SIZE];

static int g_exact_ok = 0;    /* short reads coalesced + clean EOF */
static int g_half_ok = 0;     /* response still readable after shutdown(WR) */
static int g_big_ok = 0;      /* write_all delivered all 65536 bytes */
static int g_addr_ok = 0;     /* local/peer address query */
static int g_nodelay_ok = 0;  /* connection still usable after set_nodelay */
static int g_cerrno_ok = 0;   /* out_errno stays OK on success */
static int g_refused_ok = 0;
static int g_dns_ok = 0;
static int g_invalid_ok = 0;

/* ---- part 1: short reads must be coalesced; clean EOF after half-close ---- */
static void exact_server(void* arg) {
    mco_socket_t* listener = (mco_socket_t*)arg;
    mco_socket_t* c = mco_tcp_accept(listener, 5000);

    if (c) {
        char buf[16];
        mco_write(c, "hello", 5, 3000);
        mco_sleep(60);                     /* send in two chunks: force a short read */
        mco_write(c, "world", 5, 3000);
        mco_read(c, buf, sizeof(buf), 3000); /* wait for the EOF from the half-close */
        mco_close(c);
        mco_socket_destroy(c);
    }
    mco_close(listener);
    mco_socket_destroy(listener);
}

static void exact_client(void* arg) {
    mco_socket_t* c;
    char buf[16];
    int n;

    (void)arg;
    c = mco_tcp_connect("127.0.0.1", PORT_EXACT, 3000, NULL);
    if (!c) { printf("[exact] connect failed\n"); return; }

    n = mco_read_exact(c, buf, 10, 3000);
    if (n == 10 && memcmp(buf, "helloworld", 10) == 0) {
        mco_shutdown(c, MCO_SHUT_WR);      /* stop sending, read side must stay usable */
        n = mco_read(c, buf, sizeof(buf), 3000);
        g_exact_ok = (n == 0);             /* peer close -> clean EOF, not an error */
    } else {
        printf("[exact] read_exact got %d (expect 10)\n", n);
    }
    mco_close(c);
    mco_socket_destroy(c);
}

/* ---- part 2: a client that shutdown(WR) after the request must get the
 * response ----
 * Common proxy bug: many HTTP clients close the write side after sending; if the
 * proxy tears down the whole connection, the response is lost. */
static void half_server(void* arg) {
    mco_socket_t* listener = (mco_socket_t*)arg;
    mco_socket_t* c = mco_tcp_accept(listener, 5000);

    if (c) {
        char buf[16];
        int n = mco_read(c, buf, 4, 3000);
        if (n == 4) {
            mco_sleep(30);                 /* reply late, client already closed WR */
            mco_write(c, "pong", 4, 3000);
        }
        mco_close(c);
        mco_socket_destroy(c);
    }
    mco_close(listener);
    mco_socket_destroy(listener);
}

static void half_client(void* arg) {
    mco_socket_t* c;
    char buf[16];
    int n;

    (void)arg;
    c = mco_tcp_connect("127.0.0.1", PORT_HALF, 3000, NULL);
    if (!c) { printf("[half] connect failed\n"); return; }

    if (mco_write_all(c, "ping", 4, 3000) == 4) {
        mco_shutdown(c, MCO_SHUT_WR);
        n = mco_read_exact(c, buf, 4, 3000);
        g_half_ok = (n == 4 && memcmp(buf, "pong", 4) == 0);
        if (!g_half_ok) printf("[half] got %d (expect 4 bytes 'pong')\n", n);
    }
    mco_close(c);
    mco_socket_destroy(c);
}

/* ---- part 3: write_all bulk (forces partial writes) + address + nodelay ---- */
static void big_server(void* arg) {
    mco_socket_t* listener = (mco_socket_t*)arg;
    mco_socket_t* c = mco_tcp_accept(listener, 5000);

    if (c) {
        char buf[8192];
        int total = 0;
        while (total < BIG_SIZE) {
            int n = mco_read(c, buf, sizeof(buf), 5000);
            if (n <= 0) break;
            total += n;
        }
        if (total == BIG_SIZE) mco_write(c, "OK", 2, 3000);
        else printf("[big] server got %d of %d bytes\n", total, BIG_SIZE);
        mco_close(c);
        mco_socket_destroy(c);
    }
    mco_close(listener);
    mco_socket_destroy(listener);
}

static void big_client(void* arg) {
    mco_socket_t* c;
    sockaddr_u la;
    sockaddr_u pa;
    char ip[64];
    char buf[16];
    int port = 0;
    int e = 999;
    int n;

    (void)arg;
    c = mco_tcp_connect("127.0.0.1", PORT_BIG, 3000, &e);
    if (!c) { printf("[big] connect failed\n"); return; }
    g_cerrno_ok = (e == MCO_CONNECT_OK);   /* out_errno must stay OK on success */

    memset(&la, 0, sizeof(la));
    memset(&pa, 0, sizeof(pa));
    memset(ip, 0, sizeof(ip));
    if (mco_socket_local_addr(c, &la) == 0 && mco_socket_peer_addr(c, &pa) == 0) {
        sockaddr_ip(&pa, ip, sizeof(ip));
        port = (int)sockaddr_port(&pa);
        /* peer port must be the one we connected to; local is ephemeral */
        g_addr_ok = (port == PORT_BIG && strcmp(ip, "127.0.0.1") == 0 &&
                     sockaddr_port(&la) != 0);
        if (!g_addr_ok) printf("[addr] peer=%s:%d (expect 127.0.0.1:%d)\n", ip, port, PORT_BIG);
    }

    /* Only check the connection still works; verifying the flag via getsockopt
     * needs platform headers, and this test includes only miniconet.h. */
    mco_socket_set_nodelay(c, 1);

    n = mco_write_all(c, g_big, BIG_SIZE, 8000);
    if (n == BIG_SIZE) {
        int r = mco_read_exact(c, buf, 2, 3000);
        g_big_ok = (r == 2 && memcmp(buf, "OK", 2) == 0);
        g_nodelay_ok = g_big_ok;           /* a full round trip proves it works */
    } else {
        printf("[big] write_all returned %d (expect %d)\n", n, BIG_SIZE);
    }
    mco_close(c);
    mco_socket_destroy(c);
}

/* ---- part 4: connect failure classification ---- */
static void cerrno_task(void* arg) {
    mco_socket_t* c;
    int e = -1;

    (void)arg;

    /* nobody listening -> REFUSED (SOCKS5 maps this to REP 5) */
    /* 5s budget: on some environments an unlistened port takes ~2s to RST, and a
     * 2s budget would misreport REFUSED as TIMEOUT. */
    c = mco_tcp_connect("127.0.0.1", PORT_NONE, 5000, &e);
    if (c) { mco_close(c); mco_socket_destroy(c); }
    g_refused_ok = (!c && e == MCO_CONNECT_REFUSED);
    if (!g_refused_ok) printf("[cerrno] refused case: c=%p e=%d\n", (void*)c, e);

    /* out-of-range port -> INVALID: no socket(), no DNS fallback */
    e = -1;
    c = mco_tcp_connect("127.0.0.1", 70000, 2000, &e);
    if (c) { mco_close(c); mco_socket_destroy(c); }
    g_invalid_ok = (!c && e == MCO_CONNECT_INVALID);
    if (!g_invalid_ok) printf("[cerrno] invalid case: c=%p e=%d\n", (void*)c, e);

    /* nonexistent host -> DNS; .invalid is an RFC2606 reserved TLD */
    e = -1;
    c = mco_tcp_connect("no-such-host-zzz9.invalid", 80, 5000, &e);
    if (c) { mco_close(c); mco_socket_destroy(c); }
    g_dns_ok = (!c && e == MCO_CONNECT_DNS);
    if (!g_dns_ok) printf("[cerrno] dns case: c=%p e=%d\n", (void*)c, e);

    /* out_errno must accept NULL - all legacy call sites do this */
    c = mco_tcp_connect("127.0.0.1", PORT_NONE, 2000, NULL);
    if (c) { mco_close(c); mco_socket_destroy(c); }
}

int main(void) {
    mco_loop_t* loop;
    mco_socket_t* l1;
    mco_socket_t* l2;
    mco_socket_t* l3;
    int ok;
    int i;

    for (i = 0; i < BIG_SIZE; i++) g_big[i] = (char)(i & 0x7f);

    l1 = mco_tcp_listen("127.0.0.1", PORT_EXACT);
    l2 = mco_tcp_listen("127.0.0.1", PORT_HALF);
    l3 = mco_tcp_listen("127.0.0.1", PORT_BIG);
    if (!l1 || !l2 || !l3) { printf("listen failed\n"); return 1; }

    loop = mco_loop_new();
    mco_spawn(loop, exact_server, l1);
    mco_spawn(loop, exact_client, NULL);
    mco_spawn(loop, half_server, l2);
    mco_spawn(loop, half_client, NULL);
    mco_spawn(loop, big_server, l3);
    mco_spawn(loop, big_client, NULL);
    mco_spawn(loop, cerrno_task, NULL);
    mco_loop_run(loop);
    mco_loop_free(loop);

    printf("exact=%d half=%d big=%d addr=%d nodelay=%d\n",
           g_exact_ok, g_half_ok, g_big_ok, g_addr_ok, g_nodelay_ok);
    printf("connect_errno: ok=%d refused=%d dns=%d invalid=%d\n",
           g_cerrno_ok, g_refused_ok, g_dns_ok, g_invalid_ok);

    ok = g_exact_ok && g_half_ok && g_big_ok && g_addr_ok && g_nodelay_ok &&
         g_cerrno_ok && g_refused_ok && g_dns_ok && g_invalid_ok;
    printf(ok ? "NET API TEST PASSED\n" : "NET API TEST FAILED\n");
    return ok ? 0 : 1;
}
