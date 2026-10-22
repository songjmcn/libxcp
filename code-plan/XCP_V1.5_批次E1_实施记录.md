# XCP V1.5 批次 E1 实施记录（F-08 → F-06 → F-07）

- 日期：2026-10-21
- 依据：`code-plan/XCP_V1.5_规范符合性裁决与修复方案.md` §2B、§5、§3A
- 权威来源：`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.pdf`（原始 PDF，312 页）
- 批次顺序：**F-08 → F-06 → F-07**（不可颠倒）

---

## 1. 规范依据（原始 PDF 逐条确证）

| 依据 | 位置 | 原文要点 |
|---|---|---|
| 识别字段布局 | PDF 第 95-96 页 Figure 51/52/53 | 识别字段恒为 **PID（相对 ODT 号）在前**，DAQ 字段在后；"(BYTE)"、"(\|WORD)"、"(WORD, aligned)" 三种形态分别为 `PID\|DAQ`、`PID\|DAQ(WORD)`、`PID\|FILL\|DAQ(WORD)` |
| FILL 取值 | PDF 第 96 页正文 | "there is the possibility to transfer an **extra FILL byte**"——规范不约束 FILL 取值 |
| CTR 合并 | PDF 第 97-98 页 Figure 56 | "(WORD, aligned)" 下 DTO CTR 通过**取代 FILL 字节**插入，目的是 "keep the alignment without introducing additional fill bytes" |
| CTR 位置（通用） | PDF 第 97 页 Figure 54-55 | "the counter field is located **directly after the identification field**"；"CTR only in first DTO packet of sample" |
| DAQ_KEY_BYTE 位域 | PDF 第 181-183 页 Table 140 | bit 7-6 为 Identification_Field_Type_1/0 |
| 识别字段类型编码 | PDF 第 183 页 Table 143 | `00`=Absolute ODT；`01`=Relative ODT + absolute DAQ (BYTE)；`10`=同上 (WORD)；`11`=同上 (WORD, aligned) |
| DAQ 字段多个 | PDF 第 96 页正文 | DAQ_LIST_NUMBER 参数本身编码为 WORD，"One possibility is to transfer the DAQ list number as BYTE, which reduces the number of theoretically possible packets" |

### 1.1 §2B.2A 更正的实际影响（本批次得以落实）

裁决文档 §2B.2A 已推翻"XCPlite 私有协议"定性：XCPlite 的 `DAQ_HDR_ODT_FIL_DAQW (3 << 6)` 即规范 `11`，实际线格式就是 Figure 53。
`thirdparty/XCPlite/src/xcplite.c:1616-1621` 的编码实现与规范逐字节一致（`d0[0]`=相对 ODT、`d0[1]`=FILL(0xAA)、`d0[2..3]`=DAQ WORD）。

**因此本批次不改线格式**，只让本项目的解码语义与规范对齐——XCPlite 互操作行为不变。

---

## 2. 修改范围

### 2.1 F-08：建立 DAQ_KEY_BYTE → 模式映射（先决条件）

| 文件 | 改动 |
|---|---|
| `include/libxcp/daq/dto_envelope_types.hpp` | 新增 `IdentificationFieldTypeFromKeyByte(std::uint8_t) -> std::optional<IdentificationFieldType>`；新增 `IdentificationFieldMinimumBytes(IdentificationFieldType) -> std::size_t`；新增 `IdentificationFieldMergesCounter(IdentificationFieldType) -> bool` |
| `adapter/a2l/measurement_runtime_profile.hpp` | `XcpliteMeasurementRuntimeProfile::identification_field_type` 默认值 `RelativeByte` → **`RelativeWordAligned`** |
| `adapter/a2l/measurement_runtime_profile.cpp` | 原 `if (key_byte.identification_field_type != 3U)` 位比较 → 改走 `IdentificationFieldTypeFromKeyByte`，并显式判 `*id_field_type != RelativeWordAligned`；非法取值（>3）给出独立错误文案 |

**为什么这是先决条件**：布局分支由 `identification_field_type` 触发。映射不存在时，无论解码器怎么修，XCPlite 的 `0xC0` 都只会被映射成 `RelativeByte` 而走到错误分支。裁决文档 §5.1 的"E1 内部顺序 F-08 → F-06 → F-07 不可颠倒"正是此意。

### 2.2 F-06：识别字段布局与 CTR 语义按规范修正

文件：`src/daq/dto_envelope_decoder.cpp`

| 模式 | 修正前 | 修正后 |
|---|---|---|
| `RelativeByte` | 头长 ≥4，读 `dto[0]`=relODT、`dto[1]` **强制校验 0xAA**、`dto[2..3]`=DAQ16 LE | 头长 ≥2，`dto[0]`=PID(relODT)、`dto[1]`=DAQ(BYTE)；**删除 0xAA 强制校验** |
| `RelativeWord` | 与 Aligned 同分支：`dto[0]`=DAQ、`dto[1]`=relODT（**字段顺序相反**），头长 ≥2 | 头长 ≥3，`dto[0]`=PID、`dto[1..2]`=DAQ(WORD)，按会话字节序 |
| `RelativeWordAligned` | 与 RelativeWord 同分支，FILL 未处理 | 头长 ≥4，`dto[0]`=PID、`dto[1]`=**FILL（不校验取值）**、`dto[2..3]`=DAQ(WORD) |
| `counter_enabled` | 恒在识别字段后独立读 1 字节 | `RelativeWordAligned` 下 **CTR 取 `dto[1]`（取代 FILL），不额外占字节**；其余模式保持"识别字段之后" |

删除的常量：`kXcpliteRelativeMarker = 0xAAU`（其被当作强制标志正是缺陷本体）。

### 2.3 F-07：字节序贯穿

| 项 | 修正前 | 修正后 |
|---|---|---|
| `ReadLe(...)` | 恒小端 | 改为 `ReadOrdered(view, offset, n, order)`：Intel 小端、Motorola 大端 |
| DAQ(WORD) 解码 | 固定小端 | 按 `m_byte_order_` |
| 时间戳解码 | 固定小端 | 按 `m_byte_order_` |
| `m_byte_order_` 成员 | **死字段**（构造保存但从未使用） | 已实际参与解码 |

### 2.4 N-02：识别字段长度下界

| 文件 | 改动 |
|---|---|
| `src/measurement/measurement_session.cpp` | `SetEnvelopeMode` 的 `minimum_bytes` 从手写 switch（`RelativeByte=4`、`RelativeWord/Aligned=2`）改为调用 `IdentificationFieldMinimumBytes`（`RelativeByte=2`、`RelativeWord=3`、`RelativeWordAligned=4`）；保留未知枚举值的显式拒绝 |
| `include/libxcp/measurement/measurement_session.hpp` | 默认 `m_id_field_bytes_` 由 `2` 改为 **`3`**（默认模式 `RelativeWord` 的规范头长）；Doxygen 注释同步 |

### 2.5 N-04：a2lbridge 并行实现（本项目自研，可修改）

文件：`thirdparty/a2l-sdk/a2lbridge/src/daq_layout_impl.cpp`

`EnvelopeExtraBytes` 修正：

| 项 | 修正前 | 修正后 |
|---|---|---|
| counter | 恒 `+1` | 仅非 `RelativeWordAligned` 时 `+1`（该模式下取代 FILL） |
| `RelativeWord` | `+2` | `+2`（不变） |
| `RelativeWordAligned` | `+2`（与 RelativeWord 合并） | **`+3`**（FILL + DAQ WORD） |

**边界遵守**：仅改 `a2lbridge/`（本项目自研桥接层）。同目录 `src/`、`include/liba2l/` 下的 `liba2l_export.cpp` 等**属上游 liba2l 本体，未触碰**。

### 2.6 N-05：示例无法表达 `RelativeWordAligned`

文件：`examples/xcp_integration_demo/xcp_integration_demo.cpp` — `--id-field` 新增 `relative-word-aligned` 取值；用法文本同步。
文件：`examples/measurement_demo/measurement_demo.cpp` — 头注释 `SetEnvelopeMode(RelativeByte,4)` → `(RelativeWordAligned,4)`。

---

## 3. 测试更新（错误行为的回归锁必须更新）

| 文件 | 改动 |
|---|---|
| `tests/dto_envelope_decoder_test.cpp` | 删除 2 个固化错误布局的用例（`RelativeByteXcpliteHeader`、`RelativeByteMissingMarkerThrows`），重写 2 个（`RelativeWordLayoutIsPidThenDaqWord`、`RelativeWordAlignedHasFillBeforeDaqWord`）；**新增 9 个**：`RelativeByteLayoutIsPidThenDaqByte`、`RelativeWordAlignedAcceptsAnyFillValue`、`AlignedCounterReplacesFillByte`、`NonAlignedCounterSitsAfterIdentificationField`、`MotorolaByteOrderDecodesDaqWordBigEndian`、`MotorolaByteOrderDecodesTimestampBigEndian`、`IntelByteOrderDecodesTimestampLittleEndian`、`KeyByteMapsToSpecIdentificationFieldType`、`MinimumBytesMatchSpecFigures`、`OnlyAlignedModeMergesCounterIntoIdentification` |
| `tests/measurement_session_test.cpp` | `MakeFrame` 按 Figure 52 改为 `[relODT][DAQ(WORD LE)]`；`RejectsUnknownEnvelopeModeAndShortHeader` 的脆弱耦合（`RelativeByte + 3U` 抛异常）改为 `RelativeByte + 1U` 抛异常、`RelativeByte + 2U` 通过 |
| `tests/measurement_session_hardening_test.cpp` | `MakeFrame` 同上 |
| `tests/xcplite_daq_test.cpp` | `MakeXcpliteEnvelopeLayout` 模式名 `RelativeByte` → `RelativeWordAligned`（**线格式不变**），注释更正 |
| `tests/xcplite_measurement_test.cpp` | 本地 profile 的 `RelativeByte, 4U` → `RelativeWordAligned, 4U`；注释更正 |
| `tests/measurement_adapter_test.cpp` | 断言 `RelativeByte` → `RelativeWordAligned`；拒绝用例注释更正 |
| `tests/a2l_golden_test.cpp` | `MakeEnvelope` 与 bridge 的 `EnvelopeExtraBytes` 同步（`RelativeWordAligned` 由 `+2` 改 `+3`；counter 按模式分别处理） |

---

## 4. 验证结果

### 4.1 编译

`cmake --build cmake-build-logging-release -j 4` → exit 0，无错误；警告全部来自 `thirdparty/`（glog、XCPlite、a2llib），本批次触及文件零警告。

### 4.2 全量回归

| 套件 | 结果 |
|---|---|
| `libxcp_tests` | **491 passed / 1 skipped / 1 failed** |
| `MeasurementAdapterTest` | 16/16 |
| `A2lGolden` | 40 passed / 2 skipped（既有跳过） |
| `A2lE2E` | 10/10 |
| `XcpMasterSymbolPolicyTest` | 74/74 |
| `XcpliteIntegration`（跨进程真 Slave） | **47/47** |

唯一失败为**既有缺陷** `UdpTransportClose.CloseWhileBlockedReturnsPromptly`（`tests/udp_transport_test.cpp:868-878` 断言 `< 1000ms` 而配置轮询周期恰为 1000ms，属测试自身边界疏漏；`git diff --name-only` 确认该文件与依赖均未被本批次触及）。

### 4.3 改造前后失败分布（证明测试确实覆盖了本次语义变更）

| 阶段 | `libxcp_tests` 失败数 | 说明 |
|---|---|---|
| 仅改核心解码器 | 19（除既有 flake） | 全部为固化旧布局的用例，覆盖 DTO 解码、Session 流水线、Hardening |
| 同步测试后 | 5 | 仅剩 Hardening 的独立 `MakeFrame` 副本 |
| 全部同步后 | 1（既有 flake） | 与本批次无关 |

### 4.4 XCPlite 互操作（关键回归判据）

`XcpliteIntegration` 47/47 全通过，其中含真实 `xcp_slave_demo` 进程的 DAQ 采集用例（`XcpSlaveDemoIntegrationTest`、`XcpliteDaqTest`、`XcpliteMeasurementTest`）。

日志确认 Slave 自报 `Version=1.4`、`DAQ_KEY_BYTE=0xC0`，Master 侧以 `RelativeWordAligned` 解码，**逐字节行为与修正前一致**——符合裁决文档 §5.3 的 E1 回归判据。

---

## 5. 变异测试（证明新断言有效）

| 变异 | 注入内容 | 失败断言（精确位置） |
|---|---|---|
| 1 | `RelativeWordAligned` 的 DAQ WORD 偏移 `2` → `0` | `tests/dto_envelope_decoder_test.cpp:109`（×3 `AcceptsAnyFillValue`）、`:150`（`HasFillBeforeDaqWord`）、`:367`（`AlignedCounterReplacesFillByte`）、`:405`（`MotorolaByteOrderDecodesDaqWordBigEndian`） |
| 2 | 去掉 CTR 合并语义（回到"识别字段后独立读"） | `tests/dto_envelope_decoder_test.cpp:366,368`（`AlignedCounterReplacesFillByte`） |
| 3 | 时间戳固定小端 | `tests/dto_envelope_decoder_test.cpp:423`（`MotorolaByteOrderDecodesTimestampBigEndian`） |
| 4 | adapter profile 模式回退 `RelativeByte`（F-08 原缺陷） | `tests/measurement_adapter_test.cpp:556`（`CrossChecksLiveProfileAndAllBoundEvents`） |

四次变异均**精确失败在预期断言行**，恢复后 `grep "变异测试临时改动"` 无输出、`diff` 与备份一致。

---

## 6. 未覆盖项与待确认

1. **`Absolute` 模式的 DAQ 归属**：`identity.daq_list` 仍恒为 0，由调用方在路由阶段补充（既有设计，未变更）。
2. **`RelativeWordAligned` 的 CTR 与 `timestamp_relative_first_only` 交互**：规范 Figure 55/56 规定 CTR 与时间戳都只随**本事件首 ODT 帧**出现，但解码器目前**不按 ODT 位置门控 CTR**（只门控时间戳）。若 Slave 在非首 ODT 帧也带 CTR，解码器会照读 `dto[1]`。建议后续批次明确该语义。
3. **`OVERLOAD_MSB` ODT 上限（`< 0x7C`）与 `Address_Extension=10` 非法组合**：裁决文档已列为观察项 O-2，本批次未纳入。
4. **N-01 的"CTR 取代 FILL"仅覆盖解码侧**：XCPlite 当前 `counter_enabled=false`（`tests/xcplite_daq_test.cpp` 取证值），故该分支未经真 Slave 验证，仅由 4.4/5 的单元与变异测试覆盖。
5. **`thirdparty/a2l-sdk/` 下 `src/`、`include/liba2l/`（liba2l 本体）未修改**——属上游，按裁决文档 §5.2 第 7 条禁改。
6. **XCPlite Profile 的端到端时间戳单位换算**未在本批次变更，沿用既有取证通路。

---

## 7. 批次状态

| 批次 | 内容 | 状态 |
|---|---|---|
| A | 规范裁决与测试 Oracle | 已完成（裁决文档） |
| B | GET_VERSION + 版本发现 + 降级回退 | 未开始 |
| C | F-01 + F-02 + F-04 | 已完成 |
| D | F-09 + F-10 | 已完成 |
| **E1** | **F-08 + F-06 + F-07** | **本批次完成** |
| E2 | F-05 + F-12 | 未开始 |
| F | F-03 + F-11（F-11 选 B：补 `ERR_CMD_BUSY` 重试） | 未开始 |
| G | 文档收口与三平台验收 | 未开始 |