# 发布门批次 2：TCP 发送路径超时上界与 CTR 分配顺序

## 1. 背景与修复目标

批次 1（`c93477a`）解决了 Transport 生命周期与并发关闭类缺陷。批次 2 处理**发送路径**上
的 4 条发布门缺陷（编号沿用 `docs/review/` 下的对抗审查报告）：

| 编号 | 级别 | 现象 | 归属 |
| --- | --- | --- | --- |
| A-01 | P0 | `send_timeout_ms` 完全不生效：对端停止读取时 `Send()` 永久阻塞，且全程持有 `m_send_mutex_`，把同一 Transport 上的所有发送线程一起挂死 | **本分支回归**——master 的发送路径含 `ops.wait_writable(m_socket_->handle, m_config_.send_timeout_ms)`，重写时被删除 |
| A-03 | P1 | TCP `Send()` 先递增 `m_send_ctr_` 再做帧长校验，被本地拒绝的发送会在线上留下 CTR 缺口 | 本分支 |
| T-01 | P1 | UDP `Send()` 同构问题：先递增 CTR 再校验 `max_datagram_size` | master 已存在，本分支未修 |
| A-05 | P1 | 对端 FIN/RST 后 `Send()` 抛 `InvalidState`（"用法错误"），与"通道不可用应走恢复"的契约相反 | 本分支 |

## 2. 改动内容

### 2.1 A-01：发送路径改为全程非阻塞 + 单一 deadline

**根因**：`ConnectWithTimeout()` 与 `ListenAndAccept()` 在连接建立后主动调用
`ops.set_blocking(...)` 恢复阻塞模式，注释写的是"发送路径依赖可写等待 + 阻塞 send 的组合"。
阻塞模式下 `send()` 会一直睡到对端腾出接收窗口，`WriteAll()` 中 `is_would_block()` 分支永不
可达，`wait_writable()` 与 `MakeSendTimeoutError()` 全成死代码。

**改法**：

1. 删除两处 `set_blocking()` 调用，连接保持非阻塞（接收侧本来就按
   `wait_readable` + `kReceivePollSliceMs` 轮询，不依赖阻塞模式）。
2. `WriteAll()` 在函数入口计算**整帧共用**的 deadline：
   `steady_clock::now() + milliseconds(send_timeout_ms)`；每次 `is_would_block()` 只用
   "剩余时间"等待可写，而不是每次都重置成完整超时（否则慢速对端能把一次 `Send()` 拖成无限长）。
3. 剩余时间耗尽或等待超时 → 调用新增的 `AbortConnection("sending timed out")`
   （置 `m_running_` 并 `shutdown`，让接收线程退出、监听器收到一次关闭事件），再抛
   `MakeSendTimeoutError`。按 `m00801` 决策 2 采用"**发送超时后主动断连**"。

### 2.2 A-03 / T-01：先校验尺寸，后分配 CTR

TCP 侧把 `max_frame_packet_size` 校验移到 CTR 递增之前；UDP 侧先用"暂定 CTR"编码、
校验 `max_datagram_size`，全部通过后才 `m_send_ctr_.store(ctr + 1)`。语义变为
"**只有真正上线的帧才消耗 CTR**"，线上 CTR 序列因此无缺口。

### 2.3 A-05：断连后的 `Send()` 报 `TransportError`

`Send()` 在 `!IsOpen()` 时不再一律抛 `InvalidState`，而是先查生命周期状态：

- `m_state_ == Closed`（从未打开或已完全关闭）→ 仍抛 `InvalidState`（用法错误）；
- 曾经 `Open` 过但运行标志被置否（对端断开、发送超时断连、正在关闭）→ 抛
  `MakeTransportError("TCP 发送失败：通道已关闭…")`，调用方可据此走恢复流程。

锁序：`Send()` 已持 `m_send_mutex_`，此处再取 `m_lifecycle_mutex_`；`Close()` 路径不反向
取 `m_send_mutex_`，无锁环。

## 3. 改动文件

| 文件 | 改动 |
| --- | --- |
| `src/tcp_transport.cpp` | 补 `<chrono>`；删除 connect/accept 后的 `set_blocking`；新增 `AbortConnection()`；`WriteAll()` 改为 deadline 驱动；`Send()` 尺寸校验前移、错误分类；末尾"写出不完整"改抛 `TransportError` |
| `include/libxcp/tcp_transport.hpp` | 新增私有 `AbortConnection(const std::string&) noexcept` 声明 |
| `src/udp_transport.cpp` | `Send()` 改为编码校验通过后才消耗 CTR |
| `tests/tcp_transport_test.cpp` | 新增 3 条回归用例 |
| `tests/udp_transport_test.cpp` | 新增 1 条回归用例 |

## 4. 新增测试

| 用例 | 覆盖 | 判据 |
| --- | --- | --- |
| `TcpEndToEndTest.SendFailsWithinTimeoutWhenPeerStopsReading` | A-01 | 对端压低 `SO_RCVBUF` 且不读；`send_timeout_ms=300`；断言 `Send()` 在 10s 内失败、错误文本含"超时"、`IsOpen()` 变 false |
| `TcpEndToEndTest.RejectedOversizeFrameDoesNotConsumeSendCtr` | A-03 | 超长帧被拒后 `SendCtr()==0`；随后合法帧使 `SendCtr()==1` |
| `TcpEndToEndTest.SendAfterPeerDisconnectReportsTransportError` | A-05 | 对端关闭并等通道失效后，`Send()` 抛 `XcpException` 且 `Category()==TransportError` |
| `UdpTransportSend.RejectedOversizeDatagramDoesNotConsumeSendCtr` | T-01 | 超长 Datagram 被拒后 `SendCtr()==0`；合法帧使其为 1 |

所有 TCP 用例都遵循批次 1 的教训：`RecordingListener` 声明在 `TcpTransport` **之前**
（析构顺序与声明顺序相反，否则接收线程会回调已析构对象）。

## 5. 验证结果

| 项目 | 结果 |
| --- | --- |
| 构建（VS2022 Release，`cmake-build-vs`） | exit 0 |
| 新增 4 条用例 | 4/4 PASSED（超时用例 334 ms，与 300 ms 超时吻合） |
| `libxcp_tests.exe` 全量 | 823 tests：822 passed + 1 skipped（既有 `Ag1_Cto8`），45.6 s |
| `XcpliteIntegration.exe` | 61/61 PASSED，32.3 s |
| `XcpliteV12InteropTest.exe` | 7/7 PASSED |
| 变异验证（A-05） | 把 `if (ever_opened)` 改为 `if (false)` 后该用例 FAILED；还原后 PASSED ⇒ 用例能真实抓住缺陷 |

运行前必须设置 `TEMP`/`TMP`（Windows 反斜杠）：
`export TEMP='D:\project\libxcp\.tmp\ci_temp' TMP='D:\project\libxcp\.tmp\ci_temp'`。

### 5.1 Ubuntu 24.04（WSL2）复测

| 项目 | 结果 |
| --- | --- |
| 构建（GCC 13.3 + Ninja，Release） | 298 目标全部成功 |
| 4 条新增用例 | 4/4 PASSED；`SendFailsWithinTimeoutWhenPeerStopsReading` 用时 **300 ms**（精确命中 deadline） |
| `libxcp_tests` | 822 tests：821 passed + 1 skipped（同一条既有用例）、41.0 s |
| `XcpliteIntegration` | 61/61 PASSED、31.6 s |
| `XcpliteV12InteropTest` | 7/7 PASSED |
| ThreadSanitizer 全量（`halting_on_error=0`） | **822 tests 全跑完，0 条数据竞争警告**、41.2 s，进程退出码 0 |

Linux 构建有两个环境前置（**与本次改动无关的既有问题**）：

1. 必须加 `-DCMAKE_POSITION_INDEPENDENT_CODE=ON`，否则 `thirdparty/a2l-sdk/libliba2l.so` 链接失败
   （`relocation R_X86_64_PC32 ... recompile with -fPIC`）。
2. 必须 `export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH`：Boost 1.86 装在 `/usr/local/lib`
   且系统未执行 `ldconfig`，否则 `gtest_discover_tests` 在构建期执行测试二进制时报
   `libboost_thread.so.1.86.0: cannot open shared object file`。

## 6. 已知局限

1. `SendFailsWithinTimeoutWhenPeerStopsReading` 证明的是"**存在耗时上界**"，不能区分
   "整帧共用 deadline"与"每次等待都重置超时"这两种实现——构造后者失败所需的"对端稳定地
   每隔略小于超时时间腾出一小段窗口"在回环上难以确定复现。实现上已采用总 deadline。
2. A-01 的 `AbortConnection()` 只对 TCP 生效；UDP 无连接语义，不适用。
3. POSIX 实机验证已完成（Ubuntu 24.04 / GCC 13.3），三套件全绿；TSan 全量 0 警告。

## 7. 剩余任务

- 批次 3：A-02（UDP 一次 ICMP 永久杀接收线程）、M-03（对应的反向门禁用例）、T-02（decoder
  超限帧整链终止）——三者必须同批改，否则 `tests/udp_transport_test.cpp` 的
  `ConnectionLostStopsReceiveLoop` 会因语义变化变红。
- 批次 4：C-01/C-02（ACL 判定）、M-01（快照 oracle 自 SKIP）、M-02（静默对端误判"被拒"的
  诊断）、M-05（README 失效选项）。M-04（许可证）按 `m00801` 决策 4 跳过。
