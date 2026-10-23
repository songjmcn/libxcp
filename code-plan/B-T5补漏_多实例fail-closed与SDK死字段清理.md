# B-T5 补漏：多实例 fail-closed 与 SDK 死字段清理

## 0. 背景与范围

`B-T4_实施记录_A2L端点DTO的ABI-v9与TCP端点事实.md` 结尾登记了三条已知限制，用户在
追问细节后裁决"修 L3（多实例拦截）+ 删 L2（SDK 死字段）"，L1 不动 ABI、只固定契约。

| 编号 | 内容 | 本次处置 |
| --- | --- | --- |
| L1 | `endpoint_alignment_declared` 未参与 XCPplus 冲突比对 | **不改行为**，在 `DiffIfData()` 写死契约注释 |
| L2 | `endpoint_profile_hint` / `endpoint_profile_hint_declared` 仅占位 | **删除**两个字段（ABI v9 内完成，无消费者） |
| L3 | 多实例仍只取首个 | **修**：按计划 §7.3 fail closed（带去重放宽） |

依据：`code-plan/TCP传输_实施计划_B-T1至B-T5.md` §7.3（原第 540 行附近）。

---

## 1. L3：多实例在 CLI 未覆盖时 fail closed

### 1.1 问题复述

修改前 `tools/xcp_master/main.cpp` 的端点遍历是"先到先得"：

```cpp
++a2l_facts.instance_count;
if (a2l_facts.declared) continue;  // 多实例：先到先得，需状态提示
```

`A2lTransportFacts::instance_count > 1` 只在 `transport_selection.hpp` 里产出一条
"当前使用第一个"的 note，**不阻断**。选错实例不会报错，只表现为连接超时或读到的值
不对——现场无法定位到"端点选择"这一层，因此计划要求 fail closed。

### 1.2 新增数据结构（`tools/xcp_master/transport_selection.hpp`）

`A2lTransportFacts` 的标量字段表达的是"选中的那一个"，不足以判冲突，故新增逐实例明细：

```cpp
struct A2lEndpointInstance {
    std::string host;                        ///< 主机名/地址（已按回退链解析）
    std::uint16_t port = 0;                  ///< 端口
    std::uint8_t packet_alignment_bits = 0;  ///< 显式声明的位宽；0=未声明
    std::uint16_t transport_version = 0;     ///< `XCP_ON_*` 块首版本；0=未声明
};

[[nodiscard]] inline bool SameWireTarget(const A2lEndpointInstance& lhs,
                                         const A2lEndpointInstance& rhs) noexcept;

[[nodiscard]] inline std::string RenderEndpointTarget(const A2lEndpointInstance&);
```

- `SameWireTarget` 只比"会改变线路行为"的四要素（host/port/对齐位宽/TL 版本），
  **不比声明位**——去重问的是"配置是否唯一"，不是"证据强度是否相同"。
- `RenderEndpointTarget` 渲染 `host:port(align=8bit)/TL 1.4`，同时用于错误文本与 note。
- `A2lTransportFacts::instances`（`std::vector<A2lEndpointInstance>`）**默认空**：
  空表示调用方没提供明细，此时退回旧行为（仅提示）。这条约定让既有 23 项单测
  无需改造即可继续表达"只关心标量字段"的场景。

### 1.3 规则（同文件，位于规则 5 跨 kind 诊断之后、端点回落之前）

```
if (!host_given && a2l.instances.size() > 1U) {
    去重渲染 → distinct
    distinct.size() > 1 → fail("A2L 声明了 N 个不同的同类端点（A、B），
                                无法自动判定应连接哪一个；请用 --host/--port 显式指定")
    否则                → note("A2L 重复声明了 N 个相同的端点，按同一目标处理：X")
}
```

三种情形与用户裁决的语义表一致：

| 情形 | 行为 | 理由 |
| --- | --- | --- |
| CLI 给了 `--host`+`--port` | 不拦 | 意图明确，实例数无关 |
| 无 CLI，配置去重后全同 | 允许 + note | 生成器重复输出不该阻断 |
| 无 CLI，配置有差异 | fail closed | 计划 §7.3 要求 |

### 1.4 调用方填充（`tools/xcp_master/main.cpp`）

遍历 `xcp_info->transports` 时对**每一个**目标 kind 的端点追加明细：

```cpp
a2l_facts.instances.push_back(calmcar::xcp::tools::A2lEndpointInstance{
    endpoint.remote_host, endpoint.remote_port,
    endpoint.packet_alignment_declared ? endpoint.packet_alignment
                                       : static_cast<std::uint8_t>(0U),
    endpoint.version});
if (a2l_facts.declared) continue;  // 标量字段：先到先得
```

未显式声明对齐时记为 0，**不**把桥接层默认值 8 当成已声明——否则两个"其实没写
PACKET_ALIGNMENT"的端点会被误判为相同配置。

### 1.5 新增单测（`tests/xcp_master_transport_selection_test.cpp`，+3）

| 用例 | 断言要点 |
| --- | --- |
| `MultipleDistinctTcpInstancesFailClosed` | 失败；错误文本含「不同的同类端点」、两个实例的 `host:port`、`--host` 指引 |
| `IdenticalDuplicateInstancesAreAccepted` | 成功；端点来自 A2L；notes 含「重复声明」 |
| `CliEndpointBypassesMultiInstanceCheck` | 成功；`endpoint_source == Cli`，端口为 CLI 值 |

选择理由：三个用例正好覆盖语义表的三行，且都不依赖真实 socket。

---

## 2. L2：删除两个死字段

### 2.1 删除前的取证

```bash
grep -rn "endpoint_profile_hint" --include=*.cpp --include=*.hpp .
```

`.cpp` 侧**零命中**——导出层从未赋值，桥接层也未映射，因此
`endpoint_profile_hint_declared` 恒为 `false`。外部消费者会把它读成"A2L 没给候选"，
而真相是"SDK 从没尝试推断"：这是**会误导的 API 形态**，不是"待实现"。

判据本身（TL 版本）已在 `endpoint_transport_version` 中，再放一份渲染后的字符串属于
派生数据，且极易被误当证据。

### 2.2 改动

- `thirdparty/a2l-sdk/include/liba2l/liba2l_api.hpp`：物理删除 `endpoint_profile_hint`
  与 `endpoint_profile_hint_declared`（原 :488-497 一带）。
- 同文件 v9 变更注释块：删去两字段的提及，改为记录收口决策：
  > B-T5 收口：原本规划的 `endpoint_profile_hint`/`_declared` 两个**派生**字段已删除——
  > 候选 Profile 的判据（TL 版本）已在 `endpoint_transport_version` 中，再放一份渲染后的
  > 字符串只会被误当证据。
- `kLibA2lAbiVersion` **保持 9**：v9 是本批次尚未发布的版本，在同一批次内删字段
  不构成版本回退；若已发布，就必须升 v10。

### 2.3 保留的部分

CLI 层的诊断串 `A2lTransportFacts::profile_hint`（由 `main.cpp` 从 A2L 端点 TL 版本
渲染，`transport_selection.hpp` 只作为 note 输出）**保留**：它是 tools 层的展示数据，
已有 5 项单测覆盖，且明确"仅诊断、不参与 Profile 选择"。

---

## 3. L1：不改行为，只固定契约

`thirdparty/a2l-sdk/src/liba2l_export.cpp` 的 `DiffIfData()` 在 `PACKET_ALIGNMENT`
比对处新增契约注释，要点：

- 只报**数值**差异，绝不比对 `endpoint_alignment_declared`；
- 原因：XCP 侧显式 `PACKET_ALIGNMENT_8` 与 XCPplus 侧未声明（默认 8）数值相同、证据
  强度不同，而 `XcpPlusConflict` 只有三个字符串字段，无法表达"值同、声明状态不同"，
  硬塞只能伪造文本；纯声明差异的严重级别也低于数值差异，混进 `plus_conflicts` 会被
  消费方当成"可用性冲突"；
- 声明状态由 `endpoint_alignment_declared` 独立承担，消费方可经
  `libxcp::a2l::TransportEndpoint::packet_alignment_declared` 自行判别。

附带更正：B-T4 汇报里"纳入比对会改变既有 3 条差异契约"的说法**不准确**。既有 golden
（`tests/a2l_golden_test.cpp:1207` 的 MAX_CTO/PACKET_ALIGNMENT/SUB_CMDS）两侧都显式
声明了对齐，declared 位相同，数字不会变；真实阻挡是"结构表达不了"而非"断言会红"。

---

## 4. 测试结果（macOS Release）

### 4.1 编译

`cmake --build cmake-build-release -j 4` → 成功，无新增 warning/error。

### 4.2 规则层单测

```
./XcpMasterSymbolPolicyTest --gtest_filter='TransportSelection.*'
→ 26 tests PASSED（原 23 + 新增 3）
```

### 4.3 全量回归

```
cd cmake-build-release && ctest --output-on-failure -j 4
→ 99% tests passed, 3 tests failed out of 954（总耗时 54.59 s）
```

失败的 3 项与本次改动无关，均为既有问题：

| 测试 | 原因 |
| --- | --- |
| `a2l_perf_gen` (#929) | 需要 Python 3.10+，本机捆绑解释器不满足 |
| `ParseLargeFileUnderBudget` (#930) | 依赖上一条的 fixture，未生成故 Not Run |
| `ExampleXcpIntegrationDemoCppDemo` (#946) | 本批次之前即失败（示例程序问题） |

用例总数由 951 → 954（+3）；`XcpMasterSymbolPolicyTest` 单目标 100 项全绿。

### 4.4 A2L 面验证

字段删除后 `grep -rn "endpoint_profile_hint"` 仅剩 `liba2l_api.hpp` 的变更注释一处。
`A2lGolden` / `A2lE2E` 在 §4.3 的全量回归中通过，未出现"A2L 全套需改造"的情况——
因为这两个字段从未被读写。

---

## 5. 影响面与回滚

- **ABI**：`liba2l_api.hpp` 删两个字段，但版本号仍为 9（本批次未发布）。若下游已按
  v9 预发布版接入，需要求其同步更新；否则只能升 v10。
- **行为**：`xcp_master` 在"A2L 多个不同 TCP 端点 + 未给 CLI 端点"时会**从成功变为
  失败**。这是有意的收紧，错误文本给出各实例与修复方法。使用 `--host`/`--port`
  的工作流不受影响。
- **回滚**：`A2lTransportFacts::instances` 默认空，规则块整体删除即回到旧行为；
  SDK 字段可从 v9 注释块的原始描述恢复。

---

## 6. 未做与后续建议

1. **L1 的结构性方案未做**：若要真正报告"值同、声明状态不同"，需要给
   `XcpPlusConflict` 增字段 = ABI v10，本期判断不值得。
2. **多实例的"用户选择"通道未做**：当前只支持"CLI 显式指定"作为决策手段；若产品
   需要 `--a2l-endpoint-index N` 这类选择器，应作为独立需求评估（涉及 A2L 侧实例
   顺序的稳定性契约，比 fail closed 复杂得多）。
3. **多实例 golden fixture 缺失**：现有 golden 是两个不同 `IF_DATA` 块各声明一个端点
   （`tests/a2l_golden_test.cpp:943/953`），不是同一块内多实例。因此 §1.5 的三个用例
   全部在规则层（纯函数）验证，未覆盖"A2L 解析 → 桥接 → facts"的完整链路。若后续要
   补，需要在 golden 生成器里新增一个"单块多端点"变体。