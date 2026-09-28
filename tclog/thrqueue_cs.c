/*
 * SSLsplit - transparent and scalable SSL/TLS interception
 * Copyright (c) 2009-2014, Daniel Roethlisberger <daniel@roe.ch>
 * All rights reserved.
 * http://www.roe.ch/SSLsplit
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "thrqueue.h"
#include "tclog_perf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>       /* clock_gettime / struct timespec（timedwait 用） */

#if __unix
#include <pthread.h>
#include <sched.h>      /* sched_yield()，原先靠隐式声明，C99 起是未定义行为 */
#include <errno.h>
#elif defined(_WIN32) || defined(_WIN64)
#include "compat/tclog_win32.h"     /* 自带条件变量：XP 也要能跑，见该文件头注释 */
#define pthread_mutex_t CRITICAL_SECTION
#define pthread_cond_t tclog_cv_t
int pthread_mutex_init(void* a, void* b)
{
    InitializeCriticalSection((LPCRITICAL_SECTION)a);
    return 0;
}
int pthread_cond_init(void* a, void* b)
{
    tclog_cv_init((tclog_cv_t*)a);
    return 0;
}
#define pthread_cond_destroy(a) tclog_cv_destroy(a)
#define pthread_mutex_destroy(a) DeleteCriticalSection(a)
#define pthread_mutex_lock(a) EnterCriticalSection(a)
#define pthread_mutex_unlock(a) LeaveCriticalSection(a)
#define pthread_cond_wait(a, b) tclog_cv_wait(a, b, INFINITE)
#define pthread_cond_broadcast(a) tclog_cv_broadcast(a)
#define pthread_cond_signal(b) tclog_cv_signal(b)
#if defined(_MSC_VER) && _MSC_VER < 1400
#define sched_yield() Sleep(0)      /* VC6 的 windows.h 没有声明 SwitchToThread */
#else
#define sched_yield() SwitchToThread()
#endif
#endif

struct thrqueue {
    void** data;
    size_t sz, n;
    size_t in, out;
    unsigned int block_enqueue : 1;
    unsigned int block_dequeue : 1;
    pthread_mutex_t mutex;
    pthread_cond_t notempty;
    pthread_cond_t notfull;
};

/*
 * Threadsafe, bounded-size queue based on pthreads mutex and conds.
 * Both enqueue and dequeue are available in a blocking and non-blocking
 * version.
 */

/*
 * Create a new thread-safe queue of size sz.
 */
thrqueue_t*
thrqueue_new(size_t sz)
{
    thrqueue_t* queue;

    /* thrqueue_enq_getstub() 会预留一个槽位（sz-1 判满），sz<2 会让它永远
     * 认为队列是满的而死锁。 */
    if (sz < 2) sz = 2;

    if (!(queue = (thrqueue_t*)malloc(sizeof(thrqueue_t))))
        goto out0;
    if (!(queue->data = (void**)malloc(sz * sizeof(void*))))
        goto out1;
    if (0 != pthread_mutex_init(&queue->mutex, NULL))
        goto out2;
    if (0 != pthread_cond_init(&queue->notempty, NULL))
        goto out3;
    if (0 != pthread_cond_init(&queue->notfull, NULL))
        goto out4;
    memset(queue->data, 0, sz * sizeof(void*));
    queue->sz = sz;
    queue->n = 0;
    queue->in = 0;
    queue->out = 0;
    queue->block_enqueue = 1;
    queue->block_dequeue = 1;
    return queue;

out4:
    pthread_cond_destroy(&queue->notempty);
out3:
    pthread_mutex_destroy(&queue->mutex);
out2:
    free(queue->data);
out1:
    free(queue);
out0:
    return NULL;
}

/*
 * Free all resources associated with queue.
 * The caller must ensure that there are no threads still
 * using the queue when it is free'd.
 */
void thrqueue_free(thrqueue_t* queue)
{
    if (!queue) return;
    free(queue->data);
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->notempty);
    pthread_cond_destroy(&queue->notfull);
    free(queue);
}

/*
 * Enqueue an item into the queue.  Will block if the queue is full.
 * If enqueue has been switched to non-blocking mode, never blocks
 * but instead returns NULL if queue is full.
 * Returns enqueued item on success.
 */
void* thrqueue_enqueue(thrqueue_t* queue, void* item)
{
    pthread_mutex_lock(&queue->mutex);
    while (queue->n == queue->sz) {
        if (!queue->block_enqueue) {
            pthread_mutex_unlock(&queue->mutex);
            return NULL;
        }
        // printf("ewait%d\n", queue->n);
        pthread_cond_wait(&queue->notfull, &queue->mutex);
        // printf("ewaitx%d\n", queue->n);
    }
    queue->data[queue->in++] = item;
    queue->in %= queue->sz;
    queue->n++;
    pthread_mutex_unlock(&queue->mutex);
    pthread_cond_signal(&queue->notempty);
    return item;
}

void** thrqueue_enq_getstub(thrqueue_t* queue)
{
    void** item;
    size_t sz;
#ifdef TCLOG_PERF
    /* _q0→_q1 = 阻塞等 mutex；_q1→出循环 = 等 notfull（背压，不是锁） */
    unsigned long long _q0 = tclog_perf_now();
    unsigned long long _q1;
#endif
    pthread_mutex_lock(&queue->mutex);
#ifdef TCLOG_PERF
    _q1 = tclog_perf_now();
#endif
    sz = queue->sz - 1;
    while (queue->n == sz) {
        if (!queue->block_enqueue) {
            pthread_mutex_unlock(&queue->mutex);
            return NULL;
        }
        // printf("ewait%d\n", queue->n);
        pthread_cond_wait(&queue->notfull, &queue->mutex);
        // printf("ewaitx%d\n", queue->n);
    }
#ifdef TCLOG_PERF
    tclog_perf_add(TCLOG_P_Q_LOCK, _q1 - _q0);
    tclog_perf_add(TCLOG_P_Q_NFULL, tclog_perf_now() - _q1);
#endif
    item = &queue->data[queue->in++];
    queue->in %= queue->sz;
    return item;
}

void thrqueue_enq_done(thrqueue_t* queue)
{
    queue->n++;
    pthread_mutex_unlock(&queue->mutex);
    /* signal 而不是 broadcast：notempty 上只有一个等待者（唯一的 writer
     * 线程），broadcast 只是把这一个唤醒，却要按"唤醒所有"的语义走一遍。
     * 注意：若将来出现第二个消费者，这里必须改回 broadcast。 */
    pthread_cond_signal(&queue->notempty);
}

/*
 * Non-blocking enqueue.  Never blocks.
 * Returns NULL if the queue is full.
 * Returns the enqueued item on success.
 */
void* thrqueue_enqueue_nb(thrqueue_t* queue, void* item)
{
    pthread_mutex_lock(&queue->mutex);
    if (queue->n == queue->sz) {
        pthread_mutex_unlock(&queue->mutex);
        return NULL;
    }
    queue->data[queue->in++] = item;
    queue->in %= queue->sz;
    queue->n++;
    pthread_mutex_unlock(&queue->mutex);
    pthread_cond_signal(&queue->notempty);
    return item;
}

/*
 * Dequeue an item from the queue.  Will block if the queue is empty.
 * If dequeue has been switched to non-blocking mode, never blocks
 * but instead returns NULL if queue is empty.
 * Returns dequeued item on success.
 */
void* thrqueue_dequeue(thrqueue_t* queue)
{
    void* item;

    pthread_mutex_lock(&queue->mutex);
    while (queue->n == 0) {
        if (!queue->block_dequeue) {
            pthread_mutex_unlock(&queue->mutex);
            return NULL;
        }
        // printf("dwait%d\n", queue->n);
        pthread_cond_wait(&queue->notempty, &queue->mutex);
        // printf("dwaitx%d\n", queue->n);
    }
    item = queue->data[queue->out++];
    queue->out %= queue->sz;
    queue->n--;
    pthread_mutex_unlock(&queue->mutex);
    pthread_cond_signal(&queue->notfull);
    return item;
}

/*
 * Batch dequeue with node exchange.  Takes up to max items in one critical
 * section and returns how many were taken; the caller processes them
 * afterwards, outside the lock.  Each taken item is replaced in its slot by
 * the blank node supplied in buf[k], so the consumer is the sole owner of the
 * items it took -- see thrqueue.h.
 *
 * This is what makes a multi-producer/single-consumer queue cheap: instead of
 * paying lock+unlock+signal per item, one lock covers the whole batch.  With
 * a single consumer there is no fairness problem -- nobody else is competing
 * for the dequeue side.
 */
size_t thrqueue_dequeue_swap(thrqueue_t* queue, void** buf, size_t max)
{
    size_t k = 0;
    size_t i;

    if (max == 0) return 0;

    pthread_mutex_lock(&queue->mutex);
    while (queue->n == 0) {
        if (!queue->block_dequeue) {
            pthread_mutex_unlock(&queue->mutex);
            return 0;
        }
        pthread_cond_wait(&queue->notempty, &queue->mutex);
    }
    while (k < max && queue->n > 0) {
        void* full = queue->data[queue->out];
        queue->data[queue->out] = buf[k];   /* 留下一个空节点占位 */
        queue->out = (queue->out + 1) % queue->sz;
        queue->n--;
        buf[k] = full;
        k++;
    }
    pthread_mutex_unlock(&queue->mutex);

    /* Took k slots -> up to k producers may be parked on notfull.  Signalling
     * one by one rather than broadcasting: glibc's cond_signal is a userspace
     * no-op when nobody is waiting, so this costs min(k, waiters) futexes and
     * never wakes the whole herd. */
    for (i = 0; i < k; i++) {
        pthread_cond_signal(&queue->notfull);
    }
    return k;
}

void* thrqueue_dequeue_timedwait(thrqueue_t* queue, size_t millseconds)
{
    void* item;
#if __unix
    struct timespec tm;
#endif

    pthread_mutex_lock(&queue->mutex);
    while (queue->n == 0) {
        if (!queue->block_dequeue) {
            pthread_mutex_unlock(&queue->mutex);
            return NULL;
        }
#if __unix
        {
            /* pthread_cond_timedwait 要的是绝对时间(CLOCK_REALTIME)。原实现
             * 把相对毫秒直接塞进 timespec，且 tv_nsec 不进位（>=1000ms 时溢出
             * 成无效值 → 调用立刻返回 EINVAL → 退化成忙等，而且永不返回 NULL）。 */
            long ms = (long)millseconds;
            clock_gettime(CLOCK_REALTIME, &tm);
            tm.tv_sec  += ms / 1000;
            tm.tv_nsec += (ms % 1000) * 1000000L;
            if (tm.tv_nsec >= 1000000000L) {
                tm.tv_nsec -= 1000000000L;
                tm.tv_sec  += 1;
            }
        }
        /* 真超时就交回 NULL：调用方要的是"等这么久"，不是"一直等下去" */
        if (pthread_cond_timedwait(&queue->notempty, &queue->mutex, &tm) == ETIMEDOUT) {
            pthread_mutex_unlock(&queue->mutex);
            return NULL;
        }
#elif defined(_WIN32) || defined(_WIN64)
        if (!tclog_cv_wait(&queue->notempty, &queue->mutex, (DWORD)millseconds)) {
            pthread_mutex_unlock(&queue->mutex);
            return NULL;
        }
#else
#error TODO
#endif
    }
    item = queue->data[queue->out++];
    queue->out %= queue->sz;
    queue->n--;
    pthread_mutex_unlock(&queue->mutex);
    pthread_cond_signal(&queue->notfull);
    return item;
}

/*
 * Non-blocking dequeue.  Never blocks.
 * Returns NULL if the queue is empty.
 * Returns the dequeued item on success.
 */
void* thrqueue_dequeue_nb(thrqueue_t* queue)
{
    void* item;

    pthread_mutex_lock(&queue->mutex);
    if (queue->n == 0) {
        pthread_mutex_unlock(&queue->mutex);
        return NULL;
    }
    item = queue->data[queue->out++];
    queue->out %= queue->sz;
    queue->n--;
    pthread_mutex_unlock(&queue->mutex);
    pthread_cond_signal(&queue->notfull);
    return item;
}

/*
 * Permanently make all enqueue operations on queue non-blocking and wake
 * up all threads currently waiting for the queue to become not full.
 * This is to allow threads to finish their work on the queue on application
 * shutdown, but not be blocked forever.
 */
void thrqueue_unblock_enqueue(thrqueue_t* queue)
{
    pthread_mutex_lock(&queue->mutex);
    queue->block_enqueue = 0;
    pthread_cond_broadcast(&queue->notfull);
    pthread_mutex_unlock(&queue->mutex);
    sched_yield();
}

/*
 * Permanently make all dequeue operations on queue non-blocking and wake
 * up all threads currently waiting for the queue to become not empty.
 * This is to allow threads to finish their work on the queue on application
 * shutdown, but not be blocked forever.
 */
void thrqueue_unblock_dequeue(thrqueue_t* queue)
{
    pthread_mutex_lock(&queue->mutex);
    queue->block_dequeue = 0;
    pthread_cond_broadcast(&queue->notempty);
    pthread_mutex_unlock(&queue->mutex);
    sched_yield();
}

/* vim: set noet ft=c: */
