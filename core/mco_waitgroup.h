/* mco_waitgroup.h — wait for a batch of tasks (counter + oneshot fired at 0).
 * new -> add -> submit -> wait -> free; free only after wait returns with no
 * done() in flight. */
#ifndef MCO_WAITGROUP_H
#define MCO_WAITGROUP_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mco_waitgroup mco_waitgroup_t;

mco_waitgroup_t* mco_waitgroup_new(void);
/* Must be called after wait returns and no done() is in flight. */
void mco_waitgroup_free(mco_waitgroup_t* wg);

/* Add delta to the counter (usually 1; may be negative, like sync.WaitGroup).
 * Must precede submit and must not race with done/wait. */
void mco_waitgroup_add(mco_waitgroup_t* wg, int delta);

/* Call before a task finishes; callable from any thread. Fires done_signal when
 * the count reaches 0. */
void mco_waitgroup_done(mco_waitgroup_t* wg);

/* Coroutine-only: wait for the count to reach zero. Returns 0 = all done /
 * MCO_ERR_TIMEOUT / MCO_ERR_CLOSED. */
int mco_waitgroup_wait(mco_waitgroup_t* wg, int timeout_ms);

#ifdef __cplusplus
}
#endif
#endif /* MCO_WAITGROUP_H */
