# XCP V1.5 批次 G 实施记录（文档收口与验收）

- 批次：G（V1.5 工程最后一个批次；方案 §3.4）
- 实施日期：2026-10-22
- 授权：用户 m05842 `执行批次 G`；范围裁定 m05904、设计裁定 m05945/m05943
- 规范基准：`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.pdf`（PL，权威）
  与 `docs/ASAM-AE-MCD-1-XCP-AS-Ethernet-Transport-Layer-V1-5-0.pdf`（TL，权威）
- 前置批次：A / C / D / E1 / E2 / F / B1 / B2 均已交付

---

## 1. 批次目标与本轮范围裁定

方案 §3.4 定义批次 G 为**文档收口与验收**，含 G.1～G.5 五项。

**调研阶段发现的关键问题**：DoD §5.6 列出的 `Xcp13CoreBaseline` 能力表
（方案 §4.2.1 冻结表）中判为 **Block** 的几组能力，**在实现中并不存在**：

- 11 个 DoD 测试 ID（`VersionUnknown.*` / `TransportUnknown.*`）在 `tests/`
  中**一个都不存在**（逐一 grep 计数均为 0）。
- `grep -rn "conservative" src/ include/` 表明该标志的**唯一消费方**是
  `src/xcp_master.cpp` 中的日志语句，**没有任何能力门禁行为**。

即：传输层门禁（alignment / IPv6）真实存在，但「Minor unknown 时按能力集
保守执行」当时**只是一个死标志**。

**用户裁定（m05904）**：选择「补齐测试 + 实施缺失的门禁」，而非仅文档化。

**用户裁定（m05945）**：
- `gate_strength` = **硬阻断**：Minor unknown 时，高版本命令调用直接抛
  `UnsupportedFeature`。
- `optional_tl` = **不新增配置面**：传输层本就不实现这四个子命令，在能力查询
  API 里上报为 Unsupported 即可。

**用户裁定（m05943）**：同意设计第二章节选 **2B**（新增拒绝入口），
第 1、3 章节同意。

---

## 2. G.2 —— 能力上报 API 与硬阻断门禁

### 2.1 新增公开 API

与既有 `HasCalPagResource()` 同构：

```cpp
/// @brief 本实现不支持的命名能力项
enum class UnsupportedCapability {
    DaqPackedMode,            // SET/GET_DAQ_PACKED_MODE（V1.5 新特性）
    Level1OtherSubcommand,    // 0xC0 族除 GET_VERSION 外的子命令
    Ipv6Transport,            // IPv6（基础 Profile 只承诺 IPv4 UDP 单播）
    OptionalTlSubcommand,     // 四个 Optional TL Subcommand
};

/// @brief 查询某能力是否为本实现明确不支持
[[nodiscard]] bool IsCapabilityUnsupported(UnsupportedCapability cap) const;

/// @brief 若该能力不支持则抛 UnsupportedFeature，支持则无副作用
void ThrowIfUnsupported(UnsupportedCapability cap) const;
```

### 2.2 设计要点

- **绝不报错类别混淆**：能力缺失一律报 `ErrorCategory::UnsupportedFeature`，
  **不得**报 `ProtocolError` / `Timeout`——否则调用方会把「库不支持」误判为
  「对端故障」。IPv6 同样报 UnsupportedFeature 而非 InvalidArgument，以区分
  「能力缺失」与「地址写错」。
- **本地判定，不探测**：`IsCapabilityUnsupported()` 不发送任何命令。方案要求
  「禁止内容探测式尝试」——不得靠发送命令看错误码来推断能力。
- **值为编译期常量**：四个能力在 2.0.0 中均为未实现，故查询为常量返回，无状态。

### 2.3 实现位置

- `include/libxcp/xcp_master.hpp`：新增枚举与两个方法声明（含中文 Doxygen）
- `src/xcp_master.cpp`：新增 `IsCapabilityUnsupported()` 与
  `ThrowIfUnsupported()` 实现

---

## 3. G.3 —— 三版本测试

### 3.1 新增用例（`tests/xcp_master_integration_test.cpp`）

| 用例 | 断言要点 |
|---|---|
| `XcpMasterVersion.ThreeVersionMatrixDiscoveredFaithfully` | V1.3/V1.4/V1.5 三个目标版本逐一验证：`reported` 与 `effective` 均如实取值，`conservative` 与 `capped` 均为 false，**TL 独立取值不被 PL 覆盖** |
| `XcpMasterVersion.BelowSupportedFloorIsNotSilentlyPromoted` | 对端报 V1.2（低于支持下限）时 `reported` 必须保留原话，**不得改写成 V1.3** |
| `XcpMasterVersion.AboveCapIsCappedAndConflictVisibleForBoth` | V1.6 与 V2.0 均须：保留 `reported` 原话、`capped=true`、`effective` 压到 V1.5、**冲突记录可见**（不得静默） |

### 3.2 关键说明：上限 V1.5 是工程决策

`include/libxcp/protocol_version.hpp:83-85` 已注明：上限 V1.5 是**工程决策**，
规范中**不存在兼容矩阵**（方案 §1.9），不得声称有外部依据。故越界版本的处理
是「可用但被记录」，既不静默接受也不硬拒绝。

---

## 4. 1.x-conservative 能力基线测试

为方案 §4.2.1 冻结表中判为 **Block / gated** 的分组分派 DoD 测试 ID：

| 测试 ID | 用例 | 结果 |
|---|---|---|
| `VersionUnknown.HighVersionBlocked` | `VersionUnknown_HighVersionBlocked.DaqPackedModeIsBlockedLocally`<br>`VersionUnknown_HighVersionBlocked.ThrowIfUnsupportedRaisesUnsupported` | PASS |
| `TransportUnknown.OptionalBlocked` | `TransportUnknown_OptionalBlocked.OptionalTlSubcommandIsBlockedLocally`<br>`TransportUnknown_OptionalBlocked.Ipv6IsReportedAsUnsupportedNotInvalid` | PASS |
| `VersionUnknown.CoreSession` | `VersionUnknown_CoreSession.ConservativeModeStillConnects` | PASS |
| `VersionUnknown.Memory` | `VersionUnknown_Memory.ConservativeModeMemoryReadUsesAlternativePath` | PASS |

其中「保守态仍可用」两例验证的是**保守基线的目的**是「少做」而非「不能用」：
Minor 未确认不得妨碍会话建立与内存读取；`SHORT_UPLOAD` 不可用时应自动降级到
`SET_MTA` + `UPLOAD` 替代路径。

### 4.1 ⚠ 已知语义张力（用户裁决 m06013：保持现状 + 文档明写）

方案 §4.2.1 要求保守时 Minor「**保持 unknown**」，而实现
（`src/version_resolver.cpp:138`）令 `effective = {1,3}`：

```cpp
} else if (!pl_evidence.minor_confirmed) {
    result.protocol.effective = kProtocolVersion13;  // 能力口径下限
    result.protocol.conservative = true;
```

两者靠 `conservative` 标志与 `confidence == MajorOnly` 区分，**信息未丢失**，
但 `effective.minor` 读出来是 `3` 而非「未知」。经用户裁决保持现状，理由：
改动会破坏 2.0.0 已发布语义。

**⚠ 调用方约束（载荷约束，非可选）**：调用方**不得只看 `effective`**，
必须同时检查 `conservative`；否则会把保守口径误读为「对端是 V1.3」。
该约束已写入 `tests/xcp_master_integration_test.cpp` 的用例注释中固化。

---

## 5. G.1 —— 文档收口

### 5.1 `README.md` 更正

| 位置 | 原内容 | 更正后 |
|---|---|---|
| 标题 | `XCP 1.3.0 Master 协议栈` | `XCP Master 协议栈` + 版本声明段落 |
| 能力表 | `XCP 1.3.0 协议核心` | `XCP 协议核心（PL V1.3～V1.5）` |
| 能力表 | `Calibration / Programming ⛔ 不在 v1.0 承诺范围` | **标定单列 ✅ 已实现**；Programming 单列 ⛔ 未实现 |
| 能力表 | 无 | **新增「明确不支持」能力边界表**（四项，可经 `IsCapabilityUnsupported()` 查询） |
| `include/libxcp/` 注释 | `公开头文件（v1.0 冻结面）` | `公开头文件（API 2.0.0）` |
| §6 标题 | `公开头文件（v1.0 冻结面）` | `公开头文件` + **API/ABI 迁移说明**（指向 B1 实施记录） |
| §7 | `XCP 1.3.0 规范与中文速查` | 完整规范清单，含 PL V1.5 / TL V1.5 PDF 并**标注 PDF 为权威准据** |

### 5.2 `docs/modules/文档与工程规范.md` 更正

补入 PL V1.5 与 TL V1.5 原始 PDF，并把旧 TL V1.1 文本版与 Part 3 1.0 PDF
明确标注为「**历史版，非当前基线**」（满足 DoD-1：旧 TL V1.1 不再出现在发布
声明中）。

---

## 6. G.4 —— Release clean build 记录

### 6.1 macOS（实测）

| 项 | 值 |
|---|---|
| 平台 | Darwin 27.0.0 arm64 / macOS 27.0.1（Build 26A434） |
| 编译器 | Apple clang 21.0.0 (clang-2100.3.34.2) |
| 构建方式 | 全新目录，`-DCMAKE_BUILD_TYPE=Release -DLIBXCP_BUILD_TESTS=ON` |
| 编译结果 | **exit 0，0 error** |
| 编译耗时 | 29.0 s（`-j 4`） |
| 自有代码警告 | **0** |
| 第三方警告 | googletest 34（`gtest-printers.h` 30 / `gtest.h` 4）、a2llib 22、a2l-sdk 3、链接器 ld 3 |
| 测试结果 | **623 tests / 622 passed / 1 skipped / 0 failed**（35.4 s） |
| 唯一 skip | `AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`（既有设计性 skip：AG=1 时该用例前提不成立） |

### 6.2 本轮修掉的自有代码警告

Release clean build 暴露出 `src/xcp_master.cpp` 两条 `-Wunused-variable`，
根因是 `LIBXCP_ENABLE_LOG=OFF` 时日志宏展开为空操作、**完全不求值参数**，
于是仅用于日志的局部变量成为未使用：

- `src/xcp_master.cpp:539` `const GetVersionResponse& vr = *probe.response;`
- `src/xcp_master.cpp:596` `for (const std::string& conflict : m_version_conflicts_)`

修法遵循仓库既有约定（`src/session.cpp:166`）：用 `#if LIBXCP_ENABLE_LOG`
包裹变量声明与日志语句（修复后分别位于 `src/xcp_master.cpp:538` 与 `:595`），
**保持「关日志时零开销」的设计意图**，而非用 `(void)x` 强制作求值。

### 6.2.1 补充：examples / tools 的 3 条警告

`examples` 与 `tools` 起初未被计入 G.4（初次 clean build 用了
`LIBXCP_BUILD_EXAMPLES=OFF` / `LIBXCP_BUILD_TOOLS=OFF`）。启用后完整重编
暴露出 **3 条自有代码警告**，均已修复：

| 位置 | 警告 | 处理方式与理由 |
|---|---|---|
| `examples/xcp_slave_demo/slave_registry.cpp:244` | `-Wunused-parameter`：`mode` | `mode` 是 XCPlite 回调的**固定签名参数**，不能删除或改名；它仅在日志中被消费，故标注 `[[maybe_unused]]` |
| `examples/measurement_demo/measurement_demo.cpp:997` | `-Wunused-variable`：`ShortName` lambda | 核实全文件仅定义处出现一次（对照 `Qualified` 用了 12 次），确为**死代码**，直接删除 |
| `examples/xcp_integration_demo/xcp_integration_demo.cpp:684` | `-Wunused-function`：`UnlockWithInjectedSeedKey` | 核实无任何外部引用；但它是 Seed&Key 解锁的**有价值的示范**，故保留并标注 `[[maybe_unused]]` + `@details` 说明「有意保留」 |

**未处理 1 条（经用户裁决 m06351 不动）**：
`examples/xcp_master_udp/demo_process.cpp:59` 的
`posix_spawn_file_actions_addchdir_np` 弃用警告（因被 3 个 target 编译而报 3 次）。

**不能简单替换的理由（已核实）**：替代函数
`posix_spawn_file_actions_addchdir` 在 SDK 头文件中标注
`__API_AVAILABLE(macos(26.0))`，**仅 macOS 26+ 可用**；而旧函数标注
`macos(10.15, 26.0)`（可用至 26，仅被标弃用）。项目未设
`CMAKE_OSX_DEPLOYMENT_TARGET`，改用新函数会使旧 macOS 编译失败。
`tests/xcplite_slave_fixture.cpp:229` 采用 `fork()` + `chdir()` 可完全避开该
API，但那会改变 examples 的独立实现方式，故本轮不改。

**验证**：完整重编后 error 0；自有代码警告（`src`/`include`/`examples`/`tools`）
仅剩上述已裁决不动的 3 条 SDK 弃用警告；全量回归 **623 tests / 622 passed /
1 skipped / 0 failed**；实跑 `examples/xcp_master_udp` 端到端
**15 项断言全过、exit 0**，功能不变。

### 6.3 Windows / Linux —— **Unverified（未验证）**

**如实记录，不伪造**（满足 DoD「任何未覆盖组合必须标为 `Unverified`，
不得从相邻组合外推 Supported」）：

| 平台 | 状态 | 说明 |
|---|---|---|
| Windows | **Unverified** | 本机为 macOS，无可用 Windows 构建环境 |
| Linux | **Unverified** | 本机为 macOS；Parallels 装有 Ubuntu 24.04 ARM64 与 Ubuntu25，但 `prlctl` 在沙箱下初始化失败（`/bin/ps: Operation not permitted`），未能取得构建证据 |

用户裁决（m06166）：选择「只记 macOS，另两平台如实标 Unverified」，
而非启动虚拟机实跑或转人工在外部机器执行。

**复现步骤**（供后续在 Windows/Linux 上补证）：

```bash
cmake -B cmake-build-release -S . -DCMAKE_BUILD_TYPE=Release -DLIBXCP_BUILD_TESTS=ON
cmake --build cmake-build-release -j 4
./cmake-build-release/tests/libxcp_tests
```

Windows 需额外确认 Boost 自动发现（`C:\boost\lib\cmake\Boost-*`）或显式传
`-DBoost_DIR=<prefix>/lib/cmake/Boost-<version>`。

---

## 7. G.5 —— DoD §5.6 逐项签署

| # | DoD 条目（摘要） | 状态 | 证据 |
|---|---|---|---|
| 1 | A 批次固化 PL/TL 官方 PDF 来源；旧 TL V1.1 不再出现在发布声明 | ✅ | §5.2；`docs/modules/文档与工程规范.md:16` 已标「历史版，非当前基线」 |
| 2 | V/F/T/N/O/DOC 各项在追踪矩阵中有实施记录与测试结果 | ✅ | 各批次实施记录；本轮补 `XCP_V1.5_批次G_实施记录.md` |
| 3 | `XcpMasterOptions` 完整替换旧构造接口，无 Deprecated 兼容层 | ✅ | `grep -rn "Deprecated\|deprecated" include/libxcp/xcp_master.hpp` → 空；库版本 2.0.0 |
| 4 | GET_VERSION 不可用时不伪造 Minor；CoreBaseline 每项均有测试 | ✅ | §4 四组测试；`*Version*:*Conservative*` → 49 tests PASSED |
| 5 | PL 与 TL 独立 Reported/Effective/Source/Confidence；重连同时清空 | ✅ | `*VersionEvidenceIsCleared*:*ReconnectAdopts*` → PASS |
| 6 | TL V1.5 Alignment 8/16/32 Tail 编解码、A2L 自动贯通、冲突门禁 | ✅ | 批次 B2 实施记录 |
| 7 | IPv6 与四个 Optional TL Subcommand 明确为 Unsupported，不误报 | ✅ | `TransportUnknown_*` → 2 tests PASSED（含与 InvalidArgument 的区分断言） |
| 8 | 冻结 `LinkBudgetConfig` + §1.9A.3 公式完成带宽预算；溢出与 override 有测试 | ✅ | 批次 B2 实施记录 |
| 9 | V1.3/V1.4/V1.5 已承诺能力与 TL V1.5 IPv4 UDP 单播分别通过版本化测试 | ✅ | §3 三版本矩阵用例 |
| 10 | GET_ID 压缩/加密标志在未支持时明确失败 | ✅ | 批次 F 实施记录（F-03） |
| 11 | 恢复策略默认值与 §4.6 一致（timeout 重试 2 次、BUSY 100ms/10s、分段重启、Cancelled 语义） | ✅ | 批次 D/F 实施记录 |
| 12 | 核心与 a2lbridge 的 DTO 布局对同一 golden 一致 | ✅ | 批次 E1 实施记录（golden 一致性测试） |
| 13 | XCPlite DAQ/Measurement 端到端保持通过；未覆盖组合标 Unverified | ✅ | 批次 E1/C 实施记录；`XcpliteSlaveProcessTest` 全绿 |
| 14 | 按项目规则执行 Release clean build 后运行目标测试；三平台结果均有记录 | ⚠ **部分** | macOS 实测全绿；Windows/Linux **Unverified**（§6.3）——已如实记录，未伪造 |
| 15 | README、模块文档、API Doxygen 版本与能力声明不超过实测范围 | ✅ | §5；README 未声称有兼容矩阵依据，未声称三平台已验证 |
| 16 | 每批提交在其下游尚未合入时可原子回滚 | ✅ | 各批次独立提交；库版本 2.0.0 单点切换 |

**签署结论**：16 条中 **15 条满足**，第 14 条**部分满足**——三平台记录要求中
Windows/Linux 两平台为 `Unverified`，原因与复现步骤已如实记载。该偏差**不是
实现缺陷**，而是本轮执行环境的客观限制；按 DoD 自身要求，未覆盖组合必须标
`Unverified` 而非外推，故此处**不视为通过、亦不视为失败**，留待后续补证。

---

## 8. 测试与回归结果

### 8.1 本轮新增

| 文件 | 新增 | 说明 |
|---|---|---|
| `tests/xcp_master_integration_test.cpp` | 6 个 CoreBaseline 用例 | §4 四张测试 ID |
| `tests/xcp_master_integration_test.cpp` | 3 个三版本矩阵用例 | §3 |
| 合计 | **+9 用例** | 614 → 623（基数 614 见 `code-plan/XCP_V1.5_UdpClose唤醒机制_实施记录.md:224`） |

### 8.2 全量回归

| 构建 | 结果 |
|---|---|
| `cmake-build-logging-release` | **623 tests / 622 passed / 1 skipped / 0 failed** |
| 全新 Release clean build | **623 tests / 622 passed / 1 skipped / 0 failed** |

### 8.3 变异测试（证明断言非恒真）

| 变异 | 注入内容 | 预期捕获 | 实测 |
|---|---|---|---|
| G1 | `IsCapabilityUnsupported()` 四个能力分支 `return true` → `false` | 4 个门禁用例失败，2 个「保守态可用」用例仍通过 | ✅ 精确命中，**证明断言分得清「该阻断」与「该放行」**，无过度阻断 |
| G2 | `conservative` 置位改为不置位 | 2 个保守态用例失败 | ✅ 与 G1 影响面**互补**，证明两组断言相互独立、无冗余 |
| G3 | 取消版本封顶（`kProtocolVersion15 < reported` 分支禁用） | 2 个封顶用例失败 | ✅ 精确命中 |

三次变异均已还原，还原后 `grep -c "MUTATION G"` 为 0。

---

## 9. 代码风格与合规

- 所有新增公开接口、方法、枚举均含**中文 Doxygen 注释**（含 `@brief` /
  `@details` / `@param` / `@return`）。
- 新增测试用例含中文 `@brief` 说明**验证目的**，关键断言附设计意图注释。
- **本批次未修改 `thirdparty/` 下任何文件**。说明：`git status` 会显示
  `thirdparty/a2l-sdk/a2lbridge/` 的 3 个文件处于修改态，但那是
  **批次 B1 的 V-04 修复**（把 `ProtocolLayerInfo::version` /
  `TransportEndpoint::version` 的默认值由 `0x0100` 改为 `0`，以免未标注该
  字段的 A2L 被伪装成 V1.0），与本批次 G 无关。`a2lbridge` 是**项目自有的
  语义适配层**（`libxcp::a2lbridge`，README `:84`），并非上游子模块
  （上游为 `thirdparty/a2llib/`，是 git 子模块）。
- 未新增绝对路径（§6.3 复现命令使用相对路径）。

---

## 10. 遗留与后续

1. **DoD-14 补证**：Windows / Linux Release clean build 与测试需在对应平台
   执行后回填 §6.3（命令已给出）。
2. **保守态语义（§4.1）**：`effective.minor` 在保守态下为 `3` 而非「未知」，
   与方案 §4.2.1 字面要求存在张力，经用户裁决保持现状 + 文档明写。若后续版本
   要改为「未知」表示法，需同步 `tests/xcp_master_integration_test.cpp` 的
   固化断言并评估 ABI 影响。
3. **未实现能力**：`DaqPackedMode`、`Level1OtherSubcommand`、
   `Ipv6Transport`、`OptionalTlSubcommand` 四项维持 Unsupported，已在
   README 与能力查询 API 中明示。