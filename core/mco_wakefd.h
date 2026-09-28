/* mco_wakefd.h — cross-thread wakeup channel: eventfd on Linux, pipe elsewhere.
 *
 * One producer writes, one consumer waits for readability (registered with the
 * engine, so a poll()-blocked vcpu wakes up). Used by mco_oneshot and the
 * workpool; not a public API.
 *
 * Why eventfd over a pipe when it is available:
 *   - setup:  1 fd / 1 syscall (eventfd) vs 2 fds / pipe()+2*fcntl (3 syscalls)
 *   - signal: a counter add instead of a byte copy into a ring buffer
 *   - drain:  one read consumes every pending signal (non-semaphore mode),
 *             a pipe needs one read per 64 pending bytes
 * eventfd is Linux-only, so other platforms keep the pipe/socketpair shape and
 * simply see rfd != wfd.
 */
#ifndef MCO_WAKEFD_H
#define MCO_WAKEFD_H

#include "hconfig.h"    /* HAVE_EVENTFD (0 or 1) */

#if defined(HAVE_EVENTFD) && HAVE_EVENTFD && defined(__linux__)
#  include <sys/eventfd.h>
#  define MCO_WAKEFD_USE_EVENTFD 1
#else
#  define MCO_WAKEFD_USE_EVENTFD 0
#endif

typedef struct mco_wakefd {
    int rfd;   /* wait for readability on this */
    int wfd;   /* signal this; equals rfd with eventfd */
} mco_wakefd_t;

/* 0 = ok. On failure both fds are -1, so "rfd >= 0" is the created test. */
int  mco_wakefd_init(mco_wakefd_t* w);

/* Close both ends (one fd with eventfd); safe to call twice. */
void mco_wakefd_fini(mco_wakefd_t* w);

/* Signal once; non-blocking, any thread. Losing a signal is impossible in
 * practice: a full pipe / saturated counter already means a wakeup is pending. */
void mco_wakefd_signal(mco_wakefd_t* w);

/* Consume every pending signal; any thread. Call before re-arming a channel so
 * a leftover signal cannot wake the next waiter spuriously. */
void mco_wakefd_drain(mco_wakefd_t* w);

#endif /* MCO_WAKEFD_H */
