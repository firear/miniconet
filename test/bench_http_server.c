/*
 * bench_http_server.c — HTTP server benchmark (before/after comparison, not
 * assertion-based regression). Single loop, loopback: 1) keep-alive small QPS
 * (GET /hello x20000); 2) big file GET /big (sendfile) x4; 3) POST upload
 * (8MiB to disk) x1. Temp file in /tmp.
 */
#define _GNU_SOURCE /* strcasestr */
#include "miniconet.h"
#include "miniconet_http_server.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>

#define BENCH_PORT   17491
#define N_REQ        20000
#define BIG_SIZE     (8 * 1024 * 1024)
#define BIG_PATH     "/tmp/miniconet_bench_big.bin"
#define UP_PATH      "/tmp/miniconet_bench_up.bin"

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int wr_all(mco_socket_t* s, const char* data, int len, int timeout_ms) {
    int off = 0;
    while (off < len) {
        int n = mco_write(s, data + off, len - off, timeout_ms);
        if (n <= 0) return -1;
        off += n;
    }
    return 0;
}

typedef struct response_reader {
    mco_socket_t* sock;
    char buf[4096];
    int off;
    int len;
} response_reader_t;

static int reader_read(response_reader_t* r, char* buf, int len, int timeout_ms) {
    int got = 0;
    while (got < len) {
        int avail;
        int take;
        if (r->off == r->len) {
            r->off = 0;
            r->len = mco_read(r->sock, r->buf, (int)sizeof(r->buf), timeout_ms);
            if (r->len <= 0) return got > 0 ? got : -1;
        }
        avail = r->len - r->off;
        take = len - got < avail ? len - got : avail;
        memcpy(buf + got, r->buf + r->off, (size_t)take);
        r->off += take;
        got += take;
    }
    return got;
}

static int reader_read_n(response_reader_t* r, char* buf, int len, int timeout_ms) {
    return reader_read(r, buf, len, timeout_ms) == len ? 0 : -1;
}

/* parse the response header in place (find "\r\n\r\n"), return Content-Length.
 * Requires "HTTP/1.1 200". Header must start at the current consume offset. */
static long read_hdr_clen(response_reader_t* r, int timeout_ms) {
    int base = r->off;
    for (;;) {
        int i, avail = r->len - base;
        int hdr_end = -1;
        for (i = 0; i + 4 <= avail; i++) {
            if (r->buf[base + i] == '\r' &&
                memcmp(r->buf + base + i, "\r\n\r\n", 4) == 0) {
                hdr_end = base + i + 4;
                break;
            }
        }
        if (hdr_end < 0) {
            int n;
            if (r->len == (int)sizeof(r->buf)) {
                /* compact unconsumed bytes to the front, retry */
                if (r->off > 0) {
                    memmove(r->buf, r->buf + r->off,
                            (size_t)(r->len - r->off));
                    base -= r->off;
                    r->len -= r->off;
                    r->off = 0;
                }
                if (r->len == (int)sizeof(r->buf)) return -1; /* header too large */
            }
            n = mco_read(r->sock, r->buf + r->len,
                         (int)sizeof(r->buf) - r->len, timeout_ms);
            if (n <= 0) return -1;
            r->len += n;
            continue;
        }
        {
            const char* h = r->buf + base;
            int hlen = hdr_end - base;
            int pos = 0;
            long clen = -1;
            if (hlen < 13 || memcmp(h, "HTTP/1.1 200", 12) != 0) return -1;
            pos = 12; /* skip "HTTP/1.1 200", find content-length */
            while (pos + 2 <= hlen) {
                int line_end = pos;
                while (line_end + 1 < hlen &&
                       !(h[line_end] == '\r' && h[line_end + 1] == '\n'))
                    line_end++;
                if (line_end + 2 > hlen) break;
                if (line_end - pos > 15 &&
                    strncasecmp(h + pos, "content-length:", 15) == 0) {
                    clen = strtol(h + pos + 15, NULL, 10);
                }
                pos = line_end + 2;
            }
            if (clen < 0) return -1;
            r->off = hdr_end; /* consumed the whole header */
            return clen;
        }
    }
}

/* read the whole response (header+body), return body bytes, -1 on failure */
static long read_response(response_reader_t* r, char* body, int bodycap) {
    long clen = read_hdr_clen(r, 10000);
    if (clen < 0 || clen > bodycap) return -1;
    if (clen > 0 && reader_read_n(r, body, (int)clen, 10000) != 0) return -1;
    return clen;
}

static void hello_handler(mco_http_request_t* req, mco_http_response_t* resp, void* ud) {
    (void)req; (void)ud;
    mco_http_resp_text(resp, 200, "hello");
}

static void big_handler(mco_http_request_t* req, mco_http_response_t* resp, void* ud) {
    (void)req;
    resp->status = 200; /* send_file only sets the file field */
    mco_http_resp_send_file(resp, (const char*)ud); /* zero-copy sendfile */
    mco_http_response_add_header(resp, "Content-Type", "application/octet-stream");
}

static void up_handler(mco_http_request_t* req, mco_http_response_t* resp, void* ud) {
    (void)ud;
    if (mco_http_req_save_upload(req, UP_PATH) != 0) {
        mco_http_resp_text(resp, 500, "save failed");
        return;
    }
    mco_http_resp_text(resp, 200, "ok");
}

static void client_fn(void* arg) {
    mco_socket_t* c;
    char hdr[2048];
    char body[256]; /* small response buffer */
    char reqbuf[256];
    long got;
    int i, fail = 0;
    double t0, t1;
    response_reader_t rr;
    response_reader_t rr2;
    (void)arg;

    c = mco_tcp_connect("127.0.0.1", BENCH_PORT, 3000, NULL);
    if (!c) { printf("bench_http: connect failed\n"); exit(1); }
    memset(&rr, 0, sizeof(rr));
    rr.sock = c;

    /* ---- 1. keep-alive small-response QPS ---- */
    snprintf(reqbuf, sizeof(reqbuf),
             "GET /hello HTTP/1.1\r\nHost: bench\r\nConnection: keep-alive\r\n\r\n");
    if (wr_all(c, reqbuf, (int)strlen(reqbuf), 10000) != 0) fail = 1;
    t0 = now_sec();
    for (i = 0; i < N_REQ && !fail; i++) {
        got = read_response(&rr, body, (int)sizeof(body));
        if (got < 0) { fail = 1; break; }
        if (got != 5 || memcmp(body, "hello", 5) != 0) { fail = 1; break; }
        if (i + 1 < N_REQ && wr_all(c, reqbuf, (int)strlen(reqbuf), 10000) != 0) fail = 1;
    }
    t1 = now_sec();
    if (!fail)
        printf("bench_http keep-alive: %.0f req/s (%d reqs, %.2fs)\n",
               N_REQ / (t1 - t0), N_REQ, t1 - t0);

    /* ---- 2. big file sendfile throughput (fresh conn) ---- */
    if (!fail) {
        static char bigbuf[64 * 1024]; /* static, keeps off the coroutine stack */
        double total = 0;
        mco_socket_t* c2 = mco_tcp_connect("127.0.0.1", BENCH_PORT, 3000, NULL);
        if (!c2) fail = 1;
        memset(&rr2, 0, sizeof(rr2));
        rr2.sock = c2;
        t0 = now_sec();
        for (i = 0; i < 4 && !fail; i++) {
            long clen, total_got = 0;
            snprintf(reqbuf, sizeof(reqbuf),
                     "GET /big HTTP/1.1\r\nHost: bench\r\nConnection: close\r\n\r\n");
            if (wr_all(c2, reqbuf, (int)strlen(reqbuf), 10000) != 0) { fail = 1; break; }
            clen = read_hdr_clen(&rr2, 10000);
            if (clen != BIG_SIZE) { fail = 1; break; }
            while (total_got < clen) {
                int n = reader_read(&rr2, bigbuf, (int)sizeof(bigbuf), 10000);
                if (n <= 0) { fail = 1; break; }
                total_got += n;
            }
            if (fail) break;
            /* need to reconnect after Connection: close; null first to avoid
             * double destroy */
            mco_close(c2); mco_socket_destroy(c2);
            c2 = NULL;
            total += BIG_SIZE;
            if (i < 3) {
                c2 = mco_tcp_connect("127.0.0.1", BENCH_PORT, 3000, NULL);
                if (!c2) { fail = 1; break; }
                memset(&rr2, 0, sizeof(rr2));
                rr2.sock = c2;
            }
        }
        t1 = now_sec();
        if (!fail) {
            printf("bench_http sendfile: %.1f MB/s (%.2fs, %.0f MiB total)\n",
                   total / (1024.0 * 1024.0) / (t1 - t0), t1 - t0,
                   total / (1024.0 * 1024.0));
        }
        if (c2) { mco_close(c2); mco_socket_destroy(c2); }
    }

    /* ---- 3. POST upload throughput ---- */
    if (!fail) {
        static char upbuf[32 * 1024];
        mco_socket_t* c3 = mco_tcp_connect("127.0.0.1", BENCH_PORT, 3000, NULL);
        long remain = BIG_SIZE;
        long expect;
        FILE* f;
        memset(upbuf, 0x77, sizeof(upbuf));
        if (!c3) fail = 1;
        t0 = now_sec();
        if (!fail) {
            int hlen = snprintf(hdr, sizeof(hdr),
                                "POST /up HTTP/1.1\r\nHost: bench\r\nContent-Length: %d\r\n"
                                "Connection: close\r\n\r\n", BIG_SIZE);
            if (wr_all(c3, hdr, hlen, 10000) != 0) fail = 1;
        }
        while (!fail && remain > 0) {
            int n = remain > (long)sizeof(upbuf) ? (int)sizeof(upbuf) : (int)remain;
            if (wr_all(c3, upbuf, n, 10000) != 0) { fail = 1; break; }
            remain -= n;
        }
        if (!fail) {
            /* server replies after reading body; peer closes -> EOF */
            for (;;) {
                int n = mco_read(c3, hdr, (int)sizeof(hdr), 10000);
                if (n < 0) break;
                if (n == 0) break;
            }
        }
        t1 = now_sec();
        if (!fail)
            printf("bench_http upload: %.1f MB/s (%.2fs, %d MiB)\n",
                   BIG_SIZE / (1024.0 * 1024.0) / (t1 - t0), t1 - t0, BIG_SIZE >> 20);
        mco_close(c3); mco_socket_destroy(c3);

        /* verify the uploaded file size */
        expect = BIG_SIZE;
        f = fopen(UP_PATH, "rb");
        if (!f) { fail = 1; }
        else {
            fseek(f, 0, SEEK_END);
            if (ftell(f) != expect) fail = 1;
            fclose(f);
        }
    }

    mco_close(c);
    mco_socket_destroy(c);
    printf("bench_http_server: %s\n", fail ? "FAIL" : "PASS");
    remove(BIG_PATH);
    remove(UP_PATH);
    exit(fail != 0);
}

int main(void) {
    mco_http_server_t* srv;
    mco_socket_t* l;
    mco_loop_t* loop;
    char* big;
    int i;
    FILE* f;

    /* generate the 8MiB file (fixed pattern) */
    big = (char*)malloc(BIG_SIZE);
    if (!big) return 1;
    for (i = 0; i < BIG_SIZE; i++) big[i] = (char)(i * 31 + 7);
    f = fopen(BIG_PATH, "wb");
    if (!f) return 1;
    fwrite(big, 1, BIG_SIZE, f);
    fclose(f);
    free(big);
    remove(UP_PATH);

    srv = mco_http_server_new();
    mco_http_server_handle(srv, "GET", "/hello", hello_handler, NULL);
    mco_http_server_handle(srv, "GET", "/big", big_handler, (void*)BIG_PATH);
    mco_http_server_handle(srv, "POST", "/up", up_handler, NULL);

    l = mco_tcp_listen("127.0.0.1", BENCH_PORT);
    if (!l) { printf("listen failed\n"); return 1; }
    loop = mco_loop_new();
    mco_http_server_serve(srv, l, loop);
    mco_spawn(loop, client_fn, NULL);
    mco_loop_run(loop); /* client calls exit(); does not normally return */

    remove(BIG_PATH);
    remove(UP_PATH);
    mco_http_server_free(srv);
    mco_loop_free(loop);
    return 0;
}
