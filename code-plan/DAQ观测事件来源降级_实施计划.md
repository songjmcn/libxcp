# DAQ 观测事件来源降级（方案 A）实施计划

- 日期：2026-10-26
- 触发问题：加载 `apa_complex.a2l`（87 MB，模块 `apa_complex`）后启动 DAQ 模式，绝大多数变量无法加入观测，提示「观测拒绝：需要唯一 FIXED_EVENT_LIS…」
- 方案：**A —— 只放宽“事件候选来源”的取舍规则，不引入任何新交互**
- 状态：**已批准**（2026-10-26）；已并入 XCP Protocol Layer v1.5.0 规范复核结论与三项裁决，按 §9 分期实施

---

## §0 结论速览

| 项 | 内容 |
| --- | --- |
| 根因 | `tools/xcp_master/symbol_policy.hpp` 只接受“唯一 `FIXED_EVENT_LIST`”，且注释明确“不回退到 DEFAULT/AVAILABLE”；客户 A2L 100% 使用 `DEFAULT_EVENT_LIST` |
| 规范依据 | ASAM MCD-1 XCP Protocol Layer v1.5.0 §4.1.5 EVENT CHANNELS（`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.md:516-544`）：`FIXED_EVENT_LIST` 时“工具不得更改分配”；`DEFAULT_EVENT_LIST` 是“默认分配，工具可以改成别的分配”；**无 `DEFAULT` 时 `CONSISTENCY_EVENT_LIST` “shall be used as default”**；**`DEFAULT`/`CONSISTENCY` 必须是 `AVAILABLE` 的子集（must）** |
| 规范归属 | 事件列表**不在 A2L 规范**（MCD-2 MC V1.7.0 全文 `DAQ_EVENT` 等关键字 0 命中），仅经 §4.7.82 `IF_DATA` 转授权给接口规范（XCP）；详见 §1.3 |
| 方案 A 规则 | `FIXED` 去重后唯一 → 用它；否则 `DEFAULT` 唯一 → 用它；否则 `CONSISTENCY` 唯一 → 用它；三者皆无 → 拒绝。另有前置自洽性校验（`DEFAULT`/`CONSISTENCY` ⊆ `AVAILABLE`） |
| 是否需新交互 | **否**（不提供事件选择器、不支持多事件采样——那些属方案 B） |
| 是否动测量内核 | **否**（`EventBoundMeasurementDatabase` 的单事件约束原样保留） |
| 覆盖面（客户 A2L） | 解锁 **195,990** 个 MEASUREMENT（每一个的 `DEFAULT_EVENT_LIST` 都恰含 1 个事件） |
| 覆盖不到的部分 | **836** 个 CHARACTERISTIC 仍被「DAQ 仅支持 MEASUREMENT」拒绝——这是**另一条独立策略**，不在本方案范围（见 §8） |
| ABI | **10 → 11**（`IfDataXcpDto` 新增 `daq_consistency_event`，与既有三个 `daq_event_*` 同风格：空 = 未声明；其余字段全部保留） |

---

## §1 问题定性与证据

### §1.1 客户 A2L 的事件声明形态（工具实测）

| 项 | 数量 |
| --- | --- |
| `/begin MEASUREMENT` | 195,998（其中带 `DAQ_EVENT` 的 195,990） |
| `/begin CHARACTERISTIC` | 2,558（其中带 `DAQ_EVENT` 的 836） |
| `/begin DAQ_EVENT` 合计 | 196,826 |
| `FIXED_EVENT_LIST` | **0** |
| `DEFAULT_EVENT_LIST` | **196,826**（每块恰含 1 个 `EVENT`） |
| `AVAILABLE_EVENT_LIST` | 0 |
| `CONSISTENCY_EVENT_LIST` | 0 |
| `/begin EVENT`（事件元数据） | 5（`event_20ms`=0x0、`event_40ms`=0x1…0x4） |

典型声明：

```
/begin DAQ_EVENT VARIABLE /begin DEFAULT_EVENT_LIST EVENT 0x1 /end DEFAULT_EVENT_LIST /end DAQ_EVENT
```

只读探针（`.tmp/probe/event_probe`，仅解析 A2L）对 `apa_control_input...time_stamp` 的输出：

```
fixed=<空>| default=1 | available=<空>
```

### §1.2 代码定位

| 位置 | 现状 |
| --- | --- |
| `tools/xcp_master/symbol_policy.hpp:66-72` | 取 `daq_event_fixed`，去重后要求 `size()==1`，否则返回「需要唯一 FIXED_EVENT_LIST；不回退到 DEFAULT/AVAILABLE。」 |
| `tools/xcp_master/main.cpp:3046` | `item.channel == symbol.daq_event_fixed.front()`（watch 时按 fixed 匹配事件元数据） |
| `tools/xcp_master/main.cpp:3532-3533` | `candidates.assign(symbol.daq_event_fixed.begin(), end)`（DAQ 会话候选事件唯一来源） |
| `tools/xcp_master/main.cpp:2134` / `:2175` | 仅把 `daq_event_fixed` 打进诊断日志 |
| `daq_event_default` / `daq_event_available` | 全仓**只出现在诊断日志**（`:2176`/`:2177`），从不参与决策 |
| `thirdparty/a2l-sdk/src/liba2l_export.cpp:944-950` | CHARACTERISTIC 分支**只导出 `daq_event_fixed`**，`default`/`available` 未赋值；MEASUREMENT 分支（`:723-727`）三张表都导出 → 两分支不一致 |

### §1.3 规范原文与归属

**先说归属**：事件列表的定义不在 A2L 规范里。`docs/ASAM_AE_MCD-2_MC_BS_V1-7-0.pdf`（295 页）中 `DAQ_EVENT`、`FIXED_EVENT_LIST`、`DEFAULT_EVENT_LIST`、`AVAILABLE_EVENT_LIST`、`CONSISTENCY_EVENT_LIST` 全部 0 命中；它只在 §4.7.82 `IF_DATA`（p.142）把内容转授权给接口规范：

> `ident`: Name identifier of Interface …
> `-> ...`: Data record to describe interface specific data. **The parameters associated with this keyword have to be described in the ASAM MCD-2 MC metalanguage.**

语义定义在 ASAM MCD-1 XCP Protocol Layer v1.5.0 §4.1.5（`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.md:530-544`）：

> If the assignment shall be fixed, a list with all event channels to be used (**FIXED_EVENT_LIST**) must be defined … The tool cannot change the assignment of the event channels for a MEASUREMENT variable with a fixed list.

> If the assignment shall not be fixed but variable, a list with all valid events channels … (**AVAILABLE_EVENT_LIST**) can be provided … In case this list does not exist, all event channels provided by the ECU can be assigned by the tool.

> A default assignment … can be supported by providing a list with the default event channels (**DEFAULT_EVENT_LIST**). This default assignment can be changed by the tool to a different assignment.

> A list of consistency events can be provided locally at MEASUREMENT (**CONSISTENCY_EVENT_LIST**). All MEASUREMENT variables, that are assigned to the same Event Channel defined in a CONSISTENCY_EVENT_LIST, are sampled consistently itself and to each other in this Event Channel.

> **IF no DEFAULT_EVENT_LIST is defined locally at MEASUREMENT, the CONSISTENCY_EVENT_LIST shall be used as default Event Channel assignment.**

> **In case an AVAILABLE_EVENT_LIST is defined, the event channels in the DEFAULT_EVENT_LIST and in the CONSISTENCY_EVENT_LIST must be the same or a sub-set of the event channels in the AVAILABLE_EVENT_LIST for this MEASUREMENT variable.**

> Lists are possible as some MCD tools allow measurement in multiple events.

由此得到三条结论（后两条是本轮复核新增）：

1. 只有 `FIXED_EVENT_LIST` 构成“不可更改”的约束；`DEFAULT_EVENT_LIST` 只是建议值 → 当前实现把“必须有固定事件”当硬前提，属自我加严。
2. `CONSISTENCY_EVENT_LIST` 有**双重身份**：既是“一致性采样组”，又是 `DEFAULT` 缺失时的**默认分配来源**（`shall`，强制）。当前实现未导出该字段，会误拒只声明 consistency 的合法 A2L。
3. `AVAILABLE_EVENT_LIST` 的**子集约束是 `must`**（规范性要求），不是可选的锦上添花；工具应校验并在违反时报“A2L 事件声明自相矛盾”。

---

## §2 目标与非目标

### §2.1 目标

- **G1** 对只声明 `DEFAULT_EVENT_LIST` 的 MEASUREMENT，DAQ 观测可用（客户 A2L 主场景）。
- **G2** `FIXED_EVENT_LIST` 的既有行为**逐字节不变**（XCPlite 系从站与全部既有用例不受影响）。
- **G3** 拒绝时给出可诊断的准确原因，区分「未声明任何事件」「声明了多个且无法判定」「事件号不在 EVENT 列表」。
- **G4** 让“当前事件来自 FIXED 还是 DEFAULT”在界面与日志里可见（用户需知道工具有没有替 ECU 改分配）。

### §2.2 非目标（本方案不做）

- 不提供事件通道选择界面；不支持用户改分配（规范允许，属方案 B）。
- 不支持「三张表都没有 → 回退到 ECU 全部事件」（属方案 B）。
- 不支持同一 MEASUREMENT 挂多个事件（属方案 B，需改 `MeasurementSymbolInfo.event_channel` 单值模型）。
- 不做 `CONSISTENCY_EVENT_LIST` 的**一致性采样保证**（只把它当作默认来源使用；跨事件的“同组一致采样”属方案 B，见 §8）。
- **不放开 CHARACTERISTIC 的 DAQ 准入**（§8 单列）。

---

## §3 语义规则（本方案的全部规则）

对每个符号解析“观测事件候选”，**先做自洽性校验（规则 0），再按来源优先级取候选（规则 1–5），最后做事件通道元数据校验（规则 6）**。所有列表均已去重升序。

**规则 0（前置自洽性，违反即拒绝）**

| 条件 | 结果 |
| --- | --- |
| `available` 非空且 `default` 存在任一元素 ∉ `available` | 拒绝：「A2L 事件声明自相矛盾：`DEFAULT_EVENT_LIST` 不在 `AVAILABLE_EVENT_LIST` 内」，并列出两侧事件号 |
| `available` 非空且 `consistency` 存在任一元素 ∉ `available` | 拒绝：「A2L 事件声明自相矛盾：`CONSISTENCY_EVENT_LIST` 不在 `AVAILABLE_EVENT_LIST` 内」 |
| `available` 为空 | 跳过本规则（规范：未声明时 ECU 全部事件均可分配） |

**规则 1–5（来源降级）**

| 顺序 | 条件 | 结果 | `source` |
| --- | --- | --- | --- |
| 1 | `fixed.size() == 1` | 采用该事件 | `Fixed` |
| 2 | `fixed` 为空且 `default.size() == 1` | 采用该事件 | `Default` |
| 3 | `fixed` 非空且 `size() != 1` | 拒绝：「`FIXED_EVENT_LIST` 声明了多个事件，本工具无法自动判定」 | — |
| 4 | `fixed` 空、`default` 非空且 `size() != 1` | 拒绝：「`DEFAULT_EVENT_LIST` 声明了多个事件，本工具无法自动判定」 | — |
| 5 | `fixed` 与 `default` 均空、`consistency.size() == 1` | 采用该事件（规范：无 `DEFAULT` 时 `CONSISTENCY` 作默认分配） | `Consistency` |
| 5b | `fixed` 空、`default` 空、`consistency` 非空且 `size() != 1` | 拒绝：「`CONSISTENCY_EVENT_LIST` 声明了多个事件，本工具无法自动判定」 | — |
| 5c | 三表皆空 | 拒绝：「A2L 未声明 `FIXED_EVENT_LIST`、`DEFAULT_EVENT_LIST` 或 `CONSISTENCY_EVENT_LIST`，无法确定 DAQ 事件通道」 | — |
| 6 | 规则 1/2/5 命中，但事件号不在 `available_events`（ECU 事件通道表） | 拒绝：「A2L 缺少对应 EVENT_CHANNEL 元数据。」（沿用现文本） | — |

补充约定：

- **`AVAILABLE_EVENT_LIST` 现在参与决策**：规则 0 的自洽性校验用它（`must` 子集约束），但**不作为事件候选来源**（规范允许“全 ECU 事件”只是允许，方案 A 不做自由分配）。
- **去重**：`{3,3}` 视为唯一事件 3（与现状一致）。
- **`fixed` 与 `default` 同时存在且互相矛盾**：以 `FIXED` 为准（规范：fixed 时工具不得改）。
- 规则 3 优先于规则 4/5：只要 `fixed` 声明了多个事件，即使 `default`/`consistency` 唯一也不采用（否则等于工具绕过了 fixed 约束）。
- 规则 5 的**行为变更点**：此前“`fixed`/`default` 皆空即拒绝”，现在会先看 `consistency`；客户的 `apa_complex.a2l` 的 `CONSISTENCY_EVENT_LIST` 为 0，故不受此规则影响。

---

## §4 改动清单

### §4.1 `tools/xcp_master/symbol_policy.hpp`（核心，纯函数、无 IO）

新增（放在 `SymbolDaqObservationUnsupportedReason` 之前）：

```cpp
/// @brief DAQ 观测事件候选的来源标记。
enum class DaqEventSource : std::uint8_t {
    None,        ///< 未声明任何可用事件列表
    Fixed,       ///< FIXED_EVENT_LIST（规范规定工具不得更改分配）
    Default,     ///< DEFAULT_EVENT_LIST（规范允许工具更改，本工具不提供改分配入口）
    Consistency  ///< CONSISTENCY_EVENT_LIST（无 DEFAULT 时按规范充当默认分配）
};

/// @brief 规范化后的 DAQ 事件候选。
struct DaqEventCandidate {
    std::vector<std::uint16_t> channels;  ///< 去重后升序的事件通道
    DaqEventSource source = DaqEventSource::None;  ///< 来源
};

/// @brief 按 §3 规则解析符号的 DAQ 事件候选。
[[nodiscard]] inline DaqEventCandidate ResolveDaqEventCandidate(
    const a2l::SymbolInfo& symbol);
```

改造 `SymbolDaqObservationUnsupportedReason`（`:45` 起）：把现有 `:66-72` 的固定事件判断替换为调用 `ResolveDaqEventCandidate` + 按 §3 生成原因文本；其余检查（kind、维度、Entry 上限、`available_events` 命中）**顺序与文案保持不变**。

`ResolveDaqEventCandidate` 内部：拷贝三张表 → 各自 `std::sort` + `std::unique` → **先做规则 0 自洽性校验**（`available` 非空时校验 `default`/`consistency` ⊆ `available`，违反时返回 `source=None` 并把矛盾描述放进 `error`）→ 再按规则 1–5 选来源。签名扩为：

```cpp
/// @brief 规范化后的 DAQ 事件候选。
struct DaqEventCandidate {
    std::vector<std::uint16_t> channels;           ///< 去重后升序的事件通道
    DaqEventSource source = DaqEventSource::None;  ///< 来源
    std::string error;                             ///< 非空表示解析失败的原因（规则 0/3/4/5b/5c）
};

/// @brief 按 §3 规则解析符号的 DAQ 事件候选（纯函数，不做 IO）。
[[nodiscard]] inline DaqEventCandidate ResolveDaqEventCandidate(
    const a2l::SymbolInfo& symbol);
```

**保持“纯静态事实解析”与“运行时校验”解耦**：本函数不查 `available_events`（ECU 事件通道表），该步仍由 `SymbolDaqObservationUnsupportedReason` 在规则 6 完成，便于单测。

### §4.2 `tools/xcp_master/main.cpp`

| 位置 | 改动 |
| --- | --- |
| `:3046` | `item.channel == symbol.daq_event_fixed.front()` → 先 `const auto candidate = tools::ResolveDaqEventCandidate(symbol);`，`candidate.channels` 为空则沿用既有拒绝分支（防御性，正常已被 `ObservationUnsupportedReason` 拦下），否则按 `candidate.channels.front()` 匹配事件元数据 |
| `:3532-3533` | `candidates.assign(symbol.daq_event_fixed...)` → 用 `ResolveDaqEventCandidate(symbol).channels` |
| `:2134` | 预览 `info` 输出的 `fixed-event=` 后追加 ` event-source=`（`fixed`/`default`/`consistency`/`none`） |
| `:2175-2177` | 诊断日志同行追加 `event-source=`，便于现场区分“工具是否采用了 DEFAULT/CONSISTENCY” |
| watch 成功提示 | 当 `source == Default` 或 `Consistency` 时追加说明：「事件通道 N（来自 A2L 默认事件／一致性事件，工具未改动）」——见 §7.2(a) |

**不加锁、不改线程模型**：`ResolveDaqEventCandidate` 是纯函数，只读 `SymbolInfo`。

### §4.3 `thirdparty/a2l-sdk`：导出 `CONSISTENCY_EVENT_LIST`（ABI 10 → 11）

a2llib **早已解析**该字段（`thirdparty/a2llib/include/a2l/xcp/daqevent.h:35/38` 的 `SetConsistencyEventList` / `GetConsistencyEventList`，返回 `DaqEventList = std::vector<uint16_t>`），缺的只是导出与映射。**a2llib 零改动。**

| 文件 | 改动（行号为当前实测） |
| --- | --- |
| `thirdparty/a2l-sdk/include/liba2l/liba2l_api.hpp` | `kLibA2lAbiVersion` 10 → **11**（附 v11 变更注释）；`IfDataXcpDto` 在 `daq_event_available`（:346）之后增 `std::vector<std::uint16_t> daq_consistency_event;` —— 与既有三字段同风格，**空 = 未声明**，不另设 `declared` 标志 |
| `thirdparty/a2l-sdk/src/liba2l_export.cpp` | MEASUREMENT 分支 `:725-727` 补一行 `s.daq_consistency_event.assign(consistency.begin(), consistency.end());`；CHARACTERISTIC 分支 `:947` 现只有 `fixed`，按 §4.4 一并补齐（含 consistency） |
| `thirdparty/a2l-sdk/a2lbridge/include/libxcp/a2l/a2l_types.hpp` | `SymbolInfo` 在 `daq_event_available`（:240）之后增同名 `std::vector<std::uint16_t> daq_consistency_event;` |
| `thirdparty/a2l-sdk/a2lbridge/src/a2l_dto_map.cpp` | `:284-286` 之后补 `info.daq_consistency_event = dto.daq_consistency_event;` |

ABI 说明：新字段追加在结构体字段末尾，但 `IfDataXcpDto` 是跨库传递的值类型，按既有约定仍将 `kLibA2lAbiVersion` 升到 11；旧字段、旧语义全部保留，调用方只需重编译。

### §4.4 `thirdparty/a2l-sdk/src/liba2l_export.cpp`（CHARACTERISTIC 事件导出补齐，行为中性）

`:947` 的 CHARACTERISTIC 分支目前**只导出 `daq_event_fixed`**，与 MEASUREMENT 分支（`:725-727`）不一致：

```cpp
const auto& fixed = event->GetFixedEventList();
const auto& defaults = event->GetDefaultEventList();
const auto& available = event->GetAvailableEventList();
const auto& consistency = event->GetConsistencyEventList();
d.daq_event_fixed.assign(fixed.begin(), fixed.end());
d.daq_event_default.assign(defaults.begin(), defaults.end());
d.daq_event_available.assign(available.begin(), available.end());
d.daq_consistency_event.assign(consistency.begin(), consistency.end());
```

理由：两分支字段填充不一致会让上层（含诊断工具）误判“A2L 未声明事件”。此改动**不改变** CHARACTERISTIC 能否 DAQ 的结论（那受 `symbol_policy.hpp` 的独立策略限制，见 §8）。

### §4.5 不改动的部分（明确列出，避免误改）

- `adapter/a2l/measurement_runtime_profile.cpp:48-52`：`EventBoundMeasurementDatabase::Find` 仍要求“去重后恰好 1 个通道”，**原样保留**。
- `adapter/a2l/measurement_runtime_profile.cpp:177`（A2L 声明层 `max_daq_list == 0`）与 `:194`（运行时 `GET_DAQ_EVENT_INFO` 的 `max_daq_lists == 0`）：**双层校验原样保留**，方案 A 直接复用，不新增判据；`0xFF` 表示“不限”的语义已正确处理，不改。
- `src/measurement/measurement_planner.cpp:240-241`（`prescaler = 1`、`priority = 0`）：原样保留，对外口径写入 §8。
- `include/libxcp/` 下任何公开头与 `src/`：**零改动**（本方案不触碰测量内核与传输层）。
- `thirdparty/a2llib`：**零改动**（它早已解析 consistency，缺的是导出）。

---

## §5 测试方案

### §5.1 单元测试 `tests/xcp_master_symbol_policy_test.cpp`

改造既有用例 `DaqRequiresUniqueKnownFixedEvent`（`:97`）——它当前断言“只有 DEFAULT 也必须被拒”，是本方案的**契约变更点**。拆成：

| 用例 | 断言 |
| --- | --- |
| `DaqPrefersUniqueFixedEvent` | fixed={3} + default={4} → 合格；`ResolveDaqEventCandidate` 的 `source==Fixed`、`channels=={3}` |
| `DaqFallsBackToUniqueDefaultEvent` | fixed={} + default={3} → **合格**；`source==Default` |
| `DaqRejectsAmbiguousFixedEvents` | fixed={3,4} + default={3} → 拒绝，文本含 `FIXED_EVENT_LIST` 与“多个” |
| `DaqRejectsAmbiguousDefaultEvents` | fixed={} + default={3,4} → 拒绝，文本含 `DEFAULT_EVENT_LIST` 与“多个” |
| `DaqRejectsWhenNoEventDeclared` | 三张表皆空 → 拒绝，文本含 `FIXED_EVENT_LIST`、`DEFAULT_EVENT_LIST` 与 `CONSISTENCY_EVENT_LIST` |
| `DaqRejectsEventOutsideEventChannelList` | fixed={9}、available={3} → 文本含 `EVENT_CHANNEL`（沿用现断言） |
| `ResolveDaqEventCandidateDeduplicates` | fixed={3,3} → `channels=={3}`、`source==Fixed` |
| `DaqFallsBackToConsistencyWhenDefaultAbsent` | fixed={} + default={} + consistency={2} → **合格**；`source==Consistency`（规范 `shall`） |
| `DaqRejectsAmbiguousConsistencyEvents` | fixed={} + default={} + consistency={2,3} → 拒绝，文本含 `CONSISTENCY_EVENT_LIST` 与“多个” |
| `DaqRejectsDefaultOutsideAvailable` | default={4} + available={3} → 拒绝，文本含「自相矛盾」与两侧事件号（规则 0） |
| `DaqRejectsConsistencyOutsideAvailable` | consistency={4} + available={3} → 拒绝，文本含「自相矛盾」 |
| `DaqAcceptsDefaultSubsetOfAvailable` | default={3} + available={3,4} → 合格；`source==Default`（规则 0 放行） |
| `RejectsInconsistentA2lEvenWhenFixedIsUnique` | fixed={3} + default={9} + available={3} → 拒绝（规则 0 无条件校验） |

补充：**规则 0 是无条件校验**——它约束的是“这份 A2L 是否自洽”，与工具最终采用哪个列表无关。即使 `fixed` 唯一（本次决策不需要 `default`），只要 `default` 或 `consistency` 越出了 `available`，仍拒绝并提示 A2L 矛盾，理由与多实例 fail closed 同源：**基于自相矛盾的 A2L 做出的任何事件选择都不可信**。对应用例：`RejectsInconsistentA2lEvenWhenFixedIsUnique`（fixed={3} + default={9} + available={3} → 拒绝，文本含「自相矛盾」）。

### §5.2 集成测试 `tests/xcp_slave_demo_integration_test.cpp`

1. 改造 `GeneratedA2lPropagatesFixedEventsToMeasurementLeaves`（`:956`）中 `:1162-1180` 的 `default_only` 段：原断言“必须被拒” → 改为「被接受，且 `daq_event_default == {0}`、`daq_event_fixed` 为空」，并保留“修改快速实例不影响慢速实例”的断言。
2. **新增端到端用例**（本方案的核心验收，复刻客户场景）：生成 XCPlite 示例 A2L → 把 `FIXED_EVENT_LIST EVENT 0x0` 文本替换为 `DEFAULT_EVENT_LIST EVENT 0x0` → 起从站 → 主站 `--mode` DAQ 路径加入观测并 `watch` → 断言能解码出非零/递增样本。从站不读 A2L，因此该改动只影响主站决策，天然构成“仅 DEFAULT 也能真采样”的证明。
3. **新增 consistency 端到端用例**：同样替换为 `CONSISTENCY_EVENT_LIST EVENT 0x0`（无 `DEFAULT`），断言走 `source==Consistency` 且仍能真采样 —— 用于证明“规范兜底路径”在真实链路上成立，而不是只在纯函数层通过。
4. **ABI 断言**：新增 `tests/a2l_daq_event_lists_test.cpp`（见 G-A5），在 A2L 侧锁定四个列表的字段分离，防止后续把 consistency 混进 fixed/default。

### §5.3 门禁

| Gate | 判据 |
| --- | --- |
| G-A1 | `XcpMasterSymbolPolicyTest` 全绿（含上述新增/改造） |
| G-A2 | 客户 A2L 只读验证：对抽样的 MEASUREMENT（含 `period_switch` 这类）解析出 `source==Default`、`channels` 恰 1 个；并统计 195,990 个 MEASUREMENT 全部满足 |
| G-A3 | §5.2 第 2 条端到端用例通过 |
| G-A4 | `ctest -E "^Example" -j 4` 无新增失败（基线：1027 项全绿，2 项既有 Skip） |
| G-A5 | `kLibA2lAbiVersion == 11`；A2L 侧验证 `consistency_event` 已透出：新增 `tests/a2l_daq_event_lists_test.cpp`，用内联极小 A2L 覆盖「三表 + consistency 各自声明」「只声明 consistency」两形态，并断言 `SymbolInfo.daq_consistency_event` 与 `daq_event_*` 正确分离 |

---

## §6 兼容性影响

| 场景 | 变更前 | 变更后 |
| --- | --- | --- |
| XCPlite 系 A2L（`FIXED_EVENT_LIST` 唯一） | 可观测 | **完全不变** |
| 客户 `apa_complex.a2l`（`DEFAULT_EVENT_LIST` 唯一，195,990 个 MEASUREMENT） | 全部拒绝 | **全部可观测** |
| 只声明 `CONSISTENCY_EVENT_LIST` 的 A2L | 拒绝（误判为“未声明事件”） | **可观测，`source==Consistency`** |
| `DEFAULT`/`CONSISTENCY` 越出 `AVAILABLE` 的矛盾 A2L | 静默放行 | **拒绝并指明矛盾**（规范 `must`） |
| 三张表都无声明 | 拒绝（文本指向 FIXED） | 拒绝（文本指向 FIXED/DEFAULT/CONSISTENCY 皆缺） |
| `--mode poll` 的 watch / F3 详情读 | 可用 | **完全不变**（`ObservationUnsupportedReason` 仅在 `mode=="daq"` 时走 DAQ 策略） |
| 库 API / ABI | ABI 10 | **ABI 11**（`IfDataXcpDto` 增 `daq_consistency_event`；旧字段零删除、零改义；调用方需重编译） |

---

## §7 裁决记录

### §7.1 已裁决（据此定稿本计划）

| 日期 | 问题 | 结论 |
| --- | --- | --- |
| 2026-10-26 | 方案 A 还是方案 B | **A**（只放宽事件来源取舍，不提供事件选择器、不做自由分配） |
| 2026-10-26 | `CONSISTENCY_EVENT_LIST` 作默认来源是否纳入本次 | **纳入**，接受 `kLibA2lAbiVersion` 10 → 11（规范 `shall`，且 a2llib 已解析、只需补导出） |
| 2026-10-26 | `AVAILABLE_EVENT_LIST` 子集校验是否纳入 | **纳入**（规范 `must`，不动 ABI，只改 `tools/xcp_master`） |
| 2026-10-26 | `prescaler = 1` 的对外口径 | **同意**在文档与界面写明“DAQ 模式按事件原生周期采样”（见 §8 第 5 条） |

### §7.2 仍需你确认（不影响 §9 P1–P2 开工）

- **(a) 界面是否标注事件来源？** 建议做：`source == Default`/`Consistency` 时在 watch 成功提示与 `info` 输出里写明来源。成本约 10 行，收益是现场能一眼区分“ECU 建议的事件”“规范兜底的事件”和“工具擅自选的”。若不做，则只保留在诊断日志里。
- **(b) 是否同期放开 CHARACTERISTIC 的 DAQ 准入？** 建议**不做**（属独立策略，影响面更大：需重新评估写保护、段页、数组规则）。
- **(c) 是否同期补齐 CHARACTERISTIC 的事件导出（§4.4）？** 建议做（行为中性，消除两分支不一致），但它**不改变** CHARACTERISTIC 能否 DAQ 的结论。

---

## §8 已知限制（明确写清，避免再次误判）

1. **836 个 CHARACTERISTIC 仍不可 DAQ —— 这是本工具的策略限制，不是 A2L 或 XCP 的约束**：`tools/xcp_master/symbol_policy.hpp:48-49` 的「DAQ 仅支持 MEASUREMENT」属独立策略。MCD-2 MC 允许 CHARACTERISTIC 携带 `IF_DATA`（p.19/p.20/p.22 参数表中标 `X`＝可多次），XCP 的 DAQ 也只关心地址与长度，两者都不禁止。客户 A2L 中这类符号（如 `algo_status_info.apa_fusion_status.period_switch`）即使事件声明齐全也会被拒，本方案不改变这一点。
2. **没有回退到“ECU 全部事件”**：三张表都为空的 A2L 仍不可 DAQ。规范允许（`In case this list does not exist, all event channels provided by the ECU can be assigned by the tool`），但那是工具的自由分配能力，属方案 B。
3. **不支持一个变量挂多个事件**：内核 `EventBoundMeasurementDatabase` 强制唯一通道（`adapter/a2l/measurement_runtime_profile.cpp:48-52`）；而规范明确允许（`Lists are possible as some MCD tools allow measurement in multiple events`）。
4. **多个不同事件一律拒绝**：`FIXED`/`DEFAULT`/`CONSISTENCY` 任一列表去重后若 >1 个事件即拒绝（客户 A2L 的 `DEFAULT` 全部恰含 1 个，已统计 196,826 例）。规范下这类声明是合法的，属能力限制。
5. **`prescaler` 恒为 1（`src/measurement/measurement_planner.cpp:240`）**：DAQ 模式的实际采样率 = 事件原生周期，**界面上的采样周期在 DAQ 模式不生效**（不会通过降频因子实现）；master 侧只做限流/丢弃。需要更低速率时只能用更慢的事件通道。`priority` 同样恒为 0（`:241`），即 DAQ list 优先级最低。
6. **`CONSISTENCY_EVENT_LIST` 只作默认来源，不提供一致性采样保证**：本方案用它解决“无 `DEFAULT` 时事件从哪来”，但同一一致性组的变量跨事件采样的一致性（规范 §4.1.5 第 5 段）未实现——若做多变量相关性分析，跨事件样本可能不同步，属方案 B。

---

## §9 实施分期与回滚

### §9.1 分期（6 步，P0 先做 SDK 以便上层有数据可读）

| 步 | 内容 | 完成判据 |
| --- | --- | --- |
| P0 | `thirdparty/a2l-sdk`：ABI 10→11、`daq_consistency_event` 字段（`liba2l_api.hpp` / `a2l_types.hpp`）、两处导出（`:725-727`、`:947`）、桥接映射（`a2l_dto_map.cpp:284-286` 之后） | `G-A5` + `A2lGolden`/`A2lE2E`/`A2lMultiEndpoint`/`A2lCalibrationAccess` 全绿 |
| P1 | `symbol_policy.hpp`：`DaqEventSource::Consistency`、`DaqEventCandidate.error`、`ResolveDaqEventCandidate`（规则 0 + 规则 1–6）、改造策略函数 | 单测 §5.1 全绿 |
| P2 | `main.cpp` 三处调用点改造（`:3046`、`:3532-3533`、`:2134`/`:2175`） | `XcpMasterSymbolPolicyTest` 与 demo 集成全绿 |
| P3 | 来源标注（若 §7.2(a) 通过）；`main.cpp` watch 成功提示与 `info` 输出 | 人工核对 + 日志行 | 
| P4 | §5.2 端到端复刻用例（含“仅 DEFAULT”“仅 CONSISTENCY”两种） | G-A3 |
| P5 | 全量回归 + 客户 A2L 只读复核 + `code-plan` 实施记录 | G-A1…G-A5 全过 |

### §9.2 回滚

```bash
# 1) 工具侧（P1–P3）
git checkout -- tools/xcp_master/symbol_policy.hpp tools/xcp_master/main.cpp \
                tests/xcp_master_symbol_policy_test.cpp \
                tests/xcp_slave_demo_integration_test.cpp
# 2) SDK 侧（P0）——必须整体回退，避免“ABI 号已升但字段未加”或反之
git checkout -- thirdparty/a2l-sdk/include/liba2l/liba2l_api.hpp \
                thirdparty/a2l-sdk/src/liba2l_export.cpp \
                thirdparty/a2l-sdk/a2lbridge/include/libxcp/a2l/a2l_types.hpp \
                thirdparty/a2l-sdk/a2lbridge/src/a2l_dto_map.cpp
rm -f tests/a2l_daq_event_lists_test.cpp
```

ABI 回滚要点：`kLibA2lAbiVersion` 与结构体字段是同一份契约，**二者必须同进同退**；只回退一侧会让下游按旧 ABI 号读到新布局，属未定义行为。

本方案不新增构建系统改动（除 `tests/CMakeLists.txt` 增加一个测试目标）。

---

## §10 批准状态

已批准（2026-10-26，见 §7.1）：方案 A + `CONSISTENCY` 回退（ABI 11）+ `AVAILABLE` 子集校验 + `prescaler=1` 口径。

开工前仅剩 §7.2 的 (a)(c) 两个小决定；若未回复，默认按“建议做”实施，并在实施记录中标注默认取值的理由。
---

## §13 实施状态（P0–P4 已落地）

| 步 | 状态 | 交付 |
| --- | --- | --- |
| P0 SDK | 完成 | `kLibA2lAbiVersion = 11`；`SymbolDto`/`StructInfoDto` 四表；MEASUREMENT、CHARACTERISTIC、INSTANCE 三处导出；桥接层映射 `daq_consistency_event` |
| P1 策略 | 完成 | `DaqEventSource`、`DaqEventCandidate`、`NormalizeDaqEventNumbers`、`FormatDaqEventNumbers`、`ResolveDaqEventCandidate`（规则 0 + 1–6） |
| P2 调用点 | 完成 | `main.cpp` 四处（事件匹配、事件绑定集合、预览信息、详情信息）+ 观测成功提示显示来源 |
| P3 测试 | 完成 | `tests/a2l_daq_event_lists_test.cpp` 4 项；`XcpMasterSymbolPolicyTest` 13 项新用例；2 项端到端真采样 |
| P4 文档 | 完成 | 本文件 + `code-plan/DAQ观测事件来源降级_实施记录.md` |

### §13.1 实施期发现与偏离（重要）

1. **INSTANCE 必须补全四表，不能只补 CONSISTENCY**：原计划只给 `StructInfoDto` 加 `daq_consistency_event`。实施中发现 demo 形态的 A2L 把 `DAQ_EVENT` 声明在 **INSTANCE** 上，叶子只能继承 INSTANCE 的表 —— 只有 CONSISTENCY 时 `DEFAULT` 一律丢失，故同时补 `daq_event_default` / `daq_event_available`（`thirdparty/a2l-sdk/include/liba2l/liba2l_api.hpp` 的 `StructInfoDto`）。
2. **叶子继承修复（计划未列）**：`thirdparty/a2l-sdk/a2lbridge/src/structure_layout.cpp` 原先只把 `daq_event_fixed` 传给叶子（`MakeScalarLeaf`），DEFAULT/AVAILABLE/CONSISTENCY 全部丢弃 → INSTANCE 形态下方案 A 完全无效。已改为新增 `InstanceEventLists` 结构并把四表一并继承，实测叶子由“四表全空”变为 `default=0`。
3. **CHARACTERISTIC 分支此前四表全不导出**（不是“只导出 fixed”）：`BuildCharacteristicSymbol`（`liba2l_export.cpp:765` 起）原本没有任何 `daq_event_*` 赋值；计划 §4.4 的表述已在实施中按实测修正。
4. **`FIXED_EVENT_LIST` 与 `VARIABLE` 在 A2L 语法层互斥**（`thirdparty/a2llib/src/xcp/xcpdataparser.y:689` `daq_event_union: fixed_event_list | variable;`）→ 不存在“同一符号同时声明 fixed 与 default”的合法 A2L；规则 3（fixed 多值不得退到 default）是防御性分支。测试语料因此拆成 `M_FIXED` 与 `M_VARIABLE` 两个符号，端到端用例也必须整段改写成 `VARIABLE` 形态（只替换关键字会产生非法声明并导致解析整块丢弃）。
5. **`structure_layout.cpp` 的 CHARACTERISTIC 叶子仍不继承事件**：与“CHARACTERISTIC 不参与 DAQ”的工具策略一致（§8 第 1 条），本次不改。
