# XCP 测量与标定差距收口——后续任务拆分

> 根据 `code-plan/XCP_测量标定差距收口与互操作验证计划.md` 和当前代码/测试证据整理。C++20；禁止未经授权修改 `thirdparty/`。任何代码/API任务须先按 `AGENTS.md` 提交具体设计并获批准；设备或平台不可用时如实标记未验证，不以模拟测试替代。

## 状态总览

- Windows Release 最新门禁：A2L+XCPlite ON 为 513 项（512 通过、1 个既有 SKIP、0 失败）；A2L/XCPlite OFF 为 460 项（459 通过、同一既有 SKIP、0 失败）。仅覆盖 Windows、本地 Mock/UDP Loopback 与 vendored XCPlite，不代表外部设备或 Linux/macOS 互操作通过。
- P0 文档口径补充修订已实施：本清单 T-01。
- P1 首轮测量 profile 与硬化已完成；经批准的 T-02 适配器显式绑定/runtime profile 复用接口也已实施并验证，详见 `code-plan/XCP_测量标定差距收口_P1实施记录.md`。非 XCPlite 真实设备验证仍未完成。

## 任务

### T-01｜消除 XCPlite 集成计划中的历史口径冲突 — 已完成

- 修改 `code-plan/XCPlite_Slave_协议调试集成计划.md`：Phase2-02 将 `WriteCalParam` 限定为现有非保护 CalSeg 写读且不调用 Unlock；§5 将“不实现 DAQ DTO 实时采集”注明为初始阶段范围，并指向后续批次的真实 XCPlite DAQ/测量测试。
- 变更已记入该计划的 §8；第三方代码、协议实现与测试均未修改。本次仅文档修订，不需运行测试。

### T-02｜可复用变量—事件绑定与 XCPlite runtime profile — 已完成（Windows）

- 前置事实：`a2l::SymbolInfo` 没有 symbol-event 字段（`thirdparty/a2l-sdk/a2lbridge/include/libxcp/a2l/a2l_types.hpp`），`adapter/a2l/a2l_measurement_database.cpp` 的 `event_channel=0` 是中性缺省，不能视为真实事件 0。经用户确认后在独立 `libxcp_measurement_adapter` 增加 `adapter/a2l/measurement_runtime_profile.hpp/.cpp`；核心 `include/libxcp` API 与 `thirdparty/` 均未改。
- `EventBoundMeasurementDatabase` 接受调用方显式提供的全限定符号→event candidate 列表；缺失/空绑定、多种不同候选及超出 `uint16_t` 均报 `InvalidLayout`；唯一 event 0 合法；物理换算委托内部数据库。重复相同候选作为同一绑定。
- `BuildXcpliteMeasurementRuntimeProfile` 使用现有 `XcpMaster::QueryDaqProcessorInfo()`、`QueryDaqResolutionInfo()`、`QueryDaqEventInfo(channel)` 并逐事件核对 A2L EVENT channel/cycle/unit 与 Slave 实际返回值。只接受取证的 XCPlite profile：identification-field raw code 3 映射到 `RelativeByte/4-byte header`；要求动态 DAQ、timestamp capability、fixed 32-bit timestamp、ticks>0、unit code 0..6 且缩放不溢出；应用策略为 timestamp only on first ODT。缺失或未知事实报配置错误，不猜通用 XCP 方言。
- `measurement_demo` 已改用这套 adapter wrapper/profile；EVENT/MEASUREMENT 解析限定于当前 XCPlite 运行时 A2L 格式。不能证明逐变量绑定时，只允许用户显式传 `--event` fallback，并在诊断中注明来源；这是示例策略，不是通用库默认。
- 验收：profile/wrapper 单测含显式 channel 0、缺失/歧义/越界、多个事件、unknown/mismatch processor/timestamp/event 事实、空证据与 overflow；`ExampleMeasurementDemo` 与 XCPlite 全链路回归通过。Windows Release A2L+XCPLITE ON CTest 513 项（512 PASS、1 既有 SKIP、0 FAIL）；A2L/XCPLITE OFF 460 项（459 PASS、同一既有 SKIP、0 FAIL）。完整结果及边界见 `code-plan/XCP_测量标定差距收口_P1实施记录.md`。仅为 Windows + Mock/Loopback + vendored XCPlite 证据，未完成真实非 XCPlite 设备或 Linux/macOS 互操作。

### T-03｜非 XCPlite 测量 profile 真实互操作 — 设备门禁未满足

- 需要用户提供/选定一个可用的非 XCPlite Slave/ECU 与兼容范围。
- 取证 CONNECT、DAQ processor/resolution/event、DTO identification/timestamp，并以真实采集帧核对；设备不可用时只保留模拟证据，标为未验证。

### T-04｜受保护 CAL 写回真实闭环 — 设备门禁未满足

- 需要支持 Seed&Key、CAL/PAG 保护的独立 Slave/ECU、获准测试密钥和安全测试地址。
- 验证锁定拒写、正确 key 解锁后写读回、错误 key 拒绝及断线/重连状态；禁止触碰生产区。现有 UdpTestSlave 模拟结果不计入真实设备 DoD。

### T-05｜Page Switching / MODIFY_BITS 外部闭环 — 设备门禁未满足

- 需要支持相应命令的真实独立对手端及明确的安全 CalSeg/页映射。
- 按支持子集验证能力查询、段/页查询、切页与读回、位掩码修改、COPY_CAL_PAGE 正负例；核对响应丢失时的实际状态。已完成的 Mock no-replay 测试不能替代互操作。

### T-06｜P4 跨平台与 UDP 故障门禁 — 环境/测试项未完成

- 在 Linux/GCC 或 Clang、macOS/Clang 独立跑 Release 构建、核心/DAQ/测量/A2L 与 XCPlite 子进程测试；当前 Windows/WSL 记录不构成 POSIX 通过证据。
- 为 `SO_RCVTIMEO` 设置失败分支设计可测注入并验证关闭/错误传播；实现前审查是否需要引入最小测试缝隙。
- 在至少一种非自有对手端核对 CONNECT、UDP LEN/CTR、DAQ DTO/timestamp、CAL 资源及写回负响应并形成脱敏记录。

### T-07｜能力限制与发布材料收口 — 待完成

- 汇总已实现、仅 Mock/Loopback 验证、XCPlite profile 专属、外部设备未验证及明确不支持的能力；包括 STIM、Programming/NVM、非 XCPlite 泛化测量等边界。
- 测试数/跳过项、平台、设备/软件版本与互操作证据均须可追溯；不宣称“完整 XCP”或 CANape 认证。

## 执行顺序与停止条件

1. T-01 已完成。
2. T-02 已按经确认设计实现并通过 Windows ON/OFF 回归；保留其 XCPlite 专属边界，不把 Mock/vendored Slave 结果扩展为通用互操作。
3. T-03/T-04/T-05 依赖真实对手端；设备未选定时仅维护模拟覆盖，不宣称互操作完成。
4. T-06 在可用平台执行；环境不可用则保留未验证状态。T-07 在证据稳定后整理发布说明。
