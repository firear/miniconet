#include "miniconet_http.h"
#include "picohttpparser.h"
#include "compat/hbase.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define MCO_HTTP_HEADER_BUF_SIZE 8192  /* cap on the header part */
#define MCO_HTTP_BODY_CHUNK      8192  /* body buffer growth step */

/* ---- URL parsing: reuses hv_parse_url() from compat/hbase.c ---- */

static void url_field_copy(const char* url, hurl_t* u, hurl_field_e f, char* out, int outcap) {
    int len = u->fields[f].len;
    if (len < 0) len = 0;
    if (len >= outcap) len = outcap - 1;
    if (len > 0) memcpy(out, url + u->fields[f].off, (size_t)len);
    out[len] = 0;
}

/* ---- case-insensitive header name compare ---- */
static int header_name_eq(const char* name, size_t name_len, const char* target) {
    size_t tlen = strlen(target);
    size_t i;
    if (name_len != tlen) return 0;
    for (i = 0; i < tlen; i++) {
        if (tolower((unsigned char)name[i]) != tolower((unsigned char)target[i])) return 0;
    }
    return 1;
}

const char* mco_http_response_get_header(mco_http_response_t* resp, const char* name) {
    int i;
    if (!resp) return NULL;
    for (i = 0; i < resp->num_headers; i++) {
        if (header_name_eq(resp->headers[i].name, strlen(resp->headers[i].name), name)) {
            return resp->headers[i].value;
        }
    }
    return NULL;
}

void mco_http_response_free(mco_http_response_t* resp) {
    int i;
    if (!resp) return;
    for (i = 0; i < resp->num_headers; i++) {
        free(resp->headers[i].name);
        free(resp->headers[i].value);
    }
    free(resp->headers);
    chars_t_cleanup(&resp->body);
    free(resp);
}

/* ---- body read: Content-Length / chunked / until EOF ---- */

/* ---- body sink: read_body_* only feeds raw bytes to a sink ---- */
typedef struct {
    int (*write)(void* ctx, const char* data, int len);
    void* ctx;
} body_sink_t;

/* Memory sink: accumulates into the chars_t used by the response. */
static int chars_sink_write(void* ctx, const char* data, int len) {
    chars_t* b = (chars_t*)ctx;
    if (len <= 0) return 0;
    chars_t_append(b, data, (size_t)len);
    return 0;
}

/* Read exactly Content-Length bytes. leftover is the body prefix already in the
 * read buffer. */
static int read_body_content_length(mco_socket_t* sock, body_sink_t* sink, long content_length,
                                     const char* leftover, int leftover_len, mco_deadline_t dl) {
    int remaining = (int)content_length;
    if (leftover_len > 0) {
        int take = leftover_len < remaining ? leftover_len : remaining;
        if (sink->write(sink->ctx, leftover, take) != 0) return MCO_ERR_IO;
        remaining -= take;
    }
    while (remaining > 0) {
        char tmp[MCO_HTTP_BODY_CHUNK];
        int want = remaining < (int)sizeof(tmp) ? remaining : (int)sizeof(tmp);
        int n = mco_read(sock, tmp, want, mco_deadline_ms(dl));
        if (n <= 0) return (n == 0) ? MCO_ERR_CLOSED : n;
        if (sink->write(sink->ctx, tmp, n) != 0) return MCO_ERR_IO;
        remaining -= n;
    }
    return 0;
}

/* Chunked decoding: "<hex>\r\n<data>\r\n", a 0-length chunk ends it. */
static int read_body_chunked(mco_socket_t* sock, body_sink_t* sink,
                              const char* leftover, int leftover_len, mco_deadline_t dl) {
    char inbuf[MCO_HTTP_BODY_CHUNK];
    int in_len = 0;

    if (leftover_len > 0) {
        if (leftover_len > (int)sizeof(inbuf)) return MCO_ERR_IO; /* too big */
        memcpy(inbuf, leftover, (size_t)leftover_len);
        in_len = leftover_len;
    }

    for (;;) {
        char* line_end;
        long chunk_size;
        int header_len;

        /* ensure a full chunk-size line in inbuf */
        for (;;) {
            int i;
            line_end = NULL;
            for (i = 0; i + 1 < in_len; i++) {
                if (inbuf[i] == '\r' && inbuf[i + 1] == '\n') { line_end = inbuf + i; break; }
            }
            if (line_end) break;
            if (in_len >= (int)sizeof(inbuf)) return MCO_ERR_IO; /* line too long */
            {
                int n = mco_read(sock, inbuf + in_len, (int)sizeof(inbuf) - in_len, mco_deadline_ms(dl));
                if (n <= 0) return (n == 0) ? MCO_ERR_CLOSED : n;
                in_len += n;
            }
        }

        chunk_size = strtol(inbuf, NULL, 16);
        header_len = (int)(line_end - inbuf) + 2; /* includes the trailing \r\n */

        if (chunk_size < 0) return MCO_ERR_IO; /* guard against a negative length */
        if (chunk_size == 0) return 0; /* terminating chunk; trailers ignored */

        /* ensure chunk_size + 2 bytes are in inbuf */
        while (in_len - header_len < chunk_size + 2) {
            if (in_len >= (int)sizeof(inbuf)) {
                /* inbuf full: flush what we have to the sink */
                int can_emit = in_len - header_len;
                if (can_emit > 0) {
                    if (sink->write(sink->ctx, inbuf + header_len, can_emit) != 0) return MCO_ERR_IO;
                    chunk_size -= can_emit;
                    in_len = 0; header_len = 0;
                } else {
                    return MCO_ERR_IO; /* chunk larger than inbuf */
                }
            }
            {
                int n = mco_read(sock, inbuf + in_len, (int)sizeof(inbuf) - in_len, mco_deadline_ms(dl));
                if (n <= 0) return (n == 0) ? MCO_ERR_CLOSED : n;
                in_len += n;
            }
        }

        if (sink->write(sink->ctx, inbuf + header_len, (int)chunk_size) != 0) return MCO_ERR_IO;

        /* drop this chunk (with \r\n) from inbuf */
        {
            int consumed = header_len + (int)chunk_size + 2;
            int remain = in_len - consumed;
            if (remain > 0) memmove(inbuf, inbuf + consumed, (size_t)remain);
            in_len = remain;
        }
    }
}

/* No Content-Length and not chunked: read until the peer closes. */
static int read_body_until_eof(mco_socket_t* sock, body_sink_t* sink,
                                const char* leftover, int leftover_len, mco_deadline_t dl) {
    if (leftover_len > 0) {
        if (sink->write(sink->ctx, leftover, leftover_len) != 0) return MCO_ERR_IO;
    }
    for (;;) {
        char tmp[MCO_HTTP_BODY_CHUNK];
        int n = mco_read(sock, tmp, (int)sizeof(tmp), mco_deadline_ms(dl));
        if (n < 0) return n;
        if (n == 0) return 0; /* EOF: done */
        if (sink->write(sink->ctx, tmp, n) != 0) return MCO_ERR_IO;
    }
}

/* ---- shared request logic for all get/post variants ---- */

mco_http_response_t* mco_http_request(const mco_http_options_t* opt) {
    mco_deadline_t dl = mco_deadline_after(opt->timeout_ms);
    hurl_t u;
    char host[256], path[1024], query[512];
    int is_https;
    mco_socket_t* sock;
    char req[2048];
    int req_len, n;
    char resp_buf[MCO_HTTP_HEADER_BUF_SIZE]; /* 8KB on the coroutine stack */
    int resp_len = 0;
    struct phr_header phrheaders[MCO_HTTP_MAX_HEADERS];
    size_t num_headers;
    int minor_version, status;
    const char* msg;
    size_t msg_len;
    int pret;
    long content_length = -1;
    int chunked = 0;
    int i;
    chars_t bodybuf;
    body_sink_t sink;
    int rc;
    mco_http_response_t* resp;
    const char* method = opt->method ? opt->method : "GET";

    if (!opt->url || hv_parse_url(&u, opt->url) != 0) return NULL;
    is_https = (strncmp(opt->url, "https://", 8) == 0);
    url_field_copy(opt->url, &u, HV_URL_HOST, host, sizeof(host));
    if (host[0] == 0) return NULL; /* no host: malformed url */
    url_field_copy(opt->url, &u, HV_URL_PATH, path, sizeof(path));
    if (path[0] == 0) { path[0] = '/'; path[1] = 0; } /* no path means "/" */
    url_field_copy(opt->url, &u, HV_URL_QUERY, query, sizeof(query));

    if (is_https) {
        sock = mco_tls_connect(host, u.port, opt->ctx, mco_deadline_ms(dl), NULL);
    } else {
        sock = mco_tcp_connect(host, u.port, mco_deadline_ms(dl), NULL);
    }
    if (!sock) return NULL;

    if (opt->body && opt->body_len > 0) {
        req_len = snprintf(req, sizeof(req),
            "%s %s%s%s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Connection: close\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %d\r\n",
            method, path, query[0] ? "?" : "", query, host,
            opt->content_type ? opt->content_type : "application/octet-stream", opt->body_len);
    } else {
        req_len = snprintf(req, sizeof(req),
            "%s %s%s%s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Connection: close\r\n",
            method, path, query[0] ? "?" : "", query, host);
    }
    /* extra headers, one per line "name: value\r\n" */
    if (opt->extra_headers && opt->num_extra_headers > 0) {
        for (i = 0; i < opt->num_extra_headers; i++) {
            int extra = snprintf(req + req_len, (size_t)(sizeof(req) - req_len),
                                 "%s: %s\r\n",
                                 opt->extra_headers[i].name, opt->extra_headers[i].value);
            if (extra < 0) { mco_close(sock); mco_socket_destroy(sock); return NULL; }
            req_len += extra;
            if (req_len >= (int)sizeof(req)) { mco_close(sock); mco_socket_destroy(sock); return NULL; }
        }
    }
    if (req_len < 0 || req_len >= (int)sizeof(req) - 2) {
        mco_close(sock); mco_socket_destroy(sock); return NULL;
    }
    req_len += snprintf(req + req_len, (size_t)(sizeof(req) - req_len), "\r\n");
    if (req_len < 0 || req_len >= (int)sizeof(req)) { mco_close(sock); mco_socket_destroy(sock); return NULL; }

    n = mco_write_all(sock, req, req_len, mco_deadline_ms(dl));
    if (n != req_len) { mco_close(sock); mco_socket_destroy(sock); return NULL; }
    if (opt->body && opt->body_len > 0) {
        n = mco_write_all(sock, opt->body, opt->body_len, mco_deadline_ms(dl));
        if (n != opt->body_len) { mco_close(sock); mco_socket_destroy(sock); return NULL; }
    }

    for (;;) {
        int r = mco_read(sock, resp_buf + resp_len, MCO_HTTP_HEADER_BUF_SIZE - resp_len, mco_deadline_ms(dl));
        if (r <= 0) { mco_close(sock); mco_socket_destroy(sock); return NULL; }
        resp_len += r;

        num_headers = MCO_HTTP_MAX_HEADERS;
        pret = phr_parse_response(resp_buf, (size_t)resp_len, &minor_version, &status,
                                   &msg, &msg_len, phrheaders, &num_headers, 0);
        if (pret > 0) break;
        if (pret == -1) { mco_close(sock); mco_socket_destroy(sock); return NULL; }
        if (resp_len >= MCO_HTTP_HEADER_BUF_SIZE) { mco_close(sock); mco_socket_destroy(sock); return NULL; }
    }

    for (i = 0; i < (int)num_headers; i++) {
        if (header_name_eq(phrheaders[i].name, phrheaders[i].name_len, "Content-Length")) {
            char tmp[32];
            int len = phrheaders[i].value_len < (int)sizeof(tmp) - 1 ? (int)phrheaders[i].value_len : (int)sizeof(tmp) - 1;
            memcpy(tmp, phrheaders[i].value, (size_t)len);
            tmp[len] = 0;
            content_length = strtol(tmp, NULL, 10);
        } else if (header_name_eq(phrheaders[i].name, phrheaders[i].name_len, "Transfer-Encoding")) {
            if (phrheaders[i].value_len >= 7 &&
                strncasecmp(phrheaders[i].value, "chunked", 7) == 0) {
                chunked = 1;
            }
        }
    }

    sink.write = chars_sink_write;
    sink.ctx = &bodybuf;
    chars_t_init(&bodybuf, 0); /* must init: an all-zero chars_t is invalid */
    {
        const char* leftover = resp_buf + pret;
        int leftover_len = resp_len - pret;
        if (chunked) {
            rc = read_body_chunked(sock, &sink, leftover, leftover_len, dl);
        } else if (content_length >= 0) {
            if (content_length > 0x7FFFFFFF) { /* truncation would skip the body */
                mco_close(sock); mco_socket_destroy(sock);
                chars_t_cleanup(&bodybuf);
                return NULL;
            }
            rc = read_body_content_length(sock, &sink, content_length, leftover, leftover_len, dl);
        } else {
            rc = read_body_until_eof(sock, &sink, leftover, leftover_len, dl);
        }
    }
    if (rc != 0) { chars_t_cleanup(&bodybuf); mco_close(sock); mco_socket_destroy(sock); return NULL; }

    resp = (mco_http_response_t*)calloc(1, sizeof(mco_http_response_t));
    if (!resp) { chars_t_cleanup(&bodybuf); mco_close(sock); mco_socket_destroy(sock); return NULL; }
    chars_t_init(&resp->body, 0);
    resp->status = status;
    resp->num_headers = (int)num_headers < MCO_HTTP_MAX_HEADERS ? (int)num_headers : MCO_HTTP_MAX_HEADERS;
    resp->_headers_cap = resp->num_headers > 0 ? resp->num_headers : 1;
    resp->headers = (mco_http_header_pair_t*)calloc((size_t)resp->_headers_cap, sizeof(*resp->headers));
    if (resp->num_headers > 0 && !resp->headers) {
        mco_http_response_free(resp); chars_t_cleanup(&bodybuf);
        mco_close(sock); mco_socket_destroy(sock); return NULL;
    }
    for (i = 0; i < resp->num_headers; i++) {
        resp->headers[i].name = (char*)malloc(phrheaders[i].name_len + 1);
        resp->headers[i].value = (char*)malloc(phrheaders[i].value_len + 1);
        if (!resp->headers[i].name || !resp->headers[i].value) {
            mco_http_response_free(resp); chars_t_cleanup(&bodybuf);
            mco_close(sock); mco_socket_destroy(sock); return NULL;
        }
        memcpy(resp->headers[i].name, phrheaders[i].name, phrheaders[i].name_len);
        resp->headers[i].name[phrheaders[i].name_len] = 0;
        memcpy(resp->headers[i].value, phrheaders[i].value, phrheaders[i].value_len);
        resp->headers[i].value[phrheaders[i].value_len] = 0;
    }
    /* move the whole chars_t over */
    resp->body = bodybuf;
    chars_t_init(&bodybuf, 0); /* ownership moved: reset the local to empty */

    mco_close(sock);
    mco_socket_destroy(sock);
    return resp;
}

mco_http_response_t* mco_http_get(const char* url, int timeout_ms) {
    mco_http_options_t opt;
    memset(&opt, 0, sizeof(opt));
    opt.method = "GET";
    opt.url = url;
    opt.timeout_ms = timeout_ms;
    return mco_http_request(&opt);
}

mco_http_response_t* mco_http_post(const char* url, const void* data, int data_len,
                                    const char* content_type, int timeout_ms) {
    mco_http_options_t opt;
    memset(&opt, 0, sizeof(opt));
    opt.method = "POST";
    opt.url = url;
    opt.body = data;
    opt.body_len = data_len;
    opt.content_type = content_type;
    opt.timeout_ms = timeout_ms;
    return mco_http_request(&opt);
}

mco_http_response_t* mco_http_get_ex(const char* url, mco_ssl_ctx_t ctx, int timeout_ms) {
    mco_http_options_t opt;
    memset(&opt, 0, sizeof(opt));
    opt.method = "GET";
    opt.url = url;
    opt.ctx = ctx;
    opt.timeout_ms = timeout_ms;
    return mco_http_request(&opt);
}

mco_http_response_t* mco_http_post_ex(const char* url, const void* data, int data_len,
                                       const char* content_type, mco_ssl_ctx_t ctx, int timeout_ms) {
    mco_http_options_t opt;
    memset(&opt, 0, sizeof(opt));
    opt.method = "POST";
    opt.url = url;
    opt.body = data;
    opt.body_len = data_len;
    opt.content_type = content_type;
    opt.ctx = ctx;
    opt.timeout_ms = timeout_ms;
    return mco_http_request(&opt);
}
