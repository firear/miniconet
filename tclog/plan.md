# tclog 日志路径性能剖析 — 计划与结果

范围：只剖析 `__d_debug()` 一次成功输出日志的耗时构成，目标四项
`localtime_r` / 格式化 / 队列锁 / flush+write。

**状态：已测量完成（2026-09-10）。** 结论见文末"结论"。

---

## 第 0 步：建立可重复基准

- [x] `bench/bench_debug.c` + `bench/CMakeLists.txt`：**不注册进 CTest**（耗时、
      异步 writer 使输出非确定性）。用法
      `bench_debug N THREADS ASYNC STDOUT_LEVEL FILE_LEVEL [LOGDIR]`
- [x] 参数化：N / 线程数 / async / stdout 开关 / file 开关 / logdir
- [x] 编译要求：`-DCMAKE_BUILD_TYPE=RelWithDebInfo -DTCLOG_ENABLE_SANITIZERS=OFF`
- [x] 空转基线：stdout 与 file 全设 `DEBUG_NONE` 时 `level_cache=0`，
      `d_log_enabled()` 直接返回 0 → 实测 **3.4 ns/条**，循环开销可忽略
- [x] 计时：`CLOCK_MONOTONIC`（wall）+ `CLOCK_PROCESS_CPUTIME_ID`（CPU）。
      两者必须分开看——本沙箱只有 2 物理核且常驻 IDE，wall 里混有抢占

## 第 1 步：先量整体

- [x] 单线程 / 多线程 × async on/off 各跑一遍

## 第 2 步：四项逐一定位

`perf` 装好了但**本环境只够做 `perf stat`**（`raw_syscalls` tracepoint 报
"No permissions"，paranoid=1 不够）。故分阶段数据主要靠**编译期门控插桩**：
新增 `tclog_perf.h`，`TCLOG_PERF` 未定义时展开为空，默认构建零代码零符号。

- [x] ① `localtime_r`：插桩 `strtime()` 内三点（gettimeofday / localtime_r / 时间戳 snprintf）
- [x] ② 格式化：插桩前缀 + 正文（同步路径则整体计入 WRITE）
- [x] ③ 队列锁：`thrqueue_enq_getstub` 内把**等 mutex**、**等 notfull（背压）**、
      **临界区**三者分开
- [x] ④ flush/write：`writetofile` + writer 线程的 stderr 写出分别插桩；
      writer 无退出时机导出 TLS，故用全局 + `__sync_fetch_and_add`

辅助工具分工：
- [x] `strace -c -f`：futex / write / gettid **次数**（硬件无关，最可信）
- [x] `perf stat`：CPU 时间、上下文切换、IPC
- [x] `ltrace -c`：**只取调用计数**。它的计时在 ptrace 下不可信（四个函数都报
      342 µs/call，是总额均摊），不能用

## 第 3 步：工具分层

1. 微基准 + `clock_gettime` 夹点（主力）
2. `perf stat`（CPU / IPC / 上下文切换）
3. `strace -c -f`（syscall 计数）
4. `ltrace -c`（只用于调用计数）
5. `perf record/report`、`valgrind`：本轮未用（前者 tracepoint 受限，后者太慢）

## 第 4 步：方法论红线

- [x] stdout/stderr 一律重定向 `/dev/null`
- [x] warm up 20000 条（消化 `localtime_r` 时区缓存、stdio 首次分配、缺页）
- [x] `CLOCK_MONOTONIC`；`clock_gettime` 走 vDSO（strace 里 0 次，不扰动）
- [x] 异步模式：producer 与 writer **不能绑同一个核**（首次 `taskset -c 1` 就把
      两者压到一核，wall 被调度污染，已改绑 2 核）
- [x] 多线程：本沙箱 4 逻辑核 = **2 物理核**，4 线程以上必然超订

## 环境标定（后续所有数字的判读前提）

```
getpid : 719.8 ns/syscall
gettid : 713.5 ns/syscall
```

裸金属同一数字约 60-70 ns → **本沙箱 syscall 开销约 10 倍**。因此：

- **纯用户态项**（`localtime_r`、`snprintf`、`vsnprintf`）不受膨胀影响，可直接采信
- **syscall 项**（futex、gettid）在本沙箱被放大 10 倍，**必须折算后再下结论**

`perf stat`（单线程异步，10 万条）：0.82 IPC，12611 次上下文切换（0.126 次/条）。

---

## 测量结果

### 主配置：1 线程 / 异步 / 仅文件（3 次中位数，绑 2 核）

```
ns_per_msg(wall)   2923        ns_per_msg(cpu)  4315   (producer+writer 合计)
TS(all)             740   = gettimeofday ~50 + localtime_r 303 + 时间戳 snprintf 318
FMT(prefix+body)    342
Q:lock               67   等 mutex
Q:notfull            26   等队列不满（背压）
Q:hold             1274   临界区（含上面的 FMT 342 → 非格式化部分 ~932）
producer TOTAL     2142
writer WRITE       ~290   （并行核上，不计入 producer 延迟）
```

`_gettid()` 在宏里求值、在 `__d_debug` **之前**，所以不在 TOTAL 内。

### strace 取证（7 万条消息）

| | 异步 | 同步 |
|---|---|---|
| futex | **118937（1.70 次/条）**，其中 23873 errors | **1 次（全程）** |
| gettid | **70000（1.00 次/条）** | 70000（1.00 次/条） |
| write | 1437（1 次 / 48.7 条，stdio 缓冲生效） | 1437 |

`ltrace` 计数交叉验证：每条消息恰好 1× `gettimeofday` + 1× `localtime_r` +
1× `snprintf` + 2× fprintf 族，无隐藏额外调用。

### 同步 vs 异步（1 线程，仅文件）

| | wall ns/条 | producer TOTAL | 吞吐 |
|---|---|---|---|
| 同步 | 1919 | 1148（TS 593 + WRITE 479） | **521 k/s** |
| 异步 | 2923 | 2142 | 293 k/s |

同步路径 futex 全程 1 次；异步 1.70 次/条。

### 线程扩展（异步，仅文件，4 逻辑 / 2 物理核）

| 线程 | wall ns/条 | cpu ns/条 | Q:lock | Q:notfull | 聚合吞吐 |
|---|---|---|---|---|---|
| 1 | 3409 | 5084 | 67 | 26 | **293 k/s** |
| 2 | 4345 | 8394 | 1055 | 1245 | 230 k/s |
| 4 | 9559 | 16243 | 3491 | **25436** | 105 k/s |

**聚合吞吐随线程数下降。** 4 线程时 71% 的时间耗在 `Q:notfull`（队列满、等空间）。
注意：2 物理核下 4 线程已 2× 超订，该结论有超订成分，但负扩展趋势明确。

---

## 交付表：每消息成本归属（1 线程异步，分母 = caller 承担的 2923 ns）

### 本沙箱实测

| 组件 | ns/条 | 占比 | 说明 |
|---|---|---|---|
| **`_gettid`（未缓存）** | 719 | **24.6%** | 1 次 syscall；本应缓存掉 |
| **队列（锁 + 背压 + 临界区尾）** | 1025 | **35.1%** | 其中 ~932 是 futex syscall |
| 格式化（时间戳 snprintf + 前缀正文） | 660 | 22.6% | 纯用户态 |
| `localtime_r` | 303 | 10.4% | 纯用户态 |
| 其余（gettimeofday + 循环/调度） | 216 | 7.4% | |
| flush/write | ~0（fflush 未开） | — | write 被 stdio 摊薄到 1/48.7 条 |

### 折算到裸金属（syscall ÷ 10）

| 组件 | ns/条 | 占比 |
|---|---|---|
| 格式化 | 660 | **49.4%** |
| `localtime_r` | 303 | **22.7%** |
| 队列 | ~152 | 11.4% |
| `_gettid` | 72 | 5.4% |
| 其余 | ~150 | 11.2% |

**同一个程序，两套结论**——沙箱里"队列锁"看着是第一（35%），那纯粹是 10 倍
syscall 膨胀造成的假象；裸金属上真正的大头是**格式化 + `localtime_r`（合计 72%）**。

---

## 结论

1. **`localtime_r`（303 ns）+ 格式化（660 ns）是真大头**，裸金属占 72%。
   最有价值的优化是**缓存秒级时间戳**：同一秒内复用上次格式化好的时间字符串，
   一次干掉 `localtime_r` 303 + 时间戳 snprintf 318 = **621 ns（裸金属 46%）**。
2. **`_gettid` 没有缓存**。`debug.h:115` 仍是每条调用一次，实测占 caller 成本
   **24.6%（沙箱）/ 5.4%（裸金属）**。这是纯浪费——tid 每线程恒定，线程内缓存
   即可归零。（我上一轮判断"不值得缓存"是错的：我低估了它在总成本中的占比。）
   → **已修复**（`debug.c` 用 `static __thread` 惰性缓存，tid 永不为 0 作哨兵）。
3. **队列的核心问题不是"锁慢"，是结构**：
   - 格式化（342 ns）**整个发生在队列互斥锁内**，把锁持有时间拉长 28%；
   - 每条消息 ~1.7 次 futex（unlock + `cond_broadcast` + writer 侧 lock/unlock/signal）；
   - `enq_done` 用 `cond_broadcast` 唤醒**所有**等待者 → 多线程惊群；
   - writer 一次只取一条 → 每条消息都要完整走一遍锁周期。
   方向是**批量出队**（一次锁取空队列）+ 把格式化移出临界区 + broadcast 改 signal。
4. **flush/write 不是瓶颈，不要动**。write 已被 stdio 缓冲摊薄到每 48.7 条一次
   系统调用；`g_logger.flush` 默认 0 且无 setter，`fflush` 根本不会被调用。
5. **异步比同步贵**：同步 521 k/s vs 异步 293 k/s，且同步 futex 全程 1 次。
   异步只在 I/O 真的慢时才划算；当前 buffered 文件写入下是净亏损。
6. **多线程负扩展**：293 → 230 → 105 k/s（1/2/4 线程）。瓶颈是单一全局队列 +
   单一 writer。

## 实施过程中的修正

- `writetofile` 的插桩第一次批量编辑丢失了（同文件并行编辑偶发），已补；
  此后同文件编辑改为串行。
- bench 的 producer 表曾误读未初始化的 `wcnt` 做跳过判断，导致 WRITE 行时有时无，
  已改为按 `pcnt` 判断。
- 首次测量用 `taskset -c 1` 把 producer 和 writer 压在同一核上，已改绑 2 核。
- 新增 `d_set_async(int)`：原先 `async` 只在 `thrqueue_new` 失败时才为 0，
  **同步分支是不可达代码**，没有 setter 就无法对比两条路径。

## 回归

默认构建（`TCLOG_PERF=OFF`）：ctest **2/2 通过**，插桩代码不参与编译。

---

# 本轮（2026-09-10）：秒级时间戳缓存（已实施）

## 实现

`strtime()` 按 `tv_sec` 缓存"日期 + 时分秒"前缀，毫秒手写拼在后面。

- **正确性依据**：日期+时分秒是 `tv_sec` 的**纯函数**，所以按秒缓存是严格
  正确的——时钟回拨也不受影响（回拨到同一秒本就该得到同样的串）。毫秒每次
  实时计算，精度不变。
- 缓存放 **TLS**（`static __thread`，与 `_gettid` 缓存同一路子）：无需同步、
  无伪共享，代价只是每个线程每秒多付一次 `localtime_r`。
- 命中路径上**不再调用 `snprintf`**：毫秒用三次除法手写（`.` + 3 位数字）。
  否则省下的 `localtime_r` 会被一次新的 `snprintf` 吃掉一半。
- **只在 Unix 分支做**：Windows 的 `GetLocalTime` 本来就便宜，且拿不到便宜的
  epoch 秒做 key，不值得为它引入可移植性风险。
- 补了 `bufsz == 0` 早退：新代码要算 `bufsz - 1`，原先靠 `snprintf(..,0,..)`
  不写东西兜住，现在必须显式挡掉，否则下溢。
- 已知取舍：`TZ`/DST 运行时变更后，最多有 1 秒的时间戳用旧时区。

## 效果

A/B 用临时宏 `TCLOG_NO_TS_CACHE` 切换（测完已撤，代码里已无该宏）。
配置：异步 / 仅文件 / N=50000 / `taskset -c 0-3`，各 3 轮中位数，两侧都开 tid 缓存。

| | 关时间戳缓存 | 开 | |
|---|---|---|---|
| T=1 wall | 1978 ns/条 | **1224 ns/条** | **−38%** |
| T=2 wall | 3260 ns/条 | 6834 ns/条 | **+110%（回归）** |

单线程明细（异步，N=100000，绑 2 核）：

| 阶段 | 优化前 | 优化后 |
|---|---|---|
| `ns_per_msg(wall)` | 2923 | **1276** |
| `ns_per_msg(cpu)` | 4315 | **2191** |
| TS(all) | 740 | **71** |
| ├ `localtime_r` | 303 | **0.1** |
| └ 时间戳 snprintf | 318 | 35 |
| `__d_debug` TOTAL | 2142 | **1242** |

（"优化前"那组同时也没有 tid 缓存，故 wall 差额里含 `_gettid` 的 719 ns。）

同步路径（无队列）：1919 → **552 ns/条（−71%）**，即 1.81 M 条/秒。

## 正确性验证

50 万条日志落盘后逐行校验：

- 520000 行数据行，时间戳格式 `MM-DD HH:MM:SS.mmm` **全部匹配**
- **时间零倒退**（单调），跨越 2 个秒边界时正确切换
- 单秒内出现 **684 个不同毫秒值** → 毫秒没被冻住，仍逐条实时计算

（日志首尾的 `====== Start/End logging ======` 及由此产生的空行是
`d_destory_debug` 原有输出，与本次改动无关。）

## ⚠️ 多线程回归（必须跟进）

开启缓存后 **T≥2 的聚合吞吐反而腰斩**：

| 线程 | 优化前 | 优化后 |
|---|---|---|
| 1 | 293 k/s | **817 k/s** |
| 2 | 230 k/s | **146 k/s** |
| 4 | 105 k/s | **40 k/s** |

机制（有数据支撑）：生产者变快后 **1024 槽队列被永久打满**，`Q:notfull` 从
~1.0-1.6 µs/条 涨到 **9.3-12.6 µs/条**，生产者 90%+ 时间在等空间；同时更热的
互斥锁让 **writer 自己的取锁也更频繁地落进 futex**，writer 吞吐随之下降。
即"生产者变快 → 锁更热 → writer 更慢 → 总吞吐更低"的正反馈。

旁证：**同步路径（无队列）T=2 是 630 k/s，同期异步 T=2 只有 146 k/s（4.3 倍）**
——瓶颈确实在队列，不在时间戳。

这不是时间戳缓存本身的错，而是它把早就存在的队列设计问题**暴露**了出来。

## 下一步

**批量出队**（writer 一次持锁取空队列）+ `enq_done` 的 `broadcast` 改
`signal`（`notempty` 上只有 writer 一个等待者，broadcast 纯属浪费）+
把格式化移出临界区。预计能同时收掉上面的多线程回归。

---

# 本轮（2026-09-10）：队列优化（已实施）

## 三项改动

1. **writer 批量出队**（`thrqueue_dequeue_swap`）：一次持锁取一批，锁外再写。
   原来每条消息都要 lock+unlock+signal 一轮。
2. **格式化移出临界区**：日志先在 `static __thread` 暂存节点里格式化好，
   进队列时只做指针写回。原来 ~370 ns 的格式化发生在队列互斥锁内，等于把
   所有生产者的格式化用同一把锁串行化。
3. `thrqueue_enq_done` 的 `broadcast` → `signal`：`notempty` 上只有一个
   等待者（唯一的 writer），broadcast 只是把这一个唤醒。
   **前提：单消费者。若将来出现第二个消费者必须改回 broadcast。**
   唤醒生产者侧改为逐个 `signal` 而非 `broadcast`——glibc 在无等待者时
   signal 是用户态空操作，代价是 `min(k, 等待者数)` 次 futex，且不惊群。

## ⚠️ 实现中发现并修掉的一个数据竞争

批量出队直接复用原来的"槽位节点复用"会**引入数据竞争**，而且比原来严重得多：

- 原实现：writer 一次只取一个节点，slot 一出队就可被生产者写回，而 writer
  此时可能还在读它 —— 竞争窗口约等于一次写（~300 ns）。**这个窗口原本就存在。**
- 批量出队后：writer 一次持有 256 个节点，写第 256 个要 ~77 µs；而它的 slot
  在出队瞬间就可被生产者写回并当作暂存缓冲覆写。**必现竞争。**

试过两种修法：

| 方案 | T=1 | 结论 |
|---|---|---|
| 每消息 `malloc` + writer `free`（所有权单一，简单正确） | 1428 ns/条 | 正确但每消息多付 ~430 ns（跨线程 malloc/free），放弃 |
| **空节点交换**（采用） | ~1120 ns/条 | 正确且零分配 |

**空节点交换**：`thrqueue_dequeue_swap(buf, max)` 用调用方提供的空节点换出
队列里的满节点，并在腾出的槽位留下空节点。于是：

- 空闲槽位里的节点**不可能**正被 writer 持有 → 生产者复用它安全；
- 节点所有权始终单一（生产者 TLS / 队列 / writer 三者之一）；
- 稳态下全程不 malloc、不 free（writer 预置 256 个空节点循环使用）。

## 效果（N=50000，`taskset -c 0-3`，3 轮中位数）

| 配置 | 优化前 | 时间戳缓存后 | +队列优化 | 相对最初 |
|---|---|---|---|---|
| async T=1 | 3409 ns/条 | 1224 | **1120** | **3.0x**（293→893 k/s） |
| async T=2 | 4345 | 6834（回归） | **851** | **5.1x**（230→1175 k/s） |
| async T=4 | 9559 | 25051（回归） | **974** | **9.8x**（105→1027 k/s） |
| sync T=1 | 1919 | 552 | **660** | 2.9x（523→1515 k/s） |

**多线程负扩展已消除**：聚合吞吐从"293 → 230 → 105 k/s 单调崩塌"
变成"893 → 1175 → 1027 k/s"（T=4 回落是 2 物理核的核数限制，不再是队列瓶颈）。

`Q:notfull` 从 9.3-12.6 µs/条 塌到 **~0.2-0.9 µs/条**——队列不再被打满。

注：本机噪声很大（同一配置 1019-1727 都能出现），所以只报中位数，
单看某一次容易误判。

## 正确性验证

- **内容自洽校验**：4 线程 × 5 万 = 22 万条日志落盘，逐行检查每条的
  `value == seq*7` → **0 处不自洽、0 处字段错位**（若节点被覆写必然暴露）
- **行数精确匹配**：220000 行，与期望完全一致，无丢消息
- **ASan+UBSan** 4 线程：无错误。仅有预期内的泄漏——4×280 字节 =
  4 个生产者线程的 `t_logbuf` 节点（线程退出不回收，与现有
  `t_cached_tid`/时间戳缓存的 TLS 用法一致）
- ctest（默认构建）**2/2 通过**

**未做到的**：`perf` 的 tracepoint 无权限；**TSan 在本沙箱不可用**
（`FATAL: ThreadSanitizer: unexpected memory mapping`，容器内存布局限制），
所以无竞争是靠"所有权单一"的推理 + 上面的自洽校验/ASan 佐证，
不是靠 TSan 证明。有条件时应在真机上补跑一次 TSan。

## 遗留

- 每线程一个暂存节点不回收（280 B/线程，有界）。需要的话可用
  `pthread_key_create` + 析构函数回收，但会引入平台分支，暂不做。
- `d_destory_debug()` 仍不回收队列节点和 writer 的 256 个空节点
  （原有 `#if 0` 里那段本来就没启用）。
- `thrqueue_dequeue()` 现在无人调用，作为 API 保留。

---

# Windows 平台现状

**一句话：我这几轮改动里，有两处会直接打断 MSVC 编译，已修复；但 Windows 侧
全程没有实测（本环境无 MSVC，独立 CMake 在 WIN32 直接 `FATAL_ERROR`，
Windows 只能走父项目构建）。下面按"已核实"与"未验证"分开说。**

## 已发现并修复的两个 MSVC 阻断

1. **`static __thread LogBuf_s* t_logbuf` 在文件作用域且无平台保护**
   → MSVC 不认 `__thread`，编译直接失败。
   已引入 `TCLOG_TLS` 宏：`_MSC_VER` → `__declspec(thread)`，
   `__GNUC__`/`__clang__` → `__thread`（clang-cl 同时定义两者，故先判
   `_MSC_VER`），**其它编译器 `#error`**——宁可编译失败，也不静默退化成
   普通全局变量（那会让多线程共用一个暂存节点，是静默的内存破坏）。
2. **`strtime()` 里 `if (bufsz == 0) return buf;` 排在 `SYSTEMTIME wtm;` 之前**
   → MSVC 的 C 编译器不接受"语句后再声明"（C89 规则）。
   已把 `SYSTEMTIME wtm;` 提到函数体顶部（仍在 Windows 条件编译内）。

## 各改动在 Windows 上的覆盖情况

| 改动 | Windows 是否生效 | 说明 |
|---|---|---|
| tid 缓存 | 不适用 | 该缓存在 `__linux` 分支内；Windows 的 `GetCurrentThreadId()` 读 TEB，本就便宜 |
| 秒级时间戳缓存 | **否** | 只在 Unix 分支做。Windows 仍是 `GetLocalTime` + snprintf，**输出格式与 Unix 完全一致**，只是没拿到那 ~620 ns 的收益 |
| 批量出队 + 空节点交换 | **是** | 走 pthread shim：`CRITICAL_SECTION` / `CONDITION_VARIABLE`；用到的 lock/unlock/wait/signal 都有 shim |
| 格式化移出临界区 | **是** | `t_logbuf` 走 `TCLOG_TLS` |
| `broadcast` → `signal` | **是** | `pthread_cond_signal` → `pWakeConditionVariable` |
| `tclog_perf.h` 插桩 | 否 | `TCLOG_PERF` 未定义时展开为空，不产生代码 |

## 已知风险

- **`__declspec(thread)` 在 VC6 + `LoadLibrary` 动态加载的 DLL 里不生效**，
  会退化成普通全局变量。tid/时间戳缓存退化只是多算，但 `t_logbuf` 退化会
  造成多线程串用同一节点。若父项目属于这种配置，必须改用
  `TlsAlloc`/`TlsGetValue`（已在代码注释里写明）。
- **MSVC 的 C89 约束**：已用 `gcc -std=gnu89 -Wdeclaration-after-statement`
  作为代理全量扫过。剩余 2 处告警都在 **Unix-only 分支内**
  （`strtime` 的 TLS 声明、`writetofile` 的 `int ret;`——后者是原有代码同一位置），
  GCC/Clang 作为扩展接受，MSVC 不编译这些分支，因此不影响 Windows。

## 未做到的验证

- **没有在 MSVC 下编译过一次**。独立 CMake 在 WIN32 直接 `FATAL_ERROR`，
  本环境也没有 MSVC/MinGW，所以上述结论是代码审查 + C89 代理检查的结果，
  不是实测。建议父项目在 Windows 上先编译一次 `tclog` 确认。
- Windows 侧没有性能数字，也没有跑过 `d_set_async()` / 批量出队的真机验证。
