# XCP Master DAQ 一维数组观测：实施与验证记录

状态：**首期实现完成；自动化验证通过；人工终端验收待执行。** 对应 [已批准计划](<xcp_master_DAQ一维数组观测_可行性与实施计划.md>)。本记录只描述本次连续一维数组 DAQ 任务；工作区存在其他任务遗留改动，不把它们计入本次成果。

## 1. 范围与安全边界

- 仅在 `--mode daq --daq-profile xcplite` 中开放固定长度、连续、一维 `MEASUREMENT` 数组。一个数组使用一条 Entry、一个 ODT 中的一段 DTO；标量 DAQ、POLL 和预览单次读取保持原有路径。`generic` 的数组继续在 F6 和 `watch` 预检中拒绝；标定量、多维／非连续／位字段数组、未知或超出范围的尺寸以及跨 ODT 重组不在本期范围。
- 静态阶段使用 A2L 形状、地址范围和唯一固定事件做 F6 准入，并拒绝超过核心 `uint8` 元素数／Entry 字节数可表示范围的数组。F6 仅调整观测集合，不能证明 ECU 可以采样。
- `watch` 先查询真实 Slave 的状态、DAQ Processor/Resolution/Event 信息；把 Entry 粒度及最大尺寸和协商的 MAX_DTO／AG 带入 Planner。任一数组布局、地址、尺寸或 ODT 净荷检查失败都在 `FREE_DAQ` 和危险操作确认之前拒绝。实际最大 Entry 不假定所有 Slave 都是 248 字节。
- 本次没有修改 `thirdparty/a2llib`、`thirdparty/a2l-sdk` 或 XCPlite 的文件；复用现有 A2L SDK 元素换算 API。工作区内若有这些目录的其他既存差异，属于之前的任务，不能视作本次修改。

## 2. 改动与数据流

1. [核心数据库接口](<../include/libxcp/measurement/measurement_database.hpp>)、[类型](<../include/libxcp/measurement/measurement_types.hpp>)、[样本](<../include/libxcp/measurement/measurement_sample.hpp>) 增加明确的数组形态和逐元素结果。新 `ToPhysicalElement` 为有明确“不支持”错误的非纯虚默认实现，保持既有自定义数据库的源码兼容；公开结构／虚接口变动不承诺二进制 ABI 兼容。
2. [A2L 适配器](<../adapter/a2l/a2l_measurement_database.cpp>) 校验 `MEASUREMENT` 的 1-D 连续形状、元素数、位字段和 32 位地址范围，超过 255 个元素明确失败而不截断；[事件绑定适配器](<../adapter/a2l/measurement_runtime_profile.cpp>)／相应头文件保留唯一 FIXED_EVENT 绑定，并向已有 SDK 元素换算接口转发索引。单元素数组仍明确是数组，不伪装成标量。
3. [Planner](<../src/measurement/measurement_planner.cpp>) 逐个符号验证 XCPlite 数组能力、字节总长、Entry 粒度／最大长度、AG、MAX_DTO、ODT 净荷和地址，再为每个数组规划整条 Entry／route。动态 DAQ 配置仅在完整规划及显式危险操作确认之后进行。
4. [Session](<../src/measurement/measurement_session.cpp>) 从 DTO 切出完整数组 raw，逐索引转换。只有全部元素成功才产生 `valid=true` 的整数组样本；任一元素错误仅使该数组样本无效，同一 DTO 的其他标量正常处理。短帧不会发布半个数组。
5. [Master](<../tools/xcp_master/main.cpp>) 的 F6 和 `watch` 共享静态资格策略，运行时 profile 再检查 ECU 能力；接收帧时按一次完整批次写入既有数组观测模型，展开行显示每个元素。转换失败沿用旧值标 stale 的机制，F3 Details 保持仅 A2L 元数据。[策略](<../tools/xcp_master/symbol_policy.hpp>) 区分 Generic 拒绝与 XCPlite 首期可接纳的一维数组；[README](<../tools/xcp_master/README.md>) 更新了用户操作及边界。

## 3. 新增／更新测试与实测结果

- [Planner 用例](<../tests/measurement_planner_test.cpp>)：标量／单元素数组区分，一维数组单 Entry、完整 route，Generic 与未知能力拒绝，Entry/ODT 上限拒绝。[Session 用例](<../tests/measurement_session_test.cpp>)：数组和标量共 DTO、逐索引物理换算、元素失败的整数组原子失效、缺少数组能力／超出 Entry 限制时在 `FREE_DAQ` 前零破坏性请求；旧数据库未实现元素换算时则在 DTO 解码阶段明确报告不支持，不会伪造成功数组。
- [A2L 适配器用例](<../tests/measurement_adapter_test.cpp>)：数组形状、`>255` 元素、位字段与非法布局拒绝，索引转发，运行时 Entry 粒度／最大尺寸异常拒绝；修正测试中临时数组构造 `BytesView` 的悬空引用警告。[策略用例](<../tests/xcp_master_symbol_policy_test.cpp>)：Generic 数组仍拒绝、XCPlite 合格数组接受、超过单 Entry 静态上限及非连续布局拒绝。已有 [TUI 模型测试](<../tests/xcp_master_tui_model_test.cpp>) 涵盖原子数组快照、错误旧值和展开行。
- [真实跨进程集成测试](<../tests/xcp_slave_demo_integration_test.cpp>)：独立生成 A2L，连接真正的 `xcp_slave_demo` UDP 进程，验证 `meas_fast.samples`／`meas_slow.history` 的实例事件分别继承事件 0／1；从真实 DAQ Resolution 得到 Entry 限制，规划两事件各一 ODT，数组各一 Entry 并与标量同帧；`FREE_DAQ` 后回调收到八元素数组和同帧标量，逐元素比较原始字节与物理值。该新增集成用例重复执行 **5/5** 通过。
- AppleClang Release 定向构建 `libxcp_tests`、`MeasurementAdapterTest`、`XcpMasterSymbolPolicyTest`、`XcpliteIntegration`、`xcp_master` 成功。`ctest --test-dir cmake-build-logging-release -j 4 -R 'MeasurementPlannerTest|MeasurementSession|A2lMeasurementDatabase|EventBoundMeasurementDatabase|MeasurementRuntimeProfile|XcpMaster|LatestObservationModel|PreviewRowsTest|XcpSlaveDemoIntegrationTest.GeneratedA2lFeedsLive(MeasurementSession|ContiguousArrayDaq)' --output-on-failure`：**124 项中 123 通过、1 项因 AG=BYTE 时任意字节数均合法而 Skipped、0 失败**（CTest 汇总显示 100%）。工具 `--help` 输出了 `--daq-profile generic|xcplite`。`git diff --check` 通过。唯一构建警告来自第三方 googletest 的 `char8_t` 转换及重复链接库，并非本次数组代码。

## 4. 尚未验证与人工验收

- **尚未在真实交互终端人工操作 F6／F7／确认／展开／F5**：当前工具 shell 的标准输入／输出均不是交互 TTY。自动化跨进程测试验证 Master 底层数据通路和真实 Slave DTO，但不冒充 TUI 的人工视觉验收。可用适当终端启动生成 A2L 对应的 Slave 与 `xcp_master --a2l <生成路径> --host 127.0.0.1 --port <实际端口> --mode daq --daq-profile xcplite`，聚焦 Variables 列表用 F6 加入 `meas_fast.samples`、`meas_slow.history` 和标量，F7 输入 `watch`、审阅 `FREE_DAQ` 提示后按 Y，Enter 展开并检查八个元素与快慢事件时间戳；F5 停止并复核旧快照和重启。再以 `generic` 模式确认 F6 拒绝数组，不实际向非授权 ECU 发送 DAQ 配置。
- 本次未进行 Windows／Linux 构建、全仓 clean Release 编译／全量 CTest、其他 ECU 端到端验证；定向目标构建不应被误述为完整 clean build。跨事件数组不保证同一时间基准或同周期快照；队列拥塞可能丢 DTO，未实现丢帧推断、跨 ODT 聚合、异常 ECU 的自恢复或 Seed&Key。
