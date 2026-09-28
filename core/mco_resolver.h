/* mco_resolver.h — async hostname resolution: getaddrinfo() runs on a small
 * thread pool, each job with a wake channel the coroutine waits on via
 * mco_wait_io(). Refcounted so a worker never touches a dead fd. */
#ifndef MCO_RESOLVER_H
#define MCO_RESOLVER_H

#include "../compat/hsocket.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MCO_RESOLVE_OK       0
#define MCO_RESOLVE_TIMEOUT (-1)
#define MCO_RESOLVE_FAILED  (-2)  /* resolution failed (invalid host / no address) */
#define MCO_RESOLVE_NOPOOL  (-3)  /* background pool creation failed (e.g. OOM) */

/* Resolve host (name or IP literal; literals take a synchronous fast path and
 * use no pool thread) into sockaddr_u. Coroutine-only: yields until done or
 * timed out without blocking the rest of the vcpu. timeout_ms<0 = wait forever. */
int mco_resolve(const char* host, sockaddr_u* out_addr, int timeout_ms);

/* Set the background pool size (default 4); only effective before the first
 * mco_resolve(). */
void mco_resolver_set_pool_size(int n);

/* Optional: reclaim the pool before exit; the OS cleans up anyway. */
void mco_resolver_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif /* MCO_RESOLVER_H */
