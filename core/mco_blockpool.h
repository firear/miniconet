/* mco_blockpool.h — blocking-call offload pool (MINICONET_WITH_BLOCKPOOL, off by
 * default): N plain OS threads (no vcpu) execute blocking functions such as
 * file/socket IO, so a coroutine on a vcpu never stalls its event loop.
 *
 * The submitter stays a coroutine: it parks on a one-shot and is woken by the
 * worker thread through the wake channel (fire from any thread is supported).
 * A vcpu therefore keeps serving its other connections while the blocking call
 * runs elsewhere — the uv_queue_work model.
 *
 * Lifecycle: stop() -> drain() -> free(); free() = stop + drain(5s) + join.
 */
#ifndef MCO_BLOCKPOOL_H
#define MCO_BLOCKPOOL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mco_blockpool mco_blockpool_t;

/* n_threads workers (<=0 = 1). queue_cap caps unfinished tasks (<=0 = unlimited;
 * beyond it run() returns MCO_ERR_BUSY instead of queueing). NULL on failure. */
mco_blockpool_t* mco_blockpool_new(int n_threads, int queue_cap);

/* Stop, wait for accepted tasks (5s), join workers, free. */
void mco_blockpool_free(mco_blockpool_t* bp);

/* Stop accepting new tasks (idempotent); accepted tasks still run. */
void mco_blockpool_stop(mco_blockpool_t* bp);

/* Wait until every accepted task finished. timeout_ms < 0 = forever.
 * 0 = drained, MCO_ERR_TIMEOUT = still running. Any thread. */
int mco_blockpool_drain(mco_blockpool_t* bp, int timeout_ms);

/* Coroutine-only: run fn(arg) on a worker thread and park until it returns.
 * fn must not touch mco_socket_t / vcpu state — it is a plain thread.
 * 0 = done, MCO_ERR_TIMEOUT = gave up waiting (fn keeps running to completion),
 * MCO_ERR_CLOSED = pool stopped, MCO_ERR_BUSY = queue full,
 * MCO_ERR_IO = allocation failure, MCO_ERR_ADDR = called outside a coroutine. */
int mco_blockpool_run(mco_blockpool_t* bp, void (*fn)(void* arg), void* arg,
                      int timeout_ms);

/* Unfinished tasks (queued + running). Any thread. */
int mco_blockpool_pending(mco_blockpool_t* bp);

/* Workers whose thread actually started. Any thread. */
int mco_blockpool_worker_count(mco_blockpool_t* bp);

#ifdef __cplusplus
}
#endif
#endif /* MCO_BLOCKPOOL_H */
