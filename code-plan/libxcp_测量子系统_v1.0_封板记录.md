# libxcp 测量子系统 v1.0 封板记录

> 日期：2026-09-29 ｜ 里程碑：v1.0（封板）｜ 计划文档：`code-plan/libxcp_测量子系统代码增长计划.md`
> 范围：Measurement-only 最小功能集。封板后核心公开面进入冻结期（只增不改不删）。

---

## 1. 封板结论

**v1.0 封板通过。** v0.2 → v0.3 → v0.4 → v0.5 → v0.6 → v0.9 → v1.0 全部里程碑的交付物、
测试与记录 MD 均已落盘并验证；全量回归 **466/466 通过**（含 A2lIsolation 与两个示例），
60 秒 UDP 回环长稳 **零丢弃、零解码错误**；核心既有文件改动经 numstat 审计为**纯插入**。

## 2. 冻结的公开面（核心，namespace `calmcar::xcp`）

### 2.1 `IMeasurementDatabase`（include/libxcp/measurement/measurement_database.hpp，64 行）

窄接口，**恰好两个纯虚**（v1.0 从盘复核，纠正早期"三方法"笔记——`ElementSize` 不在最终面上，
元素宽度由 `MeasurementSymbolInfo::element_size_bytes` 携带）：

```cpp
[[nodiscard]] virtual MeasurementResult<MeasurementSymbolInfo> Find(std::string_view name) const = 0;
[[nodiscard]] virtual MeasurementResult<MeasurementValue> ToPhysical(std::string_view name, BytesView raw) const = 0;
```

- `MeasurementSymbolInfo{Address address; AddressExtension extension; uint16 event_channel(0=不由通道触发);
  uint8 element_size_bytes(B-1 实占口径); uint8 element_count{1}（首版仅标量=1）}`
- 不可拷贝；生命周期须长于使用者。

### 2.2 `MeasurementSession`（include/libxcp/measurement/measurement_session.hpp，191 行）

```
MeasurementSession(IMeasurementDatabase&) :77      // 先构造，后 Bind（偏差已获用户确认并记录 v0.5 §4.1）
~MeasurementSession() override :78                  // 自动 Stop
Bind(XcpMaster&) :88                                 // 一次性绑定，重复 → XcpException(InvalidArgument)
Add / Remove / Clear :91-95                          // Clear 仅停止态
Prepare() :101                                       // 规划 + ConfigureDaqListsDynamic 落表
Start(MeasurementCallback) :108                      // 触发 StartDaqSync + worker 线程
Stop() :111   Running()   Statistics()               // 统计四原子量 dto_received/dto_dropped/decode_errors/timestamp_wraps
SetEnvelopeMode(IdentificationFieldType, size_t) :127 // Prepare 后可改（B-16）
SetTimestampUnit(uint64) :139                         // ns/tick；运行中改 → InvalidState
SetTimestampFirstOdtOnly(bool) :150                   // XCPlite D13：时间戳仅随事件首 ODT 帧
IEventListener: OnEvent/OnService no-op + OnDto :153-158   // kMaxQueue=256 有界队列，溢出丢弃并计数
```

### 2.3 `DtoEnvelopeDecoder` / `DtoFrameLayout`（include/libxcp/daq/dto_envelope_decoder.hpp 73 行 + dto_envelope_types.hpp 80 行）

```cpp
explicit DtoEnvelopeDecoder(ByteOrder = Intel) noexcept :62;
[[nodiscard]] DtoEnvelope Decode(BytesView dto, const DtoFrameLayout& layout) const :73;
// DtoEnvelope{identity{daq_list,odt}, optional counter, optional raw_timestamp, payload}
// DtoFrameLayout 尾字段 timestamp_relative_first_only=false（v0.9 追加，默认不改 v0.2 语义）
```

### 2.4 `DecodePayload`（include/libxcp/daq/dto_payload_decoder.hpp，53 行）

```cpp
struct PayloadSlice{ std::string name; size_t offset; size_t size; };   // offset 相对 envelope.payload@0
struct DecodedSlice{ std::string name; BytesView bytes; bool valid=true; };
[[nodiscard]] std::vector<DecodedSlice> DecodePayload(BytesView payload,
                                                      const std::vector<PayloadSlice>& specs) noexcept;
// 越界切片 → valid=false，不抛异常
```

### 2.5 `DaqTimestampConverter`（include/libxcp/daq/daq_timestamp.hpp，63 行）

```cpp
DaqTimestampConverter(uint64 unit_ns, uint8 bits) noexcept;
[[nodiscard]] DaqTimestamp Convert(uint64 raw) noexcept :55;   // 基线内部维护（v0.6 偏差记录）
void Reset() noexcept :58;   uint64 WrapCount() const noexcept;
// DaqTimestamp{uint64 raw; nanoseconds value; bool valid}
```

### 2.6 值类型与错误模型（measurement_types.hpp 36 行 / measurement_result.hpp）

- `MeasurementValue = std::variant<int64_t, uint64_t, double, std::string, bool>`（镜像 a2l::PhysicalValue）
- `MeasurementResult<T>` = `optional<T>` + `MeasurementError{code, message}`；`MeasurementErrorCode` 13 值
- 会话对外抛 `XcpException`，类别经 `CategoryOf` 映射（v0.6 §2.4 表）

## 3. 可选组件（不入冻结面）

`adapter/a2l/A2lMeasurementDatabase`（adapter/a2l/，66+119 行）：把 `a2l::IA2lDatabase` 投影为
`IMeasurementDatabase`。依赖 `LIBXCP_BUILD_A2L`（需 `LIBXCP_LIBA2L_ROOT`），构建开关说明见
README §构建。**桥接类型不得进入核心头**（A2lIsolation 守护）。

## 4. 全量回归（v1.0 封板口径，build-v09：TESTS+XCPLITE_SLAVE+A2L+EXAMPLES 全开）

| 项 | 结果 |
| --- | --- |
| `ctest` 全量 | **466/466 通过，0 失败**（44.96s；1 跳过 = 既有 `Ag1_Cto8`，非本里程碑引入） |
| libxcp_tests | 420 用例（79 套件）：419 通过 / 1 既有跳过 |
| #464 A2lIsolation | PASS —— S1 桥接层 21 文件 / S2 主树 37 文件，命中 0 |
| #465 ExampleXcpMasterUdp | PASS 5.99s（此前 build-v09 未编该 exe 属构建缺口，已补） |
| #466 ExampleMeasurementDemo | PASS 4.98s（ctest 内 2s 冒烟档） |
| XcpliteIntegration | 31/31（含 L3 全链路 `XcpliteMeasurementTest` 两例） |
| MeasurementAdapterTest | 6/6 |

## 5. 长时间 DAQ 稳定性（§25 最后一项，本轮闭环）

`measurement_demo --seconds 60`（UDP 回环，真实整窗采样）：

```
received=56571 (≈943 DTO/s)  dropped=0  decode_err=0  wraps=14
断言 10/10 通过，退出码 0
```

- 零丢弃 ⇒ v0.9 OnDto 漏唤醒修复在满负荷下成立（修复前 recv=4521/dropped=4265）。
- `wraps=14` 为**预期行为**：slave 32 位相对 tick 在 2^32 tick ≈ 4.3 s 一圈，60 s 必然多圈；
  转换器逐圈 +2^32 外推，正是 v0.6 的 wrap 语义（断言口径据此从"wraps==0"改为"wraps>0 且
  decode_err==0"——教训：**断言口径必须来自被测实然**，本里程碑第三次踩同一坑）。
- 长稳取证口径：显式 `--run-dir <可写目录>`（沙箱下默认当前目录创建会被拒）。

## 6. 纯追加合规审计（I4 / 不重构已完成代码）

`git diff --numstat` 对 9 个既有核心文件：**全部纯插入，零公开签名删除**。

| 文件 | + / − |
| --- | --- |
| include/libxcp/command_codec.hpp | 11 / 0 |
| include/libxcp/command_executor.hpp | 14 / 0 |
| include/libxcp/protocol_types.hpp | 4 / **1**（唯一删除 = 0xD7 注释文案改写，非签名） |
| include/libxcp/response_parser.hpp | 33 / 0 |
| include/libxcp/xcp_master.hpp | 16 / 0 |
| src/command_codec.cpp | 11 / 0 |
| src/command_executor.cpp | 25 / 0 |
| src/response_parser.cpp | 23 / 0 |
| src/xcp_master.cpp | 5 / 0 |

新增文件（全新不触碰既有语义）：`include/libxcp/{daq,measurement}/`、`src/{daq,measurement}/`、
`adapter/a2l/`、`examples/measurement_demo/`、8 个新测试 cpp。`thirdparty/` 零改动。

## 7. 已知限制（封板附带，后续里程碑候选）

1. **时间戳宽度时序**：首次 `Prepare` 在 `ConfigureDaqListsDynamic` 动态门前读
   `DaqTimestampBytesCached`，可能取 0（惰性 GET_DAQ_RESOLUTION_INFO 门内才触发）。
   v0.6 §5 决策维持现状；重规划路径 = 二次 Prepare 或 master 侧提前查询。
2. **事件通道映射**：适配器 `Find` 恒 `event_channel=0`，运行时取证注入由上层
   （demo/L3 测试的 EventBound 装饰器模式）补充；核心不感知。
3. **数组/结构体符号**：`element_count` 首版仅标量=1，规划器遇数组显式拒绝（不猜布局）。
4. **Polling 测量与 CAN 传输**：计划 §2 范围外，未实现。
5. **回绕跨圈上限**：单帧最多外推 +1 圈（不猜帧丢失），帧率高于 1 圈/帧时外推滞后。
6. **L4 真 ECU 冒烟清单**（v0.9 MD §6 九步）在 CI 之外的人工验证，交付方执行。
7. MeasurementAdapterTest/XcpliteIntegration 依赖 prepared liba2l root
   （`thirdparty/a2l-sdk/build-sdk.ps1`），默认 OFF 配置不编译——CI 需显式开 `LIBXCP_BUILD_A2L=ON`。

## 8. 封板声明

- 冻结面：§2 所列头文件公开签名自本记录起冻结；变更须走"仅追加"（新可选参数/新方法/新错误码尾部追加），
  破坏性变更另立版本。
- 记录链完整：`code-plan/libxcp_测量子系统_{v0.2,v0.3,v0.4,v0.5,v0.6,v0.9}_记录.md` → 本文件。
- 快速上手：根 `README.md`（测量链路 ≤10 行示例 + 构建开关表）。
- 架构文档 §25 封板检查表 13/13 全勾（`code-plan/libxcp_measurement_architecture.md:1187-1199`）。
