/* mco_oneshot.h — one-shot completion signal (wake channel + refcount).
 * Producer fires once, consumer waits once; whoever retreats releases and the
 * last release closes the wake channel and frees. fire/release from any thread,
 * wait is coroutine-only. (waitgroup = N-producer counter, same mechanism.) */
#ifndef MCO_ONESHOT_H
#define MCO_ONESHOT_H

#include "mco_vcpu.h"   /* MCO_ERR_TIMEOUT / MCO_ERR_CLOSED */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mco_oneshot mco_oneshot_t;

/* Create with refs=1. Optional cleanup runs at refs 0, before the wake channel
 * is closed. The channel is an eventfd on Linux, a pipe elsewhere. */
mco_oneshot_t* mco_oneshot_new(void);
mco_oneshot_t* mco_oneshot_new_with_cleanup(void (*cleanup)(void* ud), void* ud);

/* Take another reference. */
void mco_oneshot_retain(mco_oneshot_t* o);

/* Drop a ref; at zero: cleanup -> close(channel) -> free. Any thread. */
void mco_oneshot_release(mco_oneshot_t* o);

/* Wake the waiter; ok before wait starts (a later wait returns at once).
 * Any thread; caller holds a ref. Cheapest when the caller runs on the
 * waiter's own vcpu: then the waiter is just requeued, no syscall at all. */
void mco_oneshot_fire(mco_oneshot_t* o);

/* Coroutine-only: wait for fire. 0 = fired, MCO_ERR_TIMEOUT = timed out.
 * A fire from another thread is delivered through the wake channel, whose read
 * pairs with fire's write for happens-before; a fire from this vcpu needs no
 * such pair because the flag is published under a mutex. Does not release. */
int mco_oneshot_wait(mco_oneshot_t* o, int timeout_ms);

#ifdef __cplusplus
}
#endif
#endif /* MCO_ONESHOT_H */
