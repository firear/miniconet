/*
 * mco_engine_slot.h — shared fd -> slot table for the Windows backends.
 *
 * Each backend keeps a parallel, typed array of "watches" (and, for iocp, a
 * WSAPOLLFD array) indexed by slot. The table owns only the fd<=slot mapping
 * (fds[] / free_list / n / cap) and the callbacks that grow / move the
 * backend's parallel arrays when the table changes shape.
 *
 * Two modes (t->freelist):
 *   0 (swap):     removal swaps the last used entry into the hole, so the live
 *                 region [0,n) stays tight and indexable. The backend's swap()
 *                 callback moves its parallel arrays (watches / pollfds) in
 *                 step. Used by iocp / select, whose poll loops iterate by index.
 *   1 (free-list): slot numbers are STABLE (never move). Removal only marks the
 *                 slot free. Used by wepoll so the slot index can be encoded in
 *                 the kernel event (ev.data.u32) and recovered in O(1) on poll
 *                 without a linear scan.
 *
 * find() is an O(n) dedup scan and is only called from ctl (low frequency).
 * In free-list mode, freed slots set fds[slot] = -1 (valid fds are >= 0), so
 * find() naturally skips holes.
 */
#ifndef MCO_ENGINE_SLOT_H
#define MCO_ENGINE_SLOT_H

#include <stdlib.h>
#include <string.h>

typedef struct mco_slot_table mco_slot_table_t;

struct mco_slot_table {
    int*  fds;        /* fds[slot]; -1 marks a free slot */
    int   n;          /* swap: tight used count; freelist: high-water mark */
    int   cap;        /* capacity of fds[] / free_list[] */
    int*  free_list;  /* stack of free slot indices (freelist mode) */
    int   free_top;   /* number of free slots */
    int   freelist;   /* 1 = stable slots, 0 = swap-remove (tight) */
    int  (*grow)(mco_slot_table_t* t, int newcap, void* ud);
    void (*swap)(void* ud, int a, int b);  /* move parallel arrays a<-b (swap) */
    void* ud;
};

static inline void mco_slot_table_init(mco_slot_table_t* t, int freelist,
                                int  (*grow)(mco_slot_table_t*, int, void*),
                                void (*swap)(void*, int, int),
                                void* ud) {
    t->fds = NULL;
    t->free_list = NULL;
    t->n = 0;
    t->cap = 0;
    t->free_top = 0;
    t->freelist = freelist;
    t->grow = grow;
    t->swap = swap;
    t->ud = ud;
}

static inline void mco_slot_table_free(mco_slot_table_t* t) {
    free(t->fds);
    free(t->free_list);
    t->fds = NULL;
    t->free_list = NULL;
    t->n = t->cap = t->free_top = 0;
}

/* O(n) dedup scan; valid fds are >= 0 so free holes (-1) are skipped. */
static inline int mco_slot_find(mco_slot_table_t* t, int fd) {
    int i;
    for (i = 0; i < t->n; ++i)
        if (t->fds[i] == fd) return i;
    return -1;
}

/* Find the existing slot for fd, else allocate a stable one. *out_slot is set.
 * *out_is_new (if non-NULL) is 1 on fresh allocation, 0 if an existing slot was
 * reused. Returns 0 on success, -1 on OOM. */
static inline int mco_slot_ensure(mco_slot_table_t* t, int fd, int* out_slot, int* out_is_new) {
    int slot = mco_slot_find(t, fd);
    if (slot >= 0) {
        *out_slot = slot;
        if (out_is_new) *out_is_new = 0;
        return 0;
    }
    if (t->freelist && t->free_top > 0) {
        slot = t->free_list[--t->free_top];
    } else {
        if (t->n >= t->cap) {
            int newcap = t->cap > 0 ? t->cap * 2 : 64;
            int* nf = (int*)realloc(t->fds, (size_t)newcap * sizeof(int));
            int* nl = (int*)realloc(t->free_list, (size_t)newcap * sizeof(int));
            if (!nf || !nl) return -1;
            t->fds = nf;
            t->free_list = nl;
            t->cap = newcap;
            if (t->grow && t->grow(t, newcap, t->ud) != 0) return -1;
        }
        slot = t->n++;
    }
    t->fds[slot] = fd;
    *out_slot = slot;
    if (out_is_new) *out_is_new = 1;
    return 0;
}

/* Release slot. In swap mode the last used slot is swapped into the hole and
 * t->swap(ud, slot, last) moves the backend's parallel arrays. In free-list mode
 * the slot is marked free (fds = -1) and pushed for reuse. */
static inline void mco_slot_release(mco_slot_table_t* t, int slot) {
    if (t->freelist) {
        t->fds[slot] = -1;
        t->free_list[t->free_top++] = slot;
        return;
    }
    {
        int last = t->n - 1;
        if (slot != last) {
            t->fds[slot] = t->fds[last];
            if (t->swap) t->swap(t->ud, slot, last);
        }
        t->n--;
    }
}

#endif /* MCO_ENGINE_SLOT_H */
