/* Verify mco_deadline_t shares one timeout budget across calls: two mco_wait_io
 * calls on an fd that never becomes ready (with a mco_sleep in between) must
 * total ~300ms, not 600ms.
 * Also checks the mco_socket_set_timeout()/mco_read_default() convenience API.
 */
#include "../miniconet.h"
#include "../core/mco_vcpu.h"
#include "../compat/htime.h"
#include <stdio.h>
#include <fcntl.h>

static mco_loop_t* g_loop = NULL;
static int g_deadline_ok = 0;
static int g_default_timeout_ok = 0;
static int g_task_done = 0;

static void deadline_task(void* arg) {
    int sv[2];
    uint64_t t0;
    mco_deadline_t dl;
    int r1, r2;
    int elapsed_ms;
    (void)arg;

    /* an fd that never becomes readable: one end of a socketpair */
    if (Socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { g_task_done = 1; return; }
    nonblocking(sv[0]);

    t0 = gethrtime_us();
    dl = mco_deadline_after(300); /* total budget 300ms */

    r1 = mco_wait_io(sv[0], MCO_EV_READ, mco_deadline_ms(dl)); /* should time out at ~300ms */
    mco_sleep(10); /* proves dl is an absolute point, not reset per call */
    r2 = mco_wait_io(sv[0], MCO_EV_READ, mco_deadline_ms(dl)); /* already expired -> timeout at once */

    elapsed_ms = (int)((gethrtime_us() - t0) / 1000ULL);
    printf("[deadline] r1=%d r2=%d elapsed=%dms (expect both MCO_ERR_TIMEOUT=%d, total elapsed ~300~350ms not 600ms)\n",
           r1, r2, elapsed_ms, MCO_ERR_TIMEOUT);

    g_deadline_ok = (r1 == MCO_ERR_TIMEOUT) && (r2 == MCO_ERR_TIMEOUT) && (elapsed_ms < 500);

    closesocket(sv[0]);
    closesocket(sv[1]);
    g_task_done = 1;
}

static void default_timeout_task(void* arg) {
    mco_socket_t* s = (mco_socket_t*)arg;
    char buf[16];
    int n;
    mco_socket_set_timeout(s, 200); /* default timeout for later calls */
    n = mco_read_default(s, buf, sizeof(buf)); /* peer sends nothing: ~200ms timeout */
    printf("[default-timeout] mco_read_default returned %d (expect %d = MCO_ERR_TIMEOUT)\n",
           n, MCO_ERR_TIMEOUT);
    g_default_timeout_ok = (n == MCO_ERR_TIMEOUT);
    mco_close(s);
    mco_socket_destroy(s);
}

int main(void) {
    mco_socket_t* udp;
    int ok;

    g_loop = mco_loop_new();
    mco_spawn(g_loop, deadline_task, NULL);

    /* an unused loopback UDP socket is the cheapest way to test timeouts */
    udp = mco_udp_bind("127.0.0.1", 17340);
    mco_spawn(g_loop, default_timeout_task, udp);

    mco_loop_run(g_loop);
    mco_loop_free(g_loop);

    printf("task_done=%d deadline_ok=%d default_timeout_ok=%d\n",
           g_task_done, g_deadline_ok, g_default_timeout_ok);

    ok = g_task_done && g_deadline_ok && g_default_timeout_ok;
    printf(ok ? "DEADLINE/TIMEOUT TEST PASSED\n" : "DEADLINE/TIMEOUT TEST FAILED\n");
    return ok ? 0 : 1;
}
