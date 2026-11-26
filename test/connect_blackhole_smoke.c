/*
 * connect_blackhole_smoke.c — a connect to an unroutable address must end with
 * MCO_CONNECT_TIMEOUT inside its budget, every time, never hang. One path only:
 * mco_tcp_connect() with a fixed timeout_ms.
 *
 * 192.0.2.0/24 (TEST-NET-1) is unroutable: no RST, no ICMP, so only the caller's
 * timeout can end the connect. Two ways this broke before: io_uring CONNECT
 * completes at once with -EINPROGRESS for a remote address (taken as a real
 * error, so every remote connect failed in 0ms), and when the op parks instead
 * the timeout waits for the cancellation's CQE (DRAIN), which may never come.
 * Both show up as "not TIMEOUT" or "not within the budget"; a watchdog turns a
 * hang into a failure instead of blocking the suite.
 *
 * Usage: connect_blackhole_smoke [uring|default] [rounds] [host] [port]
 *   "uring" forces mco_engine_uring_ops() and fails if unavailable; the default
 *   tries it when compiled in, else the platform default.
 *   host/port override the unroutable default, e.g. a real address made silent
 *   with iptables -A OUTPUT -d <ip> -p tcp --tcp-flags SYN SYN -j DROP.
 * No route at all (EHOSTUNREACH at once) waits for nothing: reported as skipped.
 */
#include "../miniconet.h"
#include "../core/mco_vcpu.h"
#include "../core/mco_engine_backends.h"
#include "../compat/htime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static const char* g_host = "192.0.2.1";   /* TEST-NET-1: unroutable by default */
static int         g_port = 9;
#define CONNECT_TIMEOUT_MS 1000                        /* fixed on purpose */
#define RETURN_LIMIT_MS    (CONNECT_TIMEOUT_MS + 3000) /* budget + slack */
#define DEFAULT_ROUNDS     10

static int g_rounds = DEFAULT_ROUNDS;
static int g_watchdog_ms;
static int g_done;
static int g_skip;            /* no route: the SYN is never blackholed */
static int g_bad;             /* rounds that were not a timely TIMEOUT */
static int g_min_ms = -1;
static int g_max_ms = -1;

static void connect_task(void* arg) {
    int i;
    (void)arg;

    for (i = 0; i < g_rounds; i++) {
        int cls = MCO_CONNECT_OK;
        int elapsed_ms;
        mco_socket_t* s;
        uint64_t t0 = gethrtime_us();

        s = mco_tcp_connect(g_host, g_port, CONNECT_TIMEOUT_MS, &cls);
        elapsed_ms = (int)((gethrtime_us() - t0) / 1000);
        if (s) { /* nothing should answer on TEST-NET-1 */
            mco_close(s);
            mco_socket_destroy(s);
        }

        if (cls == MCO_CONNECT_UNREACH) {
            printf("  round %d: unreachable at once -- no route, nothing blackholed\n", i + 1);
            g_skip = 1;
            g_done = 1;
            return;
        }
        if (cls != MCO_CONNECT_TIMEOUT || elapsed_ms > RETURN_LIMIT_MS) {
            printf("  round %d: class=%d (want %d) elapsed=%dms (limit %dms) errno=%d%s\n",
                   i + 1, cls, MCO_CONNECT_TIMEOUT, elapsed_ms, RETURN_LIMIT_MS, errno,
                   cls == MCO_CONNECT_TIMEOUT ? "" : "  <-- anomaly");
            g_bad++;
        } else {
            printf("  round %d: TIMEOUT after %dms\n", i + 1, elapsed_ms);
        }
        if (g_min_ms < 0 || elapsed_ms < g_min_ms) g_min_ms = elapsed_ms;
        if (elapsed_ms > g_max_ms) g_max_ms = elapsed_ms;
    }
    g_done = 1;
}

static void watchdog(void* arg) {
    int waited = 0;
    (void)arg;
    while (waited < g_watchdog_ms) {
        mco_sleep_ms(200);
        waited += 200;
        if (g_done) return;
    }
    printf("[FAIL] %d connect(s) to %s:%d did not come back within %dms"
           " (timeout was %dms) -- a coroutine is hung\n",
           g_rounds, g_host, g_port, g_watchdog_ms, CONNECT_TIMEOUT_MS);
    printf("CONNECT BLACKHOLE TEST FAILED (hang)\n");
    exit(1);
}

int main(int argc, char** argv) {
    const char* backend = (argc > 1) ? argv[1] : NULL;
    mco_vcpu_t* vcpu = NULL;
    int ok;

    if (argc > 2) {
        int r = atoi(argv[2]);
        if (r > 0) g_rounds = r;
    }
    if (argc > 3 && *argv[3]) g_host = argv[3];
    if (argc > 4) {
        int p = atoi(argv[4]);
        if (p > 0 && p <= 65535) g_port = p;
    }
    /* Every round is bounded by RETURN_LIMIT_MS; leave room for all of them. */
    g_watchdog_ms = g_rounds * RETURN_LIMIT_MS + 5000;

    if (backend && strcmp(backend, "uring") == 0 && !MINICONET_WITH_IO_URING) {
        printf("io_uring requested but this build has MINICONET_WITH_IO_URING=OFF\n");
        return 1;
    }
#if MINICONET_WITH_IO_URING
    if (!backend || strcmp(backend, "uring") == 0) {
        vcpu = mco_vcpu_new(mco_engine_uring_ops());
        if (vcpu) printf("[engine] io_uring\n");
        else if (backend) { printf("io_uring requested but unavailable\n"); return 1; }
    }
#endif
    if (!vcpu) {
        vcpu = mco_loop_new(); /* platform default */
        printf("[engine] default (epoll/poll/select)\n");
    }
    if (!vcpu) { printf("loop create failed\n"); return 1; }

    printf("[config] %s:%d timeout=%dms rounds=%d\n",
           g_host, g_port, CONNECT_TIMEOUT_MS, g_rounds);

    mco_go(vcpu, connect_task, NULL);
    mco_go(vcpu, watchdog, NULL);
    mco_vcpu_run(vcpu);
    mco_vcpu_free(vcpu);

    if (g_skip) {
        printf("skipped: no route to %s, the SYN was never blackholed\n", g_host);
        return 0;
    }

    ok = g_done && g_bad == 0;
    printf("rounds=%d bad=%d elapsed min=%dms max=%dms (limit %dms) -> %s\n",
           g_rounds, g_bad, g_min_ms, g_max_ms, RETURN_LIMIT_MS, ok ? "OK" : "FAIL");
    printf(ok ? "CONNECT BLACKHOLE TEST PASSED\n" : "CONNECT BLACKHOLE TEST FAILED\n");
    return ok ? 0 : 1;
}
