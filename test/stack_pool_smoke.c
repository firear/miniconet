/* Verify coroutine stack pooling: spawn 3 batches of 50 do-nothing coroutines;
 * mco_vcpu_run() runs until a batch drains, so stacks freed by batch 1 are reused
 * by batches 2/3 instead of malloc'ed.
 * Control run with the limit at 0: reuse must be 0, proving the switch works.
 */
#include "../core/mco_vcpu.h"
#include <stdio.h>

/* Pooling auto-disables under AddressSanitizer (see pool_alloc/pool_dealloc in
 * core/mco_vcpu.c); same detection macro here so ASan does not false-fail. */
#if defined(__has_feature)
  #if __has_feature(address_sanitizer)
    #define STACK_POOL_TEST_ASAN_BUILD 1
  #endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(STACK_POOL_TEST_ASAN_BUILD)
  #define STACK_POOL_TEST_ASAN_BUILD 1
#endif
#ifndef STACK_POOL_TEST_ASAN_BUILD
  #define STACK_POOL_TEST_ASAN_BUILD 0
#endif

#define BATCHES 3
#define PER_BATCH 50

static void noop_task(void* arg) {
    (void)arg; /* do nothing, exit immediately */
}

static void run_batches(mco_vcpu_t* vcpu) {
    int b, i;
    for (b = 0; b < BATCHES; ++b) {
        for (i = 0; i < PER_BATCH; ++i) mco_go(vcpu, noop_task, NULL);
        mco_vcpu_run(vcpu); /* returns when this batch is fully done */
    }
}

int main(void) {
    mco_vcpu_t* vcpu;
    long reuse_with_pool, reuse_without_pool;
    int ok;

    /* pass 1: pooling on (default) */
    vcpu = mco_vcpu_new(NULL);
    run_batches(vcpu);
    reuse_with_pool = mco_vcpu_stack_pool_reuse_count(vcpu);
    mco_vcpu_free(vcpu);
    printf("[pool on ] reuse_count=%ld (expect > 0, close to %d; under ASan pool auto-disabled, expect ==0)\n",
           reuse_with_pool, (BATCHES - 1) * PER_BATCH);

    /* pass 2: pooling explicitly off, as control */
    vcpu = mco_vcpu_new(NULL);
    mco_vcpu_set_stack_pool_limit(vcpu, 0);
    run_batches(vcpu);
    reuse_without_pool = mco_vcpu_stack_pool_reuse_count(vcpu);
    mco_vcpu_free(vcpu);
    printf("[pool off] reuse_count=%ld (expect == 0)\n", reuse_without_pool);

#if STACK_POOL_TEST_ASAN_BUILD
    ok = (reuse_with_pool == 0) && (reuse_without_pool == 0);
#else
    ok = (reuse_with_pool > 0) && (reuse_without_pool == 0);
#endif
    printf(ok ? "STACK POOL TEST PASSED\n" : "STACK POOL TEST FAILED\n");
    return ok ? 0 : 1;
}
