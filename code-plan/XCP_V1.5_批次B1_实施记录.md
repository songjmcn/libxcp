# 批次 B1 实施记录：GET_VERSION + 版本发现

> 实施日期：2026-10-21 ～ 2026-10-22
> 依据：`code-plan/XCP_V1.5_规范符合性裁决与修复方案.md` 批次 B（用户 m03636 批准）
> 用户裁定：`version_mode` 三态 / `version_conflict` 不提供静默模式（m03674 区间）
> 授权修复重连既有缺陷：用户 m04241
> 本文档为 AGENTS.md 要求的批次实施记录

---

## 一、批次目标与背景

XCP V1.5 规范（原始 PDF）在 §3.1.1 与 Table 44/45 定义：

- **CONNECT 响应只给出主版本字节**（原始 PDF 第 12 页 §3.1.1）——无法据此判定次版本；
- **GET_VERSION（`0xC0`，PL 1 / TL 1）** 是 optional 命令，返回 PL/TL 各自的完整 major.minor（Table 45，PDF 第 114 页）。

此前库内**完全没有 GET_VERSION 能力**：版本只能来自 A2L 或 CONNECT 主版本，导致
「V1.3 还是 V1.5」这类能力裁决只能靠猜——这正是方案 §4.2 明令禁止的「库自动猜测版本」。

**批次 B1 目标**：建立「证据 → 裁决 → 快照」三层的版本发现能力，且**不得猜测**。

---

## 二、交付物

### 2.1 新增文件（4 个）

| 文件 | 内容 |
|---|---|
| `include/libxcp/protocol_version.hpp` | 版本模型：`XcpProtocolVersion`、`VersionSource`、`VersionConfidence`、`VersionEvidence`、双快照、`VersionPolicy` |
| `src/protocol_version.cpp` | `ToString()` / `SourceName()` / `ConfidenceName()` / `Validate()` 实现 |
| `include/libxcp/version_resolver.hpp` | PL/TL 独立裁决器接口（`Inputs` / `Result` / `Resolve` / `ResolveLayer`） |
| `src/version_resolver.cpp` | 裁决逻辑实现 |

### 2.2 版本模型（`include/libxcp/protocol_version.hpp`）

**`XcpProtocolVersion`**（`:51`）：`major` / `minor` 两字节 + `operator==` / `operator<` / `ToString()`。

关键语义（`:72-74`）：

```cpp
/// @brief 版本数值是否有效（Major 非 0）
/// @details 0.0 视为「未知」占位，不作为合法版本参与比较与能力裁决。
[[nodiscard]] constexpr bool IsValid() const noexcept { return major != 0U; }
```

这条是「不猜测」的基石：`0x0000` = 无证据，**不是** V0.0。

常量：`kProtocolVersion13{1,3}`（工程支持下限）、`kProtocolVersion15{1,5}`、`kTransportVersion15{1,5}`。

**`VersionSource`**（`:97`，5 档，按证据强度）：

| 值 | 含义 |
|---|---|
| `Unknown` | 尚无证据 |
| `GetVersion` | GET_VERSION 响应——**运行时唯一权威来源** |
| `ConnectMajor` | CONNECT 响应，**仅主版本** |
| `Configured` | 调用方 `VersionPolicy` 显式配置 |
| `A2l` | 可信 A2L 的 IF_DATA 声明——**静态证据**，弱于运行时 |

**`VersionConfidence`**（`:118`，与来源正交，5 档）：`Unknown` / `MajorOnly` / `Configured` / `StaticEvidence` / `RuntimeConfirmed`。

> 设计要点：来源说明「从哪来」，置信度说明「有多确定」，**二者不得互相替代**。这使「假定 V1.3」与「确认 V1.3」可区分（方案 §4.2 明确要求）。

**`VersionEvidence`**（`:144`）：`source` + `confidence` + `reported` + `minor_confirmed`，含 `IsAvailable()`（`:166`）：
```cpp
return source != VersionSource::Unknown && reported.IsValid();
```

**双快照**：`ProtocolVersionSnapshot`（`:184`）、`TransportVersionSnapshot`（`:210`）——**PL 与 TL 必须分开**（Table 45 给出两组独立版本）。

### 2.3 裁决器（`src/version_resolver.cpp`）

`VersionResolver::Resolve(const Inputs&)` → `Result`。

`Inputs`（`include/libxcp/version_resolver.hpp:36`）：`connect_protocol_major`、`connect_transport_major`、`probe`、`policy`、`a2l_protocol_version`、`a2l_transport_version`。

`Result`（`:59`）：`protocol`、`transport`、`conflicts`（只记录不改写）、`get_version_available`。

优先级：`GetVersion` > `Configured` > `A2l` > `ConnectMajor`。
冲突策略（用户裁定 m03674）：**不提供静默模式**，冲突永远记录在 `conflicts` 并对调用方可见。

### 2.4 GET_VERSION 命令族

| 层 | 接口 | 位置 |
|---|---|---|
| 编码 | `Bytes EncodeGetVersion() const` | `include/libxcp/command_codec.hpp:143` |
| 解析 | `std::optional<GetVersionResponse> ParseGetVersionResponse(...)` | `include/libxcp/response_parser.hpp:373` |
| 执行 | `GetVersionProbe ExecuteGetVersion()` | `include/libxcp/command_executor.hpp:302` |

**`GetVersionResponse`**（`include/libxcp/response_parser.hpp:209`，依 Table 45 五个位置）：
`reserved`（规范未规定语义，**保留原值供诊断、不参与裁决**）、`protocol_major`、`protocol_minor`、`transport_major`、`transport_minor`，以及视图方法 `ProtocolVersion()` / `TransportVersion()`。

**`GetVersionProbe`** + `GetVersionOutcome`（`include/libxcp/command_executor.hpp:251-284`）：

```
Available / UnimplementedCommand / UnimplementedSubcommand / TimedOut
```

`IsAvailable()` 仅在 `Available` 时为真。

> 关键设计：GET_VERSION 是 **optional 命令**，对端未实现属**正常情况**，故以 `GetVersionProbe` 的返回值表达，**不作为异常上抛**（`:293` 注释）。这是本批次最重要的错误处理决策——若上抛，未实现该命令的老 ECU 会连不上。

### 2.5 `XcpMasterOptions`（`include/libxcp/xcp_master.hpp:147-162`）

```cpp
VersionPolicy version_policy{};      // 默认全未配置：库不得自动猜测版本
bool autodetect_version{true};       // CONNECT 后自动探测；失败不构成连接失败
bool require_get_version{false};     // 默认 false；true 则拿不到精确版本即连接失败
```

`require_get_version` 失败路径（`src/xcp_master.cpp`）：

```cpp
throw detail::MakeInvalidState(
    "require_get_version 为真但 GET_VERSION 不可用（对端可能未实现该 "
    "optional 命令或响应超时）；版本保持未确认");
```

用 `InvalidState` 而非 `ProtocolError`：这是「本地配置要求与对端现实不符」，不是收到坏包（后者语义是「对端违规」）。

### 2.6 `XcpMaster` 版本接口（`include/libxcp/xcp_master.hpp`）

| 接口 | 行 |
|---|---|
| `ProtocolVersionSnapshot GetProtocolVersionSnapshot() const` | `:599` |
| `TransportVersionSnapshot GetTransportVersionSnapshot() const` | `:607` |
| `std::vector<std::string> VersionConflicts() const` | `:625` |

内部 `ApplyVersionProbe()` 在 CONNECT 后调用，把 CONNECT 主版本、探测结果、调用方策略交给裁决器。

> **注意**（`:508-519`）：该函数**未给 `inputs.a2l_protocol_version` 赋值**，A2L 输入在生产路径上恒为 `nullopt`。故 `VersionSource::A2l` 分支当前无生产入口。此为设计现状，非缺陷；若未来要让 A2L 参与裁决，需先增加配置字段。

### 2.7 破坏性接口变更（CMake 版本 1.x → 2.0.0）

`XcpMaster` 构造函数改为 `XcpMaster(std::unique_ptr<IXcpTransport>, XcpMasterOptions)`。
`CMakeLists.txt:27`：`VERSION 2.0.0`。

**全部调用方已迁移**（实测：**14 个文件 / 22 个构造点**）：

| 范围 | 文件数 | 构造点 |
|---|---|---|
| `tests/` | 11 | 19（`xcp_master_integration_test.cpp` 8 处、`xcplite_slave_fixture.cpp` 2 处，其余各 1 处） |
| `examples/` | 2 | 2 |
| `tools/xcp_master/` | 1 | 1 |
| `include/libxcp/measurement/measurement_session.hpp` | 1 | 1 |

> 说明：本记录早期草稿写的「17 个调用方」为口头估计，未经实测，已按实际计数更正。核实命令：
> `grep -rnE "make_unique<XcpMaster>|XcpMaster [a-z_]+\(" --include=*.cpp --include=*.hpp .`

---

## 三、附带修复：重连恒定失败（既有缺陷）

### 3.1 现象

`Connect → Disconnect → Connect` **恒定失败**：

```
C++ exception with description "当前状态不允许进入恢复: Connecting" thrown in the test body.
```

### 3.2 根因链

1. `Disconnect()` 关闭 Transport → `OnTransportClosed()` 把 `m_transport_closed_` 置位；
2. 重新 `Connect()` 时 `XcpMaster::Connect()` 调用 `Transport::Open()`，但**没有任何地方**把该标志清回 `false`；
3. 重连后第一条命令的 `WaitForResponse` 立即看到「通道已关闭」而提前返回 → CONNECT 超时；
4. 恢复逻辑因会话处于 `Connecting` 而抛 `InvalidState`。

### 3.3 修复（用户 m04241 授权）

新增 `void OnTransportReopened() noexcept`（`include/libxcp/command_executor.hpp:193`），
由 `XcpMaster::Connect()` 在 `Transport::Open()` 之后**立即调用**（不依赖调用方自觉）。

**只复位通道级状态**（关闭标志、原因文本、迟到响应槽位、同步/恢复标志），**不触碰会话状态**——会话由 `XcpMaster` 与 `Session` 各自管理，执行器不得越权修改。

配套 `bool IsTransportClosed() const noexcept` 供诊断与测试断言。

---

## 四、测试

### 4.1 新增/更新测试

| 测试文件 | 内容 |
|---|---|
| `tests/command_codec_test.cpp` | GET_VERSION 命令码 golden 断言（PL 1 / TL 1，两字节 `0xC0 0x00`） |
| `tests/response_parser_test.cpp` | `ParseGetVersionResponse` 布局用例 |
| `tests/version_resolver_test.cpp` | 裁决器单测：`VersionResolverTest` **13 条**（优先级、冲突记录、A2L fallback、保守状态）；同文件另含 `VersionPolicyTest` 3 条 + `XcpProtocolVersionTest` 2 条，合计 18 条 |
| `tests/xcp_master_integration_test.cpp` | `XcpMasterVersion` 集成用例 **13 条** |
| `tests/xcplite_protocol_test.cpp` | 与**真实 XCPlite 从站**的版本互操作用例 |
| `tests/seed_key_test.cpp` | mock 补 GET_VERSION 默认响应（修 15 个连带失败） |
| `tests/recovery_test.cpp` | 重连修复回归用例 |

### 4.2 测试过程中暴露的真实缺陷（均已修复）

| # | 缺陷 | 定位依据 |
|---|---|---|
| 1 | 探测成功后**未写回**版本快照 | 集成用例 `RealSlaveVersionIsReportedHonestly` |
| 2 | CONNECT 版本字节断言值有误 | XCPlite 源码取证（见 4.3） |
| 3 | `ProbeVersion()` 探测成功后未重新裁决 | 集成用例 |
| 4 | **重连恒定失败**（既有缺陷） | `Connect→Disconnect→Connect` 用例 |

### 4.3 CONNECT 版本字节语义的决定性取证

测试一度假设 CONNECT 响应中的版本字节是 `0x0104` 之类。查 XCPlite 源码：

`thirdparty/xcplite/src/xcp_cfg.h:29`
```c
#define XCP_PROTOCOL_LAYER_VERSION 0x0104 // PACKED_MODE, CC_START_STOP_SYNCH prepare
```

原始 PDF Table 38（PDF 第 110 页）确认 CONNECT 响应 position 1 为单个字节。两者对照后修正了测试断言。

> 教训：**规范原文 + 参考实现源码双向取证**才可靠。仅凭规范文字易漏掉实现约定，仅凭实现源码可能把实现 bug 当规范。

### 4.4 变异测试

对重连修复做了 **3 次变异测试**，确认新用例在还原缺陷后**确实失败**（非走过场）。

---

## 五、遗留问题

| 项 | 状态 |
|---|---|
| **V-04：A2L `0x0100` 硬编码** | 实施本批次时**未修复**（当时列为遗留）。2026-10-22 **已按方案 A 修复**，详见 `code-plan/XCP_V1.5_V04_A2L版本硬编码专项分析.md` |
| `VersionSource::A2l` 生产入口 | 仍无（`inputs.a2l_protocol_version` 恒 `nullopt`）。设计现状 |
| 更严的 `Require` 语义 | 当前只有 `require_get_version` 布尔开关；更强的版本约束由后续批次引入 |
| 批次 B2（T-01～T-08，TL V1.5 发布门禁） | **未开始** |
| 批次 G（文档收口与三平台验收） | **未开始** |

---

## 六、验证结果（2026-10-22 收尾）

| 套件 | 结果 |
|---|---|
| `libxcp_tests` | 573 tests：571 passed / 1 skipped / 1 failed |
| `A2lGolden` | 44 tests：42 passed / 2 skipped |
| `XcpliteIntegration` | 49 tests：48 passed / 1 skipped |

唯一失败 `UdpTransportClose.CloseWhileBlockedReturnsPromptly` 为**既有 flake**，与批次 B1 无关。

---

## 附：变更范围

`git diff --stat`（批次 B1 全程）：53 文件，+4402 / −495（含批次 C/D/E1/E2/F 的累积改动）。

批次 B1 本体涉及：4 个新增文件、`CMakeLists.txt`（版本 2.0.0）、`command_codec.hpp/cpp`、`response_parser.hpp/cpp`、`command_executor.hpp/cpp`、`xcp_master.hpp/cpp`、调用方迁移（14 文件 / 22 构造点）、7 个测试文件。