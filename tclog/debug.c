#if !__ANDROID__
#if defined(__GNUC__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#endif
#include "debug.h"
#if DEBUG_SHOW_DEBUG
#include "thrqueue.h"
#include "tclog_perf.h"
#include <errno.h>

#if defined(_MSC_VER) && _MSC_VER < 1600 // VS2010
    #include "compat/tclog_win32.h"     /* 自带 VC6 定长整型，见该文件头注释 */
#else
    #include <stdint.h>
#endif

#if defined(_MSC_VER) && _MSC_VER < 1900 // VS2015
#  define TCLOG_SNPRINTF _snprintf
#else
#  define TCLOG_SNPRINTF snprintf
#endif
#include <time.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#if defined _WIN32 || defined _WIN64
#include <process.h>
#include <windows.h>
#include <direct.h>
#include <io.h>
typedef unsigned int(__stdcall* MYTHREADFUNROUTE)(void*);
#pragma warning(disable : 4996)
#if _MSC_VER < 1600 // VS2010
struct tm* localtime_r(time_t const* const sourceTime, struct tm* tmDest)
{
    *tmDest = *localtime(sourceTime);
    return tmDest;
}
#else
#define localtime_r(a, b) localtime_s(b, a)
#endif
unsigned long _gettid()
{
    return GetCurrentThreadId();
}

#elif defined __linux
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>
static __thread unsigned long t_cached_tid;
unsigned long _gettid()
{
    if(t_cached_tid == 0){
        t_cached_tid = syscall(__NR_gettid);
    }
    return t_cached_tid;
}
#define __stdcall
typedef void* (*MYTHREADFUNROUTE)(void*);
#include <pthread.h>
#else
#error "not def"
#endif


#ifdef TCLOG_PERF
TCLOG_TLS unsigned long long tclog_perf_ns[TCLOG_P_N];
TCLOG_TLS unsigned long long tclog_perf_cnt[TCLOG_P_N];
unsigned long long tclog_perf_w_ns[TCLOG_P_N];
unsigned long long tclog_perf_w_cnt[TCLOG_P_N];
#endif

const char* g_colortable[] = {
    "", // NONE
    "\033[0;32;32m", // KGRN
    "\033[0;36m", // KCYN
    "\033[0;32;31m", // KRED
    "\033[1;33m", // yellow
    "\033[0;32;34m", // KBLU
    "\033[1;36m", // KCYN_L
    "\033[0;33m", // KBRN
    "\033[0m" // RESET
};

typedef struct
{
    size_t maxRotatedLogs;
    size_t logRotateSizeBytes;
    size_t outByteCount;
    FILE* fp;
    thrqueue_t* queue;
    char* outputFileName;
    int8_t level_cache; // min value of stdout and file log level
    int8_t level_stdout;
    int8_t level_file;
    uint8_t exit : 1;           /* unsigned: "= 1" must store 1, not -1 */
    uint8_t flush : 1;
    uint8_t async : 1;
    uint8_t colorful : 1;
    uint8_t initialized : 1;    /* d_init_debug 跑过没有（d_set_async 据此决定能否起线程） */
    uint8_t writer_started : 1; /* writer 线程当前在不在跑 */
#if defined(_WIN32) || defined(_WIN64)
    HANDLE writer_handle;       /* 留着句柄才能停/回收 writer，原来是 CloseHandle 直接丢掉 */
#else
    pthread_t writer_handle;
#endif
} Loger_s;

/* 后加的字段不在初始化列表里 -> 由 C 保证清零。async 初值 -1（真值）= 默认开异步，
 * 与 debug.h 里的说明一致。 */
static Loger_s g_logger = {
    10, 0, 0, NULL, NULL, NULL, -1, DEBUG_NONE, DEBUG_NONE, 0, 0, -1, -1
};


typedef struct LogBuf {
    int level;
    enum COLOR color;
    size_t clen;
    size_t caplen;
    char content[0];
} LogBuf_s;

static TCLOG_TLS LogBuf_s* t_logbuf = NULL;

/* va_copy 是 C99；MSVC 到 VS2013(_MSC_VER 1800) 才提供，VC6 更没有。
 * MSVC 的 va_list 就是 char*，直接赋值等价；而 GCC 的 va_list 在 x86-64 上是
 * 数组类型，必须走 va_copy。 */
#if defined(_MSC_VER) && _MSC_VER < 1800
#  define TCLOG_VA_COPY(dst, src) ((dst) = (src))
#else
#  define TCLOG_VA_COPY(dst, src) va_copy((dst), (src))
#endif

/*
 * 有界格式化：把 _fmt/_arglist 写进 _buf，返回**实际写入**的字符数（不含结尾
 * NUL），并保证 _buf 以 NUL 结束；内容被截断时把 *_truncated 置 1
 * （_truncated 可为 NULL）。
 *
 * 旧实现让返回值表示"所需长度"，并在截断时拿**同一个已被消耗的** va_list 再
 * 跑一次 vsnprintf —— 那是 UB：v*printf 返回后 va_list 即失效，重跑必须用
 * 开始前的副本。这里改成只报告实际写入长度，由调用方靠 truncated 标志决定
 * 要不要换更大的缓冲重试。
 *
 * 另注：VS2015 之前的 _vsnprintf 截断时返回 -1 且**不保证**结尾有 NUL，
 * 所以统一强制补一个 NUL 再取长度，返回值和缓冲区都可靠。
 */
static TCLOG_INLINE int __log_vsnprintf(char* const _buf, size_t const _bufsize, char const* const _fmt, va_list _arglist, int* _truncated)
{
    va_list cp;
    int ret;

    if (_truncated) {
        *_truncated = 0;
    }
    if (_bufsize == 0) {
        if (_truncated) {
            *_truncated = 1;
        }
        return 0;
    }
#if defined(_MSC_VER) && _MSC_VER < 1900 // VS2015
#define vsnprintf _vsnprintf
#endif
    TCLOG_VA_COPY(cp, _arglist);
    ret = vsnprintf(_buf, _bufsize, _fmt, cp);
    va_end(cp);
#if defined(_MSC_VER) && _MSC_VER < 1900 // VS2015
#undef vsnprintf
#endif
    _buf[_bufsize - 1] = '\0';
    if (ret < 0 || (size_t)ret >= _bufsize) {
        if (_truncated) {
            *_truncated = 1;
        }
        ret = (int)strlen(_buf);
    }
    return ret;
}

#define TCLOG_FMT_MAX 1024
static char* tclog_fmt_str(const char* fmt, ...)
{
    char buf[TCLOG_FMT_MAX];
    char* p;
    int n;
    va_list ap;

    va_start(ap, fmt);
    n = __log_vsnprintf(buf, sizeof(buf), fmt, ap, NULL);
    va_end(ap);
    if (n < 0) return NULL;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    buf[n] = '\0';
    p = strdup(buf);
    return p;
}

static int getbits(size_t n)
{
    int ret = 1;
    while (n /= 10)
        ret++;
    return ret;
}

static void rotateLogs(Loger_s* logger)
{
    int err;
    int i;
    int maxRotationCountDigits;

    fclose(logger->fp);
    // Compute the maximum number of digits needed to count up to logger->maxRotatedLogs in decimal.
    // eg: logger->maxRotatedLogs == 30 -> log10(30) == 1.477 -> maxRotationCountDigits == 2
    // maxRotationCountDigits = (logger->maxRotatedLogs > 0) ? (int)(floor(log10(logger->maxRotatedLogs) + 1)) : 0;
    maxRotationCountDigits = (logger->maxRotatedLogs > 0) ? getbits(logger->maxRotatedLogs) : 0;

    for (i = logger->maxRotatedLogs; i > 0; i--) {
        char *file0, *file1;
        file1 = tclog_fmt_str("%s.%.*d", logger->outputFileName, maxRotationCountDigits, i);
        if (i - 1 == 0) {
            file0 = tclog_fmt_str("%s", logger->outputFileName);
        } else {
            file0 = tclog_fmt_str("%s.%.*d", logger->outputFileName, maxRotationCountDigits, i - 1);
        }
        if (!file0 || !file1) {
            perror("while rotating log files");
            break;
        }
#ifdef _WIN32
        // no need on linux,rename will replace new path .
        if (access(file1, 0) == 0) {
            unlink(file1);
        }
#endif
        err = rename(file0, file1);
        if (err < 0 && errno != ENOENT) {
            perror("while rotating log files");
        }
        free(file1);
        free(file0);
    }
    logger->fp = fopen(logger->outputFileName, "wt");
    if (logger->fp == NULL) {
        fprintf(stderr, "couldn't open output file\n");
    }
    logger->outByteCount = 0;
}

/* 写出一段已格式化的字节。参数是 (data, len) 而非单个 LogBuf：writer 会把
 * 一整批消息合并成一段再调这里。
 * 注意：轮转检查现在是每批一次而不是每条一次，所以文件可能超出
 * logRotateSizeBytes 上限约一个批的量（<= TCLOG_WRITER_BATCH * TCLOG_NODE_BYTES）。 */
static int writetofile(Loger_s* logger, const char* data, size_t len)
{
    if (logger->fp == NULL) return 0;
    if (logger->logRotateSizeBytes > 0 && logger->outByteCount >= logger->logRotateSizeBytes) {
        rotateLogs(logger);
    }
    if (len > 0) {
        size_t ret;
#ifdef TCLOG_PERF
        unsigned long long _w1 = tclog_perf_now();
#endif
        ret = fwrite(data, 1, len, logger->fp);
#ifdef TCLOG_PERF
        tclog_perf_addw(TCLOG_P_WRITE, tclog_perf_now() - _w1);
#endif
        if (ret > 0) {
            logger->outByteCount += ret;
        } else {
            printf("%s", strerror(errno));
        }
    }
    if (logger->flush) {
#ifdef TCLOG_PERF
        unsigned long long _w2 = tclog_perf_now();
#endif
        fflush(logger->fp);
#ifdef TCLOG_PERF
        tclog_perf_addw(TCLOG_P_FLUSH, tclog_perf_now() - _w2);
#endif
    }
    return 0;
}

/* 把攒在合并缓冲里的字节冲出去，并把长度清零。合并的意义：writer 一次出队
 * 拿到一批（最多 TCLOG_WRITER_BATCH 条）时只付一次 fwrite + 一次 fflush，
 * 而不是每条一次——stdio 每次调用都有锁与缓冲管理开销。 */
static void tclog_flush_filebuf(Loger_s* logger, char* buf, size_t* plen)
{
    if (*plen == 0) return;
    writetofile(logger, buf, *plen);
    *plen = 0;
}


#define TCLOG_WRITER_BATCH 256
#define TCLOG_NODE_BYTES   256

static
#if defined _WIN32
    unsigned int __stdcall logwriter_threadfun(void* lpParam)
#elif defined __linux
    void*
    logwriter_threadfun(void* lpParam)
#else
#error "error"
#endif
{
    Loger_s* logger = (Loger_s*)lpParam;
    LogBuf_s* batch[TCLOG_WRITER_BATCH];
    size_t nbatch;
    size_t k;
    size_t i;
    char* filebuf;
    size_t filebuf_cap;
    size_t filebuf_len = 0;

    for (i = 0; i < TCLOG_WRITER_BATCH; i++) {
        LogBuf_s* b = (LogBuf_s*)malloc(sizeof(LogBuf_s) + TCLOG_NODE_BYTES);
        if (!b) break;
        b->caplen = TCLOG_NODE_BYTES;
        batch[i] = b;
    }
    nbatch = i;
    if (nbatch == 0) {
#if defined _WIN32 || defined _WIN64
        _endthreadex(0);
#endif
        return 0;
    }

    /* 合并缓冲：一批最多 nbatch 条 × TCLOG_NODE_BYTES。分配失败就退化成
     * 每条一次 fwrite（filebuf_cap 保持 0）。 */
    filebuf_cap = nbatch * TCLOG_NODE_BYTES;
    filebuf = (char*)malloc(filebuf_cap);
    if (!filebuf) filebuf_cap = 0;

    while (!logger->exit) {
        k = thrqueue_dequeue_swap(logger->queue, (void**)batch, nbatch);
        for (i = 0; i < k; i++) {
            LogBuf_s* bf = batch[i];
            if (logger->level_stdout >= bf->level) {
#ifdef TCLOG_PERF
                unsigned long long _w0 = tclog_perf_now();
#endif
                if (logger->colorful) {
                    fprintf(stderr, "%s%s%s", g_colortable[bf->color], bf->content, g_colortable[RESET]);
                } else {
                    fwrite(bf->content, bf->clen, 1, stderr);
                }
#ifdef TCLOG_PERF
                tclog_perf_addw(TCLOG_P_WRITE, tclog_perf_now() - _w0);
#endif
            }
            if (logger->fp && logger->level_file >= bf->level) {
                if (filebuf_len + bf->clen <= filebuf_cap) {
                    memcpy(filebuf + filebuf_len, bf->content, bf->clen);
                    filebuf_len += bf->clen;
                } else {
                    /* 攒不下了：先冲掉已攒的，再放这一条。单条就超过缓冲上限
                     * （只有缓冲分配失败时才会）则直接写。 */
                    tclog_flush_filebuf(logger, filebuf, &filebuf_len);
                    if (bf->clen <= filebuf_cap) {
                        memcpy(filebuf, bf->content, bf->clen);
                        filebuf_len = bf->clen;
                    } else {
                        writetofile(logger, bf->content, bf->clen);
                    }
                }
            }
        }
        /* 每批末尾冲一次：既保证日志及时可见，又把 fwrite 次数从"每条一次"
         * 降到"每批一次"。 */
        tclog_flush_filebuf(logger, filebuf, &filebuf_len);
    }
#if defined _WIN32 || defined _WIN64
    _endthreadex(0);
#endif
    return 0;
}

/* ==================== writer 线程的启停 ====================
 * 原实现（mythread_create）创建完就把句柄丢了（Windows 立刻 CloseHandle、Linux
 * pthread_detach），线程无法回收。两个后果：
 *   1) d_set_async(0) 之后 writer 还活着，永久停在 dequeue 的 cond_wait 上；
 *   2) d_destory_debug 关掉 fp 之后它可能还在用那把 FILE*。
 * 现在把句柄留在 g_logger.writer_handle 里，关异步/退出时能真正 join 回来。
 */

/* 起 writer。返回 0 成功；非 0 表示线程没起来 —— 调用方必须退回同步模式，
 * 否则生产者会往一个没人消费的队列里堆到阻塞。 */
static int tclog_writer_start(void)
{
    if (g_logger.writer_started) {
        return 0;
    }
    g_logger.exit = 0;
#if defined _WIN32 || defined _WIN64
    g_logger.writer_handle = (HANDLE)_beginthreadex(NULL, 0, (MYTHREADFUNROUTE)logwriter_threadfun, &g_logger, 0, NULL);
    if (g_logger.writer_handle == NULL) {
        return -1;
    }
#elif defined __linux
    if (pthread_create(&g_logger.writer_handle, NULL, logwriter_threadfun, &g_logger) != 0) {
        return -1;
    }
#else
#error "not def"
#endif
    g_logger.writer_started = 1;
    return 0;
}

/* 停 writer 并回收句柄：先置 exit，再把停在 notempty 上的它唤醒，最后 join。
 *
 * 注意 thrqueue_unblock_dequeue 是**不可逆**的（block_dequeue 永久置 0），停过
 * 之后这个队列就不能再用于异步了 —— 调用方负责废弃并重建（见 d_set_async）。 */
static void tclog_writer_stop(void)
{
    if (!g_logger.writer_started) {
        return;
    }
    g_logger.exit = 1;
    if (g_logger.queue != NULL) {
        thrqueue_unblock_dequeue(g_logger.queue);
    }
#if defined _WIN32 || defined _WIN64
    WaitForSingleObject(g_logger.writer_handle, INFINITE);
    CloseHandle(g_logger.writer_handle);
    g_logger.writer_handle = NULL;
#elif defined __linux
    pthread_join(g_logger.writer_handle, NULL);
#endif
    g_logger.writer_started = 0;
}

/***************************************************
  print binary buffer info
***************************************************/
void d_debug_bin_buffer(const void* temp, size_t len)
{
    size_t i = 0;
    const unsigned char* data = (const unsigned char*)temp;
    if (g_logger.level_stdout >= DEBUG_DEBUG || g_logger.level_cache == -1) {
        fprintf(stderr, "\n");
        for (i = 0; i < len; i++) {
            fprintf(stderr, "0x%02hhx, ", (unsigned char)*(data + i));
            if (i % 16 == 0 && i) {
                fprintf(stderr, "\n");
            }
        }

        if (len > 0 && (i - 1) % 16 != 0) {
            fprintf(stderr, "\n");
        }
    }

    if (g_logger.fp && g_logger.level_file > DEBUG_DEBUG) {
        fprintf(g_logger.fp, "\n");
        for (i = 0; i < len; i++) {
#if 0
            if(isprint(*(data+i)) || isspace(*(data+i))){
                fprintf(g_logger.log_fp, "%c",(unsigned char)*(data+i));
            }else
#endif
            {
                if (g_logger.level_file != DEBUG_VERBOSE && len > 30) {
                    if (i < 20 || (len - i) < 10) {
                        fprintf(g_logger.fp, "\\%02X", (unsigned char)*(data + i));
                    } else if (i == 20) {
                        fprintf(g_logger.fp, "\\......\\");
                    }
                } else {
                    fprintf(g_logger.fp, "\\%02hhX", (unsigned char)*(data + i));
                }
            }
        }
        fprintf(g_logger.fp, "\n");
    }
}

void d_destory_debug()
{
    time_t t = time(NULL);
    struct tm ptm;
    char buf[1024] = "";
    localtime_r(&t, &ptm);

    /* writer 可能仍在用 g_logger.fp，必须先把它停下来再 fclose —— 原实现直接
     * fclose，而 writer 还活着（只是没人唤醒它），是个关闭竞态。 */
    tclog_writer_stop();

    if (g_logger.fp) {
        TCLOG_SNPRINTF(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
            ptm.tm_year + 1900, ptm.tm_mon + 1, ptm.tm_mday, ptm.tm_hour, ptm.tm_min, ptm.tm_sec);
        buf[sizeof(buf) - 1] = '\0';
        fprintf(g_logger.fp, "\n====== End logging at %s. ======\n", buf);
        fclose(g_logger.fp);
    }
    g_logger.fp = NULL;
    g_logger.level_stdout = DEBUG_INFO;
    g_logger.level_file = DEBUG_ERROR;
    g_logger.exit = 1;
    g_logger.initialized = 0;
    /* 队列已随 writer 一起废弃（unblock_dequeue 不可逆），指针清掉免得后面误用。
     * 内存仍按原实现的取舍不在这里释放，见下面 #if 0 的说明。 */
    g_logger.queue = NULL;

#if 0
    //this should release when process exit.but it will auto release then, and may be crash if we release here.
    // so we do nothing.
    if(g_logger.queue){
        thrqueue_unblock_dequeue(g_logger.queue);
        thrqueue_unblock_enqueue(g_logger.queue);
        thrqueue_free(g_logger.queue);
    }
    if(g_logger.outputFileName){
        free(g_logger.outputFileName);
        g_logger.outputFileName = NULL;
    }
#endif
    fprintf(stderr, "destory debug\n");
}

#if _WIN32
#define ___mkdir(dir) mkdir(dir)
#else
#define ___mkdir(dir) mkdir(dir, 0777)
#endif
static int __mkdir_p(const char* dir)
{
    int ret = 0;
    char* tmp;
    char* p;
    char delim = '/';

    if (access(dir, 0) == 0) return EEXIST;

    tmp = strdup(dir);
    if (!tmp) return ENOMEM;
    p = tmp;
    while (*p) {
#ifdef _WIN32
        if (*p == '/' || *p == '\\') {
            delim = *p;
#else
        if (*p == '/') {
#endif
            *p = '\0';
            ___mkdir(tmp);
            *p = delim;
        }
        ++p;
    }
    if (___mkdir(tmp) != 0) ret = EPERM;
    free(tmp);
    return ret;
}

void d_init_debug(const char* logdir, size_t logfilesize, size_t maxlogfilenumber, int level, int log, const char* binname)
{
    time_t t = time(NULL);
    struct tm ptm = {0};
    localtime_r(&t, &ptm);

/* 原先这里在 VC6 下调 uv_winapi_init()（父项目 libuv 兼容层的初始化，用来动态
 * 解析 kernel32 的条件变量等符号）。tclog 现在自带 compat/tclog_win32.h，不再
 * 依赖那一层，这个调用也就不需要了 —— 留着还会在独立构建里报 undefined symbol。 */
    /* 重新初始化：先把上一轮的 writer 停掉，免得它在下面换队列/关文件之后再动
     * g_logger 里的东西。 */
    tclog_writer_stop();
    if (g_logger.fp) {
        d_destory_debug();      /* 里面也会 stop（此时是 no-op），然后再关 fp */
    }
    g_logger.initialized = 1;
    if (logfilesize > 0) {
        g_logger.logRotateSizeBytes = logfilesize * 1024;
    }
    if (maxlogfilenumber > 0) {
        g_logger.maxRotatedLogs = maxlogfilenumber;
    }
    g_logger.level_stdout = level;
    g_logger.level_file = log;
    g_logger.level_cache = level > log ? level : log;
    g_logger.exit = 0;
    g_logger.queue = thrqueue_new(1024);
    if (g_logger.queue == NULL) {
        fprintf(stderr, "new queue failed, fall back to sync logging\n");
        g_logger.async = 0;
        goto END;
    }

    if (g_logger.level_file <= DEBUG_NONE)
        goto END;

    if (binname == NULL) {
        binname = "";
    }
    if (logdir != NULL) {
        __mkdir_p(logdir);
        g_logger.outputFileName = tclog_fmt_str("%s/%s.log", logdir, binname);
    } else {
        g_logger.outputFileName = tclog_fmt_str("%s.log", binname);
    }
    if (!g_logger.outputFileName) goto END;
    g_logger.fp = fopen(g_logger.outputFileName, "at");
    if (!g_logger.fp)
        goto END;
    fprintf(g_logger.fp, "====== Start logging at %04d-%02d-%02d %02d:%02d:%02d. ======\n", ptm.tm_year + 1900, ptm.tm_mon + 1, ptm.tm_mday, ptm.tm_hour, ptm.tm_min, ptm.tm_sec);
END:
    if (g_logger.async) {
        if (tclog_writer_start() != 0) {
            /* 线程起不来：退回同步。否则生产者会往一个没人消费的队列里堆到阻塞。 */
            g_logger.async = 0;
        }
    }
    return;
}

static void refresh_level_cache()
{
    g_logger.level_cache = g_logger.level_stdout > g_logger.level_file
                         ? g_logger.level_stdout : g_logger.level_file;
    if (g_logger.async && !g_logger.queue) g_logger.async = 0;
}

// set debug level
int d_set_debug_level(int level)
{
    if (level < DEBUG_MIN || level > DEBUG_MAX)
        return -1;

    g_logger.level_stdout = level;
    refresh_level_cache();

    return 0;
}

// set log level
int d_set_log_level(int level)
{
    if (level < DEBUG_MIN || level > DEBUG_MAX)
        return -1;

    g_logger.level_file = level;
    refresh_level_cache();

    return 0;
}

/* 切同步/异步。语义见 debug.h：
 *   - 在 d_init_debug 之前调用：只记录意图。于是 "d_set_async(0) + d_init_debug"
 *     的组合下 writer 线程**根本不会被创建**，同步模式零线程。
 *   - 在 d_init_debug 之后调用：置 0 会真的停掉并回收 writer；置 1 会重建队列和
 *     线程（停过一次的队列因为 unblock_dequeue 不可逆，不能再用于异步，所以这里
 *     让 "queue != NULL" 恒等于 "有可用队列"）。
 * 返回 0 成功；-1 表示想开异步但队列/线程拿不到，此时保持同步。 */
int d_set_async(int on)
{
    on = on ? 1 : 0;

    if (!g_logger.initialized) {
        g_logger.async = on;        /* init 之前只记意图，线程由 d_init_debug 决定 */
        return 0;
    }

    if (on) {
        if (g_logger.queue == NULL) {
            g_logger.queue = thrqueue_new(1024);
        }
        if (g_logger.queue == NULL || tclog_writer_start() != 0) {
            /* 队列或线程拿不到：退回同步，绝不让生产者往没人消费的队列里堆。 */
            g_logger.async = 0;
            refresh_level_cache();
            return -1;
        }
        g_logger.async = 1;
    } else {
        /* 先把生产者切走，再停 writer、回收队列。 */
        g_logger.async = 0;
        tclog_writer_stop();
        if (g_logger.queue != NULL) {
            thrqueue_free(g_logger.queue);
            g_logger.queue = NULL;
        }
    }
    refresh_level_cache();
    return 0;
}

int d_log_enabled(int level)
{
    return (g_logger.level_cache < 0) || (level <= g_logger.level_cache);
}

void d_log_flush()
{
    if (g_logger.fp) fflush(g_logger.fp);
}

// get level string
static const char* d_level_string(int level)
{
    const char* ls[] = {
        "0",
        "F",
        "E",
        "W",
        "I",
        "D",
        "V",
        "UNK"
    };
    int ls_num = sizeof(ls) / sizeof(ls[0]);

    if (level < 0 || level >= ls_num)
        level = ls_num - 1;

    return ls[level];
}

/* 把"缓存前缀 + 手写毫秒"写进 buf（截断安全）。调用方保证 bufsz > 0。
 * 前缀长度可变，'.' + 3 位毫秒共 4 字节，另需 1 字节结束符。 */
static void tclog_fmt_ms(char* buf, size_t bufsz, const char* prefix,
                         size_t plen, int ms)
{
    if (plen + 5 > bufsz) {
        size_t cp = (plen < bufsz - 1) ? plen : bufsz - 1;
        memcpy(buf, prefix, cp);
        buf[cp] = '\0';
    } else {
        memcpy(buf, prefix, plen);
        buf[plen]     = '.';
        buf[plen + 1] = (char)('0' + ms / 100);
        buf[plen + 2] = (char)('0' + (ms / 10) % 10);
        buf[plen + 3] = (char)('0' + ms % 10);
        buf[plen + 4] = '\0';
    }
}

// Return current time in string format
/* 秒级时间戳缓存，两个平台共用一套实现，只是 key 的取法不同：
 *  - Unix：key = tv_sec。"日期+时分秒"是 tv_sec 的纯函数，所以按秒缓存严格
 *    正确；时钟回拨到同一秒也只会得到同一串。省掉 localtime_r(~303ns) 和
 *    6 段 snprintf(~318ns)。
 *  - Windows：key 由 GetLocalTime 的字段拼成。GetLocalTime 本身不贵（读共享
 *    内存里的时间 + 缓存时区），贵的是随后的 _snprintf 格式化，所以这里省的是
 *    格式化而不是取时。输出格式与 Unix 完全一致。
 * 缓存放 TLS：无需同步、无伪共享，代价只是每线程每秒多付一次时间转换。
 * 命中路径上不再调用 snprintf：毫秒用三次除法手写（'.' + 3 位数字）。 */
static char* strtime(char* buf, size_t bufsz)
{
    /* 用 uint64_t 而不是 unsigned long long：VC6 不认后者（C2632），只有 __int64，
     * 而 portable/vc6_stdint.h 里的 uint64_t 正是它。 */
    static TCLOG_TLS uint64_t t_cached_key;
    static TCLOG_TLS char t_cached_prefix[32];
    static TCLOG_TLS size_t t_cached_len;
#if defined(_WIN32) || defined(_WIN64)
    SYSTEMTIME wtm;
#else
    struct tm ptm = {0};
    struct timeval tv = {0};
#endif
    uint64_t key;
    int ms;
#ifdef TCLOG_PERF
    /* _t1/_t2 初值取 _t0：Windows 分支不单独计时，退化成 TS 全额 */
    unsigned long long _t0 = tclog_perf_now();
    unsigned long long _t1 = _t0;
    unsigned long long _t2 = _t0;
#endif

    if (bufsz == 0) return buf;

#if defined(_WIN32) || defined(_WIN64)
    GetLocalTime(&wtm);
    ms = (int)wtm.wMilliseconds;
    /* 位宽预算：月 4 + 日 5 + 时 5 + 分 6 + 秒 6 = 26 位，年放高 16 位 */
    key = ((uint64_t)wtm.wYear << 32)
        | ((uint64_t)wtm.wMonth << 28)
        | ((uint64_t)wtm.wDay << 23)
        | ((uint64_t)wtm.wHour << 18)
        | ((uint64_t)wtm.wMinute << 12)
        | ((uint64_t)wtm.wSecond << 6);
#else
    gettimeofday(&tv, NULL);
#ifdef TCLOG_PERF
    _t1 = tclog_perf_now();
#endif
    ms = (int)(tv.tv_usec / 1000);
    key = (uint64_t)tv.tv_sec;
#endif

    if (key != t_cached_key || t_cached_len == 0) {
        int n;
#if defined(_WIN32) || defined(_WIN64)
        n = TCLOG_SNPRINTF(t_cached_prefix, sizeof(t_cached_prefix),
            "%02d-%02d %02d:%02d:%02d",
            wtm.wMonth, wtm.wDay, wtm.wHour, wtm.wMinute, wtm.wSecond);
#else
        localtime_r(&tv.tv_sec, &ptm);
#ifdef TCLOG_PERF
        _t2 = tclog_perf_now();
#endif
        n = TCLOG_SNPRINTF(t_cached_prefix, sizeof(t_cached_prefix),
            "%02d-%02d %02d:%02d:%02d",
            ptm.tm_mon + 1, ptm.tm_mday, ptm.tm_hour, ptm.tm_min, ptm.tm_sec);
#endif
        if (n < 0 || (size_t)n >= sizeof(t_cached_prefix)) n = (int)sizeof(t_cached_prefix) - 1;
        t_cached_prefix[n] = '\0';
        t_cached_len = (size_t)n;
        t_cached_key = key;
    }
#ifdef TCLOG_PERF
    else {
        _t2 = _t1;   /* 命中：本地时间转换根本没被调用 */
    }
#endif

    tclog_fmt_ms(buf, bufsz, t_cached_prefix, t_cached_len, ms);
    buf[bufsz - 1] = '\0';
#ifdef TCLOG_PERF
    {
        unsigned long long _t3 = tclog_perf_now();
        tclog_perf_add(TCLOG_P_TS, _t3 - _t0);
        tclog_perf_add(TCLOG_P_TS_LT, _t2 - _t1);
        tclog_perf_add(TCLOG_P_TS_FMT, _t3 - _t2);
    }
#endif
    return buf;
}

/* ---------- 前缀拼接 ----------
 * 原来每条日志都要一次 snprintf("%s %s/%s(%lx):")；前缀只是"定长时间戳 +
 * 1 字符 level + 短 tag + 十六进制 tid"，没有走格式解析的必要。改成有界手写
 * 拼接，语义与 snprintf 截断一致：空间不足就地截断并保证 NUL 结束。 */

static size_t tclog_append_str(char* dst, size_t cap, size_t pos, const char* s)
{
    while (*s != '\0' && pos + 1 < cap) {
        dst[pos++] = *s++;
    }
    return pos;
}

static size_t tclog_append_hex(char* dst, size_t cap, size_t pos, unsigned long v)
{
    char tmp[2 * sizeof(unsigned long) + 2];
    size_t n = 0;

    if (v == 0) {
        tmp[n++] = '0';
    }
    while (v != 0) {
        tmp[n++] = "0123456789abcdef"[v & 0xFUL];
        v >>= 4;
    }
    while (n > 0 && pos + 1 < cap) {
        dst[pos++] = tmp[--n];
    }
    return pos;
}
#ifndef PRIx64
#if defined(_MSC_VER)
/* MSVC 到 VS2013 才支持 %ll，之前是 %I64。本宏只在下面 VC6 那条路径用到。 */
#define PRIx64 "I64x"
#else
#define PRIx64 "lx"
#endif
#endif

/* ==================== 同步输出的整行缓冲 ====================
 * 原来的同步分支把一条日志拆成三次 stdio 调用（前缀 fprintf + 正文 vfprintf +
 * 换行），中间没有锁，多生产者下别的线程会插进来：实测 4 线程 42 万行里约 9.8 万
 * 行被拼坏（两行的前缀/正文互相穿插）。改成"整行格式化一次，再对每个目的地各写
 * 一次"——stdio 单次调用内部有锁，一行不会被拆开。顺带也省掉了 stdout 与文件同时
 * 开启时的重复格式化（原来要 vfprintf 两遍）。
 *
 * 缓冲策略：先用调用方给的栈缓冲（覆盖绝大多数日志，零分配），放不下才按需 malloc
 * 一条够大的、用完 free。刻意不用 TLS：VC6 没有 __declspec(thread)（VS2002 才有），
 * 而这是"慢但简单可靠"的同步回退路径，不值得为它引入平台差异。也刻意不截断 ——
 * 原来的同步分支就是多长写多长，这里保持。
 */
#define TCLOG_SYNC_STACK 512

/* 把"前缀 + 正文"整条格式化出来，不截断。
 * 返回缓冲（可能是 _stack，也可能是 malloc 的，后者由调用方 free）；长度写进
 * *_plen（不含结尾 NUL）。返回 NULL 表示拿不到更大的缓冲，调用方跳过本条。
 * _append_nl：末尾补一个 '\n' —— VC6 那条路径的 LOGI/LOGE 是普通函数，格式串不
 * 带换行；VS2005+ 的 LOG 宏已经把 '\n' 附在 format 尾部（见 debug.h 的 __LOG_AT）。 */
static char* tclog_fmt_line(char* _stack, size_t _stackcap, size_t* _plen,
                            const char* ts, const char* ls, const char* tag,
                            unsigned long pid, const char* format, va_list ap,
                            int _append_nl)
{
    char* buf = _stack;
    size_t cap = _stackcap;

    for (;;) {
        size_t hlen = 0;
        int n = 0;
        int trunc = 0;
        va_list ap2;

        hlen = tclog_append_str(buf, cap, hlen, ts);
        hlen = tclog_append_str(buf, cap, hlen, " ");
        hlen = tclog_append_str(buf, cap, hlen, ls);
        hlen = tclog_append_str(buf, cap, hlen, "/");
        hlen = tclog_append_str(buf, cap, hlen, tag);
        hlen = tclog_append_str(buf, cap, hlen, "(");
        hlen = tclog_append_hex(buf, cap, hlen, pid);
        hlen = tclog_append_str(buf, cap, hlen, "):");
        buf[hlen] = '\0';

        TCLOG_VA_COPY(ap2, ap);
        n = __log_vsnprintf(buf + hlen, cap - hlen, format, ap2, &trunc);
        va_end(ap2);
        if (n < 0) {
            n = 0;
        }
        hlen += (size_t)n;
        buf[hlen] = '\0';

        if (_append_nl) {
            size_t before = hlen;
            hlen = tclog_append_str(buf, cap, hlen, "\n");
            if (hlen == before) {
                trunc = 1;          /* 连换行都放不下，得扩容重来 */
            }
        }
        if (!trunc) {
            *_plen = hlen;
            return buf;
        }

        if (buf != _stack) {
            free(buf);
        }
        cap = cap * 2 + 1;
        buf = (char*)malloc(cap);
        if (buf == NULL) {
            return NULL;            /* 内存不足：本条不写，绝不截断或越界 */
        }
    }
}

/* 同步写出一条日志：整行只格式化一次，对每个目的地各一次 fwrite。 */
static void tclog_write_sync_line(int level, const char* ts, const char* ls,
                                  const char* tag, unsigned long pid,
                                  const char* format, va_list ap, int append_nl)
{
    char stackbuf[TCLOG_SYNC_STACK];
    size_t len = 0;
    char* line;

    if (g_logger.level_stdout < level
        && !(g_logger.fp != NULL && g_logger.level_file >= level)) {
        return;                     /* 两个目的地都不要，连格式化都省掉 */
    }

    line = tclog_fmt_line(stackbuf, sizeof(stackbuf), &len,
                          ts, ls, tag, pid, format, ap, append_nl);
    if (line == NULL) {
        return;
    }

    if (g_logger.level_stdout >= level) {
        fwrite(line, 1, len, stderr);
    }
    if (g_logger.fp != NULL && g_logger.level_file >= level) {
        fwrite(line, 1, len, g_logger.fp);
        if (g_logger.flush) {
            fflush(g_logger.fp);
        }
    }

    if (line != stackbuf) {
        free(line);
    }
}

#if defined(_MSC_VER) && _MSC_VER < 1400 // less vc2005

/*
 * 把"前缀 + 正文"格式化进 head，放不下就把节点扩容后整条重来；新节点写回
 * *stub，保证队列槽位始终指向同一个节点。返回最终长度（含结尾 '\n'，不含 NUL）。
 *
 * 为什么这条路径不能像 __d_debug 那样用 TLS 暂存节点：VC6 没有
 * __declspec(thread)（VS2002 才有），所以只能直接在槽位节点里格式化——也正因
 * 如此扩容必须由自己处理。
 *
 * 两个坑：
 *  - VC6 的 _vsnprintf 截断时返回 -1，拿不到"所需长度"，所以扩容决策只能靠
 *    __log_vsnprintf 的 truncated 标志；
 *  - 每次重试都必须用**未消耗**的 va_list 副本（ap 在第一次调用后就失效了），
 *    原实现直接复用 ap，是 UB。
 */
static size_t tclog_fmt_logbuf(LogBuf_s* head, void** stub,
                               const char* ts, const char* ls, const char* tag,
                               uint64_t pid, const char* format, va_list ap)
{
    size_t hlen;

    for (;;) {
        size_t avail;
        size_t newcap;
        int n = 0;
        int trunc = 0;
        va_list ap2;
        LogBuf_s* nhead;

        hlen = (size_t)sprintf(head->content, "%s %s/%s(%" PRIx64 "):", ts, ls, tag, pid);
        avail = head->caplen - hlen;
        TCLOG_VA_COPY(ap2, ap);
        if (avail >= 2) {           /* 至少留出 '\n' 和结尾 NUL 的位置 */
            n = __log_vsnprintf(head->content + hlen, avail - 1, format, ap2, &trunc);
        } else {
            trunc = 1;
        }
        va_end(ap2);
        if (!trunc) {
            hlen += (size_t)n;
            break;
        }

        newcap = (head->caplen < 512) ? 1024 : head->caplen * 2;
        nhead = (LogBuf_s*)malloc(sizeof(LogBuf_s) + newcap);
        if (!nhead) {               /* 内存不足：接受截断，但不越界、不崩 */
            hlen += (size_t)n;
            break;
        }
        nhead->caplen = newcap;
        free(head);
        *stub = head = nhead;
    }

    if (hlen + 2 > head->caplen) {  /* 兜底：节点小到极端的边界情况 */
        hlen = (head->caplen >= 2) ? (head->caplen - 2) : 0;
    }
    head->content[hlen] = '\n';
    head->content[hlen + 1] = '\0';
    return hlen + 1;
}

#define __LOGALL(_level, _tag, _pid, _color)                                                        \
    int level = _level;                                                                             \
    const char* tag = _tag;                                                                         \
    const uint64_t pid = _pid;                                                                      \
    const enum COLOR color = _color;                                                                \
    va_list ap;                                                                                     \
    const char* ls;                                                                                 \
    char ts[32];                                                                                    \
    LogBuf_s* head = NULL;                                                                          \
    size_t hlen = 128;                                                                              \
    if (level <= g_logger.level_cache || g_logger.level_cache == -1) {                              \
        strtime(ts, sizeof(ts));                                                                                \
        ls = d_level_string(level);                                                                 \
        va_start(ap, format);                                                                       \
        if (level <= g_logger.level_cache) {                                                        \
            if (g_logger.async) {                                                                   \
                void** stub = thrqueue_enq_getstub(g_logger.queue);                                 \
                if (*stub) {                                                                        \
                    head = *stub;                                                                   \
                } else {                                                                            \
                    *stub = head = (LogBuf_s*)malloc(sizeof(LogBuf_s) + hlen);                      \
                    if (!head) { va_end(ap); return; }                                              \
                    head->caplen = hlen;                                                            \
                }                                                                                   \
                {                                                                                   \
                    size_t _clen = (size_t)tclog_fmt_logbuf(head, stub, ts, ls, tag, pid, format, ap); \
                    head = (LogBuf_s*)*stub;    /* 扩容后 tclog_fmt_logbuf 已把新节点写回 *stub */ \
                    head->level = level;                                                            \
                    head->color = color;                                                            \
                    head->clen = _clen;                                                             \
                }                                                                                   \
                thrqueue_enq_done(g_logger.queue);                                                  \
            } else {                                                                                \
                tclog_write_sync_line(level, ts, ls, tag, (unsigned long)pid, format, ap, 1);       \
            }                                                                                       \
        } else if (g_logger.level_cache == -1) {                                                    \
            fprintf(stderr, "%s %s/%s(%" PRIx64 "):", ts, ls, tag, pid);                            \
            vfprintf(stderr, format, ap);                                                           \
            fputc('\n', stderr);                                                                    \
        }                                                                                           \
        va_end(ap);                                                                                 \
    }

void LOGF(const char* format, ...)
{
    __LOGALL(DEBUG_FATAL, "", _gettid(), KRED)
    abort();
}

void LOGE(const char* format, ...)
{
    __LOGALL(DEBUG_ERROR, "", _gettid(), KRED);
}

void LOGW(const char* format, ...)
{
    __LOGALL(DEBUG_WARNING, "", _gettid(), KBRN)
}
void LOGI(const char* format, ...)
{
    __LOGALL(DEBUG_INFO, "", _gettid(), KGRN)
}
void LOGD(const char* format, ...)
{
    __LOGALL(DEBUG_DEBUG, "", _gettid(), KBLU)
}
void LOGV(const char* format, ...)
{
#if NDEBUG
    __LOGALL(DEBUG_VERBOSE, "", _gettid(), KCYN)
#else
    /* 原实现这里手工展开了一份 __LOGALL，并手工模拟 va_start
     * （(ap = (va_list)&format + ...)）——那是 32 位 x86 的取参写法，换到
     * x64 就完全错；末尾还用 (ap = (va_list)0) 代替 va_end。现在改成
     * va_start/va_end，节点扩容交给 tclog_fmt_logbuf；level/color 原本误抄成
     * DEBUG_DEBUG/KBLU，一并修正为 VERBOSE/KCYN。
     * 保留"手工展开"的形态，是为了让两个 NDEBUG 分支行为完全一致。 */
    int level = DEBUG_VERBOSE;
    const char* tag = "";
    const uint64_t pid = _gettid();
    const enum COLOR color = KCYN;
    va_list ap;
    const char* ls;
    char ts[32];
    LogBuf_s* head = ((void*)0);
    size_t hlen = 128;
    if (level <= g_logger.level_cache || g_logger.level_cache == -1) {
        strtime(ts, sizeof(ts));
        ls = d_level_string(level);
        va_start(ap, format);
        if (level <= g_logger.level_cache) {
            if (g_logger.async) {
                void** stub = thrqueue_enq_getstub(g_logger.queue);
                size_t clen;
                if (*stub) {
                    head = *stub;
                } else {
                    *stub = head = (LogBuf_s*)malloc(sizeof(LogBuf_s) + hlen);
                    if (!head) { va_end(ap); return; }
                    head->caplen = hlen;
                }
                clen = (size_t)tclog_fmt_logbuf(head, stub, ts, ls, tag, pid, format, ap);
                head = (LogBuf_s*)*stub;    /* 扩容后 tclog_fmt_logbuf 已把新节点写回 *stub */
                head->level = level;
                head->color = color;
                head->clen = clen;
                thrqueue_enq_done(g_logger.queue);
            } else {
                tclog_write_sync_line(level, ts, ls, tag, (unsigned long)pid, format, ap, 1);
            }
        } else if (g_logger.level_cache == -1) {
            fprintf(stderr, "%s %s/%s(%" PRIx64 "):", ts, ls, tag, pid);
            vfprintf(stderr, format, ap);
            fputc('\n', stderr);
        }
        va_end(ap);
    }
#endif
}

#else // less vc2005
void __d_debug(int level, const char* tag, const unsigned long pid, const enum COLOR color, const char* format, ...)
{
    va_list ap;
    const char* ls;
    char ts[32];
    LogBuf_s* head = NULL;
    size_t hlen = 256;
#ifdef TCLOG_PERF
    unsigned long long _pt0 = 0;
    unsigned long long _pa = 0;
    unsigned long long _pb = 0;
#endif
    if (level <= g_logger.level_cache || g_logger.level_cache == -1) {
#ifdef TCLOG_PERF
        _pt0 = tclog_perf_now();
#endif
        strtime(ts, sizeof(ts));
        ls = d_level_string(level);

        va_start(ap, format);
        if (level <= g_logger.level_cache) {
            if (g_logger.async && g_logger.queue) {
                int n;
                void** stub;
                LogBuf_s* prev;

                /* 日志先格式化进本线程的暂存节点——不持锁。原来这段（实测
                 * ~370ns）在临界区内，等于把所有生产者的格式化用同一把锁
                 * 串行化；这是 Q:hold 的主要成分。 */
                if (t_logbuf == NULL || t_logbuf->caplen < hlen) {
                    if (t_logbuf) free(t_logbuf);
                    t_logbuf = (LogBuf_s*)malloc(sizeof(LogBuf_s) + hlen);
                    if (!t_logbuf) { va_end(ap); return; }
                    t_logbuf->caplen = hlen;
                }
                head = t_logbuf;
#ifdef TCLOG_PERF
                _pb = tclog_perf_now();
#endif
                hlen = 0;
                hlen = tclog_append_str(head->content, head->caplen, hlen, ts);
                hlen = tclog_append_str(head->content, head->caplen, hlen, " ");
                hlen = tclog_append_str(head->content, head->caplen, hlen, ls);
                hlen = tclog_append_str(head->content, head->caplen, hlen, "/");
                hlen = tclog_append_str(head->content, head->caplen, hlen, tag);
                hlen = tclog_append_str(head->content, head->caplen, hlen, "(");
                hlen = tclog_append_hex(head->content, head->caplen, hlen, pid);
                hlen = tclog_append_str(head->content, head->caplen, hlen, "):");
                head->content[hlen] = '\0';
                /* 节点容量固定（见 TCLOG_NODE_BYTES），超长正文就地截断：
                 * 不关心 truncated，传 NULL。 */
                n = __log_vsnprintf(head->content + hlen, head->caplen - hlen, format, ap, NULL);
                if (n < 0) n = 0;
                hlen += (size_t)n;
                head->content[hlen] = '\0';
#ifdef TCLOG_PERF
                tclog_perf_add(TCLOG_P_FMT, tclog_perf_now() - _pb);
#endif
                head->level = level;
                head->color = color;
                head->clen = hlen;
                stub = thrqueue_enq_getstub(g_logger.queue);
#ifdef TCLOG_PERF
                _pa = tclog_perf_now();
#endif
                if (!stub) { va_end(ap); return; }
                prev = (LogBuf_s*)*stub;
                *stub = head;
                t_logbuf = prev;
                thrqueue_enq_done(g_logger.queue);
#ifdef TCLOG_PERF
                tclog_perf_add(TCLOG_P_Q_HOLD, tclog_perf_now() - _pa);
#endif
            } else { // sync
#ifdef TCLOG_PERF
                _pa = tclog_perf_now();
#endif
                /* 整行一次格式化 + 每个目的地一次 fwrite（原来 前缀/正文/换行 是
                 * 三次独立 stdio 调用，多生产者下会被别的线程拆开）。 */
                tclog_write_sync_line(level, ts, ls, tag, pid, format, ap, 0);
#ifdef TCLOG_PERF
                tclog_perf_add(TCLOG_P_WRITE, tclog_perf_now() - _pa);
#endif
            }
        } else if (g_logger.level_cache == -1) { // not call init
            fprintf(stderr, "%s %s/%s(%lx):", ts, ls, tag, pid);
            vfprintf(stderr, format, ap);
        }
        va_end(ap);
#ifdef TCLOG_PERF
        tclog_perf_add(TCLOG_P_TOTAL, tclog_perf_now() - _pt0);
#endif
    }
}

#endif // less vc2005

#ifdef TCLOG_PERF
void tclog_perf_reset(void)
{
    memset(tclog_perf_ns, 0, sizeof(tclog_perf_ns));
    memset(tclog_perf_cnt, 0, sizeof(tclog_perf_cnt));
}

void tclog_perf_accum(double ns[TCLOG_P_N], unsigned long long cnt[TCLOG_P_N])
{
    int i;
    for (i = 0; i < TCLOG_P_N; i++) {
        ns[i] = (double)tclog_perf_ns[i];
        cnt[i] = tclog_perf_cnt[i];
    }
}

void tclog_perf_writer_accum(double ns[TCLOG_P_N], unsigned long long cnt[TCLOG_P_N])
{
    int i;
    for (i = 0; i < TCLOG_P_N; i++) {
        ns[i] = (double)tclog_perf_w_ns[i];
        cnt[i] = tclog_perf_w_cnt[i];
    }
}
#endif // TCLOG_PERF

#endif // DEBUG_SHOW_DEBUG
#endif //!__ANDROID__
