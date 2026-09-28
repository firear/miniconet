/* Cross-thread handoff check (Step 3): thread A takes a raw fd via
 * mco_tcp_accept_fd() and passes the int to thread B (real OS threads via
 * hthread_create, no workpool); thread B adopts it on its own vcpu and verifies
 * io. Kept separate from workpool dispatch so failures localize to adopt.
 * Uses miniconet's cross-platform hthread and hmutex + hcondvar for the mailbox.
 */
#include "../miniconet.h"
#include <stdio.h>
#include <string.h>
#include "../compat/hthread.h"
#include "../compat/hmutex.h"

#define TEST_PORT 17332

/* One-shot mailbox passing the fd from thread A to thread B, guarded by
 * hmutex + hcondvar (not mco_oneshot, which is a coroutine mechanism). */
static hmutex_t    g_mailbox_lock;
static hcondvar_t  g_mailbox_cond;
static int g_mailbox_fd = -1;      /* -1 = empty; >=0 = fd pending; -2 = EOF signal */
static int g_mailbox_has_msg = 0;

static void mailbox_put(int fd_or_signal) {
    hmutex_lock(&g_mailbox_lock);
    g_mailbox_fd = fd_or_signal;
    g_mailbox_has_msg = 1;
    hcondvar_signal(&g_mailbox_cond);
    hmutex_unlock(&g_mailbox_lock);
}

static int mailbox_take(void) {
    int v;
    hmutex_lock(&g_mailbox_lock);
    while (!g_mailbox_has_msg) hcondvar_wait(&g_mailbox_cond, &g_mailbox_lock);
    v = g_mailbox_fd;
    g_mailbox_has_msg = 0;
    hmutex_unlock(&g_mailbox_lock);
    return v;
}

static int g_client_done = 0;
static int g_client_ok = 0;
static int g_threadB_done = 0;
static int g_threadB_ok = 0;

/* ---- thread A: accept only, drop the raw fd into the mailbox ---- */
static void thread_a_acceptor_task(void* arg) {
    mco_socket_t* listener = (mco_socket_t*)arg;
    int fd = mco_tcp_accept_fd(listener, 5000);
    if (fd < 0) {
        printf("[threadA] accept_fd failed: %d\n", fd);
        mailbox_put(-2);
    } else {
        printf("[threadA] accept_fd got fd=%d, handing off to thread B\n", fd);
        mailbox_put(fd);
    }
    mco_close(listener);
    mco_socket_destroy(listener);
}

static HTHREAD_ROUTINE(thread_a_main) {
    mco_loop_t* loop = mco_loop_new();
    mco_socket_t* listener = (mco_socket_t*)userdata;
    mco_spawn(loop, thread_a_acceptor_task, listener);
    mco_loop_run(loop);
    mco_loop_free(loop);
    return 0;
}

/* ---- thread B: take the fd, adopt it in a coroutine, verify io ---- */
static void thread_b_worker_task(void* arg) {
    int fd = *(int*)arg;
    mco_socket_t* conn;
    char buf[128];
    int n;

    conn = mco_socket_adopt_fd(fd);
    if (!conn) {
        printf("[threadB] adopt_fd failed\n");
        g_threadB_done = 1;
        return;
    }
    printf("[threadB] adopted fd=%d on a different OS thread than accept_fd() ran on\n", mco_socket_fd(conn));

    n = mco_read(conn, buf, sizeof(buf) - 1, 5000);
    if (n > 0) {
        buf[n] = 0;
        mco_write(conn, buf, n, 5000);
        g_threadB_ok = 1;
    } else {
        printf("[threadB] read failed n=%d\n", n);
    }

    mco_close(conn);
    mco_socket_destroy(conn);
    g_threadB_done = 1;
}

static HTHREAD_ROUTINE(thread_b_main) {
    int fd = *(int*)userdata;
    mco_loop_t* loop;

    if (fd < 0) { g_threadB_done = 1; return 0; }

    loop = mco_loop_new();
    mco_spawn(loop, thread_b_worker_task, &fd);
    mco_loop_run(loop);
    mco_loop_free(loop);
    return 0;
}

/* ---- client: separate third thread/vcpu, connect + echo ---- */
static void client_task(void* arg) {
    mco_socket_t* conn;
    char buf[128];
    int n;
    (void)arg;

    mco_sleep(80); /* give thread A's accept and thread B's adopt time */

    conn = mco_tcp_connect("127.0.0.1", TEST_PORT, 3000, NULL);
    if (!conn) { printf("[client] connect failed\n"); g_client_done = 1; return; }

    n = mco_write(conn, "cross-thread-hello", 19, 3000);
    if (n != 19) printf("[client] write failed n=%d\n", n);

    n = mco_read(conn, buf, sizeof(buf) - 1, 3000);
    if (n > 0) {
        buf[n] = 0;
        g_client_ok = (n == 19 && memcmp(buf, "cross-thread-hello", 19) == 0);
        printf("[client] echo got '%s' ok=%d\n", buf, g_client_ok);
    } else {
        printf("[client] read failed n=%d\n", n);
    }

    mco_close(conn);
    mco_socket_destroy(conn);
    g_client_done = 1;
}

static HTHREAD_ROUTINE(client_thread_main) {
    mco_loop_t* loop = mco_loop_new();
    (void)userdata;
    mco_spawn(loop, client_task, NULL);
    mco_loop_run(loop);
    mco_loop_free(loop);
    return 0;
}

int main(void) {
    mco_socket_t* listener = mco_tcp_listen("0.0.0.0", TEST_PORT);
    hthread_t th_a, th_b, th_c;
    uv_thread_options_t opt;
    int handed_fd;

    if (!listener) { printf("listen failed\n"); return 1; }

    hmutex_init(&g_mailbox_lock);
    hcondvar_init(&g_mailbox_cond);

    opt.flags = UV_THREAD_NO_FLAGS;
    opt.stack_size = 0;

    th_a = hthread_create(&opt, thread_a_main, listener);
    th_c = hthread_create(&opt, client_thread_main, NULL);

    /* main thread acts as scheduler: wait for A's accept, then start B */
    handed_fd = mailbox_take();
    th_b = hthread_create(&opt, thread_b_main, &handed_fd);

    hthread_join(th_a);
    hthread_join(th_b);
    hthread_join(th_c);

    hcondvar_destroy(&g_mailbox_cond);
    hmutex_destroy(&g_mailbox_lock);

    printf("client_done=%d client_ok=%d threadB_done=%d threadB_ok=%d\n",
           g_client_done, g_client_ok, g_threadB_done, g_threadB_ok);

    if (g_client_done && g_client_ok && g_threadB_done && g_threadB_ok) {
        printf("SMOKE TEST PASSED\n");
        return 0;
    }
    printf("SMOKE TEST FAILED\n");
    return 1;
}
