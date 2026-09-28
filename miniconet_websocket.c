#include "miniconet_websocket.h"
#include "picohttpparser.h"
#include "websocket_parser.h"
#include "wsdef.h"
#include "base64.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define WS_HANDSHAKE_BUF_SIZE 4096
#define WS_RECV_BUF_SIZE      4096

struct mco_ws {
    mco_socket_t* sock;
    websocket_parser parser;
    /* RFC 6455: clients must mask, servers must not. */
    int is_server;
    /* Bytes left unparsed by a previous mco_ws_recv() must persist across calls,
     * otherwise they are lost. */
    char pending_buf[WS_RECV_BUF_SIZE];
    int  pending_len;   /* valid bytes in pending_buf */
    int  pending_off;   /* bytes already fed to the parser */
};

/* ---- handshake ---- */

/* picohttpparser is case-sensitive; HTTP header names are not. */
static int header_name_eq(const struct phr_header* h, const char* name) {
    size_t namelen = strlen(name);
    size_t i;
    if (h->name_len != namelen) return 0;
    for (i = 0; i < namelen; i++) {
        if (tolower((unsigned char)h->name[i]) != tolower((unsigned char)name[i])) return 0;
    }
    return 1;
}

mco_ws_t* mco_ws_connect(const char* host, int port, const char* path, int timeout_ms) {
    mco_deadline_t dl = mco_deadline_after(timeout_ms);
    mco_socket_t* sock;
    mco_ws_t* ws;
    char key_raw[16];
    char key_b64[32];
    char expect_accept[32];
    char req[512];
    int req_len, i, n;
    char resp[WS_HANDSHAKE_BUF_SIZE]; /* 4KB on the coroutine's own stack */
    int resp_len = 0;
    struct phr_header headers[32];
    size_t num_headers;
    int minor_version, status;
    const char* msg;
    size_t msg_len;
    int pret;
    const char* accept_hdr = NULL;
    size_t accept_hdr_len = 0;

    sock = mco_tcp_connect(host, port, mco_deadline_ms(dl), NULL);
    if (!sock) return NULL;

    /* 16 random bytes for Sec-WebSocket-Key; rand() is fine, the key only needs
     * to look random (RFC 6455). */
    for (i = 0; i < 16; i++) key_raw[i] = (char)(rand() & 0xff);
    hv_base64_encode((const unsigned char*)key_raw, 16, key_b64);
    key_b64[24] = '\0'; /* 16 bytes base64 is 24 chars; encode writes no NUL */

    req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        (path && *path) ? path : "/", host, port, key_b64);
    if (req_len < 0 || req_len >= (int)sizeof(req)) { mco_close(sock); mco_socket_destroy(sock); return NULL; }

    n = mco_write_all(sock, req, req_len, mco_deadline_ms(dl));
    if (n != req_len) { mco_close(sock); mco_socket_destroy(sock); return NULL; }

    /* Read until the header is complete, reparsing from the start each time. */
    for (;;) {
        int r = mco_read(sock, resp + resp_len, WS_HANDSHAKE_BUF_SIZE - resp_len, mco_deadline_ms(dl));
        if (r <= 0) goto fail;
        resp_len += r;

        num_headers = sizeof(headers) / sizeof(headers[0]);
        pret = phr_parse_response(resp, (size_t)resp_len, &minor_version, &status,
                                   &msg, &msg_len, headers, &num_headers, 0);
        if (pret > 0) break;                 /* header complete */
        if (pret == -1) goto fail;            /* not a valid HTTP response */
        if (resp_len >= WS_HANDSHAKE_BUF_SIZE) goto fail; /* header too large */
        /* -2: incomplete, keep reading */
    }

    if (status != 101) goto fail; /* no protocol switch: rejected */

    for (i = 0; i < (int)num_headers; i++) {
        if (header_name_eq(&headers[i], SEC_WEBSOCKET_ACCEPT)) {
            accept_hdr = headers[i].value;
            accept_hdr_len = headers[i].value_len;
            break;
        }
    }
    if (!accept_hdr) goto fail;

    ws_encode_key(key_b64, expect_accept);
    if (accept_hdr_len != strlen(expect_accept) ||
        memcmp(accept_hdr, expect_accept, accept_hdr_len) != 0) {
        goto fail; /* Sec-WebSocket-Accept mismatch */
    }

    ws = (mco_ws_t*)calloc(1, sizeof(mco_ws_t));
    if (!ws) { mco_close(sock); mco_socket_destroy(sock); return NULL; }
    ws->sock = sock;
    ws->is_server = 0;
    websocket_parser_init(&ws->parser);
    /* Keep the bytes read after the 101 response: frames pushed early would
     * otherwise be lost. */
    {
        int lo = resp_len - pret;
        if (lo > 0) {
            if (lo > (int)sizeof(ws->pending_buf)) lo = (int)sizeof(ws->pending_buf);
            memcpy(ws->pending_buf, resp + pret, (size_t)lo);
            ws->pending_len = lo;
            ws->pending_off = 0;
        }
    }
    return ws;

fail:
    mco_close(sock);
    mco_socket_destroy(sock);
    return NULL;
}

/* ---- send ---- */

static int ws_send_frame(mco_ws_t* ws, const char* data, int len, enum ws_opcode opcode, int timeout_ms) {
    int mask = ws->is_server ? 0 : 1;   /* RFC 6455: servers must not mask */
    int frame_size = ws_calc_frame_size(len, mask);
    char* frame;
    int built = 0;   /* built frame length, needed after the write */
    int n;

    frame = (frame_size <= 512) ? NULL : (char*)malloc((size_t)frame_size);
    {
        char stackbuf[512];
        char* out = frame ? frame : stackbuf;
        built = mask ? ws_client_build_frame(out, data, len, opcode, true)
                     : ws_server_build_frame(out, data, len, opcode, true);
        n = mco_write_all(ws->sock, out, built, timeout_ms);
        if (frame) free(frame);
    }
    if (n < 0) return n;
    return (n == built) ? 0 : MCO_ERR_IO;
}

int mco_ws_send_text(mco_ws_t* ws, const char* data, int len, int timeout_ms) {
    if (!ws) return MCO_ERR_CLOSED;
    return ws_send_frame(ws, data, len, WS_OPCODE_TEXT, timeout_ms);
}

int mco_ws_send_binary(mco_ws_t* ws, const void* data, int len, int timeout_ms) {
    if (!ws) return MCO_ERR_CLOSED;
    return ws_send_frame(ws, (const char*)data, len, WS_OPCODE_BINARY, timeout_ms);
}

/* ---- receive ---- */

typedef struct {
    char* out;
    int   out_cap;
    int   out_len;          /* bytes of the current data message in out */
    int   frame_start;      /* start of this frame's body in out, so a control
                               frame can be undone */
    int   msg_opcode;       /* opcode of the first fragment */
    int   cur_opcode;       /* opcode of this frame */
    int   message_done;     /* a whole data message is complete */
    int   overflow;         /* buf too small */
    mco_ws_t* ws;
    int   timeout_ms;
    int   close_received;
} ws_recv_ctx_t;

static int on_frame_header(websocket_parser* p) {
    ws_recv_ctx_t* ctx = (ws_recv_ctx_t*)p->data;
    ctx->cur_opcode = websocket_parser_get_opcode(p);
    ctx->frame_start = ctx->out_len;
    if (ctx->cur_opcode != WS_OP_CONTINUE &&
        ctx->cur_opcode != WS_OP_PING && ctx->cur_opcode != WS_OP_PONG && ctx->cur_opcode != WS_OP_CLOSE) {
        ctx->msg_opcode = ctx->cur_opcode; /* remember it for CONTINUATION frames */
    }
    return 0;
}

static int on_frame_body(websocket_parser* p, const char* at, size_t length) {
    ws_recv_ctx_t* ctx = (ws_recv_ctx_t*)p->data;
    if (ctx->out_len + (int)length > ctx->out_cap) {
        ctx->overflow = 1;
        return -1; /* stop websocket_parser_execute early */
    }
    if (websocket_parser_has_mask(p)) {
        /* Be defensive: a server should not mask, but handle it anyway. */
        websocket_parser_decode(ctx->out + ctx->out_len, at, length, p);
    } else {
        memcpy(ctx->out + ctx->out_len, at, length);
    }
    ctx->out_len += (int)length;
    return 0;
}

static int on_frame_end(websocket_parser* p) {
    ws_recv_ctx_t* ctx = (ws_recv_ctx_t*)p->data;
    int is_control = (ctx->cur_opcode == WS_OP_PING || ctx->cur_opcode == WS_OP_PONG || ctx->cur_opcode == WS_OP_CLOSE);

    if (ctx->cur_opcode == WS_OP_PING) {
        ws_send_frame(ctx->ws, ctx->out + ctx->frame_start, ctx->out_len - ctx->frame_start,
                      WS_OPCODE_PONG, ctx->timeout_ms);
    } else if (ctx->cur_opcode == WS_OP_CLOSE) {
        ctx->close_received = 1;
    }

    if (is_control) {
        ctx->out_len = ctx->frame_start; /* control frames do not count */
        if (ctx->close_received) return -1; /* mco_ws_recv returns MCO_ERR_CLOSED */
        return 0;
    }

    if (websocket_parser_has_final(p)) {
        ctx->message_done = 1;
        return -1; /* message complete: stop and return */
    }
    return 0;
}

int mco_ws_recv(mco_ws_t* ws, void* buf, int len, int* opcode, int timeout_ms) {
    mco_deadline_t dl;
    ws_recv_ctx_t ctx;
    websocket_parser_settings settings;

    if (!ws) return MCO_ERR_CLOSED;
    dl = mco_deadline_after(timeout_ms);

    memset(&ctx, 0, sizeof(ctx));
    ctx.out = (char*)buf;
    ctx.out_cap = len;
    ctx.ws = ws;
    ctx.timeout_ms = timeout_ms;

    websocket_parser_settings_init(&settings);
    settings.on_frame_header = on_frame_header;
    settings.on_frame_body = on_frame_body;
    settings.on_frame_end = on_frame_end;
    ws->parser.data = &ctx;

    for (;;) {
        size_t consumed;

        /* Consume bytes left from the previous call first, or they are lost. */
        if (ws->pending_off >= ws->pending_len) {
            int n = mco_read(ws->sock, ws->pending_buf, sizeof(ws->pending_buf), mco_deadline_ms(dl));
            if (n == 0) return MCO_ERR_CLOSED; /* peer dropped the connection */
            if (n < 0) return n;               /* passed through as-is */
            ws->pending_len = n;
            ws->pending_off = 0;
        }

        consumed = websocket_parser_execute(&ws->parser, &settings,
                                             ws->pending_buf + ws->pending_off,
                                             (size_t)(ws->pending_len - ws->pending_off));
        ws->pending_off += (int)consumed;

        if (ctx.overflow) return MCO_ERR_IO;      /* message does not fit */
        if (ctx.close_received) return MCO_ERR_CLOSED;
        if (ctx.message_done) {
            *opcode = (ctx.msg_opcode == WS_OPCODE_BINARY) ? MCO_WS_BINARY : MCO_WS_TEXT;
            return ctx.out_len;
        }
        /* no complete message yet: keep going */
    }
}

/* ---- close ---- */

void mco_ws_close(mco_ws_t* ws) {
    if (!ws) return;
    ws_send_frame(ws, NULL, 0, WS_OPCODE_CLOSE, 1000); /* best effort, no ack */
    mco_close(ws->sock);
}

void mco_ws_destroy(mco_ws_t* ws) {
    if (!ws) return;
    mco_socket_destroy(ws->sock);
    free(ws);
}

/* ---- server handshake ---- */

/* leftover: bytes the client sent right after the Upgrade request (possibly the
 * first frame). Feed them into pending_buf or they are lost. */
mco_ws_t* mco_ws_accept(mco_socket_t* c, const char* key,
                        const char* leftover, int leftover_len) {
    char accept[64];
    char resp[256];
    int n;

    if (!c || !key) return NULL;
    ws_encode_key(key, accept);

    n = snprintf(resp, sizeof(resp),
                 "HTTP/1.1 101 Switching Protocols\r\n"
                 "Upgrade: websocket\r\n"
                 "Connection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: %s\r\n"
                 "\r\n", accept);
    if (mco_write_all(c, resp, n, 10000) != n) return NULL;

    mco_ws_t* ws = (mco_ws_t*)calloc(1, sizeof(mco_ws_t));
    if (!ws) return NULL;
    ws->sock = c;
    ws->is_server = 1;
    /* explicit init; do not rely on calloc zeroing */
    websocket_parser_init(&ws->parser);
    /* keep bytes read after the request (first frame) */
    if (leftover && leftover_len > 0) {
        if (leftover_len > (int)sizeof(ws->pending_buf))
            leftover_len = (int)sizeof(ws->pending_buf);
        memcpy(ws->pending_buf, leftover, (size_t)leftover_len);
        ws->pending_len = leftover_len;
        ws->pending_off = 0;
    }
    return ws;
}
