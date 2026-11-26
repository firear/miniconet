/* mco_workpool.h — optional multi-vCPU work pool (MINICONET_WITH_WORKPOOL, off
 * by default): N OS threads with their own vcpu + a cross-thread task queue.
 * No migration — submit a task to run elsewhere. Share-nothing: a socket belongs
 * to the vcpu that created it.
 *
 * Lifecycle (two-phase shutdown):
 *   stop()  -> refuse new tasks, keep running accepted ones
 *   drain() -> wait until every accepted task finished
 *   free()  -> stop + drain(5s) + join + release  (recommended)
 *   free_now()-> stop + abandon in-flight tasks    (escape hatch, old behaviour)
 */
#ifndef MCO_WORKPOOL_H
#define MCO_WORKPOOL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mco_workpool mco_workpool_t;

/* Create n_vcpus workers (<=0 = 1). max_inflight caps unfinished tasks: <=0
 * unlimited, >0 blocks the submitting coroutine. NULL on failure (including the
 * case where no worker thread could be started). */
mco_workpool_t* mco_workpool_new(int n_vcpus, int max_inflight);

/* Stop, wait for in-flight tasks (default 5s), join workers, then free.
 * Waiter coroutines get MCO_ERR_CLOSED. */
void mco_workpool_free(mco_workpool_t* pool);

/* Stop and join immediately; in-flight task coroutines are abandoned (they are
 * NOT resumed, so their cleanup does not run). Escape hatch only. */
void mco_workpool_free_now(mco_workpool_t* pool);

/* Submit fn(arg) to a round-robin worker vcpu; the slot frees when fn returns.
 * Blocks the calling coroutine at the cap (coroutine-only unless
 * max_inflight<=0); callable from any thread. 0 = submitted,
 * MCO_ERR_CLOSED = pool freed/stopped, MCO_ERR_BUSY = full and the caller is not
 * a coroutine (cannot park safely).
 * fn must not touch another vcpu's mco_socket_t* and must not block the worker.
 * Collect results with mco_waitgroup (batch) or mco_oneshot (single value). */
int mco_workpool_async(mco_workpool_t* pool, void (*fn)(void* arg), void* arg);

/* Non-blocking submit: never parks, never queues when the pool is full.
 * 0 = submitted; MCO_ERR_BUSY = at the inflight cap; MCO_ERR_CLOSED = stopped;
 * MCO_ERR_IO = allocation failure. Safe from any thread. */
int mco_workpool_try_async(mco_workpool_t* pool, void (*fn)(void* arg), void* arg);

/* Stop accepting new tasks (idempotent). Tasks already queued still run. */
void mco_workpool_stop(mco_workpool_t* pool);

/* Wait until every accepted task finished. timeout_ms < 0 = wait forever.
 * 0 = drained, MCO_ERR_TIMEOUT = still running tasks. Any thread. */
int mco_workpool_drain(mco_workpool_t* pool, int timeout_ms);

/* Unfinished tasks (queued + running). Any thread. */
int mco_workpool_active(mco_workpool_t* pool);

/* Workers whose thread actually started (may be < n_vcpus). Any thread. */
int mco_workpool_worker_count(mco_workpool_t* pool);

#ifdef __cplusplus
}
#endif
#endif /* MCO_WORKPOOL_H */
