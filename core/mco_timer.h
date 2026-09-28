/* mco_timer.h — per-vCPU timer heap; backs mco_sleep() and io timeouts. */
#ifndef MCO_TIMER_H
#define MCO_TIMER_H

#include "../compat/hdef.h"
#include "../compat/heap.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*mco_timer_cb)(void* ud);

typedef struct mco_timer {
    struct heap_node node;
    uint64_t         deadline_us;
    mco_timer_cb     cb;   /* NULL = inactive */
    void*            ud;
} mco_timer_t;

typedef struct mco_timer_heap {
    struct heap h;
} mco_timer_heap_t;

void mco_timer_heap_init(mco_timer_heap_t* h);

/* ---- Embedded-node API (no allocation): caller embeds the node; start/stop
 * only touch the heap. "active" == cb != NULL. ---- */

/* Initialize as inactive. */
void mco_timer_init(mco_timer_t* t);

/* Whether active (cb != NULL). */
static inline int mco_timer_active(const mco_timer_t* t) { return t->cb != NULL; }

/* Start an embedded timer (must be inactive; a task reuses one node serially). */
void mco_timer_start(mco_timer_heap_t* h, mco_timer_t* t, uint32_t ms, mco_timer_cb cb, void* ud);

/* Stop: safe no-op if already fired or inactive. */
void mco_timer_stop(mco_timer_heap_t* h, mco_timer_t* t);

/* ms to the next timer (engine->poll timeout); -1 = none. */
int mco_timer_heap_next_timeout_ms(mco_timer_heap_t* h);

/* Fire every expired timer: invoke its cb, then remove it from the heap. */
void mco_timer_heap_process(mco_timer_heap_t* h);

/* ---- Absolute deadline helpers: share one budget across calls ----
 *   mco_deadline_t dl = mco_deadline_after(3000);
 *   mco_tcp_connect(host, port, mco_deadline_ms(dl), NULL);
 *   mco_read(conn, buf, len, mco_deadline_ms(dl));
 * Per-socket defaults: mco_socket_set_timeout() + mco_read_default(). */
typedef int64_t mco_deadline_t;
#define MCO_DEADLINE_NONE ((mco_deadline_t)-1)

/* now + timeout_ms; <0 gives MCO_DEADLINE_NONE (= wait forever). */
mco_deadline_t mco_deadline_after(int timeout_ms);

/* ms left until dl; -1 for MCO_DEADLINE_NONE. */
int mco_deadline_ms(mco_deadline_t dl);

#ifdef __cplusplus
}
#endif
#endif /* MCO_TIMER_H */
