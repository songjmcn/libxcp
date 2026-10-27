# Windows 平台 CTest 挂死修复 · 实施记录

- 日期：2026-10-27
- 分支：多平台编译与 ctest 验证分支
- 目标：解决 Windows 上 `ctest` 挂死（Linux/macOS 正常），让三平台套件回到绿色
- 涉及文件：
  - `src/tcp_transport.cpp`
  - `src/program_logging.cpp`
  - `tests/tcp_fake_socket.hpp`
  - `tests/tcp_transport_test.cpp`
  - `tests/xcp_slave_demo_integration_test.cpp`
  - `.github/workflows/msbuild.yml`

## 1. 现象

Windows CI 上的 `Test` 步骤在 `TcpEndToEndTest.MasterSendsAsamV14FrameWithoutTail`
用例处长时间无输出，最终被步骤超时截断，注解里只剩“步骤超时”，看不到具体用例名。
同一个用例在 Linux/macOS 上秒级通过。

## 2. 根因

`TcpTransport` 的接收线程直接阻塞在 `recv()` 上，`Close()` 依赖 `shutdown()` 打断它：

```cpp
// 旧实现（关键路径）
const int received = ops.recv(m_socket_->handle, buffer.data(), buffer.size());
...
(void)ops.shutdown_socket(m_socket_->handle);   // Close()：想用它唤醒 recv()
m_receive_thread_.join();                        //   ← 永久等待
```

Winsock 语义与 POSIX 不同：`shutdown(SD_BOTH)` 在 Windows 上**立即返回成功**，
但另一线程里已经挂起的阻塞 `recv()` 不会被唤醒（POSIX 上会立刻返回 0）。
于是 `Close()` 里 `join()` 永久等待，用例挂死；对端是否还有数据都不影响该结论。

定位证据（本地 Windows + gtest）：

- 打开临时埋点后单跑该用例：停在 `close:before-join`，一直没有 `close:after-join`；
- `shutdown` 返回 0（成功），但接收线程的 `loop:before-recv` 之后不再推进；
- 同一用例把接收线程改成“带超时等待可读”后，`Close()` 立刻返回（新回归用例 13 ms 通过）。

## 3. 修复方案（生产代码）

接收循环改为「先带超时等可读、再 recv」，退出延迟有明确上界，收包路径不变：

```cpp
/// @brief 接收线程每个轮询片等待可读的最长时间（毫秒）
constexpr std::uint32_t kReceivePollSliceMs = 100;

// ReceiveLoop()
const int ready = ops.wait_readable(m_socket_->handle, kReceivePollSliceMs);
if (ready == 0) { continue; }          // 轮询片超时：回 while 重判 m_running_
if (ready < 0) { ... }                 // 关闭 → break；EINTR → 重试；其余 → 记录原因后 break
const int received = ops.recv(m_socket_->handle, buffer.data(), buffer.size());
```

- 数据到达即刻唤醒，**不增加收包延迟**；只有“对端静默 + 正在关闭”时才会多等最多 100 ms。
- `Close()` 中 `shutdown()` 保留，但注释改为“让对端尽快观察到 FIN”，并明确它**不是**
  接收线程的退出手段（避免后人再把退出正确性押在 `shutdown` 上）。
- 未提升为公共配置项：该值只影响关闭延迟，与 UDP 传输层 `receive_poll_interval_ms`
  的兜底语义一致，不扩大公共接口。
- 同一提交内删除了定位用的临时埋点（`DiagTrace()`、`LIBXCP_TCP_TRACE`）。

## 4. 测试改动

- 新增回归用例 `TcpEndToEndTest.CloseReturnsPromptlyWhilePeerIsSilent`
  （`tests/tcp_transport_test.cpp`）：对端静默时 `Close()` 必须在 500 ms 内返回，
  并断言 `ElapsedMs() < 500`、错误码为 `Timeout`。该用例在旧实现上会挂死，修复后 13 ms 通过。
- 删除 `tests/tcp_transport_test.cpp` 中的 `Trace()` 埋点（`LIBXCP_TEST_TRACE`）。
- `tests/tcp_fake_socket.hpp`：`FakeSocketOps::wait_readable()` 改为“有待消费的 recv 脚本时
  视为立即可读”，否则脚本注入的错误条件永远等不到 `recv()` 被调用
  （接收循环现在先等可读）。原实现直接转发真实 Socket，导致
  `TcpFakeSocketTest.ReceiveReportsConnectionReset` 超时失败。

## 5. CI 改动

`.github/workflows/msbuild.yml`：

- 删除一次性诊断步骤 `Diagnose TCP end-to-end failures`（定位完成，已无价值）。
- 保留测试看门狗与环境变量 `LIBXCP_TEST_WATCHDOG_SECONDS`：看门狗是长期设施，
  用例卡死时能打出用例名而不是只留“步骤超时”。

## 6. 验证结果（本地 Windows，Release）

- 全量套件：`ctest -C Release` → **834/834 通过**，总耗时 122.89 s（无超时、无看门狗触发）。
- TCP 相关：`tcp_transport_test` 21 个用例全部通过（含新增用例）；
  `TcpFakeSocketTest` 5/5 通过。
- 说明：本机在受限沙箱下会额外出现 15 个 A2L 用例失败，原因是 `%TEMP%` 下
  `libxcp_calibration_access` 目录创建被拒（`create_directories: Access is denied`）。
  把 `TMP`/`TEMP` 指向工作区内目录后这 15 个用例全部通过，确认与本改动无关，
  属于本机沙箱限制；GitHub 托管 runner 的 `%TEMP%` 可写，不受影响。

## 7. 遗留观察（未改动）

- 发送侧 `wait_writable()` 仍是阻塞等待：对端不读时 `Send()` 会等到
  `send_timeout_ms`（默认 1000 ms）而不是被关闭打断。目前没有任何用例依赖该路径，
  为控制本次改动范围没动它；若后续出现同类挂死，应优先排查这里。

## 8. 追加修复：两个 Windows 专有的 `XcpSlaveDemoIntegrationTest` 失败

在 CI 同参数的 Visual Studio 生成器构建（`-G "Visual Studio 17 2022" -A x64`，
`LIBXCP_ENABLE_LOG=ON`）下全量跑 ctest，除第 3–6 节已修的 TCP 挂死外，还剩两个
**只在 Windows 上失败**的用例（Linux/macOS 通过）。

### 8.1 `GenerateA2lWithoutBindingPortAndCheckOverwrite`：打开的文件句柄阻止覆盖

失败断言：`tests/xcp_slave_demo_integration_test.cpp` 中 `EXPECT_EQ(*overwrite_exit, 0)`
实际为 2，子进程输出 `无法替换输出文件：...\a2l\xcp_slave_demo.a2l`。

根因（用例侧 bug）：测试在调用 `--overwrite` 之前仍持有两个 `std::ifstream`
（读原文与读“未被改动”内容），产品侧 `examples/xcp_slave_demo/main.cpp` 用
`std::filesystem::exists() && !std::filesystem::remove()` 覆盖旧文件；Windows 上
文件被本进程以读取方式打开时 `remove` 会因共享冲突失败（`The process cannot
access the file because it is being used by another process.`），而 POSIX 允许
unlink 已打开的文件，所以只有 Windows 红。已用最小探针（`cl /std:c++20`）
实测确认：句柄打开时 `removed=0`，关闭后 `removed=1`。

修法：把两次读取各自收进作用域，读完立即关闭句柄（用例注释说明原因），
产品侧逻辑不动——生成器拒绝覆盖被占用文件是合理行为。

### 8.2 `ServiceEmitsPeriodicHeartbeatLog`：强杀 + glog 30 s 缓冲丢心跳

失败断言：等待 9 s 后日志里找不到 `从站心跳`，实际日志只有启动行。

根因（产品侧可观测性缺陷）：心跳本身已按墙钟每 5 s 触发
（`examples/xcp_slave_demo/main.cpp` 的 `next_heartbeat`）。但 glog 默认
`FLAGS_logbufsecs=30`，日志在内存里攒够 30 s 才落盘；测试的
`DemoChildProcess::Stop()` 在 Windows 上用 `TerminateProcess` 硬杀（POSIX 分支
才发 `SIGTERM` 走正常退出路径），`ShutdownGoogleLogging()` 不会执行，
5 s 处那条心跳随缓冲一起丢失。手工观测确认：进程存活 8 s 时日志文件只有 331 字节、
仅含启动行，心跳确实不在文件里。

修法：`src/program_logging.cpp` 在 `InitGoogleLogging` 之后设 `FLAGS_logbufsecs = 0`
（每条日志立即冲刷）。理由不仅是让用例通过——文件是本项目唯一日志通道
（`FLAGS_stderrthreshold` 已高于 FATAL），沿用 30 s 缓冲会让进程崩溃或被强杀时
丢掉整段诊断信息，心跳这种周期性状态日志也无法实时观察；本项目日志频率很低
（心跳 5 s 一条 + 边沿事件），逐条冲刷的代价可忽略。

## 9. 验证结果（CI 同参数：Visual Studio 2022 x64，Release）

- 配置与构建：`cmake -B cmake-build-vs -S . -G "Visual Studio 17 2022" -A x64`
  （其余参数照抄 `msbuild.yml`）→ `cfg_exit=0`；`cmake --build cmake-build-vs --config Release`
  → `build_exit=0`。
- 全量 ctest：`ctest -C Release --output-on-failure` → **1047/1048 通过**，总耗时 257.40 s。
- 唯一失败：`XcpMasterRenderBenchTest.RendersFrameWithFieldSizeCandidateList`
  （渲染 198556 行单帧 780–805 ms，超过 3× 参考预算 510 ms）。该套件是**机器性能门禁**，
  与本次改动无关（该用例不初始化日志、也不走传输层），且 `msbuild.yml` 的 ctest 已用
  `-E "XcpMasterRenderBenchTest|a2l_ui_perf_gen|a2l_perf_gen|ParseLargeFileUnderBudget"`
  排除；同一台 ThinkPad 上重复 3 次都是 ~780 ms，属本机算力低于参考机，不是平台差异。
- 定向复跑：`XcpSlaveDemoIntegrationTest.GenerateA2lWithoutBindingPortAndCheckOverwrite`
  972 ms 通过、`ServiceEmitsPeriodicHeartbeatLog` 9031 ms 通过。
- 环境说明：本机若把 `TMP`/`TEMP` 留在系统 `%TEMP%`，会有 15 个 A2L 用例因工作区外
  写入受限而失败（见第 6 节结论）；把 `TMP`/`TEMP` 指到工作区内目录后全绿，
  GitHub 托管 runner 不受此限制。