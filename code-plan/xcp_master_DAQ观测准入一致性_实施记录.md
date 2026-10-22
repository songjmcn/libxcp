# XCP Master DAQ 观测准入一致性实施记录

## 用户现象与原因

用户在 DAQ/generic 模式下用 F6 将 5 个 `cal.basic.*` CHARACTERISTIC/VALUE 加入采样集合，随后用 F7 输入 `watch`，未见观测值。截图顶部显示“已停止”。所附 `cmake-build-release/tools/xcp_master/logs/tools/xcp_master/xcp_master.INFO.20261020-175601.73308.glog` 只覆盖连接阶段且末行不完整，没有 `watch` 的执行记录。源码检查发现加入路径允许可读取的标定标量，而 DAQ 启动路径仅接受 MEASUREMENT，故截图中的标定量不满足 DAQ 静态资格；不据缺失日志声称具体失败时序。

## 修改范围

- `tools/xcp_master/symbol_policy.hpp`：新增工具私有的 `SymbolDaqObservationUnsupportedReason(const a2l::SymbolInfo&, std::span<const std::uint16_t>)`；拒绝非 MEASUREMENT、数组、不安全标量、固定事件不唯一及事件目录缺失，重复同一事件号按既有规则去重。
- `tools/xcp_master/main.cpp`：让列表的观测提示、F6 加入观测和 DAQ `watch` 启动前检查共用上述 DAQ 静态准入；POLL 路径维持原有资格；将变量行的“读取拒绝”更正为“观测拒绝”，与独立的单次读取资格区分。保留 DAQ 运行期事件/能力/配置检查、原有确认门槛及读数路径。
- `tests/xcp_master_symbol_policy_test.cpp`：新增两项 DAQ 策略回归，覆盖 CHARACTERISTIC、数组、无固定事件/仅默认事件、多不同固定事件、缺失 EVENT_CHANNEL、合法标量和重复同一固定事件号；断言标定量在原读取策略中仍可读取。
- `tools/xcp_master/README.md`：说明 F6 的 DAQ 静态准入条件和“准入不等于 ECU 实际采样”。未修改 SDK、公共 API、第三方源码或按键映射。

## 验证

- `cmake --build cmake-build-logging-release --target xcp_master XcpMasterSymbolPolicyTest -j 4`：AppleClang Release 构建通过；存在已有链接器重复库和第三方 GoogleTest `char8_t` 警告，未导致失败。
- `ctest --test-dir cmake-build-logging-release --output-on-failure -j 4 -R 'XcpMasterSymbolPolicyTest|XcpMasterCalibrationPolicyTest|LatestObservationModelTest|CommandHistoryTest|CalibrationFormModelTest|VariableModalStateTest|XcpMasterSymbolDisplayTest|XcpMasterTool|TuiInputModeStateTest|ConfirmationGateTest|PreviewRowsTest'`：60/60 通过，其中包含新增两项 DAQ 策略测试。
- `cmake --build cmake-build-release --target xcp_master -j 4`：用户提供日志所在的 Release 构建目录中的 `tools/xcp_master/xcp_master` 已重编译成功；该目录 `LIBXCP_BUILD_TESTS=OFF`，测试由上一个 Release 目录执行。
- `clang-format --dry-run --Werror tools/xcp_master/main.cpp tools/xcp_master/symbol_policy.hpp tests/xcp_master_symbol_policy_test.cpp && git diff --check`：通过。

## 未覆盖与用户复验

本轮未用真实终端/ECU 验证 DAQ DTO 能否到达、数值是否更新，也未验证跨平台构建。请退出旧进程，运行更新后的可执行文件；在 DAQ 模式选择 A2L 中具有唯一 FIXED_EVENT_LIST 和匹配 EVENT_CHANNEL 的 MEASUREMENT 标量（不要选 `cal.basic.*`），按 F6 加入后用 F7 输入 `watch`。如果顶部仍是“已停止”，用 F2 切到“命令结果”记录原样拒绝提示；若显示“DAQ 运行中”但值不更新，记录变量名、结果区提示和本次新生成日志，以便进一步区分 ECU/DTO 与 UI 刷新问题。POLL 模式仍可用于读取此前支持的 CHARACTERISTIC/VALUE。
