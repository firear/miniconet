# tclog

一个小巧的 C 语言日志库：**异步落盘（可选同步）+ 级别过滤 + 按大小轮转**，同时输出到
stdout 和文件。核心是一个多生产者/单消费者的环形队列，日志在调用线程里格式化好之后
只把节点指针交给队列，写盘由一个专属 writer 线程负责。

- 语言：C89（VC6 也能编过）
- 平台：**Windows（MSVC 6 及以上）与 Linux**；其它平台会 `#error`
- 依赖：无第三方库（Windows 上自带一份条件变量兼容层，XP 也能跑）

---

## 目录

- [特性](#特性)
- [日志格式](#日志格式)
- [快速开始](#快速开始)
- [API](#api)
- [同步与异步](#同步与异步)
- [构建](#构建)
- [Windows 构建脚本](#windows-构建脚本)
- [测试与基准](#测试与基准)
- [已知限制与注意事项](#已知限制与注意事项)
- [目录结构](#目录结构)
- [许可与来源](#许可与来源)

---

## 特性

| | |
|---|---|
| **异步写盘** | 生产者只做格式化 + 入队，不碰 I/O；writer 线程批量出队、合并成一次 `fwrite` |
| **同步直写** | `d_set_async(0)` 可切回同步路径（不经队列）；同步模式**不创建 writer 线程** |
| **级别过滤** | stdout 与文件两级独立阈值；未达级别的消息在格式化之前就返回 |
| **日志轮转** | 按大小轮转，保留 N 个历史文件（`xxx.log.01` … `xxx.log.NN`） |
| **零分配稳态** | 生产者的暂存节点走 TLS 循环复用，writer 预置一批空节点与队列交换，稳态下不 malloc |
| **无需初始化线程安全** | 多生产者安全；消费者唯一，writer 侧无需加锁遍历 |
| **VC6 可编** | 定长整型、条件变量、TLS 都有兼容层 |

---

## 日志格式

```
09-11 09:57:46.800 I/(4488):bench message 0 seq=0 tag=abcdefgh value=0
└─────┬──────┘ └┬┘└─┬─┘└┬─┘└──────────────┬──────────────┘
   时间戳      级别  tag  tid(十六进制)   消息正文（原样，含调用方给的格式）
```

| 片段 | 说明 |
|---|---|
| `MM-DD HH:MM:SS.mmm` | 本地时间，**无年份**；毫秒 3 位定长 |
| 级别字符 | `0`/`F`/`E`/`W`/`I`/`D`/`V`（见下） |
| tag | `LOG_TAG` 宏，默认空串 |
| tid | 线程 id，**十六进制**，不带 `0x` |
| 消息 | 紧跟 `):` 之后，无空格；换行由宏自动补，**不要在格式串里自己写 `\n`** |

同步模式与异步模式输出**逐字节一致**。

### 级别

宏名与级别值一一对应，"值越大越啰嗦"：

| 宏 | 值 | 字符 | 备注 |
|---|---|---|---|
| `DEBUG_NONE` | 0 | `0` | 只作为阈值使用，不用来打日志 |
| `LOGF` | 1 | `F` | 打完调用 `abort()` |
| `LOGE` | 2 | `E` | |
| `LOGW` | 3 | `W` | |
| `LOGI` | 4 | `I` | |
| `LOGD` | 5 | `D` | |
| `LOGV` | 6 | `V` | |

一条级别 L 的消息会被输出，当且仅当 `L <= max(level_stdout, level_file)`；
其中 stdout 与文件各自再判一次 `L <= 对应阈值`。所以常见配法是
"stdout 只到 INFO、文件到 VERBOSE"：

```c
d_init_debug("logs", 0, 10, DEBUG_INFO /*stdout*/, DEBUG_VERBOSE /*file*/, "app");
```

> 未调用 `d_init_debug` 之前（`level_cache == -1`）所有日志都会打到 **stderr**，
> 便于初始化阶段就能看到东西。

---

## 快速开始

### 构建

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

产物是静态库 `libtclog.a`（Windows 下 `tclog.lib`）。把本目录加入 include 路径、
链接该库即可，无需安装步骤。

### 最小示例

```c
#define LOG_TAG "app"          /* 必须在 include 之前定义；默认 "" */
#include "debug.h"

int main(void)
{
    /*       目录    单文件上限KB  保留个数  stdout级     文件级     文件名前缀 */
    LOGINIT("logs",   10 * 1024,   10,      DEBUG_INFO,  DEBUG_VERBOSE, "app");

    LOGI("started, port=%d", 8080);
    LOGW("queue is %d%% full", 90);
    LOGE("connect failed: %s", "timeout");
    LOGD("dump: %d bytes", 1024);

    LOGDESTROY();
    return 0;
}
```

`LOGINIT` 会在 `logs/` 下建 `app.log`，并写入一行 `====== Start logging at ... ======`；
`LOGDESTROY()` 写入结束行并关文件。**`d_init_debug` 可以重复调用**（会先销毁上一次）。

### 打印二进制块

```c
LOGBIN(buf, len);   /* 十六进制逐字节打印，受 DEBUG_DEBUG 级别门控 */
```

---

## API

头文件：`debug.h`（对外接口）、`thrqueue.h`（队列，一般不需要直接用）。

### 初始化 / 销毁

```c
void d_init_debug(const char* logdir,      /* 日志目录；NULL = 当前目录。无需以 / 结尾 */
                  size_t logfilesize,      /* 单个文件上限，单位 **KB**；0 = 不轮转 */
                  size_t maxlogfilenumber, /* 保留的历史文件个数；0 = 用默认值 10 */
                  int level,               /* stdout 阈值 */
                  int log,                 /* 文件阈值；<= DEBUG_NONE 则**完全不建文件** */
                  const char* binname);    /* 文件名前缀 -> <logdir>/<binname>.log */

void d_destory_debug(void);                /* 注意原拼写就是 destory */
```

### 运行期开关

```c
int  d_set_debug_level(int level);   /* 改 stdout 阈值 */
int  d_set_log_level(int level);     /* 改文件阈值 */
int  d_set_async(int on);            /* 切同步/异步，见下一节；失败返回 -1 */
void d_log_flush(void);              /* fflush(g_logger.fp) */
int  d_log_enabled(int level);       /* 该级别当前是否会被输出（用于省掉昂贵的参数构造） */
```

### 日志宏

```c
LOGINIT(dir,size,count,stdout_lvl,file_lvl,name)
LOGDESTROY()
LOGF / LOGE / LOGW / LOGI / LOGD / LOGV   /* printf 风格 */
LOGBIN(data, len)
STOPHERE                                   /* LOGD("STOP HERE") + 死循环 */
```

`d_log_enabled()` 适合这种场景：

```c
if (d_log_enabled(DEBUG_DEBUG)) {
    LOGD("payload=%s", build_expensive_string());
}
```

### 编译期总开关

```c
#define DEBUG_SHOW_DEBUG 0     /* include 之前定义：所有 LOGx 变成空语句，整库零开销 */
```

### Android

定义 `__ANDROID__` 时，`LOGx` 宏会转发到 `__android_log_print`（`LOGF` 之后仍 `abort()`），
本库自己的实现不参与编译。此路径下 `LOGINIT`/`LOGDESTROY` 是空宏。

---

## 同步与异步

```c
d_set_async(async);                 /* 必须在 d_init_debug 之前调用才能免建线程 */
d_init_debug(...);
```

| | 异步（默认） | 同步 |
|---|---|---|
| 写盘线程 | 有（1 个 writer） | **无** |
| 生产者开销 | 格式化 + 入队（一次锁） | 格式化 + `fwrite`（stdio 自带锁） |
| 消费者 | writer 批量出队 + 合并 `fwrite` | 无 |
| 顺序 | 按入队顺序（全局有序） | 线程间按到达 stdio 的顺序 |
| 轮转 | ✅ 生效 | ❌ **不生效**（见限制） |
| 多生产者 | 行完整 | 行完整（整行一次 `fwrite`） |
| 丢日志 | 队列满则生产者阻塞 | 不丢 |

### 三种调用顺序的差别

```c
/* A. 要同步且不想白建线程 —— 推荐 */
d_set_async(0);
d_init_debug(...);

/* B. 运行期开异步，用完了关掉（线程会被真正回收，队列也会被回收） */
d_init_debug(...);
d_set_async(1);
...
d_set_async(0);

/* C. 什么都不调 —— 默认异步 */
d_init_debug(...);
```

`d_set_async(0)` 在 `d_init_debug` **之后**调用时，writer 线程会先被建出来再立刻停掉；
想在同步模式下完全不创建线程，用 A 的顺序。

> ⚠️ `d_set_async()` 会停线程、回收队列，**不要在其它线程还在打日志时调用**。

---

## 构建

### 选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `RelWithDebInfo` | 未指定时由 `CMakeLists.txt` 兜底 |
| `TCLOG_PERF` | `OFF` | 分阶段耗时插桩（见 `tclog_perf.h`）。**需要 TLS**，VC6 不可用（有 `#error`） |
| `TCLOG_ENABLE_SANITIZERS` | `OFF` | ASan + UBSan，仅 GCC/Clang |
| `TCLOG_BUILD_BENCH` | `ON` | 微基准（不注册进 CTest） |
| `TCLOG_BUILD_TESTS` | `ON` | 单元测试并注册 CTest |

只编库（老编译器/嵌入式用法）：

```sh
cmake -S . -B build -DTCLOG_BUILD_BENCH=OFF -DTCLOG_BUILD_TESTS=OFF
```

### 条件变量后端（Windows）

Windows 侧 `thrqueue` 需要条件变量，而 **XP 没有 `CONDITION_VARIABLE`（Vista 才有）**。
`compat/tclog_win32.c` 在**运行期**决定用哪个后端：

| 条件 | 后端 |
|---|---|
| VS2005+ 且 `_WIN32_WINNT >= 0x0600`，且能在 `kernel32.dll` 里解析到 4 个原生函数 | 原生 `CONDITION_VARIABLE` |
| 解析不到（XP/2003） | 自带的"计数信号量 + 等待者计数"实现 |
| VC6 / 老 SDK（头文件里根本没有 `CONDITION_VARIABLE`） | 编译期就只剩自带实现 |

- 原生函数**必须**通过 `GetProcAddress` 动态取址，不能直接调用 —— 直接调用会在导入表
  里留下 `kernel32!InitializeConditionVariable`，XP 上进程**连加载都会失败**。已用
  `dumpbin /imports` 核对过：导入表里只有 `KERNEL32.dll` 和 CRT，无任何条件变量符号。
- 诊断：`tclog_cv_backend_name()` 返回 `"native"` 或 `"emu"`；bench 会在首行打印 `cv=`。
- 强制走自带实现（用于覆盖 XP 那条路径）：环境变量 `TCLOG_CV_EMU=1`，或在任何队列
  创建之前调用 `tclog_cv_force_emulation(1)`。
- ctest 会把每个测试注册两遍（`test_x` 与 `test_x_emu`），**XP 路径每次都被真的执行到**。

### 与父项目集成（子模块用法）

- 本仓库刻意**不**放 `portable/` 之类的目录：引号形式 `#include "portable/xxx.h"` 会优先
  搜被包含文件所在目录，从而遮蔽父项目自己的同名头文件。兼容层统一放在 `compat/` 下并
  使用唯一名字（`compat/tclog_win32.h`）。
- 独立构建时 `CMakeLists.txt` 不会引入任何会与父项目冲突的 include 路径。

---

## Windows 构建脚本

两个脚本都在仓库根目录，用法一致：`脚本 [cfg|clean]`。

| 脚本 | 工具链 | 产物 |
|---|---|---|
| `win_build_vs2010.bat` | VS2010 x64 + Ninja | **amd64** |
| `win_build_vc6.bat` | VC6 (x86) + Ninja `--parallel 1`（NMake 兜底） | **x86** |

```bat
win_build_vs2010.bat cfg     :: 只重新 configure
win_build_vs2010.bat         :: 编译（日志在 build-vs2010.log）
win_build_vs2010.bat clean   :: 删构建目录
```

### 为什么 VC6 不能生成 `.dsw`/`.dsp`

CMake **3.3 起弃用、3.6 起移除了 `Visual Studio 6` 生成器**，所以 VC6 只能走
Ninja / NMake 这类命令行生成器。

### 为什么 VC6 的 Ninja 要加 `--parallel 1`

VC6 的 `cl.exe` **不能让多个进程共用一个 `/Fd` PDB**（VS2010+ 有 `/FS` 才行）。
Ninja 默认并行，于是并发的第二个 `cl` 立刻报：

```
fatal error C1033: cannot open program database '...\tclog.pdb'
```

串行（`-j1`）就没问题。NMake 本来就是串行，所以脚本在 Ninja 不可用/失败时回退 NMake。

---

## 测试与基准

### 测试

```sh
ctest --test-dir build --output-on-failure
```

- `test_thrqueue` —— 队列的阻塞/非阻塞、批量出队、多生产者单消费者、`unblock_*`
- `test_debug` —— 初始化、级别过滤、落盘内容
- Windows 上每个测试还会以 `TCLOG_CV_EMU=1` 再跑一遍（覆盖 XP 的条件变量路径）

### 基准

```sh
build/bench/bench_debug N THREADS ASYNC STDOUT_LEVEL FILE_LEVEL [LOGDIR]
```

| 参数 | 说明 |
|---|---|
| `N` | 每线程日志条数 |
| `THREADS` | 生产者线程数 |
| `ASYNC` | `1` 异步 / `0` 同步 |
| `STDOUT_LEVEL` | `0` = 关闭 stdout（**测量时必须关，或把 stderr 重定向到 `/dev/null`**） |
| `FILE_LEVEL` | `0` = 不建文件；`4` = `DEBUG_INFO` |
| `LOGDIR` | 日志目录，省略则写当前目录 |

输出 `ns_per_msg(wall)` / `ns_per_msg(cpu)` / `msg_per_sec`，首行会打印本次用的
条件变量后端（`cv=native` / `cv=emu`）。加 `-DTCLOG_PERF=ON` 会额外打印分阶段耗时表。

不加 `ASYNC` 参数的细节：`N THREADS ASYNC` 之外还有 stdout/file 两个级别参数；
两个都设 0 时 `level_cache` 为 0，**所有日志会被直接丢弃**，等于只测循环开销。

> 详细的性能剖析过程、数据与结论见 [`plan.md`](plan.md)。

几条比较稳的定性结论（细节见 `plan.md`）：

- 优化后的主要固定成本是**格式化**，不是锁；秒级时间戳缓存把 `localtime_r` + 时间戳
  `snprintf` 从 ~620 ns 降到 ~70 ns 量级（Linux）。
- 队列的核心改进是**批量出队 + 空节点交换**：锁周期从"每条一次"降到"每批一次"，
  且稳态零分配。之前的多线程负扩展（线程越多吞吐越低）已消除。
- **同步路径在单/双线程下通常比异步更快**，异步的价值在 I/O 慢、或需要保证多生产者
  下行完整与文件有序时；`plan.md` 里有具体的吞吐对比。
- Windows 上自带条件变量实现比原生慢不少（实测端到端约 1.3~1.7 倍），但那只影响
  **XP 等没有原生条件变量的系统**；Vista+ 走原生后端。

---

## 已知限制与注意事项

1. **异步路径的单条日志有 256 字节上限**。VS2005+ 的异步实现使用固定 256 字节的 TLS
   节点（`TCLOG_NODE_BYTES`），正文超长会**就地静默截断**。同步路径不截断；
   VC6 的异步路径会扩容，也不截断。
2. **日志轮转只在异步模式下生效**。轮转与字节计数发生在 `writetofile()` 里，而它只被
   writer 线程调用；同步路径直接写 `fp`，不累计字节。要轮转就用异步。
3. **轮转是"每批检查一次"**，所以文件可能超出 `logfilesize` 约一个批的量
   （≤ `TCLOG_WRITER_BATCH × TCLOG_NODE_BYTES`）。
4. **每条 `fflush` 打不开**：`g_logger.flush` 没有 setter，恒为 0。需要立即落盘用
   `d_log_flush()`。
5. **`d_destory_debug()` 会往 stderr 打印一行 `destory debug`**（原有行为）。
6. **多线程 + 同步模式的安全前提**：整行会被一次 `fwrite` 写出（stdio 单次调用内部有锁），
   所以行不会被拆开；但**同一进程不要同时用同步模式做轮转/关文件**（异步下 writer 是
   唯一写者，没有这个问题）。
7. **平台限定**：只支持 Windows 与 Linux。macOS 等其它平台在 `debug.c` 里会 `#error`。
8. **VC6 上 `LOG_TAG` 不生效**：`_MSC_VER < 1400` 走的是 `LOGI()` 这类普通函数（而非
   宏），tag 固定为空串。
9. **`__declspec(thread)` 与动态加载的 DLL**：VC6/老编译器下，若把本库编进用
   `LoadLibrary` 动态加载的 DLL，`__declspec(thread)` 会退化成普通全局变量 ——
   生产者的 TLS 暂存节点被多线程共用会造成内存破坏。这种配置必须改用
   `TlsAlloc`/`TlsGetValue`（代码注释里已标明）。
10. **Windows 源文件是 UTF-8 无 BOM，MSVC 按 CP936 读**，所以源码里的中文注释会刷
    `C4819` 告警（无害）。改代码时注意：**连续奇数个汉字时，最后一个汉字的尾部字节会
    吃掉紧随其后的一个 ASCII 字节** —— 例如注释写成 `。）*/`（没有空格）会让 `*/`
    失效、整段代码被注释掉；字符串字面量里同样会让闭合引号被吃掉。写入中文时在
    结尾留一个空格（`。 */`）即可避开。

---

## 目录结构

```
debug.h            对外 API（宏 + 函数声明）
debug.c            实现：级别过滤、格式化、时间戳、writer 线程、轮转
thrqueue.h         队列接口
thrqueue_cs.c      队列实现（pthread / Win32 双后端）
tclog_perf.h       分阶段耗时插桩（TCLOG_PERF 门控，默认展开为空）
compat/tclog_win32.h / .c   Windows 兼容层：VC6 定长整型 + 条件变量（原生/自带后端）
tests/             test_thrqueue.c / test_debug.c（注册进 CTest）
bench/bench_debug.c 微基准（不注册进 CTest）
plan.md            性能剖析的过程、数据与结论
win_build_vs2010.bat / win_build_vc6.bat  Windows 构建脚本
```

---

## 许可与来源

- `thrqueue.h`、`thrqueue_cs.c` 头部保留了 **SSLsplit**（Copyright © 2009-2014,
  Daniel Roethlisberger）的 **BSD 2-clause** 许可声明；这两个文件源自该项目。
  其中的批量出队 / 空节点交换（`thrqueue_dequeue_swap`）等为本项目后续增补。
- 其余文件未附显式许可声明，请按所在父项目的许可处理。
