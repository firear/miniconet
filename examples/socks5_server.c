/* socks5_server.c — SOCKS5 proxy (RFC 1928) on miniconet coroutines.
 * Scope: no-auth only (METHOD 0x00, else reply 0xFF and close); CONNECT only
 * (BIND / UDP ASSOCIATE -> REP 7); IPv4 / IPv6 / domain addresses.
 * Demonstrates coroutine protocol code: one coroutine per connection with a
 * straight-line handshake; both directions need two coroutines joined by a
 * waitgroup; EOF only does shutdown(WR) half-close; connect failures map to REP
 * via mco_tcp_connect()'s out_errno.
 * Protocol constants are defined locally on purpose: reusing a vendor SOCKS5
 * header would pull in GPL-licensed code and MSVC-incompatible
 * __attribute__((packed)) structs, and only covers the fixed 4-byte header.
 * Usage:
 *   ./socks5_server [-l 1080] [-b 0.0.0.0] [-t 10000] [-H 10000] [-i 300000] [-c 1024] [-v]
 *   curl -x socks5h://127.0.0.1:1080 http://example.com/
 *
 * Warning: no access control — do not expose it publicly (open proxy).
 */
#include "../miniconet.h"
#include "core/mco_waitgroup.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOCKS5_VER           0x05
#define METHOD_NOAUTH        0x00
#define METHOD_UNACCEPTABLE  0xFF

#define CMD_CONNECT          0x01
#define CMD_BIND             0x02
#define CMD_UDP_ASSOCIATE    0x03

#define ATYP_IPV4            0x01
#define ATYP_DOMAIN          0x03
#define ATYP_IPV6            0x04

#define REP_OK               0x00
#define REP_GENERAL          0x01
#define REP_NET_UNREACH      0x03
#define REP_HOST_UNREACH     0x04
#define REP_CONN_REFUSED     0x05
#define REP_CMD_UNSUPPORTED  0x07
#define REP_ATYP_UNSUPPORTED 0x08

#define PUMP_BUFSIZE         8192
#define ADDR_STRLEN          64

/* ---- config ---- */
typedef struct {
    const char* listen_host;
    int listen_port;
    int handshake_timeout_ms; /* budget for method negotiation + request */
    int connect_timeout_ms;   /* connect to target */
    int idle_timeout_ms;      /* idle timeout while pumping; <=0 disables */
    int max_connections;      /* max active connections; excess rejected */
    int verbose;              /* log one summary line per connection */
} socks5_config_t;

/* ---- runtime stats (single vcpu: no locking needed; with
 * MINICONET_WITH_WORKPOOL these would need atomics or per-thread counters) ---- */
typedef struct {
    long active;
    long total;
    long failed;     /* connections replied with REP != 0 */
    long bytes_c2s;
    long bytes_s2c;
} socks5_stats_t;

/* context of one pump direction: whatever src reads is written to dst */
typedef struct {
    mco_socket_t* src;
    mco_socket_t* dst;
    mco_waitgroup_t* wg;
    int idle_ms;
    long bytes;
} pump_ctx_t;

/* SOCKS5 request: cmd/atyp + host string + port */
typedef struct {
    int cmd;
    int atyp;
    char host[256 + 1]; /* domain <= 255; IPs as dotted/hex strings */
    int port;
} socks5_request_t;

static socks5_config_t g_config;
static socks5_stats_t g_stats;
static volatile sig_atomic_t g_running = 1;

void socks5_config_init(socks5_config_t* cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->listen_host = "0.0.0.0";
    cfg->listen_port = 1080;
    cfg->handshake_timeout_ms = 10000;
    cfg->connect_timeout_ms = 10000;
    cfg->idle_timeout_ms = 300000;
    cfg->max_connections = 1024;
    cfg->verbose = 0;
}

/* ---- method negotiation: no-auth only ----
 * Returns 0 on success; <0 = protocol error or 0xFF already sent: close it. */
static int socks5_handshake(mco_socket_t* c, mco_deadline_t dl)
{
    unsigned char hdr[2];
    unsigned char methods[256];
    unsigned char resp[2];
    int nmethods, i, noauth = 0;

    if (mco_read_exact(c, hdr, 2, mco_deadline_ms(dl)) != 2) return -1;
    if (hdr[0] != SOCKS5_VER) return -1;

    nmethods = hdr[1];
    if (nmethods <= 0) return -1;
    if (mco_read_exact(c, methods, nmethods, mco_deadline_ms(dl)) != nmethods) return -1;

    for (i = 0; i < nmethods; i++) {
        if (methods[i] == METHOD_NOAUTH) noauth = 1;
    }

    if (!noauth) {
        /* no common method: reply 0xFF per RFC 1928, then close */
        resp[0] = SOCKS5_VER;
        resp[1] = METHOD_UNACCEPTABLE;
        mco_write_all(c, resp, 2, mco_deadline_ms(dl));
        return -1;
    }

    resp[0] = SOCKS5_VER;
    resp[1] = METHOD_NOAUTH;
    if (mco_write_all(c, resp, 2, mco_deadline_ms(dl)) != 2) return -1;
    return 0;
}

/* ---- read request. 0 = ok; -1 = io/protocol error (drop);
 * -2 = unsupported address type (must reply REP 8) ---- */
static int socks5_read_request(mco_socket_t* c, socks5_request_t* req, mco_deadline_t dl)
{
    unsigned char hdr[4];
    unsigned char portbuf[2];

    if (mco_read_exact(c, hdr, 4, mco_deadline_ms(dl)) != 4) return -1;
    if (hdr[0] != SOCKS5_VER || hdr[2] != 0) return -1;

    req->cmd = hdr[1];
    req->atyp = hdr[3];
    memset(req->host, 0, sizeof(req->host));

    if (req->atyp == ATYP_IPV4) {
        unsigned char a[4];
        if (mco_read_exact(c, a, 4, mco_deadline_ms(dl)) != 4) return -1;
        sprintf(req->host, "%d.%d.%d.%d", a[0], a[1], a[2], a[3]);
    } else if (req->atyp == ATYP_DOMAIN) {
        unsigned char n = 0;
        if (mco_read_exact(c, &n, 1, mco_deadline_ms(dl)) != 1) return -1;
        if (n == 0) return -1;
        if (mco_read_exact(c, req->host, (int)n, mco_deadline_ms(dl)) != (int)n) return -1;
        req->host[n] = '\0';
    } else if (req->atyp == ATYP_IPV6) {
        unsigned char a[16];
        int i, off = 0;
        if (mco_read_exact(c, a, 16, mco_deadline_ms(dl)) != 16) return -1;
        for (i = 0; i < 8; i++) {
            off += sprintf(req->host + off, "%s%02x%02x",
                           i ? ":" : "", a[i * 2], a[i * 2 + 1]);
        }
    } else {
        return -2;
    }

    if (mco_read_exact(c, portbuf, 2, mco_deadline_ms(dl)) != 2) return -1;
    req->port = (portbuf[0] << 8) | portbuf[1];
    return 0;
}

/* ---- send reply. NULL or unavailable bnd -> ATYP=IPv4 all-zero ---- */
static int socks5_send_reply(mco_socket_t* c, int rep, const sockaddr_u* bnd, int timeout_ms)
{
    unsigned char buf[4 + 16 + 2];
    int len;

    buf[0] = SOCKS5_VER;
    buf[1] = (unsigned char)rep;
    buf[2] = 0;

    if (bnd && bnd->sa.sa_family == AF_INET) {
        unsigned short p = (unsigned short)sockaddr_port((sockaddr_u*)bnd);
        buf[3] = ATYP_IPV4;
        memcpy(buf + 4, &bnd->sin.sin_addr, 4);
        buf[8] = (unsigned char)((p >> 8) & 0xff);
        buf[9] = (unsigned char)(p & 0xff);
        len = 10;
    } else if (bnd && bnd->sa.sa_family == AF_INET6) {
        unsigned short p = (unsigned short)sockaddr_port((sockaddr_u*)bnd);
        buf[3] = ATYP_IPV6;
        memcpy(buf + 4, &bnd->sin6.sin6_addr, 16);
        buf[20] = (unsigned char)((p >> 8) & 0xff);
        buf[21] = (unsigned char)(p & 0xff);
        len = 22;
    } else {
        /* failure path: the client ignores BND when REP != 0, zero-fill it */
        buf[3] = ATYP_IPV4;
        memset(buf + 4, 0, 6);
        len = 10;
    }
    return mco_write_all(c, buf, len, timeout_ms) == len ? 0 : -1;
}

/* ---- connect target: map mco_tcp_connect failures to SOCKS5 REP ----
 * A proxy must report the real upstream reason; a blanket general failure would
 * leave the client unable to tell a bad host from a dead service. */
static mco_socket_t* socks5_connect_target(const socks5_request_t* req, int timeout_ms, int* rep)
{
    int e = MCO_CONNECT_OK;
    mco_socket_t* t = mco_tcp_connect(req->host, req->port, timeout_ms, &e);

    if (t) {
        *rep = REP_OK;
        return t;
    }

    switch (e) {
    case MCO_CONNECT_REFUSED: *rep = REP_CONN_REFUSED; break;
    case MCO_CONNECT_UNREACH: *rep = REP_NET_UNREACH;  break;
    case MCO_CONNECT_DNS:     /* DNS failure: "host unreachable" to the client */
    case MCO_CONNECT_TIMEOUT: *rep = REP_HOST_UNREACH; break;
    default:                  *rep = REP_GENERAL;      break;
    }
    return NULL;
}

/* Graceful close after an error reply: shutdown(WR), drain the unread request
 * bytes, then close. A bare close with unread data triggers RST and discards the
 * reply just written. */
static void socks5_drain_and_close(mco_socket_t* c)
{
    char buf[256];
    mco_shutdown(c, MCO_SHUT_WR);
    while (mco_read(c, buf, sizeof(buf), 200) > 0) { /* drain */ }
    mco_close(c);
    mco_socket_destroy(c);
}

/* ---- one-way pump: src -> dst ----
 * EOF only does shutdown(dst, WR): announce "no more data" while keeping our read
 * side alive so the reverse direction can still deliver the response. */
static void pump_task(void* arg)
{
    pump_ctx_t* ctx = (pump_ctx_t*)arg;
    char buf[PUMP_BUFSIZE];

    for (;;) {
        int n = mco_read(ctx->src, buf, sizeof(buf), ctx->idle_ms);
        if (n == 0) {
            mco_shutdown(ctx->dst, MCO_SHUT_WR);
            break;
        }
        if (n < 0) break;                 /* timeout / error / woken by mco_close */
        if (mco_write_all(ctx->dst, buf, n, ctx->idle_ms) != n) break;
        ctx->bytes += n;
    }

    mco_waitgroup_done(ctx->wg);
}

/* ---- lifetime of one SOCKS5 connection ---- */
static void conn_task(void* arg)
{
    mco_socket_t* client = (mco_socket_t*)arg;
    mco_loop_t* loop = mco_vcpu_current();
    socks5_request_t req;
    sockaddr_u bnd;
    char peer_ip[ADDR_STRLEN];
    mco_socket_t* target = NULL;
    mco_waitgroup_t* wg = NULL;
    pump_ctx_t* up = NULL;
    pump_ctx_t* down = NULL;
    mco_deadline_t dl;
    int idle_ms;
    int rep = REP_GENERAL;
    int counted = 0;
    int rc;

    memset(&req, 0, sizeof(req));
    memset(&bnd, 0, sizeof(bnd));
    strncpy(peer_ip, "?", sizeof(peer_ip) - 1);
    peer_ip[sizeof(peer_ip) - 1] = '\0';
    if (mco_socket_peer_addr(client, &bnd) == 0) {
        sockaddr_ip(&bnd, peer_ip, sizeof(peer_ip));
    }

    /* proxies must disable Nagle: 40ms coalescing hurts interactivity */
    mco_socket_set_nodelay(client, 1);

    /* one budget for negotiation + request: slowloris cannot hog a coroutine */
    dl = mco_deadline_after(g_config.handshake_timeout_ms);

    if (socks5_handshake(client, dl) != 0) goto cleanup;

    rc = socks5_read_request(client, &req, dl);
    if (rc == -2) {
        rep = REP_ATYP_UNSUPPORTED;
        socks5_send_reply(client, rep, NULL, mco_deadline_ms(dl));
        socks5_drain_and_close(client);
        client = NULL; /* already freed; cleanup must not touch it */
        goto cleanup;
    }
    if (rc != 0) goto cleanup;

    if (req.cmd != CMD_CONNECT) {
        rep = REP_CMD_UNSUPPORTED;        /* BIND / UDP ASSOCIATE unsupported */
        socks5_send_reply(client, rep, NULL, mco_deadline_ms(dl));
        goto cleanup;
    }

    target = socks5_connect_target(&req, g_config.connect_timeout_ms, &rep);
    if (!target) {
        socks5_send_reply(client, rep, NULL, mco_deadline_ms(dl));
        goto cleanup;
    }
    mco_socket_set_nodelay(target, 1);

    /* BND.ADDR = local addr of the proxy->target connection */
    memset(&bnd, 0, sizeof(bnd));
    if (mco_socket_local_addr(target, &bnd) == 0) {
        socks5_send_reply(client, REP_OK, &bnd, mco_deadline_ms(dl));
    } else {
        socks5_send_reply(client, REP_OK, NULL, mco_deadline_ms(dl));
    }

    idle_ms = g_config.idle_timeout_ms > 0 ? g_config.idle_timeout_ms : -1;

    wg = mco_waitgroup_new();
    up = (pump_ctx_t*)calloc(1, sizeof(pump_ctx_t));
    down = (pump_ctx_t*)calloc(1, sizeof(pump_ctx_t));
    if (!wg || !up || !down) {
        if (wg) mco_waitgroup_free(wg);
        free(up);
        free(down);
        wg = NULL;
        goto cleanup;
    }

    up->src = client;   up->dst = target;  up->wg = wg;  up->idle_ms = idle_ms;
    down->src = target; down->dst = client; down->wg = wg; down->idle_ms = idle_ms;

    g_stats.active++;
    g_stats.total++;
    counted = 1;

    mco_waitgroup_add(wg, 2);
    mco_spawn(loop, pump_task, up);
    mco_spawn(loop, pump_task, down);

    /* Wait for both directions. -1 is safe: pumps have an idle timeout, and wg may
     * only be freed after wait returns. */
    mco_waitgroup_wait(wg, -1);

    g_stats.bytes_c2s += up->bytes;
    g_stats.bytes_s2c += down->bytes;

    if (g_config.verbose) {
        printf("[socks5] %s -> %s:%d rep=0 up=%ldB down=%ldB\n",
               peer_ip, req.host, req.port, up->bytes, down->bytes);
    }

    mco_waitgroup_free(wg);
    free(up);
    free(down);
    wg = NULL;

cleanup:
    if (rep != REP_OK) g_stats.failed++;
    if (counted) g_stats.active--;
    if (target) { mco_close(target); mco_socket_destroy(target); }
    if (client) { mco_close(client); mco_socket_destroy(client); }
}

/* ---- accept loop; also the exit-flag checkpoint ---- */
static void accept_task(void* arg)
{
    mco_loop_t* loop = mco_vcpu_current();
    mco_socket_t* listener = (mco_socket_t*)arg;

    printf("[socks5] listening on %s:%d (CONNECT only, no auth)\n",
           g_config.listen_host, g_config.listen_port);

    while (g_running) {
        /* a listener does not support concurrent accept, so this is the only accept
         * coroutine; the 1000ms timeout is the g_running poll tick */
        mco_socket_t* c = mco_tcp_accept(listener, 1000);
        if (!c) continue;

        if (g_stats.active >= g_config.max_connections) {
            /* backpressure: reject before negotiation; handshakes cost coroutines */
            mco_close(c);
            mco_socket_destroy(c);
            continue;
        }
        mco_spawn(loop, conn_task, c);
    }

    mco_close(listener);
    mco_socket_destroy(listener);
    printf("[socks5] listener stopped\n");
}

/* Start a SOCKS5 server: copy cfg into g_config, create listener, spawn accept
 * task. Returns the listener (NULL on failure); accept_task owns closing it. */
mco_socket_t* socks5_start(mco_loop_t* loop, const socks5_config_t* cfg)
{
    mco_socket_t* listener;

    if (cfg) g_config = *cfg;
    g_running = 1;

    listener = mco_tcp_listen(g_config.listen_host, g_config.listen_port);
    if (!listener) return NULL;

    mco_spawn(loop, accept_task, listener);
    return listener;
}

#ifndef SOCKS5_EXCLUDE_MAIN
static void on_signal(int sig)
{
    /* flag only: the handler may run in a coroutine context; never touch loop */
    (void)sig;
    g_running = 0;
}

static void show_usage(const char* prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("  -b HOST    Listen host      (default 0.0.0.0)\n");
    printf("  -l PORT    Listen port      (default 1080)\n");
    printf("  -H MS      Handshake budget (default 10000)\n");
    printf("  -t MS      Connect timeout  (default 10000)\n");
    printf("  -i MS      Idle timeout, 0 = none (default 300000)\n");
    printf("  -c N       Max active connections (default 1024)\n");
    printf("  -v         Log one line per connection\n");
    printf("  -?         Show this help\n");
}

int main(int argc, char** argv)
{
    socks5_config_t cfg;
    mco_loop_t* loop;
    mco_socket_t* listener;
    int i;

    socks5_config_init(&cfg);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            cfg.listen_host = argv[++i];
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            cfg.listen_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-H") == 0 && i + 1 < argc) {
            cfg.handshake_timeout_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            cfg.connect_timeout_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            cfg.idle_timeout_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            cfg.max_connections = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-v") == 0) {
            cfg.verbose = 1;
        } else if (strcmp(argv[i], "-?") == 0) {
            show_usage(argv[0]);
            return 0;
        } else {
            printf("unknown option: %s\n", argv[i]);
            show_usage(argv[0]);
            return 1;
        }
    }
    if (cfg.listen_port <= 0 || cfg.listen_port > 65535) {
        printf("invalid listen port: %d\n", cfg.listen_port);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    loop = mco_loop_new();
    if (!loop) { printf("failed to create loop\n"); return 1; }

    listener = socks5_start(loop, &cfg);
    if (!listener) {
        printf("failed to listen on %s:%d\n", cfg.listen_host, cfg.listen_port);
        mco_loop_free(loop);
        return 1;
    }

    /* run until no coroutine is alive: Ctrl+C stops accept, in-flight
     * connections finish */
    mco_loop_run(loop);
    mco_loop_free(loop);

    printf("[socks5] stopped. total=%ld failed=%ld up=%ldB down=%ldB\n",
           g_stats.total, g_stats.failed, g_stats.bytes_c2s, g_stats.bytes_s2c);
    return 0;
}
#endif /* SOCKS5_EXCLUDE_MAIN */
