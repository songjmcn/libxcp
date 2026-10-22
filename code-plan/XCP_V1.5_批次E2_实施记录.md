# XCP V1.5 批次 E2 实施记录（F-05 + F-12 + O-2）

- 关联方案：`code-plan/XCP_V1.5_规范符合性裁决与修复方案.md`（§5「实施批次划分」的 E2 批次）
- 实施日期：2026-10-21
- 前置批次：批次 C（F-01/F-02/F-04）、批次 D（F-09/F-10）、批次 E1（F-08→F-06→F-07）均已完成
- 本批次范围（用户裁定，ask_user_question 两项）：
  - **F-05**：核心支持非零 `MIN_DAQ`，**adapter 侧「从 0 起」的窄化保留**（标注为 profile 限制，非核心限制）
  - **F-12 C1～C5**：ODT Entry 粒度与对齐校验
  - **F-12 时间戳**：`TIMESTAMP_MODE` 码表合法性（Size/Unit）
  - **O-2**：`Address_Extension = 10` 非法组合校验
  - **O-2**：`OVERLOAD_MSB` 启用时 ODT 编号 `< 0x7C` 校验

> **来源限定**：本批次全部规范依据均取自**原始 PDF**
> `docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.pdf`（312 页，pypdf 提取）。
> 依用户裁决（方案文档「来源限定」，最高优先级），**不再参考 Markdown 版规范**。

---

## 1. 规范依据（原始 PDF 逐条）

| 判据 | 原始 PDF 位置 | 规范原文（关键句） |
|---|---|---|
| F-05 `MIN_DAQ` 语义 | 第 180 页 | "**MIN_DAQ is the number of predefined DAQ lists.** For predefined DAQ lists, DAQ_LIST_NUMBER is in the range **[0,1,..MIN_DAQ-1]**. **MAX_DAQ-MIN_DAQ** is the number of configurable DAQ lists. For configurable DAQ lists, DAQ_LIST_NUMBER is in the range **[MIN_DAQ,MIN_DAQ+1,..MAX_DAQ-1]**. If DAQ_CONFIG_TYPE = dynamic, **MAX_DAQ equals MIN_DAQ+DAQ_COUNT**." |
| F-12 C1 粒度取值 | 第 15 页 §4.1.2 | "The possible values for `GRANULARITY_ODT_ENTRY_SIZE_x` are **{1,2,4,8}**. The possible values for ADDRESS_GRANULARITY are **{1,2,4}**." |
| F-12 C2 可表示性 | 第 15 页 §4.1.2 | "`GRANULARITY_ODT_ENTRY_SIZE_x[BYTE] mod (ADDRESS_GRANULARITY[BYTE]) = 0`" |
| F-12 C3 地址对齐 | 第 15 页 §4.1.2 | "`Address[AG] mod (GRANULARITY_ODT_ENTRY_SIZE_x[BYTE] / AG[BYTE]) = 0`" |
| F-12 C4 长度对齐 | 第 15 页 §4.1.2 | "`SizeOf(element described by ODT entry)[AG] mod (GRANULARITY_ODT_ENTRY_SIZE_x[BYTE] / AG[BYTE]) = 0`" |
| F-12 C5 上限 | 第 15 页 §4.1.2 | "The `MAX_ODT_ENTRY_SIZE_x` parameters indicate the upper limits for the size of the element described by an ODT entry **in ADDRESS_GRANULARITY**." … "`SizeOf(element described by ODT entry)[AG] <= MAX_ODT_ENTRY_SIZE_x[AG]`" |
| F-12 时间戳位宽码 | 第 185 页 Table 147 | `000`=No timestamp、`001`=1B、`010`=2B、**`011`=Not allowed**、`100`=4B |
| F-12 时间戳单位码 | 第 186 页 Table 148 | `0000`～`1100` 共 13 个单位；**`1101`～`1111` 未列出（保留值）** |
| F-12 时间戳前提 | 第 185 页 | "If the slave does not support a time-stamped mode (**no `TIMESTAMP_SUPPORTED` in GET_DAQ_PROCESSOR_INFO**), the parameters `TIMESTAMP_MODE` and `TIMESTAMP_TICKS` are **invalid**" |
| O-2 地址扩展非法组合 | 第 182 页 Table 142 | `00`/`01`/`11` 合法；**`10` = Not allowed** |
| O-2 OVERLOAD_MSB ODT 上限 | 第 180 页 | "**`0x00 <= ODT_NUMBER(DAQ with overrun_msb) < 0x7C`**"；触发条件为 `DAQ_PROPERTIES` bit7-6 == `01` |

---

## 2. 修改范围

### 2.1 核心：`src/xcp_master.cpp`（主要改动）

**新增 4 个匿名命名空间内的校验辅助函数**：

| 函数 | 作用 |
|---|---|
| `IsAllowedTimestampSizeCode(uint8_t)` | Table 147 码表判定（0/1/2/4 合法，`011` 及保留值非法） |
| `IsAllowedTimestampUnitCode(uint8_t)` | Table 148 码表判定（`<= 0x0C` 合法） |
| `IsAllowedGranularity(uint8_t)` | C1：粒度 ∈ {1,2,4,8} |
| `ValidateOdtEntryGranularity(spec, gran, max_entry, ag)` | C1～C5 五条判据 |
| `ValidateDaqKeyByte(const DaqKeyByte&)` | O-2：地址扩展位 `10` 拒绝 |
| `ValidateOdtNumberAgainstOverloadMsb(spec, bool)` | O-2：ODT 编号 `< 0x7C` |

**单位纪律（关键实现要点）**：规范中 `Address`、`Size`、`MAX_ODT_ENTRY_SIZE_x` **均以 AG 为单位**，而 `GRANULARITY_ODT_ENTRY_SIZE_x` 是**字节数**。因此实现**必须先验证 `GRANULARITY >= AG && GRANULARITY mod AG == 0`，再计算 `ratio = GRANULARITY / AG`**，最后才做取模——若先做整数除法，`GRANULARITY < AG` 时会出现除零/模零。这与 F-01 属同一族错误。

**F-05 修复（动态通路 `ConfigureDaqListsDynamicImpl`）**：
- 旧代码硬性要求 `specs[i].daq_list == i`（强制从 0 起），并在文案中写道 "MIN_DAQ=0"；
- 新代码改为按规范区间 `[MIN_DAQ, MIN_DAQ + n - 1]` 校验，`MIN_DAQ` 取自 `GET_DAQ_PROCESSOR_INFO` 的取证快照；
- **取证顺序调整**：`GET_DAQ_PROCESSOR_INFO` 的调用移到编号校验**之前**（F-05 需要 `MIN_DAQ` 才能判定合法区间）；
- `MIN_DAQ` 未取证时按 `0` 保守处理——**不猜**（B-3），与旧行为一致。

**取证快照扩展**：
- 新增成员 `m_daq_processor_info_`（`optional<GetDaqProcessorInfoResponse>`）与 `m_daq_resolution_info_`（`optional<GetDaqResolutionInfoResponse>`），保留完整响应供多判据消费；
- 新增私有方法 `bool IsOverloadMsbEnabled() const`（判 `DAQ_PROPERTIES` bit7-6 == `01`）。

**一处实现缺陷的自查修复**：初版把粒度校验挂在 `DaqTimestampBytesCached()` 的返回值上，而该函数原先**只在 `spec.timestamp == true` 时被调用** —— 导致无时间戳配置完全绕过 F-12 校验。已改为**无条件先调用一次 `DaqTimestampBytesCached()`**（返回值仅在 `spec.timestamp` 时计入 header），确保取证与校验不被时间戳开关旁路。**此缺陷由新增用例暴露**（见 §4）。

**另一处自查修复**：`ValidateDaqKeyByte` 初版写在 `if (!m_daq_processor_info_queried_)` 块内，只在**首次查询**时执行。已移到块外，改为「只要有取证快照就校验」。

### 2.2 `include/libxcp/xcp_master.hpp`
- 新增 `m_daq_processor_info_`、`m_daq_resolution_info_` 两个取证快照成员（含中文 Doxygen 注释，说明各自服务的判据）；
- 新增 `IsOverloadMsbEnabled()` 声明。

### 2.3 `MAX_ODT_ENTRY_SIZE` 单位口径全链路修正（既有缺陷，本轮一并处理）

**这是本轮发现的既有缺陷，不是 E2 引入的**：

| 位置 | 旧状态 | 新状态 |
|---|---|---|
| `include/libxcp/measurement/measurement_types.hpp` | `max_entry_size_bytes`（**字节**） | `max_entry_size_elements`（**AG 元素数**） |
| `adapter/a2l/measurement_runtime_profile.hpp` | `max_daq_entry_size_bytes` | `max_daq_entry_size_elements` |
| `src/measurement/measurement_planner.cpp:92-93` | `byte_width > max_entry_size_bytes`（字节比字节） | 先按 AG 换算为元素数，再 `element_width_ag > max_entry_size_elements` |
| `src/measurement/measurement_session.cpp:155` | 非零检查（单位无关，无需改语义，仅更名） | 更名 |

- **为何长期未暴露**：AG=1（BYTE）时字节数与元素数恰好相等，两种口径等价；
- **触发证据**：`tests/measurement_session_test.cpp` 的 mock 声明 `MAX_ODT_ENTRY_SIZE=8`，而用例配置 8 个 4 字节元素 = 32 个 AG 元素 —— 旧代码放行，新 C5 校验正确拒绝；
- 用户裁定「一并修正整个链路」，故本轮完成命名与消费口径的统一；
- `entry_granularity_bytes` **保持字节含义不变**（规范 `GRANULARITY_ODT_ENTRY_SIZE_x` 本身即以 BYTE 表示）。

### 2.4 `tools/xcp_master/main.cpp`
- 仅 1 行：字段更名同步（`max_daq_entry_size_bytes` → `max_daq_entry_size_elements`）。**无逻辑改动**。

### 2.5 测试文件
| 文件 | 改动 |
|---|---|
| `tests/xcp_daq_test.cpp` | `DaqSlave` mock 参数化（`SetMinDaq`/`SetKeyByte`/`SetGranularity`/`SetTimestampMode`/`SetMaxDto`）；`Rig` 构造函数新增 `max_dto` 形参（**必须在 `Connect()` 前设定**）；新增 20 个 E2 用例 |
| `tests/measurement_session_test.cpp` | mock 的 `MAX_ODT_ENTRY_SIZE` 由 8 改为 248，与用例 `SetDaqEntryCapabilities(...{true,1,248})` 的意图一致（原值 8 是旧字节口径下的笔误） |
| `tests/measurement_adapter_test.cpp` | 2 处字段更名断言 |
| `tests/xcp_slave_demo_integration_test.cpp` | 1 处字段更名断言 |

---

## 3. 验证

### 3.1 编译
```
cmake --build cmake-build-logging-release -j 4   → exit 0
```
无错误；警告全部来自 `thirdparty/`（glog、XCPlite、a2llib、a2l-sdk）与既有测试文件，**无一条来自本批次改动的生产文件**。

### 3.2 全量回归

| 套件 | 结果 |
|---|---|
| `libxcp_tests` | **513 tests，511 PASSED / 1 SKIPPED / 1 FAILED** |
| `XcpMasterSymbolPolicyTest` | 74/74 PASSED |
| `MeasurementAdapterTest` | 16/16 PASSED |
| `A2lGolden` | 40 PASSED / 2 SKIPPED（两项既有无害跳过） |
| `A2lSmoke` | 111 checks, 0 failures |
| `A2lE2E` | 10/10 PASSED |
| `XcpliteIntegration` | **47/47 PASSED**（25.8 s） |

唯一失败为**既有 flake** `UdpTransportClose.CloseWhileBlockedReturnsPromptly`：
- 根因在测试自身的边界疏漏（`tests/udp_transport_test.cpp:868-878`）：配置 `receive_poll_interval_ms = 1000` 而断言 `EXPECT_LT(elapsed, 1000)` 取严格小于，实际恰好 1000ms；
- `git diff --name-only` 确认该文件**未被本批次触及**，与本批次无关。

### 3.3 新增用例清单（`tests/xcp_daq_test.cpp`，20 个）

| 套件 | 用例 | 验证目标 |
|---|---|---|
| `XcpDaqMinDaq` | `ZeroMinDaqKeepsZeroBasedNumbering` | MIN_DAQ=0 时仍从 0 起（回归保护） |
| | `NonZeroMinDaqShiftsNumberingBase` | **MIN_DAQ=2 时编号从 2 起必须放行**（修复前必被拒） |
| | `ZeroBasedNumberingRejectedWhenMinDaqNonZero` | MIN_DAQ=2 时按 0 起必须被拒 |
| | `SkippedNumberingRejected` | 跳号（2,5）必须被拒 |
| `XcpDaqGranularity` | `EntryExceedingMaxOdtEntrySizeRejected` | C5 上限被拒（并断言 0 条 WRITE_DAQ 发出） |
| | `EntryExactlyAtMaxOdtEntrySizeAccepted` | C5 边界值放行（防差一） |
| | `IllegalGranularityValueRejected` | C1：粒度 3 被拒 |
| | `GranularitySmallerThanAgRejected` | C3 地址对齐独立触发 |
| | `MisalignedEntryAddressRejected` | C3：地址非 4 倍数被拒 |
| | `AlignedEntryAddressAccepted` | C3 边界放行 |
| | `MisalignedEntrySizeRejected` | C4：长度 6（非 4 倍数）被拒 |
| | `AbsentResolutionInfoSkipsGranularityChecks` | Optional 缺失时不做粒度校验（不猜） |
| `XcpDaqTimestampMode` | `IllegalSizeCode011Rejected` | Table 147：`011` 非法 |
| | `ReservedUnitCodeRejected` | Table 148：`1101` 保留值非法 |
| | `XcpliteMode0x0cAccepted` | XCPlite 实际值 `0x0C` 不误伤 |
| `XcpDaqKeyByte` | `IllegalAddressExtensionModeRejected` | Table 142：`10` 被拒 |
| | `AllowedAddressExtensionModesAccepted` | `00`/`01`/`11` 三个合法值放行 |
| `XcpDaqOverloadMsb` | `TooManyOdtsRejectedWhenOverloadMsbEnabled` | 125 个 ODT ≥ 0x7C 被拒 |
| | `OdtsBelowLimitAccepted` | 124 个 ODT 恰好未超 |
| | `LimitNotAppliedWhenOverloadMsbDisabled` | 未启用时不受限（不误伤） |

### 3.4 变异测试（证明断言有效）

向 `src/xcp_master.cpp` 注入 3 处变异（均加 `// 变异测试临时改动` 标记），重编译后运行：

| 变异 | 目标断言 | 结果 |
|---|---|---|
| M1：`if (false && specs[i].daq_list != expected)` | F-05 编号区间 | **精确失败**：`ZeroBasedNumberingRejectedWhenMinDaqNonZero`、`SkippedNumberingRejected` |
| M2：`if (false && e.size > max_odt_entry_size)` | F-12 C5 | **精确失败**：`EntryExceedingMaxOdtEntrySizeRejected` |
| M3：`if (false && key_byte.address_extension_mode == 2U)` | O-2 地址扩展 | **精确失败**：`IllegalAddressExtensionModeRejected` |

**M2 的一次有效修正**：初版 C5 用例用 `MakeSpec()`（CTO=16 → MAX_DTO=8）配置 8 元素 Entry，实际是 **MAX_DTO 预检先拒绝**，变异后用例依旧通过——即该用例当时**并未证明 C5**。已改为 `Rig(16U, 64U)` 放大 MAX_DTO 并精简为单 Entry，使 C5 成为唯一可能的拒绝来源，重跑变异后精确失败。**这是变异测试直接促成的用例质量改进。**

恢复：从 `/tmp/e2_backup.cpp` 拷回，`grep -c "变异测试临时改动"` 返回 **0**，全量回归重新回到 511 PASSED。

---

## 4. 实施过程中修正的自身错误（如实记录）

| # | 错误 | 发现方式 | 修正 |
|---|---|---|---|
| 1 | mock 的 `GET_DAQ_PROCESSOR_INFO` 响应写成 6 字节（漏了 `MAX_EVENT_CHANNEL` 高字节） | 新用例报 `响应长度不足（6 字节，期望 >= 7）` | 按 Table 136 补齐为 7 字节 |
| 2 | 粒度校验挂在 `DaqTimestampBytesCached()` 返回值上，无时间戳时被旁路 | 新用例 `IllegalGranularityValueRejected` 未抛异常 | 改为无条件取证 |
| 3 | `ValidateDaqKeyByte` 只在首次查询块内执行 | 新用例 `IllegalAddressExtensionModeRejected` 未抛异常 | 移出缓存块，有快照即校验 |
| 4 | `spec.odts.assign(0x7D, spec.odts[0])` 自引用导致 `std::bad_alloc` | 用例抛 `bad_alloc` | 先取模板副本再 `assign` |
| 5 | 把 root 的 MAX_DTO 改成跟随 CTO，打翻 2 个既有 MAX_DTO 预检用例 | 全量回归新增 2 个失败 | 回退为默认 8，改由 `Rig` 构造函数形参按需放大 |
| 6 | `SetMaxDto` 在 `Connect()` 之后调用，对已建会话无效 | 两用例仍报 MAX_DTO 超限 | 移入 `Rig` 构造函数，`Connect()` 之前设定 |
| 7 | C5 用例实际由 MAX_DTO 预检拒绝，未证明 C5 | **变异测试** | 放大 MAX_DTO 并精简单 Entry |

---

## 5. 未覆盖项与后续

1. **adapter 侧 `MIN_DAQ != 0` 仍被拒绝**：`adapter/a2l/measurement_runtime_profile.cpp:103` 与 `:199` 保留「从 0 起」的窄化（**用户裁定**：核心支持任意起始编号，adapter 保留 profile 限制）。当前无 `MIN_DAQ != 0` 的真实对端可供端到端验证，故未放开。
2. **`TIMESTAMP_SUPPORTED` 前置校验未实现**：规范要求 Slave 未声明 `TIMESTAMP_SUPPORTED` 时 `TIMESTAMP_MODE`/`TIMESTAMP_TICKS` 无效。本轮实现了码表合法性（Size/Unit），但「是否声明支持」这一前置判据未加——需确认 `GET_DAQ_PROCESSOR_INFO` 的 `DAQ_PROPERTIES` 中该位的确切位置后另行处理。
3. **C2（`GRANULARITY mod AG == 0`）未构造独立用例**：现有夹具 AG 固定为 BYTE(1)，任何 ∈{1,2,4,8} 的粒度都自动满足 `mod 1 == 0`，无法触发。C2 代码路径已实现且有防御（避免除零），但**缺少可执行证明**；需具备 AG>1 的夹具后补充。
4. **`ALTERNATING` + `TIMESTAMP` 组合约束未实现**（方案 §2D 列为观察项）。
5. **时间戳截断/环绕的物理换算未实现**（方案 §2D：单 DTO 只能得相对时间，环绕须按无符号模运算）。
6. **后续批次未实施**：F（F-03 + F-11，用户选 B 即补 `ERR_CMD_BUSY` 重试）、B（GET_VERSION + 版本发现 + V1.3/V1.4 降级回退）、G（文档收口与三平台验收）。
7. **既有 flake 未修**：`UdpTransportClose.CloseWhileBlockedReturnsPromptly`（测试自身边界疏漏，与本批次无关，按 AGENTS.md「不重构已完成代码」保留）。

---

## 6. 本轮改动文件清单

生产代码（7 个）：
```
src/xcp_master.cpp
include/libxcp/xcp_master.hpp
include/libxcp/measurement/measurement_types.hpp
src/measurement/measurement_planner.cpp
src/measurement/measurement_session.cpp
adapter/a2l/measurement_runtime_profile.hpp
adapter/a2l/measurement_runtime_profile.cpp
```
调用点同步（1 个）：
```
tools/xcp_master/main.cpp
```
测试（5 个）：
```
tests/xcp_daq_test.cpp
tests/measurement_session_test.cpp
tests/measurement_adapter_test.cpp
tests/xcp_slave_demo_integration_test.cpp
```