/*
 * thrqueue unit tests. Exercises FIFO ordering, non-blocking full, timedwait
 * timeout, unblock wakeups (both ends), and a multi-producer / single-consumer
 * stress run that asserts no items are lost.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "thrqueue.h"

#include <stdio.h>
#include <stdlib.h>

/* CHECK() 在 NDEBUG（RelWithDebInfo / Release）下展开为空，而本文件的断言
 * 大量带副作用（pthread_create / pthread_join）：release 构建里线程根本没被
 * 创建，还会拿未初始化的 pthread_t 去 join —— Linux 上直接段错误，Windows 上
 * 静默跳过。改用始终求值的 CHECK()。 */
static void tclog_check_fail(const char* file, int line, const char* expr)
{
    fprintf(stderr, "CHECK FAILED %s:%d: %s\n", file, line, expr);
    abort();
}
#define CHECK(cond) \
    do { if (!(cond)) tclog_check_fail(__FILE__, __LINE__, #cond); } while (0)

/* ---------- 跨平台：最小线程/睡眠集合 ----------
 * Windows 没有 <pthread.h> / <unistd.h>。这里用宏把它补成本测试用到的那一
 * 小撮 API（create / join / usleep），Unix 下直接包含真实头文件。
 * 刻意不引入 portable/ 目录，避免遮蔽父项目的同名头文件。 */
#if defined(_WIN32) || defined(_WIN64)
#  include <process.h>
#  include <windows.h>
typedef HANDLE pthread_t;
static __inline int tclog_test_thread_create(pthread_t* t,
                                             void* (*fn)(void*), void* arg)
{
    /* size_t 是 C89 里能拿到的"与指针同宽的无符号整型"；VC6 既没有 <stdint.h>
     * 也没有 uintptr_t，用它当中间类型两边都能编。 */
    size_t h;
    h = _beginthreadex(NULL, 0, (unsigned int (__stdcall*)(void*))fn,
                       arg, 0, NULL);
    if (h == 0) return -1;
    *t = (HANDLE)h;
    return 0;
}
static __inline int tclog_test_thread_join(pthread_t t, void** retval)
{
    DWORD code = 0;
    WaitForSingleObject(t, INFINITE);
    GetExitCodeThread(t, &code);
    CloseHandle(t);
    if (retval) *retval = (void*)(size_t)code;
    return 0;
}
static __inline int usleep(unsigned int usec)
{
    Sleep((DWORD)(usec / 1000));
    return 0;
}
#define pthread_create(th, attr, fn, arg) tclog_test_thread_create((th), (fn), (arg))
#define pthread_join(th, retval)          tclog_test_thread_join((th), (retval))
#else
#  include <pthread.h>
#  include <unistd.h>
#endif

/* 1. basic FIFO ordering */
static void test_basic(void)
{
    int i = 0;
    thrqueue_t *q = thrqueue_new(8);
    CHECK(q);
    for (; i < 5; i++) {
        void *r = thrqueue_enqueue(q, (void *)(long)(i + 1));
        CHECK(r == (void *)(long)(i + 1));
    }
    for (i = 0; i < 5; i++) {
        void *r = thrqueue_dequeue(q);
        CHECK(r == (void *)(long)(i + 1));
    }
    thrqueue_free(q);
    printf("ok: basic FIFO\n");
}

/* 2. non-blocking enqueue on a full queue returns NULL */
static void test_nb_full(void)
{
    thrqueue_t *q = thrqueue_new(2);   /* sz=2 -> holds 2 items */
    CHECK(thrqueue_enqueue(q, (void *)1));
    CHECK(thrqueue_enqueue(q, (void *)2));
    CHECK(thrqueue_enqueue_nb(q, (void *)3) == NULL);  /* full */
    CHECK(thrqueue_dequeue(q) == (void *)1);
    CHECK(thrqueue_enqueue_nb(q, (void *)3) != NULL);  /* now fits */
    thrqueue_free(q);
    printf("ok: non-blocking full\n");
}

/* 3. timedwait on an empty queue returns NULL on timeout */
static void test_timedwait_timeout(void)
{
    thrqueue_t *q = thrqueue_new(4);
    void *r = thrqueue_dequeue_timedwait(q, 50);
    CHECK(r == NULL);
    thrqueue_free(q);
    printf("ok: timedwait timeout\n");
}

/* 4. a blocked dequeue wakes and returns NULL after unblock_dequeue */
static thrqueue_t *g_q4;
static void *consumer_block(void *arg)
{
    (void)arg;
    return thrqueue_dequeue(g_q4);   /* NULL once unblocked */
}
static void test_unblock_dequeue(void)
{
    pthread_t t;
    void *r;
    g_q4 = thrqueue_new(4);
    CHECK(pthread_create(&t, NULL, consumer_block, NULL) == 0);
    usleep(50000);                   /* let consumer block */
    thrqueue_unblock_dequeue(g_q4);
    CHECK(pthread_join(t, &r) == 0);
    CHECK(r == NULL);
    thrqueue_free(g_q4);
    printf("ok: unblock dequeue\n");
}

/* 5. a blocked enqueue wakes and returns NULL after unblock_enqueue */
static thrqueue_t *g_q5;
static void *producer_block(void *arg)
{
    (void)arg;
    return thrqueue_enqueue(g_q5, (void *)99);   /* NULL once unblocked */
}
static void test_unblock_enqueue(void)
{
    pthread_t t;
    void *r;
    g_q5 = thrqueue_new(2);
    CHECK(thrqueue_enqueue(g_q5, (void *)1));
    CHECK(thrqueue_enqueue(g_q5, (void *)2));   /* full now */
    CHECK(pthread_create(&t, NULL, producer_block, NULL) == 0);
    usleep(50000);
    thrqueue_unblock_enqueue(g_q5);
    CHECK(pthread_join(t, &r) == 0);
    CHECK(r == NULL);
    thrqueue_free(g_q5);
    printf("ok: unblock enqueue\n");
}

/* 6. multi-producer / single-consumer: nothing lost, nothing duplicated */
#define MPC_PRODUCERS 4
#define MPC_PER       1000
static thrqueue_t *g_q6;
static long g_consumed;

static void *mpc_producer(void *arg)
{
    int i = 0;
    long base = (long)arg;
    for (; i < MPC_PER; i++)
        thrqueue_enqueue(g_q6, (void *)(base + i));
    return NULL;
}
static void *mpc_consumer(void *arg)
{
    (void)arg;
    for (;;) {
        void *r = thrqueue_dequeue(g_q6);
        if (!r) break;               /* unblocked -> done */
        g_consumed++;
    }
    return NULL;
}
static void test_mpc(void)
{
    int i;
    pthread_t prod[MPC_PRODUCERS], cons;
    g_q6 = thrqueue_new(16);
    g_consumed = 0;
    CHECK(pthread_create(&cons, NULL, mpc_consumer, NULL) == 0);
    
    for (i = 0; i < MPC_PRODUCERS; i++)
        CHECK(pthread_create(&prod[i], NULL, mpc_producer,
                              (void *)(long)(i * MPC_PER + 1)) == 0);
    for (i = 0; i < MPC_PRODUCERS; i++)
        pthread_join(prod[i], NULL);
    thrqueue_unblock_dequeue(g_q6);  /* wake consumer so it can exit */
    pthread_join(cons, NULL);
    CHECK(g_consumed == MPC_PRODUCERS * MPC_PER);
    thrqueue_free(g_q6);
    printf("ok: multi-producer single-consumer (%ld items)\n", g_consumed);
}

int main(void)
{
    test_basic();
    test_nb_full();
    test_timedwait_timeout();
    test_unblock_dequeue();
    test_unblock_enqueue();
    test_mpc();
    printf("ALL THRQUEUE TESTS PASSED\n");
    return 0;
}
