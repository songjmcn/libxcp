# V-04 专项分析：A2L `0x0100` 硬编码

> 分析时间：2026-10-22T09:45+08:00
> **实施完成时间：2026-10-22T09:49+08:00**
> 结论：**V-04 确认为真实缺陷（不是文档问题），已按方案 A 修复并验证通过**
> 文档状态：分析 + **实施记录**（§八 起为实施部分）

---

## 〇、实施摘要（2026-10-22 补记）

| 项 | 内容 |
|---|---|
| 用户决策 | 方案 **A**（修正上游取值），且**只改 A2L 侧两处 + 映射函数**（m04751） |
| 实际改动 | 3 处：`a2l_dto_map.cpp:431`、`if_data_xcp.hpp:55`、`if_data_xcp.hpp:33` |
| 保持不动 | `a2l_bridge.hpp:62` 的 `RuntimeXcpParams::protocol_version`（运行时侧，非 A2L 侧） |
| 新增测试 | 2 条（`tests/a2l_golden_test.cpp`），专门做「文件声明 ↔ 桥接层输出」对账 |
| 变异测试 | **通过**——还原硬编码后两条新用例均失败，信号精准 |
| 全量回归 | `libxcp_tests` 573（571 通过）、`A2lGolden` 44（42 通过）、`XcpliteIntegration` 49（48 通过） |
| 回归结论 | **无新增失败**；唯一失败为既有 flake `UdpTransportClose.CloseWhileBlockedReturnsPromptly` |

### 严重度更正（重要）

分析阶段曾把本项定为「中高」并称「A2L fallback 会伪造 V1.0」。**该定性不准确，已更正**：

实测 `src/xcp_master.cpp:508-519`（生产侧唯一调用 `VersionResolver::Resolve` 处）**从未给 `inputs.a2l_protocol_version` 赋值**，全仓 `grep` 确认该赋值只出现在 `tests/version_resolver_test.cpp`（3 处）。

**故 `VersionSource::A2l` 分支在生产路径上永不触发**，不存在「XcpMaster 版本裁决被伪造」的问题。

真实暴露面只有一处：`A2lBridge::CompareWithRuntime()` 的版本比对项（`thirdparty/a2l-sdk/a2lbridge/src/a2l_bridge.cpp:427` 绑定 `protocol_layer`，`:467` 取 `a2l.version >> 8` → 恒为 `0x01`）。

**修正后严重度：中**（不阻塞 XCPlite 端到端，不污染 XcpMaster 版本裁决）。修复理由不变：上游已正确解析，桥接层丢弃属实现错误，修复代价极低。

---

## 一、结论先行

计划 §4.1 与 V-04 要求「B1 必须先消除 `thirdparty/a2l-sdk/a2lbridge/src/a2l_dto_map.cpp:419-425` 的 `0x0100` 硬编码，**或**将 `A2l` 来源保持为 Disabled/Unavailable」。

**两项都没做。**

- 硬编码**仍然存在**，并**真实覆盖**了上游已正确解析出的版本值。
- `VersionResolver` 中 `VersionSource::A2l` 分支**是活代码**（`src/version_resolver.cpp:55-63`），不是 Disabled。
- 因此当前处于计划明确禁止的中间状态：**"把硬编码值包装成可信证据"**。

---

## 二、证据链（全部实测，非推理）

### 2.1 硬编码位置（3 处，均为默认值）

| 文件:行 | 代码 | 性质 |
|---|---|---|
| `thirdparty/a2l-sdk/a2lbridge/src/a2l_dto_map.cpp:424` | `layer->version = 0x0100;` | **无条件覆盖**（真凶） |
| `thirdparty/a2l-sdk/a2lbridge/include/libxcp/a2l/if_data_xcp.hpp:52` | `std::uint16_t version = 0x0100;` | 结构默认值 |
| `thirdparty/a2l-sdk/a2lbridge/include/libxcp/a2l/a2l_bridge.hpp:62` | `std::uint16_t protocol_version = 0x0100;` | 结构默认值 |

`a2l_dto_map.cpp:424` 是决定性的：函数已拿到上游 DTO，却**忽略** `dto.protocol_version` 而写入常量。

### 2.2 上游实际是**正确的**（关键事实，与计划假设不同）

计划 §4.1 称"当前 a2lbridge 将协议版本硬编码为 `0x0100`"，暗示数据模型可能无法提供该字段（"若现有 liba2l 数据模型无法可靠提供该字段…"）。

**实测证明：上游完全能提供，且解析正确。**

证据链（逐层核实）：

1. `thirdparty/a2llib/src/xcp/xcpdataparser.y:190` 语法规则：
   ```
   protocol_layer: BLOCK_BEGIN PROTOCOL_LAYER UINT ... {
       protocol.SetVersion($3);
   ```
2. `thirdparty/a2llib/include/a2l/xcp/protocollayer.h:128`：`GetVersion() { return version_; }`
3. `thirdparty/a2l-sdk/src/liba2l_export.cpp:1892`：`dto->protocol_version = pl->GetVersion();`
4. `thirdparty/a2l-sdk/include/liba2l/liba2l_api.hpp:399`：`std::uint16_t protocol_version = 0;`

### 2.3 决定性实验（一次性探针）

写两个旁路探针，**分别绕过桥接层**直测上游、以及**穿过桥接层**：

**实验一：合成 A2L，声明 `0x201`**

| 路径 | 回报值 | 判定 |
|---|---|---|
| 上游 `IDoc::GetIfDataXcp()` | **`0x0201`** | ✅ 正确解析 |
| a2lbridge `XcpInfo()->protocol_layer.version` | **`0x0100`** | ❌ 被覆盖 |

**实验二：仓库内真实文件 `tests/data/a2l/cpp_demo_V201_corrected.a2l`（第 178 行声明 `0x104` = V1.4）**

| 路径 | 回报值 | 判定 |
|---|---|---|
| 上游 | **`0x0104`** | ✅ 正确 |
| a2lbridge | **`0x0100`** | ❌ 谎报为 V1.0 |

**这是真实 ECU 描述文件，不是合成样本。**桥接层把一个声明 V1.4 的 A2L 报告成 V1.0。

### 2.4 该值确实流入版本裁决（活代码）

`src/version_resolver.cpp:55-63`：

```cpp
} else if (a2l.has_value() && a2l->IsValid()) {
    evidence.reported = *a2l;
    evidence.source = VersionSource::A2l;
    evidence.confidence = VersionConfidence::StaticEvidence;
    evidence.minor_confirmed = true;   // ← 硬编码值被当作"已确认的 Minor"
}
```

优先级高于 `ConnectMajor`，且**设置 `minor_confirmed = true`**。

---

## 三、测试为何没抓到（重要）

### 3.1 现有覆盖是"巧合性通过"

| 测试 | 为何通过 |
|---|---|
| `A2lGoldenTest.RuntimeTruthAndSeverity`（`tests/a2l_golden_test.cpp:820`） | 断言 `match.protocol_version = 0x0100`，而 golden A2L 第 49 行**恰好**也声明 `0x100`。硬编码与期望值**碰巧一致** |
| `VersionResolverTest.RuntimeVersusA2lConflictIsRecorded` | **直接注入** `inputs.a2l_protocol_version`，**根本不过桥接层** |
| `VersionResolverTest.FallsBackToA2lWhenNothingElseAvailable` | 同上 |

**没有任何一个测试把"真实 A2L 文件的声明值"与"桥接层输出"对账。**

### 3.2 反证实验（决定性）

我把 golden A2L 的声明从 `0x100` 改成 `0x104`，重跑：

```
[ RUN      ] A2lGoldenTest.RuntimeTruthAndSeverity
[       OK ] A2lGoldenTest.RuntimeTruthAndSeverity (0 ms)
[  PASSED  ] 2 tests.
```

**测试依然通过** —— 因为桥接层无视文件内容恒返回 `0x0100`，正好匹配测试里写死的期望值。

（该改动是 build-tree 生成物，已还原并复验：`A2lGolden` 42 tests / 40 passed / 2 skipped，与修改前一致。）

### 3.3 影响判定

| 场景 | 是否受影响 |
|---|---|
| `XcpMaster` 与真实 XCPlite 建链 | **不受影响**——自动 GET_VERSION 探测成功，优先级最高，A2L 值被遮蔽 |
| `A2lBridge::CompareWithRuntime()` | **受影响**——B-16 版本比对会把真实 V1.4 的 A2L 与运行时做错误对账 |
| A2L 作为 fallback（GET_VERSION 不可用） | **会伪造 V1.0**，并使用 cf. 计划 D1 硬约束被违反 |
| `adapter/`、`tools/`、`examples/` | 未发现生产调用 `CompareWithRuntime`；但 `XcpInfo()` 被 tools 读取（仅取 event_channels，未读 version） |

**当前实际暴露面：`CompareWithRuntime` 的版本比对项 + A2L fallback 路径。**

---

## 四、与计划表述的偏差（须记录）

| 计划原文 | 实测 |
|---|---|
| §4.1「当前 a2lbridge 将协议版本硬编码为 `0x0100`，修正前不得作为 Minor 证据」 | ✅ 准确 |
| §4.1「若现有 liba2l 数据模型无法可靠提供该字段，则…保持 Disabled」 | ❌ **前提不成立**——上游能正确提供；本可直接修复而非禁用 |
| V-04「B1 移除硬编码**或**禁用该来源」 | ❌ **两项都没做**，且 `A2l` 分支是活的 |

---

## 五、当前代码状态总结

### 5.1 已完成的（B1 主体）

- `include/libxcp/protocol_version.hpp`、`src/protocol_version.cpp`：版本模型（`VersionSource` 5 档、`VersionConfidence` 5 档、`VersionEvidence`）
- `include/libxcp/version_resolver.hpp`、`src/version_resolver.cpp`：PL/TL 独立裁决器
- GET_VERSION 命令族：`EncodeGetVersion()`、`ParseGetVersionResponse()`、`ExecuteGetVersion()`
- `XcpMasterOptions` 破坏性迁移，17 个调用方全部迁移，CMake 升至 2.0.0

### 5.2 缺口

| 缺口 | 严重度 | 说明 |
|---|---|---|
| **V-04 A2L 硬编码未消除** | **中高** | 真实缺陷；会伪造 V1.0 并置 `minor_confirmed=true` |
| 无"真实 A2L 声明值 = 桥接层输出"对账测试 | 中 | 正是它让 V-04 逃过检测 |
| 批次 B1 实施记录未写 | 低 | AGENTS.md 要求 |

---

## 六、修复方案（待用户批准，尚未实施）

### 方案 A（推荐）：修上游取值，启用 A2L 来源

1. `a2l_dto_map.cpp:424` 改为 `layer->version = dto.protocol_version;`
2. 处理 `dto.protocol_version == 0`（A2L 未标注）→ 保持 `0`，由 `XcpProtocolVersion::IsValid()` 判为无效证据
3. 同步 `if_data_xcp.hpp:52` / `a2l_bridge.hpp:62` 默认值 `0x0100` → `0`（去掉伪装）
4. 补测试：合成声明 `0x201` 的 A2L，断言桥接层回报 `0x0201`
5. 补测试：`cpp_demo_V201_corrected.a2l` 断言桥接层回报 `0x0104`

**理由**：上游已正确，桥接层丢弃属实现错误；修复成本低、风险集中在 `a2lbridge` 自研层（可改），收益是 A2L fallback 真实可用。

**风险**：`0` 可能被现有代码当作合法版本；需核实 `IsValid()` 与所有消费点。

### 方案 B（保守）：禁用 A2L 来源

1. `XcpMaster` 不再把 A2L 版本传入 `VersionResolver`（或传入 `nullopt`）
2. `VersionSource::A2l` 分支保留但无生产入口
3. 文档明确标注 A2L 版本来源为 Unavailable

**理由**：符合计划 §4.1 的备选路径，零风险。

**代价**：放弃一个本可用的证据来源；`CompareWithRuntime` 的版本比对项失去意义。

### 方案 C：仅修 `CompareWithRuntime`，不动核心

治标不治本，**不推荐**。

---

## 七、决策结果

1. **方案 A**（用户 m04714 `我建议 A` / m04751 确认）
2. `dto.protocol_version == 0` 的处理：**已核实**——`include/libxcp/protocol_version.hpp:74`
   ```cpp
   [[nodiscard]] constexpr bool IsValid() const noexcept { return major != 0U; }
   ```
   `0x0000` 判为「未知占位/无证据」，**无需额外分支**，改默认值为 `0` 即自然落入该语义。
3. 范围限定（用户 m04751）：**只改 A2L 侧两处 + 映射函数**，`a2l_bridge.hpp:62` 的运行时侧默认值保持不动。

---

# 实施记录

## 八、实际改动清单（3 处）

| # | 文件:行 | 改动前 | 改动后 | 说明 |
|---|---|---|---|---|
| 1 | `thirdparty/a2l-sdk/a2lbridge/src/a2l_dto_map.cpp:431` | `layer->version = 0x0100;` | `layer->version = dto.protocol_version;` | **真凶修正**：透传上游解析结果 |
| 2 | `thirdparty/a2l-sdk/a2lbridge/include/libxcp/a2l/if_data_xcp.hpp:55` | `std::uint16_t version = 0x0100;` | `std::uint16_t version = 0;` | 默认值去伪装：0 = A2L 未标注 |
| 3 | `thirdparty/a2l-sdk/a2lbridge/include/libxcp/a2l/if_data_xcp.hpp:33` | `std::uint16_t version = 0x0100;` | `std::uint16_t version = 0;` | `TransportEndpoint::version`，同类问题 |

**保持不动**（按用户 m04751 限制）：`thirdparty/a2l-sdk/a2lbridge/include/libxcp/a2l/a2l_bridge.hpp:62` ——
`RuntimeXcpParams::protocol_version` 是**运行时**侧参数（CONNECT RES 的传输层版本），与 A2L 侧不共用语义，改它会改变"调用方未填"时的比对行为。

三处改动均带中文 Doxygen/行内注释，说明「为何不再硬编码」及「0 表示未声明而非伪造 V1.0」。

### 8.1 实施中的一个错误（已纠正，记录备查）

实施过程中曾写下 `ep.version = dto.if_data_version;` ——**该字段名是凭记忆猜测的**。

按 AGENTS.md「禁止凭记忆编码」立即读取 `thirdparty/a2l-sdk/include/liba2l/liba2l_api.hpp:394-437` 的真实 `IfDataXcpDto` 定义，确认：
- **不存在 `if_data_version` 字段**；
- 上游 `IfDataXcpDto` 只有一个 `protocol_version`（`:399`），**没有与 PROTOCOL_LAYER 分开的段版本字段**。

桥接层因此无处可取，**该行已还原**，改为仅修正默认值并在注释中说明原因。若凭猜测留下该行，会直接编译失败（或更糟——若恰有同名字段则静默取错值）。

## 九、新增回归测试（2 条）

位置：`tests/a2l_golden_test.cpp` 末尾（匿名命名空间内、末尾 `}  // namespace` 之前）。

设计意图：**专门把「A2L 文件里的声明」与「桥接层的输出」对账**——该对账此前完全缺失，正是 V-04 逃过检测的根因。

| 用例 | 输入 | 断言 |
|---|---|---|
| `V04A2lDeclaredVersionIsNotRewritten` | **真实文件** `tests/data/a2l/cpp_demo_V201_corrected.a2l`（第 178 行声明 `0x104`） | 桥接层回报 `0x0104` |
| `V04SyntheticNonV1VersionIsPropagated` | 临时合成 A2L，声明 `0x201` | 桥接层回报 `0x0201` |

两条用例的失败信息均写明「A2L 声明的 Vx.y 被改写；V-04 回归（应透传 dto.protocol_version）」，便于日后回归时一眼定位。

### 9.1 为什么此前 42 条 A2L 用例全部漏检（复述根因）

1. `tests/a2l_golden_test.cpp:746` 断言 `pl.version == 0x0100`，而 `golden_basic.a2l` **恰好也声明 `0x100`** —— 硬编码值与期望值**巧合一致**；
2. `VersionResolverTest` 的两个 A2L 用例**直接注入** `inputs.a2l_protocol_version`，**根本不过桥接层**。

反证实验（分析阶段已做）：把 golden 声明改成 `0x104` 重跑，旧测试**依然通过**——证明对 V-04 **零覆盖**。

### 9.2 构建踩坑（备查）

`tests/a2l_golden_test.cpp` 属于 **`A2lGolden`** 目标，**不属于** `libxcp_tests`（依据 `tests/CMakeLists.txt:194`：`add_executable(A2lGolden a2l_golden_test.cpp)`）。

首次用 `--target libxcp_tests` 构建后运行，得到 `Running 0 tests from 0 test suites`——**二进制未重编**，易误判为"测试未生效"。后续必须先 `--target A2lGolden`。

## 十、验证结果

### 10.1 变异测试（决定性证据）

将 `a2l_dto_map.cpp:431` 还原为写死 `0x0100`，重编 `A2lGolden` 后运行：

```
V04A2lDeclaredVersionIsNotRewritten:
    Expected equality: Which is: 256 (0x0100)   ← 实际
                       Which is: 260 (0x0104)   ← 期望
    A2L 声明的 V1.4 被改写；V-04 回归（应透传 dto.protocol_version）
    [  FAILED  ]

V04SyntheticNonV1VersionIsPropagated:
    Expected equality: Which is: 256 (0x0100)
                       Which is: 513 (0x0201)
    A2L 声明的 V2.1 被改写；V-04 回归
    [  FAILED  ]
```

**两条用例均如期失败，且数值精准指向硬编码值 `0x0100`。**证明新用例对 V-04 真实有效（非走过场）。

还原后重编，两条用例恢复 `OK`。

### 10.2 全量回归

| 套件 | 结果 | 判定 |
|---|---|---|
| `libxcp_tests` | 573 tests：**571 passed / 1 skipped / 1 failed** | 唯一失败为**既有 flake** `UdpTransportClose.CloseWhileBlockedReturnsPromptly`，与本改动无关 |
| `A2lGolden` | 44 tests：**42 passed / 2 skipped** | 新增 2 条已计入；2 skipped 为既有（`ParseLargeFileUnderBudget`、`Utf16LeBomExploratory`） |
| `XcpliteIntegration` | 49 tests：**48 passed / 1 skipped** | 确认与真实 XCPlite 跨进程互操作**不受影响** |

**无新增失败。**

### 10.3 附带确认

`A2lGoldenTest.UdpEndpointAndProtocolLayer` 的 `EXPECT_EQ(pl.version, 0x0100)`（`tests/a2l_golden_test.cpp:746`）**修复后依然通过**——因为 `golden_basic.a2l` 真实声明就是 `0x100`，语义不变。这反向印证了原用例的"巧合性通过"性质：它既没发现缺陷，修复后也无需改动。

## 十一、遗留与建议

| 项 | 状态 |
|---|---|
| `XcpMaster` 的 `VersionSource::A2l` 分支 | **仍无生产入口**（`inputs.a2l_protocol_version` 恒 `nullopt`）。本次未接线，属设计现状；若未来要让 A2L 参与版本裁决，需先在 `XcpMasterOptions` 增加 A2L 版本字段并显式传入 |
| `CompareWithRuntime` 版本比对项 | 本次修复后**已恢复真实语义**（不再恒判 `0x01`） |
| 与计划 §4.1 的偏差 | 计划假设「上游可能无法提供该字段」**不成立**，故未走备选路径（禁用来源），而是直接修正实现 |
| 待办 | 批次 B1 实施记录（另见 `code-plan/XCP_V1.5_批次B1_实施记录.md`）；批次 B2（T-01～T-08）、批次 G 未开始 |

---

## 附：证据文件清单

| 文件:行 | 内容 |
|---|---|
| `thirdparty/a2l-sdk/a2lbridge/src/a2l_dto_map.cpp:431` | **已修正**：原硬编码本体，现透传 `dto.protocol_version` |
| `thirdparty/a2l-sdk/a2lbridge/include/libxcp/a2l/if_data_xcp.hpp:33,55` | **已修正**：两处默认值 `0x0100` → `0` |
| `thirdparty/a2l-sdk/src/liba2l_export.cpp:1891-1892` | 上游正确取值 `dto->protocol_version = pl->GetVersion();` |
| `thirdparty/a2llib/src/xcp/xcpdataparser.y:190-193` | 语法层解析证据 `protocol.SetVersion($3)` |
| `thirdparty/a2llib/include/a2l/xcp/protocollayer.h:125-129` | `SetVersion`/`GetVersion` |
| `thirdparty/a2l-sdk/include/liba2l/liba2l_api.hpp:394-437` | `IfDataXcpDto` 真实定义（**无 `if_data_version` 字段**） |
| `src/version_resolver.cpp:55-63` | `VersionSource::A2l` 分支（生产路径不可达） |
| `src/xcp_master.cpp:508-519` | 生产侧唯一 `Resolve` 调用点（未赋 A2L 输入） |
| `thirdparty/a2l-sdk/a2lbridge/src/a2l_bridge.cpp:427,467` | `CompareWithRuntime` 版本比对（真实暴露面） |
| `tests/a2l_golden_test.cpp:746` | 巧合性通过的旧断言（修复后仍成立） |
| `tests/a2l_golden_test.cpp`（末尾新增） | **本次新增 2 条 V-04 回归用例** |
| `thirdparty/a2l-sdk/a2lbridge/include/libxcp/a2l/a2l_bridge.hpp:62` | `RuntimeXcpParams::protocol_version`（按裁定**保持不动**） |
| `tests/data/a2l/cpp_demo_V201_corrected.a2l:178` | 真实文件声明 `0x104` |
| `tests/CMakeLists.txt:194` | `A2lGolden` 目标定义（构建踩坑依据） |