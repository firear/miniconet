/*
 * mco_uring.c — raw io_uring syscall ring (ABI in mco_uring.h).
 * Memory ordering: acquire for kernel-written fields (sq_head/cq_tail), release
 * for fields we publish (sq_tail/cq_head); the rest is plain local access.
 */
#if defined(__linux__)

#include "mco_uring.h"

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#ifndef SYS_io_uring_setup
#ifdef __NR_io_uring_setup
#define SYS_io_uring_setup __NR_io_uring_setup
#else
#define SYS_io_uring_setup 425  /* x86_64/arm64/riscv64 */
#endif
#endif
#ifndef SYS_io_uring_enter
#ifdef __NR_io_uring_enter
#define SYS_io_uring_enter __NR_io_uring_enter
#else
#define SYS_io_uring_enter 426
#endif
#endif

static long uring_sys_setup(unsigned entries, struct mco_io_uring_params* p) {
    return syscall(SYS_io_uring_setup, (unsigned long)entries, p);
}

static long uring_sys_enter(int fd, unsigned to_submit, unsigned min_complete, unsigned flags) {
    return syscall(SYS_io_uring_enter, (unsigned long)fd,
                   (unsigned long)to_submit, (unsigned long)min_complete,
                   (unsigned long)flags, (void*)0, (size_t)0);
}

static long uring_sys_enter_arg(int fd, unsigned to_submit, unsigned min_complete,
                                unsigned flags, const void* argp) {
    return syscall(SYS_io_uring_enter, (unsigned long)fd,
                   (unsigned long)to_submit, (unsigned long)min_complete,
                   (unsigned long)flags, argp,
                   (size_t)sizeof(struct mco_uring_getevents_arg));
}

/* acquire for kernel-written fields, release for fields we publish. */
static inline unsigned load_acq(volatile unsigned* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static inline void store_rel(volatile unsigned* p, unsigned v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

int mco_uring_create(mco_uring_t* r, unsigned entries) {
    struct mco_io_uring_params p;
    size_t sq_sz, cq_sz, sqes_sz;
    void* base;

    memset(r, 0, sizeof(*r));
    memset(&p, 0, sizeof(p));

    r->ring_fd = (int)uring_sys_setup(entries, &p);
    if (r->ring_fd < 0) return -1;

    /* NODROP (5.5+) is required: without it the kernel drops CQ events silently. */
    if (!(p.features & MCO_IORING_FEAT_NODROP)) {
        close(r->ring_fd);
        r->ring_fd = -1;
        errno = EOPNOTSUPP;
        return -1;
    }

    r->sq_tail_cached = 0;
    r->sq_head_cached = 0;
    r->features = p.features;

    sq_sz = (size_t)p.sq_off.array + (size_t)p.sq_entries * sizeof(unsigned);
    cq_sz = (size_t)p.cq_off.cqes + (size_t)p.cq_entries * sizeof(struct mco_io_uring_cqe);
    sqes_sz = (size_t)p.sq_entries * sizeof(struct mco_io_uring_sqe);

    if (p.features & MCO_IORING_FEAT_SINGLE_MMAP) {
        size_t sz = sq_sz > cq_sz ? sq_sz : cq_sz;
        base = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_POPULATE, r->ring_fd, MCO_IORING_OFF_SQ_RING);
        if (base == MAP_FAILED) { int e = errno; close(r->ring_fd); errno = e; return -1; }
        r->mmap_sq_ring = base;
        r->mmap_sq_ring_size = sz;
        r->mmap_cq_ring = base;
        r->mmap_cq_ring_size = 0;  /* same mapping as SQ; unmapped once */
    } else {
        base = mmap(NULL, sq_sz, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_POPULATE, r->ring_fd, MCO_IORING_OFF_SQ_RING);
        if (base == MAP_FAILED) { int e = errno; close(r->ring_fd); errno = e; return -1; }
        r->mmap_sq_ring = base;
        r->mmap_sq_ring_size = sq_sz;
        base = mmap(NULL, cq_sz, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_POPULATE, r->ring_fd, MCO_IORING_OFF_CQ_RING);
        if (base == MAP_FAILED) {
            int e = errno;
            munmap(r->mmap_sq_ring, r->mmap_sq_ring_size);
            close(r->ring_fd);
            errno = e;
            return -1;
        }
        r->mmap_cq_ring = base;
        r->mmap_cq_ring_size = cq_sz;
    }

    r->mmap_sqes = mmap(NULL, sqes_sz, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE, r->ring_fd, MCO_IORING_OFF_SQES);
    if (r->mmap_sqes == MAP_FAILED) {
        int e = errno;
        munmap(r->mmap_sq_ring, r->mmap_sq_ring_size);
        if (r->mmap_cq_ring_size) munmap(r->mmap_cq_ring, r->mmap_cq_ring_size);
        close(r->ring_fd);
        errno = e;
        return -1;
    }
    r->mmap_sqes_size = sqes_sz;

    {
        volatile unsigned char* sq = (volatile unsigned char*)r->mmap_sq_ring;
        volatile unsigned char* cq = (volatile unsigned char*)r->mmap_cq_ring;
        r->sq_head      = (volatile unsigned*)(sq + p.sq_off.head);
        r->sq_tail      = (volatile unsigned*)(sq + p.sq_off.tail);
        r->sq_mask      = (unsigned*)(sq + p.sq_off.ring_mask);
        r->sq_entries_p = (unsigned*)(sq + p.sq_off.ring_entries);
        r->sq_array     = (unsigned*)(sq + p.sq_off.array);
        r->cq_head      = (volatile unsigned*)(cq + p.cq_off.head);
        r->cq_tail      = (volatile unsigned*)(cq + p.cq_off.tail);
        r->cq_mask      = (unsigned*)(cq + p.cq_off.ring_mask);
        r->cq_overflow  = (unsigned*)(cq + p.cq_off.overflow);
        r->cqes         = (struct mco_io_uring_cqe*)((unsigned char*)r->mmap_cq_ring + p.cq_off.cqes);
        r->sqes         = (struct mco_io_uring_sqe*)r->mmap_sqes;
    }
    return 0;
}

void mco_uring_destroy(mco_uring_t* r) {
    if (!r || r->ring_fd < 0) return;
    close(r->ring_fd);  /* kernel cancels all in-flight requests */
    munmap(r->mmap_sq_ring, r->mmap_sq_ring_size);
    if (r->mmap_cq_ring_size) munmap(r->mmap_cq_ring, r->mmap_cq_ring_size);
    munmap(r->mmap_sqes, r->mmap_sqes_size);
    memset(r, 0, sizeof(*r));
    r->ring_fd = -1;
}

struct mco_io_uring_sqe* mco_uring_sqe_get(mco_uring_t* r) {
    unsigned head, tail, idx;

    head = r->sq_head_cached;
    tail = r->sq_tail_cached;
    if (tail - head >= *r->sq_entries_p) return NULL;  /* SQ full */
    idx = tail & *r->sq_mask;
    r->sq_array[idx] = idx;  /* SQ indirect array: no reordering */
    r->sq_tail_cached = tail + 1;
    return &r->sqes[idx];
}

int mco_uring_submit(mco_uring_t* r) {
    unsigned to_submit = r->sq_tail_cached - load_acq(r->sq_head);
    long ret;

    if (to_submit == 0) return 0;
    store_rel(r->sq_tail, r->sq_tail_cached);  /* release: SQE contents visible first */

    ret = uring_sys_enter(r->ring_fd, to_submit, 0, 0);
    if (ret < 0) {
        int e = errno;
        if (e == EINTR) return (int)to_submit;  /* a partial submit may have landed */
        if (e == EBUSY) {  /* CQ full: drain and retry once (tail cannot roll back) */
            (void)uring_sys_enter(r->ring_fd, 0, 0, MCO_IORING_ENTER_GETEVENTS);
            ret = uring_sys_enter(r->ring_fd, to_submit, 0, 0);
            if (ret < 0) {
                if (errno == EINTR) return (int)to_submit;
                return -errno;
            }
        } else {
            return -e;
        }
    }
    r->sq_head_cached = load_acq(r->sq_head);
    return (int)ret;
}

/* Drain all currently ready CQEs. */
static int reap_locked(mco_uring_t* r, mco_uring_cqe_cb cb, void* ud) {
    unsigned head = load_acq(r->cq_head);
    unsigned tail = load_acq(r->cq_tail);
    int n = 0;

    while (head != tail) {
        struct mco_io_uring_cqe* cqe = &r->cqes[head & *r->cq_mask];
        if (cb) cb(ud, cqe);
        head++;
        n++;
        store_rel(r->cq_head, head);  /* publish head so the kernel sees progress */
        tail = load_acq(r->cq_tail);
    }
    return n;
}

int mco_uring_reap_all(mco_uring_t* r, mco_uring_cqe_cb cb, void* ud) {
    /* NODROP: the kernel buffers overflowed events and enter(GETEVENTS) moves
     * them back. The overflow counter is more portable than flags. */
    if (*r->cq_overflow) {
        (void)uring_sys_enter(r->ring_fd, 0, 0, MCO_IORING_ENTER_GETEVENTS);
    }
    return reap_locked(r, cb, ud);
}

int mco_uring_wait_one(mco_uring_t* r, struct mco_io_uring_cqe* out, int timeout_ms) {
    unsigned head, tail;
    long ret;

    if (timeout_ms == 0) {
        head = load_acq(r->cq_head);
        tail = load_acq(r->cq_tail);
        if (head == tail) return 0;
    } else {
        unsigned flags = MCO_IORING_ENTER_GETEVENTS;
        ret = uring_sys_enter(r->ring_fd, 0, 1, flags);
        if (ret < 0) {
            if (errno == EINTR) return 0;
            return -errno;
        }
    }
    head = load_acq(r->cq_head);
    tail = load_acq(r->cq_tail);
    if (head == tail) return 0;
    *out = r->cqes[head & *r->cq_mask];
    store_rel(r->cq_head, head + 1);
    return 1;
}

int mco_uring_enter_wait(mco_uring_t* r, struct mco_io_uring_cqe* out,
                         int timeout_ms, const struct mco_kernel_timespec* ts) {
    unsigned to_submit = r->sq_tail_cached - load_acq(r->sq_head);
    unsigned head, tail;
    long ret;

    store_rel(r->sq_tail, r->sq_tail_cached);  /* release: SQE contents visible first */

    /* EXT_ARG only with SQEs in flight: some kernels EINVAL on to_submit=0.
     * Retry without it on EINVAL. */
    if (ts && timeout_ms >= 0 && to_submit > 0) {
        struct mco_uring_getevents_arg arg;
        memset(&arg, 0, sizeof(arg));
        arg.ts = (mco_uring_u64)(uintptr_t)ts;
        ret = uring_sys_enter_arg(r->ring_fd, to_submit, 1,
                                  MCO_IORING_ENTER_GETEVENTS | MCO_IORING_ENTER_EXT_ARG, &arg);
        if (ret < 0 && errno == EINVAL)
            ret = uring_sys_enter(r->ring_fd, to_submit, 1, MCO_IORING_ENTER_GETEVENTS);
    } else {
        ret = uring_sys_enter(r->ring_fd, to_submit, 1, MCO_IORING_ENTER_GETEVENTS);
    }
    if (ret < 0) {
        int err = errno;
        if (err == EINTR) return 0;
        if (err == ETIME) return 0;  /* EXT_ARG timeout, no events */
        if (err == EBUSY) {  /* CQ full: drain and retry once */
            (void)uring_sys_enter(r->ring_fd, 0, 0, MCO_IORING_ENTER_GETEVENTS);
            ret = uring_sys_enter(r->ring_fd, to_submit, 1, MCO_IORING_ENTER_GETEVENTS);
            if (ret < 0) {
                err = errno;
                if (err == EINTR || err == ETIME) return 0;
                return -err;
            }
        } else {
            return -err;
        }
    }
    r->sq_head_cached = load_acq(r->sq_head);

    head = load_acq(r->cq_head);
    tail = load_acq(r->cq_tail);
    if (head == tail) return 0;
    *out = r->cqes[head & *r->cq_mask];
    store_rel(r->cq_head, head + 1);
    return 1;
}

#endif /* __linux__ */
