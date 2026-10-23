# B-T5 实施记录：CLI 传输选择与 XCPlite 真实 TCP 互操作

> 批次：**B-T5**（计划见 [`TCP传输_实施计划_B-T1至B-T5.md`](TCP传输_实施计划_B-T1至B-T5.md) §8）
> 验证平台：**macOS（AppleClang，Release）**——按用户裁决，本期只做 macOS 验证（m02116）。
> 状态：**代码与测试完成**；§8.3 因无独立 AsamV15 对端，按计划允许的方式收尾并声明。

---

## 1. 交付清单

### 1.1 新增源文件

| 文件 | 作用 |
| --- | --- |
| `tools/xcp_master/transport_selection.hpp` | 传输选择的**纯函数规则层**：把 CLI 请求 + A2L 事实解析为 `TransportSelection`，不含 socket、不读 A2L 文件，可单测 |
| `tests/xcp_master_transport_selection_test.cpp` | 规则层单测 **26 项**（B-T5 补漏后：23 + 多实例 3） |
| `tests/xcplite_tcp_interop_test.cpp` | XCPlite 真实 TCP 互操作 Gate **7 项** |

### 1.2 修改的既有文件

| 文件 | 改动 |
| --- | --- |
| `tools/xcp_master/main.cpp` | 新增 5 个选项与 `--preset xcplite`；接入规则层；按选择结果构造 `TcpTransport`；打印选择摘要与运行时版本摘要 |
| `tests/xcp_test_slave/main.cpp` | 新增 `--tcp`、`--queue-size <bytes>` 与"位置参数 + 选项混排"解析；`XcpEthServerInit(..., use_tcp, queue_size)`、`A2lInit(..., useTCP, ...)`；就绪行打印 `(tcp)`/`(udp)` |
| `tests/xcplite_slave_fixture.hpp/.cpp` | `XcpliteSlaveProcess` 增加 `use_tcp` 参数与 `UsesTcp()`；`ProbeOnce(port, use_tcp)` 在 TCP 模式下用 `TcpTransport` 探测；Windows/POSIX 两条 Spawn 路径都追加 `--tcp` |
| `tests/CMakeLists.txt` | `XcpliteIntegration` 增加 `xcplite_tcp_interop_test.cpp`；`XcpMasterSymbolPolicyTest` 链接 `libxcp::libxcp`（见 §5.4） |
| `README.md` | 公开头文件章节补充 TCP 传输头文件与"V1.5 独立对端未验证"声明 |

---

## 2. §8.1 CLI 实现

### 2.1 选项

```text
--transport udp|tcp
--tcp-profile asam-v1.5|asam-v1.4
--packet-alignment 8|16|32
--protocol-version 1.2|1.3|1.4|1.5
--transport-version 1.4|1.5
--preset xcplite
```

`--preset xcplite` 展开为 `transport=tcp, tcp-profile=asam-v1.4, PL=1.2, TL=1.4`，
并在选择摘要里**显式打印展开值与来源**（规则 7 要求日志/status 不得隐藏展开结果）。

### 2.2 规则落地（逐条对应计划 §8.1）

| 计划规则 | 落地方式 | 验证用例（`TransportSelection`） |
| --- | --- | --- |
| 1. TCP 必须显式 `--tcp-profile`，hint 只用于诊断 | 缺失即失败，错误文案含 `--tcp-profile` | `TcpWithoutProfileIsRejected`、`ParsesTcpProfileNames` |
| 2. `asam-v1.5` 必须取得 alignment：CLI > A2L 明确声明 | 两来源皆无则失败，错误含 `--packet-alignment` | `AsamV15RequiresAlignmentEvidence`、`AsamV15TakesA2lAlignment`、`CliAlignmentWinsOverA2l` |
| 3. `asam-v1.4` 禁止转 Tail；显式 `--packet-alignment` 冲突即报错 | `asam-v1.4` + 16/32 直接拒绝（错误含 `asam-v1.4`）；+8 接受且 `source=ProfileDefault` | `AsamV14RejectsWideAlignment`、`AsamV14AcceptsEightBitAlignment` |
| 4. `--host`/`--port` 成对；都无才取 A2L TCP 端点 | 不成对报错（含「成对」）；全缺时回落 A2L endpoint | `TcpRequiresHostAndPortInPair`、`TcpFallsBackToA2lEndpoint`、`TcpWithoutAnyEndpointIsRejected` |
| 5. 端点/对齐/Profile 同属一个 kind | 跨 kind 诊断，错误点明「A2L 只声明了 UDP 端点」 | `TcpIgnoresUdpOnlyA2lFacts` |
| 6. PL/TL 是独立证据，不因 Profile 自动声称 | Profile 不参与版本推断，显式版本原样透传 | `ProfileDoesNotImplyProtocolVersion`、`ExplicitVersionsArePassedThrough`、`ExplicitVersionBeatsA2lEvidence` |
| 7. preset 原子展开并展示来源 | 展开 + 摘要打印 + 与显式值冲突时报错 | `PresetXcpliteExpandsFullCombination`、`PresetRejectsConflictingProfile`、`PresetRejectsUdpTransport`、`PresetAcceptsConsistentExplicitValues` |
| 8. `--daq-profile` 与 Wire Profile 分离 | 未改动 `--daq-profile` 语义，仅 Wire Profile 影响 framing | 既有 DAQ 测试全部保持绿 |
| 9. UDP 默认行为兼容 | 不写 `--transport` 时仍为 UDP | `DefaultsToUdpAndKeepsCliEndpoint`、`UdpTakesExplicitA2lAlignment` |

### 2.3 状态输出

选择结果先打印 `FormatSelectionSummary()`（Wire Profile、来源、alignment、端点来源、
PL/TL 的 reported/effective、被降级能力），建链后再打印
`FormatRuntimeVersionSummary()`（`PL=1.5/1.2`、`TL=1.5/1.4` 形式的
运行时确认值/协商值）。

### 2.4 一处真实的顺序缺陷（已修）

跨 kind 诊断（`other_kind_declared` → 「A2L 只声明了 UDP 端点…」）最初写在
"端点回落"之后，而 TCP 无端点会先命中通用错误，导致该诊断是**死代码**。
现已前移到 `host_given != port_given` 校验之后、端点解析之前，并删除原重复块。

---

## 3. §8.2 XCPlite 真实 TCP 互操作 Gate

### 3.1 夹具与真实度

`XcpliteTcpInteropTest` 以 `--tcp` 启动由 `thirdparty/XCPliteV1.2` 构建的
`xcp_test_slave` 进程，Master 侧用真实 `TcpTransport` + `AsamV14` Profile，
**两侧都不替换 codec**，全部真实 socket。`SetUp()` 额外断言 A2L 中出现了
`XCP_ON_TCP_IP`——否则说明 `--tcp` 没真正传到 `A2lInit`，本次"TCP 互操作"名不副实。

### 3.2 覆盖矩阵（计划 §8.2 十条）

| # | 计划条目 | 覆盖方式 | 用例/依据 |
| --- | --- | --- | --- |
| 1 | Connect 且 PL major=1 / TL major=1 | **真实 XCPlite** | `ConnectOverTcpReportsMajorOne` |
| 2 | GET_VERSION | **真实 XCPlite**（**与计划假设相反**，见 §3.3） | `GetVersionOverTcp` |
| 3 | GET_STATUS、Polling Upload | **真实 XCPlite** | `ConnectOverTcpReportsMajorOne`（status）、`PollingUploadOverTcp` |
| 4 | 标定写入与回读 | **真实 XCPlite** | `CalibrationWriteAndReadBackOverTcp` |
| 5 | Dynamic DAQ：Select + START_STOP_SYNCH，不发 mode 3 | **真实 XCPlite** | `DynamicDaqOverTcp`（`ConfigureDaqListsDynamic` + `StartDaqSync`，代码路径不含 mode 3） |
| 6 | 接收 DTO LEN 内扩长，按 ODT layout 解码 | **真实 XCPlite** | `DynamicDaqOverTcp`：用核心 `DtoEnvelopeDecoder` 切身份，净荷做**前缀**比较（尾部 4 字节补白不断言） |
| 7 | CRM/DTO 共用 CTR；队列 lost 时 forward gap 只告警 | **loopback 故障注入 + 源级复核** | `TcpReceiveCtrTest`（9 项）；源级依据 `xcpEthTl.c:651` `gXcpTl.Ctr += (uint16_t)lost;`——丢帧时对端主动推进 CTR，故 v1.4 前向缺号只告警 |
| 8 | 多 DTO 单次 send、任意 recv 分片 | **loopback 故障注入** | `TcpLoopbackTest.ByteWiseSplitFramesAreReassembled`、`FrameHeaderSplitAcrossWrites`、`Profiles/TcpLoopbackProfileTest`（12 项） |
| 9 | DISCONNECT 后 socket 确实关闭；bounded backoff 后重连 | **真实 XCPlite** | `DisconnectThenReconnectOverTcp`（关闭由"同端口二次建链成功"反证）；本库**不自动重连**，故"backoff 后重连"落为"调用方显式重建链" |
| 10 | peer 主动关闭与异常退出 | **真实 XCPlite** | `PeerExitFailsCommandWithoutSilentReconnect`（杀进程）；主动关闭由 loopback `PeerCloseAfterResponseIsObserved`、`NoAutomaticReconnectAfterPeerClose` 覆盖 |

第 7、8 条为何不用真实进程：XCPlite 的队列丢帧与 TCP 分段时机依赖实时调度，
无法确定性触发。用 `tcp_test_slave` 的注入点可以**精确**制造"丢一帧""一帧分多次写"
"多帧挤在一次 write"，覆盖等价路径，且结论有源码级依据。**这是可复现性优先的选择，
不是能力缺失。**

### 3.3 计划假设的正误更正（重要）

计划 §8.2 第 2 条写"GET_VERSION 预期 unavailable/`ERR_CMD_UNKNOWN`，随后依据
显式或 A2L 证据解析为 PL1.2/TL1.4"。**实测与该假设相反**：

- XCPlite V1.2 的 TCP 服务**实现了 GET_VERSION**，`ProbeVersion()` 返回
  `GetVersionOutcome::Available`，响应里 PL/TL major 均为 1；
- 因此本期**不需要**为 XCPlite 链起"版本未知 → 降级解析"的兜底路径，
  显式 `--protocol-version/--transport-version` 与 A2L 证据只是**交叉验证**用途。

计划文本保留原样（不追改历史计划），以本节为权威更正。

---

## 4. §8.3 AsamV15 独立互操作 Gate：未执行与声明

本期**未获取到不复用本库 codec 的 AsamV15 独立对端**，按计划 §8.3 末段允许收尾。
发布声明（已写入 `README.md` 公开头文件章节）：

> **V1.5 线路布局（LEN 外 Tail、8/16/32 alignment、strict CTR）目前仅由本库自有
> loopback 与解码器单测验证，未与独立实现做过互操作验证。** 生产环境使用
> `asam-v1.5` 前应先与目标 ECU 联调。

AsamV15 侧的现有验证面（自证强度）：

- 解码器 36 项（`TcpStreamDecoderCtor/Single/Fragmentation/Coalescing/Finish/Malformed/Reset/CrossProfile/Gate`）；
- loopback 12 项（`Profiles/TcpLoopbackProfileTest`：V14Bit8、V15Bit8/16/32 × 全会话/ShortUpload/CTR 单调）；
- 通用帧层 25 项（`EthernetHeader*` 系列）。

---

## 5. 实施期间发现并修正的问题

### 5.1 `--preset` 与显式 `--transport` 的顺序依赖

`--preset` 最初在解析循环内直接改写 `transport`，导致 `--transport udp --preset xcplite`
与相反顺序行为不同。现改为：循环内只置 `preset_xcplite` 与 `transport_explicit`，
**循环结束统一收尾**——显式 `--transport` 压过 preset 隐含值，冲突交由规则层报错。

### 5.2 我自己写错的三处测试期望

1. `ExplicitProfileOverridesPreset` 期望"显式 profile 压过 preset"——与规则 7
   （preset 原子展开、冲突报错）矛盾。代码行为是对的，测试错。改为
   `PresetRejectsConflictingProfile`，并补 `PresetAcceptsConsistentExplicitValues`。
2. `TcpFallsBackToA2lEndpoint` 的 A2L 事实漏了 alignment，被规则 2 正确拒绝；
   补 `packet_alignment_bits=8` + `alignment_declared=true`。
3. 摘要里 Profile 渲染用 `TcpWireProfileName()`，得到 `AsamV15`/`AsamV14`
   （**不是** `asam-v1.5`），断言已按真实输出改。

### 5.3 DAQ 断言最初绕开了被测代码

`DynamicDaqOverTcp` 最初用 `frame[0]`、`frame.data()` 手算偏移找期望值，全部失败。
根因不是 framing 错，而是**帧头 4 字节识别字段 + 首 ODT 的 4 字节时间戳**被我忽略。
改为调用核心 `DtoEnvelopeDecoder`，按 `identity.odt` 分别核对 ODT0/ODT1 净荷前缀
——既修正了期望，也真正让解码器进入断言链路。

### 5.4 链接缺陷：`XcpMasterSymbolPolicyTest` 缺 `libxcp::libxcp`

加入选择规则测试后链接报
`Undefined symbols: calmcar::xcp::XcpProtocolVersion::ToString() const`。
`a2lbridge` 只提供头文件与桥接符号，`ToString()` 的实现符号在 `libxcp` 本体，
故在该目标 `target_link_libraries(...)` 头部补 `libxcp::libxcp`。

---

## 6. 测试结果（macOS Release，全量 ctest）

```
99% tests passed, 3 tests failed out of 951
Total Test time (real) = 52.79 sec
```

3 项失败**均为本期之前已存在的环境/既有缺陷**，与本批改动无关：

| 测试 | 失败性质 |
| --- | --- |
| `a2l_perf_gen` | 需 Python 3.10+，本机 `python3` 版本不满足 |
| `ParseLargeFileUnderBudget` | 依赖上一项的 fixture，fixture 未生成故 Not Run |
| `ExampleXcpIntegrationDemoCppDemo` | 既有示例程序缺陷，早于本批即复现 |

### 6.1 本期新增/相关测试面

| 测试面 | 数量 | 结果 |
| --- | --- | --- |
| `TransportSelection`（规则层） | 23 | 全绿 |
| `XcpliteTcpInteropTest`（真实 TCP） | 7 | 全绿（2175 ms） |
| `TcpLoopbackTest` + `Profiles/TcpLoopbackProfileTest` | 7 + 12 | 全绿 |
| `TcpTransportConfigTest/Capability/Connection/EndToEnd/ReceiveCtr` | 6+2+3+14+9 | 全绿 |
| `TcpStreamDecoder*` | 36 | 全绿 |
| `EthernetHeader*/EthernetFrame*/EthernetDatagram*/EthernetGate*` | 25 | 全绿 |

`libxcp_tests` 本轮共 750 项；`XcpliteIntegration` 56 项；`XcpMasterSymbolPolicyTest` 97 项。

---

## 7. 已知限制与后续建议

1. **无 AsamV15 独立对端**：见 §4 声明。建议下一期接入至少一个第三方 V1.5 TCP 实现，
   重点验证 LEN 外 Tail 与 8/16bit alignment 的真实对端行为。
2. **无自动重连**：这是刻意设计（"绝不静默重连"不变量），断链失败后须由调用方
   显式 `Disconnect()`/`Connect()`。若产品需要 bounded backoff，应在应用层实现。
3. **`--queue-size` 只影响 XCPlite 侧测量队列**，用于复现"队列溢出丢 DTO"现象，
   不是本库参数。
4. **丢帧/分片 Gate 依赖故障注入**：真实进程无法确定性触发，见 §3.2 第 7、8 条说明。
5. **仅 macOS 验证**：Linux/Windows 的 socket 层分支（`src/socket_util.cpp`）已按平台
   条件编译实现，但本批未在对应平台跑过测试。
6. **`xcp_master` 交互界面未做手工冒烟**：该工具要求真实 TTY（重定向时直接报
   "交互界面需要交互式终端"），而当前执行环境禁止 `openpty`（`script: openpty:
   Operation not permitted`），无法构造伪终端。TCP 建链路径本身由
   `XcpliteTcpInteropTest` 用同一份 `TcpTransport` + `AsamV14` 配置覆盖；未被
   自动化覆盖的差异仅剩"配置值来自规则层"，而规则层有 26 项单测。若需端到端
   命令行验证，请在带终端的机器上执行：
   `xcp_master --a2l <slave.a2l> --host 127.0.0.1 --port <port> --transport tcp --preset xcplite`。
7. **B-T4 登记的三条限制已收口**（后续补漏）：多实例改为 fail closed（计划 §7.3）、
   SDK 的 `endpoint_profile_hint`/`_declared` 死字段已删除、`plus_conflicts` 只报数值
   差异的契约已在 `DiffIfData()` 写死。详见
   [B-T5补漏_多实例fail-closed与SDK死字段清理.md](B-T5补漏_多实例fail-closed与SDK死字段清理.md)。