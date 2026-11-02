# 发布门批次 1：Transport 生命周期线程安全修复记录

## 1. 背景

对抗式发布前审查（`docs/review/`，基线 `40c4ee9`）发现 5 个 P0 缺陷，其中 5 条全部属于
Transport 的生命周期/回调重入问题，且现有 811 个单测对其零覆盖：

| 编号 | 结论 |
|---|---|
| B-01 | UDP 接收线程持 `m_recv_mutex_` 调 `OnTransportWarning`；回调内 `LastReceiveCtr()` 二次加同一把锁，异常穿过 `noexcept` 边界 → `std::terminate`（10/10 `0xC0000409`） |
| B-02 | `Send()` 只在锁内查 `IsOpen()`，之后无同步解引用 `m_socket_`；`Close()` 不取发送锁就 `reset()` → 访问违例（含 A-06/A-07） |
| B-03 | 两个线程并发 `Close()`：双 join + 双向 close 同一句柄 + 双 reset → 段错误（10/10 `0xC0000005`） |
| B-04 | 头文件承诺"可从监听器回调内 `Close()`"，实际是接收线程 join 自己：terminate 或只完成一半清理（端口 10048 仍被占用） |
| B-06 | `Open()` 中接收线程创建失败时未释放唤醒对，两个回环 Socket 泄漏 |

本批次只处理上述生命周期问题，不改变发送/接收的协议语义（那是批次 2/3）。

## 2. 改法

### 2.1 统一的关闭状态机（TCP 与 UDP 同构）

`Close()` 原先"置标记 → shutdown/唤醒 → join → 释放"是无保护顺序执行的，任何一步被
并发进入都会造成双 join 或对已释放对象解引用。改为显式状态机：

```
enum class LifecycleState { Closed, Open, Closing };
```

- `m_lifecycle_mutex_` 保护状态与"收尾认领"标志 `m_finalize_owned_`；
- `m_receive_tid_` 记录接收线程身份，`IsReceiveThread()` 据此识别"回调内关闭"；
- `Close()` 只有三条出口：
  1. 状态已是 `Closed` → 幂等返回；
  2. 由接收线程在回调内调用 → 只置 `Closing`、发出关闭请求后**立即返回**（不 join 自己），
     收尾延后到析构函数或后续任意线程的 `Close()`；
  3. 由其它线程调用 → 认领收尾（`m_finalize_owned_ = true`），释放锁后 join，再重新加锁
     释放资源并置 `Closed`；
- 若另一个线程已在 `Closing` 且收尾已被认领，则用 `m_close_cv_` 等待其完成，**绝不第二次
  join 同一线程**；
- `join()` 必须在锁外执行：接收线程可能正在等这把锁（它自己也可能调 `Close()`）。

### 2.2 句柄改用 `shared_ptr` 快照

`m_socket_` 由 `std::unique_ptr` 改为 `std::shared_ptr`。`Send()` / `WriteAll()` / `ReceiveLoop()`
在入口取一份快照，此后只通过快照访问 `handle`：

- `Close()` 的 `ReleaseResources()` 会先把 `handle` 置为无效再 `reset()`，因此即使发送线程
  仍持有快照，也不会出现"关闭后句柄号被复用、被二次 close"的串扰；
- 发送路径不必与 `Close()` 争用同一把锁，也就不会出现"持发送锁 join 接收线程"导致的死锁。

### 2.3 告警回调移出接收锁

`HandleFrame()` 改为"锁内只判定并拼装诊断文本，锁外调用 `NotifyWarning()`"：

- 监听器在告警回调中重入 `LastReceiveCtr()` 不再二次加同一把非递归锁；
- 新增 `NotifyWarning()` 统一包裹 `try/catch`，告警通道抛异常不会终止接收线程（B-05）。

### 2.4 其它

- 接收线程创建失败分支补 `ReleaseWakeupPair()`，消除唤醒对泄漏（B-06）；
- 析构函数在 `Close()` 返回后检查状态，若关闭是由接收线程回调内发起的，则 join 后补做一次
  `ReleaseResources()`，保证句柄不泄漏。

## 3. 改动文件

| 文件 | 说明 |
|---|---|
| `include/libxcp/tcp_transport.hpp` | 新增 `LifecycleState`、`IsReceiveThread()`、`SignalShutdown()`、`ReleaseResources()`、生命周期成员；`m_socket_` 改 `shared_ptr`；更新 `Close()` 契约文档 |
| `src/tcp_transport.cpp` | `Close()` 状态机重写、析构收尾、`Open()` 状态登记、线程入口登记 tid、`WriteAll()`/`ReceiveLoop()` 句柄快照 |
| `include/libxcp/udp_transport.hpp` | 同构新增状态机成员与 `NotifyWarning()`、`ReleaseWakeupPair()`；`m_socket_` 改 `shared_ptr` |
| `src/udp_transport.cpp` | `Close()` 状态机重写、析构收尾、`Open()` 状态登记与唤醒对释放、`HandleFrame()` 锁外告警、`Send()` 句柄快照 |
| `tests/udp_transport_test.cpp` | 新增 4 个回归用例 |
| `tests/tcp_transport_test.cpp` | 新增 3 个回归用例 |

## 4. 新增测试

| 用例 | 覆盖缺陷 | 断言要点 |
|---|---|---|
| `UdpTransportLifecycle.ConcurrentCloseFromTwoThreadsIsSafe` | B-03 | 两线程同时 `Close()` 不崩溃，`OnTransportClosed` 仍交付一次 |
| `UdpTransportLifecycle.CloseFromPacketCallbackReturnsImmediately` | B-04 | 收包回调内 `Close()` 立即返回，接收线程正常退出并交付关闭回调 |
| `UdpTransportLifecycle.WarningCallbackMayReadLastReceiveCtr` | B-01 | 告警回调内重入 `LastReceiveCtr()` 不死锁、不 terminate |
| `UdpTransportLifecycle.SendRacingCloseDoesNotCrash` | B-02 | 发送线程与 `Close()` 并发不崩溃 |
| `TcpEndToEndTest.ConcurrentCloseFromTwoThreadsIsSafe` | B-03 | 同上（TCP） |
| `TcpEndToEndTest.CloseFromPacketCallbackReturnsImmediately` | B-04 | 同上（TCP） |
| `TcpEndToEndTest.SendRacingCloseDoesNotCrash` | B-02 | 同上（TCP） |

## 5. 验证结果

构建：`cmake --build cmake-build-vs --config Release -j 8`（VS2022，Release）→ exit 0。

| 套件 | 结果 |
|---|---|
| `libxcp_tests`（含新增 7 条） | 全量通过（exit 0；仅 1 条既有 SKIP、1 条既有 DISABLED） |
| `XcpliteIntegration` | 61/61 通过（31.7 s，含 DAQ 帧率门禁） |
| `XcpliteV12InteropTest` | 7/7 通过 |

运行环境必须显式指定可写临时目录，且**必须用 Windows 反斜杠形式**
`TEMP='D:\project\libxcp\.tmp\ci_temp' TMP='D:\project\libxcp\.tmp\ci_temp'`：正斜杠形式会被
`GetTempPath` 忽略并回退到 `C:\Users\<user>\AppData\Local\Temp`，从而因
`create_directories: Access is denied` 造成 11 条集成用例假失败（本机环境问题，与被测代码无关）。

## 6. 未覆盖项（后续批次）

- 批次 2：A-01（TCP 发送超时失效导致永久挂死）、A-03/T-01（CTR 在发送被拒时跳号）、A-05；
- 批次 3：A-02（UDP 一次 ICMP 永久杀接收线程）、T-02（超限帧令整条 TCP 流终止）、M-03（反向门禁用例）；
- 批次 4：C-01/C-02（ACL 判定）、M-01（快照 oracle 零检出）、M-02（"全部被拒"诊断）。
- 未做：TSan 专项目标（MSVC 不支持 ThreadSanitizer，需在 Linux/macOS 的 CI 上启用）。
