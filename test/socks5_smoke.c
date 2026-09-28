/* socks5_smoke.c — end-to-end smoke test for examples/socks5_server.c
 *
 * Reuses the example by defining SOCKS5_EXCLUDE_MAIN then including that .c
 * (its main() is wrapped in #ifndef), so test and example cannot drift apart.
 * Test by hand-rolling a mini SOCKS5 client (tens of bytes); no external tool.
 * Covers:
 *   positive  CONNECT(IPv4) / CONNECT(domain) forward, half-close does not truncate
 *   negative  UDP ASSOCIATE -> REP 7, bad ATYP -> REP 8, no listener -> REP 5,
 *            unknown host -> REP 4, unsupported auth -> 0xFF then close
 * BIND is skipped: the example answers BIND and UDP alike with REP 7. */
#define SOCKS5_EXCLUDE_MAIN
#include "../examples/socks5_server.c"

#define PORT_PROXY 17411
#define PORT_ECHO  17412
#define PORT_NONE  17419  /* unlistened port */

static int g_echo_ok = 0;      /* CONNECT(IPv4) forward + no truncation */
static int g_domain_ok = 0;    /* CONNECT(domain) forward */
static int g_udp_rep7 = 0;
static int g_atyp_rep8 = 0;
static int g_refused_rep5 = 0;
static int g_dns_rep4 = 0;
static int g_noauth_ff = 0;    /* unsupported method must reply 0xFF */
static int g_stats_ok = 0;

#define CASE_COUNT 7
static int g_case_done = 0;

/* a finished case lets the accept coroutine shut down (the example uses
 * g_running). Count, don't sleep, so no fixed delay sneaks into the test. */
static void case_finish(void)
{
    g_case_done++;
    if (g_case_done >= CASE_COUNT) g_running = 0;
}

/* Fallback: if a case stalls or returns early (skipping case_finish), force
 * shutdown after 10s instead of letting CTest wait to its 30s timeout. Poll
 * 100ms x100 rather than one sleep(10000) so normal paths don't wait needlessly. */
static void watchdog(void* arg)
{
    int i;
    (void)arg;
    for (i = 0; i < 100; i++) {
        mco_sleep(100);
        if (!g_running) return;   /* cases done, someone is shutting down */
    }
    g_running = 0;
}

/* ---- target server: an echo, to prove data round-trips through the proxy ---- */
static void echo_conn(void* arg)
{
    mco_socket_t* c = (mco_socket_t*)arg;
    char buf[256];

    for (;;) {
        int n = mco_read(c, buf, sizeof(buf), 3000);
        if (n <= 0) break;
        if (mco_write_all(c, buf, n, 3000) != n) break;
    }
    mco_close(c);
    mco_socket_destroy(c);
}

static void echo_server(void* arg)
{
    mco_socket_t* listener = (mco_socket_t*)arg;
    int i;

    /* two client connections go through the proxy, so accept twice */
    for (i = 0; i < 2; i++) {
        mco_socket_t* c = mco_tcp_accept(listener, 5000);
        if (!c) break;
        mco_spawn(mco_vcpu_current(), echo_conn, c);
    }
    mco_close(listener);
    mco_socket_destroy(listener);
}

/* ---- mini SOCKS5 client ---- */

static int s5_greet(mco_socket_t* c, unsigned char method)
{
    unsigned char req[3];
    unsigned char resp[2];

    req[0] = 0x05;
    req[1] = 0x01;
    req[2] = method;
    if (mco_write_all(c, req, 3, 3000) != 3) return -1;
    if (mco_read_exact(c, resp, 2, 3000) != 2) return -1;
    if (resp[0] != 0x05) return -1;
    return resp[1];   /* 0x00 = no-auth chosen, 0xFF = none acceptable */
}

/* send CONNECT and read REP; -1 on failure (no connect / protocol error) */
static int s5_request(mco_socket_t* c, unsigned char cmd, unsigned char atyp,
                      const char* host, int port)
{
    unsigned char buf[4 + 1 + 255 + 2];
    unsigned char hdr[4];
    unsigned char tmp[16];
    int len = 0;
    int addrlen;

    buf[len++] = 0x05;
    buf[len++] = cmd;
    buf[len++] = 0x00;
    buf[len++] = atyp;

    if (atyp == 0x01) {                   /* IPv4 dotted -> 4 bytes */
        unsigned int a, b, d, e;
        if (sscanf(host, "%u.%u.%u.%u", &a, &b, &d, &e) != 4) return -1;
        buf[len++] = (unsigned char)a;
        buf[len++] = (unsigned char)b;
        buf[len++] = (unsigned char)d;
        buf[len++] = (unsigned char)e;
    } else if (atyp == 0x03) {            /* domain: 1 length byte + name */
        size_t n = strlen(host);
        buf[len++] = (unsigned char)n;
        memcpy(buf + len, host, n);
        len += (int)n;
    } else if (atyp == 0x04) {            /* IPv6: unused placeholder */
        return -1;
    } else {
        /* illegal ATYP: send 4 junk bytes; server should answer REP 8 after the header */
        buf[len++] = 127; buf[len++] = 0; buf[len++] = 0; buf[len++] = 1;
    }

    buf[len++] = (unsigned char)((port >> 8) & 0xff);
    buf[len++] = (unsigned char)(port & 0xff);

    if (mco_write_all(c, buf, len, 3000) != len) return -1;

    if (mco_read_exact(c, hdr, 4, 3000) != 4) return -1;
    if (hdr[0] != 0x05) return -1;

    if (hdr[3] == 0x01) addrlen = 4;
    else if (hdr[3] == 0x04) addrlen = 16;
    else if (hdr[3] == 0x03) {
        unsigned char n = 0;
        if (mco_read_exact(c, &n, 1, 3000) != 1) return -1;
        addrlen = (int)n;
    } else {
        return -1;                        /* reply ATYP unknown */
    }
    if (addrlen > (int)sizeof(tmp)) return -1;
    if (mco_read_exact(c, tmp, addrlen, 3000) != addrlen) return -1;
    if (mco_read_exact(c, tmp, 2, 3000) != 2) return -1;  /* BND.PORT */

    return hdr[1];                        /* REP */
}

/* ---- cases ---- */

static void case_echo(void* arg)
{
    mco_socket_t* c;
    char buf[16];
    int rep;
    int n;

    (void)arg;
    c = mco_tcp_connect("127.0.0.1", PORT_PROXY, 3000, NULL);
    if (!c) { printf("[echo] connect proxy failed\n"); return; }

    if (s5_greet(c, 0x00) != 0x00) goto out;
    rep = s5_request(c, 0x01, 0x01, "127.0.0.1", PORT_ECHO);
    if (rep != 0) { printf("[echo] rep=%d (expect 0)\n", rep); goto out; }

    if (mco_write_all(c, "hello", 5, 3000) != 5) goto out;
    n = mco_read_exact(c, buf, 5, 3000);
    if (n != 5 || memcmp(buf, "hello", 5) != 0) {
        printf("[echo] got %d bytes (expect 5 'hello')\n", n);
        goto out;
    }

    /* key semantics: close the write side after the request, the read side must
     * stay alive (peer EOF). The proxy must not kill the whole connection. */
    mco_shutdown(c, MCO_SHUT_WR);
    n = mco_read(c, buf, sizeof(buf), 3000);
    g_echo_ok = (n == 0);
    if (!g_echo_ok) printf("[echo] after half-close read got %d (expect 0)\n", n);

out:
    case_finish();
    mco_close(c);
    mco_socket_destroy(c);
}

static void case_domain(void* arg)
{
    mco_socket_t* c;
    char buf[16];
    int rep;
    int n;

    (void)arg;
    c = mco_tcp_connect("127.0.0.1", PORT_PROXY, 3000, NULL);
    if (!c) { printf("[domain] connect proxy failed\n"); return; }

    if (s5_greet(c, 0x00) != 0x00) goto out;
    /* ATYP=domain: the proxy resolves it (the common socks5h:// usage) */
    rep = s5_request(c, 0x01, 0x03, "localhost", PORT_ECHO);
    if (rep != 0) { printf("[domain] rep=%d (expect 0)\n", rep); goto out; }

    if (mco_write_all(c, "abc", 3, 3000) != 3) goto out;
    n = mco_read_exact(c, buf, 3, 3000);
    g_domain_ok = (n == 3 && memcmp(buf, "abc", 3) == 0);
    if (!g_domain_ok) printf("[domain] got %d bytes (expect 3 'abc')\n", n);

out:
    case_finish();
    mco_close(c);
    mco_socket_destroy(c);
}

static void case_udp_cmd(void* arg)
{
    mco_socket_t* c;
    int rep;

    (void)arg;
    c = mco_tcp_connect("127.0.0.1", PORT_PROXY, 3000, NULL);
    if (!c) return;
    if (s5_greet(c, 0x00) != 0x00) goto out;
    rep = s5_request(c, 0x03, 0x01, "127.0.0.1", PORT_ECHO);
    g_udp_rep7 = (rep == 7);
    if (!g_udp_rep7) printf("[udp] rep=%d (expect 7)\n", rep);
out:
    case_finish();
    mco_close(c);
    mco_socket_destroy(c);
}

static void case_bad_atyp(void* arg)
{
    mco_socket_t* c;
    int rep;

    (void)arg;
    c = mco_tcp_connect("127.0.0.1", PORT_PROXY, 3000, NULL);
    if (!c) return;
    if (s5_greet(c, 0x00) != 0x00) goto out;
    rep = s5_request(c, 0x01, 0x02, "127.0.0.1", PORT_ECHO);  /* ATYP 0x02 undefined */
    g_atyp_rep8 = (rep == 8);
    if (!g_atyp_rep8) printf("[atyp] rep=%d (expect 8)\n", rep);
out:
    case_finish();
    mco_close(c);
    mco_socket_destroy(c);
}

static void case_refused(void* arg)
{
    mco_socket_t* c;
    int rep;

    (void)arg;
    c = mco_tcp_connect("127.0.0.1", PORT_PROXY, 3000, NULL);
    if (!c) return;
    if (s5_greet(c, 0x00) != 0x00) goto out;
    /* no listener -> proxy must report "connection refused" (REP 5), not generic */
    rep = s5_request(c, 0x01, 0x01, "127.0.0.1", PORT_NONE);
    g_refused_rep5 = (rep == 5);
    if (!g_refused_rep5) printf("[refused] rep=%d (expect 5)\n", rep);
out:
    case_finish();
    mco_close(c);
    mco_socket_destroy(c);
}

static void case_dns(void* arg)
{
    mco_socket_t* c;
    int rep;

    (void)arg;
    c = mco_tcp_connect("127.0.0.1", PORT_PROXY, 3000, NULL);
    if (!c) return;
    if (s5_greet(c, 0x00) != 0x00) goto out;
    rep = s5_request(c, 0x01, 0x03, "no-such-host-zzz9.invalid", 80);
    g_dns_rep4 = (rep == 4);
    if (!g_dns_rep4) printf("[dns] rep=%d (expect 4)\n", rep);
out:
    case_finish();
    mco_close(c);
    mco_socket_destroy(c);
}

static void case_no_method(void* arg)
{
    mco_socket_t* c;
    char buf[16];
    int r;
    int n;

    (void)arg;
    c = mco_tcp_connect("127.0.0.1", PORT_PROXY, 3000, NULL);
    if (!c) return;

    /* offer only user/pass: server must answer 0xFF and close */
    r = s5_greet(c, 0x02);
    if (r != 0xFF) { printf("[nomethod] method resp=%d (expect 255)\n", r); goto out; }

    /* after 0xFF it should close: reading again yields EOF (0), not a hang */
    n = mco_read(c, buf, sizeof(buf), 3000);
    g_noauth_ff = (n == 0);
    if (!g_noauth_ff) printf("[nomethod] read after 0xFF got %d (expect 0)\n", n);

out:
    case_finish();
    mco_close(c);
    mco_socket_destroy(c);
}

int main(void)
{
    socks5_config_t cfg;
    mco_loop_t* loop;
    mco_socket_t* echo_listener;
    mco_socket_t* proxy_listener;
    int ok;

    /* target-side echo server */
    echo_listener = mco_tcp_listen("127.0.0.1", PORT_ECHO);
    if (!echo_listener) { printf("echo listen failed\n"); return 1; }

    /* the SOCKS5 server under test: reuse socks5_start() from the example */
    socks5_config_init(&cfg);
    cfg.listen_host = "127.0.0.1";
    cfg.listen_port = PORT_PROXY;
    cfg.connect_timeout_ms = 5000;
    cfg.idle_timeout_ms = 10000;
    cfg.verbose = 0;

    loop = mco_loop_new();
    mco_spawn(loop, echo_server, echo_listener);

    proxy_listener = socks5_start(loop, &cfg);
    if (!proxy_listener) { printf("socks5_start failed\n"); return 1; }

    mco_spawn(loop, case_echo, NULL);
    mco_spawn(loop, case_domain, NULL);
    mco_spawn(loop, case_udp_cmd, NULL);
    mco_spawn(loop, case_bad_atyp, NULL);
    mco_spawn(loop, case_refused, NULL);
    mco_spawn(loop, case_dns, NULL);
    mco_spawn(loop, case_no_method, NULL);
    mco_spawn(loop, watchdog, NULL);

    mco_loop_run(loop);

    /* only case_echo / case_domain actually forwarded, so total is 2; also
     * assert active went to zero (no leaked connections). */
    g_stats_ok = (g_stats.total == 2 && g_stats.active == 0);
    if (!g_stats_ok) printf("[stats] total=%ld active=%ld (expect 2/0)\n",
                            g_stats.total, g_stats.active);

    mco_loop_free(loop);

    printf("echo=%d domain=%d udp_rep7=%d atyp_rep8=%d refused_rep5=%d dns_rep4=%d noauth_ff=%d stats=%d\n",
           g_echo_ok, g_domain_ok, g_udp_rep7, g_atyp_rep8,
           g_refused_rep5, g_dns_rep4, g_noauth_ff, g_stats_ok);

    ok = g_echo_ok && g_domain_ok && g_udp_rep7 && g_atyp_rep8 &&
         g_refused_rep5 && g_dns_rep4 && g_noauth_ff && g_stats_ok;
    printf(ok ? "SOCKS5 TEST PASSED\n" : "SOCKS5 TEST FAILED\n");
    return ok ? 0 : 1;
}
