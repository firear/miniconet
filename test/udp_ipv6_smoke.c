/* Verify mco_udp_socket()'s "lazy create, family chosen by destination" design:
 *  1. IPv4 regression.
 *  2. IPv6: one sendto+recvfrom over the "::1" loopback.
 * The host may have IPv6 disabled; probe first and skip honestly rather than
 * claiming a pass.
 */
#include "../miniconet.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define PORT_V4 17350
#define PORT_V6 17351

static int g_v4_done = 0, g_v4_ok = 0;
static int g_v6_done = 0, g_v6_ok = 0;
static int g_v6_available = 1;

static void v4_server_task(void* arg) {
    mco_socket_t* s = (mco_socket_t*)arg;
    char buf[64], from_host[64];
    int from_port = 0;
    int n = mco_udp_recvfrom(s, buf, sizeof(buf), from_host, sizeof(from_host), &from_port, 3000);
    if (n >= 0) mco_udp_sendto(s, buf, n, from_host, from_port, 3000);
    mco_close(s);
    mco_socket_destroy(s);
}

static void v4_client_task(void* arg) {
    mco_socket_t* s;
    char buf[64];
    int n;
    (void)arg;
    mco_sleep(30);
    s = mco_udp_socket(); /* lazy create: resolves to AF_INET, target 127.0.0.1 */
    mco_udp_sendto(s, "v4-ping", 7, "127.0.0.1", PORT_V4, 3000);
    n = mco_udp_recvfrom(s, buf, sizeof(buf), NULL, 0, NULL, 3000);
    g_v4_ok = (n == 7 && memcmp(buf, "v4-ping", 7) == 0);
    printf("[ipv4] recv n=%d ok=%d\n", n, g_v4_ok);
    mco_close(s);
    mco_socket_destroy(s);
    g_v4_done = 1;
}

static void v6_server_task(void* arg) {
    mco_socket_t* s = (mco_socket_t*)arg;
    char buf[64], from_host[64];
    int from_port = 0;
    int n = mco_udp_recvfrom(s, buf, sizeof(buf), from_host, sizeof(from_host), &from_port, 3000);
    if (n >= 0) mco_udp_sendto(s, buf, n, from_host, from_port, 3000);
    mco_close(s);
    mco_socket_destroy(s);
}

static void v6_client_task(void* arg) {
    mco_socket_t* s;
    char buf[64];
    int n;
    (void)arg;
    mco_sleep(30);
    s = mco_udp_socket(); /* lazy create: target "::1" -> AF_INET6 */
    mco_udp_sendto(s, "v6-ping", 7, "::1", PORT_V6, 3000);
    n = mco_udp_recvfrom(s, buf, sizeof(buf), NULL, 0, NULL, 3000);
    g_v6_ok = (n == 7 && memcmp(buf, "v6-ping", 7) == 0);
    printf("[ipv6] recv n=%d ok=%d\n", n, g_v6_ok);
    mco_close(s);
    mco_socket_destroy(s);
    g_v6_done = 1;
}

static int ipv6_capable(void) {
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

int main(void) {
    mco_loop_t* loop;
    mco_socket_t* v4_listener;
    int ok;

    g_v6_available = ipv6_capable();
    if (!g_v6_available) {
        printf("[ipv6] IPv6 not enabled in current environment (socket(AF_INET6,...) failed), "
               "skip IPv6 part, run IPv4 regression only\n");
    }

    v4_listener = mco_udp_bind("0.0.0.0", PORT_V4);
    if (!v4_listener) { printf("v4 bind failed\n"); return 1; }

    loop = mco_loop_new();
    mco_spawn(loop, v4_server_task, v4_listener);
    mco_spawn(loop, v4_client_task, NULL);

    if (g_v6_available) {
        mco_socket_t* v6_listener = mco_udp_bind("::1", PORT_V6);
        if (!v6_listener) {
            printf("[ipv6] bind(::1) failed, skip IPv6 part\n");
            g_v6_available = 0;
        } else {
            mco_spawn(loop, v6_server_task, v6_listener);
            mco_spawn(loop, v6_client_task, NULL);
        }
    }

    mco_loop_run(loop);
    mco_loop_free(loop);

    printf("v4_done=%d v4_ok=%d v6_available=%d v6_done=%d v6_ok=%d\n",
           g_v4_done, g_v4_ok, g_v6_available, g_v6_done, g_v6_ok);

    ok = g_v4_done && g_v4_ok;
    if (g_v6_available) ok = ok && g_v6_done && g_v6_ok;

    printf(ok ? "UDP DUAL-STACK TEST PASSED (IPv6 %s)\n"
              : "UDP DUAL-STACK TEST FAILED\n",
           g_v6_available ? "verified" : "unsupported, skipped");
    return ok ? 0 : 1;
}
