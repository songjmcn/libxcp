# XCP Master DAQ 观测准入一致性修复计划

状态：设计各节已获用户确认；工具私有策略和提示已实现，Release 聚焦构建及回归通过；真实 ECU DAQ 采样待用户复验。测试命令与结果见同目录实施记录。

## 1. 现象与证据

用户在 DAQ/generic 模式下用 F6 将 5 个 `cal.basic.*` CHARACTERISTIC/VALUE 加入采样集合，随后用 F7 输入 `watch`，预览区一直没有观测值。所附截图顶部显示“DAQ/generic | 已停止”，变量行显示“等待新样本 | 已加入采样”。所附 `xcp_master.INFO.20261020-175601.73308.glog` 只有连接阶段的信息，末行不完整，不能据此确认 `watch` 的执行过程。

现有 `tools/xcp_master/main.cpp` 的 `ObservationUnsupportedReason` 在标量路径仅调用 `SymbolReadUnsupportedReason`，因而允许可读的 CHARACTERISTIC；`Add` 使用这一判断；`StartDaqWatch` 却只允许 MEASUREMENT，并另外要求唯一 FIXED_EVENT_LIST 和可找到的 A2L EVENT_CHANNEL。该准入不一致足以解释截图中这 5 个标定量不能产生 DAQ 样本；不把日志缺失部分推断为实际执行记录。

## 2. 范围与架构（已确认）

- 只调整工具私有的 DAQ 模式静态准入策略及用户提示；POLL 模式仍可观测此前支持的标量 CHARACTERISTIC/VALUE 与已验证的一维 MEASUREMENT 数组。
- DAQ 仅支持现有能力范围：可读的标量 MEASUREMENT、有且仅有一个不同的固定事件号，且对应 A2L EVENT_CHANNEL 存在。重复声明同一个事件号时沿用现有去重规则。保留 `watch` 时的运行期检查（会话能力、事件/ODT/DTO 配置等），不扩大协议支持。
- 不更改 SDK/API、ECU、第三方库、键位或单次读取规则。

## 3. 组件、数据流与错误处理（已确认）

- 在工具私有 `tools/xcp_master/symbol_policy.hpp` 中增加一个面向 DAQ 的静态资格判断，使用现有 `SymbolInfo` 和 A2L 事件号作为输入、中文拒绝原因为输出；已有 `SymbolReadUnsupportedReason` 不变。
- `tools/xcp_master/main.cpp` 中的 `ObservationUnsupportedReason` 按 mode 分流：POLL 保持原有判断，DAQ 调用新资格判断。变量列表使用该原因显示“观测拒绝: …”；F6 加入前沿用相同原因明确拒绝，不让明知不可采的变量进入集合。
- `StartDaqWatch` 再次调用相同的静态资格判断（处理运行期状态与防御性复核），并保留它原有的实时配置校验和失败返回；不尝试按 DAQ 读取标定量或数组。
- 单次读取的拒绝原因由 `PreviewReadUnsupportedReason` 独立决定，不能因 DAQ 不可观测而误报为不可单次读取。

## 4. 测试与验收（已确认）

- 在 `tests/xcp_master_symbol_policy_test.cpp` 增加 DAQ 资格单测：CHARACTERISTIC 和数组拒绝；无固定事件、多不同固定事件、缺失 A2L 事件拒绝；合法 MEASUREMENT 标量及同一事件号重复声明通过。保留既有 POLL/读取资格测试并执行相关回归。
- 执行可行的 Release 聚焦构建、相关 CTest、clang-format 与 `git diff --check`；如运行完整构建则先执行 clean。将实际命令与测试结果写入同目录独立实施记录。
- 真实终端/ECU DAQ 流入、数值实时变化及跨平台行为由用户在目标环境复验；未运行不得记为通过。

## 5. 自审

本方案中的“可加入观测”只表示通过 A2L 静态资格，不保证 ECU 支持启动 DAQ；“观测拒绝”不等同于“单次读取拒绝”。CHARACTERISTIC 在 DAQ 拒绝而在 POLL 仍沿用原资格，前后一致。固定事件号的重复去重与现有启动逻辑一致。无占位符、无新写入路径、无对第三方源码或公共 API 的更改。
