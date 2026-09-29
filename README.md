# libxcp —— XCP 1.3.0 Master SDK（C++20）

`calmcar::xcp` 提供跨平台（Windows / Linux / macOS）的 XCP Master 静态库，
目标名 `libxcp::libxcp`。第一版（v1.0）封板范围为 **Measurement-only 最小功能集**：

```text
A2L 变量名 → MeasurementPlanner → Dynamic DAQ 配置 → ECU DTO
           → DtoEnvelopeDecoder → Timestamp/换算 → MeasurementFrame 回调
```

Calibration / Programming / Polling 读面等其余能力不在本版承诺范围；
CAN / CAN FD Transport 为 v1.0 之后的下一步（见
`code-plan/libxcp_measurement_architecture.md` §25/§26）。

## 构建

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

可选开关（全部默认 OFF）：

| 开关 | 作用 |
|------|------|
| `LIBXCP_BUILD_TESTS` | GoogleTest 单测/集成测试（ctest 套件） |
| `LIBXCP_BUILD_A2L` | 构建 A2L 桥接与 `libxcp::measurement_adapter`（需 `LIBXCP_LIBA2L_ROOT` 指向预备好的 liba2l 根，由 `thirdparty/a2l-sdk/build-sdk.ps1` 产出） |
| `LIBXCP_BUILD_XCPLITE_SLAVE` | 构建 XCPlite 回环 Slave（L3 集成测试载体） |
| `LIBXCP_BUILD_EXAMPLES` | 构建示例（`xcp_master_udp`、`measurement_demo`；需上面两门同开） |

核心库与测量路径**不依赖 A2L**：`IMeasurementDatabase` 是核心窄接口，
A2L 接入是独立可选组件（`adapter/a2l/A2lMeasurementDatabase`）。
依赖隔离由 ctest `A2lIsolation` 门禁持续校验。

## 测量快速上手（变量名 → 帧回调）

```cpp
using namespace calmcar::xcp;
MeasurementSession session(database);          // IMeasurementDatabase（可用 A2L 适配器）
session.Bind(master);                          // master 以 &session 作 listener 构造
session.SetEnvelopeMode(IdentificationFieldType::RelativeWord, 2U);
session.Add("EngineSpeed");                    // 全库唯一名或规范键
session.Prepare();                             // 规划 + 动态 DAQ 落表
session.Start([](const MeasurementFrame& f) {  // 物理值帧回调（worker 线程）
    for (const auto& s : f.samples) { /* s.name / s.value / s.valid */ }
});
// ... 采集期间可查 session.Statistics()（received/dropped/decode_errors/wraps）
session.Stop();
```

时间戳语义：Slave 若只对事件首 ODT 帧盖时间戳（XCPlite 实然），
在 `Prepare()` 前调用 `session.SetTimestampFirstOdtOnly(true)`，
并经 `SetTimestampUnit(ns_per_tick)` 注入分辨率；回卷由内部
`DaqTimestampConverter` 外推并计入 `Statistics().timestamp_wraps`。

端到端可运行样例：`examples/measurement_demo/measurement_demo.cpp`
（启动 XCPlite Slave → UPLOAD 取 A2L → 适配器 → 会话全链路，含 RAII 守卫范式）。

## 公开头文件（v1.0 冻结面）

- `include/libxcp/daq/`：`dto_envelope_types.hpp`、`dto_envelope_decoder.hpp`、
  `dto_payload_decoder.hpp`、`daq_timestamp.hpp`
- `include/libxcp/measurement/`：`measurement_types.hpp`、`measurement_result.hpp`、
  `measurement_database.hpp`、`measurement_planner.hpp`、`measurement_sample.hpp`、
  `measurement_session.hpp`
- 既有协议核心（`xcp_master.hpp` 等）仅追加式扩展，签名未变。

封板与已知限制记录：`code-plan/libxcp_测量子系统_v1.0_封板记录.md`；
各里程碑实施记录见 `code-plan/libxcp_测量子系统_v0.*_记录.md`。

## 测试

```sh
ctest --test-dir build -C Release --output-on-failure
```

L1 单元（解码器/规划器/时间戳）→ L2 mock transport（会话状态机）→
L3 XCPlite UDP 回环 + 真实 A2L（`XcpliteIntegration`）→ `A2lIsolation` 静态门禁。
L4 真 ECU 冒烟清单见 `code-plan/libxcp_测量子系统_v0.9_记录.md` §6。
