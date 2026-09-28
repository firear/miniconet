/* mco_workpool.h — optional multi-vCPU work pool (MINICONET_WITH_WORKPOOL, off
 * by default): N OS threads with their own vcpu + a cross-thread task queue.
 * No migration — submit a task to run elsewhere. Share-nothing: a socket belongs
 * to the vcpu that created it. */
#ifndef MCO_WORKPOOL_H
#define MCO_WORKPOOL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mco_workpool mco_workpool_t;

/* Create n_vcpus workers (<=0 = 1). max_inflight caps unfinished tasks: <=0
 * unlimited, >0 blocks the submitting coroutine. NULL on failure. */
mco_workpool_t* mco_workpool_new(int n_vcpus, int max_inflight);

/* Stop and join all workers, then free. Waiters get MCO_ERR_CLOSED. In-flight
 * tasks are abandoned; must not race with async(). */
void mco_workpool_free(mco_workpool_t* pool);

/* Submit fn(arg) to a round-robin worker vcpu; the slot frees when fn returns.
 * Blocks the calling coroutine at the cap (coroutine-only unless
 * max_inflight<=0); callable from any thread. 0 = submitted,
 * MCO_ERR_CLOSED = pool freed, fn will not run.
 * fn must not touch another vcpu's mco_socket_t* and must not block the worker.
 * Collect results with mco_waitgroup (batch) or mco_oneshot (single value). */
int mco_workpool_async(mco_workpool_t* pool, void (*fn)(void* arg), void* arg);

#ifdef __cplusplus
}
#endif
#endif /* MCO_WORKPOOL_H */
