# miniconet 性能优化 — 计划（待用户确认范围后实施）

跨平台约束已纳入：kqueue/evport/wepoll 改动须编译级验证并如实标注"未真机跑通"；
Linux 上用 engine_matrix_smoke/duplex_engine_smoke 回归 epoll/select/poll 三后端。

## 实施状态（已完成并全量验证，2026-08-28）

首轮实施未经验证；本轮补齐验证时发现并修复若干问题（含一处性能
回退，见文末"验证记录"），修复后普通构建与 ASan/UBSan 构建
全量 ctest 23/23 通过，bench 前后对照已记录。

- [x] 1. 静态大文件 sendfile（miniconet_http_server.c）：send_file_sock 加
      start_off 参数；serve_file_content 大文件改设 _server_file_path/off/size
      （含 Range 206 区间起点）；miniconet_http.h 加 _server_file_off 字段
- [x] 2. writev 聚合写：writev_all()（TLS/Windows 退化顺序写）；
      write_chunk 头/数据/CRLF 3 写并 1；write_response 头+大 body 并 1
- [x] 3. 定时器零分配：mco_timer_init/start/stop 内嵌节点 API（替换
      add/cancel，唯一调用方 mco_vcpu.c 已改）；节点内嵌 mco_task_t::
      wait_timer_obj；process() 不再 free（先摘激活标记再回调，cb 内可重 start）
- [x] 4. 引擎惰性注销 + armed 守卫：
      - mco_vcpu.c on_io_event 加 wait_fd/方向守卫（拒收陈旧事件）
      - epoll/wepoll：EPOLLONESHOT + armed_mask 记账（派发时清 0、派发尾
        为未唤醒方向重 arm——duplex 场景；唤醒后注销通常零 syscall）；
        事件数组 64→256
      - kqueue：每方向独立 knote + EV_ONESHOT（双向天然独立无需重 arm），
        armed_read/armed_write 记账；数组 64→256
      - evport：associated/assoc_mask 关联记账（派发后注销零 syscall）；
        port_event 数组 64→256
      - select/poll：用户态集合注销本就无 syscall，维持现状（poll 无固定
        事件数组，select 用 fd_set，无需扩）
- [x] 5. task 结构池化：per-vcpu task_free 链表（复用 memset 等价 calloc），
      上限 64，vcpu_free 排空；协程栈仍走原栈池
- [x] 6. HTTP 连接上下文池化：server 级 conn_free 链表（free_next 专用字段），
      上限 16；http_conn_t+64KB 池内存+32KB scratch 整体复用；
      mco_http_server_free 排空
- [x] 7. 栈池复用 memset 缩小范围：**已做**（2026-09-03）。复用时只清
      "mco_coro 结构 + 协程上下文 + storage"所在的块头部（size - 96KB），栈区
      不碰——已验证 minicoro 的 _mco_makectx 只在栈顶写初始帧、不依赖零栈；
      photon 池化分配器同样不清零。新增 `MCO_STACK_ZERO_FULL` 开关供安全敏感
      场景切回整块清零。效果：协程创建 1945 ns → 80 ns（24x）。
- [x] 8. body 读块 8KB→32KB（MCO_HTTP_BODY_CHUNK）；scratch 移出 64KB 池
      独立 malloc（随连接池复用）；上传落盘 setvbuf 64KB（save_upload +
      multipart 两处 fopen）
- [x] 9. build_response_head 单遍写入（先试写剩余空间，放不下才扩容重写，
      典型路径省一半格式化）
- [x] 10. serve_loop accept 批量排空（首连 500ms 超时轮询、后续 0 超时
      探到空即收手；stop 时提前退出排空）
- [x] 11. 事件数组扩容（epoll/wepoll/kqueue/evport 四家 64→256；select/poll
      无此结构）
- [x] 0. bench：test/bench_echo.c（32MiB 吞吐 + 1B ping-pong 延迟）、
      test/bench_http_server.c（keep-alive QPS 20000 / sendfile 8MiB×4 /
      POST 8MiB 上传+落盘校验）；CMake 注册（bench_http gated
      MINICONET_WITH_HTTP_PARSER、链 http_server 库、TIMEOUT 120、不挂
      sanitizer；bench_echo 走通用 test 循环）

## 验证记录（2026-08-28）

1. 修复首轮实施引入/暴露的问题：
   - 编译错误两处：注释里 `"bytes */<total>"` 的 `*/` 提前终止注释
     （HEAD 上即存在，说明首轮之前该文件就没编译过）；http_conn_t
     定义位置晚于 mco_http_server_free 的引用（已前移到
     struct mco_http_server 之前）
   - send_file_sock 回退路径（TLS/非 Linux）没按 start_off seek，
     Range/206 会从头错发——补 lseek（Linux sendfile 分支本就正确，
     已由 5c 用例逐字节验证）
   - on_io_timeout 在同一轮 poll+timer 双唤醒时覆盖 wait_result——
     改为仅当尚无结果时才置超时
   - **性能回退修复**：epoll/wepoll/evport 的 poll 内"重 arm/续关联"
     块删掉。它把刚唤醒的方向也重新 arm，随后协程注销又 DEL、再等
     再 ADD，每等待周期 3 次 syscall，比旧实现（ADD+DEL 两次）还多，
     keep-alive QPS/echo 延迟反而回退 ~5-8%。现依赖调度不变量
     （vcpu_run 先排空就绪队列再 poll）由唤醒协程 ctl 注销时按剩余
     方向重 arm——每周期降回 1 次 syscall。kqueue 每方向独立 knote
     本就无需处理
   - bench_http_server.c：body 未声明（编译错）、c2 悬挂指针二次 destroy、
     big_handler 缺 resp->status=200（响应行成 "HTTP/1.1 0"）
   - http_server_smoke 5c 用例按新契约重写：大文件整发 + 大区间
     Range 逐字节校验（fixture 改为模式内容），长度与偏移都能抓错
   - writev_all：形参 d[4]/l[4] 伪装定长数组（实参 d[2]/d[3] 不等长，
     触发 -Wstringop-overflow 伪溢出）+ 移位推进读未初始化槽位——
     改为 const char* const* 形参 + 偏移推进
   - static_file_handler 的 range_len 未初始化（find_header_value_r
     未命中 Range 头时不写 out_len），补初值
   - **fd 关闭复用窗口修复**：引擎槽位只按 fd 号记账，而等待者的注销
     发生在恢复之后——期间 fd 被 close 并复用、新协程在同一 fd 号注册
     时，旧等待者的注销会误清新注册（清 cb / DEL 内核项），新协程永远
     收不到事件；且新注册会沿用 stale armed 记账误判"内核已 arm"跳过
     ADD。修法：ctl 加注册身份校验（按 ud 判定）——注销时槽位已易主则
     整段跳过；注册覆盖外来注册时强制内核重同步。六个后端全改，
     mco_wait_io 注销时传回 task 作凭证。
     新增 test/fd_reuse_close_smoke.c（时序上消除一切中间让出点保证
     窗口确定形成）：修复前 epoll/select/poll 三后端必 FAIL，
     修复后全 PASS，已做判别力验证
2. 回归：普通构建 24/24 通过；ASan+UBSan 构建 24/24 通过
   （沙箱里需 ASAN_OPTIONS=verify_asan_link_order=0，环境问题非代码问题）
3. kqueue/evport/wepoll：本机无法编译/运行，维持"未真机跑通"标注；
   evport/wepoll 本轮仅做块删除式修改并人工核对结构
4. bench 前后对照（回环、多次取样；前=HEAD 基线，后=优化后）：
   - http keep-alive QPS：前 ~8480 → 后 ~8390（持平，噪声内）
   - echo 吞吐：前 ~1210 MB/s → 后 ~1300 MB/s（+7%）
   - echo 延迟：前 ~21.9 µs/往返 → 后 ~20.8 µs（-5%）
   - sendfile 8MiB×4：前 ~3400 MB/s → 后 ~3600 MB/s（持平/略好；
     该场景前后同走 sendfile 路径）
   - POST 上传落盘：前 ~708 MB/s → 后 ~928 MB/s（+30%，32KB 读块 +
     64KB stdio 缓冲生效）
   - strace -c 抽样：sendfile 80 次/32MiB；epoll_ctl 12 万次/全程
     （含客户端协程自身等待），无 DEL 风暴
5. 项 7（栈池 memset 缩小范围）已落地（见上方 [x] 7），复用时只清块头部，
   栈区留脏——已验证 minicoro 不依赖零初始化栈，ASan 构建本就关池化走 calloc。

## 补充优化（2026-09-04）：epoll 复用等待去掉 ADD/EEXIST/MOD 多余 syscall

- core/mco_engine_epoll.c 的 dirty（用户态槽位易主）分支原先把 in_kernel 清零，
  强制下次等待走 EPOLL_CTL_ADD；由于 fd 仍在集合中，ADD 必 EEXIST 失败再
  MOD——keep-alive/回环每次等待多付一次 syscall。
- 改为 dirty 时只清 armed_mask、保留 in_kernel：MOD 的 ENOENT 分支（fd 已
  close/reuse 时内核项消失）会安全回退 ADD，不丢事件。
- 效果（micro waitio_rt，pin cpu1，多轮一致）：epoll 1893 ns → 1526~1586 ns
  （约 -17%）；回归 25/27（net_api_smoke / socks5_smoke 为沙箱透明代理
  造成的 refused 环境性失败，改动前后同样失败，非本次引入）。

## 补充优化（2026-09-04）：mco_sleep_ms(0) 纯就绪让出

- `mco_sleep_ms(0)` 不再创建/插入/移除定时器节点，改为直接重新排入 ready
  队列并 yield。
- 配套（必需，不是可选补丁）：`mco_vcpu_run` 的 drain 循环加就绪预算
  （`MCO_DRAIN_BUDGET`=64，主循环局部变量，不进 vcpu 结构）。预算耗尽时
  队列里还有就绪任务，poll 必须用 `timeout=0`——此时若用
  `mco_timer_heap_next_timeout_ms()`（堆空返回 -1）会无限阻塞，排着的就绪
  任务再也跑不到。实测：预算调到 UINT_MAX 时"自旋 + 定时器 + 回环 echo"
  直接挂死（rc=124），预算 64 时 50ms 内自旋 87 万次且定时器/I/O 均正常。
- 效果：epoll `sleep0_rt_ns` 约 612 ns → 110~210 ns；uring 多轮约 64~116 ns。
  deadline、duplex、uring_async 等调度相关测试通过（27/27）。


## 补充实验（2026-09-04）：thr_c8 差距定位 & epoll poll 拷贝削减

- 背景：e2e thr_c8（8 连接持续双向 echo）仍是唯一稳定落后项：
  epoll 2684 vs 2887 MB/s（0.93x），uring 2467 vs 2697 MB/s（0.91x）。
  syscall 取证（同进程 ping-pong 20000）显示 read/write/recvfrom/sendto、
  epoll_wait/epoll_ctl、io_uring_enter 次数与 photon 基本一致，不能归因于
  "额外 read(EAGAIN)"或"重复 epoll_ctl"。
- 编译层 A/B（各自独立 /tmp 构建，同一 referee、同绑核、thr8×256MiB 多轮）：
  - `-O3 -DNDEBUG` vs 正式 `-O2 -DNDEBUG`：无稳定收益（epoll 一轮反而回落，
    uring 也在 ±10% 噪声内穿插）。**不提交** build flag 变更。
  - `-O2 -flto`（链接期同样加）vs base：同样无稳定收益，未跨静态库内联
    补偿差距。
  - 结论：正式对比保持在 `-O2 -DNDEBUG`；thr_c8 稳态几乎全走 read()/write()
    快路径，async/SQE 批量路径很少命中，SQE memset 非主因。
- 主机噪声极大（i3-7100 4 逻辑核 + IDE 常驻，编译器 A/B 同场景吞吐波动
  ±10%，超过 7-9% 差距），e2e thr 上难以可靠分辨微优化。
- 落地的干净削减：[x] `core/mco_engine_epoll.c` epoll_poll_engine 不再整块拷贝
  `mco_ev_watch_t`（含回调用不到的 armed_mask/gen/in_kernel），改为一次性取出
  本事件两个方向的 cb/ud 四个指针再回调——语义与"快照再调用"完全一致，
  指令更少。回归 25/27（net_api_smoke/socks5_smoke 仍为沙箱透明代理 refused
  环境性失败）。稳定 a3 指标：epoll ioloop_rtt ~19.5 µs，uring ~17.4 µs。

## 补充实验（2026-09-04 下午）：thr_c8 定位 —— CPU 效率碾压、自旋实验回退

**方法**：机器不再强求空闲；横向（mc vs ph）与纵向（mc 前后变体）多轮交替对比，
strace -c 直接对 thr 服务端取证。

1. **thr 服务端 syscall 形态与 photon 一致**（strace 256MiB 工作量）：
   write 2678 vs sendto 2655、read 2704/17 EAGAIN vs recvfrom 2679/16、
   epoll_wait 44 vs 34、epoll_ctl 96 vs 27（差值绝对量可忽略）。
   排除"多付 epoll_ctl/唤醒 syscall"假设。
2. **CPU 效率对比（/proc/pid/stat utime+stime，多轮一致）**：
   同为 512MiB 工作量，mc 服务端 ~13-14 ticks（≈17 ticks/GB），
   photon ~34-35 ticks（≈95 ticks/GB）——**mc 每字节 CPU 成本只有 photon ~1/5**。
   推论：photon 的空闲吞吐优势（thr +4~5%）是用烧 CPU（自旋/重调度）换
   反应延迟；代价是负载一上来崩得厉害。
3. **负载韧性（意外收获）**：高负载下（IDE 常驻，load~1.9）同条件交替对比，
   mc thr ~1500-1800 MB/s vs photon ~700-770 —— **mc 吞吐是 photon 的 2 倍**。
   此前所有"空闲 4-5% 落后"的观察与"负载 2 倍领先"并存，说明 photon 优势
   是 CPU 换来的、负载下不可持续；mc 的收益曲线远更健康。
4. **自适应忙探针实验（已回退）**：在 mco_vcpu_run 阻塞前做热流探测
   （上次 poll 有事件才探、至多 8 次 poll(0)、探空转冷）。三方交替 A/B
   （no-spin / spin / photon，两引擎各 3 轮）：spin/no-spin = 0.91~1.13，
   无稳定收益、多数略负——负载下数据到达被客户端调度延迟拉长，探空全
   浪费。空闲收益无法在本机验证（负载常驻），按证据回退，代码未保留。
5. **epoll poll 快照微优化（保留）**：epoll_poll_engine 派发不再整块拷贝
   mco_ev_watch_t，改为取 cb/ud 四指针。a3 纵向 A/B（before/after 交替 4 轮）
   20.65 vs 20.85 µs——噪声内，无收益 claim，纯代码精简保留。
6. 回归：27 项 ctest 25/27（net_api_smoke/socks5_smoke 为沙箱透明代理
   环境性失败，与改动无关）。

**结论**：thr_c8 空闲 4-5% 差距 = photon 烧 5 倍 CPU 买来的；不追。mc 的
真实卖点是同吞吐下 CPU 效率与负载韧性。下一批可做：A5a 每协程内存
（68KB vs 8.3KB，但两库分配模型不同，用户已确认不做对比目标）。

## 本轮（2026-09-04）：A 组反超 + 每协程 RSS 反超

### 调度器热路径（A 组核心改动）

- [x] **nblocked 记账 + 无阻塞者时跳过 poll**。原 `mco_vcpu_run` 在 drain
      预算（64）耗尽且就绪队列非空时无条件 `poll(0)`——`mco_sleep_ms(0)`
      自旋场景下每 64 次让出白付一次 epoll_wait/io_uring_enter。
      `mco_task_t` 新增 `blocked` 标志：`task_yield` 时若不在就绪队列则
      `vcpu->nblocked++`，`push_ready`（所有唤醒路径的必经点）对应清账。
      `nblocked==0` 时 poll 与定时器处理都不可能产出新任务（定时器只属于
      等待中的协程），直接回到 drain。纵向 A/B：sleep0_rt 54.9 → 47.2 ns。
- [x] **栈池复用不再清零**（`MCO_STACK_ZERO_HEADER` → `MCO_STACK_ZERO_NONE`）：
      这不是偷懒而是纠正重复劳动——minicoro 每次创建都会
      `memset(co, 0, sizeof(mco_coro))`（mco_init）并 memset 上下文
      （_mco_create_context），块里剩下的 storage（由 bytes_stored 划界）
      与栈区（C 未初始化区）本就是写前读。`MCO_STACK_ZERO_FULL` 保留给
      安全敏感场景；ASan 构建不走池，天然全清。
- [x] **栈池新块 calloc → malloc**：calloc 要把整个 96KB+ 块触碰成驻留页
      （glibc 对大块显式清零），这是"每协程 RSS 68KB"的直接原因。malloc
      出来的页只有真被写的才驻留。效果：`rss_per_coro_kb` 68.3 → **4.88**
      （反超 photon 的 8.35，前一轮记录里"不做对比目标"的目标就此达成）；
      `rss_per_conn_kb` 5.06-5.17 vs photon 8.58-8.66。
- [x] **自适应试探 I/O**（`mco_socket_t::rd_likely/wr_likely`）：上次试探
      EAGAIN 就跳过这次的无谓 read/write syscall，试探命中或 async 拿到
      数据则恢复。ping-pong 型负载里试探必然失败（strace 实测 20000 次
      ping 白打 40000 次 read）——此优化对 bulk 型收益明显，ping-pong 两侧
      同付，无相对收益，保留因为无害且有场景收益。

### 基准准确性修复（bench 仓）

- [x] **referee lat：static 共享缓冲 → 每线程 sbuf/rbuf + 回显校验**。旧版
      所有线程共用一个 static 64KB 缓冲：同一物理核的两个 HT 在收包时反复
      写同一缓冲，L1/L2 伪共享直接进延迟数字且随绑核方式漂移；且报告声称
      的"echo 校验回显"实际做不到（收到的数据会被其它线程覆盖）。
- [x] **referee httpbig 校验和逐字节 → 按机器字长**。32MB 逐字节累加在
      客户端（pin 在 1 个物理核）要 ~8ms，与服务端传输时间同量级——
      测出的"大文件吞吐"其实是客户端瓶颈。改成 8 字节一步后 httpbig 从
      ~2160 MB/s（与 photon 打平的假象）释放到 **5109 vs photon 3085
      （1.66x）**——之前两侧都被同一个客户端瓶颈压平了。
- [x] **micro a2/a3 超时语义与 photon 对齐**：photon 的
      `wait_for_fd_readable(fd)` 与 `sock->recv` 默认 `Timeout{}`/`m_timeout
      =-1`（不挂定时器），miniconet 侧却各挂一个 1000/10000ms 定时器
      （每次等待多付插堆+摘堆+两次取时钟）。改回 -1 后 waitio_rt
      1414 → ~1390（epoll）/ 986 → ~996（uring），与 photon 的差距消除。
- [x] **micro wait_until 忙自旋 → mco_sleep_ms(1)**：A1/A4 的计时点都在子
      协程里，守卫粒度不影响数字，但 photon 侧用 thread_usleep(100)（让出
      不抢 CPU），miniconet 自旋等于给对方让路，属不公平比较。
- [x] **run_e2e.sh 轮次奇偶交替实现顺序**：固定"mc 先 ph 后"会让后跑方
      系统性吃到更热状态，实测能把 lat_c64 P99 拉开 10%+（此前 e2e 表里
      uring 1.16x 落后即此偏置，交替后 0.94/0.97 反超）。
- [x] **BIG_SIZE_MB 8 → 32**：8MiB 一轮 ~15ms，连接建立/首包/收尾占比过高，
      测的是冷启动混合吞吐而非稳态。
- [x] **referee 延迟统计只计完成请求**：失败/提前退出的连接留下的 calloc 0
      值样本会把 P50 往下拉；nlat==0 时不再除零/越界。
- [x] **chunk 头 snprintf → 手写十六进制**（write_chunk 热路径）。

## 本轮（2026-09-04）：端到端路径（echo / HTTP）

### 先量化再动手：服务端 syscall 已是最优

`strace -c` 挂在服务端进程上实测（uring 引擎）：

- **HTTP**：30000 个 keep-alive 请求 = 29987 write + 30019 read（其中仅 435
  次 EAGAIN）+ **66 次 io_uring_enter**。即每请求恰好 2 个 syscall（1 read
  + 1 write），事件循环 enter 几乎不参与——keep-alive 下下一个请求在写完
  上一响应前就已到达，自适应试探读直接命中，**连让出都不发生**。这条路径
  已经顶到"1 读 1 写"的理论下限。
- **echo**：128MiB / 1302 write + 1310 read ≈ 每 ~98KB 一对 syscall，
  同样是下限。

结论：e2e 剩余的差距全部在用户态，而不是 syscall/拷贝。

### 改动

- [x] **小响应体内联存储**（`mco_http_response_t::_body_inline[64]` +
      `_body_is_inline`）：`mco_http_resp_text` / `mco_http_resp_html` 对
      <=64B 的 body 直接拷进结构体内的缓冲，省掉每请求一对 malloc/free
      （QPS 型负载里 "hello"/健康检查这类短响应最常见），顺带消除分配器
      抖动这个尾延迟来源。释放路径（free_response_members、
      mco_http_response_free、static_file_handler 的两处 body 替换）全部
      识别 inline 标记，杜绝 free 栈内/结构体内指针。client 端 body 来自
      网络，恒不走内联，行为不变。
- [x] **基准补上"服务端 CPU 效率"维度**（mc/ph 两侧对称实现）：
      bench server 退出时用 getrusage 差值上报 CPU 秒 + 计数
      （http_reqs / echo_bytes），run_e2e.sh 换算成
      `srv_cpu_us_per_req`（http_c32）/ `srv_cpu_us_per_mb`（thr_c8）
      写进 TSV，aggregate_e2e.py 出表。吞吐相同时 CPU/请求才是实现效率
      的真实度量——QPS 高可能只是对方烧了更多 CPU。
- [x] 顺手修一个对照陷阱：photon 的 `--big` 参数要的是**目录**
      （/tmp/benchbig），传文件路径会让 photon 服务端直接段错误——
      run_e2e.sh 本来就传对了，但手工对照时极易踩（已亲测）。

### 效果（1 轮快跑 + 3 轮正跑交叉验证，数字与 5/3 轮结论一致）

- http_c32 服务端 CPU：**10.2-11.6 µs/req vs photon 15.2**（0.67-0.76x，
  即每请求省 25-33% CPU），与 QPS 1.19-1.51x 的领先同源。
- thr_c8 服务端 CPU：403-454 µs/MB，两侧基本持平（0.93-1.05x）——
  证实 echo 吞吐持平是内核路径主导，不是实现差距。
- 回归：ctest 27/27（Debug）+ 22/22（Release）。

### 结果（2026-09-04，5/3 轮中位数）

A 组全部指标反超或持平（仅 `ioloop_rtt_us` epoll 1.01x 持平，uring 0.89x
反超）；B 组 http QPS 1.33-1.40x、http P50/P99 0.66-0.86x、httpbig
1.62-1.66x、lat_c64 P50/P99 0.90-0.97x 全部反超，thr_c8 0.97-1.02x 持平
（内核 memcpy 主导，两侧都到同一墙）。回归：ctest 27/27（Debug）+ 22/22
（Release）。

## 本轮（2026-09-09）：唤醒通道 eventfd 化 + oneshot 同 vCPU 快路径

### 先测量：量级差在"创建/销毁"，不在单次读写

- 唤醒 fd 现状：6 个引擎后端各一条 `wake_fd[2]`、`mco_workpool` 每 worker 一条 pipe、
  `mco_oneshot` 每次 `new` 一条 pipe（→ waitgroup / resolver 每次都建）。
- **引擎 `wake()` 是冷路径**：全仓库只有 `mco_vcpu.c` 的 `mco_vcpu_stop()` 一处调用。
  本文前面已用 strace 证明 HTTP/echo 热路径是"每请求 1 read + 1 write"，自适应试探
  命中时连让出都不发生，根本不经过 wake → 引擎侧不改。
- 裸测（同机，`getpid` 680 ns，绝对值是裸金属约 10 倍，只看比值）：
  create+destroy 8167 → 2508 ns（**3.3x**）；signal+drain 2759 → 2452（−11%）；
  跨线程 epoll 往返 2390 → 2153（−9%）。结论：**量级差全在"创建/销毁"
  （pipe = 2 fd + pipe()+2×fcntl+2×close = 5 syscall；eventfd = 1 fd + 2 syscall），
  单次读写只有 ~10%**。所以真正的优化点是"少建通道"+"同 vcpu 时根本不碰通道"。

### 改动

- [x] **`core/mco_wakefd.{h,c}`**：唤醒通道抽象。Linux 且 `HAVE_EVENTFD` 走 eventfd
      （1 fd / 1 syscall；`EFD_NONBLOCK|EFD_CLOEXEC` 免掉两次 fcntl；非 semaphore 模式
      一次 read 返回累计值并清零，突发只付一次 syscall）；其余平台维持 pipe/socketpair，
      只是 `rfd != wfd`。`HAVE_EVENTFD` 早就在 `cmake/PlatformConfig.cmake:115` 探测好了，
      此前从没被用过。
- [x] **`mco_oneshot`**：pipe → 通道，并加 wait 状态机（`fired/waiting/fast` +
      `waiter/waiter_vcpu`，全部复用结构里已有的 `hmutex`，不引新锁）。
      - `fire`：无人挂起 → 只置标志、不写通道（后面 `wait` 会短路）；
        **同 vcpu 且 waiter 确实挂起 → `mco_task_wake()` 直接挂 ready 队列，零 syscall**；
        其余 → 写通道。
      - `wait`：先查标志，已 fired 直接返回（连 `epoll_ctl` 都省了）；否则既挂通道
        （跨线程 fire 只能经此到达，而走哪条路事先不可知）又登记 waiter 身份。
      - 两个必要守卫：`mco_task_self() == waiter` 时不 `push_ready`（waiter 正跑在 CPU 上，
        push 会让它被二次恢复）；恢复后在锁内清 `waiting`（否则"超时后 fire"会误唤醒
        一个已经不在等的 task，同样造成二次恢复）。
      - happens-before：同 vcpu 路径由 mutex 提供（标志在锁内发布），跨线程路径仍由
        write/read 配对提供。
- [x] **`mco_vcpu`**：新增 `mco_vcpu_self()`（不惰性创建——在 resolver 那种普通线程上
      调 `mco_vcpu_current()` 会每线程泄漏一个 engine + 一条 pipe）与 `mco_task_wake()`
      （`push_ready` 的对外封装，同线程契约写进注释）。
- [x] **`mco_workpool`**：每 worker 的 pipe → 通道；突发时一次 read 排空全部
      （pipe 要每 64 字节一次 read）。

### 效果（Debug 构建，同一 bench 分别链接改动前后两版 libminiconet2.a，同机同绑核）

oneshot 生命周期（new + fire + wait + release，n=200000）：

- 同 vcpu（fire 来自本 vcpu 上另一协程）：**14900~16700 → 5133~5262 ns（2.9x）**
- 跨线程批量（线程先 fire 一批、协程再逐个 wait）：**14645~15142 → 3639~3756 ns（4.0x）**
  （4.0x 来自"fire 早于 wait 时 wait 完全不进内核"——旧实现每个 wait 都要
  epoll_ctl + read，哪怕信号早就到了）
- 每 in-flight oneshot 的 fd 占用 **2 → 1**（DNS 并发解析、workpool 背压等待直接受益）
- 折算提醒：本沙箱裸 syscall 680 ns，绝对值偏大约 10x；裸金属按 syscall 数缩减折算
  （每次等待从 ~6 个 syscall 降到 1~2 个）。

### 回归

- 默认构建 25/27；`WORKPOOL=ON` 构建 29/31。两次都只剩 `net_api_smoke` /
  `socks5_smoke` —— 已用 `git stash` 在 HEAD 基线上验证同样失败（沙箱透明代理 refused，
  非本次引入）。
- ASan+UBSan+WORKPOOL：串行 22/26，失败项 = net_api/socks5 + 2 个随机超时；
  HEAD 基线同配置串行失败更多（≥8 项，含 fd_reuse_close / tcp_echo / udp_echo 等），
  且这些用例逐个单独重跑全部 ≤2 s 通过 → ASan 超时是沙箱随机抖动，非代码问题。
- oneshot / workpool / resolver 相关 6 个用例 × 15 轮压力复跑全过（抓跨线程竞态）。

### 明确不改

- 六个引擎的 `wake_fd`：冷路径，改了在 bench 上量不出来；且 uring 的
  `submit_wake_rearm()` 依赖 pipe 的 drain 语义，属于花钱买不到东西还引入风险。
  顺带：evport 用的 `PORT_SOURCE_USER` + `port_send()` 本就是零 fd 唤醒，思路早已验证。

## 本轮（2026-09-26）：代码审查——热路径削减 + 死代码清理 + error_handler 接线

非 bench 驱动的静态审查（性能 / 冗余 / 可读性三路），全部以"行为不变"为前提
（唯一例外是 error_handler 接线，见下，已单独确认）。四种构建全量回归：
默认 22/22、`HTTP_PARSER+WORKPOOL` 31/31、`HTTP_PARSER+ASan/UBSan` 27/27、
`IO_URING+HTTP_PARSER` 27/27（`engine_matrix_smoke` 实测打印
`[io_uring] ... client_ok=1`，`uring_async_smoke` PASSED——非仅编译级）。

### 性能

- [x] **每请求 header 单遍扫描**（miniconet_http_server.c）：原来 Content-Length /
      Transfer-Encoding / Content-Type / Upgrade / Sec-WebSocket-Key 各做一次线性
      搜索，Connection 又在 `client_wants_close` 里扫一次，且每次比较都重算
      `strlen(target)`。新增 `header_name_is(name,name_len,target,target_len)`
      （字面量长度由调用方给，扫描期零 strlen），一次遍历按 name_len（14/17/12/7/10）
      分派取出全部字段；`client_wants_close` 改收两个标志位。语义保持"首个匹配优先"、
      Connection 仍"同名头 OR"。
- [x] **`mco_http_req_body` 栈播种**：原 `memset(&g,0,...)` 后 `growbuf_ensure` 必走
      malloc（首个字节就 32KB），注释却写"Seeded on the stack"。种子缓冲改放
      `handle_one_request` 的栈帧（`body_seed[4KB]`，与 handler 同生命周期）经
      `body_reader_t` 传入，`body_on_stack` 标记所有权，3 处释放点改为条件 free；
      空 body 仍返回 NULL（保持历史语义）。
      **为什么不能把种子放 `mco_http_req_body` 自己的栈帧**：`req->body` 随后会被
      `free()`，指向栈会崩。小 body（`http_server_smoke` 的 `POST /echo` 11 字节）
      现在零分配；ASan 下验证（若误 free 栈指针会立刻 bad-free）。
- [x] **静态文件不再打开两次**：`serve_file_content` 原来 `fopen`+2×`fseek`+`ftell`
      +`fclose` 只为量大小，大文件随后 `send_file_response` 又 `open` 一次（4 个
      syscall）。改 `stat()`（1 个）+ `S_ISREG` 守卫；大文件分支不再 open，小文件才
      fopen。**顺带修掉**"目录请求 `fread` 失败却按 `content_len` 输出未初始化内存"
      （`(void)rd` 忽略返回值）。
- [x] **chunk 长度头 `snprintf` → 手写十六进制**（新增 `u64_to_hex`，与已有
      `u64_to_str` 同款）。PLAN 里"chunk 头 snprintf → 手写"此前只落在 bench 仓，
      库内 `mco_http_resp_write_chunk` 漏改。
- [x] **accept 硬错误忙自旋**：`mco_tcp_accept` 把超时和 EMFILE 都收敛成 NULL，
      `serve_loop` 直接 `continue`，fd 耗尽时打满 CPU。失败后加 `mco_sleep_ms(1)`
      （正常空闲 accept 阻塞 500ms，多 1ms 可忽略）。
- [x] **`build_response_head` 单遍**：原来遍历 header 数组两遍、`strlen(name)` 算两遍。
      改一遍：边写自定义头边判 has_*，自动头（CL/TE + Connection）先攒进
      `auto_hdr[80]` 再 `memmove` 插到自定义头之前，**头顺序不变**；顺带删掉因此
      不再使用的 `growbuf_append`。
- [x] **select 后端扫描收窄**（mco_engine_select.c）：新增 `max_fd`（最高在用 fd），
      `ctl` 时上调、释放最高位时下调，poll 的 fd_set 重建从 `0..watches_cap`
      （可达 FD_SETSIZE=1024 且从不收缩）改为 `0..max_fd`；收缩循环摊销 O(1)。
- [x] **定时器堆空时不再取时钟**（mco_timer.c）：`mco_timer_heap_process` 原来先
      `gethrtime_us()` 再判空；该函数每轮 poll 都调，无定时器时是纯浪费
      （`next_timeout_ms` 本就是先判空再取时钟）。

### 冗余清理（死代码，均 grep 验证）

- [x] 死字段：`mco_timer_t::canceled`（只写不读，且注释"延迟到 process() 移除"与实现
      `stop()` 立即 `heap_remove` 矛盾）、`mco_vcpu::owns_default`、
      `mco_http_server::serve_loop`、`mco_uring_t` 的 5 个只写不读字段
      （`cq_tail_cached`/`sq_flags`/`sq_dropped`/`cq_entries_p`/`cq_flags`）、
      `mco_engine_iocp` 的 `connect_fn`/`has_connectex`。
- [x] 死代码/宏：`WS_FRAME_HEADER_MAX`、`MCO_IORING_SQ_CQ_OVERFLOW`、
      miniconet_http.c 的 `file_sink_t`/`file_sink_write`（服务端另有一份才是活的）、
      `mco_test_socket_ssl_handle`、两个空 multipart 回调
      （`mp_on_part_data_begin`/`mp_on_body_end`）、注释掉的死代码、
      mco_vcpu.c 里与头文件重复定义的 `MCO_STACK_ZERO_*`。
- [x] 未用 include：miniconet.c 的 htime.h、http_server.c 的 hmemdup.h、
      mco_timer.c 的 stdlib.h。
- [x] 未用形参：`serve_file_content` 的 `timeout_ms`/`chunked_ok`、
      `task_resume` 的 `vcpu`。
- [x] 重复函数：`find_header_value` 与 `find_header_value_r` 逻辑相同
      （`req->headers` 就是 `struct phr_header[]`），删前者、调用点改用后者。
- [x] 一次性包装：`conn_ctx_t`（单字段结构）降为局部 `int`。
- [x] **io_uring 唤醒死链**：`wake_tag()` 无调用 → `URING_KIND_WAKE` 分支不可达 →
      `submit_wake_rearm()` 只在该分支被调。**注意**：09-09 那条"uring 的
      `submit_wake_rearm()` 依赖 pipe 的 drain 语义"的记载与 HEAD 不符——该函数在
      HEAD 上已是死代码（编译期即可证：删掉 `URING_KIND_WAKE` 后无编译错误）。
      唤醒实际由 create 里那次 **WATCH 标签**的站立 POLL_ADD 承担。已删死链并在原地
      补注释说明真实机制。**遗留**：该 poll 是一次性且 pipe 从不 drain，够用只是因为
      `mco_vcpu_stop()` 只会发生一次；若将来要支持重复唤醒需重新设计（单独排期）。
- [x] **IOCP**：删 `connect_fn`/`has_connectex`；`iocp_submit_connect` **保留为显式
      stub**（`mco_wait_connect` 会无条件解引用 `ops->submit_connect`，置 NULL 会崩；
      stub 返回 0 正好触发就绪回退）。ConnectEx 无法附着到已 `connect()` 的 socket，
      所以"不做异步 connect"是正确设计，不是缺陷。Windows 专属，仅静态核对，
      未真机编译。

### 行为变更（唯一一处，已确认后实施）

- [x] **`error_handler` 接线**（方案 b，非破坏）：handler 返回后若 `resp->status == 0`
      （handler 忘了产出响应），先调 `srv->error_handler`；未设或它仍未设状态则回
      内置 500 页。此前这种情况会发出畸形状态行 `HTTP/1.1 0 OK`（`status_reason(0)`
      落到 default 分支返回 "OK"）。同时给公共 API
      `mco_http_server_set_error_handler` 一个真实触发点（此前字段只写不读，
      grep 全仓 0 处读取）。
      - 临时程序实测：无钩子 → `HTTP/1.1 500`；装钩子 → `HTTP/1.1 503`；
        正常路由不受影响 → `HTTP/1.1 200`。
      - 头文件注释同步改为 "handler left status 0 -> 500"。

### 明确不做（本轮讨论后决定，记录理由）

- **WebSocket 持久发送缓冲复用：有真实重入风险**。`ws_send_frame` 先构建帧再
  `mco_write_all`（会 yield）；若共享缓冲，协程 A 发送中 yield 时，协程 B 对同一
  `ws` 调 `mco_ws_recv` 收到 PING → 自动回 PONG → 覆盖 A 正在发送的帧。socket 层
  `MCO_ERR_BUSY` 挡不住（缓冲在到达 `mco_write` 前就已被覆盖）。保持现状
  malloc/free。若要做，需加 `sending` 标志、占用中退回 malloc（安全的混合方案）。
- **跨后端去重**（`on_wake_drain`×6 / `watches_ensure`×5 / `ctl` 身份校验×8）：
  会同时改动 kqueue/evport/wepoll/iocp 等未真机验证的后端，风险高，先不做。
- **`mco_read`/`mco_write` 双分支去重**（TLS 分支 + 就绪分支近逐字重复 ~120 行）：
  属最热路径，重构风险高，不改。
- 低优先级可选项（未做）：HTTP **客户端** body 读块 8KB→32KB（会增加协程栈占用）、
  chunked 解码的"每段从头重扫 + 每 chunk memmove"。

## 本轮（2026-09-29）：B 组端到端复测（epoll / io_uring，2 轮）

环境：Intel i3-7100（2C/4T），Linux 7.0.0-34，governor=powersave，
cpu_server=1 / cpu_client=0,2，big_bytes=32MiB，rounds=2。裁判客户端同一套。

相对值 = miniconet ÷ photon（延迟/CPU 越低越好，吞吐越高越好）：

| 场景 / 指标 | epoll | io_uring |
|---|---|---|
| http_c32 QPS | 113040 / 86350 = **1.31×** | 121631 / 89696 = **1.36×** |
| http_c32 P50 (µs) | 280.6 / 371.3 = **0.76×** | 261.3 / 353.9 = **0.74×** |
| http_c32 P99 (µs) | 313.1 / 557.9 = **0.56×** | 291.5 / 406.0 = **0.72×** |
| http_c32 服务端 CPU (µs/req) | 8.88 / 11.7 = **0.76×** | 8.26 / 11.2 = **0.74×** |
| httpbig_c4 (MB/s) | 5667 / 3412 = **1.66×** | 5468 / 3478 = **1.57×** |
| thr_c8 echo (MB/s) | 2747 / 2786 = 0.99× | 2890 / 2730 = **1.06×** |
| thr_c8 服务端 CPU (µs/MB) | 365.4 / 361.5 = 1.01× | 349.2 / 370.3 = **0.94×** |
| lat_c1_1B 平均 (µs) | 14.3 / 14.3 = 1.00× | 14.3 / 14.4 = 1.00× |
| lat_c1_512B 平均 (µs) | 14.7 / 14.6 = 1.01× | 14.4 / 14.7 = 0.98× |
| lat_c64_512B 平均 (µs) | 531.6 / 526.3 = 1.01× | 508.6 / 513.3 = 0.99× |
| lat_c64_512B QPS | 119552 / 120614 = 0.99× | 125250 / 123942 = 1.01× |

结论：

- **HTTP 全面领先**：QPS +31~36%、每请求 CPU −24~26%、P99 最多 −44%、大文件 +57~66%。
- **TCP echo 基本持平**（吞吐 0.99~1.06×、CPU 0.94~1.01×）：该路径受内核拷贝限制，
  双方都到同一堵墙，与 2026-09-04 的结论一致。
- **lat_c64_512B（64 连接小包 ping-pong）落后 chv ~15%**：avg 531.6 vs chv 456.5 µs，
  QPS 119552 vs 138373。miniconet 与 photon 在此互在 1% 以内。
- 与本文件 2026-09-04 记录的"每字节 CPU ~1/5"**不一致**：那是 /proc/pid/stat 的
  ticks/GB 口径；本轮 getrusage 口径的 thr_c8 服务端 CPU 是持平。README 已改用本轮
  口径（更保守、可复现），旧的"~1/5"不再对外引用。

## 第 0 步：基准（先测量后优化）

- [x] `test/bench_http_server.c`：keep-alive QPS / 静态文件 MB/s / POST 上传 MB/s
- [x] `test/bench_echo.c`：TCP 吞吐
- [x] `strace -c` 抽样每请求 syscall 数，记录基线

## 第 1 批：热路径 syscall/拷贝削减（核心收益）

- [ ] 1. 静态大文件走 sendfile 零拷贝：`serve_file_content`（miniconet_http_server.c
      ~:1014）>1MB 现走 32KB fread+write_chunk；改设 `_server_file_path` 走
      `send_file_sock`（~:792），**新增起始偏移参数**支持 Range/206。
      仅 `__linux__` sendfile，其余平台/TLS 用已有 read/write 回退（不加新平台分支）
- [ ] 2. chunk 写出 3 次 write → 1 次 writev：`mco_http_resp_write_chunk`（~:743）
      head/data/CRLF 聚合；compat 层薄封装（POSIX writev / Windows WSASend 或退化顺序写）；
      `write_response` 头+大 body 同样合并
- [ ] 3. 等待定时器零分配：mco_timer_t 节点内嵌进 mco_task_t（wait_timer 字段已预留），
      每次 `mco_wait_io` 省一对 calloc/free（core/mco_timer.c + mco_vcpu.c）
- [ ] 4. io 注册惰性注销（按后端分策略，vcpu 层加 armed 守卫 = `task->wait_fd` 校验）：
      - epoll/wepoll：ADD 带 `EPOLLONESHOT`，每次等待 1 次 MOD 重 arm（原 ADD+DEL 两次）
      - kqueue：`EV_ONESHOT`/`EV_DISPATCH`
      - evport：原生一次性关联，只跳过唤醒后注销
      - select/poll：无 per-fd syscall，维持现状

## 第 2 批：分配削减

- [ ] 5. mco_task_t 结构池化（per-vcpu freelist，对齐栈池模式；ASan 构建自动关闭同栈池）
- [ ] 6. HTTP 连接上下文池化（http_conn_t + 64KB pool_mem 每连接 malloc/free → server 级 freelist）
- [ ] 7. 栈池复用 memset 缩到 mco_coro struct 区域（mco_create 自己 memset co，minicoro.h:1708；
      需先验证 storage 布局再动）
- [ ] 8. body 读块 8KB→32KB（scratch 移出 64KB 池独立分配）+ 上传落盘 setvbuf 64KB

## 第 3 批：微优化

- [ ] 9. build_response_head 单遍写入（现在每 header 两次 snprintf）
- [ ] 10. serve_loop accept 批量化（accept 到 EAGAIN 再让出）
- [ ] 11. 各后端 poll 事件数组 64→256（同步六个后端）

## 验证

- 全量 ctest + ASan/UBSan 不回归；bench 前后对比（QPS / MB/s / syscall 数）

## 协程栈守则（96KB 栈，栈上缓冲 ≤32KB 才免 malloc，见 core/mco_vcpu.c:194）

- 所有新增缓冲 ≤32KB 才允许走协程栈，超了必须池/堆分配；可用 gcc -fstack-usage 抽查
- 项 8 的 32KB scratch 是连接级池内分配，绝不改成栈缓冲
- 项 1 sendfile 在 Linux 非 TLS 下消灭 serve_file_content 的 32KB 栈缓冲；回退路径保留的
  32KB 栈 buf 恰在预算内
- 项 7 memset 缩减后栈区留脏，实施前先验证 minicoro 不依赖零初始化栈（ASan 构建本就关池化）

## 明确不做（本轮）

- io_uring 后端 / SO_REUSEPORT 多核 worker 池（架构级，单独立项）
- DNS 缓存、响应头预编码缓存（语义变化）
