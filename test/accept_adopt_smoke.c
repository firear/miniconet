/* Split accept/adopt API test (Step 2).
 *   1. TCP: the server does mco_tcp_accept_fd() + mco_socket_adopt_fd() as two
 *      steps; the result must match mco_tcp_accept() (covered by tcp_echo_smoke).
 *   2. UDP: mco_socket_adopt_fd() must work for any fd, not just accepted TCP
 *      fds — two raw POSIX UDP sockets are adopted and driven through
 *      mco_udp_sendto/recvfrom.
 * No workpool dependency, so it is not in the gated list.
 */
#include "../miniconet.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>

#define TEST_PORT 17331

static int g_tcp_server_done = 0;
static int g_tcp_client_done = 0;
static int g_tcp_ok = 0;

static void tcp_server_task(void* arg) {
    mco_socket_t* listener = (mco_socket_t*)arg;
    int fd;
    mco_socket_t* conn;
    char buf[128];
    int n;

    /* accept only to get the raw fd, no wrapping */
    fd = mco_tcp_accept_fd(listener, 5000);
    if (fd < 0) { printf("[tcp] accept_fd failed: %d\n", fd); g_tcp_server_done = 1; return; }
    printf("[tcp] accept_fd got fd=%d\n", fd);

    /* then adopt separately, same vcpu here (Step 3 covers cross-thread) */
    conn = mco_socket_adopt_fd(fd);
    if (!conn) { printf("[tcp] adopt_fd failed\n"); close(fd); g_tcp_server_done = 1; return; }
    printf("[tcp] adopt_fd ok, fd=%d\n", mco_socket_fd(conn));

    n = mco_read(conn, buf, sizeof(buf) - 1, 5000);
    if (n > 0) {
        buf[n] = 0;
        mco_write(conn, buf, n, 5000);
    } else {
        printf("[tcp] server read failed n=%d\n", n);
    }

    mco_close(conn);
    mco_socket_destroy(conn);
    mco_close(listener);
    mco_socket_destroy(listener);
    g_tcp_server_done = 1;
}

static void tcp_client_task(void* arg) {
    mco_socket_t* conn;
    char buf[128];
    int n;
    (void)arg;

    mco_sleep(50);

    conn = mco_tcp_connect("127.0.0.1", TEST_PORT, 3000, NULL);
    if (!conn) { printf("[tcp] connect failed\n"); g_tcp_client_done = 1; return; }

    n = mco_write(conn, "accept-fd-adopt-fd", 19, 3000);
    if (n != 19) { printf("[tcp] write failed n=%d\n", n); }

    n = mco_read(conn, buf, sizeof(buf) - 1, 3000);
    if (n > 0) {
        buf[n] = 0;
        g_tcp_ok = (n == 19 && memcmp(buf, "accept-fd-adopt-fd", 19) == 0);
        printf("[tcp] echo got '%s' ok=%d\n", buf, g_tcp_ok);
    } else {
        printf("[tcp] client read failed n=%d\n", n);
    }

    mco_close(conn);
    mco_socket_destroy(conn);
    g_tcp_client_done = 1;
}

static int run_tcp_part(void) {
    mco_loop_t* loop = mco_loop_new();
    mco_socket_t* listener = mco_tcp_listen("0.0.0.0", TEST_PORT);
    if (!listener) { printf("[tcp] listen failed\n"); return 0; }

    mco_spawn(loop, tcp_server_task, listener);
    mco_spawn(loop, tcp_client_task, NULL);
    mco_loop_run(loop);
    mco_loop_free(loop);

    return g_tcp_server_done && g_tcp_client_done && g_tcp_ok;
}

/* ---- UDP generality: no coroutine coordination needed ---- */

static int g_udp_ok = 0;

static void udp_task(void* arg) {
    int rx_fd, tx_fd;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    mco_socket_t *rx, *tx;
    int n;
    char buf[64];
    (void)arg;

    /* raw POSIX socket(), to prove adopt works on fds created outside the lib */
    rx_fd = socket(AF_INET, SOCK_DGRAM, 0);
    tx_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (rx_fd < 0 || tx_fd < 0) {
        printf("[udp] socket() failed\n");
        goto done;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; /* let the kernel pick a free port */
    if (bind(rx_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        printf("[udp] bind failed\n");
        close(rx_fd); close(tx_fd);
        goto done;
    }
    if (getsockname(rx_fd, (struct sockaddr*)&addr, &addrlen) != 0) {
        printf("[udp] getsockname failed\n");
        close(rx_fd); close(tx_fd);
        goto done;
    }

    rx = mco_socket_adopt_fd(rx_fd);
    tx = mco_socket_adopt_fd(tx_fd);
    if (!rx || !tx) {
        printf("[udp] adopt_fd failed\n");
        if (!rx) close(rx_fd);
        if (!tx) close(tx_fd);
        goto done;
    }

    n = mco_udp_sendto(tx, "udp-adopt-hello", 15, "127.0.0.1", (int)ntohs(addr.sin_port), 2000);
    if (n != 15) { printf("[udp] sendto failed n=%d\n", n); goto cleanup; }

    n = mco_udp_recvfrom(rx, buf, sizeof(buf) - 1, NULL, 0, NULL, 2000);
    if (n > 0) {
        buf[n] = 0;
        g_udp_ok = (n == 15 && memcmp(buf, "udp-adopt-hello", 15) == 0);
        printf("[udp] recvfrom got '%s' ok=%d\n", buf, g_udp_ok);
    } else {
        printf("[udp] recvfrom failed n=%d\n", n);
    }

cleanup:
    mco_close(rx); mco_socket_destroy(rx);
    mco_close(tx); mco_socket_destroy(tx);
done:
    return;
}

static int run_udp_part(void) {
    mco_loop_t* loop = mco_loop_new();
    mco_spawn(loop, udp_task, NULL);
    mco_loop_run(loop);
    mco_loop_free(loop);
    return g_udp_ok;
}

int main(void) {
    int tcp_ok = run_tcp_part();
    int udp_ok = run_udp_part();

    printf("tcp_ok=%d udp_ok=%d\n", tcp_ok, udp_ok);

    if (tcp_ok && udp_ok) {
        printf("SMOKE TEST PASSED\n");
        return 0;
    }
    printf("SMOKE TEST FAILED\n");
    return 1;
}
