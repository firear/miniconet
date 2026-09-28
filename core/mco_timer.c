#include "mco_timer.h"
#include "../compat/htime.h"

static int timer_less(const struct heap_node* a, const struct heap_node* b) {
    mco_timer_t* ta = container_of(a, mco_timer_t, node);
    mco_timer_t* tb = container_of(b, mco_timer_t, node);
    return ta->deadline_us < tb->deadline_us;
}

void mco_timer_heap_init(mco_timer_heap_t* h) {
    heap_init(&h->h, timer_less);
}

/* ---- Embedded-node API (see mco_timer.h) ---- */

void mco_timer_init(mco_timer_t* t) {
    t->cb = NULL;   /* cb==NULL == inactive */
    t->ud = NULL;
}

void mco_timer_start(mco_timer_heap_t* h, mco_timer_t* t, uint32_t ms, mco_timer_cb cb, void* ud) {
    t->deadline_us = gethrtime_us() + (uint64_t)ms * 1000ULL;
    t->cb = cb;
    t->ud = ud;
    heap_insert(&h->h, &t->node);
}

void mco_timer_stop(mco_timer_heap_t* h, mco_timer_t* t) {
    if (!t || !t->cb) return;   /* inactive/fired: no-op */
    heap_remove(&h->h, &t->node);
    t->cb = NULL;
    t->ud = NULL;
}

int mco_timer_heap_next_timeout_ms(mco_timer_heap_t* h) {
    mco_timer_t* nearest;
    uint64_t now, diff_us;
    if (h->h.nelts == 0 || h->h.root == NULL) return -1;
    nearest = container_of(h->h.root, mco_timer_t, node);
    now = gethrtime_us();
    if (nearest->deadline_us <= now) return 0;
    diff_us = nearest->deadline_us - now;
    return (int)((diff_us + 999) / 1000); /* round up to ms */
}

void mco_timer_heap_process(mco_timer_heap_t* h) {
    uint64_t now;
    /* Check for an empty heap before reading the clock: with no timers this is
     * called once per poll round and the clock read would be pure overhead. */
    if (h->h.nelts == 0 || h->h.root == NULL) return;
    now = gethrtime_us();
    for (;;) {
        mco_timer_t* t;
        mco_timer_cb cb;
        void* ud;
        if (h->h.nelts == 0 || h->h.root == NULL) break;
        t = container_of(h->h.root, mco_timer_t, node);
        if (t->deadline_us > now) break;
        heap_dequeue(&h->h);
        /* Clear active first: cb may restart the same node. */
        cb = t->cb;
        ud = t->ud;
        t->cb = NULL;
        t->ud = NULL;
        if (cb) cb(ud);
    }
}

mco_deadline_t mco_deadline_after(int timeout_ms) {
    if (timeout_ms < 0) return MCO_DEADLINE_NONE;
    return (mco_deadline_t)(gethrtime_us() + (uint64_t)timeout_ms * 1000ULL);
}

int mco_deadline_ms(mco_deadline_t dl) {
    uint64_t now_us;
    int64_t diff_us;
    if (dl == MCO_DEADLINE_NONE) return -1;
    now_us = gethrtime_us();
    diff_us = dl - (int64_t)now_us;
    if (diff_us <= 0) return 0;
    return (int)((diff_us + 999) / 1000); /* round up to ms */
}
