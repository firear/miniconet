/*
 * engine_fallback_smoke.c — two contracts around default backend selection.
 *
 * 1. mco_vcpu_new(NULL) uses the default backend and must not fail just because
 *    the preferred backend cannot start (io_uring on an old kernel / seccomp);
 *    mco_engine_default_fallback() retries on a readiness engine.
 * 2. A backend the caller pinned itself is NEVER substituted. This is what lets
 *    uring_async_smoke.c really test io_uring: if a failed pinned backend were
 *    swapped for epoll, that test would pass without touching io_uring at all.
 *
 * The fallback path itself needs a kernel that refuses io_uring, so it is
 * covered by review; the pinned rule is covered here.
 */
#include "../miniconet.h"
#include "../core/mco_engine.h"
#include "../core/mco_vcpu.h"
#include <stdio.h>

/* A backend that always refuses to start. */
static mco_engine_t* broken_create(void) { return NULL; }
static void broken_destroy(mco_engine_t* e) { (void)e; }
static int broken_ctl(mco_engine_t* e, int fd, int events, mco_engine_cb cb, void* ud) {
    (void)e; (void)fd; (void)events; (void)cb; (void)ud;
    return 0;
}
static int broken_poll(mco_engine_t* e, int timeout_ms) {
    (void)e; (void)timeout_ms;
    return 0;
}

static const mco_engine_ops_t s_broken_ops = {
    "broken", broken_create, broken_destroy, broken_ctl, broken_poll,
    NULL,   /* wake */
    NULL    /* async */
};

int main(void) {
    mco_loop_t* default_loop;
    mco_loop_t* pinned_loop;
    int ok_default, refused_pinned, pass;

    /* Contract 1: the default backend must yield a usable loop. */
    default_loop = mco_loop_new();
    ok_default = (default_loop != NULL);

    /* Contract 2: pinning a backend that cannot start must fail, not fall back. */
    pinned_loop = mco_vcpu_new(&s_broken_ops);
    refused_pinned = (pinned_loop == NULL);
    if (pinned_loop) mco_loop_free(pinned_loop);

    pass = ok_default && refused_pinned;
    printf("engine_fallback_smoke: default_ok=%d pinned_refused=%d\n",
           ok_default, refused_pinned);

    if (default_loop) mco_loop_free(default_loop);

    printf(pass ? "ENGINE FALLBACK TEST PASSED\n" : "ENGINE FALLBACK TEST FAILED\n");
    return pass ? 0 : 1;
}
