/*
 * mco_uring.h — raw io_uring syscall ring: no liburing, no <linux/io_uring.h>
 * (the ABI is defined here for old sysroots). Syscall numbers fixed at
 * 425/426/427. Required features, else creation fails and the caller falls back
 * to epoll: SINGLE_MMAP (5.4+) one mmap for SQ/CQ; NODROP (5.5+) CQ overflow
 * is buffered instead of silently dropping events.
 */
#ifndef MCO_URING_H
#define MCO_URING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Minimal ABI (matches kernel uapi) ---- */

typedef unsigned long long mco_uring_u64;
typedef int mco_uring_s32;
typedef unsigned int mco_uring_u32;
typedef unsigned char mco_uring_u8;
typedef unsigned short mco_uring_u16;

struct mco_io_uring_sqe {
    mco_uring_u8  opcode;
    mco_uring_u8  flags;
    mco_uring_u16 ioprio;
    mco_uring_s32 fd;
    mco_uring_u64 off;      /* union: only off used; address length for CONNECT */
    mco_uring_u64 addr;
    mco_uring_u32 len;
    mco_uring_u32 op_flags; /* union: rw_flags/poll_events/accept_flags by opcode */
    mco_uring_u64 user_data;
    mco_uring_u16 buf_group;
    mco_uring_u64 pad2[2];
};  /* sizeof == 64 */

struct mco_io_uring_cqe {
    mco_uring_u64 user_data;
    mco_uring_s32 res;            /* >=0 success; <0 = -errno */
    mco_uring_u32 flags;
};

struct mco_io_sqring_offsets {
    mco_uring_u32 head;
    mco_uring_u32 tail;
    mco_uring_u32 ring_mask;
    mco_uring_u32 ring_entries;
    mco_uring_u32 flags;
    mco_uring_u32 dropped;
    mco_uring_u32 array;
    mco_uring_u32 resv1;
    mco_uring_u64 resv2;
};

struct mco_io_cqring_offsets {
    mco_uring_u32 head;
    mco_uring_u32 tail;
    mco_uring_u32 ring_mask;
    mco_uring_u32 ring_entries;
    mco_uring_u32 overflow;
    mco_uring_u32 cqes;
    mco_uring_u32 flags;
    mco_uring_u32 resv1;
    mco_uring_u64 resv2;
};

struct mco_io_uring_params {
    mco_uring_u32 sq_entries;
    mco_uring_u32 cq_entries;
    mco_uring_u32 flags;
    mco_uring_u32 sq_thread_cpu;
    mco_uring_u32 sq_thread_idle;
    mco_uring_u32 features;
    mco_uring_u32 wq_fd;
    mco_uring_u32 resv[3];
    struct mco_io_sqring_offsets sq_off;
    struct mco_io_cqring_offsets cq_off;
};

/* opcode (values per kernel 5.x ABI) */
#define MCO_IORING_OP_POLL_ADD      6u   /* 5.1 */
#define MCO_IORING_OP_POLL_REMOVE   7u   /* 5.2 */
#define MCO_IORING_OP_TIMEOUT      11u   /* 5.1 */
#define MCO_IORING_OP_ACCEPT       13u   /* 5.1 */
#define MCO_IORING_OP_ASYNC_CANCEL 14u   /* 5.5 */
#define MCO_IORING_OP_CONNECT      16u   /* 5.1 */
#define MCO_IORING_OP_READ         22u   /* 5.6 */
#define MCO_IORING_OP_WRITE        23u   /* 5.6 */

/* enter flags: GETEVENTS=bit0 / SQ_WAIT=bit2 / EXT_ARG=bit3 */
#define MCO_IORING_ENTER_GETEVENTS 1u
#define MCO_IORING_ENTER_EXT_ARG   8u  /* argp = struct mco_uring_getevents_arg (5.11+) */

/* mmap offsets */
#define MCO_IORING_OFF_SQ_RING 0ULL
#define MCO_IORING_OFF_CQ_RING 0x8000000ULL
#define MCO_IORING_OFF_SQES    0x10000000ULL

/* feature bits */
#define MCO_IORING_FEAT_SINGLE_MMAP 0x00000001u
#define MCO_IORING_FEAT_NODROP      0x00000002u
#define MCO_IORING_FEAT_EXT_ARG     0x00000100u  /* enter may take a kernel-side timeout (5.11+) */

/* TIMEOUT timespec (kernel ABI: two s64, not glibc timespec) */
struct mco_kernel_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

/* EXT_ARG block (kernel uapi io_uring_getevents_arg): kernel-side timeout
 * replacing a TIMEOUT SQE. */
struct mco_uring_getevents_arg {
    mco_uring_u64 sigmask;
    mco_uring_u32 sigmask_sz;
    mco_uring_u32 pad;
    mco_uring_u64 ts;
};

/* ---- ring object ---- */

/* Hot fields (ring_fd, cached counters, SQ/CQ pointers) first, cold ones last. */
typedef struct mco_uring {
    int ring_fd;
    unsigned           sq_tail_cached;   /* cached tail; flushed on submit */
    unsigned           sq_head_cached;   /* avoids reading the kernel head */
    unsigned           features;         /* feature bits from setup */

    /* SQ ring (kernel writes head) */
    volatile unsigned* sq_head;
    volatile unsigned* sq_tail;
    unsigned*          sq_mask;
    struct mco_io_uring_sqe* sqes;

    /* CQ ring (kernel writes tail) */
    volatile unsigned* cq_head;
    volatile unsigned* cq_tail;
    unsigned*          cq_mask;
    struct mco_io_uring_cqe* cqes;

    unsigned*          sq_entries_p;
    unsigned*          sq_array;
    unsigned*          cq_overflow;

    /* mmap records, unmapped on destroy */
    void* mmap_sq_ring;
    void* mmap_cq_ring;
    void* mmap_sqes;
    size_t mmap_sq_ring_size;
    size_t mmap_cq_ring_size;
    size_t mmap_sqes_size;
} mco_uring_t;

/* Create the ring; entries is the SQ depth (rounded to a power of 2).
 * -1 with errno set: ENOSYS no io_uring, EPERM blocked by seccomp. */
int mco_uring_create(mco_uring_t* r, unsigned entries);

void mco_uring_destroy(mco_uring_t* r);

/* Get a free SQE slot; NULL when the SQ is full. */
struct mco_io_uring_sqe* mco_uring_sqe_get(mco_uring_t* r);

/* Submit filled SQEs to the kernel. Returns the count or <0 (-errno). */
int mco_uring_submit(mco_uring_t* r);

/* Wait for one completion (timeout_ms<0 forever, 0 = poll). 1 = cqe filled,
 * 0 = none, <0 = -errno (EINTR reports 0). */
int mco_uring_wait_one(mco_uring_t* r, struct mco_io_uring_cqe* out, int timeout_ms);

/* Submit + wait in one enter. With to_submit>0 and ts != NULL the timeout goes
 * to the kernel via EXT_ARG (some kernels EINVAL on to_submit=0). Same return
 * as wait_one; ETIME counts as "no event" (0). */
int mco_uring_enter_wait(mco_uring_t* r, struct mco_io_uring_cqe* out,
                         int timeout_ms, const struct mco_kernel_timespec* ts);

/* SQEs pending submission */
static inline unsigned mco_uring_pending(const mco_uring_t* r) {
    return r->sq_tail_cached - __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);
}

/* Drain all ready CQEs via on_cqe(ud, cqe). Returns the count or <0. */
typedef void (*mco_uring_cqe_cb)(void* ud, const struct mco_io_uring_cqe* cqe);
int mco_uring_reap_all(mco_uring_t* r, mco_uring_cqe_cb cb, void* ud);

#ifdef __cplusplus
}
#endif
#endif /* MCO_URING_H */
