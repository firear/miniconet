/*
 * mco_oneshot.c — one-shot completion signal (wake channel + refcount).
 *
 * Producer fires once, consumer waits once; whoever retreats releases and the
 * last release closes the channel and frees. fire/release from any thread, wait
 * is coroutine-only. (waitgroup = N-producer counter, same mechanism.)
 *
 * Two wake paths, decided by fire() at the moment it runs:
 *   same vcpu  — set the flag and requeue the waiter directly: zero syscalls.
 *                The mutex is the happens-before edge for everything the
 *                producer wrote before firing.
 *   otherwise  — signal the wake channel (eventfd on Linux, pipe elsewhere),
 *                which the engine is already polling.
 * The channel is armed before parking because which path a fire will take is
 * not known in advance; the same-vcpu path only skips the write and the drain.
 */
#include "mco_oneshot.h"
#include "mco_vcpu.h"      /* mco_wait_io, mco_task_self, mco_task_wake */
#include "mco_wakefd.h"
#include "compat/hmutex.h"
#include <stdlib.h>

struct mco_oneshot {
    mco_wakefd_t   wake;      /* cross-thread channel: eventfd or pipe */
    int            refs;      /* guarded by lock */
    void (*cleanup)(void* ud);
    void*          cleanup_ud;
    hmutex_t       lock;
    /* wait state, all under lock */
    int            fired;     /* a fire is pending, not yet consumed */
    int            waiting;   /* a coroutine is parked on this oneshot */
    int            fast;      /* woken without touching the channel */
    mco_task_t*    waiter;    /* valid while waiting == 1 */
    mco_vcpu_t*    waiter_vcpu;
};

mco_oneshot_t* mco_oneshot_new_with_cleanup(void (*cleanup)(void* ud), void* ud) {
    mco_oneshot_t* o = (mco_oneshot_t*)calloc(1, sizeof(mco_oneshot_t));
    if (!o) return NULL;
    if (mco_wakefd_init(&o->wake) != 0) { free(o); return NULL; }
    o->refs = 1;
    o->cleanup = cleanup;
    o->cleanup_ud = ud;
    hmutex_init(&o->lock);
    return o;
}

mco_oneshot_t* mco_oneshot_new(void) {
    return mco_oneshot_new_with_cleanup(NULL, NULL);
}

void mco_oneshot_retain(mco_oneshot_t* o) {
    if (!o) return;
    hmutex_lock(&o->lock);
    o->refs++;
    hmutex_unlock(&o->lock);
}

void mco_oneshot_release(mco_oneshot_t* o) {
    int should_free;
    void (*cleanup)(void*);
    void* cleanup_ud;

    if (!o) return;
    hmutex_lock(&o->lock);
    o->refs--;
    should_free = (o->refs == 0);
    /* Snapshot cleanup args under the lock, run it outside. */
    cleanup = o->cleanup;
    cleanup_ud = o->cleanup_ud;
    hmutex_unlock(&o->lock);

    if (should_free) {
        if (cleanup) cleanup(cleanup_ud);
        mco_wakefd_fini(&o->wake);
        free(o);
    }
}

void mco_oneshot_fire(mco_oneshot_t* o) {
    mco_task_t* task = NULL;
    int use_channel = 1;

    if (!o) return;

    hmutex_lock(&o->lock);
    if (o->fired) {
        hmutex_unlock(&o->lock);
        return;   /* one-shot: a second fire changes nothing */
    }
    o->fired = 1;

    if (!o->waiting) {
        /* Nobody is parked: the flag is enough, wait() short-circuits on it.
         * Never signal a channel whose reader has not armed yet. */
        use_channel = 0;
    } else if (o->waiter_vcpu == mco_vcpu_self() && o->waiter != mco_task_self()) {
        /* Same thread and the waiter is really parked: requeue it directly, no
         * write and no kernel round trip. mco_task_self() != waiter also covers
         * the window where the waiter has resumed but not yet cleared the
         * state — requeuing a running task would resume it a second time. */
        task = o->waiter;
        o->fast = 1;      /* tell wait() the channel is untouched */
        use_channel = 0;
    }
    hmutex_unlock(&o->lock);

    if (task) mco_task_wake(task);
    else if (use_channel) mco_wakefd_signal(&o->wake);
}

int mco_oneshot_wait(mco_oneshot_t* o, int timeout_ms) {
    mco_task_t* task;
    mco_vcpu_t* vcpu;
    int r, fired, fast;

    if (!o) return MCO_ERR_CLOSED;

    task = mco_task_self();
    if (!task) return MCO_ERR_CLOSED;   /* coroutine-only, see the header */
    vcpu = task->vcpu;

    hmutex_lock(&o->lock);
    if (o->fired) {
        /* Already fired before we parked: consume it. No syscall at all. */
        o->fired = 0;
        o->fast = 0;
        hmutex_unlock(&o->lock);
        return 0;
    }
    o->waiting = 1;
    o->fast = 0;
    o->waiter = task;
    o->waiter_vcpu = vcpu;
    hmutex_unlock(&o->lock);

    /* Park on the channel as well: a fire from another thread can only reach us
     * through it, and which path a fire takes is not known in advance. */
    r = mco_wait_io(o->wake.rfd, MCO_EV_READ, timeout_ms);

    hmutex_lock(&o->lock);
    o->waiting = 0;
    o->waiter = NULL;
    o->waiter_vcpu = NULL;
    fired = o->fired;
    fast = o->fast;
    o->fired = 0;
    o->fast = 0;
    hmutex_unlock(&o->lock);

    /* r >= 0 is a readiness mask: only fire() ever signals this channel. */
    if (fired || r >= 0) {
        if (!fast) {
            /* Pairs with fire's write for happens-before, and resets the
             * channel (eventfd counter / pending pipe bytes) for a later wait. */
            mco_wakefd_drain(&o->wake);
        }
        return 0;
    }
    /* Timed out (or the fd went away): leave no stale signal behind. */
    mco_wakefd_drain(&o->wake);
    return r;
}
