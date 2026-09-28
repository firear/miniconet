/*
 * miniconet_websocket.h — coroutine-based WebSocket client.
 * Only built with MINICONET_WITH_HTTP_PARSER=ON. Client only; see handle_ws in
 * miniconet_http_server.h for the server side.
 *   mco_ws_t* ws = mco_ws_connect("echo.example.com", 80, "/ws", 3000);
 *   mco_ws_send_text(ws, "hello", 5, 3000);
 *   char buf[256]; int opcode;
 *   int n = mco_ws_recv(ws, buf, sizeof(buf), &opcode, 3000);
 *   mco_ws_close(ws); mco_ws_destroy(ws);
 */
#ifndef MINICONET_WEBSOCKET_H
#define MINICONET_WEBSOCKET_H

#include "miniconet.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mco_ws mco_ws_t;

/* Message type reported by mco_ws_recv(). */
#define MCO_WS_TEXT   1
#define MCO_WS_BINARY 2

/* Connect + send the Upgrade request + check Sec-WebSocket-Accept. timeout_ms
 * covers connect + handshake. NULL on failure. Coroutine-only. */
mco_ws_t* mco_ws_connect(const char* host, int port, const char* path, int timeout_ms);

/* Server handshake on a connection that already received a valid Upgrade
 * request: computes Accept from the client key and replies 101. leftover holds
 * bytes the client sent right after the request (may contain the first frame);
 * pass NULL/0 if none. NULL on failure (no response written). Coroutine-only. */
mco_ws_t* mco_ws_accept(mco_socket_t* c, const char* key,
                        const char* leftover, int leftover_len);

/* Send a whole message as one FIN=1 frame (no fragmentation). 0 on success,
 * <0 = MCO_ERR_*. */
int mco_ws_send_text(mco_ws_t* ws, const char* data, int len, int timeout_ms);
int mco_ws_send_binary(mco_ws_t* ws, const void* data, int len, int timeout_ms);

/* Receive a whole message: reassembles fragments, replies PONG and ignores
 * PONGs, returns only TEXT/BINARY. >=0 = bytes written; a message larger than
 * buf gives MCO_ERR_IO (no truncation, no growth); CLOSE gives MCO_ERR_CLOSED. */
int mco_ws_recv(mco_ws_t* ws, void* buf, int len, int* opcode, int timeout_ms);

/* Send CLOSE (best effort, no wait for ack) and close the TCP connection. */
void mco_ws_close(mco_ws_t* ws);
void mco_ws_destroy(mco_ws_t* ws);

#ifdef __cplusplus
}
#endif
#endif /* MINICONET_WEBSOCKET_H */
