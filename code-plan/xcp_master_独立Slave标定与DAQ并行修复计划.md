# XCP Master 对独立 Slave 的标定及运行中 DAQ 标定修复计划

## 1. 状态、目标与约束

- 日期：2026-10-21。
- 状态：根因分析、适用范围、架构、交互/数据流、安全边界、测试方案已分别与用户确认；**本轮只提交计划文档，不实施代码修改，也不声称修复已经通过测试**。实施须另获明确指令。
- 问题：`xcp_slave_demo` 的 `cal.*` 在 Master 的 POLL/DAQ 使用方式下不能标定；用户明确要求 DAQ 持续采样时也能标定，并要求**所有 DAQ profile 都允许通过同一门禁尝试标定**。
- “允许尝试”不等于保证任意 Slave 可在线标定：不支持页查询、活动页不可写或其他证据不足时必须安全拒绝；本仓库仅能对真实 `xcp_slave_demo` 作端到端验证，不能据此声称所有 Generic 设备已经验证。
- 保留项目现有 C++20、Windows/Linux/macOS、中文 Doxygen 与 `tests/` 回归测试要求；不修改 `thirdparty/XCPlite`、`thirdparty/a2llib`、`thirdparty/a2l-sdk`，不放宽核心 SDK 或所有 A2L 的写入权限解释。

## 2. 已核对的根因和区别

1. **生成的 A2L 缺少写权限。** [`thirdparty/XCPlite/src/a2l.c:1092-1116`](<../thirdparty/XCPlite/src/a2l.c#L1092-L1116>) 只对 ABS/APP/DYN 寻址的 `INSTANCE` 输出 `READ_WRITE`；独立 Slave 的标定对象是段相对寻址，[`examples/xcp_slave_demo/slave_registry.cpp:148-151`](<../examples/xcp_slave_demo/slave_registry.cpp#L148-L151>) 创建 `cal`，生成文件的 `cal` 实例没有该标记（[`cmake-build-release/examples/xcp_slave_demo/a2l/xcp_slave_demo.a2l:141`](<../cmake-build-release/examples/xcp_slave_demo/a2l/xcp_slave_demo.a2l#L141>)）。[`thirdparty/a2l-sdk/a2lbridge/src/structure_layout.cpp:281-289`](<../thirdparty/a2l-sdk/a2lbridge/src/structure_layout.cpp#L281-L289>) 把实例 `read_write` 原样继承给 `cal.*` 叶子；Master 在 [`tools/xcp_master/symbol_policy.hpp:124-149`](<../tools/xcp_master/symbol_policy.hpp#L124-L149>) 的写入资格检查阶段即拒绝。这发生在实际页检查和内存写入之前，是 POLL 和 DAQ 均存在的共因。
2. **DAQ 运行中另有有意门禁。** 标定表单按钮 [`tools/xcp_master/main.cpp:417-439`](<../tools/xcp_master/main.cpp#L417-L439>)、标定读取 [`tools/xcp_master/main.cpp:2003-2016`](<../tools/xcp_master/main.cpp#L2003-L2016>)、写入入口及确认后的最终检查 [`tools/xcp_master/main.cpp:2333-2349`](<../tools/xcp_master/main.cpp#L2333-L2349>)、[`tools/xcp_master/main.cpp:2481-2508`](<../tools/xcp_master/main.cpp#L2481-L2508>) 都拒绝活动 DAQ。只修正 A2L 无法满足运行中标定需求。
3. **不能据现有直接写测试认定工具已可标定。** [`tests/xcp_slave_demo_integration_test.cpp:1033-1154`](<../tests/xcp_slave_demo_integration_test.cpp#L1033-L1154>) 用手算段偏移调用 `WriteMemoryBytes`，证明底层写入和后续 DAQ 可用，却没有经过桥接层叶子权限与 Master 标定表单。当前文件第 1149 行是在这些写入**之后**才启动 DAQ，不能证明运行中写入。
4. **现有页面与命令基础可以复用，但还需实测。** 生成的标定段编号为 1，工作页 0 可写而参考页 1 不可写（[`cmake-build-release/examples/xcp_slave_demo/a2l/xcp_slave_demo.a2l:202-208`](<../cmake-build-release/examples/xcp_slave_demo/a2l/xcp_slave_demo.a2l#L202-L208>)）；Slave 的实时 `GET_PAGE_INFO` 也按页返回相应权限（[`thirdparty/XCPlite/src/cal.c:953-981`](<../thirdparty/XCPlite/src/cal.c#L953-L981>)）。当前配置没有启用“启动于参考页”选项（[`thirdparty/XCPlite/src/xcplib_cfg.h:110-113`](<../thirdparty/XCPlite/src/xcplib_cfg.h#L110-L113>)），按 [`thirdparty/XCPlite/src/cal.c:618-625`](<../thirdparty/XCPlite/src/cal.c#L618-L625>) 将从工作页启动；实施测试仍必须**在线查询**，不能以配置推定实时页。

以上为文件及源码核查，**尚未运行本问题的新修复或交互重现**；若实际错误不是“未标记为可写”或“DAQ 期间禁止”，实施前须收集 Master 标定状态/日志和活动页结果，确认是否还有第三道阻断。

## 3. 组件与实施步骤

### 3.1 项目自有 A2L 生成修正

在 [`examples/xcp_slave_demo/main.cpp:437-459`](<../examples/xcp_slave_demo/main.cpp#L437-L459>) 的 `A2lFinalize()` 成功且文件非空之后、`A2lBridge::Load` 之前，对**生成模式刚产出的文件**施加严格局部修正：定位恰好一个项目 `cal`、类型 `CalibrationParameters`、与已登记标定段基址一致的 `INSTANCE` 声明；仅为此声明补齐 `READ_WRITE`。不能按全文件字符串替换全部 `INSTANCE`，不能给 `meas_*`、参考页或其他段提升权限。

- 匹配零次/多次、字段不一致、语法不符、I/O 不完整时失败关闭；不能发布未经验证的可写文件。兼顾输出文件已带该标记时的幂等核验，避免重复关键字。
- 先写入并校验同目录临时产物，再按 Windows/Linux/macOS 均经验证的文件替换方式发布；无法安全替换则失败并执行既有输出清理，禁止在原文件上局部覆写。不改变外链 AML、XCP 地址扩展、段号、页面或 XCPlite 注册顺序。
- 重新用桥接层加载，并校验目标叶子的 `kind`/`characteristic_type`、`read_write`、32 位地址/扩展、标定段与页信息；例如 `cal.basic.real_32` 和 `cal.model.offset`，固定轴曲线如确实存在则另测，不虚构本 demo 存在的曲线。仅这些原始数据符合预期才报告生成成功。
- 不推荐在 Master 对 `cal.*` 名字做硬编码白名单，也不推荐修改第三方生成器：前者绕过可信授权，后者超出本轮约束。

### 3.2 标定页与 DAQ 并发数据流

- `POLL` 和各个 DAQ profile 均使用**同一**标定页/命令写流程；只解除标定表单目标输入、标定预读及写入路径中的活动 DAQ 一刀切拒绝。Generic 不靠 profile 名获得写入特权；缺少实时页面查询能力照常拒绝。
- 保留普通预览单次读取、命令行一般 `read`、DAQ 观测集合增删及 DAQ 配置变更的现有限制；不把 `CHARACTERISTIC` 纳入 DAQ 测量准入，也不开放整条数组的批量标定。
- 接收线程把 RES/ERR 放入命令响应槽，DTO 另行异步分派（[`src/command_executor.cpp:181-320`](<../src/command_executor.cpp#L181-L320>)）。标定请求只由工具的后台命令线程发出；采样 DTO 回调只发布数据，不同步调用 XCP 命令，也不等待标定锁。POLL 读取与写命令沿用 `command_mutex_` 序列化，不跨用户确认等待持锁。
- 标定页的“当前值”和写后读回由内存读取返回；DAQ 样本继续沿观测模型更新，不能将一个测量 DTO 冒充标定读回，也不要求二者具有同一时间戳。

### 3.3 发送前授权与失败处理

维持 [`tools/xcp_master/main.cpp:2412-2529`](<../tools/xcp_master/main.cpp#L2412-L2529>) 的完整顺序：A2L 符号类型/写权限/段及静态页核验 → 实时活动页和 `PAGE_PROPERTIES` → 预读和再次页复核 → 含变量、元素索引、当前值、目标值的明确确认 → 最终选择/页/确认票据复核 → 写入 → 同页读回。任何发送前失败都禁止发 `DOWNLOAD`/`SHORT_DOWNLOAD`；不能因 DAQ 活跃而跳过这套流程。

- `stop`、退出或选择改变在**发送前**取消许可；写命令已经发送后不能承诺撤销。UI 停止请求可能需要等待当前网络命令完成；不得从 DTO 工作线程直接调用会 join 它自己的 `MeasurementSession::Stop()`。
- 正常响应但回读失败/页改变：报告“命令已发送，实际值未知”并展示原因；读回值不等于目标时应报告实际值而非宣称目标已经生效。
- `DOWNLOAD`/`SHORT_DOWNLOAD` 响应超时为 `OperationOutcomeUnknown`，底层不会盲重放（[`src/command_executor.cpp:674-714`](<../src/command_executor.cpp#L674-L714>)）；UI 必须单独呈现“结果未知、不要直接重试、需独立读回”，不得统称“写入失败/未写入”。本期单个标量或曲线单点依然要承认协议上的发送后不确定性；整数组跨块写入不在范围内。

## 4. 自动化测试和验收

| 层级 | 验收条件 |
|---|---|
| 生成与静态策略 | 新生成 A2L 可被桥接层加载；目标标定叶子 `read_write=true` 且地址/扩展/段号/页与原布局一致；`meas_*` 不获标定写资格；缺失/重复/错误形状的 `cal` INSTANCE 生成修正失败；参考页及只读/缺页符号拒写。 |
| UI 状态与安全门禁 | POLL 与 DAQ 激活时仅标定页输入、读回、写申请可用；普通预览单读、改 DAQ 观测集合维持限制。确认取消、Stop 在发送前抢先、目标改变、页变更、预读失败、页命令不支持均验证零写调用；确认后的发送与回读结果分别显示，不把超时报作未写。 |
| 协议模拟并发 | 可控 Slave 在活动 DTO 流中交错 `GET_CAL_PAGE`、`GET_PAGE_INFO`、预读、DOWNLOAD/SHORT_DOWNLOAD 和回读的 RES/ERR；覆盖 Generic/XCPlite profile、响应超时、Stop 请求和恢复；验证持续收到 DTO、命令应答不错路、确认不重放写命令。Generic 模拟用例不是所有真实 Generic ECU 的互操作证明。 |
| 真实跨进程 | 使用重新生成的 A2L 和独立 `xcp_slave_demo` 进程，按桥接叶子地址先在 POLL/停止状态写入并回读，再在 DAQ **已经启动且持续有 DTO** 时标定 `cal.model.offset` 等标量；验证写后真实读回、后续 `meas_fast.output` 受参数影响、采样继续、活动页仍正确。另测页 1 只读、取消零写入路径；需要切页时恢复测试初始页，保证隔离。 |
| 人工交互 | 环境有合适终端时检查 F4 标定页在 POLL/DAQ 中的按钮、确认文案与错误状态；人工冒烟不代替自动测试。 |

实施后按工程要求先清理对应 Release 构建目录再做完整编译，运行相关 CTest 和真实跨进程测试；测试命令、数量、结果、未覆盖环境（含 Windows/Linux/macOS 实际可运行范围）写入新的 `code-plan` 实施记录。**当前未执行这些实施后测试，不能预填通过数量。**

## 5. 自审与范围封口

- 根因修复必须从生成文件经桥接层到 Master 准入贯通；只放开 DAQ 检查不能解决 POLL，单补 A2L 也不能解决活动 DAQ。
- A2L `READ_WRITE` 是静态候选，不是 ECU 授权；实时页与最终发送前复核依旧必须成功。
- 不将“任何 DAQ profile 可尝试”偷换为“所有真实 Slave 均已验证”；对于可选 XCP 页命令，失败时零写入。
- 未扩展 SDK/公共 API、未修改第三方或测量/DAQ 数组配置，不引入以变量名猜测事件或标定页的逻辑。
- 本文件是设计和验收约定，不是实施记录；**下一步需用户明确批准实施**，再修改代码并记录真实测试结果。
