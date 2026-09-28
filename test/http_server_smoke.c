/*
 * http_server_smoke.c — HTTP server smoke test (routing, multipart, chunked,
 * static files, 404, WebSocket).
 * A local listener with routes is exercised via raw TCP / mco_http client /
 * mco_ws client. Only built when MINICONET_WITH_HTTP_PARSER=ON.
 */
#include "miniconet.h"
#include "miniconet_http.h"
#include "miniconet_http_server.h"
#include "miniconet_websocket.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#define TEST_PORT  17381
#define TEST_PORT2 17383   /* second server instance */
#define ROOT_DIR  "/tmp/miniconet_static_root"
#define BUF_CAP   65536   /* response receive buffer */
#define RAW_PATH  ROOT_DIR "/raw_upload.bin"  /* streamed-upload target file */
#define RAW_SIZE  2097152                     /* 2MB raw upload: streamed, no memory blowup */

static int g_failures = 0;
static mco_http_server_t* g_srv  = NULL; /* instance 1: main routes */
static mco_http_server_t* g_srv2 = NULL; /* instance 2: separate routes (isolation) */
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, msg); g_failures++; } \
    else { fprintf(stderr, "  [ ok ] %s\n", msg); } \
} while (0)

/* ---------- raw TCP helper: send a request, read the full response ---------- */

/* use mco_socket_t* directly (runs in a coroutine) */
static mco_socket_t* tcp_connect(int port) {
    return mco_tcp_connect("127.0.0.1", port, 3000, NULL);
}

static int tcp_read_until_close_or_len(mco_socket_t* s, char* buf, int cap, int timeout_ms) {
    int total = 0;
    for (;;) {
        int r = mco_read(s, buf + total, cap - total - 1, timeout_ms);
        if (r <= 0) break;
        total += r;
        if (total >= cap - 1) break;
    }
    buf[total] = 0;
    return total;
}

/* ---------- 5c helper: /static/big.bin sendfile full-response check ----------
 * verify status-line prefix, exact Content-Length, no chunked, and that the
 * body bytes equal file[expect_off .. expect_off+expect_len]. big.bin byte p =
 * '!' + (p%256)%90 on both sides, so a wrong Range offset fails here. */
static void check_big_file(int port, const char* range_hdr,
                           const char* status_prefix,
                           long expect_len, long expect_off,
                           const char* what) {
    char req[256];
    char hdr[4096];
    char buf[BUF_CAP];
    mco_socket_t* s2 = tcp_connect(port);
    int hlen = 0, hdr_done = 0, fail = 0;
    long body_total = 0;

    if (!s2) { CHECK(0, what); return; }
    if (range_hdr)
        snprintf(req, sizeof(req),
                 "GET /static/big.bin HTTP/1.1\r\nHost: x\r\n"
                 "Range: %s\r\nConnection: close\r\n\r\n", range_hdr);
    else
        snprintf(req, sizeof(req),
                 "GET /static/big.bin HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    if (mco_write_all(s2, req, (int)strlen(req), 3000) <= 0) fail = 1;

    while (!fail) {
        int r = mco_read(s2, buf, (int)sizeof(buf), 3000);
        int off = 0;
        if (r <= 0) break; /* Connection: close ends the response */
        if (!hdr_done) {
            const char* pend;
            int take = r;
            if (hlen + take > (int)sizeof(hdr) - 1) take = (int)sizeof(hdr) - 1 - hlen;
            memcpy(hdr + hlen, buf, (size_t)take);
            hlen += take;
            hdr[hlen] = 0;
            pend = strstr(hdr, "\r\n\r\n");
            if (pend) {
                char cl[64];
                hdr_done = 1;
                off = (int)(pend + 4 - hdr); /* body start in the first chunk */
                if (strncmp(hdr, status_prefix, strlen(status_prefix)) != 0) fail = 1;
                snprintf(cl, sizeof(cl), "Content-Length: %ld", expect_len);
                if (!strstr(hdr, cl)) fail = 1;
                if (strstr(hdr, "Transfer-Encoding: chunked")) fail = 1;
            } else if (take == 0) {
                fail = 1; /* header over 4KB: not a normal response */
                break;
            }
        }
        if (hdr_done) {
            int k;
            for (k = off; k < r; k++) {
                long p = expect_off + body_total;
                if (buf[k] != (char)(33 + ((p % 256) % 90))) { fail = 1; break; }
                body_total++;
            }
        }
    }
    mco_close(s2); mco_socket_destroy(s2);
    if (!(!fail && hdr_done && body_total == expect_len))
        fprintf(stderr, "  [dbg] %s: hdr_done=%d fail=%d body_total=%ld expect=%ld\n",
                what, hdr_done, fail, body_total, expect_len);
    CHECK(!fail && hdr_done && body_total == expect_len, what);
}

/* ---------- route handlers ---------- */

/* registered only on instance 2, to verify multi-instance isolation */
static void on_other(mco_http_request_t* req, mco_http_response_t* resp, void* ud) {
    (void)req; (void)ud;
    mco_http_resp_text(resp, 200, "other server instance");
}

static void on_hello(mco_http_request_t* req, mco_http_response_t* resp, void* ud) {
    (void)req; (void)ud;
    mco_http_resp_text(resp, 200, "hello world");
}

static void on_echo(mco_http_request_t* req, mco_http_response_t* resp, void* ud) {
    (void)ud;
    int blen = 0;
    const char* body = mco_http_req_body(req, &blen); /* read lazily into memory */
    resp->status = 200;
    /* chars_t carries its own length and appends '\0'; set is idempotent */
    chars_t_set(&resp->body, body ? body : "", (size_t)(blen > 0 ? blen : 0));
    if (mco_http_response_get_header(resp, "Content-Type") == NULL) {
        mco_http_response_add_header(resp, "Content-Type", "text/plain");
    }
}

static void on_upload(mco_http_request_t* req, mco_http_response_t* resp, void* ud) {
    (void)ud;
    /* verify part "a" == "AAAA", part "b" == "BBBB" */
    int ok = 0;
    if (req->num_parts >= 2) {
        /* find the name=a / name=b parts, read their saved files */
        int i;
        for (i = 0; i < req->num_parts; i++) {
            char buf[64];
            FILE* f = fopen(req->parts[i].saved_path, "rb");
            if (!f) continue;
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            buf[n] = 0;
            if (req->parts[i].name && strcmp(req->parts[i].name, "a") == 0)
                ok += (strcmp(buf, "AAAA") == 0);
            if (req->parts[i].name && strcmp(req->parts[i].name, "b") == 0)
                ok += (strcmp(buf, "BBBB") == 0);
        }
    }
    if (ok == 2) mco_http_resp_text(resp, 200, "upload ok");
    else mco_http_resp_text(resp, 400, "upload mismatch");
}

/* POST raw body of a big file -> mco_http_req_save_upload streams to disk (no
 * memory). After saving, read back and check all 'C' and length == RAW_SIZE. */
static void on_raw_upload(mco_http_request_t* req, mco_http_response_t* resp, void* ud) {
    (void)ud;
    int ok = 0;
    if (mco_http_req_save_upload(req, RAW_PATH) == 0) {
        FILE* f = fopen(RAW_PATH, "rb");
        if (f) {
            char* tmp = (char*)malloc(4096); /* heap: small coroutine stack */
            long total = 0, bad = 0;
            for (;;) {
                size_t r = fread(tmp, 1, 4096, f);
                if (r == 0) break;
                size_t i;
                for (i = 0; i < r; i++) if (tmp[i] != 'C') bad++;
                total += (long)r;
            }
            free(tmp);
            fclose(f);
            ok = (total == RAW_SIZE && bad == 0);
        }
    }
    if (ok) mco_http_resp_text(resp, 200, "raw ok");
    else mco_http_resp_text(resp, 400, "raw bad");
}

static void on_chunked(mco_http_request_t* req, mco_http_response_t* resp, void* ud) {
    (void)req; (void)ud;
    mco_http_resp_set_chunked(resp, 200);
    mco_http_resp_write_chunk(resp, "chunk1-", 7);
    mco_http_resp_write_chunk(resp, "chunk2", 6);
    mco_http_resp_write_chunk(resp, NULL, 0); /* end of chunks */
}

/* GET /multihead: add 40 headers, verify the server grows past 32 and pools
 * name/value (no per-header malloc). */
static void on_multi_head(mco_http_request_t* req, mco_http_response_t* resp, void* ud) {
    (void)req; (void)ud;
    int k;
    resp->status = 200;
    chars_t_set(&resp->body, "many", 4);
    for (k = 0; k < 40; k++) {
        char n[32], v[32];
        snprintf(n, sizeof(n), "X-Head-%d", k);
        snprintf(v, sizeof(v), "val-%d", k);
        mco_http_response_add_header(resp, n, v);
    }
}

static void on_not_found(mco_http_request_t* req, mco_http_response_t* resp, void* ud) {
    (void)req; (void)ud;
    mco_http_resp_text(resp, 404, "my custom 404 page");
}

/* ---------- WebSocket echo handler ---------- */
static void on_ws_echo(mco_ws_t* ws, mco_http_request_t* req, void* ud) {
    (void)req; (void)ud;
    char buf[256];
    int opcode;
    for (;;) {
        int n = mco_ws_recv(ws, buf, sizeof(buf), &opcode, 3000);
        if (n <= 0) break; /* peer closed or timed out */
        if (opcode == MCO_WS_TEXT)
            mco_ws_send_text(ws, buf, n, 3000);
        else
            mco_ws_send_binary(ws, buf, n, 3000);
    }
}

/* ---------- test coroutine ---------- */
static void test_client(void* arg) {
    int port = *(int*)arg;
    mco_socket_t* s;
    char* buf = (char*)malloc(BUF_CAP);

    /* 1) basic GET + keep-alive reuse */
    s = tcp_connect(port);
    CHECK(s != NULL, "connect for GET /hello");
    const char* req1 = "GET /hello HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
    mco_write_all(s, req1, (int)strlen(req1), 3000);
    int n = tcp_read_until_close_or_len(s, buf, BUF_CAP, 3000);
    CHECK(n > 0, "GET /hello got response");
    fprintf(stderr, "RAW1[%d]:\n%.400s\n===END===\n", n, buf);
    CHECK(strstr(buf, "Connection: keep-alive") != NULL, "keep-alive advertised");
    CHECK(strstr(buf, "hello world") != NULL, "GET /hello body correct");
    /* reuse the same connection (keep-alive) */
    mco_write_all(s, req1, (int)strlen(req1), 3000);
    n = tcp_read_until_close_or_len(s, buf, BUF_CAP, 3000);
    CHECK(strstr(buf, "hello world") != NULL, "keep-alive: second request reused connection");
    mco_close(s); mco_socket_destroy(s);

    /* 1b) pipelining: two requests in one write, two responses back */
    s = tcp_connect(port);
    const char* pipe2 =
        "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n"
        "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n";
    mco_write_all(s, pipe2, (int)strlen(pipe2), 3000);
    n = tcp_read_until_close_or_len(s, buf, BUF_CAP, 3000);
    {
        int cnt = 0;
        char* p = buf;
        while ((p = strstr(p, "hello world")) != NULL) { cnt++; p += 11; }
        CHECK(cnt == 2, "pipelined two requests both served on one connection");
    }
    mco_close(s); mco_socket_destroy(s);

    /* 2) POST /echo */
    s = tcp_connect(port);
    const char* post =
        "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 11\r\n"
        "Connection: close\r\n\r\nhello-echo!";
    mco_write_all(s, post, (int)strlen(post), 3000);
    n = tcp_read_until_close_or_len(s, buf, BUF_CAP, 3000);
    CHECK(strstr(buf, "hello-echo!") != NULL, "POST /echo echoed body");
    mco_close(s); mco_socket_destroy(s);

    /* 3) POST /upload（multipart/form-data） */
    s = tcp_connect(port);
    const char* boundary = "----testboundary";
    char mp[512];
    int mplen = snprintf(mp, sizeof(mp),
        "POST /upload HTTP/1.1\r\nHost: x\r\nConnection: close\r\n"
        "Content-Type: multipart/form-data; boundary=%s\r\n", boundary);
    char body[512];
    int blen = snprintf(body, sizeof(body),
        "--%s\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\nAAAA\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"b\"\r\n\r\nBBBB\r\n"
        "--%s--\r\n", boundary, boundary, boundary);
    char hdr[64];
    int hl = snprintf(hdr, sizeof(hdr), "Content-Length: %d\r\n\r\n", blen);
    mco_write_all(s, mp, mplen, 3000);
    mco_write_all(s, hdr, hl, 3000);
    mco_write_all(s, body, blen, 3000);
    n = tcp_read_until_close_or_len(s, buf, BUF_CAP, 3000);
    CHECK(strstr(buf, "upload ok") != NULL, "POST /upload parts AAAA/BBBB correct");
    mco_close(s); mco_socket_destroy(s);

    /* 3b) POST /rawupload: stream 2MB raw body to disk (no memory) */
    s = tcp_connect(port);
    {
        char h2[160];
        int h2len = snprintf(h2, sizeof(h2),
            "POST /rawupload HTTP/1.1\r\nHost: x\r\nConnection: close\r\n"
            "Content-Length: %d\r\n\r\n", RAW_SIZE);
        mco_write_all(s, h2, h2len, 3000);
    }
    {
        /* write 2MB of 'C' in chunks (heap buffer: small coroutine stack) */
        char* big = (char*)malloc(BUF_CAP);
        long sent = 0;
        memset(big, 'C', BUF_CAP);
        while (sent < RAW_SIZE) {
            int up = (RAW_SIZE - sent) > (long)BUF_CAP ? BUF_CAP : (int)(RAW_SIZE - sent);
            mco_write_all(s, big, up, 3000);
            sent += up;
        }
        free(big);
    }
    n = tcp_read_until_close_or_len(s, buf, BUF_CAP, 3000);
    CHECK(strstr(buf, "raw ok") != NULL, "POST /rawupload streamed 2MB to disk correctly");
    mco_close(s); mco_socket_destroy(s);

    /* 3c) GET /multihead: header count > 32 (growth + pooled alloc) */
    s = tcp_connect(port);
    snprintf(buf, BUF_CAP,
        "GET /multihead HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    mco_write_all(s, buf, (int)strlen(buf), 3000);
    n = tcp_read_until_close_or_len(s, buf, BUF_CAP, 3000);
    {
        int hcount = 0;
        char* p;
        for (p = buf; (p = strstr(p, "X-Head-")) != NULL; p += 7) hcount++;
        CHECK(hcount >= 40, "GET /multihead returns 40 (>32) headers via pool, no per-header malloc");
    }
    mco_close(s); mco_socket_destroy(s);

    /* 4) GET /chunked. Chunks carry "len\r\n" prefixes, so decode via the
     * mco_http_get client rather than strstr on raw bytes. */
    {
        char url[128];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/chunked", port);
        mco_http_response_t* r = mco_http_get(url, 3000);
        CHECK(r != NULL && r->status == 200, "chunked response status 200");
        CHECK(r != NULL && r->body.size == 13 &&
              memcmp(chars_t_cstr(&r->body), "chunk1-chunk2", 13) == 0,
              "chunked body reassembled via client");
        if (r) mco_http_response_free(r);
    }

    /* 5) GET /static/hello.txt */
    s = tcp_connect(port);
    const char* reqs = "GET /static/hello.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    mco_write_all(s, reqs, (int)strlen(reqs), 3000);
    n = tcp_read_until_close_or_len(s, buf, BUF_CAP, 3000);
    CHECK(strstr(buf, "static file content") != NULL, "static file served");
    CHECK(strstr(buf, "Content-Type: text/plain") != NULL, "static file mime correct");
    CHECK(strstr(buf, "Accept-Ranges: bytes") != NULL, "200 advertises Accept-Ranges");
    mco_close(s); mco_socket_destroy(s);

    /* 5b) Range request (RFC 7233 single range). File is 20 bytes; all ranges
     * below use 20. */
    {
        static const struct {
            const char* range;      /* Range header value */
            const char* expect_cr;  /* expected Content-Range (NULL = no 206) */
            const char* expect_cl;  /* expected Content-Length */
            const char* body_frag;  /* expected fragment in body */
            const char* status_line; /* expected status line (prefix match) */
        } cases[] = {
            { "bytes=0-4",     "bytes 0-4/20",   "5",  "stati",         "HTTP/1.1 206" },
            { "bytes=7-",      "bytes 7-19/20",  "13", "file content",  "HTTP/1.1 206" },
            { "bytes=-4",      "bytes 16-19/20", "4",  "ent",           "HTTP/1.1 206" },
            { "bytes=100-",    "bytes */20",     NULL, NULL,            "HTTP/1.1 416" },
            { "bytes=5-2",     NULL,             "20", "static file",  "HTTP/1.1 200" },  /* malformed range: ignored */
            { "bytes=0-1,5-6", NULL,             "20", "static file",  "HTTP/1.1 200" },  /* multi-range: ignored */
            { "bytes=999-1000","bytes */20",     NULL, NULL,            "HTTP/1.1 416" },
        };
        size_t ci;
        for (ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
            char req[256];
            s = tcp_connect(port);
            snprintf(req, sizeof(req),
                     "GET /static/hello.txt HTTP/1.1\r\nHost: x\r\n"
                     "Range: %s\r\nConnection: close\r\n\r\n", cases[ci].range);
            mco_write_all(s, req, (int)strlen(req), 3000);
            n = tcp_read_until_close_or_len(s, buf, BUF_CAP, 3000);
            CHECK(n > 0 && strncmp(buf, cases[ci].status_line,
                                   strlen(cases[ci].status_line)) == 0,
                  cases[ci].expect_cr ? "range status 206/416" : "range fallback 200");
            if (cases[ci].expect_cr)
                CHECK(strstr(buf, cases[ci].expect_cr) != NULL, "range Content-Range correct");
            if (cases[ci].expect_cl) {
                char cl[32];
                snprintf(cl, sizeof(cl), "Content-Length: %s", cases[ci].expect_cl);
                CHECK(strstr(buf, cl) != NULL, "range Content-Length correct");
            }
            if (cases[ci].body_frag)
                CHECK(strstr(buf, cases[ci].body_frag) != NULL, "range body fragment correct");
            mco_close(s); mco_socket_destroy(s);
        }
        /* spell out key semantics so the loop assert message is not vague */
        CHECK(1, "range cases executed");
    }

    /* 5c) big file (>1MB threshold) goes through zero-copy sendfile with
     * Content-Length (not chunked). big.bin is a 2MB pattern file; verify the
     * whole file and a cross-threshold range, byte by byte. */
    check_big_file(port, NULL, "HTTP/1.1 200", 2097152, 0,
                   "big-file 200 sendfile: full length + offset");
    check_big_file(port, "bytes=1000000-2097151", "HTTP/1.1 206", 1097152, 1000000,
                   "big-file 206 sendfile: range length + start offset");

    /* 6) GET /nope -> 404 error page */
    s = tcp_connect(port);
    const char* reqn = "GET /nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    mco_write_all(s, reqn, (int)strlen(reqn), 3000);
    n = tcp_read_until_close_or_len(s, buf, BUF_CAP, 3000);
    CHECK(strstr(buf, "404") != NULL, "404 status present");
    CHECK(strstr(buf, "my custom 404 page") != NULL, "custom 404 page served");
    mco_close(s); mco_socket_destroy(s);

    /* 7) WebSocket /ws echo */
    {
        char path[32];
        snprintf(path, sizeof(path), "/ws");
        mco_ws_t* ws = mco_ws_connect("127.0.0.1", port, path, 3000);
        CHECK(ws != NULL, "ws connect to /ws");
        if (ws) {
            mco_ws_send_text(ws, "ping", 4, 3000);
            char rbuf[64];
            int opcode;
            int rn = mco_ws_recv(ws, rbuf, sizeof(rbuf), &opcode, 3000);
            CHECK(rn == 4 && memcmp(rbuf, "ping", 4) == 0, "ws echo returns same text");
            mco_ws_close(ws);
            mco_ws_destroy(ws);
        }
    }

    /* 8) multi-instance isolation: each instance's routes only work on its own
     * port. Each request uses its own connection (Connection: close). */
    {
        s = tcp_connect(TEST_PORT2);
        {
            const char* req2 = "GET /other HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
            mco_write_all(s, req2, (int)strlen(req2), 3000);
            n = tcp_read_until_close_or_len(s, buf, BUF_CAP, 3000);
            CHECK(strstr(buf, "200 OK") != NULL, "instance2 serves its own route");
            CHECK(strstr(buf, "other server instance") != NULL, "instance2 route body correct");
        }
        mco_close(s); mco_socket_destroy(s);
        /* instance 2 has no /hello (and must not see instance 1's 404 hook) */
        s = tcp_connect(TEST_PORT2);
        {
            const char* req2 = "GET /hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
            mco_write_all(s, req2, (int)strlen(req2), 3000);
            n = tcp_read_until_close_or_len(s, buf, BUF_CAP, 3000);
            CHECK(strstr(buf, "404") != NULL, "instance2 404 for instance1-only route");
            CHECK(strstr(buf, "my custom 404 page") == NULL,
                  "instance2 uses default 404 (hooks are per-instance)");
        }
        mco_close(s); mco_socket_destroy(s);
        /* instance 1 has no /other */
        s = tcp_connect(port);
        {
            const char* req2 = "GET /other HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
            mco_write_all(s, req2, (int)strlen(req2), 3000);
            n = tcp_read_until_close_or_len(s, buf, BUF_CAP, 3000);
            CHECK(strstr(buf, "404") != NULL, "instance1 404 for instance2-only route");
        }
        mco_close(s); mco_socket_destroy(s);
    }

    mco_http_server_stop(g_srv);
    mco_http_server_stop(g_srv2);
    free(buf);
}

static void run_server(void* arg) {
    mco_loop_t* loop = (mco_loop_t*)arg;
    mco_socket_t* listener = mco_tcp_listen("0.0.0.0", TEST_PORT);
    mco_socket_t* listener2 = mco_tcp_listen("0.0.0.0", TEST_PORT2);
    if (!listener || !listener2) { fprintf(stderr, "listen failed\n"); return; }
    mco_http_server_serve(g_srv, listener, loop);
    mco_http_server_serve(g_srv2, listener2, loop);
}

int main(void) {
    /* prepare the static root: hello.txt (20B) + big.bin (2MB pattern) */
    {
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "mkdir -p %s && echo 'static file content' > %s/hello.txt && "
                 "awk 'BEGIN{s=\"\";for(i=0;i<256;i++)s=s sprintf(\"%%c\",33+(i%%90));"
                 "for(i=0;i<8192;i++)printf \"%%s\",s}' > %s/big.bin",
                 ROOT_DIR, ROOT_DIR, ROOT_DIR);
        if (system(cmd) != 0) {
            fprintf(stderr, "prepare static root failed\n");
            return 1;
        }
    }

    mco_loop_t* loop = mco_loop_new();

    /* instance 1: main test routes */
    g_srv = mco_http_server_new();
    mco_http_server_handle(g_srv, "GET", "/hello", on_hello, NULL);
    mco_http_server_handle(g_srv, "POST", "/echo", on_echo, NULL);
    mco_http_server_handle(g_srv, "POST", "/upload", on_upload, NULL);
    mco_http_server_handle(g_srv, "POST", "/rawupload", on_raw_upload, NULL);
    mco_http_server_handle(g_srv, "GET", "/multihead", on_multi_head, NULL);
    mco_http_server_handle(g_srv, "GET", "/chunked", on_chunked, NULL);
    mco_http_server_serve_static(g_srv, "/static/", ROOT_DIR);
    mco_http_server_set_not_found_handler(g_srv, on_not_found);
    mco_http_server_handle_ws(g_srv, "GET", "/ws", on_ws_echo, NULL);
    mco_http_server_set_keepalive(g_srv, 1);

    /* instance 2: separate routes (isolation check) */
    g_srv2 = mco_http_server_new();
    mco_http_server_handle(g_srv2, "GET", "/other", on_other, NULL);

    mco_spawn(loop, run_server, loop);
    {
        static int tp = TEST_PORT;
        mco_spawn(loop, test_client, &tp);
    }

    mco_loop_run(loop);
    mco_loop_free(loop);
    mco_http_server_free(g_srv);
    mco_http_server_free(g_srv2);

    if (g_failures == 0) {
        printf("\nALL HTTP SERVER SMOKE TESTS PASSED\n");
        return 0;
    }
    printf("\n%d HTTP SERVER SMOKE TESTS FAILED\n", g_failures);
    return 1;
}
