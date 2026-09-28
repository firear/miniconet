/*
 * bench_echo.c — TCP echo throughput/latency benchmark (before/after comparison,
 * not an assertion-style regression test). Fixed port 17490.
 *   throughput: MB/s for 32MiB in 64KB round trips
 *   latency:    average us per 1000 1-byte ping-pongs
 * Exit code 0 = all checks passed.
 */
#include "miniconet.h"
#include "htime.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define BENCH_PORT     17490
#define THRPUT_TOTAL   (32 * 1024 * 1024)
#define THRPUT_CHUNK   (64 * 1024)
#define LAT_ROUNDS     1000

static mco_loop_t* g_loop;

static double now_sec(void) {
    uint64_t t = gethrtime_ns();
    return (double)t / 1e9;
}

/* simple write-all loop (bench local) */
static int wr_all(mco_socket_t* s, const char* data, int len, int timeout_ms) {
    int off = 0;
    while (off < len) {
        int n = mco_write(s, data + off, len - off, timeout_ms);
        if (n <= 0) return -1;
        off += n;
    }
    return 0;
}

static void echo_conn(void* arg) {
    mco_socket_t* s = (mco_socket_t*)arg;
    static char buf[THRPUT_CHUNK]; /* 64KB exceeds the small stack budget */
    for (;;) {
        int n = mco_read(s, buf, sizeof(buf), 10000);
        if (n <= 0) break;
        if (wr_all(s, buf, n, 10000) != 0) break;
    }
    mco_close(s);
    mco_socket_destroy(s);
}

static void serve_accept(void* arg) {
    mco_socket_t* l = (mco_socket_t*)arg;
    for (;;) {
        mco_socket_t* c = mco_tcp_accept(l, 500);
        if (!c) continue;
        mco_spawn(g_loop, echo_conn, c);
    }
}

static void client(void* arg) {
    (void)arg;
    {
        mco_socket_t* s = mco_tcp_connect("127.0.0.1", BENCH_PORT, 3000, NULL);
        char* buf;
        long total;
        int i, fail = 0;
        double t0, t1;

        if (!s) { printf("bench_echo: connect failed\n"); exit(1); }
        buf = (char*)malloc(THRPUT_CHUNK);
        memset(buf, 0x5a, THRPUT_CHUNK);

        /* ---- throughput: 64KB round trips ---- */
        t0 = now_sec();
        for (total = 0; total < THRPUT_TOTAL; total += THRPUT_CHUNK) {
            int got = 0;
            if (wr_all(s, buf, THRPUT_CHUNK, 10000) != 0) { fail = 1; break; }
            while (got < THRPUT_CHUNK) {
                int n = mco_read(s, buf + got, THRPUT_CHUNK - got, 10000);
                if (n <= 0) { fail = 1; break; }
                got += n;
            }
            if (fail || (unsigned char)buf[0] != 0x5a ||
                (unsigned char)buf[THRPUT_CHUNK - 1] != 0x5a) { fail = 1; break; }
        }
        t1 = now_sec();
        if (!fail)
            printf("bench_echo throughput: %.1f MB/s (%.2fs, %ld MiB)\n",
                   (total / (1024.0 * 1024.0)) / (t1 - t0), t1 - t0, total >> 20);

        /* ---- latency: 1-byte ping-pong ---- */
        if (!fail) {
            t0 = now_sec();
            for (i = 0; i < LAT_ROUNDS; i++) {
                char c = (char)i;
                if (wr_all(s, &c, 1, 10000) != 0) { fail = 1; break; }
                if (mco_read(s, &c, 1, 10000) != 1) { fail = 1; break; }
            }
            t1 = now_sec();
            if (!fail)
                printf("bench_echo latency: %.2f us/roundtrip (%d rounds)\n",
                       (t1 - t0) * 1e6 / LAT_ROUNDS, LAT_ROUNDS);
        }

        free(buf);
        mco_close(s);
        mco_socket_destroy(s);
        printf("bench_echo: %s\n", fail ? "FAIL" : "PASS");
        exit(fail != 0);
    }
}

int main(void) {
    mco_socket_t* l;

    l = mco_tcp_listen("127.0.0.1", BENCH_PORT);
    if (!l) { printf("bench_echo: listen failed\n"); return 1; }
    g_loop = mco_loop_new();
    mco_spawn(g_loop, serve_accept, l);
    mco_spawn(g_loop, client, NULL);
    mco_loop_run(g_loop);
    mco_loop_free(g_loop);
    return 0;
}
