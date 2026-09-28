/*
 * miniconet_http.h — coroutine-based HTTP client (one call returns the whole
 * result, not a streaming callback API). Only built with
 * MINICONET_WITH_HTTP_PARSER=ON (target miniconet2_http_client).
 * GET/POST, Content-Length and chunked handled automatically, https:// uses TLS.
 * No redirect handling (3xx bodies are returned as-is); the body is buffered in
 * memory, so this is not for large downloads.
 */
#ifndef MINICONET_HTTP_H
#define MINICONET_HTTP_H

#include "miniconet.h"
#include "compat/wrapper/charry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MCO_HTTP_MAX_HEADERS 32

/* Header pair (server: from the connection pool; client: heap array). */
typedef struct { char* name; char* value; } mco_http_header_pair_t;

/* Layout: pointers and 8-byte members first to avoid padding holes. body must
 * stay last so the preceding fields can be memset. */
typedef struct mco_http_response {
    mco_http_header_pair_t* headers; /* dynamic array, see _server_pool */
    mco_socket_t* _server_sock;      /* used by write_response / write_chunk */
    /* Connection pool: when non-NULL the headers array and its strings come from
     * it and are reused per request (do not free individually). NULL on the
     * client, where headers is heap-owned by mco_http_response_free. */
    void*         _server_pool;
    /* Set by mco_http_resp_send_file(): write_response emits Content-Length from
     * _server_file_size and sendfile()s it without reading into user memory.
     * Always NULL on the client. */
    char*         _server_file_path;  /* strdup'ed path, freed with the request */
    long long     _server_file_size;  /* bytes to send (range length for Range) */
    long long     _server_file_off;   /* sendfile offset (Range/206, else 0) */

    int status;
    int num_headers;
    int _headers_cap;    /* allocated slots; grows in the server pool, no 32 cap */

    /* ---- server-only fields (ignored by the client) ---- */
    uint8_t       _server_chunked;   /* set by mco_http_resp_set_chunked() */
    uint8_t       _server_headers_sent; /* chunked header already sent with the first chunk */
    uint8_t       _server_keepalive; /* keep the connection after the handler */

    chars_t body;
} mco_http_response_t;

/* ---- Generic request entry (options struct) ----
 * mco_http_get/post are wrappers; fill options directly for custom headers. */
typedef struct {
    const char* name;
    const char* value;
} mco_http_header_t;

typedef struct {
    const char* method;          /* "GET"/"POST"/...; NULL means GET */
    const char* url;
    const void* body;            /* POST body; NULL for GET */
    int         body_len;
    const char* content_type;    /* NULL = application/octet-stream */
    mco_ssl_ctx_t ctx;           /* TLS ctx for https, ignored for http */
    int         timeout_ms;
    const mco_http_header_t* extra_headers; /* may be NULL */
    int         num_extra_headers;
} mco_http_options_t;

/* Generic entry: covers all get/post variants plus custom headers. */
mco_http_response_t* mco_http_request(const mco_http_options_t* opt);

/* NULL on connect/handshake/timeout/parse failure. HTTP-level errors (4xx/5xx)
 * are not failures: a response is returned with the real status. */
mco_http_response_t* mco_http_get(const char* url, int timeout_ms);
mco_http_response_t* mco_http_post(const char* url, const void* data, int data_len,
                                    const char* content_type, int timeout_ms);

/* As get/post but with an explicit TLS ctx. NULL = no verification; ignored for
 * http:// URLs. */
mco_http_response_t* mco_http_get_ex(const char* url, mco_ssl_ctx_t ctx, int timeout_ms);
mco_http_response_t* mco_http_post_ex(const char* url, const void* data, int data_len,
                                       const char* content_type, mco_ssl_ctx_t ctx, int timeout_ms);

/* Case-insensitive header lookup; NULL if absent. */
const char* mco_http_response_get_header(mco_http_response_t* resp, const char* name);

void mco_http_response_free(mco_http_response_t* resp);

#ifdef __cplusplus
}
#endif
#endif /* MINICONET_HTTP_H */
