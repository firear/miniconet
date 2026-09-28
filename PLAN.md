# miniconet2 重新设计 — 计划清单

参考 PhotonLibOS 思路的全新实现：minicoro 协程 + hv 基础设施 + 自研 io engine，
不兼容旧版 API/实现。

## 阶段 0：核心架构

- [x] 目录骨架：`core/`（engine+调度器+定时器）、`compat/`（hv 基础设施，仅保留非事件循环部分）、`minicoro/`
- [x] `core/mco_engine.h` — io engine 最小 vtable 接口（create/destroy/ctl/poll/wake）
- [x] `core/mco_timer.h/.c` — 基于 hv `heap.h` 的定时器堆（mco_sleep / 读写超时）
- [x] `core/mco_vcpu.h/.c` — 对称调度器：就绪队列 + minicoro 协程生命周期 + `mco_wait_io()`
- [x] `miniconet.h/.c` — 用户 API：`mco_tcp_listen/accept/connect/read/write/close`

## 阶段 1：正确性修复（相对旧版审计出的 bug）

- [x] EOF 与错误精确区分（`mco_read` 返回值 >=0 是字节数，0=EOF，<0 是 `MCO_ERR_*`）
- [x] `mco_read/mco_write` 直接用调用者的 `buf/len`，不再有内部缓冲区吞数据的问题
- [x] `mco_close()` 主动唤醒所有等待该 socket 的协程（`MCO_ERR_CLOSED`），不再永久挂起泄漏
- [x] 同一方向并发读/写从"静默覆盖状态"改成显式 `MCO_ERR_BUSY` 拒绝
- [x] **本轮自查新发现并修复**：同一 fd 读方向、写方向是独立槽位，两个不同协程可以分别只等读/只等写而不互相顶掉注册（原设计里所有后端都是"每 fd 一个 cb/ud 槽位"，已重写为读写双槽位，`mco_wait_io` 取消注册时也已改为只取消对应方向）

## 阶段 2：io engine 后端

| 后端 | 平台 | 实现 | 编译验证 | 功能测试 | 备注 |
|---|---|---|---|---|---|
| epoll | Linux | ✅ | ✅ | ✅ | 本沙箱内验证，含 duplex/close-wakeup/EOF 场景 |
| select | POSIX + Windows | ✅ | ✅(POSIX) | ✅(POSIX) | Windows 分支用 hv `Socketpair()` 模拟自唤醒，未实测 |
| poll | POSIX | ✅ | ✅ | ✅ | 不支持 Windows（WSAPoll 语义不同，让 wepoll/select 兜底） |
| kqueue | macOS/*BSD | ✅ | ⬜ 未在真机验证 | ⬜ | 读写天然独立 filter，increment ADD/DELETE |
| evport | Solaris/illumos | ✅ | ⬜ 未在真机验证 | ⬜ | 处理了"一次性关联"语义下的自动重新 associate |
| wepoll | Windows | ✅ | ⬜ 未在真机验证 | ⬜ | 注意 `HANDLE`/`SOCKET` 类型，不是简单 int fd |
| iocp | Windows | ✅ | ✅(编译级) | ⬜ 未在真机验证 | 完成式后端，`MINICONET_WITH_IOCP` 开关，见阶段 21 |

- [x] `core/mco_engine_backends.h` — 统一声明各平台后端的显式 ops 获取函数
- [x] `core/mco_engine_default.c` — 按平台自动选择（Linux→epoll, BSD/mac→kqueue, Solaris→evport, Windows→wepoll, 其它→select）
- [ ] **待办**：找 macOS/BSD 机器跑 `examples/engine_matrix_smoke.c` + `examples/duplex_engine_smoke.c`（把 kqueue 加进矩阵）
- [ ] **待办**：找 Windows 机器（MinGW/MSVC）交叉编译验证 wepoll 后端 + `Socketpair()` 路径
- [ ] **待办**：Solaris/illumos 环境验证 evport（如果没有真机，至少找到能装的 CI 镜像跑一次）

## 阶段 3：测试覆盖

- [x] `examples/tcp_echo_smoke.c` — accept/connect/read/write 全链路 + EOF 精确语义
- [x] `examples/close_wakeup_smoke.c` — close 时唤醒阻塞中的协程，不泄漏
- [x] `examples/engine_matrix_smoke.c` — 同一套 echo 场景在 epoll/select/poll 上跑一遍
- [x] `examples/duplex_engine_smoke.c` — 确定性验证读写协程共享同一 fd 不互相顶掉（socketpair + 灌满缓冲区），已覆盖 epoll/select/poll
- [x] `examples/duplex_smoke.c` — 同一场景的真实 TCP 版本（时序上不如 duplex_engine_smoke 确定，作为补充）
- [x] `examples/dns_async_smoke.c` — 8 个并发异步解析 + 1 个持续打点协程证明不阻塞 vcpu；外加 `mco_tcp_connect("localhost", ...)` 端到端集成测试
- [x] `examples/udp_echo_smoke.c` — `mco_udp_bind/socket/sendto/recvfrom` 全链路
- [x] `examples/deadline_smoke.c` — 绝对 deadline 跨调用共享超时预算 + socket 默认超时
- [x] `examples/stack_pool_smoke.c` — 栈池确实复用（且能被关掉），ASan 构建下自动降级验证
- [x] `examples/udp_ipv6_smoke.c` — IPv4 回归 + IPv6（带能力探测的优雅跳过，本沙箱环境未验证过 IPv6 真实收发）
- [x] `examples/tls_echo_smoke.c` — 真实 TLS 握手+加密读写+EOF 映射，用 tcpdump 实际抓包验证过是密文（非仅代码走查）
- [x] `examples/tls_verify_smoke.c` — 验证 `verify_peer` 参数真的生效，不是摆设
- [x] `examples/tls_duplex_smoke.c` — 验证 `ssl_cross_owner` 保护不会误伤正常的并发读写（负向测试）
- [x] `examples/tls_cross_owner_smoke.c` — 用测试专用钩子确定性验证 `ssl_cross_owner` 保护逻辑本身：占用期间返回 `MCO_ERR_BUSY`，释放后恢复正常
- [x] `examples/websocket_smoke.c` — WS 客户端握手（Sec-WebSocket-Accept 校验）+ 文本回声 + 服务端 PING 触发客户端自动 PONG + CLOSE 帧映射成 `MCO_ERR_CLOSED`（见阶段 13）
- [x] `examples/http_client_smoke.c` — `mco_http_get/post`：Content-Length body 读取、Transfer-Encoding: chunked 解码、POST 请求体回显、响应头大小写不敏感查找（见阶段 13）
- [x] `examples/tcp_proxy_server.c` — 示例程序（非断言式测试）：双向转发的协程化用法演示，命令行参数可调，作为集成样例而不是回归测试
- [x] `examples/workpool_smoke.c` — workpool fan-out：8 任务投到 4 worker，CPU bound 负载 + mco_sleep，断言总耗时 < 阈值（单 vcpu 串行会明显超），验证多核并行 + 结果正确性（见阶段 14）
- [x] 用 ASan + UBSan 跑过全部测试，无内存/未定义行为报告
- [x] 用 ThreadSanitizer 单独跑过 `dns_async_smoke`（唯一涉及真实多线程的部分），发现并修复了一处数据竞争（见阶段 4）

## 阶段 4：本轮完成

- [x] **UDP 支持**：`mco_udp_bind`（本地绑定，同步解析）/ `mco_udp_socket`（未绑定客户端 socket，~~目前固定 IPv4，已知限制~~ 阶段 7 已改为懒创建双栈）/ `mco_udp_sendto` / `mco_udp_recvfrom`，复用同一套 `mco_wait_io` 原语，读写各自独立槽位（duplex 修复自动适用）
- [x] **DNS 异步化**：`core/mco_resolver.h/.c`——后台线程池（默认 4 线程，`mco_resolver_set_pool_size()` 可调）跑阻塞的 `ResolveAddr()`，每个任务配一个独立 pipe，协程用标准 `mco_wait_io()` 在 pipe 读端挂起，不需要给调度器加任何特殊机制。`mco_tcp_connect`/`mco_udp_sendto` 已接入；`mco_tcp_listen`/`mco_udp_bind` 刻意保持同步（理由见 miniconet.h 注释：常在协程外、`mco_loop_run()` 之前调用，此时没有"当前协程"可用）
  - 用 8 个并发解析 + 1 个持续打点的协程验证：`examples/dns_async_smoke.c`，ticker 全程跑满（40/40 ticks）证明没被 resolve 卡住
  - **过程中 TSan 抓到一个真实的数据竞争**：最初实现只依赖"epoll 通知管道可读"就去读 worker 线程写的结果字段，没有真正 `read()` 那个通知字节——没有实际的 read()/write() 系统调用配对，就没有 POSIX/工具认可的 happens-before。修复：读到通知后先真正 `read()` 一次再访问结果，问题解决（TSan 复测干净）
- [x] **CMakeLists.txt**：`add_library(miniconet2 STATIC ...)` + 每个 `examples/*.c` 自动生成一个可执行文件并注册进 CTest；`build_and_test.sh` 保留作为不装 CMake 时的备用方式，两者源文件列表保持一致

## 阶段 6：本轮完成（超时精细化 + 协程栈池化）

- [x] **超时精细化**：`core/mco_timer.h/.c` 新增 `mco_deadline_t`（绝对时间点）+ `mco_deadline_after()`/`mco_deadline_ms()`，跨多次调用共享同一个超时预算，不用业务代码手动做减法；`mco_tcp_connect`/`mco_udp_sendto` 内部已改用它（替换掉之前手写的 `remaining_timeout()`，用自己的新工具吃自己的狗粮）。另外加了 socket 级别的默认超时：`mco_socket_set_timeout()` + `mco_read_default()`/`mco_write_default()`，纯加法，不改变任何现有函数签名。（**后半句已被阶段 20 推翻**：`mco_tcp_connect()`/`mco_tls_connect()` 末尾加了 `out_errno` 出参，是第一次改既有函数签名，见阶段 20 的理由）
  - `examples/deadline_smoke.c` 验证：同一个 dl 跨两次 `mco_wait_io` 调用，总耗时 ~310ms（不是 300+300=600ms），证明预算是共享的
- [x] **协程栈池化**：`mco_go()` 创建的协程结束后，栈内存回收进每个 vcpu 自己的空闲池，下次复用而不是重新 malloc；`mco_vcpu_set_stack_pool_limit()` 可调上限/关闭。
  - `examples/stack_pool_smoke.c` 验证：3 批各 50 个短命协程，第 2、3 批全部复用（`reuse_count=100`），关闭池化后 `reuse_count=0`
  - **排查记录（自查环节，别删）**：ASan 下这个功能会触发 `stack-buffer-underflow` 崩溃。根因不是池化逻辑本身写错——是 minicoro 为了支持 ASan 会用 `__sanitizer_start_switch_fiber`/`finish_switch_fiber` 把协程栈标记成"纤程栈"，ASan 因此围绕它布置*栈风格*的 redzone；我们绕过正常 `free()`→`calloc()` 循环、直接复用裸内存，那段地址范围里 ASan 的栈 redzone 影子字节没有随复用正确复位，导致后续合法的 `memset()` 被判定为越界写。修法：检测同一个 `__has_feature(address_sanitizer)`/`__SANITIZE_ADDRESS__` 宏（和 minicoro 自己用的一致），ASan 构建下自动关闭池化、退化成每次真 calloc/free（ASan 自带缓存分配器，池化在那种构建下本来也没有性能意义）
  - **顺带发现并修的另一个问题**：`examples/duplex_smoke.c` 里两个协程共享同一个 `mco_socket_t*`（一个只读一个只写），但都没负责关闭/释放它，ASan 跑出 32 字节泄漏——不是库的 bug，是这个测试文件自己的资源管理疏漏，补了个"两边都用完才关闭"的完成计数协调

## 阶段 7：本轮完成（IPv6）

- [x] **UDP IPv6**：`mco_udp_socket()` 改成懒创建（`fd=-1`），真正的 `socket()` 推迟到第一次 `mco_udp_sendto()` 解析出目标地址后，按解析结果的地址族现场创建——同一个函数天然支持双栈，不再需要 `mco_udp_socket6()` 变体。代价：必须先 `mco_udp_sendto()` 才能 `mco_udp_recvfrom()`（纯接收场景本来就该用 `mco_udp_bind()`）。TCP 一侧（`mco_tcp_connect/listen`）从一开始就是地址族无关的，不需要改
  - `examples/udp_ipv6_smoke.c` 验证：IPv4 回归 + IPv6 回环（`::1`），**带能力探测的优雅跳过**——本沙箱环境本身未启用 IPv6（`socket(AF_INET6,...)` 返回 `EAFNOSUPPORT`），测试如实报告"环境不支持，已跳过"而不是假装验证过；IPv4 部分照常跑通，证明双栈改造没有破坏原有行为。**IPv6 端到端行为目前只有代码审查级别的信心，没有真机跑通过**，如实记录

## 阶段 20：本轮完成（SOCKS5 代理 + 一批"写协议实现天天要用"的 API）

起心动念是补 D 类待办里的 SOCKS5 代理，动手时发现缺的几个能力（凑够 N 字节读写、半关闭、connect 失败原因）是所有协议实现共用的，于是先补公开 API，再写示例。

- [x] **SOCKS5 代理**（`examples/socks5_server.c`）：RFC 1928，只做 CONNECT + 无认证（METHOD 0x00），BIND / UDP ASSOCIATE 一律回 REP 7。每连接一协程做协商（一条直线代码、无状态机跳表）；双向转发必须拆成两个 pump 协程（同一 socket 同一方向只允许一个等待者，见 miniconet.h 头注释）用 `mco_waitgroup` 等两边收摊；一个方向读到 EOF 只 `mco_shutdown(WR)` 半关闭，保证"发完请求就关写方向、等着收响应"的客户端不被截断；`BND.ADDR` 用"代理→目标"这条连接的 `getsockname` 结果，多网卡时才正确。协议常量自带一份没有复用 `compat/protocol/socks5.h`（vendor 自 shadowsocks-libev 的 GPL 文件 + `__attribute__((packed))` 在 MSVC 编不过 + 它只覆盖定长 4 字节头）
- [x] **新增 API**（`miniconet.h/.c`）：
  - `mco_read_exact()` / `mco_write_all()`：内部用 deadline 摊分预算（timeout 是整个操作的，不是单次 syscall 的，否则分多次凑满时总耗时会随次数放大）；`read_exact` 遇 EOF 返回已凑到的字节数（可能是 0），错误返回 `MCO_ERR_*`
  - `mco_shutdown(s, how)`：`MCO_SHUT_RD/WR/RDWR` 取 0/1/2，与 POSIX `SHUT_*`、Winsock `SD_*` 同值可直接透传；只做 fd 层 shutdown，不唤醒等待者、不置 `closed`（半关闭后那个方向还要能读到 EOF / 拿到 EPIPE，收摊时机交给调用方）
  - `mco_socket_local_addr()` / `mco_socket_peer_addr()`：返回 `sockaddr_u` 而不是字符串——不丢地址族、也不必纠结缓冲区给多大；要字符串用 hsocket.h 的 `sockaddr_ip()`/`sockaddr_port()`/`sockaddr_str()`
  - `mco_socket_set_nodelay()`：代理/交互式协议的标配
  - `mco_tcp_connect()` 3 参改 4 参、`mco_tls_connect()` 4 参改 5 参，末尾加 `int* out_errno`（`MCO_CONNECT_*`），可传 NULL
- [x] **`compat/hsocket.h` 正式对外**：`sockaddr_u` 及 `sockaddr_ip/port/str/compare` 一套地址工具就是给调用方用的（此前已通过 `target_include_directories(... PUBLIC ...)` 暴露，这次在 `miniconet.h` 里显式 include 并把语义写进注释，不再靠间接包含）
- [x] **破坏性改动的代价与理由**：全仓 ~30 处调用点同步改成传 NULL，编译期即可发现遗漏。改签名的理由是 SOCKS5 要把 REFUSED 和 DNS 失败翻译成不同的 REP、HTTP 代理要区分 502 和 504——只返回 NULL 拿不到这些信息
- [x] 验证：`test/net_api_smoke.c`（分两次到达的短读要被凑齐、半关闭后仍能收完响应、65536 字节 `write_all`、地址查询、REFUSED/DNS/INVALID 三类分类）+ `test/socks5_smoke.c`（端到端：CONNECT(IPv4) / CONNECT(域名) 转发、半关闭不截断、UDP→REP 7、坏 ATYP→REP 8、端口没人听→REP 5、域名不存在→REP 4、不接受的认证方法→0xFF 后关闭）；ctest 22/22
- [x] `test/socks5_smoke.c` 用 `#define SOCKS5_EXCLUDE_MAIN` + `#include "../examples/socks5_server.c"` 直接复用示例源码（示例的 `main()` 用 `#ifndef SOCKS5_EXCLUDE_MAIN` 包住），示例和测试不会漂移
- [x] **用新 API 反哺现有代码**（"自己吃自己的狗粮"，顺手清掉两个短写 bug）：
  - `miniconet_http_server.c` 的私有 `static write_all()` 手写循环 → 改成薄封装，内部调 `mco_write_all()`（返回值语义是"0/-1"，本文件几十处调用都按这个写，不为了换底层去全改调用点；新的 deadline 是"整次操作"预算，比原循环"每次 `mco_write` 都给满 timeout、大响应分多次写总耗时会随次数线性放大"更合理）
  - `miniconet_websocket.c` 的 `ws_send_frame`：原来单次 `mco_write` 拿单次结果和整帧长度 `frame_size` 比——**短写会被误判成 IO 错误、且帧已发了一半**，真 bug。改为 `mco_write_all`，比对 `built`（帧实际构建长度）而非 `frame_size`
  - `miniconet_http.c` 的 `mco_http_(get|post)_ex` 发请求头 + 可选 body：原来单次 `mco_write` + `n != len` 判断——POST 大 body 必然短写被误判失败，真 bug。改 `mco_write_all`
  - `examples/tcp_proxy_server.c` 的 `forward_data`：同样的单次 `mco_write` 部分写误判，改 `mco_write_all`
  - **刻意没动的**：`read_body_content_length/chunked/until_eof` 是"边读边流式下沉到 sink"语义（不缓存整个 body），`mco_read_exact` 会强制整块入内存破坏流式；HTTP server 的 `writev` 聚合写是 syscall 优化路径、不等价。这俩保留原循环
- [x] 验证：默认构建 ctest 22/22；`-DMINICONET_WITH_HTTP_PARSER=ON` 构建 ctest 27/27（覆盖上面三处库内改动）；ASan+UBSan 22/22
- [x] **修 `tcp_proxy_server` 转发缺陷**（之前审查时确认的两个 bug）：
  - 半关闭不传播：原 `forward_task` 单协程串行轮询，某方向读到 EOF 只把本协程的 closed 标志置 1，从不对对端 `mco_shutdown(WR)`。标准 HTTP 客户端"发完请求就 shutdown(WR) 等响应"、服务端靠读到 EOF 判定请求体结束，于是服务端永远等不到 EOF 卡死。改成**两个方向各一个 pump 协程并发转发**：源 EOF 时对 dst 做 `mco_shutdown(WR)` 通知对端，但本方向继续读 dst 收尾包；最后一个结束的 pump 负责兜底 `mco_close`+`destroy`+`free`（`closing`/`cleaned` 标志防重复 close、防重入）。错误/超时分支对照处理（真实错误关整条连接唤醒对端；超时只继续循环，循环顶部查 `g_running` 保证 Ctrl+C 可退出）。
  - 命令行参数全是死代码：`-h/-p/-l` 在 main 里解析进局部变量，但 `handle_connection_task` 连接目标用的是写死的宏 `TARGET_HOST/TARGET_PORT`（永远 `127.0.0.1:80`），`listener_task` 的监听/转发打印也用宏。改成全局 `g_target_host/g_target_port/g_listen_port`，main 解析后真正生效。
  - 验证：起一个"收到 EOF 才回显"的 echo 服务端当 target，客户端连 proxy 发 `hello` 后 `shutdown(WR)`，能正常收回 `hello`（半关闭不传播时会卡死）；同时确认 proxy 日志 `Listening on ...:18090 / Forwarding to 127.0.0.1:18080` 即参数真正生效

### 讨论过但没做（记录理由，免得下次重新推一遍）

- **UDP ASSOCIATE / BIND**：本次明确不做。做 UDP 需要一个绑定到本地端口的 relay socket（`mco_udp_socket()` 是 send-first 懒创建、且不能指定本地端口，不满足），而且单 socket 无法可靠区分"客户端发来的带头报文"和"目标服务器发来的裸报文"，得拆成 cli/tgt 两个 socket，tgt 还要按 v4/v6 各建一个（UDP socket 定型后不能混发）
- **splice 零拷贝转发**：技术上可行（`mco_wait_io` 能等 pipe fd，io_uring 后端有 POLL_ADD/POLL_REMOVE 就绪模拟层），但每方向多 2 个 fd、Linux-only、TLS 用不了，且**短连接小请求反而更慢**（syscall 数与 pipe 唤醒更多）。真要上就配 `test/bench_*` 拿数字对照，不凭感觉优化
- **把失败原因做成 errno 风格的隐式状态**（如 `mco_connect_last_error()`）而不是改签名：零破坏，但引入隐式状态，yield 期间可能被别的协程覆盖。选了显式出参
- **在 `mco_socket_t` 里缓存 local/peer 地址**：只省一次 ~1μs 的 syscall（这个 API 不在每包热路径上），代价是每 socket 多 ~220 字节（10 万连接 = 22MB），且 UDP 下 peer 会变、缓存会给出错误答案。真有热路径需求应该在应用层 accept 后取一次存着

## 阶段 19：本轮完成（HTTP 服务端 HTTPS 支持：mco_http_server_set_tls + smoke）

- [x] **TLS 服务端接入**：实例新增 `ssl_ctx` 字段 + `mco_http_server_set_tls(srv, ctx)`（对齐 miniconet.h 的 SSL 接入模型；ctx 须 `mco_ssl_ctx_new_server(crt,key,...)` 创建，挂 NULL 即明文，无 SSL 后端时退化为明文不静默混流）；`conn_handler` 开头做 `mco_ssl_server_handshake`，失败即关连接——放在连接协程内、走 `mco_wait_io`，不阻塞 serve_loop 的 accept 循环
- [x] **smoke**（`examples/http_https_server_smoke.c`，纳入 MINICONET_HTTP_PARSER_TESTS）：用 `test/certs/` 自签证书起 HTTPS 实例；正例 `mco_tls_connect`（关校验）握手后拿到 200 + body；负例纯明文打 HTTPS 端口在握手阶段被拒、拿不到 200。无 SSL 后端时优雅跳过（同 tls_echo_smoke 约定）
- [x] 验证：`-DMINICONET_WITH_OPENSSL=ON` 构建下 ctest 全绿（21/21，含新增 http_https_server_smoke）

## 阶段 18：本轮完成（HTTP 服务端实例化重构，支持多实例）

阶段 17 完成后复盘发现服务端是进程级单例：路由表/钩子/keep-alive 总闸/serve 状态全是 `static g_*` 全局，一个进程跑不了两套独立路由的 server。对齐 PhotonLibOS `net::http::Server` 的实例化模型做破坏性重构（无兼容层，调用方一次性迁移）：

- [x] **新 API**（全部挂 `mco_http_server_t*`，不透明结构定义在 .c 内）：
  `mco_http_server_new/free` + `mco_http_server_handle` + `mco_http_server_handle_ws` + `mco_http_server_serve_static` + `mco_http_server_set_not_found_handler/error_handler/keepalive` + `mco_http_server_serve/stop`
  旧→新映射：`mco_http_handle(m,p,h,ud)`→`mco_http_server_handle(srv,...)`、`mco_ws_handle`→`mco_http_server_handle_ws`、`mco_http_serve_static`→`mco_http_server_serve_static`、`mco_http_serve(l,loop)`→`mco_http_server_serve(srv,l,loop)`、`mco_http_serve_stop`→`mco_http_server_stop(srv)` 等
- [x] 全局状态搬进实例结构（routes/ws_routes/static_routes 内联数组 + 钩子 + keepalive + stop/serve_loop）；`srv` 指针沿 serve_loop→conn_arg_t→conn_handler→handle_one_request→route_find/ws_route_find/client_wants_close 全链路传递；静态路由上下文内嵌实例并带反向 `srv` 指针（404 回退取该实例的钩子）
- [x] 同一实例可对多个 listener 调 serve（多端口共享路由）；不同实例路由完全隔离（含 not-found 钩子）
- [x] 生命周期约定（头文件注明）：`mco_http_server_free` 前必须已 stop 且 loop 排空，否则在飞协程引用已释放内存
- [x] 冒烟测试新增"实例 2"（17383 端口）：验证 /other 只在实例 2 命中、/hello 在实例 2 是默认 404（非实例 1 的自定义页）、/other 在实例 1 是 404；常规 + ASan/UBSan/LSan 全绿，ctest 20/20

## 阶段 17：本轮完成（HTTP/1.1 服务端增强 tier-3 + WebSocket 服务端）

在阶段 13 的第 2 档 HTTP 服务端上增量增强，详细计划见 [httpserverplan.md](./httpserverplan.md)。明确不做 HTTP/2（与每连接一协程模型冲突）。

- [x] **keep-alive 连接复用**：`conn_handler` 外层改为请求循环，单连接顺序处理多请求；支持管道化（一次写回的字节流里夹多个请求，读缓冲续解析不丢字节）；`Connection: close`/HTTP/1.0 语义正确处理
- [x] **chunked 分块响应**：`mco_http_resp_set_chunked()`/`mco_http_resp_write_chunk()`，`write_response` 自动切 `Transfer-Encoding: chunked` 编码，结尾 `0\r\n\r\n`
- [x] **静态文件服务**：`mco_http_serve_static(prefix, root_dir)` 前缀路由；`..` 路径穿越防护（`realpath` 前缀校验）；内置 MIME 小表；小文件 Content-Length、大文件（>1MB 阈值）chunked 边读边写
- [x] **Range 请求（RFC 7233 单区间，补齐 206）**：`Range: bytes=S-E / S- / -N` 三种形式；有效区间回 206 + `Content-Range` + `Accept-Ranges: bytes`（大区间仍走 chunked 流式，不整块进内存）；起点越界/`bytes=-0`/空文件回 416 + `Content-Range: bytes */总长`；多区间（逗号）与畸形值按 RFC 允许整体 200 返回（`parse_range_header` 返回三态：忽略/区间/不可满足）
- [x] **错误页**：内置最小 HTML 404/500，`mco_http_set_not_found_handler()` 可覆盖
- [x] **WebSocket 服务端**：`mco_ws_accept(sock, key, leftover, len)` 完成 101 握手（承接 HTTP 层已读回的 leftover 字节，防首个 WS 帧丢失）；`mco_ws_handle(path, handler)` WS 路由表；HTTP 服务端检测 `Upgrade: websocket` 时分流，handler 内用 `mco_ws_recv/send` 通信
- [x] `examples/http_server_smoke.c` 覆盖：keep-alive 复用 + 管道化 / POST echo / multipart 上传 / chunked 客户端解码重组 / 静态文件 + MIME / 404 / WS echo；常规 + ASan/UBSan/LSan 构建全部通过
- **本轮修的两个真实 bug**：
  1. **协程栈溢出伪装成堆 UAF**：静态文件 handler 用了 64KB 栈数组，而协程默认栈只有 ~56KB，写穿栈底落进相邻（已回收的）协程栈内存，ASan 报 heap-use-after-free。修法：读缓冲改堆 `malloc`。教训：协程里忌讳大栈数组，和普通线程代码直觉不同
  2. **`ws_encode_key()` 输出未 NUL 终止**：`hv_base64_encode()` 只写 28 个编码字符不补 `\0`（上游 smtp.c 的用法是自己续写），而 `ws_encode_key` 的契约是 C 字符串——服务端 `snprintf("%s", accept)` 把未初始化栈垃圾拼进 101 响应、客户端 `strlen(expect)` 读越界，WS 握手时好时坏（取决于栈残留）。修法：`wsdef.c` 里显式 `accept[28]='\0'`；`mco_ws_connect` 的 `key_b64` 同款补终止
- [x] ASan 构建下协程栈池自动关闭（`MCO_VCPU_ASAN_BUILD` 检测，与 minicoro 的 fiber 标注宏同源；原因见阶段 6 排查记录），栈池 debug 日志已清理

## 阶段 16：本轮完成（build_and_test.sh 改为 CMake 薄封装）

`build_and_test.sh` 原本维护一份独立的核心源文件列表 + 自行 gcc 编译，和 `CMakeLists.txt` 的 `MINICONET_CORE_SOURCES` 是双份事实来源——每加一个源文件、一个 example、一个开关都要改两处（阶段 14/15 各付了一次同步税），且默认 profile 已悄悄漂移（脚本无条件编 http parser，CMake 默认 OFF）。

- [x] 脚本改成 `cmake -B build → cmake --build → ctest` 的薄封装：环境变量（`MINICONET_WITH_OPENSSL`/`ENABLE_UDS`/`WITH_WORKPOOL`/`WITH_HTTP_PARSER`/`ENABLE_SANITIZERS` + `MINICONET_CMAKE_ARGS` 透传）翻译成 `-D` 参数；`clean` 子命令先 `rm -rf build`
- [x] **单一事实来源归一**：源文件列表只在 `CMakeLists.txt`，脚本不再有 `CORE_SRCS`。新增源/开关零同步成本
- [x] **默认 profile 对齐 CMake**：`MINICONET_WITH_HTTP_PARSER` 默认 OFF，`websocket_smoke`/`http_client_smoke` 默认不构建（要跑显式开）。取代旧脚本"无条件编 http parser"的不一致行为
- [x] 增量编译：脚本不再每次 `rm -rf`，cmake 自行增量（要全量重编用 `./build_and_test.sh clean`）
- [x] README "方式二" 同步更新；旧脚本"不想装 CMake 时用"的定位改为"一键便捷封装"（脚本现在需要 cmake）
- [x] 验证：`MINICONET_WITH_WORKPOOL=1 ./build_and_test.sh` 通过；`./build_and_test.sh`（默认）通过

## 阶段 15：本轮完成（Layer 0 并发原语提取：oneshot / waitgroup）

把阶段 14 里分散的"自管道 + 引用计数 + wait"模式提取成通用原语，确立整体分层：Layer 0（并发原语）→ 上层（resolver / workpool / 协议客户端）。背景：`core/mco_resolver.c` 那套 pipe+refcount 逻辑被 TSan 抓出过真实数据竞争（漏掉唤醒后的 `read()`），值得只维护一份。

- [x] **`core/mco_oneshot.h/.c`** — 一次性完成信号（one-shot completion）。把 resolver 的 `pipe_fd[2]`+`refs`+`job_release`+`mco_wait_io`+唤醒后 `read()` 提取成 `mco_oneshot_new/retain/release/fire/wait`。
  - `new_with_cleanup(cleanup, ud)`：refs 归零时先调 `cleanup(ud)` 再 `close(pipe)+free`，让 oneshot 能 owning 关联状态（resolver 的 job）。`new()` 是 cleanup=NULL 的常见情况
  - `fire` 写 1 字节（任意线程）；`wait` 在协程内 `mco_wait_io` + `read()` 清空——这次 `read` 和 `fire` 的 `write` 配对构成 happens-before（旧版 resolver 漏掉 `read` 被 TSan 抓到的就是这点）
  - 语义：1 生产者对 1 消费者、消费者可提前放弃（超时）。和 waitgroup 语义不同（见下）
- [x] **`core/mco_waitgroup.h/.c`** — 等一批任务全部完成，built on oneshot。`hatomic_t pending` + `mco_oneshot_t* done_sig`；`done()` = `if (hatomic_dec(&pending)==1) oneshot_fire(done_sig)`；`wait()` = `oneshot_wait`。
  - **不用 oneshot 的引用计数那一半**：waitgroup 生命周期由调用方管理（new → add(N) → submit → wait → free），没有"消费者提前放弃、生产者还在跑"的场景
  - **已知限制（契约）**：`wait` 超时返回时可能仍有任务在飞、稍后会调 `done()`，此时不能直接 `free`（会和在飞 `done()` 竞争 `done_sig`）。调用方须先确保在飞任务退出。典型用法：wait 成功才 free；超时则整体判失败、不在此处 free（分段下载的 sidecar 保留路径）
  - 原子 dec 的 modification order + 最后一个 `done()` 的 fire write → `wait` 的 read 配对，构成 happens-before，让 wait 返回后能安全读到所有 worker 写的结果字段
- [x] **重构 `core/mco_resolver.c` 基于 `mco_oneshot`**（纯整理、行为不变）：job 去掉 `pipe_fd[2]`+`refs`，改成 `mco_oneshot_t* sig`（cleanup=`job_free_fn` 释放 job）；worker `ResolveAddr → oneshot_fire → oneshot_release`；`mco_resolve` `oneshot_wait → 超时 release 放弃 / 成功读结果后 release`。`make_pipe`/`job_release`/pipe 宏全部删除，pipe 逻辑只此一份（在 oneshot）
  - `examples/dns_async_smoke.c` 验证无回归：8/8 解析、40/40 ticks（异步未阻塞）、`mco_tcp_connect("localhost")` 端到端通过
- [x] **`core/mco_workpool.h` 文档更新**：`mco_workpool_async` 的结果回传从"调用方自己卷 pipe"改成推荐 `mco_waitgroup`（Add(N)→submit→Wait；fn 内 `done()`；结果按槽位写）。workpool 自身代码不变
- [x] **`examples/workpool_smoke.c` 重构**：去掉手卷 completion pipe，改用 `mco_waitgroup`（Add(8)→8×async→Wait）。实测 120ms、15 次连续运行 0 失败
- [x] **CMake**：`mco_oneshot.c` + `mco_waitgroup.c` 加入 `MINICONET_CORE_SOURCES` 始终编译（resolver 依赖 oneshot，必须常驻 core；waitgroup 跟随）。workpool 仍 gated 在 `MINICONET_WITH_WORKPOOL`。（`build_and_test.sh` 此时还是独立源列表——阶段 16 把它改成 cmake 薄封装，源列表同步税归零）
- [x] 默认 OFF 构建 + `dns_async_smoke` 通过，证明 oneshot/waitgroup 在无 workpool 时也工作
- **未做 TSan 自动化**：同阶段 14，沙箱 ASan 链接顺序问题影响全部测试。跨线程正确性靠设计保证（oneshot 内部 mutex 保护 refs + pipe read/write 配对；waitgroup 原子 dec + oneshot happens-before）+ 压力测试验证；正式环境建议手动跑一次 TSan（重点：resolver 的 job 生命周期、waitgroup 的 done/wait/free 顺序）

## 阶段 14：本轮完成（可选 WorkPool 多 vCPU 薄层）

给默认单 vcpu 模型补"真并行"能力。`MINICONET_WITH_WORKPOOL` 默认 OFF，不影响主路径。详细设计见 [PLAN-workpool-draft.md](./PLAN-workpool-draft.md)。

- [x] **`core/mco_workpool.h/.c`**：N 个 OS 线程各跑一个独立 vcpu（各有自己的 engine + 调度器 + 栈池），加一个 mutex 保护的共享任务队列 + 每 worker 一个 wake pipe。
  - `mco_workpool_new(n)` / `mco_workpool_free` / `mco_workpool_async(pool, fn, arg)`（fire-and-forget，任意线程可调）
  - dispatcher 协程常驻 `mco_wait_io(wake_pipe_r, MCO_EV_READ, -1)`；`async` 入队 + round-robin 选 worker + 写 1 字节唤醒；dispatcher 醒来先 `read()` 清空 pipe（建立 happens-before，和 [core/mco_resolver.c](./core/mco_resolver.c) 同款），再加锁把队列所有待办摘到本地、逐个 `mco_go` 派生协程
  - 为什么需要这层（而不是手动建线程 + `mco_vcpu_new`）：`mco_go()` 无锁，只能从"跑这个 vcpu 的线程"调用；跨线程往运行中的 vcpu 派活必须自己加锁队列 + 唤醒机制。workpool 就是封装这套样板。静态分片（任务在 run 前 `mco_go` 好、不跨线程派活）不需要 workpool，手动建线程即可
  - **约束（头注释里写明）**：socket share-nothing，不跨 vcpu 共享 `mco_socket_t*`；fn 里不做阻塞系统调用（会卡死整个 worker vcpu，阻塞调用走 resolver 那种专用线程池）；不做 `thread_migrate`（minicoro 协程中途跨线程迁移栈+调度状态复杂度太高、ROI 低）；结果回传不内置 future，调用方按 resolver 模式自己用 pipe 回传
  - `make_pipe` 抄一份 `static`（和 resolver 各一份，不冲突）；未做去重，符合"最小改动"
- [x] **CMakeLists.txt**：`option(MINICONET_WITH_WORKPOOL ...)` 默认 OFF；开时把 `core/mco_workpool.c` 追加进 `MINICONET_CORE_SOURCES`；`examples/workpool_smoke.c` 列入 `MINICONET_WORKPOOL_EXAMPLES` gated 列表（关时摘出通用循环、不构建，避免链接失败）
- [x] **build_and_test.sh**：支持 `MINICONET_WITH_WORKPOOL=1`；关时 `workpool_smoke.c` 在 examples 循环里 skip
- [x] `examples/workpool_smoke.c`：8 任务投到 4 worker，每任务做可校验 CPU 计算 + 纯 CPU 负载 + mco_sleep；主协程用 completion pipe（pipe write/read 配对做跨线程 happens-before）收齐 8 个完成字节。断言总耗时 < 250ms——刻意用 CPU bound 负载而非纯 sleep（mco_sleep 在单 vcpu 上也会被 timer 堆多路复用，区分不了单/多 vcpu；CPU bound 工作在单 vcpu 上必然串行）
  - 实测：~89ms（单 vcpu 串行约 230ms），20 次连续运行 0 失败，无 race/crash
- [x] 默认 OFF 构建 + ctest 确认 `workpool_smoke` 被正确排除、无符号膨胀
- **未做 TSan 自动化**：本沙箱 `MINICONET_ENABLE_SANITIZERS` 只挂 ASan+UBSan，且 ASan 在沙箱里有"runtime does not come first"的链接顺序问题（影响全部测试，非 workpool 引入）。workpool 的跨线程正确性靠设计保证（队列加锁 + pipe read/write 配对建立 happens-before）+ 20 次压力测试无 race 验证；正式环境建议手动跑一次 TSan

## 阶段 13：本轮完成（WebSocket + HTTP 协程化胶水层）

阶段 12 D 类清单的前两项——验证"parser + `mco_tcp_*` 组装成协程化协议客户端"这个模式。

- [x] **WebSocket 客户端**（`miniconet_websocket.h/.c`，CMake target `miniconet2_websocket`，链接 `miniconet2` + `miniconet2_http_parser`）：
  - `mco_ws_connect(host,port,path,timeout_ms)`：`mco_tcp_connect` + 手写 HTTP Upgrade 请求 + `picohttpparser` 解析响应头 + 用 `wsdef.c` 的 `ws_encode_key()` 算 `Sec-WebSocket-Accept` 做对端校验（不是只检查 101 状态码）
  - `mco_ws_send_text/binary`：内部只发单帧 FIN=1，不做分片；小帧走栈缓冲（≤512B），大帧才 malloc
  - `mco_ws_recv`：用 `websocket_parser` 解帧，自动应答 PING（回 PONG）、自动忽略 PONG、自动拼接 CONTINUATION 分片；只有 TEXT/BINARY 消息才返回给调用方，CLOSE 帧返回 `MCO_ERR_CLOSED`
  - **跨调用残留字节**：`mco_read` 一次读回的字节经常跨多个 WS 帧边界，`mco_ws_t` 内部维护 `pending_buf/pending_off/pending_len`，下次调用先消费残留再读新数据，否则未解析字节会永久丢失（这是流式协议解析的常见坑，注释里写明了）
  - 已知限制：`Sec-WebSocket-Key` 用 `rand()` 不是 CSPRNG——和 wsdef.h 帧掩码同一个标准，RFC 6455 本身只要求"看起来随机"，够用，已在代码注释里如实标注
- [x] **HTTP 客户端**（`miniconet_http.h/.c`，CMake target `miniconet2_http_client`，链接 `miniconet2` + `miniconet2_http_parser`）：
  - `mco_http_get/post(_ex)`：URL 解析复用 `compat/hbase.c` 里一直编译的 `hv_parse_url()`（不需要额外 vendor `hurl.c`），`https://` 自动走 `mco_tls_connect`，`http://` 走 `mco_tcp_connect`
  - 响应头用 `picohttpparser` 解析；body 三种策略：`Content-Length` / `Transfer-Encoding: chunked` / 读到 EOF（无长度指示时按需增长缓冲区）
  - 已知限制：**不处理 3xx 重定向**（响应体原样返回，不跟 Location）；响应体整个缓冲在内存里，不适合下载大文件。这两条在 `miniconet_http.h` 头注释里写明了
- [x] **`build_and_test.sh` 同步**：脚本现在无条件编译 `compat/http/*` + `miniconet_websocket.c` + `miniconet_http.c`（因为 `websocket_smoke` / `http_client_smoke` 硬依赖它们），不受 `MINICONET_WITH_HTTP_PARSER` 开关控制——和 CMake 默认（`MINICONET_BUILD_EXAMPLES=ON` 时两个 target 都构建）保持行为一致
- [x] `examples/websocket_smoke.c` / `examples/http_client_smoke.c` 端到端验证（见阶段 3 清单）
- [x] 全部开关同时打开 + ASan/UBSan 跑过，无内存/未定义行为报告

## 阶段 12：chv-master 对比后的补齐路线图（本轮确认，按顺序推进）

对照 `chv-master` 参考项目重新核实了一遍剩余没碰的部分，比之前粗略估计的更细：

### C 类：完全没碰、自成一体、零/低依赖，可以直接 vendor（已完成）
- [x] `http/httpdef.c/h`（HTTP 状态码/方法等纯定义，之前漏查了，其实不耦合 hio）
- [x] `http/wsdef.c/h`（WebSocket 帧结构定义，同上；实际编译时发现它需要 SHA1+base64 算 `Sec-WebSocket-Accept`，是真实的链接期依赖不是可有可无的 include，已经在 CMake 里让 `MINICONET_WITH_HTTP_PARSER` 链接 `miniconet2_crypto_utils`）
- [x] `event/kcp/ikcp.c/h`（KCP 协议核心算法本体，不是 `hkcp.c` 那层 hloop 封装），`MINICONET_WITH_KCP`
- [x] `util/md5.c/h`、`util/sha1.c/h`，`MINICONET_WITH_CRYPTO_UTILS`（`MINICONET_WITH_HTTP_PARSER`/`MINICONET_WITH_PROTOCOL_CLIENTS` 任一打开时也会自动一起编译，因为 `wsdef.c`/`smtp.c` 真的需要它）
- [x] `util/idna.c/h`，`MINICONET_WITH_IDNA`（国际化域名，目前只是 vendor 进来，`mco_resolve()` 还没接它）
- [x]（决定跳过）~~`tomlc99/toml.c/h`（TOML 配置解析器，纯 C 零依赖）~~ 用户明确表示不需要，没有 vendor
- [x] `wrapper/charry.c/h`（动态字节数组）+ `base/rbtree.c/h` + `wrapper/rbmap.c/h`（红黑树 map 封装），合并成一个 `MINICONET_WITH_WRAPPER_UTILS` 开关
- **踩坑记录**：`base64.c` 原来单独挂在 `MINICONET_WITH_PROTOCOL_CLIENTS` 底下，`wsdef.c` 也需要它，如果两个开关都开、`base64.c` 在两个静态库里各编译一份，会在最终可执行文件链接期报重复符号——重新组织成一个共享的 `miniconet2_crypto_utils` 静态库（md5+sha1+base64），`protocol`/`http_parser` 都链接它而不是各自编译一份

### D 类：需要按协程 API 重写胶水层（C 类做完后依次跟进）
- [x] **WebSocket 客户端**（先做这个，依赖链最短，用来验证"parser + mco_tcp_* 组装成协程化协议客户端"这个模式）——见阶段 13
- [x] HTTP **客户端**（`mco_tcp_*` + `httpdef.c` + 已 vendor 的 `picohttpparser.c`）——见阶段 13
- [x] HTTP **服务端**（协程 accept + 路由分发 + 优雅文件上传；`miniconet_http_server.c/.h`，参考 PhotonLibOS net/http 纯 C 重写，ASan+UBSan 通过 `examples/http_server_smoke.c`）
- [ ] MQTT client（`mco_tcp_*` + `mqtt_protocol.c`）
- [ ] UPnP 发现（`mco_udp_*` 做 SSDP + 已 vendor 的 `minixml.c`）
- [ ] KCP over UDP（`ikcp.c` + `mco_udp_*` 的协程封装；和之前讨论的 io_uring 完成端口模型一样，"以后单独封装"）
- [x] SOCKS5 代理（chv 只给了协议常量头文件 `protocol/socks5.h`，没有可抄的实现，得全新写）——见阶段 20（`examples/socks5_server.c`，只做 CONNECT + 无认证）

### E 类：需要重量级外部依赖，优先级放低，除非明确要用
- [ ] `quic/`（需要 ngtcp2 + nghttp3，本沙箱环境没有，工作量也最大）
- [ ] `wrapper/zlibutils.c`（需要 zlib，依赖不重，但目前没有模块用得到压缩，随缘）
- [ ] `wrapper/hpwd.c`（用途还没确认，价值待评估）

### F 类：明确不做
`vc/`（VC6 工程文件）、chv 自己的 `examples/`、`cert/`（我们有自己的一套）

## 阶段 11：本轮完成（参考 chv-master 项目补全 vendor 代码 + 编译开关最小化）

参考上传的 `chv-master`（miniconet2 的"上游血缘"项目）的目录结构和"默认全部关闭、按需开启"的编译哲学，做了这些事：

- [x] **补全完整的日志/线程安全队列子系统**：`compat/debug.c/h` + `compat/thrqueue.h` + `compat/thrqueue_cs.c`（替换掉之前因为缺 `thrqueue.h` 依赖而写的简化版 `LOGE` 垫片）；`compat/ssl/base/debug.h` 现在是个一行的转发头，指向真正 vendor 进来的实现，`compat/ssl/openssl.c` 本身没有被改动
- [x] **TLS 后端三选一 + 互斥校验**：新增 `cmake/SSLBackends.cmake`，`MINICONET_WITH_OPENSSL`/`MINICONET_WITH_MBEDTLS`/`MINICONET_WITH_GNUTLS` 默认全部 OFF（之前 OpenSSL 默认开，这轮改成和 chv 一致），同时开两个直接 `FATAL_ERROR`（chv 本身没做这个校验，容易在链接期才发现问题，这里补上了）。三个后端都过了真实握手 + 加密读写 + 证书校验的端到端测试：
  - **过程中在 vendor 代码里揪出两个真实 bug**：
    1. `compat/ssl/mbedtls.c` 的 `hssl_close()` 原来是空实现，不发 TLS 层 `close_notify`，导致对端只能看到连接被硬断开而不是"干净关闭"——补了 `mbedtls_ssl_close_notify()`，行为和 openssl.c 对齐
    2. `compat/ssl/gnutls.c` 的 `verify_peer=1` **原来完全不生效**——只影响"要不要加载系统信任链"，从没真正调用 `gnutls_session_set_verify_cert()`，也就是说打开这个选项之前只是心理安慰，任何证书（包括自签名）握手都会成功。这是安全相关的真实缺口，已修复（client 端；server 端双向 TLS 校验客户端证书还需要额外的 `gnutls_certificate_server_set_request()`，留了 TODO 注释，没做）
- [x] **`MINICONET_ENABLE_UDS`**：Unix Domain Socket 支持，`compat/hsocket.c` 里的实现代码本来就在，只是没接线；新增 `mco_tcp_listen_unix()`/`mco_tcp_connect_unix()`，和 `mco_tcp_connect()` 共用同一段"非阻塞 connect 收尾"逻辑（抽成 `finish_nonblocking_connect()` 避免重复代码）
  - **顺带修了一个真实的潜在越界读**：`hsocket.h` 的 `sockaddr_set_path()` 原来 `strncpy` 不保证以 `\0` 结尾（路径长度正好等于/超过 108 字节时），补了截断保护
- [x] **vendor 了三类 chv-master 的代码，但按"能不能直接编译"分开处理**：
  - `compat/protocol/{dns,ftp,smtp,icmp}.c/h`（+ `compat/herr.c/h`、`compat/util/base64.c/h`）：同步阻塞的协议客户端，`MINICONET_WITH_PROTOCOL_CLIENTS` 开关（默认 OFF），编译成独立的 `miniconet2_protocol` 静态库。**明确标注是阻塞实现，不要在协程里直接调用**
  - `compat/http/{picohttpparser,multipart_parser,websocket_parser}.c/h`、`compat/mqtt/mqtt_protocol.c/h`、`compat/miniupnp/minixml.c/h`：纯协议编解码，不碰 socket，各自一个开关（`MINICONET_WITH_HTTP_PARSER`/`MINICONET_WITH_MQTT_PROTOCOL`/`MINICONET_WITH_MINIUPNP`），各自一个独立静态库
  - chv 的 `http/`、`mqtt/mqtt_client.c`、`miniupnp/hv_upnp.c` 等深度耦合 `hloop_t`/`hio_t`（我们已经完全推翻重写的回调式反应堆 API）的部分，**这轮没有 vendor**——直接塞进来编不过，真要用得按协程 API 重写，是后续工作
- [x] **engine 后端按平台精确选源文件**（而不是六个全丢进去靠各自 `#if` 编译成空文件），减少无谓的编译器调用
- [x] `examples/uds_echo_smoke.c` 验证 UDS 收发；三个 TLS 后端各自过了 `tls_echo_smoke`/`tls_verify_smoke`/`tls_duplex_smoke`/`tls_cross_owner_smoke` 全套（14→15 个测试，测试数没变，只是 UDS 测试是新增的）
- [x] `build_and_test.sh` 同步支持所有新开关（环境变量形式，`MINICONET_WITH_OPENSSL=1`/`MINICONET_WITH_MBEDTLS=1`/`MINICONET_WITH_GNUTLS=1`/`MINICONET_ENABLE_UDS=1`），默认值和 CMake 保持一致（全部 OFF）
- [x] 全部开关同时打开 + ASan/UBSan 跑过一遍，零警告零内存问题

## 阶段 10：本轮完成（补上 SSL 重协商的已知边界情况，参考 Swoole 的处理方式）

- [x] 之前记录的已知限制——TLS 重协商期间读操作可能需要等 WRITE 方向，若此时另一个协程恰好在写同一个 socket，两者会争抢同一个 fd-WRITE 方向的引擎注册——已修复。做法参考 Swoole"协程读写分离"实现里对同一问题的处理：新增 `mco_socket_t::ssl_cross_owner`，握手期间、以及读写操作意外命中"跨方向等待"（读时要等写 / 写时要等读）期间，独占整个 SSL 对象，另一个方向的调用直接返回 `MCO_ERR_BUSY`，不会去并发碰同一个 `SSL*`
  - 顺带修了一个握手阶段本来就存在的漏洞：旧版握手期间只用 `rd_waiter` 挡住并发的 `mco_read()`，`mco_write()` 检测不到、能在握手完成前溜进来碰同一个 SSL 对象；现在和读写路径统一用 `ssl_cross_owner`，两个方向都挡住了
  - 正常场景（没有重协商、没在握手）完全不受影响，读写依然可以真正并发——`examples/tls_duplex_smoke.c` 专门验证这条（负向测试：证明新加的保护逻辑不会误伤正常情况）
  - **验证过程记录（诚实说明，别删）**：本想用 `SSL_renegotiate()` + `mco_write()` 真实触发一次重协商来验证 `MCO_ERR_BUSY` 确实生效，试了两版都没能稳定复现——第一次协商结果是 TLS 1.3（根本没有传统重协商机制），强制锁定 TLS 1.2 后 `SSL_renegotiate()` 成功标记了，但实测发现 OpenSSL 3.0 会把 HelloRequest 和后续应用数据一起塞进同一次 `SSL_write()` 发出去，并不会同步阻塞等对端握手响应完成——本机环回网络往返又快，观测下来这次写操作经常根本不会真正 yield，抓不到"另一个协程能撞进来"的窗口。放弃了对"真实触发一次重协商"的追求，改成用一个仅测试用的内部钩子（`mco_test_socket_set_ssl_cross_owner()`，需要 `-DMINICONET_TEST_HOOKS`）**确定性地**摆好"现在有另一个协程正独占 SSL 对象"的状态，直接测保护逻辑本身对不对，不依赖能不能撞上一次真实的重协商竞态——`examples/tls_cross_owner_smoke.c`
  - 用 ASan+UBSan 跑过，无内存/未定义行为报告

## 阶段 9：本轮完成（SSL / TLS）

- [x] **SSL 接入**：复用 `compat/ssl/hssl.h`（vendor 进来的 openssl/mbedtls/gnutls/wintls/appletls 统一抽象），CMake 默认自动探测系统 OpenSSL（`find_package(OpenSSL)`），找不到就优雅降级——`mco_ssl_ctx_new_client/server()` 返回 NULL，不会静默把 TLS 变成明文
  - 补全了 `hssl.h` 抽象里一个原本缺失的接口：`hssl_read()`/`hssl_write()` 的失败返回值本身不区分"要等读"还是"要等写"还是"真错误"（不像 `hssl_accept`/`hssl_connect` 内部已经做过这层翻译），加了 `hssl_get_error()` 补上，`openssl.c`/`nossl.c` 都实现了
  - `mco_read`/`mco_write` 对握手完成后的 TLS 连接透明生效——业务代码不需要区分明文/密文，同一套函数继续用；`HSSL_ERROR_ZERO_RETURN`（TLS 层的 close_notify）映射成 `mco_read` 返回 0，和明文 EOF 语义保持一致
  - 新增 `mco_ssl_ctx_new_client/server`、`mco_ssl_client_handshake`/`mco_ssl_server_handshake`、`mco_tls_connect` 便捷封装；握手过程完全走 `mco_wait_io`，不阻塞所在 vcpu
  - vendor 进来的 `compat/ssl/openssl.c` 依赖一个我们没有的 `base/debug.h`（背后是没 vendor 进来、且有缺失依赖的整套 hv 日志子系统），写了个几行的 `compat/ssl/base/debug.h` 垫片替代，只提供它实际用到的 `LOGE` 宏，没有改 `openssl.c` 一个字
  - `examples/tls_echo_smoke.c`：真实握手 + 加密读写 + close_notify→EOF 映射，**用 `tcpdump` 实际抓包验证过传输内容确实是密文**（对照组：同样方法跑明文 `tcp_echo_smoke` 确认能直接抓到明文，证明抓包方法本身有效，不是"抓包没生效所以什么都没看见"）
  - `examples/tls_verify_smoke.c`：验证 `verify_peer=1` 真的会拒绝自签名证书（不是摆设参数）
  - 两个 TLS 测试在检测不到真实 SSL 后端时优雅跳过（`HV_WITH_SSL` 检测），不会在没装 `libssl-dev` 的环境里报错
  - 用 ASan+UBSan 跑过，无内存/未定义行为报告
  - ~~已知边界情况：TLS 读操作在极少数情况下（重协商）可能需要等 WRITE 方向...~~ 已在阶段 10 修复，见下
  - 只支持 TCP 上的 TLS，不支持 UDP/DTLS

## 阶段 8：仍未开始 / 明确不做

- [x] **多读者/多写者排队**：现状是同一方向并发访问直接返回 `MCO_ERR_BUSY`。考虑过换成 FIFO 排队，但"排队排到的这条消息该怎么分"是协议语义问题（轮流分 vs 抢到算数），库这层强加策略容易和业务逻辑打架；且典型用法（一个连接对应一个读协程+一个写协程）根本用不到。因此现状：冲突就明确报错，比隐式排队更可预测

## 阶段 21：本轮完成（Windows IOCP 完成式后端）

把 Windows 的 IOCP 完成模式集成进库，参考 io_uring 后端的"就绪模拟 + 完成式扩展"两层结构。`MINICONET_WITH_IOCP` 开关（默认 OFF），开启后成为 Windows 默认后端（`mco_engine_default()` 选 iocp，否则 wepoll）。

- [x] **`core/mco_engine_iocp.c`** — 两层能力：
  - **就绪模拟**：用 `WSAPoll`（Vista+）实现标准 `ctl/poll/wake` 接口，供 `mco_wait_io` 的 TLS/UDP 等就绪路径使用。ctl 的注册身份校验（ud 凭证）与 wepoll/select 后端逐字对齐。
  - **完成式扩展（`ops->async`）**：`submit_read/write` 用 `WSARecv/WSASend`、`submit_connect` 用 `ConnectEx`（`WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER)` 惰性取函数指针）、`submit_accept` 用 `AcceptEx`（后端预创建 accept socket，完成时经 `res` 交回）、`submit_cancel` 用 `CancelIoEx`。完成包在 `poll()` 内同步派发。
  - **poll() 双机制**：先非阻塞 `GetQueuedCompletionStatusEx` 收割 IOCP 完成包，再按需 `WSAPoll` 就绪事件；有在飞 async op 时把 WSAPoll 超时压到 1ms，避免长时间阻塞在 WSAPoll 上漏掉 IOCP 完成包。`wake()` 双路唤醒（写 wake socket + `PostQueuedCompletionStatus`）。
- [x] **同步完成处理（`core/mco_vcpu.c/.h`）**：与 io_uring 的关键差异——`WSARecv/WSASend/ConnectEx/AcceptEx` 可能**同步完成**（返回 0/TRUE，不投递 IOCP）。若直接退回就绪路径会"数据已被同步读走、再 read() 却 EAGAIN"的重复读 bug。修法（已改）：`submit_*` 同步完成时在完成回调里记下结果，并在**返回的 cookie** 上置 `MCO_ASYNC_SYNC_DONE`（bit63，两个后端都不占用）；`async_wait_common` 检测到该位就不 yield 直接返回。早期实现是把该状态放在 `mco_task_t.async_sync_done` 字段上，会**跨 op 残留**（残留标志让下一个 op 不 yield 直接返回上一次的脏结果），已废弃该字段。纯加法，不影响 io_uring 的异步路径（`uring_async_smoke` 回归通过）。
- [x] **CMake**：`MINICONET_WITH_IOCP` 选项 + `core/mco_engine_iocp.c` 进 Windows 源列表 + PUBLIC 宏定义；`mco_engine_backends.h`/`mco_engine_default.c` 按宏切换选型。
- [x] **`test/iocp_async_smoke.c`**：与 `uring_async_smoke.c` 对齐的完成式冒烟（echo 64KB / 读超时后连接仍可用 / close 唤醒 / connect 失败不留下坏状态），gated 在 `MINICONET_WITH_IOCP`，未开启时退化为空测试。
- **验证状态（诚实说明）**：本沙箱为 Linux，无法真机跑 Windows。用最小 Windows 头桩（winsock2/mswsock/windows 的声明）对 `mco_engine_iocp.c`/`mco_engine_default.c`/`mco_engine_backends.h` 做了 `-D_WIN32 -DMINICONET_WITH_IOCP` 的编译级验证（`-fsyntax-only` 通过）；Linux 侧 `mco_vcpu.c` 改动经默认 + io_uring 两种构建 ctest 22/22 回归。**IOCP 后端本身未真机跑通**，同 wepoll 后端的标注。

## 已知限制（有意为之，不是遗漏）

实现细节/排查记录见对应阶段，这里只作速查。

- Windows 下 `int fd` 与 `SOCKET`（`uintptr_t`）的转换假设句柄值不超过 32 位，超出时显式报错
- IOCP 后端（`MINICONET_WITH_IOCP`）未真机跑通，仅编译级验证 + 代码走查（见阶段 21）；就绪路径用 WSAPoll、完成式路径用 IOCP，两套机制并存，有在飞 async op 时 WSAPoll 超时压到 1ms
- `mco_close()` 不释放 `mco_socket_t` 内存，需调用方在确认无协程再持有指针后调用 `mco_socket_destroy()`
- select 后端受 `FD_SETSIZE` 限制（POSIX 通常 1024），超过会显式拒绝注册
- 同一方向并发访问直接返回 `MCO_ERR_BUSY`，不排队（决策见阶段 8）
- 协程栈池化在 ASan/UBSan 构建下自动关闭，production 不受影响；"用 ASan 跑性能测试看不出池化效果"是预期行为（排查见阶段 6）
- `mco_udp_socket()` 的 IPv6 路径只有代码审查 + IPv4 回归级别验证过，未真机跑通（见阶段 7）
- TLS 默认不编译，三个后端互斥、配置期校验（见阶段 11）
- vendor 模块与协程 API 的集成状态：`compat/http/` 已通过 `miniconet_websocket.c`/`miniconet_http.c` 接入（阶段 13）；`compat/protocol/`（阻塞实现）、`compat/mqtt/`、`compat/miniupnp/` 能编译但不能在协程里安全使用，胶水层未写；`compat/kcp/`、`compat/util/idna.c` 仅 vendor 进来，`mco_resolve()` 也尚未接 idna（见阶段 12）
- workpool 是可选能力（`MINICONET_WITH_WORKPOOL` 默认 OFF）：socket 不跨 vcpu 共享、`mco_workpool_free` 放弃在飞任务、不做 `thread_migrate`、不内置 future（结果回传用 `mco_waitgroup`，见阶段 15）；阻塞系统调用不能放进 workpool 任务（会卡死 worker vcpu，走 resolver 线程池）（见阶段 14）
- `mco_waitgroup_free` 必须在 `wait` 返回且无在飞 `done()` 之后调用：`wait` 超时返回时可能仍有任务在飞、稍后会调 `done()`，此时直接 `free` 会和 `done()` 竞争 `done_sig`。超时路径应整体判失败、不在此处 free（见阶段 15）
