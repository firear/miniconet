#include "../miniconet.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// config
#define FORWARD_PORT 17321
#define TARGET_HOST "127.0.0.1"
#define TARGET_PORT 80

// global control
static volatile int g_running = 1;
static int g_active_connections = 0;

static const char* g_target_host = TARGET_HOST;
static int         g_target_port = TARGET_PORT;
static int         g_listen_port = FORWARD_PORT;

// Forwarding context for one client<->target connection, with one pump coroutine
// per direction.
// Fixes two defects of the original version:
//   1) half-close was not propagated: EOF only set a local flag, so an HTTP client
//      that shuts down its write side and waits for the response deadlocked the
//      server. Correct: shutdown(dst, WR) on source EOF, then keep reading dst.
//   2) one coroutine polling both directions serially stalled one direction while
//      the other was blocked.
typedef struct {
    mco_socket_t* client;
    mco_socket_t* target;
    int done;     // finished pumps (EOF or error), 0..2
    int closing;  // mco_close already called (guards proxy_close_once)
    int cleaned;  // destroy+free already done (reentry guard)
} proxy_ctx_t;

typedef struct {
    proxy_ctx_t* ctx;
    mco_socket_t* src;   // source read by this direction
    mco_socket_t* dst;   // destination written by this direction
    char*        buf;
    int          timeout;
} proxy_pump_arg_t;

#define PROXY_BUF_SIZE 8192

// signal handling
static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
    printf("\n[proxy] Shutting down...\n");
}

// Idempotent: close once per ctx. Either pump may want to close (an error must
// wake the peer blocked in read), so closing guards against a double mco_close.
static void proxy_close_once(proxy_ctx_t* ctx)
{
    if (ctx->closing) return;
    ctx->closing = 1;
    mco_close(ctx->client);
    mco_close(ctx->target);
}

// Pump coroutine for one direction: read src, write dst.
//   - EOF(0)   : shutdown(dst, WR) and stop; do not close, the peer may still
//                have trailing bytes.
//   - timeout  : keep looping (the loop head rechecks g_running).
//   - other err: close the whole connection (wakes the peer pump).
//   - write err: the connection is untrusted, close it.
static void proxy_pump(void* arg)
{
    proxy_pump_arg_t* pa = (proxy_pump_arg_t*)arg;
    proxy_ctx_t*      ctx = pa->ctx;
    mco_socket_t*     src = pa->src;
    mco_socket_t*     dst = pa->dst;
    char*             buf = pa->buf;
    int               timeout = pa->timeout;
    int               n;
    int               finished;

    for (;;) {
        if (!g_running) {            // Ctrl+C: close the whole connection
            proxy_close_once(ctx);
            break;
        }
        n = mco_read(src, buf, PROXY_BUF_SIZE - 1, timeout);
        if (n > 0) {
            if (mco_write_all(dst, buf, n, timeout) != n) {
                proxy_close_once(ctx);  // write failed: tear down
                break;
            }
        } else if (n == 0) {
            mco_shutdown(dst, MCO_SHUT_WR);  // propagate half-close
            break;
        } else if (n != MCO_ERR_TIMEOUT) {
            proxy_close_once(ctx);  // real io error / closed
            break;
        }
        // MCO_ERR_TIMEOUT: keep looping
    }

    // the last pump to finish does the cleanup
    finished = ++ctx->done;
    if (finished >= 2 && !ctx->cleaned) {
        ctx->cleaned = 1;
        proxy_close_once(ctx);  // final close
        mco_socket_destroy(ctx->client);
        mco_socket_destroy(ctx->target);
        free(ctx);
        g_active_connections--;
    }
    free(pa);
}

// coroutine task for a new connection
static void handle_connection_task(void* arg)
{
    mco_socket_t* client_conn = (mco_socket_t*)arg;
    mco_loop_t*   loop = mco_vcpu_current();
    mco_socket_t* target_conn;
    proxy_ctx_t*  ctx;
    proxy_pump_arg_t* pa1;
    proxy_pump_arg_t* pa2;

    printf("[proxy] New connection from fd=%d\n", mco_socket_fd(client_conn));

    target_conn = mco_tcp_connect(g_target_host, g_target_port, 5000, NULL);
    if (!target_conn) {
        printf("[proxy] Failed to connect to target %s:%d\n", g_target_host, g_target_port);
        mco_close(client_conn);
        mco_socket_destroy(client_conn);
        return;
    }
    printf("[proxy] Connected to target %s:%d fd=%d\n",
        g_target_host, g_target_port, mco_socket_fd(target_conn));
    mco_socket_set_nodelay(client_conn, 1);
    mco_socket_set_nodelay(target_conn, 1);
    ctx = (proxy_ctx_t*)calloc(1, sizeof(proxy_ctx_t));
    if (!ctx) {
        printf("[proxy] Memory allocation failed\n");
        mco_close(target_conn); mco_socket_destroy(target_conn);
        mco_close(client_conn); mco_socket_destroy(client_conn);
        return;
    }
    ctx->client = client_conn;
    ctx->target = target_conn;

    g_active_connections++;

    // one pump per direction; the last one to finish cleans up
    pa1 = (proxy_pump_arg_t*)malloc(sizeof(proxy_pump_arg_t));
    pa1->ctx = ctx; pa1->src = client_conn; pa1->dst = target_conn;
    pa1->buf = (char*)malloc(PROXY_BUF_SIZE); pa1->timeout = 100;
    mco_spawn(loop, proxy_pump, pa1);

    pa2 = (proxy_pump_arg_t*)malloc(sizeof(proxy_pump_arg_t));
    pa2->ctx = ctx; pa2->src = target_conn; pa2->dst = client_conn;
    pa2->buf = (char*)malloc(PROXY_BUF_SIZE); pa2->timeout = 100;
    mco_spawn(loop, proxy_pump, pa2);
}

// main listener task
static void listener_task(void* arg)
{
    mco_socket_t* listener = (mco_socket_t*)arg;
    mco_loop_t* loop = mco_vcpu_current();

    printf("[proxy] Listening on 0.0.0.0:%d\n", g_listen_port);
    printf("[proxy] Forwarding to %s:%d\n", g_target_host, g_target_port);
    printf("[proxy] Press Ctrl+C to stop\n");

    while (g_running) {
        mco_socket_t* client_conn = mco_tcp_accept(listener, 1000);
        if (!client_conn) {
            if (!g_running) break;
            continue;
        }

        // one coroutine per client connection
        mco_spawn(loop, handle_connection_task, client_conn);
    }

    mco_close(listener);
    mco_socket_destroy(listener);
    printf("[proxy] Listener stopped\n");
}

// print usage
static void show_usage(const char* prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -h HOST    Target host (default: %s)\n", TARGET_HOST);
    printf("  -p PORT    Target port (default: %d)\n", TARGET_PORT);
    printf("  -l PORT    Listen port (default: %d)\n", FORWARD_PORT);
    printf("  -?         Show this help\n");
}

int main(int argc, char* argv[])
{
    mco_loop_t* loop = NULL;
    mco_socket_t* listener = NULL;
    int i = 1;

    // parse command line
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            g_target_host = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            g_target_port = atoi(argv[++i]);
            if (g_target_port <= 0 || g_target_port > 65535) {
                printf("Invalid port: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            g_listen_port = atoi(argv[++i]);
            if (g_listen_port <= 0 || g_listen_port > 65535) {
                printf("Invalid port: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "-?") == 0) {
            show_usage(argv[0]);
            return 0;
        } else {
            printf("Unknown option: %s\n", argv[i]);
            show_usage(argv[0]);
            return 1;
        }
    }

    // install signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // create event loop
    loop = mco_loop_new();
    if (!loop) {
        printf("[proxy] Failed to create event loop\n");
        return 1;
    }

    // create listening socket
    listener = mco_tcp_listen("0.0.0.0", g_listen_port);
    if (!listener) {
        printf("[proxy] Failed to listen on port %d\n", g_listen_port);
        mco_loop_free(loop);
        return 1;
    }

    printf("[proxy] Target: %s:%d\n", g_target_host, g_target_port);

    // start listener task
    mco_spawn(loop, listener_task, listener);

    // run event loop
    mco_loop_run(loop);

    // wait for all connections to close
    while (g_active_connections > 0) {
        printf("[proxy] Waiting for %d active connections to close...\n",
            g_active_connections);
        mco_sleep(1000);
    }

    // cleanup
    mco_loop_free(loop);

    printf("[proxy] Shutdown complete\n");
    return 0;
}
