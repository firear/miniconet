/*
 * bench_debug — tclog 日志路径微基准（不注册进 CTest）。
 *
 * 用法：bench_debug N THREADS ASYNC STDOUT_LEVEL FILE_LEVEL [LOGDIR]
 *   N            每线程日志条数
 *   THREADS      生产者线程数
 *   ASYNC        1=异步（队列+writer 线程） 0=同步（直写）
 *   STDOUT_LEVEL 0..6，DEBUG_NONE(0)=关闭 stdout
 *   FILE_LEVEL   0..6，DEBUG_NONE(0)=不建日志文件
 *   LOGDIR       日志文件目录，省略则不传（写当前目录）
 *
 * 注意：stderr 必须重定向到 /dev/null，否则终端渲染会淹没被测开销。
 *       TCLOG_PERF 构建下额外打印分阶段耗时。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "debug.h"
#include "tclog_perf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- 64 位无符号整型 ----------
 * MSVC 到 VS2013 才认 `unsigned long long`（VC6 直接报
 * C2632: 'long' followed by 'long' is illegal），MSVC 侧改用 __int64。
 *
 * 为什么不用现成的 uint64_t：在 LP64（Linux）上 uint64_t 是 unsigned long，与
 * tclog_perf.h 里声明的 `unsigned long long` **不是同一类型**，TCLOG_PERF 构建时
 * 往 tclog_perf_accum / tclog_perf_writer_accum 传数组会类型不匹配。所以取
 * "MSVC: unsigned __int64 / 其它: unsigned long long" —— 两侧都与 tclog_perf.h
 * 一致（MSVC 里 long long 就是 __int64 的同义词）。
 *
 * 字面量后缀同理：ULL/LL 后缀 VC6 不认（C2059: bad suffix on number）。本文件用到
 * 的字面量（10000000 / 100 / 1000000000）都放得下 32 位，且另一操作数已经是 64 位，
 * 整型提升后仍是 64 位运算，所以直接省掉后缀最省事、也最不容易出错。 */
#if defined(_MSC_VER)
typedef unsigned __int64 tclog_u64_t;
#else
typedef unsigned long long tclog_u64_t;
#endif

/* 打印本次实际用的条件变量后端，便于区分 native / 自带实现（= XP 的路径）。
 * Windows 下用环境变量 TCLOG_CV_EMU=1 可强制走自带实现，见 compat/tclog_win32.h。 */
#if defined(_WIN32) || defined(_WIN64)
#  include "compat/tclog_win32.h"
#  define TCLOG_BENCH_CV_BACKEND()  tclog_cv_backend_name()
#else
#  define TCLOG_BENCH_CV_BACKEND()  "pthread"
#endif

/* ---------- 跨平台：pthread / usleep / clock_gettime ----------
 * Windows 没有 <pthread.h> / <unistd.h>，且 VS2010 的 <time.h> 里没有
 * clock_gettime 和 struct timespec。这里用宏把它们补成 bench 用到的那一小撮
 * （create / join / usleep / MONOTONIC / PROCESS_CPUTIME_ID）。
 * 刻意不引入 portable/ 目录，避免遮蔽父项目的同名头文件。 */
#if defined(_WIN32) || defined(_WIN64)
#  include <process.h>
#  include <time.h>
#  include <windows.h>
#  ifndef CLOCK_MONOTONIC
#    define CLOCK_MONOTONIC 1
#  endif
#  ifndef CLOCK_PROCESS_CPUTIME_ID
#    define CLOCK_PROCESS_CPUTIME_ID 2
#  endif
#  if !defined(_TIMESPEC_DEFINED)
#    define _TIMESPEC_DEFINED
struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};
#  endif
static __inline int clock_gettime(int clk_id, struct timespec* ts)
{
    if (clk_id == CLOCK_PROCESS_CPUTIME_ID) {
        FILETIME creation, exit_time, kernel, user;
        ULARGE_INTEGER ku, uu;
        tclog_u64_t total100ns;
        if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_time,
                             &kernel, &user)) {
            return -1;
        }
        ku.LowPart  = kernel.dwLowDateTime;
        ku.HighPart = kernel.dwHighDateTime;
        uu.LowPart  = user.dwLowDateTime;
        uu.HighPart = user.dwHighDateTime;
        total100ns = ku.QuadPart + uu.QuadPart;
        ts->tv_sec  = (time_t)(total100ns / 10000000);
        ts->tv_nsec = (long)((total100ns % 10000000) * 100);
        return 0;
    }
    {
        LARGE_INTEGER freq, ctr;
        if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0) return -1;
        QueryPerformanceCounter(&ctr);
        ts->tv_sec  = (time_t)(ctr.QuadPart / freq.QuadPart);
        ts->tv_nsec = (long)((ctr.QuadPart % freq.QuadPart) * 1000000000
                             / freq.QuadPart);
        return 0;
    }
}
typedef HANDLE pthread_t;
static __inline int tclog_bench_thread_create(pthread_t* t,
                                              void* (*fn)(void*), void* arg)
{
    /* size_t 是 C89 里能拿到的"与指针同宽的无符号整型"；VC6 没有 uintptr_t。 */
    size_t h;
    h = _beginthreadex(NULL, 0, (unsigned int (__stdcall*)(void*))fn,
                       arg, 0, NULL);
    if (h == 0) return -1;
    *t = (HANDLE)h;
    return 0;
}
static __inline int tclog_bench_thread_join(pthread_t t)
{
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return 0;
}
static __inline int usleep(unsigned int usec)
{
    Sleep((DWORD)(usec / 1000));
    return 0;
}
#  define pthread_create(th, attr, fn, arg) tclog_bench_thread_create((th), (fn), (arg))
#  define pthread_join(th, retval)          tclog_bench_thread_join((th))
#else
#  include <pthread.h>
#  include <time.h>
#  include <unistd.h>
#endif

#ifdef TCLOG_PERF
/* 64 位无符号格式符（含 12 列右对齐宽度）：VS2010 的 CRT 不支持 %ll
 * （VS2013 才加），它用 %I64。
 * 注意宽度必须写进格式符里，不能写成 "%12" TCLOG_U64 —— 那会拼出
 * "%12%I64u"，MSVC 把它当非法转换、把 %I64u 原样打出来。 */
#if defined(_MSC_VER)
#  define TCLOG_U64 "%12I64u"
#else
#  define TCLOG_U64 "%12llu"
#endif

static const char* kPhase[] = {
    "TS(all)", "TS:localtime_r", "TS:snprintf", "FMT(prefix+body)",
    "Q:lock", "Q:notfull", "Q:hold", "WRITE", "FLUSH", "TOTAL"
};
#endif

static tclog_u64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (tclog_u64_t)ts.tv_sec * 1000000000 + (tclog_u64_t)ts.tv_nsec;
}

/* CPU 时间：把"真在烧 CPU"和"被抢占/等 CPU"分开。本沙箱只有 2 物理核且
 * 常驻 IDE，wall 里混有大量其它进程的抢占，CPU 时间才是稳定的服务量。 */
static tclog_u64_t cpu_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (tclog_u64_t)ts.tv_sec * 1000000000 + (tclog_u64_t)ts.tv_nsec;
}

typedef struct {
    int count;
#ifdef TCLOG_PERF
    double ns[TCLOG_P_N];
    tclog_u64_t cnt[TCLOG_P_N];
#endif
} worker_t;

static void* worker(void* arg)
{
    worker_t* w = (worker_t*)arg;
    int i;
#ifdef TCLOG_PERF
    tclog_perf_reset();
#endif
    for (i = 0; i < w->count; i++) {
        LOGI("bench message %d seq=%d tag=%s value=%d", i, i, "abcdefgh", i * 7);
    }
#ifdef TCLOG_PERF
    tclog_perf_accum(w->ns, w->cnt);
#endif
    return NULL;
}

int main(int argc, char** argv)
{
    int n = (argc > 1) ? atoi(argv[1]) : 100000;
    int nthreads = (argc > 2) ? atoi(argv[2]) : 1;
    int async = (argc > 3) ? atoi(argv[3]) : 1;
    int lvl_stdout = (argc > 4) ? atoi(argv[4]) : DEBUG_NONE;
    int lvl_file = (argc > 5) ? atoi(argv[5]) : DEBUG_INFO;
    const char* logdir = (argc > 6) ? argv[6] : NULL;

    pthread_t th[64];
    worker_t w[64];
    int i;
#ifdef TCLOG_PERF
    int p;
    double pns[TCLOG_P_N];
    double wns[TCLOG_P_N];
    double wns0[TCLOG_P_N];
    tclog_u64_t pcnt[TCLOG_P_N];
    tclog_u64_t wcnt[TCLOG_P_N];
    tclog_u64_t wcnt0[TCLOG_P_N];
#endif
    tclog_u64_t t0, t1, c0, c1;
    double wall_ns;

    if (nthreads < 1) nthreads = 1;
    if (nthreads > 64) nthreads = 64;

    /* 先声明意图再 init：同步模式因此**完全不创建** writer 线程（见 debug.h）。
     * init 之前 d_set_async 只记录标志，不会真去起线程。 */
    d_set_async(async);
    d_init_debug(logdir, 0, 0, lvl_stdout, lvl_file, "bench");

    /* warm up：时区缓存、stdio 缓冲首次分配、缺页都在这一轮消化掉 */
    {
        worker_t wu;
        wu.count = 20000;
        worker(&wu);
    }
    usleep(300000);   /* 等 writer 排空 warm up 的积压 */

#ifdef TCLOG_PERF
    tclog_perf_writer_accum(wns0, wcnt0);
#endif

    t0 = now_ns();
    c0 = cpu_ns();
    for (i = 0; i < nthreads; i++) {
        w[i].count = n;
        pthread_create(&th[i], NULL, worker, &w[i]);
    }
    for (i = 0; i < nthreads; i++) {
        pthread_join(th[i], NULL);
    }
    c1 = cpu_ns();
    t1 = now_ns();

    /* 排空尾部的队列积压（最多 1024 条）后再统计，不计入计时 */
    usleep(300000);

    wall_ns = (double)(t1 - t0);

    printf("=== bench_debug: N=%d threads=%d async=%d stdout_lvl=%d file_lvl=%d dir=%s cv=%s\n",
        n, nthreads, async, lvl_stdout, lvl_file, logdir ? logdir : "(cwd)",
        TCLOG_BENCH_CV_BACKEND());
    printf("wall_ms            %.3f\n", wall_ns / 1e6);
    printf("ns_per_msg(wall)   %.1f\n", wall_ns / (double)(n * nthreads));
    printf("ns_per_msg(cpu)    %.1f\n", (double)(c1 - c0) / (double)(n * nthreads));
    printf("msg_per_sec        %.0f\n", (double)(n * nthreads) / (wall_ns / 1e9));

#ifdef TCLOG_PERF
    memset(pns, 0, sizeof(pns));
    memset(pcnt, 0, sizeof(pcnt));
    for (i = 0; i < nthreads; i++) {
        for (p = 0; p < TCLOG_P_N; p++) {
            pns[p] += w[i].ns[p];
            pcnt[p] += w[i].cnt[p];
        }
    }
    tclog_perf_writer_accum(wns, wcnt);

    printf("\n-- producer (sum of %d threads) --\n", nthreads);
    printf("%-18s %12s %12s %12s\n", "phase", "ns/msg", "total_ms", "count");
    for (p = 0; p < TCLOG_P_N; p++) {
        if (pcnt[p] == 0) continue;   /* 该阶段本侧没有样本（如异步下生产者不写） */
        printf("%-18s %12.1f %12.3f " TCLOG_U64 "\n", kPhase[p],
            pcnt[p] ? pns[p] / (double)pcnt[p] : 0.0,
            pns[p] / 1e6, pcnt[p]);
    }
    printf("\n-- writer thread (delta vs post-warmup) --\n");
    printf("%-18s %12s %12s %12s\n", "phase", "ns/msg", "total_ms", "count");
    for (p = 0; p < TCLOG_P_N; p++) {
        tclog_u64_t dc = wcnt[p] - wcnt0[p];
        double dn = wns[p] - wns0[p];
        if (dc == 0) continue;
        printf("%-18s %12.1f %12.3f " TCLOG_U64 "\n", kPhase[p], dn / (double)dc, dn / 1e6, dc);
    }
#endif

    d_destory_debug();
    return 0;
}
