# libxcp 测量子系统 v0.6 记录：时间戳换算 / 统计 / 错误语义硬化

> 对应计划：`code-plan/libxcp_测量子系统代码增长计划.md` §5.4 与 v0.6 步骤（:689-743）。
> 完成日期：2026-09-29。基线：v0.5（`code-plan/libxcp_测量子系统_v0.5_记录.md`）。

## 1. 交付物清单

| 文件 | 类型 | 说明 |
| --- | --- | --- |
| `include/libxcp/daq/daq_timestamp.hpp` | 新增（74 行） | `DaqTimestamp` + `DaqTimestampConverter` 声明 |
| `src/daq/daq_timestamp.cpp` | 新增（89 行） | 回卷延展 / 饱和乘法实现 |
| `src/measurement/measurement_session.cpp` | 修改 | 每流转换器接入、错误类别映射、`SetTimestampUnit` |
| `include/libxcp/measurement/measurement_session.hpp` | 修改 | 新公开 API `SetTimestampUnit(uint64_t)`（:139）+ 成员 `m_timestamp_unit_ns_`（:170） |
| `include/libxcp/measurement/measurement_sample.hpp` | 修改 | `MeasurementFrame` 新增 `timestamp_raw` / `timestamp_valid` |
| `CMakeLists.txt` :93 | 修改 | 注册 `src/daq/daq_timestamp.cpp` |
| `tests/daq_timestamp_test.cpp` | 新增（153 行，12 用例） | L1 单元测试 |
| `tests/measurement_session_hardening_test.cpp` | 新增（432 行，6 用例） | L2 集成硬化测试 |
| `tests/CMakeLists.txt` :56-57 | 修改 | 注册两个测试文件 |

## 2. 设计决策与偏差

### 2.1 `Convert(raw)` 内部基线（偏离计划 §5.4 的 `Convert(raw, prev_raw)`）
计划字面签名要求调用方显式传 `prev_raw`。实现改为转换器**自持**基线
（`m_prev_raw_` / `m_has_prev_`），理由：
- 会话侧每 DAQ List 一条流（架构文档 §12），基线本来就是流的私有状态，
  外置只会把"必须按序调用、必须记住上一帧"的负担推给每个调用方；
- 基线与周期计数（`m_cycles_`）必须同生命周期，拆开存放易失同步。
语义不变：`raw < prev` 判回卷一次，单帧至多 `+1` 周期（不猜丢失帧数）。

### 2.2 "不猜"三原则在本步的落点（B-3 / R13）
- 位宽非 8/16/32 → 模未知 → `valid=false`，**保留 raw**（`ModuloOf` :23-34）；
- `unit_ns==0`（单位码表无权威来源，R13）→ `valid=false`，仍推进回卷基线与
  计数（raw 流语义与单位无关，:69-72）；
- 时间单位来源：`MeasurementSession::SetTimestampUnit(unit_ns)` 由上层显式
  注入；运行中调用抛 `XcpException(InvalidState)`。

### 2.3 回卷计数跨 Stop→Start 连续
`Reset()` 清基线与周期、**不清** `m_total_wraps_`（历史累计）；而
Stop→Start 重建 worker 线程时 `converters` map 整体重建，基线自然复位。
统计语义：`stats.timestamp_wraps` 单调累计；帧时间戳不跨流续接。
（`DaqTimestampConverter::Reset` 目前无生产调用方——流生命周期由
worker 线程局部 map 管理；保留 API 供单流复用场景与 L1 测试。）

### 2.4 错误类别映射 `CategoryOf`（v0.5 遗留"有损映射"闭环）
`Prepare` 规划失败不再统一抛 `InvalidArgument`：
`src/measurement/measurement_session.cpp` :166-170 按 `MeasurementErrorCode`
映射（表 :49-70）：
- `UnsupportedDataType` / `UnsupportedConversion` → `UnsupportedFeature`
- `Busy` / `NotRunning` → `InvalidState`
- `Fatal` / `DtoDecodeError` / `DaqConfigurationError` → `ProtocolError`
- `NotFound` / `AmbiguousName` / `RawSizeMismatch` / `InvalidLayout` / 其余 → `InvalidArgument`

### 2.5 帧字段扩展
`MeasurementFrame.timestamp_raw`（uint64，取自信封原始计数）与
`timestamp_valid`（bool）。`timestamp`（chrono::nanoseconds）仅在
`valid=true` 时被填充（含回卷延展）；无效时保持默认 0 且 raw 可查。

## 3. 每流转换器接线（WorkerLoop）

`src/measurement/measurement_session.cpp` :298-306：
`std::map<uint16_t, DaqTimestampConverter> converters`（worker 线程局部，
key=daq_list），位宽 `ts_bits = m_timestamp_bytes_ * 8`，单位取
`m_timestamp_unit_ns_`。:366-392 对每个带 ts 的帧：find_or_create →
`WrapCount()` 前后差值 `fetch_add` 进 `m_timestamp_wraps_` →
`Convert(*raw)` → valid 才写 `frame_out.timestamp`。
不同事件通道共用一个转换器会把两路 raw 的差值误判为回卷——
`PerListStreamsWrapIndependently` 用例对此固化。

## 4. v0.5 遗留项处置

| 遗留项（v0.5 记录 §6） | 处置 |
| --- | --- |
| timestamp_wraps 恒 0 | 已闭环：每流转换器 + 会话统计（:379-387） |
| 错误类别有损映射 | 已闭环：`CategoryOf`（§2.4） |
| ts 宽度时序：`Prepare` 在 `ConfigureDaqListsDynamic` **之前**读 `DaqTimestampBytesCached()`（:153），而惰性 GET_DAQ_RESOLUTION_INFO 要到动态门（xcp_master.cpp:481）才触发 → 首次 Prepare 可能拿到 0 | **维持原状，显式记录为已知限制**（见 §5）。不做"二次读取"补丁的理由：改序会让规划器与预检的表预算口径不一致（规划按 0 字节 ts 排表、预检按 4 字节截断报错），语义上需要先回答"截断帧是否可接受"——超出 v0.6 硬化范围，留待 v0.9 端到端暴露真实 Slave 行为后再定。 |

## 5. 已知限制（v0.9 前提请注意）

- 首次 `Prepare()`（连接后从未发过动态/静态 DAQ 配置）时
  `m_timestamp_bytes_` 取到 0 → 信封按无时间戳解析，帧头 4 字节 ts 会被
  当作净荷前缀。当前测试 Slave（GET_DAQ_RESOLUTION_INFO 应答 0x0C）在
  `ConfigureDaqListsDynamic` 预检内已触发惰性取证，且会话的 routes 只按
  (daq,odt) 匹配不受净荷长影响，故 L2 用例语义正确；对真实 XCPlite
  Slave 的先后次序验证放在 v0.9 端到端。
- `MeasurementValue` 为 variant 五型；A2L 定标数组（ARRAY 逐元素换算）
  未在本步扩展。

## 6. 验证结果

- 构建：`cmake --build build-v02 --config Release`（MSVC Release）通过，
  **0 warning**（新测试中 `[[nodiscard]] Convert` 裸调用以
  `static_cast<void>` 显式丢弃，17 处逐一处理）。
- 测试：`libxcp_tests.exe` → **420 tests / 79 suites：419 通过、1 跳过**
  （预存在 `AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`，
  `tests/xcp_master_integration_test.cpp:495`）。
- 新增 18 个用例：
  - `daq_timestamp_test.cpp`（12）：基线建立、单调、8/16/32 位回卷延展
    （含 0 边界）、持平不判回卷、单位缩放、非法位宽、未知单位（回卷
    照常计数）、Reset 保历史、int64 饱和钳位；
  - `measurement_session_hardening_test.cpp`（6）：会话流 32 位回卷
    （2^32-1→0 = 2^32 ns）、每流独立回卷（2 流共 2 次）、未知单位仅保
    raw、运行中 SetTimestampUnit 抛、Stop→Start 基线复位 + 统计累计、
    畸形/未规划 odt/截断净荷混合序列的降级与恢复。
- A2lIsolation：`S1 扫描 21 个桥接层文件、S2 扫描 37 个主树文件，命中均为 0`
  （新增核心文件 `daq_timestamp.*`、`measurement_session.*` 均不引用 A2L）。

## 7. 教训

- `[[nodiscard]]` 返回值在测试里做"仅推进基线"调用时必须显式
  `static_cast<void>`，否则 MSVC C4834（本仓库按无警告标准交付）。
- 匿名命名空间内复制测试 harness（FakeDb/SessionSlave/Rig/FrameCollector）
  到新 TU：各测试文件独立编译，无符号冲突；两份 Rig 的 `SetTimestampUnit(1)`
  使 ns==raw 的既有断言全部原义保留。
