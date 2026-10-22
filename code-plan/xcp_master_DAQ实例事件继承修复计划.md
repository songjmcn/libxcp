# XCP Master DAQ 实例事件继承修复计划

状态：已按确认的架构、拒绝规则和显式 profile 方案实施；真实生成 A2L 的正反例、事件绑定→双事件规划→实际 Slave 的 MeasurementSession 解码及 35 项相关 Release 回归测试均通过。当前沙箱无法分配 PTY，真实 Master TUI 的交互式终端联调尚未完成。

## 1. 背景与证据

`xcp_slave_demo.a2l` 的 `meas_fast` 与 `meas_slow` INSTANCE 分别声明 `FIXED_EVENT_LIST` 的事件 0、1，对应 A2L DAQ EVENT 0、1；子测量量由 TYPEDEF_STRUCTURE 展开。示例 `slave_registry.cpp` 注册并触发两个事件，现有 `XcpSlaveDemoIntegrationTest.CrossProcessCalibrationChangesDaqOutput` 可实际收到快速、慢速 DTO（本次诊断中在 `cmake-build-logging-release` 运行 1/1 通过）。用户截图中的 DAQ/generic、采样集合 0 及所有标量测量叶子提示缺少固定事件，说明启动 DAQ 之前静态准入已经失败；不能以此推断 Slave 不支持 DAQ。

`thirdparty/a2l-sdk/src/liba2l_export.cpp` 中 `BuildInstanceInfo` 没有将 INSTANCE IF_DATA 事件写入 `StructInfoDto`；`thirdparty/a2l-sdk/a2lbridge/src/structure_layout.cpp` 的 `ExpandStruct` 生成 `SymbolInfo` 叶子时自然缺少事件。`tools/xcp_master/symbol_policy.hpp` 中新加的 DAQ 静态准入因此一致但对该真实语料误拒绝。另一个独立条件：XCPlite 使用相对标识字段与固定时间戳，Master 默认 generic 只接受 Absolute PID/非固定时间戳。

## 2. 架构与改动边界（已确认）

允许**限定**修改仓库内 `thirdparty/a2l-sdk` 的 DTO 和桥接实现，以保留 INSTANCE 的 XCP DAQ 固定事件、让测量叶子继承可证明的关联。不修改 `thirdparty/a2llib`、XCP SDK 的 DAQ 协议实现、主程序快捷键及已完成标定读写。直接 MEASUREMENT 及现有固定事件策略保留；标定叶子和数组不会因继承事件而变成 DAQ 候选。

把源事实存入 `StructInfoDto` 的固定事件列表，复用普通 MEASUREMENT 导出中对有效 XCPplus/plain XCP IF_DATA 的选取规则；无法解析的事件不推断，保持空列表并让策略拒绝。实施试验发现上游 `thirdparty/a2llib/src/a2lparser.y:1330-1353` 不允许 TYPEDEF_MEASUREMENT 携带 IF_DATA；构造该语料的加载测试得到 `parse failed at line 1477: Parser error: syntax error`，故不添加无法由 A2L 表达的 typedef 事件 DTO，也不修改本轮禁止修改的 a2llib。冲突测试改用 INSTANCE 自身列出两个不同固定事件。该项目内 SDK 数据结构变更需重编译依赖它的目标；不宣称可与旧二进制 ABI 混用。

## 3. 数据流及 UI（已确认）

A2L INSTANCE IF_DATA → 实例 DTO 固定事件 → `BuildStructureLeaves` 为同一 MODULE/INSTANCE 路径的 MEASUREMENT 标量叶子赋予事件 → `IA2lDatabase` 的 `SymbolInfo.daq_event_fixed` → Master 预览提示、F6 加入与 `watch` 启动前共用的 `SymbolDaqObservationUnsupportedReason`。嵌套结构与结构实例数组均随所属 INSTANCE 传递，不按变量名前缀、事件名称或 ECU 品牌猜测。INSTANCE 自身列出多个不同固定事件时须保留完整列表供策略拒绝，不能任选一个。普通直接声明 IF_DATA 的 MEASUREMENT 维持原行为。复核主程序调用链发现既有 `EventBoundMeasurementDatabase` 会在适配器的默认值之后执行独立事件绑定（事件缺失/歧义拒绝，事件 0 有效）；无需修改 `A2lMeasurementDatabase`，但应以真实 A2L 测试这个包装层和 `MeasurementPlanner` 的事件分组。

启动命令保持 `--mode daq --daq-profile xcplite` **显式**选择 XCPlite；`--daq-profile` 默认仍为 generic，不根据 A2L 名或 Slave 名自动切换。可在 generic 运行期拒绝时增加条件式诊断提示“若已确认是 XCPlite，请重启并显式使用 --daq-profile xcplite”，但不得自动发送配置或重试。

## 4. 拒绝、安全和错误处理（已确认）

仅 MEASUREMENT 标量、且去重后恰好一个 `FIXED_EVENT_LIST` 事件并有对应 A2L EVENT_CHANNEL，才可加入 DAQ 观测；缺失、多个不同固定事件、不同固定事件并存或通道缺失均拒绝并给出原因。禁止用 DEFAULT/AVAILABLE 替代、禁止由名称猜测，数组的 ODT/DTO 映射未经验证仍拒绝。即使静态通过，`watch` 仍执行现有运行时 GET_DAQ_*、布局/能力校验和默认取消的 FREE_DAQ 确认；失败必须保持未采样，不能误报已启动。`generic` 不匹配时提示仍保持 fail-closed。

## 5. 测试与验收（已确认）

1. 用真实生成的 `xcp_slave_demo.a2l` 回归 SDK 导出和桥接：`meas_fast.input` 事件 0、`meas_slow.basic.real_32` 事件 1、嵌套标量继承、`cal.*` 不继承；检查模块/实例隔离及事件 0 不当作缺省空值。对真实生成的 A2L 做最小变体，覆盖仅有 DEFAULT 事件、重复相同固定事件和多个不同固定事件；无有效事件时维持拒绝。
2. Master 策略：上述合法标量能加入 F6 观测；标定、数组、事件不明及 EVENT_CHANNEL 缺失仍拒绝；普通直接 MEASUREMENT 事件不退化。测量的一次性读取和 POLL 行为不因 DAQ 限制改变。
3. AppleClang Release 聚焦构建与相关 CTest、clang-format、diff 检查；现有独立 Slave 跨进程 DAQ 测试作为 Slave 能力证据。若允许真实终端/本机 Slave 联调，以 `--mode daq --daq-profile xcplite` 加入标量、执行 `watch`、确认 FREE_DAQ、检查顶部状态与 DTO 数值；不能运行时记录限制，不以单独 Slave 测试冒充 Master 端到端验证。更新工具 README 并新增本轮实施记录。

## 6. 设计自查

- 无未决占位符；`--mode` 拼写已纠正，不将用户误写的 `--model` 视作可接受参数。
- 元数据静态准入和运行期 DAQ profile 是两个独立门槛，继承事件并不承诺采样成功。
- EVENT_CHANNEL 存在与否继续由既有策略检查；无证据或冲突始终拒绝，绝不吞并到某个事件号。
- SDK 修改范围由用户明确允许，`a2llib` 和 XCP 协议实现仍禁止改动；旧进程必须重启，不支持新旧 DTO ABI 混用。
- 真实 Master/Slave 终端联调与 Windows/Linux 构建若未执行，实施记录必须显式列为未验证。
