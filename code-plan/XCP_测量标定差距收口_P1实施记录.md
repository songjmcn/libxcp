# XCP 测量标定差距收口计划 —— P1 实施记录

## 1. 范围与结论

本记录覆盖 P1 首轮实现及经用户确认的 T-02 适配器复用扩展：以运行时元数据建立 XCPlite profile，显式提供 symbol-event 关系，增强测量配置/生命周期检查并核正 GET_DAQ_EVENT_INFO CRO；随后在独立 `libxcp_measurement_adapter` 增加可复用绑定包装器与 XCPlite profile utility。未修改核心 `include/libxcp` API 或 `thirdparty/`。Windows Release 的 A2L OFF/ON 全量测试通过；Linux/macOS 与真实非 XCPlite 测量 Slave 尚未验证，故不代表 G2 或全协议互操作完成。

## 2. 元数据与 XCPlite profile

`tests/xcplite_measurement_test.cpp` 现在从上传的运行时 A2L `/begin EVENT` 项解析周期和时间单位，再查询 Master 的 GET_DAQ_PROCESSOR_INFO、GET_DAQ_RESOLUTION_INFO、GET_DAQ_EVENT_INFO，并交叉核对事件周期/单位。只有固定 32-bit timestamp、可识别的时间单位和有效 tick 才接受该 profile；支持的 ns 时间单位按 tick 转为 measurement session 的时间缩放。XCPlite profile 显式配置 `RelativeByte` 解码模式、4-byte envelope header、timestamp 仅首 ODT，并在 Prepare 前设置。

变量到事件的关系不是 A2L 能证明的信息，因此集成测试通过显式 symbol-event binding table 提供；未映射或存在歧义会报 InvalidLayout，不再把所有变量默认为 event 0。事件通道以独立的“是否找到”状态表达，避免将合法 event 0 当作缺失。测试另核对非法通道的运行时查询失败。XCPlite 的 DAQ identification raw code 3（`DAQ_HDR_ODT_FIL_DAQW`）映射到本库当前的 RelativeByte vendor profile；这不是对所有 XCP Slave 方言的通用推断。

已有非 XCPlite 测量单测仍覆盖不同的 RelativeWord DTO envelope、多事件路由与显式时间戳配置；该部分是模拟测试，不是非 XCPlite 真实设备互操作。

## 3. 防御性检查与协议修正

- `src/measurement/measurement_session.cpp`：拒绝未知 identification mode 和短于该模式最低长度的 envelope header；`Start` 前核对 Prepare 保存的 DAQ 配置 generation，检测 Prepare 后 DAQ 表被改动/清空的过期计划。
- `src/daq/dto_envelope_decoder.cpp`：对未知 identification mode 抛 MalformedPacket，而非按默认布局继续解码。
- 新增单元测试：`tests/dto_envelope_decoder_test.cpp` 未知模式；`tests/measurement_session_test.cpp` 未知模式/过短 header、陈旧 DAQ generation、阻塞 consumer 时 256 容量队列丢弃计数（送入 300 个额外 DTO，期望丢弃 44 个）。
- 核对规范 CRO 布局后修正 `src/command_codec.cpp::EncodeGetDaqEventInfo`：GET_DAQ_EVENT_INFO CRO 为 4 字节 `[0xD7][0x00 reserved][event lo][event hi]`。根据 `thirdparty/XCPlite/src/xcp.h:817-819` 的 CRO 长度与 `CRO_WORD(1)` 定义，reserved byte 位于 event WORD 前。更新 `tests/command_codec_test.cpp` golden packet 和 `tests/get_daq_event_info_test.cpp` executor wire assertion。

## 4. P1 首轮验证记录（历史基线）

Windows 本机，Visual Studio 2022 Release，使用 P0 的构建目录增量编译变更目标后运行全量 CTest：

| 构建目录 | 选项 | 结果 |
|---|---|---|
| `build-p0-on` | `LIBXCP_BUILD_TESTS=ON`、`LIBXCP_BUILD_A2L=ON`、`LIBXCP_BUILD_XCPLITE_SLAVE=ON` | `cmake --build ... --config Release --target libxcp_tests XcpliteIntegration --parallel 1` 成功；CTest **496/496 通过，0 失败**，1 个既有 skip：`AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`；35.14s。覆盖测量 lifecycle/queue、DAQ event command、XCPlite A2L/DAQ/measurement E2E。 |
| `build-p0-off` | `LIBXCP_BUILD_TESTS=ON`、`LIBXCP_BUILD_A2L=OFF`、`LIBXCP_BUILD_XCPLITE_SLAVE=OFF` | `cmake --build ... --config Release --target libxcp_tests --parallel 1` 成功；CTest **451/451 通过，0 失败**，同一既有 skip；19.51s。 |

第一次 ON 全量 CTest 发现 `tests/get_daq_event_info_test.cpp` 仍断言 CRO 长度为 3（实际新正确编码长度为 4）；同步更新该旧 executor test 后，ON 全量重跑全绿。聚焦 40 项 profile/session/decoder/codec 测试也全部通过。

## 5. T-02 复用型 Adapter 接入（经确认实施）

- 新增 `adapter/a2l/measurement_runtime_profile.hpp/.cpp`，编入独立的 `libxcp_measurement_adapter`。不修改 `include/libxcp` 公共 API，也不修改 `thirdparty/`。
- `EventBoundMeasurementDatabase` 接受调用方提供的全限定 symbol → event candidate 列表；缺失/空列表、多种不同候选或超出 `uint16_t` 返回 `InvalidLayout`，唯一显式 event 0 合法；`ToPhysical` 委托原数据库。重复相同候选视为同一绑定。
- `BuildXcpliteMeasurementRuntimeProfile` 从 `XcpMaster` 查询 DAQ processor/resolution/event，并与输入的 A2L EVENT channel/cycle/unit 逐通道交叉校验。只接受 XCPlite 实然 profile：wire identification code 3 映射为本库 XCPlite `RelativeByte + 4-byte header` envelope；要求动态 DAQ、timestamp capability、固定 32-bit timestamp、ticks>0、unit code 0..6 且乘法不溢出；单位映射仅属于本 profile。未知或缺失证据报 `DaqConfigurationError`，不猜通用 XCP 方言。
- `measurement_demo` 现从上传 A2L EVENT/MEASUREMENT 文本收集通道和元数据并查询实时 profile；缺少逐符号映射仅在用户明确提供 `--event` 时回退，并在输出中注明。该 CLI 文本映射仅面向当前 XCPlite 生成格式；通用应用仍须提供其可证明的绑定来源。

### 最新验证（Windows / VS2022 / Release）

| 配置 | 构建/测试结果 |
|---|---|
| `build-v09`：A2L ON、XCPLITE ON | `MeasurementAdapterTest` 与 `measurement_demo` 增量构建成功；聚焦 wrapper/profile/demo **6/6 通过**；全量 CTest **513 项：512 通过、1 个既有 SKIP、0 失败**。SKIP 仍为 `AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`。 |
| `build-p0-off`：A2L OFF、XCPLITE OFF | `libxcp_tests` 增量构建成功；全量 CTest **460 项：459 通过、1 个同类既有 SKIP、0 失败**。 |

`clang-format` 已运行于本批涉及的 C++ 文件；CTest 证明仍只覆盖 Windows、本地 Mock/UDP Loopback 与 vendored XCPlite，不构成非 XCPlite ECU 或 Linux/macOS 互操作验证。

## 6. 未完成项

- 无非 XCPlite 真实 Slave/ECU 的事件绑定、DTO layout、timestamp profile 抓包验证；现有不同 envelope 用例仅模拟。
- Linux/macOS 构建和 POSIX XCPlite 子进程夹具未在本批实测。
- P2 真实受保护 Seed&Key 写回、P3 分页/MODIFY_BITS 真实对手端闭环及非幂等超时决策、P4 跨平台/非自有对手端互操作仍按总计划未完成。