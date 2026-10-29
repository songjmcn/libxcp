# DAQ 跨进程用例诊断增强 实施记录

## 1. 背景

`tests/xcp_slave_demo_integration_test.cpp` 中的
`XcpSlaveDemoIntegrationTest.CrossProcessCalibrationChangesDaqOutput`
在 macOS CI 上出现过失败（提交 `0de843d` 红灯，纯文档提交 `c11f439`
重跑又变绿；`master` 的 `b9f49481` 上同样出现过一次），属于真实的不稳定用例。

失败时日志只有断言文本"10 ms DAQ DTO 未观察到标定偏置 42.0"，无法区分
以下三种完全不同的原因：

1. 一帧 DTO 都没收到（Slave 没发 / 主站没收到 / 端口选错）；
2. 帧收到了，但按用例假设的信封布局（relative-byte + 32 位时间戳 + 4 字节头）
   解码失败；
3. 帧解码成功，但数值确实不是 42.0（标定写入或 CalSeg 快照未生效）。

原实现中 `WaitForDouble()` / `WaitForNonzeroUint32()` 的
`catch (const XcpException&) {}` 把解码异常静默丢弃，且不保留任何帧现场，
因此上述三种情形在日志里完全等价。

本记录只做**诊断增强**：不改变用例行为、不放宽超时与断言阈值、不加重试、
不改动任何产品代码。

## 2. 改动清单

全部改动集中在 `tests/xcp_slave_demo_integration_test.cpp`：

| 位置 | 内容 |
| --- | --- |
| `:779` | 新增文件内辅助函数 `HexPreview(const Bytes&, std::size_t)`，把字节序列格式化为十六进制文本，超出 `limit` 的部分以 `…` 表示 |
| `:830`、`:866` | 两个等待函数在 `Decode()` 成功后累加 `decoded_frames_` |
| `:841`、`:877` | 原静默 `catch` 改为 `catch (const XcpException& error)`，累加 `decode_failures_` 并保存 `last_decode_error_ = error.what()` |
| `:893-921` | 新增只读访问器 `ReceivedFrameCount()`、`DecodedFrameCount()`、`DecodeFailureCount()`、`LastDecodeError()` |
| `:935-951` | 新增 `Diagnostics()`，拼装"已收帧数 / 解码成功 / 解码失败 / 最后错误 / 首帧长度与十六进制（最多 32 字节）/ 未收到任何 DTO 帧" |
| `:957-959` | 新增成员 `decoded_frames_`、`decode_failures_`、`last_decode_error_`（仍在同一把 `mutex_` 下读写） |
| `:982-1014` | 新增单元用例 `DemoDtoCollectorDiagnosticsCounters`，锁定统计语义 |
| `:1573-1579`、`:1593-1596` | 两处 DAQ 断言消息追加事件通道号与 `collector.Diagnostics()` |

设计要点：

- 统计仍由原有接收线程与等待线程在 `mutex_` 保护下写入，不引入新的并发面；
- 十六进制只打印**第一帧**且最多 32 字节，避免 100 Hz DAQ 把日志刷爆；
- `DecodedFrameCount()` 用于区分"解码成功但值不匹配"，是本次诊断能落地的关键；
- 断言仍然 `EXPECT_TRUE` / `ASSERT_NO_THROW` 原样失败，诊断只是把现场附在消息后面。

新的失败消息形如：

```text
10 ms DAQ DTO 未观察到标定偏置 42.0；事件通道 0，已收 DTO 37 帧，解码成功 37 次，
解码失败 0 次，首帧长度 12 字节，内容 00 00 00 00 00 00 00 00 00 00 45 40；
可能是相对基址、CalSeg 快照未生效或 DAQ 未启动
```

若首帧十六进制与预期布局不符（例如时间戳位置或头长度不同），日志会直接暴露；
若"已收 0 帧"，则问题在网络/端口而非解码。

## 3. 验证

环境：Windows 10 + MSVC 2022（`cmake-build-vs`，`Release`）。

### 3.1 构建

```bash
cmake --build cmake-build-vs --config Release --target XcpliteIntegration
```

结果：成功（仅有 glog 既有的 `C4244`/`C4996` 警告与
`xcp_slave_demo_integration_test.cpp(1542)` 既有的 `C4267` 警告，均非本次改动引入）。

### 3.2 定向测试

```bash
XcpliteIntegration.exe --gtest_filter='*DemoDtoCollectorDiagnosticsCounters:*CrossProcessCalibrationChangesDaqOutput'
```

结果：**2/2 通过**。其中

- `DemoDtoCollectorDiagnosticsCounters` 覆盖：空收集器的"未收到任何 DTO 帧"文案、
  合法帧的解码成功计数与"首帧长度"文案、截断帧的解码失败计数与非空错误文本；
- `CrossProcessCalibrationChangesDaqOutput` 在原路径下仍然通过，证明诊断改动
  没有改变用例行为。

### 3.3 全量回归

```bash
ctest --test-dir cmake-build-vs -C Release
```

结果：**1159 项中 1158 项执行，99% 通过；1 项失败**。

唯一失败项 `XcpMasterRenderBenchTest.RendersFrameWithFieldSizeCandidateList`
经核实是**本机算力导致的基准抖动，与本次改动无关**：

- 失败日志：`渲染 198556 行单帧 = 1517.0 ms（预算 参考=510 ms 硬上限=1500 ms）
  … 能力系数=8.35`，超出硬上限 1.1%；
- 单独复跑该项：`873.4 ms`，能力系数 5.44 > 容忍倍数 2.0，用例自行 `SKIPPED`；
- 该项是性能基准，测的是 `tools/xcp_master` 的渲染耗时，不涉及本文件任何代码。

跳过项（与本次改动无关，均为既有设计）：
`TcpStreamDecoderFinish.HeaderOnlyIsTruncated`（Disabled）、
`AgIntegration/...Ag1_Cto8`（Skipped）、
`SnapshotTest.StagesReadableCopyOfSource` / `CreatesDistinctFilesPerCall` /
`DeletesSnapshotWhenGuardReleased`（环境能力不足时 Skipped）。

## 4. 遗留与限制

1. **根因仍未定位**。本次只让下一次 macOS 失败变得可诊断，没有修复不稳定本身；
   需要等到下一次红灯带着新的诊断文本，才能判断是收不到帧、布局不符还是标定未生效。
2. Windows 上连续 10 次执行该用例全部通过（每次约 1.4 s），本地无法复现，
   因此不能在 Windows 上验证诊断路径的真实输出格式；格式正确性由
   `DemoDtoCollectorDiagnosticsCounters` 的字符串断言保证。
3. GitHub Actions 作业日志需要鉴权，本机无法直接读取那次 macOS 失败的详细输出；
   若后续仍无法从 CI 取到注解之外的上下文，需要改为在用例失败时上传日志工件。
