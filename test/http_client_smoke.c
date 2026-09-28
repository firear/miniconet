/* HTTP client end-to-end: a minimal raw-TCP fake server that serves
 * Content-Length, chunked, and POST echo, to validate mco_http_get/post
 * request building, response header parsing and body reading.
 */
#include "../miniconet.h"
#include "../miniconet_http.h"
#include <stdio.h>
#include <string.h>

#define TEST_PORT 17371
#define BIG_BODY_LEN 70000   /* exceeds the 16B inline and 8192 read block: heap + growth */

static mco_loop_t* g_loop = NULL;
static int g_server_done = 0;
static int g_plain_ok = 0, g_chunked_ok = 0, g_post_ok = 0, g_header_ok = 0, g_big_ok = 0;

/* read a request up to "\r\n\r\n" (plus body if Content-Length), return the path */
static int read_one_request(mco_socket_t* conn, char* buf, int cap, char* path, int pathcap) {
    int len = 0;
    char* body_start;
    char* cl_line;
    int content_length = 0;

    for (;;) {
        int r = mco_read(conn, buf + len, cap - len, 3000);
        if (r <= 0) return -1;
        len += r;
        buf[len < cap ? len : cap - 1] = 0;
        if (strstr(buf, "\r\n\r\n")) break;
        if (len >= cap) return -1;
    }

    {
        char* sp1 = strchr(buf, ' ');
        char* sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
        int plen = (sp1 && sp2) ? (int)(sp2 - sp1 - 1) : 0;
        if (plen <= 0 || plen >= pathcap) return -1;
        memcpy(path, sp1 + 1, plen);
        path[plen] = 0;
    }

    cl_line = strstr(buf, "Content-Length:");
    if (cl_line) content_length = atoi(cl_line + strlen("Content-Length:"));

    body_start = strstr(buf, "\r\n\r\n") + 4;
    if (content_length > 0) {
        int have = len - (int)(body_start - buf);
        while (have < content_length) {
            int r = mco_read(conn, buf + len, cap - len, 3000);
            if (r <= 0) return -1;
            len += r;
            have += r;
        }
    }
    return 0;
}

/* big body: a predictable repeating alphabet, byte-checked by the client
 * (covers the heap + growth path when chars_t exceeds its inline cap) */
static void send_big_response(mco_socket_t* conn) {
    char header[128];
    char pattern[4096];
    int i, hlen, sent = 0;

    hlen = snprintf(header, sizeof(header),
                    "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", BIG_BODY_LEN);
    if (mco_write_all(conn, header, hlen, 3000) != hlen) return;
    while (sent < BIG_BODY_LEN) {
        /* byte i of the whole body is 'a'+i%26; must generate across chunks */
        int n = BIG_BODY_LEN - sent < (int)sizeof(pattern) ? BIG_BODY_LEN - sent : (int)sizeof(pattern);
        for (i = 0; i < n; i++) pattern[i] = (char)('a' + ((sent + i) % 26));
        if (mco_write_all(conn, pattern, n, 3000) != n) return;
        sent += n;
    }
}

static void handle_one(mco_socket_t* listener) {
    mco_socket_t* conn = mco_tcp_accept(listener, 5000);
    char reqbuf[4096];
    char path[128];
    const char* resp;

    if (!conn) return;
    if (read_one_request(conn, reqbuf, sizeof(reqbuf), path, sizeof(path)) != 0) {
        mco_close(conn); mco_socket_destroy(conn); return;
    }

    if (strcmp(path, "/plain") == 0) {
        resp = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nX-Test: yes\r\n\r\nhello";
        mco_write(conn, resp, (int)strlen(resp), 3000);
    } else if (strcmp(path, "/chunked") == 0) {
        resp = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
               "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
        mco_write(conn, resp, (int)strlen(resp), 3000);
    } else if (strcmp(path, "/echo") == 0) {
        /* echo the request body as-is (find it after "\r\n\r\n") */
        char* body = strstr(reqbuf, "\r\n\r\n") + 4;
        int body_len = (int)strlen(body); /* printable strings only */
        char header[128];
        int hlen = snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", body_len);
        mco_write(conn, header, hlen, 3000);
        mco_write(conn, body, body_len, 3000);
    } else if (strcmp(path, "/big") == 0) {
        send_big_response(conn);
    } else {
        resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        mco_write(conn, resp, (int)strlen(resp), 3000);
    }

    mco_close(conn);
    mco_socket_destroy(conn);
}

static void server_task(void* arg) {
    mco_socket_t* listener = (mco_socket_t*)arg;
    int i;
    for (i = 0; i < 4; i++) handle_one(listener); /* plain/chunked/echo/big */
    mco_close(listener);
    mco_socket_destroy(listener);
    g_server_done = 1;
}

static void client_task(void* arg) {
    mco_http_response_t* resp;
    (void)arg;
    mco_sleep(50);

    resp = mco_http_get("http://127.0.0.1:17371/plain", 3000);
    if (resp) {
        g_plain_ok = (resp->status == 200 && resp->body.size == 5 &&
                      memcmp(chars_t_cstr(&resp->body), "hello", 5) == 0);
        g_header_ok = (mco_http_response_get_header(resp, "x-test") != NULL &&
                       strcmp(mco_http_response_get_header(resp, "x-test"), "yes") == 0);
        printf("[client] /plain status=%d body='%.*s' header_ok=%d\n",
               resp->status, (int)resp->body.size, chars_t_cstr(&resp->body), g_header_ok);
        mco_http_response_free(resp);
    } else {
        printf("[client] /plain request failed\n");
    }

    resp = mco_http_get("http://127.0.0.1:17371/chunked", 3000);
    if (resp) {
        g_chunked_ok = (resp->status == 200 && resp->body.size == 11 &&
                        memcmp(chars_t_cstr(&resp->body), "hello world", 11) == 0);
        printf("[client] /chunked status=%d body='%.*s' (expect 'hello world')\n",
               resp->status, (int)resp->body.size, chars_t_cstr(&resp->body));
        mco_http_response_free(resp);
    } else {
        printf("[client] /chunked request failed\n");
    }

    resp = mco_http_post("http://127.0.0.1:17371/echo", "post-body-data", 14, "text/plain", 3000);
    if (resp) {
        g_post_ok = (resp->status == 200 && resp->body.size == 14 &&
                     memcmp(chars_t_cstr(&resp->body), "post-body-data", 14) == 0);
        printf("[client] POST /echo status=%d body='%.*s'\n",
               resp->status, (int)resp->body.size, chars_t_cstr(&resp->body));
        mco_http_response_free(resp);
    } else {
        printf("[client] POST /echo request failed\n");
    }

    resp = mco_http_get("http://127.0.0.1:17371/big", 3000);
    if (resp) {
        const char* b = chars_t_cstr(&resp->body);
        int ok = (resp->status == 200 && resp->body.size == BIG_BODY_LEN);
        int i;
        for (i = 0; ok && i < BIG_BODY_LEN; i++) {
            if (b[i] != (char)('a' + (i % 26))) ok = 0;
        }
        if (ok && b[BIG_BODY_LEN] != '\0') ok = 0; /* trailing '\0' */
        g_big_ok = ok;
        printf("[client] /big status=%d body_len=%d content_ok=%d\n",
               resp->status, (int)resp->body.size, ok);
        mco_http_response_free(resp);
    } else {
        printf("[client] /big request failed\n");
    }
}

int main(void) {
    mco_socket_t* listener = mco_tcp_listen("0.0.0.0", TEST_PORT);
    int ok;

    if (!listener) { printf("listen failed\n"); return 1; }

    g_loop = mco_loop_new();
    mco_spawn(g_loop, server_task, listener);
    mco_spawn(g_loop, client_task, NULL);
    mco_loop_run(g_loop);
    mco_loop_free(g_loop);

    printf("plain_ok=%d chunked_ok=%d post_ok=%d header_ok=%d big_ok=%d server_done=%d\n",
           g_plain_ok, g_chunked_ok, g_post_ok, g_header_ok, g_big_ok, g_server_done);

    ok = g_plain_ok && g_chunked_ok && g_post_ok && g_header_ok && g_big_ok && g_server_done;
    printf(ok ? "HTTP CLIENT TEST PASSED\n" : "HTTP CLIENT TEST FAILED\n");
    return ok ? 0 : 1;
}
