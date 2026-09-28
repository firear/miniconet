/*
 * mco_resolver.c — async name resolution (thread pool + mco_oneshot).
 * A job holds host/addr/err plus an mco_oneshot_t*; the wake channel and
 * refcount teardown live in mco_oneshot, and at refs 0 cleanup frees the job.
 */
#include "mco_resolver.h"
#include "mco_oneshot.h"
#include "../compat/hthread.h"
#include "../compat/hmutex.h"
#include <stdlib.h>
#include <string.h>

typedef struct mco_resolve_job {
    char host[256];
    sockaddr_u addr;
    int err;                       /* 0 = ok, else ResolveAddr failed */
    mco_oneshot_t* sig;            /* completion signal; its refs also own the job */
    struct mco_resolve_job* next;
} mco_resolve_job_t;

static hmutex_t   g_mutex;
static hcondvar_t g_cond;
static mco_resolve_job_t* g_queue_head = NULL;
static mco_resolve_job_t* g_queue_tail = NULL;
static int g_pool_started = 0;
static int g_pool_size = 4;
static int g_shutdown = 0;

/* cleanup at refs 0: free the job (oneshot frees itself). */
static void job_free_fn(void* ud) {
    free(ud);
}

static HTHREAD_ROUTINE(resolver_worker_main) {
    (void)userdata;
    for (;;) {
        mco_resolve_job_t* job;

        hmutex_lock(&g_mutex);
        while (!g_queue_head && !g_shutdown) {
            hcondvar_wait(&g_cond, &g_mutex);
        }
        if (g_shutdown && !g_queue_head) {
            hmutex_unlock(&g_mutex);
            break;
        }
        job = g_queue_head;
        g_queue_head = job->next;
        if (!g_queue_head) g_queue_tail = NULL;
        hmutex_unlock(&g_mutex);

        /* the blocking call, off any vcpu */
        job->err = ResolveAddr(job->host, &job->addr);

        /* Write the result before fire: that pairs with the waiter's read, so it
         * can read err/addr unlocked. */
        mco_oneshot_fire(job->sig);

        mco_oneshot_release(job->sig); /* the worker's reference is done */
    }
    return 0;
}

static honce_t g_once = HONCE_INIT;

static void resolver_init_once(void) {
    hmutex_init(&g_mutex);
    hcondvar_init(&g_cond);
}

static int resolver_pool_init(void) {
    int i;
    /* honce(): init runs once and is visible to all threads */
    honce(&g_once, resolver_init_once);

    hmutex_lock(&g_mutex);
    if (g_pool_started) { hmutex_unlock(&g_mutex); return 1; }
    for (i = 0; i < g_pool_size; ++i) {
        uv_thread_options_t opt;
        hthread_t th;
        opt.flags = UV_THREAD_NO_FLAGS;
        opt.stack_size = 0;
        th = hthread_create(&opt, resolver_worker_main, NULL);
        hthread_detach(th); /* no join; shutdown signals the workers */
    }
    g_pool_started = 1;
    hmutex_unlock(&g_mutex);
    return 1;
}

void mco_resolver_set_pool_size(int n) {
    if (n > 0 && !g_pool_started) g_pool_size = n;
}

void mco_resolver_shutdown(void) {
    if (!g_pool_started) return; /* pool never started, g_mutex uninitialized */
    hmutex_lock(&g_mutex);
    g_shutdown = 1;
    hcondvar_broadcast(&g_cond);
    hmutex_unlock(&g_mutex);
    /* detached workers: signal only, no join */
}

int mco_resolve(const char* host, sockaddr_u* out_addr, int timeout_ms) {
    mco_resolve_job_t* job;
    int r;

    if (!host || !*host) return MCO_RESOLVE_FAILED;

    /* IP literals resolve locally; no pool thread, no vcpu impact */
    if (is_ipaddr(host)) {
        if (sockaddr_set_ip(out_addr, host) == 0) return MCO_RESOLVE_OK;
        return MCO_RESOLVE_FAILED;
    }

    if (!resolver_pool_init()) return MCO_RESOLVE_NOPOOL;

    job = (mco_resolve_job_t*)calloc(1, sizeof(mco_resolve_job_t));
    if (!job) return MCO_RESOLVE_NOPOOL;
    strncpy(job->host, host, sizeof(job->host) - 1);
    /* refs=1 with cleanup; retain one for the worker, so either may finish first */
    job->sig = mco_oneshot_new_with_cleanup(job_free_fn, job);
    if (!job->sig) { free(job); return MCO_RESOLVE_NOPOOL; }
    mco_oneshot_retain(job->sig); /* refs=2: caller + worker */

    hmutex_lock(&g_mutex);
    job->next = NULL;
    if (g_queue_tail) g_queue_tail->next = job; else g_queue_head = job;
    g_queue_tail = job;
    hcondvar_signal(&g_cond);
    hmutex_unlock(&g_mutex);

    r = mco_oneshot_wait(job->sig, timeout_ms);

    if (r == MCO_ERR_TIMEOUT) {
        /* Timed out: do not read the result (the worker may still write); just
         * drop our reference. */
        mco_oneshot_release(job->sig);
        return MCO_RESOLVE_TIMEOUT;
    }
    if (r == MCO_ERR_CLOSED) {
        /* cannot happen */
        mco_oneshot_release(job->sig);
        return MCO_RESOLVE_FAILED;
    }

    /* paired with fire's write: safe to read the result */
    if (job->err == 0) *out_addr = job->addr;
    {
        int err = job->err;
        mco_oneshot_release(job->sig); /* caller's reference done; refs 0 frees */
        return err == 0 ? MCO_RESOLVE_OK : MCO_RESOLVE_FAILED;
    }
}
