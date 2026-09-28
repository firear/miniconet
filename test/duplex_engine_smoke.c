/* At the core layer: on one fd, a coroutine waiting only for READ and another
 * only for WRITE must not clobber each other's registration.
 * socketpair makes "the send buffer is full, the next write() always EAGAIN"
 * deterministic, independent of network timing.
 */
#include "../core/mco_vcpu.h"
#include "../core/mco_engine_backends.h"
#include "hsocket.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

static int sv[2];
static int g_read_woken = 0, g_read_result = -999;
static int g_write_woken = 0, g_write_result = -999;


/* Flood sv[0]'s send buffer so a later write() always returns EAGAIN. */
static void flood_fill(int fd) {
    char buf[4096];
    memset(buf, 'x', sizeof(buf));
    for (;;) {
        /* send/recv: on Windows an fd is a SOCKET handle, CRT read/write fail */
        ssize_t n = send(fd, buf, (int)sizeof(buf), 0);
        if (n < 0) {
            int e = socket_errno();
            if (e == EAGAIN || e == EWOULDBLOCK) break;
            if (e == EINTR) continue;
            break;
        }
    }
}

static void reader_task(void* arg) {
    (void)arg;
    /* sv[0] has no readable data yet, so this must park */
    g_read_result = mco_wait_io(sv[0], MCO_EV_READ, 3000);
    g_read_woken = 1;
    printf("[reader] woken, result=%d\n", g_read_result);
}

static void writer_task(void* arg) {
    (void)arg;
    /* sv[0]'s send buffer is full, so this must park */
    g_write_result = mco_wait_io(sv[0], MCO_EV_WRITE, 3000);
    g_write_woken = 1;
    printf("[writer] woken, result=%d\n", g_write_result);
}

/* Fire both directions only after reader/writer are parked:
 *  - drain sv[1] to free sv[0]'s send buffer -> wakes writer
 *  - write to sv[1] so sv[0] becomes readable -> wakes reader
 */
static void trigger_task(void* arg) {
    char drain[4096];
    ssize_t n;
    (void)arg;
    mco_sleep_ms(50); /* ensure reader/writer are parked and registered */

    /* drain everything sv[1] received to free sv[0]'s send buffer */
    do {
        n = recv(sv[1], drain, (int)sizeof(drain), 0);
    } while (n > 0);

    (void)!send(sv[1], "hi", 2, 0); /* make sv[0] readable */
}

static int run_one(const char* name, const mco_engine_ops_t* ops) {
    mco_vcpu_t* vcpu;

    if (Socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); return 0; }
    nonblocking(sv[0]);
    nonblocking(sv[1]);
    flood_fill(sv[0]);

    g_read_woken = g_write_woken = 0;
    g_read_result = g_write_result = -999;

    vcpu = mco_vcpu_new(ops);
    mco_go(vcpu, reader_task, NULL);
    mco_go(vcpu, writer_task, NULL);
    mco_go(vcpu, trigger_task, NULL);
    mco_vcpu_run(vcpu);
    mco_vcpu_free(vcpu);

    closesocket(sv[0]);
    closesocket(sv[1]);

    printf("[%s] read_result=%d write_result=%d\n", name, g_read_result, g_write_result);
    return g_read_woken && g_read_result >= 0 && g_write_woken && g_write_result >= 0;
}

int main(void) {
    int ok = 1;
#ifdef _WIN32
    /* Windows ships three backends: select, wepoll, and iocp (when built). */
    ok &= run_one("select", mco_engine_select_ops());
    ok &= run_one("wepoll", mco_engine_wepoll_ops());
  #if MINICONET_WITH_IOCP
    ok &= run_one("iocp",   mco_engine_iocp_ops());
  #endif
#else
    ok &= run_one("epoll",  mco_engine_default());
    ok &= run_one("select", mco_engine_select_ops());
    ok &= run_one("poll",   mco_engine_poll_ops());
  #if MINICONET_WITH_IO_URING && defined(__linux__)
    ok &= run_one("io_uring", mco_engine_uring_ops());
  #endif
#endif

    printf(ok ? "DUPLEX ENGINE MATRIX TEST PASSED\n" : "DUPLEX ENGINE MATRIX TEST FAILED\n");
    return ok ? 0 : 1;
}
