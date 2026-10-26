# DAQ 观测事件来源降级（方案 A）实施记录

> 关联计划：`code-plan/DAQ观测事件来源降级_实施计划.md`（含 §13 实施状态）
> 报障来源：加载客户 `apa_complex.a2l`（87 MB / 198,556 符号）后，DAO 模式下大量变量报「观测拒绝：需要唯一 FIXED_EVENT_LIST」。

## §0 结论速览

| 项 | 结果 |
| --- | --- |
| 客户阻断是否解除 | **是**：195,990 个带 `DAQ_EVENT` 的 MEASUREMENT **全部**可用（此前 0 个可用） |
| 来源判定 | 全部为 `default`（客户 A2L 没有 `FIXED_EVENT_LIST`，每块恰含 1 个 `DEFAULT`） |
| ABI | `thirdparty/a2l-sdk` 10 → **11**（破坏性：`StructInfoDto` 新增 2 字段） |
| 端到端真采样 | 仅 `DEFAULT`、仅 `CONSISTENCY` 两种 A2L 形态均能真正采到数据 |
| 全量回归 | **1046 / 1046**（`-E "^Example"`）+ **14 / 14** Examples 串行，0 失败 |

## §1 改动清单

### §1.1 SDK（`thirdparty/a2l-sdk`）

| 文件 | 改动 |
| --- | --- |
| `include/liba2l/liba2l_api.hpp` | `kLibA2lAbiVersion` 10 → 11；`SymbolDto` 增 `daq_consistency_event`；`StructInfoDto` 增 `daq_event_default`、`daq_event_available`、`daq_consistency_event`；v11 变更说明块 |
| `src/liba2l_export.cpp` | MEASUREMENT（`BuildMeasurementSymbol`）：补 `daq_consistency_event`；CHARACTERISTIC（`BuildCharacteristicSymbol`）：**新增整段四表导出**（原为完全没有）；INSTANCE（`BuildInstanceInfo`）：四表导出 |
| `a2lbridge/include/libxcp/a2l/a2l_types.hpp` | `SymbolInfo` 增 `daq_consistency_event` |
| `a2lbridge/src/a2l_dto_map.cpp` | DTO → 领域符号映射补 `daq_consistency_event` |
| `a2lbridge/src/structure_layout.cpp` | 新增 `InstanceEventLists`；`MakeScalarLeaf` / `ExpandStruct` 改为继承 INSTANCE 的**四张表**（原先只继承 `daq_event_fixed`） |

### §1.2 主站策略（`tools/xcp_master`）

| 文件 | 改动 |
| --- | --- |
| `symbol_policy.hpp` | `DaqEventSource`（None/Fixed/Default/Consistency）、`DaqEventCandidate`（source/channels/error）、`NormalizeDaqEventNumbers`、`FormatDaqEventNumbers`、`ResolveDaqEventCandidate`（规则 0 + 1–6）、`SymbolDaqObservationUnsupportedReason` 改为委托它 |
| `main.cpp` | DAQ 事件匹配改用候选通道；事件绑定集合按来源收集；预览与详情信息显示 `event-source=`；观测成功提示按来源给出不同措辞 |

### §1.3 测试

| 文件 | 改动 |
| --- | --- |
| `tests/a2l_daq_event_lists_test.cpp`（新，4 项） | `FIXED_EVENT_LIST` 形态、`VARIABLE` 四表形态、仅 CONSISTENCY、CHARACTERISTIC 四表 |
| `tests/xcp_master_symbol_policy_test.cpp` | 原 `DaqRequiresUniqueKnownFixedEvent` 替换为 13 项规则用例（含规则 0 自洽性、来源优先级、来源名文案） |
| `tests/xcp_slave_demo_integration_test.cpp` | 契约测试改名 `GeneratedA2lSeparatesInstanceEventLists` 并改为“仅 DEFAULT 合格”；新增 `DefaultEventListDrivesLiveDaq`、`ConsistencyEventListDrivesLiveDaq` 两个真实采样用例 |
| `tests/CMakeLists.txt` | 新增 `A2lDaqEventListsTest` 目标 |

## §2 规则实现（`ResolveDaqEventCandidate`）

| 序 | 条件 | 结果 |
| --- | --- | --- |
| 0 | `DEFAULT`/`CONSISTENCY` 去重后不在 `AVAILABLE` 内 | 拒绝「A2L 事件声明自相矛盾…」 |
| 1 | `fixed` 去重后恰 1 | 采用，`source=Fixed` |
| 2 | `fixed` 为空且 `default` 恰 1 | 采用，`source=Default` |
| 3 | `fixed` 去重后 >1 | 拒绝「声明了 N 个不同事件…」 |
| 4 | `fixed` 空、`default` 去重后 >1 | 拒绝 |
| 5c | 二者皆空且 `consistency` 恰 1 | 采用，`source=Consistency` |
| 5b | 二者皆空、`consistency` 去重后 >1 | 拒绝 |
| 6 | 事件号不在 A2L 声明的事件通道表内 | 拒绝 |

## §3 验证

### §3.1 客户 A2L 全量判定（只读探针）

```
符号总数=198556
MEASUREMENT=195998
带 DAQ_EVENT=195990
可用=195990
来源分布: default=195990
```

0 条拒绝；样例 `apa_complex::algo_status_info.apa_control_status.actual_period_ms -> source=default channel=0`。

### §3.2 测试计数

| 目标 | 结果 |
| --- | --- |
| `A2lDaqEventListsTest` | 4 / 4 |
| `XcpMasterSymbolPolicyTest` | 全绿（含 13 项新规则用例） |
| `XcpliteIntegration` 新增 2 项端到端 | 2 / 2（各约 450 ms，真实事件采样） |
| `ctest -E "^Example" -j 4` | **1046 / 1046**（2 项既有 Skip） |
| `ctest -R "^Example" -j 1` | **14 / 14**（必须串行，并行会抢默认端口） |

## §4 实施期发现（与原计划的偏离）

1. **INSTANCE 不能只补 CONSISTENCY**：demo 形态把 `DAQ_EVENT` 声明在 INSTANCE 上，叶子只继承 INSTANCE 的表；只补 CONSISTENCY 会让 `DEFAULT` 全丢。故同时补 `daq_event_default` / `daq_event_available`。
2. **叶子继承是必修项**：`structure_layout.cpp` 原先只继承 `daq_event_fixed`，四表里的另三张直接丢弃 → INSTANCE 形态下方案 A 完全无效。修复后实测叶子 `default=0`（修复前四表全空）。
3. **CHARACTERISTIC 此前四表全不导出**：`BuildCharacteristicSymbol` 原本没有任何事件赋值，本次补齐四表。
4. **A2L 语法互斥**：`FIXED_EVENT_LIST` 与 `VARIABLE` 互斥（`thirdparty/a2llib/src/xcp/xcpdataparser.y:689`），因此测试语料拆成 `M_FIXED` / `M_VARIABLE`，端到端用例必须整段改写成 `VARIABLE` 形态；只替换关键字会得到非法声明，整块被解析器丢弃。
5. **踩过的验证坑**：第一次跑契约测试时 `--gtest_filter` 写成了另一个用例名（`GeneratedA2lPropagatesFixedEventsToMeasurementLeaves`），旧断言恰好通过，掩盖了“叶子不继承 DEFAULT”的真问题；改为先写只读探针直查叶子四表才发现。

## §5 未做 / 已知限制

1. CHARACTERISTIC 仍不参与 DAQ（工具策略，非规范限制）——见计划 §8 第 1 条。
2. 一个变量挂多个事件、无任何事件声明的回退、跨事件一致性采样：均未做（方案 B 范围）。
3. `structure_layout.cpp` 的 CHARACTERISTIC 叶子仍不继承事件——与上条策略一致，未改。
4. UI 侧只用 `event-source=` 文本标注来源，未做专门的来源图标/列。

## §6 回滚

```bash
git checkout -- thirdparty/a2l-sdk/include/liba2l/liba2l_api.hpp \
                thirdparty/a2l-sdk/src/liba2l_export.cpp \
                thirdparty/a2l-sdk/a2lbridge/include/libxcp/a2l/a2l_types.hpp \
                thirdparty/a2l-sdk/a2lbridge/src/a2l_dto_map.cpp \
                thirdparty/a2l-sdk/a2lbridge/src/structure_layout.cpp \
                tools/xcp_master/symbol_policy.hpp tools/xcp_master/main.cpp \
                tests/xcp_master_symbol_policy_test.cpp \
                tests/xcp_slave_demo_integration_test.cpp tests/CMakeLists.txt
rm -f tests/a2l_daq_event_lists_test.cpp
```

> ABI 与 SDK 必须整体回滚：`liba2l_api.hpp` 的 `StructInfoDto` 增字段会让旧桥接层读到错位内存。