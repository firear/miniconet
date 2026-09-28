/*
 * tclog_perf.h — 日志路径分阶段耗时插桩（编译期门控）。
 *
 * 仅在定义 TCLOG_PERF 时生效。默认构建下本头文件展开为空：不产生代码、
 * 不引入符号，生产构建的大小与行为完全不受影响。
 *
 * 两侧累加器的分工：
 *  - 生产者侧（调用 LOGI/LOGE 的线程）：per-thread 累加器（TCLOG_TLS），
 *    无共享、无锁，插桩本身不会变成新的竞争源。
 *  - 消费者侧（唯一的 writer 线程）：全局累加器 + 原子加。writer 线程在
 *    d_destory_debug() 之后仍阻塞在 thrqueue_dequeue 上，没有退出时机可以
 *    导出它自己的 TLS 累加器，只能走全局。writer 只有一个，故原子加争用可忽略。
 *
 * 跨平台：本文件同时被 MSVC(VS2010) 与 GCC/Clang 编译，所以所有平台原语
 * （TLS / inline / 单调时钟 / 原子加）都在这里按平台分叉，不要再散落到 .c 里。
 * 之前的版本直接用裸 __thread / __inline__ / __sync_* / clock_gettime，
 * 在 VS2010 下会直接编不过。
 *
 * 本环境没有 perf/valgrind（见 plan.md），所以分阶段数据靠这里的显式夹点而非
 * 采样。注意夹点是**有成本**的：每条消息约 11-13 次取时（vDSO ~20ns / QPC
 * ~10-30ns），合计 ~250-350ns，相对被测的 ~1µs 量级不是"可忽略"，解读时需
 * 把这一项扣除。
 */
#ifndef TCLOG_PERF_H
#define TCLOG_PERF_H

/* ---------- 平台原语（门控之外，始终可用） ---------- */

#ifndef TCLOG_TLS
#  if defined(_MSC_VER)
#    define TCLOG_TLS __declspec(thread)
#  elif defined(__GNUC__) || defined(__clang__)
#    define TCLOG_TLS __thread
#  else
#    error "tclog: 该编译器没有可用的线程局部存储，请补充 TCLOG_TLS 定义"
#  endif
#endif

#ifndef TCLOG_INLINE
#  if defined(_MSC_VER)
#    define TCLOG_INLINE __inline
#  else
#    define TCLOG_INLINE __inline__
#  endif
#endif

#ifdef TCLOG_PERF

#if defined(_WIN32) || defined(_WIN64)
#  include <windows.h>
#else
#  include <time.h>
#endif

enum {
    TCLOG_P_TS = 0,   /* strtime() 总计：取时钟 + 本地时间转换 + 时间戳格式化 */
    TCLOG_P_TS_LT,    /* 其中本地时间转换单项（Unix: localtime_r，嵌套在 TS 内） */
    TCLOG_P_TS_FMT,   /* 其中时间戳字符串格式化单项（嵌套在 TS 内） */
    TCLOG_P_FMT,      /* 前缀 + 正文格式化（拼前缀 + vsnprintf） */
    TCLOG_P_Q_LOCK,   /* 队列：阻塞等 mutex */
    TCLOG_P_Q_NFULL,  /* 队列：等 notfull（背压，不是锁） */
    TCLOG_P_Q_HOLD,   /* 队列：临界区（getstub 返回 → enq_done 返回），已不含格式化 */
    TCLOG_P_WRITE,    /* 实际写出（stderr / 文件 fwrite，写文件侧已按批合并） */
    TCLOG_P_FLUSH,    /* fflush */
    TCLOG_P_TOTAL,    /* 生产者侧 __d_debug 单次总计 */
    TCLOG_P_N
};

extern TCLOG_TLS unsigned long long tclog_perf_ns[TCLOG_P_N];
extern TCLOG_TLS unsigned long long tclog_perf_cnt[TCLOG_P_N];

/*
 * 单调时钟，返回纳秒。各平台的起点不同，但插桩只做**同线程内的差值**比较
 * （writer 用独立累加器），所以不需要共同 epoch。
 */
static TCLOG_INLINE unsigned long long tclog_perf_now(void)
{
#if defined(_WIN32) || defined(_WIN64)
    static LARGE_INTEGER freq;      /* 函数内 static：0 表示尚未初始化 */
    LARGE_INTEGER ctr;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
        if (freq.QuadPart == 0) {
            return 0;
        }
    }
    QueryPerformanceCounter(&ctr);
    return (unsigned long long)(ctr.QuadPart / freq.QuadPart) * 1000000000ULL
         + (unsigned long long)(ctr.QuadPart % freq.QuadPart)
           * 1000000000ULL / (unsigned long long)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL
         + (unsigned long long)ts.tv_nsec;
#endif
}

static TCLOG_INLINE void tclog_perf_add(int phase, unsigned long long ns)
{
    tclog_perf_ns[phase] += ns;
    tclog_perf_cnt[phase] += 1;
}

/* 供唯一的 writer 线程使用：累加进全局计数器 */
static TCLOG_INLINE void tclog_perf_addw(int phase, unsigned long long ns)
{
    extern unsigned long long tclog_perf_w_ns[TCLOG_P_N];
    extern unsigned long long tclog_perf_w_cnt[TCLOG_P_N];
#if defined(_WIN32) || defined(_WIN64)
    InterlockedExchangeAdd64((volatile LONG64*)&tclog_perf_w_ns[phase], (LONG64)ns);
    InterlockedIncrement64((volatile LONG64*)&tclog_perf_w_cnt[phase]);
#else
    __sync_fetch_and_add(&tclog_perf_w_ns[phase], ns);
    __sync_fetch_and_add(&tclog_perf_w_cnt[phase], 1ULL);
#endif
}

void tclog_perf_reset(void);
/* 导出【调用线程】的累加值 */
void tclog_perf_accum(double ns[TCLOG_P_N], unsigned long long cnt[TCLOG_P_N]);
/* 导出 writer 线程的累加值 */
void tclog_perf_writer_accum(double ns[TCLOG_P_N], unsigned long long cnt[TCLOG_P_N]);

#endif /* TCLOG_PERF */
#endif /* TCLOG_PERF_H */
