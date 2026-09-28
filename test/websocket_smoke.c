/* WebSocket client end-to-end test: there is no ready coroutine-based WS server
 * yet, so this hand-rolls a minimal WS frame "fake server" over raw TCP — just
 * enough to validate client handshake, frame send/recv and automatic PING-PONG.
 * Coverage: handshake ok, text echo, server-initiated PING, client auto-PONG,
 * MCO_ERR_CLOSED from mco_ws_recv() after a server CLOSE frame.
 */
#include "../miniconet.h"
#include "../miniconet_websocket.h"
#include "../compat/http/wsdef.h"
#include <stdio.h>
#include <string.h>

#define TEST_PORT 17370

static mco_loop_t* g_loop = NULL;
static int g_handshake_ok = 0;
static int g_echo_ok = 0;
static int g_pong_ok = 0;
static int g_close_ok = 0;
static int g_server_done = 0, g_client_done = 0;

/* ---- hand-rolled minimal WS framing (fake server only) ---- */

static int server_read_frame(mco_socket_t* sock, char* out, int outcap, int* opcode) {
    unsigned char hdr[2];
    unsigned char mask[4];
    int payload_len, n, i;

    n = mco_read(sock, hdr, 2, 3000);
    if (n != 2) return -1;
    *opcode = hdr[0] & 0x0f;
    payload_len = hdr[1] & 0x7f; /* test only uses small payloads */

    n = mco_read(sock, mask, 4, 3000); /* client frames must be masked */
    if (n != 4) return -1;

    if (payload_len > outcap) return -1;
    if (payload_len > 0) {
        n = mco_read(sock, out, payload_len, 3000);
        if (n != payload_len) return -1;
        for (i = 0; i < payload_len; i++) out[i] = (char)(out[i] ^ mask[i % 4]);
    }
    return payload_len;
}

static int server_send_frame(mco_socket_t* sock, const char* data, int len, int opcode) {
    unsigned char hdr[2];
    hdr[0] = (unsigned char)(0x80 | (opcode & 0x0f)); /* FIN=1 */
    hdr[1] = (unsigned char)len;                       /* server frames are unmasked */
    if (mco_write(sock, hdr, 2, 3000) != 2) return -1;
    if (len > 0 && mco_write(sock, data, len, 3000) != len) return -1;
    return 0;
}

static void server_task(void* arg) {
    mco_socket_t* listener = (mco_socket_t*)arg;
    mco_socket_t* conn;
    char reqbuf[2048];
    int req_len = 0;
    char* key_line;
    char key[128];
    char accept[32];
    char resp[256];
    char framebuf[128];
    int opcode, n;

    conn = mco_tcp_accept(listener, 5000);
    if (!conn) { g_server_done = 1; mco_close(listener); mco_socket_destroy(listener); return; }

    /* read until "\r\n\r\n" (the header arrives in one read here) */
    for (;;) {
        int r = mco_read(conn, reqbuf + req_len, sizeof(reqbuf) - req_len, 3000);
        if (r <= 0) goto done;
        req_len += r;
        reqbuf[req_len < (int)sizeof(reqbuf) ? req_len : (int)sizeof(reqbuf) - 1] = 0;
        if (strstr(reqbuf, "\r\n\r\n")) break;
        if (req_len >= (int)sizeof(reqbuf)) goto done;
    }

    key_line = strstr(reqbuf, "Sec-WebSocket-Key:");
    if (!key_line) goto done;
    key_line += strlen("Sec-WebSocket-Key:");
    while (*key_line == ' ') key_line++;
    {
        char* end = strstr(key_line, "\r\n");
        int klen = end ? (int)(end - key_line) : 0;
        if (klen <= 0 || klen >= (int)sizeof(key)) goto done;
        memcpy(key, key_line, klen);
        key[klen] = 0;
    }

    ws_encode_key(key, accept); /* reuse wsdef.h SHA1+base64 */
    snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n", accept);
    if (mco_write(conn, resp, (int)strlen(resp), 3000) != (int)strlen(resp)) goto done;
    g_handshake_ok = 1;

    /* 1) read the client's "hello-ws" text frame and echo it back */
    n = server_read_frame(conn, framebuf, sizeof(framebuf), &opcode);
    if (n == 8 && opcode == 0x1 && memcmp(framebuf, "hello-ws", 8) == 0) {
        server_send_frame(conn, framebuf, n, 0x1 /* WS_OP_TEXT */);
    } else {
        goto done;
    }

    /* 2) send a PING; expect the client to auto-reply with an identical PONG */
    server_send_frame(conn, "ping-data", 9, 0x9 /* WS_OP_PING */);
    n = server_read_frame(conn, framebuf, sizeof(framebuf), &opcode);
    g_pong_ok = (n == 9 && opcode == 0xA /* WS_OP_PONG */ && memcmp(framebuf, "ping-data", 9) == 0);

    /* 3) send CLOSE; the next mco_ws_recv() must return MCO_ERR_CLOSED */
    server_send_frame(conn, NULL, 0, 0x8 /* WS_OP_CLOSE */);

done:
    mco_close(conn);
    mco_socket_destroy(conn);
    mco_close(listener);
    mco_socket_destroy(listener);
    g_server_done = 1;
}

static void client_task(void* arg) {
    mco_ws_t* ws;
    char buf[128];
    int opcode, n;
    (void)arg;
    mco_sleep(50);

    ws = mco_ws_connect("127.0.0.1", TEST_PORT, "/ws", 5000);
    if (!ws) { printf("[client] mco_ws_connect failed\n"); g_client_done = 1; return; }
    printf("[client] handshake OK\n");

    n = mco_ws_send_text(ws, "hello-ws", 8, 3000);
    printf("[client] send_text returned %d (expect 0)\n", n);

    n = mco_ws_recv(ws, buf, sizeof(buf), &opcode, 3000);
    if (n == 8 && opcode == MCO_WS_TEXT && memcmp(buf, "hello-ws", 8) == 0) {
        g_echo_ok = 1;
        printf("[client] got echo: '%.8s'\n", buf);
    } else {
        printf("[client] echo mismatch n=%d opcode=%d\n", n, opcode);
    }

    /* this recv expects the server PING handled internally, then CLOSE ->
     * MCO_ERR_CLOSED */
    n = mco_ws_recv(ws, buf, sizeof(buf), &opcode, 3000);
    g_close_ok = (n == MCO_ERR_CLOSED);
    printf("[client] recv after ping/close returned %d (expect %d = MCO_ERR_CLOSED)\n", n, MCO_ERR_CLOSED);

    mco_ws_close(ws);
    mco_ws_destroy(ws);
    g_client_done = 1;
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

    printf("handshake_ok=%d echo_ok=%d pong_ok=%d close_ok=%d server_done=%d client_done=%d\n",
           g_handshake_ok, g_echo_ok, g_pong_ok, g_close_ok, g_server_done, g_client_done);

    ok = g_handshake_ok && g_echo_ok && g_pong_ok && g_close_ok && g_server_done && g_client_done;
    printf(ok ? "WEBSOCKET TEST PASSED\n" : "WEBSOCKET TEST FAILED\n");
    return ok ? 0 : 1;
}
