#include "../miniconet.h"
#include <stdio.h>
#include <string.h>

#define TEST_PORT 17331

static int g_server_done = 0, g_client_done = 0, g_client_ok = 0;

static void server_task(void* arg) {
    mco_socket_t* s = (mco_socket_t*)arg;
    char buf[64];
    char from_host[64];
    int from_port = 0;
    int n = mco_udp_recvfrom(s, buf, sizeof(buf), from_host, sizeof(from_host), &from_port, 5000);
    if (n >= 0) {
        printf("[server] got %d bytes from %s:%d\n", n, from_host, from_port);
        mco_udp_sendto(s, buf, n, from_host, from_port, 3000); /* echo back */
    } else {
        printf("[server] recvfrom failed n=%d\n", n);
    }
    mco_close(s);
    mco_socket_destroy(s);
    g_server_done = 1;
}

static void client_task(void* arg) {
    mco_socket_t* s;
    char buf[64];
    int n;
    (void)arg;
    mco_sleep(30);

    s = mco_udp_socket();
    if (!s) { printf("[client] mco_udp_socket failed\n"); g_client_done = 1; return; }

    n = mco_udp_sendto(s, "hello-udp", 9, "127.0.0.1", TEST_PORT, 3000);
    printf("[client] sent %d bytes\n", n);

    n = mco_udp_recvfrom(s, buf, sizeof(buf), NULL, 0, NULL, 3000);
    if (n > 0) {
        buf[n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1] = 0;
        printf("[client] echo got %d bytes: '%s'\n", n, buf);
        g_client_ok = (n == 9 && memcmp(buf, "hello-udp", 9) == 0);
    } else {
        printf("[client] recvfrom(echo) failed n=%d\n", n);
    }

    mco_close(s);
    mco_socket_destroy(s);
    g_client_done = 1;
}

int main(void) {
    mco_loop_t* loop;
    mco_socket_t* server_sock = mco_udp_bind("0.0.0.0", TEST_PORT);
    int ok;

    if (!server_sock) { printf("udp bind failed\n"); return 1; }

    loop = mco_loop_new();
    mco_spawn(loop, server_task, server_sock);
    mco_spawn(loop, client_task, NULL);
    mco_loop_run(loop);
    mco_loop_free(loop);

    printf("server_done=%d client_done=%d client_ok=%d\n",
           g_server_done, g_client_done, g_client_ok);

    ok = g_server_done && g_client_done && g_client_ok;
    printf(ok ? "UDP ECHO TEST PASSED\n" : "UDP ECHO TEST FAILED\n");
    return ok ? 0 : 1;
}
