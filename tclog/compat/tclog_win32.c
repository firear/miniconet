/*
 * compat/tclog_win32.c — tclog 自带 Windows 兼容层的实现。
 * 设计取舍与正确性说明见 compat/tclog_win32.h。
 *
 * 为什么单独一个 .c 而不是全塞在头文件里：后端状态（解析结果 + 4 个原生函数指针
 * + 强制开关）必须是**全进程一份**。若放头文件里用 static 变量，每个包含它的
 * 编译单元会各持一份，tclog_cv_force_emulation() 就只影响调用方自己那份，开关形
 * 同虚设。
 *
 * 文件结构：
 *   [1] 后端状态
 *   [2] 自带实现（计数信号量 + 等待者计数）
 *   [3] 后端解析（一次性，运行期）
 *   [4] 对外接口：挑后端 + 转发
 *   [5] 诊断 / 测试
 */
#include "compat/tclog_win32.h"

#if defined(_WIN32) || defined(_WIN64)

#include <stdlib.h>     /* getenv */

#ifndef TCLOG_INLINE
#  if defined(_MSC_VER)
#    define TCLOG_INLINE __inline
#  else
#    define TCLOG_INLINE __inline__
#  endif
#endif

/* ======================= [1] 后端状态 ======================= */
typedef enum {
    TCLOG_CV_UNKNOWN = 0,
    TCLOG_CV_NATIVE  = 1,
    TCLOG_CV_EMU     = 2
} tclog_cv_backend_t;

#if TCLOG_CV_NATIVE_CANDIDATE
/* 原生 4 个入口一律先取址。注意这里只出现函数指针类型，绝不直接引用
 * InitializeConditionVariable 之类的符号 —— 那会把它写进导入表。 */
typedef VOID (WINAPI* tclog_pfn_init_cv)(PCONDITION_VARIABLE);
typedef BOOL (WINAPI* tclog_pfn_sleep_cv)(PCONDITION_VARIABLE, PCRITICAL_SECTION, DWORD);
typedef VOID (WINAPI* tclog_pfn_wake_cv)(PCONDITION_VARIABLE);

static tclog_pfn_init_cv  g_nat_init;
static tclog_pfn_sleep_cv g_nat_wait;
static tclog_pfn_wake_cv  g_nat_signal;
static tclog_pfn_wake_cv  g_nat_broadcast;

static volatile LONG g_cv_backend   = TCLOG_CV_UNKNOWN;
static volatile LONG g_cv_force_emu = 0;
#endif /* TCLOG_CV_NATIVE_CANDIDATE */

/* ======================= [2] 自带实现 ======================= */
/* waiters 的增减不需要 interlocked：wait 按契约在持有 cs 时调用，所以所有等待者
 * 的 ++ 互斥；-- 发生在重新 EnterCriticalSection(cs) 之后，同样互斥。唯一的无锁
 * 访问是 signal/broadcast 的读，按头文件里那段时序它必然能看到已登记的等待者。
 * 这样写同时避开了 VC6 里 InterlockedCompareExchange 是 PVOID 签名带来的类型
 * 不匹配。 */

static void tclog_cv_emu_init(tclog_cv_t* cv)
{
    cv->emu.waiters = 0;
    cv->emu.sema = CreateSemaphore(NULL, 0, 0x7fffffff, NULL);
}

static void tclog_cv_emu_destroy(tclog_cv_t* cv)
{
    if (cv->emu.sema != NULL) {
        CloseHandle(cv->emu.sema);
        cv->emu.sema = NULL;
    }
}

static BOOL tclog_cv_emu_wait(tclog_cv_t* cv, CRITICAL_SECTION* cs, DWORD ms)
{
    DWORD rc;

    cv->emu.waiters++;              /* 必须在释放 cs 之前登记 */
    LeaveCriticalSection(cs);
    rc = WaitForSingleObject(cv->emu.sema, ms);
    EnterCriticalSection(cs);
    cv->emu.waiters--;

    return rc == WAIT_OBJECT_0;
}

static void tclog_cv_emu_signal(tclog_cv_t* cv)
{
    if (cv->emu.waiters > 0) {
        ReleaseSemaphore(cv->emu.sema, 1, NULL);
    }
}

static void tclog_cv_emu_broadcast(tclog_cv_t* cv)
{
    LONG n = cv->emu.waiters;

    while (n-- > 0) {
        ReleaseSemaphore(cv->emu.sema, 1, NULL);
    }
}

/* ======================= [3] 后端解析（一次性） ======================= */

#if TCLOG_CV_NATIVE_CANDIDATE
static tclog_cv_backend_t tclog_cv_probe_native(void)
{
    HMODULE k32 = GetModuleHandleA("kernel32.dll");

    if (k32 == NULL) {
        return TCLOG_CV_EMU;
    }

    g_nat_init      = (tclog_pfn_init_cv) GetProcAddress(k32, "InitializeConditionVariable");
    g_nat_wait      = (tclog_pfn_sleep_cv)GetProcAddress(k32, "SleepConditionVariableCS");
    g_nat_signal    = (tclog_pfn_wake_cv) GetProcAddress(k32, "WakeConditionVariable");
    g_nat_broadcast = (tclog_pfn_wake_cv) GetProcAddress(k32, "WakeAllConditionVariable");

    if (g_nat_init != NULL && g_nat_wait != NULL
        && g_nat_signal != NULL && g_nat_broadcast != NULL) {
        return TCLOG_CV_NATIVE;
    }

    /* 只有半套可用没有意义，整体回退并清干净，免得后面误用。 */
    g_nat_init      = NULL;
    g_nat_wait      = NULL;
    g_nat_signal    = NULL;
    g_nat_broadcast = NULL;
    return TCLOG_CV_EMU;
}

static tclog_cv_backend_t tclog_cv_resolve(void)
{
    tclog_cv_backend_t b;

    if (g_cv_force_emu || getenv("TCLOG_CV_EMU") != NULL) {
        b = TCLOG_CV_EMU;
    } else {
        b = tclog_cv_probe_native();
    }

    /* 结果只可能被写成同一个值，所以两个线程同时走到这里也无害：
     * GetProcAddress 在同一进程内结果相同，函数指针被写入的也是同一批值。
     * CAS 负责让最终值唯一，并保证函数指针先于后端标志可见。 */
    InterlockedCompareExchange(&g_cv_backend, (LONG)b, TCLOG_CV_UNKNOWN);
    return (tclog_cv_backend_t)g_cv_backend;
}

static TCLOG_INLINE tclog_cv_backend_t tclog_cv_backend(void)
{
    if (g_cv_backend == TCLOG_CV_UNKNOWN) {
        return tclog_cv_resolve();
    }
    return (tclog_cv_backend_t)g_cv_backend;
}

#  define TCLOG_CV_USING_NATIVE()  (tclog_cv_backend() == TCLOG_CV_NATIVE)

#else  /* !TCLOG_CV_NATIVE_CANDIDATE */

/* VC6 / 老 SDK 的 windows.h 里根本没有 CONDITION_VARIABLE，编译期就断掉：
 * 后端恒为自带实现，不需要探测、不需要原子、也不需要函数指针。 */
#  define tclog_cv_backend()       ((tclog_cv_backend_t)TCLOG_CV_EMU)
#  define TCLOG_CV_USING_NATIVE()  0

#endif /* TCLOG_CV_NATIVE_CANDIDATE */

/* ======================= [4] 对外接口 ======================= */
/* 每个入口都是"挑后端 + 转发"。原生分支只在候选编译时存在。 */

void tclog_cv_init(tclog_cv_t* cv)
{
#if TCLOG_CV_NATIVE_CANDIDATE
    if (TCLOG_CV_USING_NATIVE()) {
        g_nat_init(&cv->nat);
        return;
    }
#endif
    tclog_cv_emu_init(cv);
}

void tclog_cv_destroy(tclog_cv_t* cv)
{
#if TCLOG_CV_NATIVE_CANDIDATE
    if (TCLOG_CV_USING_NATIVE()) {
        return;                     /* 原生 CONDITION_VARIABLE 无需销毁 */
    }
#endif
    tclog_cv_emu_destroy(cv);
}

BOOL tclog_cv_wait(tclog_cv_t* cv, CRITICAL_SECTION* cs, DWORD ms)
{
#if TCLOG_CV_NATIVE_CANDIDATE
    if (TCLOG_CV_USING_NATIVE()) {
        return g_nat_wait(&cv->nat, cs, ms);
    }
#endif
    return tclog_cv_emu_wait(cv, cs, ms);
}

void tclog_cv_signal(tclog_cv_t* cv)
{
#if TCLOG_CV_NATIVE_CANDIDATE
    if (TCLOG_CV_USING_NATIVE()) {
        g_nat_signal(&cv->nat);
        return;
    }
#endif
    tclog_cv_emu_signal(cv);
}

void tclog_cv_broadcast(tclog_cv_t* cv)
{
#if TCLOG_CV_NATIVE_CANDIDATE
    if (TCLOG_CV_USING_NATIVE()) {
        g_nat_broadcast(&cv->nat);
        return;
    }
#endif
    tclog_cv_emu_broadcast(cv);
}

/* ======================= [5] 诊断 / 测试 ======================= */

const char* tclog_cv_backend_name(void)
{
    return (tclog_cv_backend() == TCLOG_CV_NATIVE) ? "native" : "emu";
}

int tclog_cv_force_emulation(int on)
{
#if !TCLOG_CV_NATIVE_CANDIDATE
    (void)on;                       /* 本来就是自带实现 */
    return 1;
#else
    if (g_cv_backend != TCLOG_CV_UNKNOWN) {
        return 0;                   /* 后端已定：不改，避免已创建的 cv 后端错配 */
    }
    g_cv_force_emu = on ? 1 : 0;
    return 1;
#endif
}

#endif /* _WIN32 || _WIN64 */
