/*
 * compat/tclog_win32.h — tclog 自带的 Windows 兼容层（接口）。
 *
 * 只提供 tclog 真正需要的两样东西：
 *   1) VC6 缺失的定长整型（int8_t / uint64_t …）；
 *   2) 条件变量 —— XP 没有 CONDITION_VARIABLE（Vista 才有），需要自带一份。
 *
 * 为什么不复用父项目的 portable/winapi.h：那份是 libuv 的整套 shim（133KB，还会
 * 拖进 WinSock2 / psapi / Iphlpapi），而 tclog 只用 4 个函数；而且它只有
 * GetProcAddress、没有回退，XP 上拿到 NULL 直接崩，本来就解决不了 XP。
 *
 * 命名刻意用 `compat/tclog_win32.h` 这种唯一名字：引号包含会先搜被包含文件所在
 * 目录；若 tclog 里放一个 `portable/winapi.h`，就会在父项目构建里把父项目自己
 * 那份顶掉。
 *
 * ── 条件变量：后端在运行期选，不是编译期 ────────────────────────────────
 * Vista+ 的原生条件变量明显更快（实测，i3-7100 / Win11，2 线程强制交接）：
 *     signal 无等待者    native 2.8ns    emu 6.7ns
 *     一次阻塞→唤醒      native 0.6µs    emu 9.7µs
 *     带超时等待立刻过期 native 3.4µs    emu 0.76µs
 * 端到端（async 日志、仅文件）用自带实现要慢 1.6~2 倍，所以能拿到原生就用原生。
 *
 * 但必须**动态解析**，不能直接调用：直接调用会在导入表里留下
 * kernel32!InitializeConditionVariable，XP 上进程连加载都会失败（"找不到入口
 * 点"），运行时判空根本轮不到。解析不到（XP / 2003）就退回自带实现。
 *
 * 自带实现 = 计数信号量 + 等待者计数。正确性依据：等待者先**在持有调用方 mutex
 * 的情况下**把 waiters 加一，然后才释放 mutex 去等信号量；而修改谓词同样要那把
 * mutex。于是"谓词已改、但等待者还没登记"的窗口不存在 —— 不会丢唤醒。这正是
 * pthread 条件变量的契约，thrqueue 完全符合。
 * signal 在没有等待者时只做一次读，不发系统调用。
 *
 * 已知取舍：带超时的等待若与 signal 相撞，可能多留一个信号量配额，让将来某次
 * 等待被立刻唤醒一次（虚假唤醒）。thrqueue 所有等待都在 while 谓词循环里，虚假
 * 唤醒只是重判一次谓词，无害。
 *
 * 实现见 compat/tclog_win32.c。
 */
#ifndef TCLOG_WIN32_H
#define TCLOG_WIN32_H

#if defined(_WIN32) || defined(_WIN64)

#include <windows.h>

/* ======================= 1. VC6 没有 <stdint.h> ======================= */
#if defined(_MSC_VER) && _MSC_VER < 1600
typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef short              int16_t;
typedef unsigned short     uint16_t;
typedef int                int32_t;
typedef unsigned int       uint32_t;
typedef __int64            int64_t;
typedef unsigned __int64   uint64_t;
#endif

/* ======================= 2. 条件变量 ======================= */

/* 原生后端是否可能可用：需要 SDK 头里声明了 CONDITION_VARIABLE，即 VS2005+ 且
 * _WIN32_WINNT >= 0x0600。VC6 / 老 SDK 这里就是 0，编译期直接断掉，连探测代码
 * 都不生成。 */
#if defined(_MSC_VER) && (_MSC_VER >= 1400) \
 && defined(_WIN32_WINNT) && (_WIN32_WINNT >= 0x0600)
#  define TCLOG_CV_NATIVE_CANDIDATE 1
#else
#  define TCLOG_CV_NATIVE_CANDIDATE 0
#endif

/* 自带实现的状态。 */
typedef struct {
    HANDLE       sema;
    volatile LONG waiters;   /* 只在持有调用方 mutex 时增减；signal 侧只做一次读 */
} tclog_cv_emu_t;

/* 两种后端的状态放进同一个 union：后端在运行期定下之后只用其中一个分支。
 * 这样 tclog_cv_t 在所有平台上是同一个类型，调用方（thrqueue_cs.c）不需要任何
 * 条件编译。 */
typedef union {
    tclog_cv_emu_t emu;
#if TCLOG_CV_NATIVE_CANDIDATE
    CONDITION_VARIABLE nat;
#endif
} tclog_cv_t;

/* 与 pthread_cond_* 一一对应。tclog_cv_wait 返回 TRUE = 被唤醒，FALSE = 超时。 */
void tclog_cv_init(tclog_cv_t* cv);
void tclog_cv_destroy(tclog_cv_t* cv);
BOOL tclog_cv_wait(tclog_cv_t* cv, CRITICAL_SECTION* cs, DWORD ms);
void tclog_cv_signal(tclog_cv_t* cv);
void tclog_cv_broadcast(tclog_cv_t* cv);

/* ======================= 3. 诊断 / 测试 ======================= */

/* 当前后端名："native" 或 "emu"。首次调用会触发后端解析。 */
const char* tclog_cv_backend_name(void);

/* 强制走自带实现，用于在开发机上覆盖 XP 那条代码路径。
 * 只在后端尚未解析时生效；已解析则返回 0 且不做任何改变（避免已创建的 cv 后端
 * 错配）。返回 1 表示已生效。
 * 另支持环境变量 TCLOG_CV_EMU（非空即强制），便于 ctest/bench 在不改代码的前提下
 * 把整套测试跑在自带实现上。 */
int tclog_cv_force_emulation(int on);

#endif /* _WIN32 || _WIN64 */
#endif /* TCLOG_WIN32_H */
