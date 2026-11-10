# PR#22 Review 问题修复实施记录

## 0. 状态与依据

- 状态：已实施并验证。Release 全目标构建成功，CTest **1324/1324 全部通过**（0 失败）。
- 依据：GitHub PR `songjmcn/libxcp#22`（`feat: XCP V1.5 测量标定收口 B0-B4`）的 6 条 Copilot 审查意见。用户在分析后指示“同意方案，继续修改”。
- 基线：分支 `feate_daq_all`，HEAD=`b3de15a8051682ca0d58fb73c5ba27cc04d526f1`（即 PR 头）。
- 本文只记录**本次审查问题修复**；PR 原有的 B0–B4 功能内容不在范围内。

## 1. 问题逐条分析与结论

6 条审查意见**全部成立**，其中 #2 为静默数据错误（最严重）。

| # | 位置 | 问题 | 后果 |
|---|---|---|---|
| 1 | `include/libxcp/measurement/measurement_sample.hpp:74,76` | `MeasurementFrame::sample_index/sample_count` 为 `uint8_t` | 规范 `DPM_SAMPLE_COUNT` 合法范围是 `[2, 0xFFFF]`，S>255 时帧内采样序号与总数无法表达 |
| 2 | `src/measurement/measurement_planner.cpp:269,323` | element-grouped 下 `packed_stride = route.size`，但 `payload_offset` 仍是**单采样偏移** | 第 2 个及以后的 Entry 基址漏乘 S，从**前一个 Entry 的后续采样**取数，静默错位且不报错 |
| 3 | `src/measurement/measurement_session.cpp:333` | 只校验事件周期，未校验 `DaqEventPropertyBit::kPacked`，未复用已实现的 `CanUseDaqPackedMode` | “列表支持、事件不支持”的组合被放行，按 packed 拆帧产生错位 |
| 4 | `src/measurement/measurement_session.cpp:345-346` | `static_cast<uint8_t>(std::min<uint16_t>(S, 0xFF))` | S=256 截断为 0，worker 第 633-635 行再兜底成 1，**只解出 1 个采样**，其余 255 个静默丢弃 |
| 5 | `src/measurement/measurement_session.cpp:349` | 用本地 `spec.prescaler`（planner:292 硬编码为 1），未回读 `GET_DAQ_LIST_MODE` | Slave 实际降频时，非锚点采样时间戳按错误间隔重建，且错误看起来完全“有效” |
| 6 | `src/memory_access.cpp:328` | 用 `connect.slave_block_mode_supported`（COMM_MODE_BASIC bit6）判定下载块 | bit6 是 Slave→Master **上传**块；应为 `COMM_MODE_OPTIONAL` bit0 MASTER_BLOCK_MODE。两个方向都判错：支持下载块的被降级，仅支持上传的却被下发续帧 |

**补充发现（审查意见未覆盖）**：`src/measurement/measurement_session.cpp:307-311` 存在一条“DAQ Packed Mode 的采样序号按 8 位表达，DAQ List 号必须 < 256”的本地限制，它正是 #1 的直接产物，一旦位宽修正即成为错误的假限制。

## 2. 修复内容

### 2.1 #6 下载块能力判定改为 MASTER_BLOCK_MODE

- `include/libxcp/protocol_types.hpp`：新增 `constexpr std::uint8_t kCommModeOptionalMasterBlockMode = 0x01U;`，注释明确其与 bit6 的方向差异。
- `src/memory_access.cpp`（`CanUseBlockMode`）：判据改为
  `comm_mode_info.has_value() && (comm_mode_optional & kCommModeOptionalMasterBlockMode) && max_bs >= 1`，
  不再引用 `slave_block_mode_supported`。
- `include/libxcp/memory_access.hpp`：`@details` 同步改写，说明为何不得回落到 bit6。

### 2.2 #2 element-grouped 基址乘 S

`src/measurement/measurement_planner.cpp`（`close_odt`）：

```cpp
} else if (m_packed_mode_ == DaqPackedMode::ElementGrouped) {
    route.packed_stride = route.size;
    route.payload_offset *= sample_multiplier;   // 新增
} else {
    route.packed_stride = active.odt_single_used;
}
```

关键区别：element-grouped 的物理布局是“Entry e 的全部 S 个采样连续，再排 Entry e+1”，
故 Entry e 的基址为 `Σ_{i<e} w_i × S`；event-grouped 是“采样 k 的全部 Entry 连续”，
单采样偏移已是正确基址，**不得**再乘 S。

### 2.3 #3 + #5 能力取证与 prescaler 回读

- `include/libxcp/xcp_master.hpp`：
  - 新增嵌套结构 `DaqPackedRuntimeInfo { bool prescaler_verified; std::uint8_t prescaler; }`。
  - `SetDaqPackedMode` 签名增加 `event_channel` 参数：
    `(daq_list, mode, event_channel, timestamp_mode, sample_count)`。
  - 新增 `[[nodiscard]] DaqPackedRuntimeInfo ReadDaqPackedRuntimeInfo(std::uint16_t daq_list);`
- `src/xcp_master.cpp`：
  - `SetDaqPackedMode` 在发帧前依次取证：列表 `kPacked` → 事件 `kPacked` → `time_cycle != 0`；
    任一不满足即 `active=false` 并返回中文 `reason`，**不抛异常**（“退回非 packed”是正常业务分支）。
  - 新增 `ReadDaqPackedRuntimeInfo`：经 `QueryDaqListMode` 取实际 prescaler；
    命令不支持或 `prescaler == 0` 时 `prescaler_verified = false`。
- `src/measurement/measurement_session.cpp`：
  - 调用传入 `spec.event_channel`；
  - 协商 `active` 后立即回读，未验证则抛 `UnsupportedFeature`（不允许用请求值冒充确认值）；
  - `sample_interval_ns = time_cycle × unit_ns × runtime.prescaler`。

### 2.4 #1 + #4 采样数全链路 16 位

- `include/libxcp/measurement/measurement_sample.hpp`：`sample_index`/`sample_count` → `std::uint16_t`。
- `include/libxcp/measurement/measurement_session.hpp`：`PackedListInfo::sample_count` → `std::uint16_t`。
- `src/measurement/measurement_session.cpp`：
  - `info.sample_count = m_packed_sample_count_`（不再 `min(...,0xFF)`）；
  - worker 的 `sample_total` 与循环变量改为 `std::uint16_t`；
  - 删除 2.5 所述的 `< 256` 假限制。

### 2.5 删除错误的 List 号限制

原 `measurement_session.cpp:307-311` 以“采样序号按 8 位表达”为由拒绝 DAQ List 号 ≥ 256。
DAQ List 号在协议上是 EPK（16 位），全链路 `command_codec.cpp` 各 `Encode*` 亦按 `WriteU16` 写入，
该限制纯属位宽缺陷的副作用，已删除。

## 3. 测试

修改/新增于 `tests/calibration_block_test.cpp` 与 `tests/daq_packed_mode_test.cpp`。

测试装置同步修正：

- `CommModeInfoResponse(max_bs, master_block_mode)`：`optional` 按位**重建**（`0x0F`/`0x0E`），
  而非在 `0x0E` 上叠加，避免位含义混淆。
- `BlockSlave`/`Harness` 增加 `master_block_mode` 参数；`BlockSlave::m_block_mode_` 的含义收窄为
  CONNECT bit6，下载块判定改由 `m_master_block_mode_` 驱动。
- `PackedSessionSlave` 增加可配置 `max_dto`（默认 `0x0100`）。

新增用例（9 条）：

| 用例 | 覆盖 |
|---|---|
| `DaqPackedModePlanner.ElementGroupedBaseAddressIsScaledBySampleCount` | #2：Entry b 基址 2 → 8（S=4） |
| `DaqPackedModePlanner.EventGroupedBaseAddressIsNotScaled` | #2 对照：event-grouped 不得放大 |
| `DaqPackedModePlanner.NonPackedBaseAddressIsNotScaled` | #2 对照：非 packed 倍率恒为 1 |
| `DaqPackedModePlanner.LargeDaqListNumberIsNotRejectedInPackedMode` | 2.5：产出的 List 号 ≥ 256 |
| `DaqPackedModeSession.SampleCountAbove255IsNotTruncated` | #1/#4：S=256 产出 256 帧，末帧 `sample_index=255`、`sample_count=256` |
| `DaqPackedModeSession.UsesReadBackPrescalerForSampleInterval` | #5：回读 prescaler=4 → 时间戳 1000/5000/9000 |
| `DaqPackedModeSession.RejectsWhenPrescalerCannotBeReadBack` | #5：回读不可用 → 拒绝启用 |
| `CalibrationBlockWrite.FallsBackToChunkedWhenMasterBlockModeMissing` | #6：bit6=1 但 bit0=0 → 回退逐块 |
| `CalibrationBlockWrite.BlocksWhenOnlyMasterBlockModeIsDeclared` | #6 反向：bit6=0 但 bit0=1 → 应走块序列 |

**实现注意**：

- `GET_DAQ_LIST_MODE` 解析器拿到的 `res_data` 已**去掉 RES 标识**，故 prescaler 位于载荷下标 5
  （对应含 RES 报文的第 6 字节）。构造 Mock 响应时容易错位以为在 `body[5]`。
- `MAX_DTO` 在 CONNECT 时固化为会话参数；测试中调大 `PackedSessionSlave::max_dto` 后必须
  `Disconnect()` + `Connect()` 才会生效。
- S 倍率会乘进净荷容量：S=256 的 1 字节符号需要 `256 + 4B 时间戳 + 3B 识别字段 = 263B`，
  故该用例把 `max_dto` 调到 `0x0200`。
- 用 Python 改写这两个测试文件时必须保持 **CRLF** 行尾，否则整个文件都会被记为改动
  （曾出现 1627 行假 diff）。

## 4. 验证

```
cmake -B cmake-build-release -S . -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release -j 4        # exit=0
cd cmake-build-release && ctest -j 4          # 1324/1324 passed, 0 failed
```

未运行项：`TcpStreamDecoderFinish.HeaderOnlyIsTruncated`（Disabled）与
`AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/2`（用例内显式 Skip），均与本次改动无关。

## 5. 未覆盖与残余风险

- 验证全部基于 **Mock Slave**。真实 Slave 的 prescaler 改写、S>255 的 DTO 尺寸、
  以及仅申报 MASTER_BLOCK_MODE 的对端行为，尚无实机验证。
- `packed_sample_count`（S）本身未与 Slave 的 `MAX_DTO` 做**预检查**：容量不足时由规划器以
  “超过单 ODT 可用容量”拒绝，错误信息可读，但属于间接发现。
- 本次未改动 `DaqPackedModeNegotiation::reason` 的文案约定，调用方若按字符串匹配需自行确认。