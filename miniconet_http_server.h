/*
 * miniconet_http_server.h — coroutine-based HTTP server (one coroutine per
 * connection). Only built with MINICONET_WITH_HTTP_PARSER=ON, no HTTP/2.
 * Routing, multipart upload to disk, keep-alive, chunked, static files with a
 * single range, 404/500 hooks and WebSocket upgrade. The response type is shared
 * with miniconet_http.h; mco_http_request_t is defined here.
 */

#ifndef MINICONET_HTTP_SERVER_H
#define MINICONET_HTTP_SERVER_H

#include "miniconet.h"
#include "miniconet_http.h"       /* mco_http_response_t */
#include "miniconet_websocket.h"  /* mco_ws_t for the ws handler */
#include "picohttpparser.h"       /* struct phr_header: headers used in place */

#ifdef __cplusplus
extern "C" {
#endif

#define MCO_HTTP_MAX_PARAMS   8
#define MCO_HTTP_MAX_PARTS    16   /* max parts per multipart request */

/* One multipart part: data is streamed to saved_path, so uploads cost no memory. */
typedef struct {
    char* name;          /* form field name */
    char* filename;      /* original filename, NULL for plain fields */
    char* content_type;  /* part Content-Type, may be NULL */
    char  saved_path[256]; /* temp path written by the file sink */
    long  size;          /* part size */
} mco_http_part_t;

/* A parsed request. Fields point into the request buffer; the library frees
 * everything when the coroutine exits. */
typedef struct {
    const char* method;          /* "GET"/"POST"/... */
    const char* path;            /* path without the query */
    const char* query;           /* after '?', may be NULL */
    /* Body (non-multipart only), read lazily via mco_http_req_body or
     * mco_http_req_save_upload. */
    const char* body;
    int  body_len;
    int num_headers;
    int num_params;
    int num_parts;
    /* internal io handle for lazy body reads */
    void* _bodyio;
    /* Headers point into the read buffer (no copy); name/value carry a length
     * and are not NUL-terminated — use mco_http_header_name_eq. */
    struct phr_header headers[MCO_HTTP_MAX_HEADERS];
    /* :param values extracted by the route */
    struct {
        const char* name;
        const char* value;
    } params[MCO_HTTP_MAX_PARAMS];
    /* multipart parts; num_parts==0 for other requests */
    mco_http_part_t parts[MCO_HTTP_MAX_PARTS];
} mco_http_request_t;

/* Handler, called in its own coroutine. */
typedef void (*mco_http_handler_t)(mco_http_request_t* req,
                                   mco_http_response_t* resp, void* userdata);

/* Case-insensitive header name compare; target must be NUL-terminated. */
int mco_http_header_name_eq(const char* name, size_t name_len, const char* target);

/* ---- server instance (several may run in one process) ----
 * Routes, hooks, the keep-alive switch and state all live on the instance. */
typedef struct mco_http_server mco_http_server_t;

/* new returns NULL on failure. Before free the server must be stopped with all
 * connection coroutines finished. */
mco_http_server_t* mco_http_server_new(void);
void mco_http_server_free(mco_http_server_t* srv);

/* Register a route: method (NULL = any) + pattern with :param support; a repeat
 * overwrites. */
void mco_http_server_handle(mco_http_server_t* srv, const char* method,
                            const char* path_pattern,
                            mco_http_handler_t handler, void* userdata);

/* overridable default hooks */
void mco_http_server_set_not_found_handler(mco_http_server_t* srv,
                                           mco_http_handler_t h); /* no match -> 404 */
void mco_http_server_set_error_handler(mco_http_server_t* srv,
                                       mco_http_handler_t h);   /* handler left status 0 -> 500 */

/* Master keep-alive switch (default on); a client "Connection: close" wins. */
void mco_http_server_set_keepalive(mco_http_server_t* srv, int enabled);

/* Serve on a listening socket; call before mco_loop_run(). Spawns the accept
 * coroutine and returns. May be called more than once (several ports). */
void mco_http_server_serve(mco_http_server_t* srv,
                           mco_socket_t* listener, mco_loop_t* loop);

/* Attach a server TLS ctx to serve HTTPS; NULL restores plaintext. Call before
 * serve(). */
void mco_http_server_set_tls(mco_http_server_t* srv, mco_ssl_ctx_t ctx);

/* Graceful stop: serve_loop exits after the next accept poll. */
void mco_http_server_stop(mco_http_server_t* srv);

void mco_http_response_add_header(mco_http_response_t* resp, const char* name, const char* value);

/* Build a plain-text response (copied into resp->body). */
void mco_http_resp_text(mco_http_response_t* resp, int status, const char* text);

/* ---- request body (lazy) ---- */

/* Read the body into memory. Idempotent; the library frees it. NULL if it was
 * already streamed to disk or the read failed. */
const char* mco_http_req_body(mco_http_request_t* req, int* out_len);

/* Stream the body to path with constant memory. 0 on success, -1 on failure.
 * The body can be consumed once: this or mco_http_req_body, not both. */
int mco_http_req_save_upload(mco_http_request_t* req, const char* path);

/* ---- keep-alive / chunked / static files / error pages ---- */

/* Mark the response as chunked; call before writing any body, then use
 * mco_http_resp_write_chunk() and finish with NULL/0. */
void mco_http_resp_set_chunked(mco_http_response_t* resp, int status);

/* Write one chunk ("len\r\n<data>\r\n"); NULL/0 writes "0\r\n\r\n". */
void mco_http_resp_write_chunk(mco_http_response_t* resp, const char* data, int len);

/* Send a file: Content-Length comes from its size and Linux uses sendfile()
 * (falls back to a read loop over TLS). 0 on success, -1 on failure. Do not also
 * fill resp->body. */
int mco_http_resp_send_file(mco_http_response_t* resp, const char* path);

/* Serve static files: registers "GET prefix*", mapping the rest under root_dir
 * (Content-Type/Length, chunked above a threshold, ".." guarded). Falls back to
 * root_dir/404.html then the not-found hook. prefix must start with "/".
 * A single range (RFC 7233) gives 206 + Content-Range; an out-of-range start
 * gives 416; multiple/malformed ranges are ignored (200). */
void mco_http_server_serve_static(mco_http_server_t* srv,
                                  const char* prefix, const char* root_dir);

/* Content-Type by extension (no charset); unknown extensions fall back to
 * "application/octet-stream"。 */
const char* mco_http_mime_type(const char* path);

/* ---- WebSocket server ---- */

/* WS handler: the library builds mco_ws_t* and closes/destroys it on return. */
typedef void (*mco_ws_handler_t)(mco_ws_t* ws, mco_http_request_t* req, void* userdata);

/* Register a WS route: on "Upgrade: websocket" the handshake runs and the
 * connection goes to ws_handler. */
void mco_http_server_handle_ws(mco_http_server_t* srv, const char* method,
                               const char* path_pattern,
                               mco_ws_handler_t ws_handler, void* userdata);

#ifdef __cplusplus
}
#endif
#endif /* MINICONET_HTTP_SERVER_H */
