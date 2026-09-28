/*
 * mco_waitgroup.c — wait for a batch of tasks (counter + oneshot fired at 0).
 */
#include "mco_waitgroup.h"
#include "mco_oneshot.h"
#include "../compat/hatomic.h"
#include <stdlib.h>

struct mco_waitgroup {
    hatomic_t pending;       /* atomic_long */
    mco_oneshot_t* done_sig;
};

mco_waitgroup_t* mco_waitgroup_new(void) {
    mco_waitgroup_t* wg = (mco_waitgroup_t*)calloc(1, sizeof(mco_waitgroup_t));
    if (!wg) return NULL;
    wg->pending = 0;
    wg->done_sig = mco_oneshot_new();
    if (!wg->done_sig) { free(wg); return NULL; }
    return wg;
}

void mco_waitgroup_free(mco_waitgroup_t* wg) {
    if (!wg) return;
    /* Caller must ensure no done() is in flight. */
    mco_oneshot_release(wg->done_sig);
    free(wg);
}

void mco_waitgroup_add(mco_waitgroup_t* wg, int delta) {
    if (!wg) return;
    /* add before submit; a race with done() could fire at 0 early. */
    (void)hatomic_add(&wg->pending, delta);
}

void mco_waitgroup_done(mco_waitgroup_t* wg) {
    long prev;
    if (!wg) return;
    /* hatomic_dec returns the previous value; prev==1 means it hit 0. */
    prev = hatomic_dec(&wg->pending);
    if (prev == 1) {
        mco_oneshot_fire(wg->done_sig);
    }
}

int mco_waitgroup_wait(mco_waitgroup_t* wg, int timeout_ms) {
    if (!wg) return MCO_ERR_CLOSED;
    return mco_oneshot_wait(wg->done_sig, timeout_ms);
}
