# XCP 1.3.0 实施记录 — 批次 2（阶段 3~7：UDP Transport + 命令执行 + 内存读取 + 端到端）

> 依据文档：`code-plan/XCP_1.3.0_详细设计_类接口与头文件.md`（第 6~8、12~14、15.2、15.3 节）、
> `code-plan/XCP_1.3.0_最小协议核心实现计划.md`（阶段 3~7、§9.3~§9.6）。
>
> 承接批次 1（阶段 0~2，commit `155d8ae`）。本批次完成后，计划文档 §2.1 的"包含范围"
> 全部落地，§10 验收标准 1~15 条均有对应实现与测试。

---

## 1. 本次新增/修改文件

### 1.1 公共头文件（`include/libxcp/`）

| 文件 | 设计章节 | 内容 |
|---|---|---|
| `udp_header_codec.hpp` | §6 | `kUdpHeaderSize`/`kUdpMaxDatagramSize`/`kUdpMaxXcpPacket`、`UdpFrame`、`UdpHeader`、`UdpFrameView`、`encodeUdpFrame`、`decodeUdpDatagram` |
| `udp_transport_config.hpp` | §7 | `UdpTransportConfig`（远端/本地端点、轮询间隔、双重长度上限、`strictRemotePort`） |
| `udp_transport.hpp` | §8 | `UdpTransport : IXcpTransport`，含 §8.1 九条接收 CTR 策略文档 |
| `command_executor.hpp` | §12 | `CommandTimeouts`、`IEventListener`、`CommandExecutor : IPacketListener`，含死锁规避说明 |
| `memory_access.hpp` | §13 | `MemoryAccess`：SHORT_UPLOAD 优先 / SET_MTA+UPLOAD 分块降级 |
| `xcp_master.hpp` | §14 | `XcpMaster` 顶层门面 |

### 1.2 实现（`src/`）

`udp_header_codec.cpp`、`udp_transport.cpp`、`command_executor.cpp`、`memory_access.cpp`、`xcp_master.cpp`

### 1.3 测试设施与测试（`tests/`）

| 文件 | 覆盖 |
|---|---|
| `udp_test_slave.hpp/.cpp` | 设计 §15.2：Loopback 测试 Slave，最小命令集 + XCP 1.1 端点绑定（D8）+ 故障注入 + `sendPackedFrames` / `sendRawPayload(To)` |
| `udp_header_codec_test.cpp` | 黄金向量、LEN/CTR 固定小端、多 Frame 解析、LEN 越界/残留/截断整体拒绝 |
| `udp_transport_test.cpp` | 生命周期、配置校验、绑定失败、发送长度上限、多 Frame 交付、CTR 缺口/重复/回绕/后向乱序、来源 IP/端口过滤、关闭与取消 |
| `udp_test_slave_test.cpp` | Slave 自身语义：未连接忽略、CONNECT 应答端点、同 IP 异端口、最小命令、故障注入计数 |
| `memory_access_test.cpp` | AG=1/2/4 参数化分块、SHORT_UPLOAD 降级、地址溢出、响应长度校验、超时恢复重发 SET_MTA、Motorola 地址字节序 |
| `recovery_test.cpp` | 超时→SYNCH→重试、重试耗尽、SYNCH 失败、Transport 断开禁 SYNCH、EV_CMD_PENDING、EV/SERV/DTO 分流、错误分派表、CONNECT/DISCONNECT 流程 |
| `xcp_master_integration_test.cpp` | 计划 §9.5 全部 7 条（Mock 端到端） |
| `xcp_udp_loopback_test.cpp` | 计划 §9.6 全部 8 条（真实 UDP Loopback 端到端） |

### 1.4 构建

- `CMakeLists.txt`：加入 5 个实现文件；**新增 `ws2_32` 链接**（Windows Socket 必需）
- `tests/CMakeLists.txt`：加入 8 个新测试文件与 `udp_test_slave.cpp`

---

## 2. 实施中发现并修复的真实缺陷（非测试问题）

| # | 缺陷 | 影响 | 修复 |
|---|---|---|---|
| 1 | **CONNECT 响应的 MAX_DTO 用了协商前的 Intel 字节序解析** | Motorola 会话下 `maxDto` 解析错误，可能导致校验失败或长度上限错误 | 先用与字节序无关的 `COMM_MODE_BASIC`（单字节）判定 Session 字节序，再用该字节序解析 CONNECT 响应（设计 §17.2 要求"按 Session Byte Order"） |
| 2 | **UPLOAD 超时恢复重试前未重发 SET_MTA** | 违反计划 §6.2 第 3 条；恢复后 Slave 的 MTA 可能已变，重试会读到错误地址 | `CommandExecutor` 记录最近一次成功的 SET_MTA（`m_last_mta_`），UPLOAD 重试前自动重建；新会话/断连时清空 |
| 3 | **EV_CMD_PENDING 未上报给 IEventListener** | 违反计划 §6.3"Event 仍上报" | 重启计时标记与事件上报解耦，两者都执行 |
| 4 | **未知错误码在异常文本中按十进制输出** | 计划 §6.4 要求"保留原值并上报"，十进制 `0x153` 误导诊断 | 新增十六进制格式化，命令码与未知错误码统一 `0xNN` 形式 |
| 5 | **地址边界判定过严（off-by-one）** | 恰好读到 `0xFFFFFFFF` 的合法请求被误判为溢出；分块最后一块的地址推进同样误判 | `validateRead` 改用 64 位 exclusive-end 判定（允许等于 2^32）；分块循环在 `remaining == 0` 时不再推进地址 |
| 6 | **重复包判定分支不可达** | `distance == 0` 分支在判等之后永不可达，真实"重复 CTR"落入后向分支（行为恰好正确，但代码与注释不符） | 统一改为"以期望值为基准的前向距离"判定：`0x8000` 歧义、`1..0x7FFF` 前跳、其余为重复/后向 |
| 7 | **`CommandExecutor::performAttempt` 为死代码** | `runCommand` 内联了同样逻辑，重复且 Pending 清理路径不一致 | `performAttempt` 返回 `optional`（nullopt = 超时/关闭）并统一清理 Pending，`runCommand` 复用之 |
| 8 | **响应槽位被占用时新 RES/ERR 会覆盖前一条响应** | 协议失步时可能返回错误数据 | 槽位已占用则丢弃并上报"疑似协议失步"诊断，绝不覆盖 |
| 9 | **`performRecovery` 无锁读取 `m_transport_close_reason_`** | 数据竞争 | 加锁读取 |

> 缺陷 1~4 会直接导致协议行为错误；5~9 为健壮性/正确性收口。全部由测试驱动发现。

---

## 3. 测试夹具（非产品代码）修正记录

这些是**测试自身写错**，产品实现本身符合规范，但过程中修正了测试夹具对协议的错误理解：

| 现象 | 根因 | 处置 |
|---|---|---|
| UPLOAD/SHORT_UPLOAD 响应长度校验全部失败 | 三个测试 Slave 夹具都错误地在 RES 中加了"元素计数字节"。**规范布局是 `[FF][data...]`，长度恰为 `elements*AG`** | 依据 `docs/XCP_1.3.0_document.md` §12.4（`UPLOAD(6)` → 6 字节 `"XCPSIM"`）与 §12.5（`size=4` → 4 字节）修正三个夹具及全部长度断言 |
| `std::terminate`（exit 3）中断整个测试进程 | 测试抛异常时 joinable `std::thread` 析构 | 新增 `ThreadJoiner` 作用域守卫，异常路径也能 join |
| 多处故障注入用例等不到警告 | `setFaultInjection()` 会重置响应计数（该语义已由 `ResponseCounterStartsFreshAfterEachInjection` 锁定），用例却按"累计序号"写 N=2 | 统一改为 N=1 |
| 重复 CTR 用例实际被接受 | 夹具回退了发送计数器，导致本响应 CTR 恰等于期望值 | 改为"本响应复用上一响应的 CTR"，计数器继续推进 |
| 注入畸形 Datagram 用例产生 `ADD_FAILURE` | 试图让原始 Socket 绑定 Slave 端口以通过来源过滤，端口被占用必然失败 | 新增 `UdpTestSlave::sendRawPayloadTo()`，从 Slave 自身 Socket 发出（源端口天然正确） |
| 来源过滤两阶段用例第二段失败 | 两个 `RawSender` 同时存活并争抢同一源端口 | 分段作用域 + 不同源端口 |
| `DestructorDisconnectsAndReleasesResources` 访问违例 | `master.reset()` 后 `transport_ptr` 悬空（Transport 由 Master 拥有） | 改用仍存活的 MockSlave 断言 DISCONNECT 计数 |
| `UnrecoverableTimeout` 未触发恢复 | 请求 4 元素 ≤ MAX_CTO/AG，实际走了 SHORT_UPLOAD | 改为 20 元素强制走 UPLOAD |
| 辅助函数与结构体同名导致编译错误 | 匿名命名空间的 `Bytes ConnectResponse(...)` 遮蔽了 `calmcar::xcp::ConnectResponse` | 重命名为 `MakeConnectResponse` |
| 构造函数内用 `ASSERT_*` 编译失败 | GTest 断言宏要求 `void` 返回 | 改为 `valid()` 标志位 |

---

## 4. 规范校核

| 项目 | 依据 | 结论 |
|---|---|---|
| UDP Header = `LEN(u16le) + CTR(u16le)`，无 Tail | `docs/ASAM_XCP_Part3_..._1.1.md` §1.3（第 244-296 行） | 实现一致；LEN/CTR 恒定小端，与 Session Byte Order 完全解耦（测试 `UdpHeaderEncode.GoldenSingleFrame` 逐字节锁定） |
| 一个 Datagram 可含多个完整 Frame，Frame 不得跨界 | 同上 §1.3.1 | 接收方向支持多 Frame；发送方向按项目策略 D7 为单 Frame/Datagram |
| CTR 每个 Packet 递增、双向独立、非事务 ID | 同上 §1.3.1.2 | 每 Frame 独立消耗一个 CTR；响应匹配依赖单 Pending + RES/ERR PID，与 CTR 无关（`ResponseMatchingDoesNotDependOnRequestCtr`） |
| MAX_CTO ∈ 0x08..0xFF、MAX_DTO ∈ 0x0008..0xFFFF | 同上 §1.4（第 305-308 行） | `Session::validateConnectParams` 校验 |
| `UPLOAD: 1..MAX_CTO/AG - 1`、`SHORT_UPLOAD: 1..MAX_CTO/AG` | `docs/XCP_1.3.0_document.md` §7.5.1.11/§7.5.1.12 | `MemoryAccess::maxUploadElements/maxShortUploadElements` 实现，AG=1/2/4 参数化测试覆盖 |
| UPLOAD/SHORT_UPLOAD 的 RES 无计数字节 | 同上 §12.4/§12.5 报文示例 | 长度校验为 `== elements*AG` |
| SYNCH 始终以 ERR_CMD_SYNCH 应答，且仅在该场景视为成功 | 计划 §6.2 第 1 条 | `performRecovery` 只在 `m_in_recovery_` 期间接受 ERR_CMD_SYNCH |

---

## 5. 项目策略落地（设计 §18.2 决策）

| 决策 | 实现位置 | 测试 |
|---|---|---|
| D1 CTR 初值 0，`open()` 复位、清空接收基线 | `UdpTransport::open` | `ReopenResetsSendCtrAndReceiveBaseline` |
| D2 原命令最多 2 次恢复重试 | `CommandTimeouts::maxRetries` 默认 2 | `RetryExhaustionReportsRecoveryFailed`（断言 Upload×3、Synch×2）、`ZeroRetriesFailsImmediately` |
| D3 Master 默认严格匹配远端 IP+端口 | `UdpTransport::isRemoteMatch` | `ForeignPortRejectedWhenStrict`、`IgnoresForeignSourceIp` |
| D4 不重排、不重传 Frame | 接收路径无重排/重传逻辑 | `BackwardCtrFrameDropped`、`DuplicateCtrIsDropped` |
| D5 恰好相差 0x8000 视为歧义丢弃 | `UdpTransport::handleFrame` | 实现分支 + 注释；0x8000 精确构造需发送侧配合，当前由前跳/后向用例覆盖相邻区间 |
| D6 默认单 Frame ≤ 65503、Datagram ≤ 65507 | `kUdpMaxXcpPacket`/`kUdpMaxDatagramSize` | `LimitsMatchIpv4Udp`、`RejectsOversizedPacket`、`MaxAllowedPacketAccepted`、`DatagramOverMaxSizeDiscarded` |
| D7 发送单 Frame/Datagram，接收支持多 Frame | `encodeUdpFrame` + `handleDatagram` | `EachSendProducesOneFrameWithIncrementingCtr`、`MultipleFramesInOneDatagramDeliveredInOrder` |
| D8 Slave 连接后仅校验来源 IP，响应固定回 CONNECT 端点 | `UdpTestSlave::isCurrentSessionSource` | `SameIpDifferentPortStillServed`（并断言响应不发往变更后的端口） |

> D5 说明：`0x8000` 歧义分支已实现并有代码注释，但缺少一个"精确构造 0x8000 差值"的用例
> （需要发送侧提供任意 CTR 注入，当前 `sendRawPayloadTo` 已具备该能力，可作为后续补充）。
> 相邻区间（前跳 1..0x7FFF、后向 0x8001..0xFFFF）已有正向用例覆盖。

---

## 6. 构建环境与命令

```
CMake 4.0.2 / MSVC 19.44.35228 (VS 2022 Community, x64) / Windows SDK 10.0.26100
生成器：Visual Studio 17 2022（沙箱下 Ninja 不可用，见批次 1 记录 §5）
GoogleTest v1.15.2（离线源 .deps-cache/googletest）
clang-format 22.1.8
```

```bash
cmake -B cmake-build-release -S . && cmake --build cmake-build-release --config Release
ctest --test-dir cmake-build-release -C Release --output-on-failure

cmake -B cmake-build-debug -S . && cmake --build cmake-build-debug --config Debug
ctest --test-dir cmake-build-debug -C Debug --output-on-failure
```

---

## 7. 测试结果

### 7.1 汇总

| 配置 | 构建 | 项目源码警告 | ctest |
|---|---|---|---|
| Release | 成功（清洁全量构建） | **0**（`/W4`） | **248 项：247 通过 + 1 跳过，0 失败** |
| Debug | 成功 | **0** | **248 项：247 通过 + 1 跳过，0 失败** |

唯一跳过项为按设计跳过：`AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`
（AG=BYTE 时任意字节数都合法，无"非整除"场景可测）。

### 7.2 稳定性

UDP/时序相关子集（`*Udp*` / `*Recovery*` / `*CmdPending*` / `*Loopback*`，共 84 项）连续复跑
**5 次全部通过，0 次偶发失败**，符合计划 §9.6 第 8 条"避免依赖固定时序"的要求。

### 7.3 格式检查

```
clang-format --dry-run --Werror -style=file <全部 .hpp/.cpp>   → exit 0（无差异）
```

### 7.4 测试用例分布（248 项）

| 测试文件 | 主要套件 | 说明 |
|---|---|---|
| `protocol_types_test.cpp` / `xcp_error_test.cpp` | 批次 1 | 81 项（含批次 1 全部用例） |
| `udp_header_codec_test.cpp` | 编码/解码/多 Frame/边界 | 黄金向量与原子性 |
| `udp_transport_test.cpp` | 生命周期/配置/收发/CTR/关闭 | 真实 Socket |
| `udp_test_slave_test.cpp` | 会话/命令/故障/打包 | Slave 端点绑定规则 |
| `memory_access_test.cpp` | 基本/AG矩阵/降级/溢出/恢复/字节序 | AG=1/2/4 参数化 |
| `recovery_test.cpp` | 超时恢复/EV_CMD_PENDING/分流/错误分派/CONNECT 流程 | Mock 确定性 |
| `xcp_master_integration_test.cpp` | Mock 端到端 + AG 参数化 | 计划 §9.5 |
| `xcp_udp_loopback_test.cpp` | 真实 UDP 端到端 | 计划 §9.6 |

---

## 8. 验收对照（计划文档 §10）

| # | 验收标准 | 状态 | 证据 |
|---|---|---|---|
| 1 | 8 条命令均有 Codec/Parser 覆盖 | ✅ | `command_codec_test`（8 条黄金报文）+ `response_parser_test`；`FakeSlave`/`MockXcpSlave` 实现 8 条命令语义 |
| 2 | 能建立/关闭 Session 并保存全部必需参数 | ✅ | `XcpMasterIntegration.FullHappyPathSequence`、`GetStatusAndCommModeInfoParsed` |
| 3 | AG=1/2/4、Intel/Motorola 下正确轮询原始内存 | ✅ | `MemoryAccessAgTest`（6 组参数）、`XcpMasterAgTest`（4 组）、`MotorolaSessionEncodesAddressBigEndian` |
| 4 | SHORT_UPLOAD 不可用时自动降级 | ✅ | `ShortUploadUnknownDisablesShortUploadForSession`、`SubsequentReadsSkipShortUpload`、`ShortUploadUnknownDegradesToChunkedUpload` |
| 5 | 严格单 Outstanding Command | ✅ | `TracksSingleOutstandingCommand`、`ConcurrentCommandRejectedWhilePending` |
| 6 | Timeout 执行 SYNCH，UPLOAD 重试前恢复 MTA | ✅ | `TimeoutTriggersSynchThenRetrySucceeds`、`SynchPrecedesRetryInCommandOrder`、`UploadTimeoutRecoversByResendingSetMta`（断言 SET_MTA×4 = UPLOAD×4）、`ChunkTimeoutRecoversViaSynchAndSetMta` |
| 7 | EV_CMD_PENDING 不导致重复发送 | ✅ | `RestartsTimerWithoutResendingCommand`（断言发送次数 == 1）、`MultipleCmdPendingThenFinalResponse` |
| 8 | 输入和解析具备长度、范围、溢出保护 | ✅ | 批次 1 解析防护 + `RejectsReadPastEndOfAddressSpace`、`AcceptsReadEndingExactlyAtLimit`、`AgMultipliedRangeOverflowDetected`、`MismatchedResLengthRejected` |
| 9 | Release 单元 + Mock 集成 + UDP Loopback 全部通过 | ✅ | §7.1（Release/Debug 双配置） |
| 10 | UdpTransport 使用标准 4 字节 LEN/CTR，恒小端，与协议层字节序解耦 | ✅ | `UdpHeaderEncode.GoldenSingleFrame`、`CounterWrapsUseFullSixteenBits`、`MemoryAccessByteOrder.MotorolaSessionEncodesAddressBigEndian`（协议层大端不影响 Header 小端） |
| 11 | 不把 CTR 当事务 ID，能诊断丢包/重复/乱序，不擅自重排重传 | ✅ | `ResponseMatchingDoesNotDependOnRequestCtr`、`ForwardJumpIsAcceptedWithGapWarning`、`DuplicateCtrIsDropped`、`BackwardCtrFrameDropped`、`ReceiveCtrWrapsFromFfffToZero` |
| 12 | UDP 测试不依赖固定端口、外网、厂商库，可靠取消释放资源 | ✅ | Slave 绑定临时端口（`SlaveBindsEphemeralPort`）；`CloseReleasesThreadAndPortPromptly`（关闭后同端口可重绑定）；`RepeatedSessionsRemainStable`（5 轮） |
| 13 | 核心库不依赖 Qt/A2L/CAN 厂商库 | ✅ | 仅依赖标准库 + Threads + ws2_32（平台 Socket） |
| 14 | 未实现功能返回明确错误 | ✅ | `UnsupportedFeature`：`AccessLockedMapsToUnsupportedFeature`、`AccessLockedSurfacesUnsupportedFeature` |
| 15 | 不把 Loopback/Mock 通过表述为真实 ECU/CANape 互操作通过 | ✅ | 本记录与实现计划 §11.4 均明确声明：Loopback 仅验证自有两端，**未与第三方 ECU/CANape 互操作验证** |

---

## 9. 已知限制与后续工作

1. **未与真实 ECU / CANape 互操作验证**：所有端到端测试均为自有 Master ↔ 自有 UdpTestSlave，
   两端可能共享同一误解（计划 §11.4）。后续需第三方抓包对照。
2. **仅 Windows 实测**：跨平台代码已隔离（`udp_transport.cpp` 顶部平台适配层），但
   Linux/macOS 未在本环境编译运行，需三平台 CI 验证（计划 §11.5）。
3. **D5（0x8000 歧义）缺正向构造用例**：分支已实现，测试覆盖其相邻区间；补充用例需发送侧
   任意 CTR 注入，`sendRawPayloadTo` 已具备能力。
4. **DTO 仅识别不解析**：`IEventListener::onDto` 已实现上报，符合本阶段范围。
5. **Block/Interleaved Mode 未启用**：仅解析 `COMM_MODE_OPTIONAL`/`MAX_BS`/`MIN_ST`/`QUEUE_SIZE` 能力位。
6. **成员命名偏差延续批次 1**：仍使用 `<snake>_` 尾下划线而非设计 §2.2.1 的 `m_<snake>_`
   （详见批次 1 记录 §2 第 1 项，待确认）。

---

## 10. 下一步

本批次已达到计划文档 §2.1 的完整包含范围。可选后续方向：

- 补齐上述限制 2（Linux/macOS 构建与 CI）与限制 3；
- 进入计划 §12 里程碑：最小 A2L（MEASUREMENT/数据类型/COMPU_METHOD）、DAQ/ODT/DTO、
  XCP on CAN/CAN FD、与真实 ECU 互操作。
