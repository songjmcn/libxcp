# 通用 XCP Master 工具 DAQ 实施记录

状态：实现与自动化验证完成；包括 XCPlite MAX_DAQ 语义修正。用户已于 m01561 批准整体计划，真实 Slave 联测尚未执行。

## 范围与约束

在既有 `tools/xcp_master/` UDP CLI 增加保守 DAQ 模式并保留默认 poll 模式；DAQ 仅按 A2L 唯一 `FIXED_EVENT_LIST` 自动绑定。通用模式只接受已验证的 timestamp-free Absolute-PID DTO、FREE 地址扩展与单 DAQ list；XCPlite 仅由 `--daq-profile xcplite` 显式选择。每次会执行 `FREE_DAQ` 的 watch 都先完成本地布局与运行时能力查询，再要求当次 `y` 确认；DAQ 运行时禁止 read/write/add/remove。未改 `thirdparty/a2llib/`，未复用 examples/ 代码。

## 当前实现

- `thirdparty/a2l-sdk/include/liba2l/liba2l_api.hpp`：`SymbolDto` 增加 MEASUREMENT 的 fixed/default/available DAQ_EVENT 向量；ABI 版本更新为 7。
- `thirdparty/a2l-sdk/src/liba2l_export.cpp`：按有效 XCPplus 优先、缺失/无效时回退 plain XCP 提取逐符号事件声明；`thirdparty/a2l-sdk/a2lbridge/src/a2l_dto_map.cpp` 与 `thirdparty/a2l-sdk/a2lbridge/include/libxcp/a2l/a2l_types.hpp` 分别映射到公开 `SymbolInfo`。
- `tests/a2l_golden_test.cpp` 与 `tests/data/a2l/measurement_daq_event.a2l`：新增 event 0、多 fixed、default-only、XCPplus 优先/fallback 的 A2L fixture 覆盖；Release 定向用例已通过。
- `adapter/a2l/measurement_runtime_profile.hpp/.cpp`：增加窄通用 Absolute-PID profile 查询；核验资源保护、动态 DAQ、识别字段、FREE 扩展、DAQ 编号、非固定时间戳、DAQ entry granularity/max size，以及 A2L EVENT 与 Slave runtime channel/time/capacity。generic 按 XCP `MAX_DAQ` 核验容量；XCPlite profile 不把 `MAX_DAQ` 当作容量，因为 XCPlite 返回的是当前已分配列表数。generic 与 XCPlite 均在 FREE_DAQ 前拒绝 DAQ 资源受保护的会话。A2L EVENT metadata 保留类型和 MAX_DAQ_LIST。
- `include/libxcp/xcp_master.hpp`、`src/xcp_master.cpp`：保留旧 `ConfigureDaqListsDynamic(specs)` 时间戳默认语义，新增显式识别头宽配置重载，按真实识别头/每列表时间戳预算 MAX_DTO。
- `include/libxcp/measurement/measurement_session.hpp`、`src/measurement/measurement_session.cpp`、`src/measurement/measurement_planner.cpp`、`src/daq/dto_envelope_decoder.cpp`：增加可关闭时间戳与识别头预算；通用 Absolute 模式仅允许 list 0，按 FIRST_PID 将绝对 PID 还原为相对 ODT，并拒绝低于 FIRST_PID 的帧；planner 拒绝 ODT 编号超过 255 的布局。
- `tools/xcp_master/main.cpp`、`tools/xcp_master/CMakeLists.txt`、`tools/xcp_master/README.md`：增加 `--mode poll|daq`、显式 `--daq-profile generic|xcplite`、固定事件信息展示、DAQ 命令边界、FREE_DAQ 二次确认、异步结果队列及 stop/join；链接 measurement adapter。README 记录受限布局和拒绝条件。
- `tests/measurement_adapter_test.cpp`、`tests/measurement_planner_test.cpp`、`tests/dto_envelope_decoder_test.cpp`、`tests/xcp_daq_test.cpp`：新增通用 profile/资源保护、识别头预算、FIRST_PID 解码及新旧动态配置时间戳语义相关用例；`tests/CMakeLists.txt` 覆盖 CLI 参数互斥。

## XCPlite MAX_DAQ 语义修正（m03242）

- 联测报告在 cpp_demo 中只选取 `SigGen1.time` 与 `SigGen1.value_` 时，XCPlite profile 报“动态 DAQ List 从 0 开始且容量非零”。
- 根因：`thirdparty/XCPlite/src/xcplite.c:2477-2478` 将 `GET_DAQ_PROCESSOR_INFO.MAX_DAQ` 填为 `shared.daq_lists.daq_count`，即当前已分配 list 数；fresh session 尚无 list 时 0 是正常值，不代表容量为零。`GET_DAQ_EVENT_INFO.MAX_DAQ_LIST` 才是另一个按事件返回的运行时字段。原 profile 把 MAX_DAQ 当总容量，并据此比较选中事件数，语义不适用。
- 已修改 `adapter/a2l/measurement_runtime_profile.cpp`：XCPlite profile 只要求 `MIN_DAQ==0`，不再以 processor MAX_DAQ 推断容量或限制事件数；保留每个 A2L EVENT 的 Slave runtime 信息校验。预配置仍先做本地布局检查并在执行 `FREE_DAQ` 前要求用户确认，之后分配失败仍由 XCP 错误反馈。
- 已修改 `tests/measurement_adapter_test.cpp`：用 processor MAX_DAQ=0 且两个有效 event 的场景覆盖 profile 接受；Release 下 `XcpliteMeasurementRuntimeProfile.*` 3/3 通过，全量构建及 CTest 496 项无失败（1 项既有 skip）。

## 验证结果

- XCPlite 修正后的 Release 全量构建通过：`cmake --build cmake-build-release --config Release`。
- 修正后的 Release 全量 CTest 通过：`ctest --test-dir cmake-build-release -C Release --output-on-failure`，495 项通过、1 个既有测试按预期跳过（`AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`）。修正后 XCPlite profile 定向测试 3/3 通过；此前 profile 6/6、DAQ/CLI 核心定向测试 25/25 也通过。
- 独立配置 `LIBXCP_BUILD_TOOLS=OFF` 成功，`cmake-build-tools-off` 下未生成 `tools/` 构建目录。A2L SDK 构建期间只见既有 C++20 `std::filesystem::u8path` 弃用告警与 bridge 局部变量遮蔽告警；未修改 `thirdparty/a2llib/`。
- `git diff --check` 通过。真实 Slave 手工验证可选，本次未执行。
