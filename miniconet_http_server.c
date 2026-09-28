/*
 * miniconet_http_server.c — HTTP server implementation.
 * Body reading uses the same body_sink pattern as miniconet_http.c (a separate
 * copy here, since the client and server live in different static libraries).
 */

#define _GNU_SOURCE  /* strcasestr */
#include "miniconet_http_server.h"
#include "picohttpparser.h"
#include "multipart_parser.h"
#include "miniconet_websocket.h"   /* mco_ws_t / mco_ws_accept */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>                 /* open/O_RDONLY（send_file_response） */
#include <sys/stat.h>              /* stat（mco_http_resp_send_file） */
#include <errno.h>
#include "hsocket.h"               /* tcp_nodelay / tcp_nopush */
#if defined(__linux__)
#include <sys/sendfile.h>          /* sendfile zero-copy */
#endif
#include "mco_vcpu.h"              /* mco_wait_io / MCO_EV_WRITE（send_file_response） */

#define MCO_HTTP_HEADER_BUF_SIZE 8192
#define MCO_HTTP_BODY_CHUNK      (32 * 1024)   /* conn read buffer: one read per 32KB */
/* Stack seed for mco_http_req_body(): bodies up to this size need no malloc.
 * Deliberately smaller than MCO_HTTP_BODY_CHUNK — this buffer lives on the
 * coroutine stack, the 32KB scratch stays a connection-pool allocation. */
#define MCO_HTTP_REQ_BODY_SEED   (4 * 1024)

/* ---- case-insensitive header name compare (public) ---- */
int mco_http_header_name_eq(const char* name, size_t name_len, const char* target) {
    size_t tlen = strlen(target);
    size_t i;
    if (name_len != tlen) return 0;
    for (i = 0; i < tlen; i++) {
        if (tolower((unsigned char)name[i]) != tolower((unsigned char)target[i])) return 0;
    }
    return 1;
}

/* Same test with the literal length supplied by the caller: no strlen in the
 * per-header scan (the public entry point has to strlen(target) every call). */
static int header_name_is(const char* name, size_t name_len,
                          const char* target, size_t target_len) {
    size_t i;
    if (name_len != target_len) return 0;
    for (i = 0; i < target_len; i++) {
        if (tolower((unsigned char)name[i]) != tolower((unsigned char)target[i])) return 0;
    }
    return 1;
}

/* Bounded case-insensitive substring search (strcasestr within vlen). */
static int mem_contains_ci(const char* v, size_t vlen, const char* tok) {
    size_t t = strlen(tok);
    size_t i;
    if (t > vlen) return 0;
    for (i = 0; i + t <= vlen; i++) {
        if (strncasecmp(v + i, tok, t) == 0) return 1;
    }
    return 0;
}

/* ---- extract the boundary from Content-Type ---- */
static int extract_multipart_boundary(const char* ct, size_t ct_len,
                                       char* out, int outcap) {
    const char* p = (const char*)memmem(ct, ct_len, "boundary=", 9);
    size_t blen;
    if (!p) return -1;
    p += 9;
    blen = ct_len - (size_t)(p - ct);
    if (blen > (size_t)outcap - 1) blen = (size_t)outcap - 1;
    /* strip quotes */
    if (blen > 1 && p[0] == '"' && p[blen - 1] == '"') { p++; blen -= 2; }
    /* this parser expects the boundary to include the leading "--" */
    if (blen + 2 > (size_t)outcap - 1) blen = (size_t)outcap - 3;
    memmove(out + 2, p, blen);
    out[0] = '-'; out[1] = '-';
    out[blen + 2] = 0;
    return 0;
}

/* ---- body read: growable buffer + sink ----
 * Seeded on the stack (<=32KB); spills to heap when it outgrows. */
typedef struct {
    char* data;
    int   len;
    int   cap;
    int   on_stack;  /* data on the stack; cleared once moved to heap */
} growbuf_t;

static void growbuf_init_stack(growbuf_t* b, void* stackbuf, int cap) {
    b->data = (char*)stackbuf;
    b->len = 0;
    b->cap = cap;
    b->on_stack = 1;
}

static int growbuf_ensure(growbuf_t* b, int extra) {
    if (b->len + extra > b->cap) {
        int newcap = b->cap > 0 ? b->cap : MCO_HTTP_BODY_CHUNK;
        char* nd;
        while (newcap < b->len + extra) newcap *= 2;
        /* cannot realloc: data may point at the stack */
        nd = (char*)malloc((size_t)newcap);
        if (!nd) return -1;
        if (b->data && b->len > 0) memcpy(nd, b->data, (size_t)b->len);
        b->data = nd;
        b->cap = newcap;
        b->on_stack = 0;
    }
    return 0;
}

/* frees the heap block, if any */
static void growbuf_release(growbuf_t* b) {
    if (b->data && !b->on_stack) free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->on_stack = 0;
}

typedef struct {
    int (*write)(void* ctx, const char* data, int len);
    void* ctx;
} body_sink_t;

static int growbuf_sink_write(void* ctx, const char* data, int len) {
    growbuf_t* b = (growbuf_t*)ctx;
    if (growbuf_ensure(b, len) != 0) return -1;
    memcpy(b->data + b->len, data, (size_t)len);
    b->len += len;
    return 0;
}

/* ---- Per-connection arena allocator ----
 * One big block per connection; requests bump-allocate from it and the cursor
 * rewinds at request end, avoiding per-request malloc/free. Pool strings are not
 * freed individually. */
#define MCO_HTTP_POOL_CAP (64 * 1024)
typedef struct {
    char*  base;
    size_t cap;
    size_t off;   /* allocation cursor */
} mco_http_pool_t;

static void mco_pool_init(mco_http_pool_t* p, char* base, size_t cap) {
    p->base = base; p->cap = cap; p->off = 0;
}
/* record the cursor; pool_reset rewinds to it */
static size_t mco_pool_mark(mco_http_pool_t* p) { return p->off; }
static void mco_pool_reset(mco_http_pool_t* p, size_t mark) { p->off = mark; }
/* 8-byte aligned; NULL when full */
static void* mco_pool_alloc(mco_http_pool_t* p, size_t n) {
    size_t aligned = (n + 7u) & ~(size_t)7u;
    if (p->off + aligned > p->cap) return NULL;
    void* r = p->base + p->off;
    p->off += aligned;
    return r;
}
/* copy with a NUL terminator; NULL when full */
static char* mco_pool_str(mco_http_pool_t* p, const char* s, size_t len) {
    char* d = (char*)mco_pool_alloc(p, len + 1);
    if (!d) return NULL;
    memcpy(d, s, len);
    d[len] = 0;
    return d;
}

/* Read Content-Length bytes using the connection scratch buffer. */
static int read_body_content_length(mco_socket_t* sock, body_sink_t* sink, long content_length,
                                     const char** leftover, int* leftover_len, int timeout_ms,
                                     char* tmp_buf) {
    int remaining = (int)content_length;
    if (*leftover_len > 0) {
        int take = *leftover_len < remaining ? *leftover_len : remaining;
        if (sink->write(sink->ctx, *leftover, take) != 0) return MCO_ERR_IO;
        *leftover += take; *leftover_len -= take;
        remaining -= take;
    }
    while (remaining > 0) {
        int want = remaining < MCO_HTTP_BODY_CHUNK ? remaining : MCO_HTTP_BODY_CHUNK;
        int n = mco_read(sock, tmp_buf, want, timeout_ms);
        if (n <= 0) return (n == 0) ? MCO_ERR_CLOSED : n;
        if (sink->write(sink->ctx, tmp_buf, n) != 0) return MCO_ERR_IO;
        remaining -= n;
    }
    return 0;
}

static int read_body_until_eof(mco_socket_t* sock, body_sink_t* sink,
                                const char** leftover, int* leftover_len, int timeout_ms,
                                char* tmp_buf) {
    if (*leftover_len > 0) {
        if (sink->write(sink->ctx, *leftover, *leftover_len) != 0) return MCO_ERR_IO;
        *leftover += *leftover_len; *leftover_len = 0;
    }
    for (;;) {
        int n = mco_read(sock, tmp_buf, MCO_HTTP_BODY_CHUNK, timeout_ms);
        if (n < 0) return n;
        if (n == 0) return 0;
        if (sink->write(sink->ctx, tmp_buf, n) != 0) return MCO_ERR_IO;
    }
}

/* Chunked decoding; leftover is updated with the unconsumed bytes (a pipelined
 * next request or WS frames) for the keep-alive loop. */
static int read_body_chunked(mco_socket_t* sock, body_sink_t* sink,
                              const char** leftover, int* leftover_len, int timeout_ms,
                              char* tmp_buf) {
    char* inbuf = tmp_buf;   /* connection scratch, avoids a big stack array */
    int in_len = 0;
    long total_sock_read = 0;   /* bytes read from the socket */
    int orig_leftover_len = *leftover_len;

    if (*leftover_len > MCO_HTTP_BODY_CHUNK) return MCO_ERR_IO;
    if (*leftover_len > 0) {
        memcpy(inbuf, *leftover, (size_t)*leftover_len);
        in_len = *leftover_len;
    }

    for (;;) {
        char* line_end;
        long chunk_size;
        int header_len;

        for (;;) {
            int i;
            line_end = NULL;
            for (i = 0; i + 1 < in_len; i++) {
                if (inbuf[i] == '\r' && inbuf[i + 1] == '\n') { line_end = inbuf + i; break; }
            }
            if (line_end) break;
            if (in_len >= MCO_HTTP_BODY_CHUNK) return MCO_ERR_IO;
            {
                int n = mco_read(sock, inbuf + in_len, MCO_HTTP_BODY_CHUNK - in_len, timeout_ms);
                if (n <= 0) return (n == 0) ? MCO_ERR_CLOSED : n;
                in_len += n; total_sock_read += n;
            }
        }

        chunk_size = strtol(inbuf, NULL, 16);
        header_len = (int)(line_end - inbuf) + 2;
        if (chunk_size < 0) return MCO_ERR_IO; /* guard against a negative length */
        if (chunk_size == 0) {
            /* Return the unconsumed tail to leftover. Consumption is FIFO, so the
             * bytes of the original leftover consumed are min(consumed, leftover). */
            int remaining = in_len - header_len;   /* unconsumed bytes */
            long total_in = (long)orig_leftover_len + total_sock_read;
            long consumed_total = total_in - remaining;
            int from_leftover;
            if (consumed_total < 0) consumed_total = 0;
            from_leftover = orig_leftover_len - (int)(consumed_total < orig_leftover_len ? consumed_total : orig_leftover_len);
            if (from_leftover < 0) from_leftover = 0;
            if (from_leftover > *leftover_len) from_leftover = *leftover_len;
            *leftover += *leftover_len - from_leftover;
            *leftover_len = from_leftover;
            return 0;
        }

        while (in_len - header_len < chunk_size + 2) {
            if (in_len >= MCO_HTTP_BODY_CHUNK) {
                int can_emit = in_len - header_len;
                if (can_emit > 0) {
                    if (sink->write(sink->ctx, inbuf + header_len, can_emit) != 0) return MCO_ERR_IO;
                    chunk_size -= can_emit;
                    in_len = 0; header_len = 0;
                } else {
                    return MCO_ERR_IO;
                }
            }
            {
                int n = mco_read(sock, inbuf + in_len, MCO_HTTP_BODY_CHUNK - in_len, timeout_ms);
                if (n <= 0) return (n == 0) ? MCO_ERR_CLOSED : n;
                in_len += n; total_sock_read += n;
            }
        }

        if (sink->write(sink->ctx, inbuf + header_len, (int)chunk_size) != 0) return MCO_ERR_IO;

        {
            int consumed = header_len + (int)chunk_size + 2;
            int remain = in_len - consumed;
            if (remain > 0) memmove(inbuf, inbuf + consumed, (size_t)remain);
            in_len = remain;
        }
    }
}

/* ---- route table (per server instance) ---- */
#define MCO_HTTP_MAX_ROUTES 64

typedef struct {
    char method[16];           /* empty = any method */
    char pattern[256];
    mco_http_handler_t handler;
    void* userdata;
} mco_http_route_t;

/* WS routes are separate: Upgrade is checked first, so they never shadow GET. */
#define MCO_WS_MAX_ROUTES 32
typedef struct {
    char method[16];
    char pattern[256];
    mco_ws_handler_t handler;
    void* userdata;
} mco_ws_route_t;

/* static file route ctx (embedded in the instance) */
#define MCO_HTTP_MAX_STATIC_ROUTES 16
typedef struct {
    char prefix[128];   /* registered prefix, e.g. "/static/" */
    size_t prefix_len;
    char root[512];     /* document root */
    struct mco_http_server* srv; /* back pointer for the 404 fallback */
} static_route_t;

/* Server instance: routes, hooks, keep-alive switch, serve state. */

/* Per-connection ctx: pool + req/resp + body scratch, reused across keep-alive
 * requests and recycled into the instance's conn_free pool. */
typedef struct http_conn {
    mco_http_pool_t    pool;
    size_t             mark;    /* pool cursor at request start */
    char*              scratch; /* body scratch (MCO_HTTP_BODY_CHUNK) */
    mco_http_request_t req;     /* reused per connection */
    mco_http_response_t resp;   /* reused per connection */
    struct http_conn*  free_next; /* free-list link */
} http_conn_t;

struct mco_http_server {
    mco_http_route_t   routes[MCO_HTTP_MAX_ROUTES];
    mco_ws_route_t     ws_routes[MCO_WS_MAX_ROUTES];
    static_route_t     static_routes[MCO_HTTP_MAX_STATIC_ROUTES];
    mco_http_handler_t not_found_handler;
    mco_http_handler_t error_handler;
    mco_ssl_ctx_t      ssl_ctx;         /* non-NULL: TLS server handshake (HTTPS) */
    /* Recycled connection contexts: saves a malloc/free per connection. */
    struct http_conn*  conn_free;
    int                num_routes;
    int                num_ws_routes;
    int                num_static_routes;
    int                keepalive;      /* master switch, default on */
    int                stop;            /* graceful stop flag */
    int                num_conn_free;
};

#define MCO_HTTP_CONN_FREE_MAX 16

typedef struct { mco_socket_t* l; mco_loop_t* loop; mco_http_server_t* srv; } serve_arg_t;
typedef struct { mco_http_server_t* srv; mco_socket_t* sock; } conn_arg_t;

mco_http_server_t* mco_http_server_new(void) {
    mco_http_server_t* srv = (mco_http_server_t*)calloc(1, sizeof(mco_http_server_t));
    if (!srv) return NULL;
    srv->keepalive = 1;
    return srv;
}

void mco_http_server_free(mco_http_server_t* srv) {
    /* See the header: must be stopped with the loop drained before free. */
    if (!srv) return;
    while (srv->conn_free) {
        http_conn_t* c = srv->conn_free;
        srv->conn_free = c->free_next;
        free(c->pool.base);
        free(c->scratch);
        chars_t_cleanup(&c->resp.body);   /* response body buffer (reused) */
        free(c);
    }
    free(srv);
}

void mco_http_server_handle(mco_http_server_t* srv, const char* method,
                            const char* path_pattern,
                            mco_http_handler_t handler, void* userdata) {
    int i;
    if (!srv) return;
    /* a repeat overwrites */
    for (i = 0; i < srv->num_routes; i++) {
        if (strcmp(srv->routes[i].pattern, path_pattern) == 0 &&
            (method == NULL ? srv->routes[i].method[0] == 0
                            : strcmp(srv->routes[i].method, method) == 0)) {
            break;
        }
    }
    if (i == srv->num_routes) {
        if (srv->num_routes >= MCO_HTTP_MAX_ROUTES) return; /* full: ignore */
        srv->num_routes++;
    }
    srv->routes[i].method[0] = 0;
    if (method) {
        /* snprintf guarantees the NUL terminator */
        snprintf(srv->routes[i].method, sizeof(srv->routes[i].method), "%s", method);
    }
    snprintf(srv->routes[i].pattern, sizeof(srv->routes[i].pattern), "%s", path_pattern);
    srv->routes[i].handler = handler;
    srv->routes[i].userdata = userdata;
}

void mco_http_server_set_not_found_handler(mco_http_server_t* srv, mco_http_handler_t h) {
    if (srv) srv->not_found_handler = h;
}
void mco_http_server_set_error_handler(mco_http_server_t* srv, mco_http_handler_t h) {
    if (srv) srv->error_handler = h;
}

void mco_http_server_set_keepalive(mco_http_server_t* srv, int enabled) {
    if (srv) srv->keepalive = enabled ? 1 : 0;
}

void mco_http_server_set_tls(mco_http_server_t* srv, mco_ssl_ctx_t ctx) {
    if (srv) srv->ssl_ctx = ctx;
}

void mco_http_server_handle_ws(mco_http_server_t* srv, const char* method,
                               const char* path_pattern,
                               mco_ws_handler_t ws_handler, void* userdata)
{
    if (!srv || srv->num_ws_routes >= MCO_WS_MAX_ROUTES) return;
    mco_ws_route_t* r = &srv->ws_routes[srv->num_ws_routes++];
    snprintf(r->method, sizeof(r->method), "%s", method ? method : "GET");
    snprintf(r->pattern, sizeof(r->pattern), "%s", path_pattern ? path_pattern : "/");
    r->handler = ws_handler;
    r->userdata = userdata;
}

/* forward declaration for ws_route_find */
static int route_match(const char* pattern, const char* path, mco_http_request_t* req);

/* Find a matching WS route (pattern supports :param). */
static const mco_ws_route_t* ws_route_find(mco_http_server_t* srv,
                                          const char* method, const char* path)
{
    int i;
    for (i = 0; i < srv->num_ws_routes; i++) {
        const mco_ws_route_t* r = &srv->ws_routes[i];
        if (strcasecmp(r->method, method) != 0) continue;
        if (route_match(r->pattern, path, NULL)) return r;
    }
    return NULL;
}

/* Match path against pattern, supporting ":param" and "*". Fills req->params
 * (req may be NULL for a pure match). */
static int route_match(const char* pattern, const char* path, mco_http_request_t* req) {
    const char* pp = pattern;
    const char* qp = path;
    if (req) req->num_params = 0;
    for (;;) {
        if (*pp == '*') {
            /* "*" as the last segment matches anything (including empty);
             * otherwise backtrack over qp. */
            pp++;
            if (*pp == '\0') return 1;
            for (;;) {
                if (route_match(pp, qp, NULL)) return 1; /* backtrack, no params */
                if (*qp == '\0') return 0;
                qp++;
            }
        }
        if (*pp == '\0' && *qp == '\0') return 1;
        if (*pp == '\0' || *qp == '\0') {
            /* a trailing "/" in the pattern is lenient */
            if (*pp == '/' && *(pp + 1) == '\0' && *qp == '\0') return 1;
            return 0;
        }
        if (*pp == ':') {
            /* param segment: up to the next '/' */
            const char* pname = pp + 1;
            const char* pname_end = pname;
            while (*pname_end && *pname_end != '/') pname_end++;
            const char* val_start = qp;
            const char* val_end = qp;
            while (*val_end && *val_end != '/') val_end++;
            if (req && req->num_params < MCO_HTTP_MAX_PARAMS) {
                /* slice into the path buffer (caller owns its lifetime) */
                req->params[req->num_params].name = pname;  /* pattern is static */
                req->params[req->num_params].value = val_start;
                req->num_params++;
            }
            pp = pname_end;
            qp = val_end;
        } else if (*pp == *qp) {
            pp++; qp++;
        } else {
            return 0;
        }
    }
}

static mco_http_route_t* route_find(mco_http_server_t* srv, const char* method,
                                    const char* path, mco_http_request_t* req) {
    int i;
    for (i = 0; i < srv->num_routes; i++) {
        int method_ok = (srv->routes[i].method[0] == 0) || strcmp(srv->routes[i].method, method) == 0;
        if (method_ok && route_match(srv->routes[i].pattern, path, req)) return &srv->routes[i];
    }
    return NULL;
}

/* ---- status code -> reason text ---- */
static const char* status_reason(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 416: return "Range Not Satisfiable";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default:  return "OK";
    }
}

static void resp_body_reset(mco_http_response_t* resp) {
    resp->body.size = 0;
    *chars_t_cstr(&resp->body) = '\0';
}

/* Free resp's heap members. With a pool, headers die on rewind; otherwise each is
 * freed. body is reset, not destroyed (capacity is reused). */
static void free_response_members(mco_http_response_t* resp) {
    int i;
    if (!resp) return;
    if (resp->_server_pool == NULL) {
        /* client/standalone: all heap */
        for (i = 0; i < resp->num_headers; i++) {
            free(resp->headers[i].name);
            free(resp->headers[i].value);
        }
        free(resp->headers);
    }
    resp_body_reset(resp);
    free(resp->_server_file_path);   /* strdup'ed file path */
    resp->_server_file_path = NULL;
    resp->_server_file_size = 0;
    resp->headers = NULL;
    resp->num_headers = 0;
    resp->_headers_cap = 0;
}

void mco_http_response_add_header(mco_http_response_t* resp, const char* name, const char* value)
{
    if (resp->_server_pool) {
        /* server: headers grow freely; name/value come from the pool. */
        mco_http_pool_t* p = (mco_http_pool_t*)resp->_server_pool;
        if (resp->num_headers >= resp->_headers_cap) {
            int newcap = resp->_headers_cap ? resp->_headers_cap * 2 : 8;
            mco_http_header_pair_t* arr = (mco_http_header_pair_t*)
                mco_pool_alloc(p, (size_t)newcap * sizeof(*arr));
            if (arr) {
                /* bump-allocated array: copy the existing entries over. */
                if (resp->num_headers > 0)
                    memcpy(arr, resp->headers, (size_t)resp->num_headers * sizeof(*arr));
                resp->headers = arr;
                resp->_headers_cap = newcap;
            }
        }
        if (resp->num_headers < resp->_headers_cap) {
            char* n = mco_pool_str(p, name, strlen(name));
            char* v = mco_pool_str(p, value, strlen(value));
            if (n && v) {
                resp->headers[resp->num_headers].name = n;
                resp->headers[resp->num_headers].value = v;
                resp->num_headers++;
            }
        }
    } else {
        /* client/standalone: heap array grown by _headers_cap. */
        if (resp->num_headers >= resp->_headers_cap) {
            int newcap = resp->_headers_cap ? resp->_headers_cap * 2 : 8;
            mco_http_header_pair_t* arr;
            if (newcap > MCO_HTTP_MAX_HEADERS) newcap = MCO_HTTP_MAX_HEADERS;
            if (resp->num_headers >= newcap) return; /* at the cap: drop it */
            arr = (mco_http_header_pair_t*)realloc(resp->headers,
                        (size_t)newcap * sizeof(*arr));
            if (!arr) return;
            resp->headers = arr;
            resp->_headers_cap = newcap;
        }
        resp->headers[resp->num_headers].name = strdup(name);
        resp->headers[resp->num_headers].value = strdup(value);
        if (!resp->headers[resp->num_headers].name || !resp->headers[resp->num_headers].value) {
            free(resp->headers[resp->num_headers].name);
            free(resp->headers[resp->num_headers].value);
            return; /* strdup failed: do not advance the count */
        }
        resp->num_headers++;
    }
}

/* Set body + default Content-Type (only when the handler has not set one). */
static void resp_set_body_type(mco_http_response_t* resp, int status,
                               const char* s, const char* ctype) {
    resp->status = status;
    chars_t_set(&resp->body, s, strlen(s));   /* overwrites without reallocating */
    if (mco_http_response_get_header(resp, "Content-Type") == NULL) {
        mco_http_response_add_header(resp, "Content-Type", ctype);
    }
}

void mco_http_resp_text(mco_http_response_t* resp, int status, const char* text) {
    resp_set_body_type(resp, status, text ? text : "", "text/plain; charset=utf-8");
}

/* ---- lazy body: the handler consumes it on demand ---- */

/* Body read state on req->_bodyio; valid while the handler runs. */
typedef struct {
    mco_socket_t* sock;
    const char**  leftover;
    int*          leftover_len;
    int           timeout_ms;
    int           is_chunked;     /* Transfer-Encoding: chunked */
    long          content_length; /* -1 = no body */
    int           consumed;       /* already consumed */
    char*         scratch;        /* connection scratch */
    /* Stack seed owned by handle_one_request: lets mco_http_req_body() hold
     * small bodies without a malloc. When the growbuf stays on it, req->body
     * must not be freed (see body_on_stack). */
    char*         body_stack;
    int           body_stack_cap;
    int           body_on_stack;
} body_reader_t;

/* Read the body into a sink. 0 ok, <0 failure. */
static int body_read(body_reader_t* br, body_sink_t* sink) {
    if (br->is_chunked)
        return read_body_chunked(br->sock, sink, br->leftover, br->leftover_len, br->timeout_ms,
                                 br->scratch);
    if (br->content_length >= 0)
        return read_body_content_length(br->sock, sink, br->content_length,
                                        br->leftover, br->leftover_len, br->timeout_ms, br->scratch);
    return 0; /* no body */
}

/* file sink */
static int file_sink_write(void* ctx, const char* data, int len) {
    FILE* f = (FILE*)ctx;
    return fwrite(data, 1, (size_t)len, f) == (size_t)len ? 0 : -1;
}

/* discard sink: drains an unconsumed body */
static int null_sink_write(void* ctx, const char* data, int len) {
    (void)ctx; (void)data; (void)len;
    return 0;
}

/* stream the body to path (see the header) */
int mco_http_req_save_upload(mco_http_request_t* req, const char* path) {
    body_reader_t* br = (body_reader_t*)req->_bodyio;
    body_sink_t sink;
    FILE* f;
    int rc;
    if (!br || br->consumed || !path) return -1;
    f = fopen(path, "wb");
    if (!f) return -1;
    setvbuf(f, NULL, _IOFBF, 64 * 1024); /* avoid per-4KB stdio writes */
    sink.write = file_sink_write;
    sink.ctx = f;
    rc = body_read(br, &sink);
    fclose(f);
    if (rc != 0) return -1;
    br->consumed = 1;
    req->body = NULL; /* on disk now, not in memory */
    req->body_len = 0;
    return 0;
}

/* read the body into memory and cache it */
const char* mco_http_req_body(mco_http_request_t* req, int* out_len) {
    body_reader_t* br = (body_reader_t*)req->_bodyio;
    if (out_len) *out_len = 0;
    if (!br || br->consumed) return req->body; /* empty/multipart/streamed -> NULL */
    {
        growbuf_t g;
        body_sink_t sink;
        int rc;
        /* Seed on the handler's stack frame (body_stack) when available: a body
         * that fits needs no allocation at all. */
        growbuf_init_stack(&g, br->body_stack, br->body_stack_cap);
        sink.write = growbuf_sink_write;
        sink.ctx = &g;
        rc = body_read(br, &sink);
        if (rc != 0) { growbuf_release(&g); return NULL; }
        br->consumed = 1;
        if (g.len == 0) {
            /* nothing read: keep the historical NULL (no body) */
            growbuf_release(&g);
            req->body = NULL;
            req->body_len = 0;
        } else {
            br->body_on_stack = g.on_stack;
            req->body = g.data; /* owned by req; freed only when heap-allocated */
            req->body_len = g.len;
        }
    }
    if (out_len) *out_len = req->body_len;
    return req->body;
}

/* build an HTML response (for the built-in error pages) */
static void mco_http_resp_html(mco_http_response_t* resp, int status, const char* html) {
    resp_set_body_type(resp, status, html ? html : "", "text/html; charset=utf-8");
}

/* ---- chunked / static files / MIME ---- */

/* decimal digits into dst (no NUL), returns the count */
static int u64_to_str(char* dst, unsigned long long v) {
    char tmp[24];
    int n = 0, i;
    do { tmp[n++] = (char)('0' + (int)(v % 10u)); v /= 10u; } while (v);
    for (i = 0; i < n; ++i) dst[i] = tmp[n - 1 - i];
    return n;
}

/* lowercase hex digits into dst (no NUL), returns the count */
static int u64_to_hex(char* dst, unsigned long long v) {
    char tmp[16];
    int n = 0, i;
    do {
        int d = (int)(v & 0xfu);
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
        v >>= 4;
    } while (v);
    for (i = 0; i < n; ++i) dst[i] = tmp[n - 1 - i];
    return n;
}

/* Build the response head into out: status line, Content-Length/Transfer-Encoding,
 * Connection, custom headers, blank line. Shared by normal and chunked responses.
 * Hand-assembled (memcpy + a local itoa); snprintf cost ~6% in profiles. */
static int build_response_head(growbuf_t* out, mco_http_response_t* resp, int is_chunked) {
    int i;
    const char* reason = status_reason(resp->status);
    int keepalive = resp->_server_keepalive;
    int has_cl = 0, has_te = 0, has_conn = 0;
    int hdr_start;      /* where the custom headers begin */
    char auto_hdr[80];  /* auto CL/TE + Connection, inserted before them */
    int auto_len = 0;

    /* status line */
    {
        char num[24];
        int nlen, rlen = (int)strlen(reason);
        if (growbuf_ensure(out, 9 + 20 + 1 + rlen + 2) != 0) return -1;
        memcpy(out->data + out->len, "HTTP/1.1 ", 9);
        out->len += 9;
        nlen = u64_to_str(num, (unsigned long long)resp->status);
        memcpy(out->data + out->len, num, (size_t)nlen);
        out->len += nlen;
        out->data[out->len++] = ' ';
        memcpy(out->data + out->len, reason, (size_t)rlen);
        out->len += rlen;
        out->data[out->len++] = '\r';
        out->data[out->len++] = '\n';
    }

    /* Single pass over the headers: detect the auto-managed ones and write the
     * custom ones. The auto headers are built separately and inserted before
     * them afterwards, keeping the original order with one traversal. */
    hdr_start = out->len;
    for (i = 0; i < resp->num_headers; i++) {
        const char* hname = resp->headers[i].name;
        const char* hval  = resp->headers[i].value;
        int nlen = (int)strlen(hname);
        int vlen = (int)strlen(hval);
        if (nlen == 14) {
            if (header_name_is(hname, (size_t)nlen, "Content-Length", 14)) has_cl = 1;
        } else if (nlen == 17) {
            if (header_name_is(hname, (size_t)nlen, "Transfer-Encoding", 17)) has_te = 1;
        } else if (nlen == 10) {
            if (header_name_is(hname, (size_t)nlen, "Connection", 10)) has_conn = 1;
        }
        if (growbuf_ensure(out, nlen + vlen + 4) != 0) return -1;
        memcpy(out->data + out->len, hname, (size_t)nlen);
        out->len += nlen;
        out->data[out->len++] = ':';
        out->data[out->len++] = ' ';
        memcpy(out->data + out->len, hval, (size_t)vlen);
        out->len += vlen;
        out->data[out->len++] = '\r';
        out->data[out->len++] = '\n';
    }

    if (is_chunked) {
        if (!has_te) {
            memcpy(auto_hdr + auto_len, "Transfer-Encoding: chunked\r\n", 28);
            auto_len += 28;
        }
    } else if (!has_cl) {
        /* file response: Content-Length is the file size */
        unsigned long long v = resp->_server_file_path
            ? (unsigned long long)resp->_server_file_size
            : (unsigned long long)resp->body.size;
        memcpy(auto_hdr + auto_len, "Content-Length: ", 16);
        auto_len += 16;
        auto_len += u64_to_str(auto_hdr + auto_len, v);
        auto_hdr[auto_len++] = '\r';
        auto_hdr[auto_len++] = '\n';
    }
    if (!has_conn) {
        const char* s = keepalive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
        int sl = (int)strlen(s);
        memcpy(auto_hdr + auto_len, s, (size_t)sl);
        auto_len += sl;
    }
    if (auto_len > 0) {
        if (growbuf_ensure(out, auto_len) != 0) return -1;
        memmove(out->data + hdr_start + auto_len, out->data + hdr_start,
                (size_t)(out->len - hdr_start));
        memcpy(out->data + hdr_start, auto_hdr, (size_t)auto_len);
        out->len += auto_len;
    }

    if (growbuf_ensure(out, 2) != 0) return -1;
    out->data[out->len++] = '\r';
    out->data[out->len++] = '\n';
    return 0;
}

void mco_http_resp_set_chunked(mco_http_response_t* resp, int status) {
    resp->status = status;
    resp->_server_chunked = 1;
}

/* Only "0 = ok / -1 = failed".
 */
static inline int write_all(mco_socket_t* sock, const char* data, int len, int timeout_ms) {
    return mco_write_all(sock, data, len, timeout_ms) == len ? 0 : -1;
}

/* ---- writev gather write: several segments in one syscall. TLS and Windows
 * fall back to per-segment writes. 0 = all written, -1 = failure. At most 4
 * segments (not declared as d[4]/l[4]: callers pass d[2]/d[3] and gcc emits a
 * bogus -Wstringop-overflow). */
#if !defined(_WIN32)
#include <sys/uio.h>
#endif
static int writev_all(mco_socket_t* sock, const char* const* d, const int* l, int nseg, int timeout_ms) {
    int i;
    if (mco_socket_is_ssl(sock)) {
        for (i = 0; i < nseg; i++)
            if (l[i] > 0 && write_all(sock, d[i], l[i], timeout_ms) != 0) return -1;
        return 0;
    }
#if defined(_WIN32)
    for (i = 0; i < nseg; i++)
        if (l[i] > 0 && write_all(sock, d[i], l[i], timeout_ms) != 0) return -1;
    return 0;
#else
    {
        struct iovec iov[4];
        int cnt = 0;
        int fd = mco_socket_fd(sock);
        for (i = 0; i < nseg; i++) {
            if (l[i] <= 0) continue;
            iov[cnt].iov_base = (void*)d[i];
            iov[cnt].iov_len  = (size_t)l[i];
            cnt++;
        }
        /* advance by offset instead of shifting: never touches uninitialised slots */
        {
            int off = 0;   /* iov[off..cnt-1] is what is left */
            while (off < cnt) {
                ssize_t n = writev(fd, iov + off, cnt - off);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
                    if (mco_wait_io(fd, MCO_EV_WRITE, timeout_ms) < 0) return -1;
                    continue;
                }
                while (off < cnt && (size_t)n >= iov[off].iov_len) {
                    n -= (ssize_t)iov[off].iov_len;
                    off++;
                }
                if (off < cnt && n > 0) {
                    iov[off].iov_base = (char*)iov[off].iov_base + n;
                    iov[off].iov_len -= (size_t)n;
                }
            }
        }
        return 0;
    }
#endif
}

void mco_http_resp_write_chunk(mco_http_response_t* resp, const char* data, int len) {
    char head[32];
    int headlen;
    if (!resp->_server_sock) return; /* not inside a handler */
    /* The head must precede any chunk: write_response skips it once
     * _server_headers_sent is set. */
    if (!resp->_server_headers_sent) {
        char headbuf[1024];  /* stack-seeded; spills to heap */
        growbuf_t out;
        growbuf_init_stack(&out, headbuf, (int)sizeof(headbuf));
        if (build_response_head(&out, resp, 1) == 0) {
            write_all(resp->_server_sock, out.data, out.len, 10000);
        }
        growbuf_release(&out);
        resp->_server_headers_sent = 1;
    }
    if (data == NULL || len <= 0) {
        /* terminating chunk */
        const char* end = "0\r\n\r\n";
        write_all(resp->_server_sock, end, (int)strlen(end), 10000);
        resp->_server_chunked = 0; /* done; keep-alive may continue */
        return;
    }
    headlen = u64_to_hex(head, (unsigned long long)len);
    head[headlen++] = '\r';
    head[headlen++] = '\n';
    {
        /* head + data + end in one write */
        const char* d[3] = { head, data, "\r\n" };
        int l[3] = { headlen, len, 2 };
        writev_all(resp->_server_sock, d, l, 3, 10000);
    }
}

int mco_http_resp_send_file(mco_http_response_t* resp, const char* path) {
    struct stat st;
    if (!resp || !path) return -1;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    free(resp->_server_file_path);   /* may be called again, overwriting */
    resp->_server_file_path = strdup(path);
    if (!resp->_server_file_path) return -1;
    resp->_server_file_size = (long long)st.st_size;
    return 0;
}

/* Chunk sizes for large files: the sendfile count cap per syscall, and the
 * read/SSL_write feed size for the TLS fallback (malloc'ed, too big for stack). */
#define MCO_HTTP_SENDFILE_CHUNK (512 * 1024)
#define MCO_HTTP_TLS_CHUNK      (32 * 1024)

static int send_file_response(mco_socket_t* sock, const char* path,
                               long long size, long long start_off, int timeout_ms) {
    int filefd = open(path, O_RDONLY);
    int ret;
    if (filefd < 0) return -1;

#if defined(__linux__)
    if (!mco_socket_is_ssl(sock)) {
        int outfd = mco_socket_fd(sock);
        off_t off = (off_t)start_off;
        ret = 0;
        while (off < (off_t)(start_off + size)) {
            ssize_t n = sendfile(outfd, filefd, &off,
                                 (size_t)((start_off + size - (long long)off > MCO_HTTP_SENDFILE_CHUNK)
                                              ? MCO_HTTP_SENDFILE_CHUNK
                                              : (start_off + size - (long long)off)));
            if (n > 0) continue;
            if (n == 0) { ret = -1; break; }   /* file shrank since stat */
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) { ret = -1; break; }
            {
                int r = mco_wait_io(outfd, MCO_EV_WRITE, timeout_ms);
                if (r < 0) { ret = -1; break; } /* timeout / closed / io error */
            }
        }
        close(filefd);
        return ret;
    }
#endif

    /* fallback: TLS or non-Linux */
    {
        char buf[MCO_HTTP_TLS_CHUNK];
        ret = 0;
        /* seek to the range start; sendfile(&off) does it on Linux */
        if (start_off > 0 && lseek(filefd, (off_t)start_off, SEEK_SET) < 0) {
            close(filefd);
            return -1;
        }
        {
            long long remaining = size;
            while (remaining > 0) {
                int want = remaining > MCO_HTTP_TLS_CHUNK
                         ? MCO_HTTP_TLS_CHUNK : (int)remaining;
                ssize_t r = read(filefd, buf, (size_t)want);
                if (r == 0) { ret = -1; break; } /* file shorter than expected */
                if (r < 0) {
                    if (errno == EINTR) continue;
                    ret = -1;
                    break;
                }
                if (write_all(sock, buf, (int)r, timeout_ms) != 0) {
                    ret = -1;
                    break;
                }
                remaining -= r;
            }
        }
        close(filefd);
        return ret;
    }
}

/* common MIME types for static files */
static struct { const char* ext; const char* type; } g_mime[] = {
    { "html", "text/html; charset=utf-8" },
    { "htm",  "text/html; charset=utf-8" },
    { "css",  "text/css; charset=utf-8" },
    { "js",   "application/javascript; charset=utf-8" },
    { "json", "application/json; charset=utf-8" },
    { "txt",  "text/plain; charset=utf-8" },
    { "png",  "image/png" },
    { "jpg",  "image/jpeg" },
    { "jpeg", "image/jpeg" },
    { "gif",  "image/gif" },
    { "svg",  "image/svg+xml" },
    { "ico",  "image/x-icon" },
    { "pdf",  "application/pdf" },
    { "zip",  "application/zip" },
    { "wasm", "application/wasm" },
    { NULL,   NULL },
};

const char* mco_http_mime_type(const char* path) {
    const char* dot = strrchr(path, '.');
    if (dot && dot[1]) {
        const char* ext = dot + 1;
        int i;
        for (i = 0; g_mime[i].ext; i++) {
            if (strcasecmp(g_mime[i].ext, ext) == 0) return g_mime[i].type;
        }
    }
    return "application/octet-stream";
}

/* Join the relative path under root, rejecting any ".." (must stay under root). */
static int static_resolve_path(const char* root, const char* rest, char* out, int outcap) {
    const char* p = rest;
    int off;
    /* rest starts with '/'; skip it */
    while (*p == '/') p++;
    /* join segment by segment, rejecting ".." */
    off = snprintf(out, (size_t)outcap, "%s", root);
    if (off <= 0 || off >= outcap) return -1;
    while (*p) {
        /* take one segment */
        const char* seg = p;
        size_t seglen;
        while (*p && *p != '/') p++;
        seglen = (size_t)(p - seg);
        if (seglen == 0) { if (*p == '/') { p++; } continue; }
        if (seglen == 1 && seg[0] == '.') {
            /* ignore "." */
        } else if (seglen == 2 && seg[0] == '.' && seg[1] == '.') {
            return -1; /* traversal: reject */
        } else {
            if (out[off - 1] != '/') { out[off++] = '/'; if (off >= outcap) return -1; }
            if (off + (int)seglen >= outcap) return -1;
            memcpy(out + off, seg, seglen);
            off += (int)seglen;
            out[off] = 0;
        }
        if (*p == '/') p++;
    }
    return 0;
}

/* Static files above 1MB stream via chunked; smaller ones use Content-Length. */
#define MCO_HTTP_STATIC_CHUNK_THRESHOLD (1 << 20)

/* ---- Range support (RFC 7233, single range) ---- */
static const char* find_header_value_r(mco_http_request_t* req, const char* name,
                                       size_t* out_len);

/* Parse a Range header. 0 = none or unsupported form (ignore, 200);
 * 1 = valid inclusive range; -1 = unsatisfiable (416). vlen-bounded. */
static int parse_range_header(const char* v, size_t vlen, long size, long* start, long* end) {
    const char* p;
    const char* v_end;
    if (!v) return 0;
    v_end = v + vlen;
    p = v;
    /* skip leading whitespace */
    while (p < v_end && (*p == ' ' || *p == '\t')) p++;
    if ((size_t)(v_end - p) < 6 || strncasecmp(p, "bytes=", 6) != 0) return 0; /* bytes only */
    p += 6;
    if (memchr(p, ',', (size_t)(v_end - p))) return 0; /* multiple ranges: ignore */

    if (p < v_end && *p == '-') {
        /* bytes=-N: the last N bytes */
        char* endp;
        long n;
        if (p + 1 == v_end) return 0; /* no digits after '-' */
        n = strtol(p + 1, &endp, 10);
        if (endp == p + 1 || endp != v_end) return 0; /* malformed: ignore */
        if (n <= 0) return -1; /* bytes=-0 is never satisfiable */
        if (size <= 0) return -1;   /* empty file */
        if (n > size) { *start = 0; *end = size - 1; } /* whole file */
        else { *start = size - n; *end = size - 1; }
        return 1;
    }
    {
        char* endp;
        long s = strtol(p, &endp, 10);
        if (endp == p) return 0; /* no digits: ignore */
        p = endp;
        if (p >= v_end || *p != '-') return 0; /* missing '-': ignore */
        p++;
        if (p == v_end) {
            /* bytes=S-: to the end of the file */
            if (size <= 0 || s >= size) return -1;
            *start = s; *end = size - 1;
            return 1;
        }
        {
            long e = strtol(p, &endp, 10);
            if (endp == p || endp != v_end) return 0;
            if (e < s) return 0; /* start>end: ignore the header */
            if (size <= 0 || s >= size) return -1;
            *start = s;
            *end = (e >= size) ? size - 1 : e; /* clamp to the end of the file */
            return 1;
        }
    }
}

/* Serve a file as the body. range_hdr non-NULL applies RFC 7233 (206/416/ignore).
 * status is the base code (404 fallback ignores range_hdr). 0 ok, -1 unopenable. */
static int serve_file_content(mco_http_response_t* resp, const char* full,
                              int status, const char* range_hdr, size_t range_len) {
    struct stat st;
    long sz, start = 0, end;
    int is_range = 0;
    /* stat() instead of fopen+fseek+ftell: one syscall, and the large-file path
     * below never needs the fd (send_file_response opens the path itself). */
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    sz = (long)st.st_size;
    end = sz > 0 ? sz - 1 : 0;

    if (status == 200 && range_hdr) {
        int r = parse_range_header(range_hdr, range_len, sz, &start, &end);
        if (r < 0) {
            /* 416 carries Content-Range with the full size */
            if(resp->num_headers < MCO_HTTP_MAX_HEADERS){
                char cr[64];
                snprintf(cr, sizeof(cr), "bytes */%ld", sz);
                mco_http_response_add_header(resp, "Content-Range", cr);
            }
            mco_http_resp_text(resp, 416, "requested range not satisfiable");
            return 0;
        }
        is_range = r;
    }

    /* Content-Type from the mime table */
    mco_http_response_add_header(resp, "Content-Type", mco_http_mime_type(full));
    /* advertise range support */
    mco_http_response_add_header(resp, "Accept-Ranges", "bytes");
    if (is_range && resp->num_headers < MCO_HTTP_MAX_HEADERS) {
        char cr[64];
        snprintf(cr, sizeof(cr), "bytes %ld-%ld/%ld", start, end, sz);
        mco_http_response_add_header(resp, "Content-Range", cr);
    }

    resp->status = is_range ? 206 : status;
    {
        /* bytes and offset to send */
        long content_len = is_range ? (end - start + 1) : sz;
        if (content_len > MCO_HTTP_STATIC_CHUNK_THRESHOLD) {
            /* Large files: sendfile() zero-copy (TLS/non-Linux falls back to a
             * read/write loop). Nothing is buffered in memory. */
            resp_body_reset(resp);   /* mutually exclusive with a file response */
            resp->_server_file_path = strdup(full);
            if (!resp->_server_file_path) return -1;
            resp->_server_file_size = content_len;
            resp->_server_file_off = is_range ? start : 0;
            return 0;
        }

        /* small file/range: read into resp->body */
        {
            FILE* f = fopen(full, "rb");
            char* buf;
            if (!f) return -1;
            buf = (char*)malloc((size_t)content_len > 0 ? (size_t)content_len : 1);
            if (!buf) { fclose(f); return -1; }
            if (content_len > 0) {
                if (is_range) fseek(f, start, SEEK_SET);
                { size_t rd = fread(buf, 1, (size_t)content_len, f); (void)rd; }
            }
            fclose(f);
            chars_t_set(&resp->body, buf, (size_t)content_len);
            free(buf);
        }
    }
    return 0;
}

/* ---- static file route: registers a "GET <prefix>*" route and maps the rest
 * under root_dir with ".." guarded; userdata is the embedded route ctx. ---- */

static void static_file_handler(mco_http_request_t* req, mco_http_response_t* resp, void* userdata) {
    static_route_t* sr = (static_route_t*)userdata;
    const char* rest = req->path + sr->prefix_len; /* relative path */
    char full[4096];
    size_t range_len = 0; /* find_header_value_r leaves it untouched on a miss */
    const char* range_hdr = find_header_value_r(req, "Range", &range_len);
    if (static_resolve_path(sr->root, rest, full, (int)sizeof(full)) != 0) {
        goto not_found;
    }
    if (serve_file_content(resp, full, 200, range_hdr, range_len) != 0) {
        goto not_found;
    }
    return;

not_found:
    /* 404 fallback: root/404.html, else the not-found hook (no Range). */
    {
        char nf[4096];
        if (static_resolve_path(sr->root, "/404.html", nf, (int)sizeof(nf)) == 0 &&
            serve_file_content(resp, nf, 404, NULL, 0) == 0) {
            return;
        }
    }
    if (sr->srv && sr->srv->not_found_handler) sr->srv->not_found_handler(req, resp, NULL);
    else mco_http_resp_html(resp, 404,
        "<html><body><h1>404 Not Found</h1></body></html>");
}

void mco_http_server_serve_static(mco_http_server_t* srv,
                                  const char* prefix, const char* root_dir) {
    static_route_t* sr;
    char pattern[288];
    size_t plen;

    if (!srv || srv->num_static_routes >= MCO_HTTP_MAX_STATIC_ROUTES) return;
    sr = &srv->static_routes[srv->num_static_routes++];
    snprintf(sr->prefix, sizeof(sr->prefix), "%s", prefix ? prefix : "/static/");
    sr->prefix_len = strlen(sr->prefix);
    snprintf(sr->root, sizeof(sr->root), "%s", root_dir ? root_dir : ".");
    sr->srv = srv;

    /* pattern: append "*", adding "/" if needed */
    plen = sr->prefix_len;
    if (plen > 0 && sr->prefix[plen - 1] == '/')
        snprintf(pattern, sizeof(pattern), "%s*", sr->prefix);
    else
        snprintf(pattern, sizeof(pattern), "%s/*", sr->prefix);
    mco_http_server_handle(srv, "GET", pattern, static_file_handler, sr);
}

/* ---- temp file names for multipart ---- */
static void make_tmp_path(char* out, int outcap, int idx) {
    snprintf(out, (size_t)outcap, "/tmp/miniconet_upload_%d_%d.tmp",
             (int)getpid(), idx);
}

/* multipart ctx: on_part_data writes to the current part's file sink */
typedef struct {
    mco_http_request_t* req;
    mco_http_pool_t*    pool;   /* name/filename/content_type come from the pool */
    FILE* cur_file;        /* file of the current part */
    int cur_idx;
    char field_name[128];  /* accumulated Content-Disposition name */
    int  field_name_len;
    char filename[256];    /* Content-Disposition: filename= */
    int  filename_len;
    char ctype[128];       /* Content-Type value */
    int  ctype_len;
    /* pending header field/value accumulation */
    char hf[128]; int hf_len;     /* current header field */
    char hv[256]; int hv_len;     /* current header value */
} mp_ctx_t;

/* commit a (field, value) pair */
static void mp_commit_header(mp_ctx_t* c) {
    if (c->hf_len == 0) return;
    if (strncasecmp(c->hf, "Content-Disposition", c->hf_len) == 0) {
        /* parse name= and filename= */
        const char* p = c->hv;
        const char* end = c->hv + c->hv_len;
        while (p < end) {
            if (strncasecmp(p, "name=\"", 6) == 0) {
                const char* q = (const char*)memchr(p + 6, '"', (size_t)(end - (p + 6)));
                if (q) { int l = (int)(q - (p + 6)); c->field_name_len = l < 127 ? l : 127;
                         memcpy(c->field_name, p + 6, (size_t)c->field_name_len); p = q + 1; continue; }
            }
            if (strncasecmp(p, "filename=\"", 10) == 0) {
                const char* q = (const char*)memchr(p + 10, '"', (size_t)(end - (p + 10)));
                if (q) { int l = (int)(q - (p + 10)); c->filename_len = l < 255 ? l : 255;
                         memcpy(c->filename, p + 10, (size_t)c->filename_len); p = q + 1; continue; }
            }
            p++;
        }
    } else if (strncasecmp(c->hf, "Content-Type", c->hf_len) == 0) {
        c->ctype_len = c->hv_len < 127 ? c->hv_len : 127;
        memcpy(c->ctype, c->hv, (size_t)c->ctype_len);
    }
    c->hf_len = 0; c->hv_len = 0;
}

static int mp_on_header_field(multipart_parser* p, const char* at, size_t len) {
    mp_ctx_t* c = (mp_ctx_t*)multipart_parser_get_data(p);
    /* new field: commit the previous pair first */
    mp_commit_header(c);
    if (c->hf_len + (int)len > (int)sizeof(c->hf)) len = sizeof(c->hf) - c->hf_len;
    memcpy(c->hf + c->hf_len, at, len);
    c->hf_len += (int)len;
    return 0;
}

static int mp_on_header_value(multipart_parser* p, const char* at, size_t len) {
    mp_ctx_t* c = (mp_ctx_t*)multipart_parser_get_data(p);
    if (c->hv_len + (int)len > (int)sizeof(c->hv)) len = sizeof(c->hv) - c->hv_len;
    memcpy(c->hv + c->hv_len, at, len);
    c->hv_len += (int)len;
    return 0;
}

static int mp_on_part_data(multipart_parser* p, const char* at, size_t length) {
    mp_ctx_t* c = (mp_ctx_t*)multipart_parser_get_data(p);
    /* on_headers_complete advanced cur_idx; the current part is cur_idx-1 */
    int cur = c->cur_idx - 1;
    if (cur < 0 || cur >= MCO_HTTP_MAX_PARTS) return 0;
    if (c->cur_file) {
        if (fwrite(at, 1, length, c->cur_file) != length) return -1;
        c->req->parts[cur].size += (long)length;
    }
    return 0;
}

static int mp_on_part_data_end(multipart_parser* p) {
    mp_ctx_t* c = (mp_ctx_t*)multipart_parser_get_data(p);
    if (c->cur_file) {
        fclose(c->cur_file);
        c->cur_file = NULL;
    }
    return 0;
}

static int mp_on_headers_complete(multipart_parser* p) {
    mp_ctx_t* c = (mp_ctx_t*)multipart_parser_get_data(p);
    mp_commit_header(c);  /* commit the last pair */
    if (c->cur_idx >= MCO_HTTP_MAX_PARTS) { c->cur_file = NULL; return 0; }
    mco_http_part_t* part = &c->req->parts[c->cur_idx];
    part->name = c->field_name_len ? mco_pool_str(c->pool, c->field_name, c->field_name_len) : NULL;
    part->filename = c->filename_len ? mco_pool_str(c->pool, c->filename, c->filename_len) : NULL;
    part->content_type = c->ctype_len ? mco_pool_str(c->pool, c->ctype, c->ctype_len) : NULL;
    if ((c->field_name_len && !part->name) ||
        (c->filename_len && !part->filename) ||
        (c->ctype_len && !part->content_type)) return -1;
    part->size = 0;
    /* every part streams to a temp file */
    make_tmp_path(part->saved_path, (int)sizeof(part->saved_path), c->cur_idx);
    c->cur_file = fopen(part->saved_path, "wb");
    if (c->cur_file) setvbuf(c->cur_file, NULL, _IOFBF, 64 * 1024); /* as in save_upload */
    if (!c->cur_file) return -1;
    c->cur_idx++;
    c->req->num_parts = c->cur_idx;
    return 0;
}

/* feed raw body bytes into the multipart parser */
typedef struct {
    multipart_parser* parser;
    mp_ctx_t* ctx;
} mp_sink_ctx_t;

static int mp_sink_write(void* vctx, const char* data, int len) {
    mp_sink_ctx_t* m = (mp_sink_ctx_t*)vctx;
    size_t consumed = multipart_parser_execute(m->parser, data, (size_t)len);
    if (consumed != (size_t)len) return -1; /* parse failed */
    return 0;
}

/* Write the response: normal responses get Content-Length + Connection, chunked
 * ones Transfer-Encoding. Also covers the trailing chunk, an empty chunked body,
 * and splitting a large body from the head. 0 ok, -1 failure. */
static int write_response(mco_socket_t* sock, mco_http_response_t* resp, int timeout_ms) {
    char headbuf[1024];  /* stack-seeded; spills to heap */
    growbuf_t out;
    int n;
    growbuf_init_stack(&out, headbuf, (int)sizeof(headbuf));

    if (resp->_server_headers_sent) {
        /* chunked: add the terminating chunk if the handler forgot it */
        if (resp->_server_chunked) {
            write_all(sock, "0\r\n\r\n", 5, timeout_ms);
            resp->_server_chunked = 0;
        }
        return 0;
    }

    if (resp->_server_chunked) {
        /* chunked with no chunks: head + terminator */
        if (build_response_head(&out, resp, 1) != 0) { growbuf_release(&out); return -1; }
        if (growbuf_ensure(&out, 5) != 0) { growbuf_release(&out); return -1; }
        memcpy(out.data + out.len, "0\r\n\r\n", 5);
        out.len += 5;
        n = write_all(sock, out.data, out.len, timeout_ms);
        growbuf_release(&out);
        resp->_server_chunked = 0;
        return n;
    }

    if (build_response_head(&out, resp, 0) != 0) { growbuf_release(&out); return -1; }

    if (resp->_server_file_path) {
        /* File response: sendfile() after the head (TLS/non-Linux falls back);
         * tcp_nopush coalesces head + data up to an MSS. */
        int fd = mco_socket_fd(sock);
        int use_nopush = !mco_socket_is_ssl(sock);
        if (use_nopush) tcp_nopush(fd, 1);
        n = write_all(sock, out.data, out.len, timeout_ms);
        growbuf_release(&out);
        if (n == 0)
            n = send_file_response(sock, resp->_server_file_path,
                                   resp->_server_file_size, resp->_server_file_off,
                                   timeout_ms);
        if (use_nopush) tcp_nopush(fd, 0);
        return n;
    }

    if (resp->body.size > 0) {
        const char* body = chars_t_cstr(&resp->body);
        int body_len = (int)resp->body.size;
        if (out.len + body_len <= out.cap) {
            /* small body: write it with the head in one go */
            memcpy(out.data + out.len, body, (size_t)body_len);
            out.len += body_len;
            n = write_all(sock, out.data, out.len, timeout_ms);
            growbuf_release(&out);
            return n;
        }
        /* large body: writev head + body, no full memcpy */
        {
            const char* d[2] = { out.data, body };
            int l[2] = { out.len, body_len };
            n = writev_all(sock, d, l, 2, timeout_ms);
            growbuf_release(&out);
            return n;
        }
    }

    n = write_all(sock, out.data, out.len, timeout_ms);
    growbuf_release(&out);
    return n;
}

/* Header value lookup; phr_header gives lengths, not NUL-terminated strings. */
static const char* find_header_value_r(mco_http_request_t* req, const char* name, size_t* out_len) {
    int i;
    for (i = 0; i < req->num_headers; i++) {
        if (mco_http_header_name_eq(req->headers[i].name, req->headers[i].name_len, name)) {
            *out_len = req->headers[i].value_len;
            return req->headers[i].value;
        }
    }
    return NULL;
}

/* Whether the connection closes after this response. explicit_close/keepalive
 * are the Connection header flags gathered by the request's single header scan. */
static int client_wants_close(mco_http_server_t* srv, int explicit_close,
                              int explicit_keepalive, int minor_version) {
    if (!srv->keepalive) return 1;
    if (explicit_close) return 1;
    if (minor_version == 0) return !explicit_keepalive;
    return 0; /* HTTP/1.1 keeps by default */
}

/* Connection read buffer reused across the keep-alive loop; unconsumed bytes
 * (a pipelined request, WS frames) carry over. */
typedef struct {
    char* buf;   /* MCO_HTTP_HEADER_BUF_SIZE, owned by conn_handler */
    int   len;   /* valid bytes in buf */
    int   start; /* start of the current request */
} conn_reader_t;

/* http_conn_t is defined above, before struct mco_http_server. */

/* handle_one_request return values:
 *   1  = keep the connection (keep-alive)
 *   0  = close it (conn_handler owns the socket)
 *  -1  = protocol/io error, also close
 *  -2  = the WS path destroyed the socket; conn_handler must not touch it
 */
#define MCO_HTTP_RET_WS_SOCK_DESTROYED (-2)

/* Handle one request: read header + body, route (or WS), write the response.
 * req/resp are reused; small strings come from the pool and rewind. */
static int handle_one_request(http_conn_t* conn, mco_http_server_t* srv, mco_socket_t* sock,
                              int timeout_ms, conn_reader_t* rd) {
    mco_http_request_t* req = &conn->req;
    mco_http_response_t* resp = &conn->resp;
    size_t num_headers = MCO_HTTP_MAX_HEADERS;
    int pret = 0, minor_version = 1;
    const char* method = NULL; size_t method_len = 0;
    const char* path = NULL;   size_t path_len = 0;
    long content_length = -1;
    int chunked = 0;
    /* Header fields of interest, filled by the single scan below. */
    const char* te_val = NULL;    size_t te_len = 0;
    const char* ctype_val = NULL; size_t ctype_len = 0;
    const char* up_val = NULL;    size_t up_len = 0;
    const char* wskey_val = NULL; size_t wskey_len = 0;
    int conn_close = 0, conn_keepalive = 0;
    const char* leftover = NULL; int leftover_len = 0; int orig_leftover = 0;
    body_reader_t br;
    body_sink_t sink;
    int rc = 0;
    mp_ctx_t mpc;
    multipart_parser* mpp = NULL;
    multipart_parser_settings mpset;
    mp_sink_ctx_t mpsink;
    int is_multipart = 0;
    char boundary[256];
    char* path_dup;
    char body_seed[MCO_HTTP_REQ_BODY_SEED]; /* mco_http_req_body() stack seed */

    memset(req, 0, sizeof(*req));
    memset(resp, 0, offsetof(mco_http_response_t, body));
    resp_body_reset(resp);
    resp->_server_pool = &conn->pool;   /* headers come from the pool */
    memset(&br, 0, sizeof(br));
    conn->mark = mco_pool_mark(&conn->pool);   /* request start; pool rewinds here */

    /* Compact the buffer to offset 0 so parsing and leftover maths stay simple. */
    if (rd->start > 0 && rd->start < rd->len) {
        memmove(rd->buf, rd->buf + rd->start, (size_t)(rd->len - rd->start));
        rd->len -= rd->start;
    }
    rd->start = 0;

    /* Read until phr_parse_request succeeds (continuing from rd->len). */
    for (;;) {
        int r;
        num_headers = MCO_HTTP_MAX_HEADERS;
        pret = phr_parse_request(rd->buf, (size_t)rd->len, &method, &method_len,
                                 &path, &path_len, &minor_version,
                                 req->headers, &num_headers, 0);
        if (pret > 0) break;
        if (pret == -1) return -1;                    /* malformed request */
        if (rd->len >= MCO_HTTP_HEADER_BUF_SIZE) return -1; /* header too large */
        r = mco_read(sock, rd->buf + rd->len, MCO_HTTP_HEADER_BUF_SIZE - rd->len, timeout_ms);
        if (r <= 0) return -1;
        rd->len += r;
    }

    /* the header ends at buf + pret; the rest is body and maybe the next request */
    leftover = rd->buf + pret;
    leftover_len = rd->len - pret;
    orig_leftover = leftover_len;

    /* Headers are parsed in place into rd->buf with lengths, not NUL-terminated;
     * read them with mco_http_header_name_eq + value_len. */
    req->num_headers = (int)num_headers < MCO_HTTP_MAX_HEADERS ? (int)num_headers : MCO_HTTP_MAX_HEADERS;
    req->method = mco_pool_str(&conn->pool, method, method_len);
    if (!req->method) { goto cleanup; }

    /* split path + query (from the pool, truncated if very long) */
    {
        const char* q = (const char*)memchr(path, '?', path_len);
        size_t plen = q ? (size_t)(q - path) : path_len;
        if (plen >= 2048) plen = 2047;
        path_dup = mco_pool_str(&conn->pool, path, plen);
        if (!path_dup) { goto cleanup; }
        req->path = path_dup; /* pool copy, valid for this request */
        if (q) {
            size_t qlen = path_len - plen - 1;
            if (qlen >= 2048) qlen = 2047;
            req->query = mco_pool_str(&conn->pool, q + 1, qlen);
            if (!req->query) { goto cleanup; }
        }
    }

    /* One scan for every header field this request path needs, instead of a
     * separate linear search per field. First occurrence wins, as before. */
    {
        size_t hi;
        const char* cl_val = NULL;
        for (hi = 0; hi < num_headers; hi++) {
            const char* n = req->headers[hi].name;
            size_t nl = req->headers[hi].name_len;
            const char* v = req->headers[hi].value;
            size_t vl = req->headers[hi].value_len;
            if (nl == 14) {
                if (!cl_val && header_name_is(n, nl, "Content-Length", 14)) cl_val = v;
            } else if (nl == 17) {
                if (!te_val && header_name_is(n, nl, "Transfer-Encoding", 17)) { te_val = v; te_len = vl; }
                else if (!wskey_val && header_name_is(n, nl, "Sec-WebSocket-Key", 17)) { wskey_val = v; wskey_len = vl; }
            } else if (nl == 12) {
                if (!ctype_val && header_name_is(n, nl, "Content-Type", 12)) { ctype_val = v; ctype_len = vl; }
            } else if (nl == 7) {
                if (!up_val && header_name_is(n, nl, "Upgrade", 7)) { up_val = v; up_len = vl; }
            } else if (nl == 10) {
                if (header_name_is(n, nl, "Connection", 10)) {
                    if (mem_contains_ci(v, vl, "close")) conn_close = 1;
                    if (mem_contains_ci(v, vl, "keep-alive")) conn_keepalive = 1;
                }
            }
        }
        if (cl_val) content_length = strtol(cl_val, NULL, 10);
    }
    if (content_length > 0x7FFFFFFF) return -1; /* truncation would skip the body */
    if (te_val && te_len >= 7 && strncasecmp(te_val, "chunked", 7) == 0) chunked = 1;
    if (ctype_val && ctype_len >= 19 && strncasecmp(ctype_val, "multipart/form-data", 19) == 0) {
        if (extract_multipart_boundary(ctype_val, ctype_len, boundary, (int)sizeof(boundary)) == 0) {
            is_multipart = 1;
        }
    }

    /* pick the body strategy; leftover bytes of the next request are preserved */
    if (is_multipart) {
        memset(&mpc, 0, sizeof(mpc));
        mpc.req = req;
        mpc.pool = &conn->pool;
        memset(&mpset, 0, sizeof(mpset));
        mpset.on_header_field = mp_on_header_field;
        mpset.on_header_value  = mp_on_header_value;
        mpset.on_part_data = mp_on_part_data;
        mpset.on_headers_complete = mp_on_headers_complete;
        mpset.on_part_data_end = mp_on_part_data_end;
        mpp = multipart_parser_init(boundary, &mpset);
        if (!mpp) { goto cleanup; }
        multipart_parser_set_data(mpp, &mpc);
        mpsink.parser = mpp;
        mpsink.ctx = &mpc;
        sink.write = mp_sink_write;
        sink.ctx = &mpsink;
        if (chunked) rc = read_body_chunked(sock, &sink, &leftover, &leftover_len, timeout_ms, conn->scratch);
        else if (content_length >= 0) rc = read_body_content_length(sock, &sink, content_length, &leftover, &leftover_len, timeout_ms, conn->scratch);
        else rc = 0;
        if (mpp) { multipart_parser_free(mpp); mpp = NULL; }
        if (rc != 0) {
            /* close the part file before cleanup to avoid an fd leak */
            if (mpc.cur_file) { fclose(mpc.cur_file); mpc.cur_file = NULL; }
            goto cleanup;
        }
    } else {
        /* lazy: only attach the io state; a drain empties an unconsumed body */
        br.sock = sock;
        br.leftover = &leftover;
        br.leftover_len = &leftover_len;
        br.timeout_ms = timeout_ms;
        br.is_chunked = chunked;
        br.content_length = content_length;
        br.consumed = 0;
        br.scratch = conn->scratch;   /* connection scratch */
        br.body_stack = body_seed;    /* seed for mco_http_req_body() */
        br.body_stack_cap = (int)sizeof(body_seed);
        br.body_on_stack = 0;
        req->_bodyio = &br;
    }

    /* ---- WebSocket upgrade takes priority over HTTP routes ---- */
    if (up_val && up_len >= 9 && strncasecmp(up_val, "websocket", 9) == 0) {
        const mco_ws_route_t* wr = ws_route_find(srv, req->method, req->path);
        if (wr && wskey_val) {
            char keybuf[256];
            size_t keylen = wskey_len;
            if (keylen >= sizeof(keybuf)) keylen = sizeof(keybuf) - 1;
            memcpy(keybuf, wskey_val, keylen);
            keybuf[keylen] = 0;
            /* handshake (replies 101); leftover may hold the first frame */
            mco_ws_t* ws = mco_ws_accept(sock, keybuf, leftover, leftover_len);
            if (ws) {
                wr->handler(ws, req, wr->userdata);
                mco_ws_close(ws);
                mco_ws_destroy(ws); /* frees the socket */
                /* socket already destroyed: conn_handler must not touch it */
                goto cleanup_close;
            }
            /* handshake failed: the socket is still ours, close it */
            goto cleanup;
        }
    }

    /* route dispatch */
    {
        int want_keepalive = !client_wants_close(srv, conn_close, conn_keepalive, minor_version);
        /* attach the connection ctx to resp */
        resp->_server_sock = sock;
        resp->_server_chunked = 0;
        resp->_server_keepalive = want_keepalive;

        mco_http_route_t* route = route_find(srv, req->method, req->path, req);
        if (!route) {
            if (srv->not_found_handler) srv->not_found_handler(req, resp, NULL);
            else mco_http_resp_html(resp, 404,
                "<html><body><h1>404 Not Found</h1></body></html>");
        } else {
            route->handler(req, resp, route->userdata);
        }

        /* A handler that returns without producing a response (status still 0)
         * has failed: run the error hook, else the built-in 500 page. Without
         * this the status line would be malformed ("HTTP/1.1 0 OK"). */
        if (resp->status == 0) {
            if (srv->error_handler) srv->error_handler(req, resp, NULL);
            if (resp->status == 0) {
                mco_http_resp_html(resp, 500,
                    "<html><body><h1>500 Internal Server Error</h1></body></html>");
            }
        }

        /* drain an unconsumed body so the buffer stays aligned */
        if (!is_multipart && !br.consumed) {
            body_sink_t dsink;
            dsink.write = null_sink_write;
            dsink.ctx = NULL;
            body_read(&br, &dsink);
            br.consumed = 1;
        }

        /* write the response (chunked or not per _server_chunked) */
        write_response(sock, resp, timeout_ms);
        /* with chunked the terminator is already sent */
        int keep = want_keepalive ? 1 : 0;

        /* leftover carries the pipelined next request */
        rd->start = pret + (orig_leftover - leftover_len);
        if (rd->start >= rd->len) { rd->start = 0; rd->len = 0; }

        /* Cleanup: pool strings die on rewind; free only the heap objects (the
         * lazily read body, the response body and its dup'ed headers). */
        if (!br.body_on_stack) free((void*)req->body);
        free_response_members(resp);
        mco_pool_reset(&conn->pool, conn->mark);   /* rewind the pool */
        return keep;
    }

cleanup_close:
    /* WS: socket gone, only clean up req/resp */
    if (!br.body_on_stack) free((void*)req->body);
    free_response_members(resp);
    return MCO_HTTP_RET_WS_SOCK_DESTROYED;

cleanup:
    /* parse/alloc failure: free the heap body and resp; conn_handler frees the rest */
    if (!br.body_on_stack) free((void*)req->body);
    free_response_members(resp);
    return -1;
}

/* Per-connection coroutine: keep-alive loop over handle_one_request. arg is a
 * conn_arg_t (srv + sock) whose ownership is taken and freed here. */
static void conn_handler(void* arg) {
    conn_arg_t* ca = (conn_arg_t*)arg;
    mco_http_server_t* srv = ca->srv;
    mco_socket_t* sock = ca->sock;
    conn_reader_t rd;
    http_conn_t* conn;
    char rdbuf[MCO_HTTP_HEADER_BUF_SIZE]; /* read buffer on the coroutine stack */
    int timeout_ms = 10000;
    int r = 1;

    free(ca);

    /* HTTPS: handshake first via mco_wait_io, without blocking accept. */
    if (srv->ssl_ctx &&
        mco_ssl_server_handshake(sock, srv->ssl_ctx, 10000) != 0) {
        mco_close(sock);
        mco_socket_destroy(sock);
        return;
    }

    rd.buf = rdbuf;
    rd.len = 0; rd.start = 0;

    /* Reuse a recycled connection ctx (pool + scratch already allocated). */
    if (srv->num_conn_free > 0) {
        conn = srv->conn_free;
        srv->conn_free = conn->free_next;
        srv->num_conn_free--;
        mco_pool_init(&conn->pool, conn->pool.base, conn->pool.cap);
    } else {
        conn = (http_conn_t*)calloc(1, sizeof(*conn));
        if (!conn) { mco_close(sock); mco_socket_destroy(sock); return; }
        conn->pool.base = (char*)malloc(MCO_HTTP_POOL_CAP);
        conn->scratch = (char*)malloc(MCO_HTTP_BODY_CHUNK);
        if (!conn->pool.base || !conn->scratch) {
            free(conn->pool.base); free(conn->scratch); free(conn);
            mco_close(sock); mco_socket_destroy(sock);
            return;
        }
        conn->pool.cap = MCO_HTTP_POOL_CAP;
        mco_pool_init(&conn->pool, conn->pool.base, conn->pool.cap);
        chars_t_init(&conn->resp.body, 0);
    }

    while (r > 0) {
        r = handle_one_request(conn, srv, sock, timeout_ms, &rd);
    }

    if (srv->num_conn_free < MCO_HTTP_CONN_FREE_MAX) {
        conn->free_next = srv->conn_free;
        srv->conn_free = conn;
        srv->num_conn_free++;
    } else {
        free(conn->pool.base);            /* pool memory */
        free(conn->scratch);              /* body scratch */
        chars_t_cleanup(&conn->resp.body); /* response body buffer */
        free(conn);                       /* connection ctx */
    }
    if (r != MCO_HTTP_RET_WS_SOCK_DESTROYED) {
        /* -2: the WS path freed the socket */
        mco_close(sock);
        mco_socket_destroy(sock);
    }
}

static void serve_loop(void* arg) {
    serve_arg_t* sa = (serve_arg_t*)arg;
    mco_http_server_t* srv = sa->srv;
    mco_socket_t* listener = sa->l;
    mco_loop_t* loop = sa->loop;
    free(sa);
    for (;;) {
        mco_socket_t* c = mco_tcp_accept(listener, 500);
        if (!c) {
            if (srv->stop) break; /* graceful stop */
            /* mco_tcp_accept() maps both a timeout and a hard error (e.g. EMFILE)
             * to NULL; without a pause the error case would spin at 100% CPU. */
            mco_sleep_ms(1);
            continue;
        }
        /* drain the backlog in a batch (later accepts use a 0 timeout) */
        do {
            conn_arg_t* ca = (conn_arg_t*)malloc(sizeof(conn_arg_t));
            if (!ca) { mco_close(c); mco_socket_destroy(c); break; }
            ca->srv = srv;
            ca->sock = c;
            /* disable Nagle (as nginx does); fails harmlessly on UDS */
            tcp_nodelay(mco_socket_fd(c), 1);
            mco_spawn(loop, conn_handler, ca);
            if (srv->stop) break;
            c = mco_tcp_accept(listener, 0);
        } while (c);
    }
    /* close the listener on exit */
    mco_close(listener);
    mco_socket_destroy(listener);
}

/* graceful stop: serve_loop exits at the next poll */
void mco_http_server_stop(mco_http_server_t* srv) {
    if (srv) srv->stop = 1;
}

/* Call before mco_loop_run(): spawns the accept coroutine and returns. May be
 * called once per listener. */
void mco_http_server_serve(mco_http_server_t* srv,
                           mco_socket_t* listener, mco_loop_t* loop) {
    serve_arg_t* sa;
    if (!srv) return;
    sa = (serve_arg_t*)malloc(sizeof(serve_arg_t));
    if (!sa) return;
    sa->l = listener;
    sa->loop = loop;
    sa->srv = srv;
    mco_spawn(loop, serve_loop, sa);
}
