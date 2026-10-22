# XCP Master DAQ 一维数组观测：可行性与实施计划

状态：**已获批准；首期实现和自动化验证完成，人工终端验收待执行。** 用户确认首期单 Entry／单 ODT、核心兼容性扩展、仅显式 XCPlite profile，以及完整自动化／真实 Slave／人工终端测试。本文 §1、§5 中「当前／尚未」叙述保留为立项时的诊断，不是当前代码状态；实施结果、测试和未覆盖项单独见 [实施记录](<xcp_master_DAQ一维数组观测_实施记录.md>)。

## 1. 立项时结论与已核实的事实（“当前”均指立项时）

- 首期支持 `xcp_slave_demo::meas_fast.samples`（`FLOAT32_IEEE[8]`，事件 0）和 `xcp_slave_demo::meas_slow.history`（`uint32_t[8]`，事件 1）**可行**。生成的 A2L 声明对应的一维 `MATRIX_DIM 8` 和所属 INSTANCE 固定事件；示例先更新快／慢变量再触发对应事件（`cmake-build-release/tools/xcp_master/xcp_slave_demo.a2l:77-113`、`examples/xcp_slave_demo/main.cpp:520-544`）。实际进程必须使用与其内存布局、端口相符的 A2L，不能仅凭文件名断言匹配。
- XCPlite 源码中的单条 ODT Entry 上限为 248 字节（`thirdparty/XCPlite/src/xcplite.c:121-129,1260-1263`），上述两个 8 × 4 = 32 字节的连续数组可以各放入一条 Entry；首 ODT 的标识头和时间戳仍须计入实际 DTO 容量。`src/measurement/measurement_planner.cpp:33-127,195-218` 已按元素宽度 × 数量装入一条 Entry、一段同 ODT route，超过单 ODT 净荷则拒绝，**不支持把单个变量自动拆到多个 ODT**。
- 当前瓶颈不是事件元数据：`src/measurement/measurement_session.cpp:438-495` 把数组 route 的整段 raw 送进标量 `ToPhysical(name, raw)`；`thirdparty/a2l-sdk/a2lbridge/src/a2l_database_impl.cpp:381-396` 明确拒绝非空 dimensions，因此直接放开 F6 只会得到 invalid sample 和解码错误。已有 `IA2lDatabase::ToPhysicalElement(...)`（`thirdparty/a2l-sdk/a2lbridge/src/a2l_database_impl.cpp:399-443`）能按元素索引换算，但当前核心窄接口和事件包装层均未暴露它。`tools/xcp_master/main.cpp:2919-2945` 只按标量发布 DAQ 值；`tools/xcp_master/main.cpp:2594-2612` 的原子数组快照入口目前用于 POLL／单次读取。
- `adapter/a2l/a2l_measurement_database.cpp:67-80` 对超过 255 元素的数量作静默截断；**实施数组 DAQ 前必须改为明确拒绝，不得向 ECU 配置错误长度**。当前 `MeasurementSymbolInfo::element_count` 为 `uint8_t`，单元素数组与标量不能只凭数量区分。

## 2. 首期目标与明确非目标

1. 仅 `--mode daq --daq-profile xcplite` 开放经验证的 **MEASUREMENT、固定长度、连续、一维数组**；每个数组完整占用一条 DAQ Entry 和一个 ODT 的同一 DTO。使用已继承的唯一 `FIXED_EVENT_LIST` 和匹配 EVENT_CHANNEL；合法事件号 0 不视为缺失。保持原有标量、POLL 和单次读取功能。
2. `generic` 继续支持原有标量，但首期对数组显示“该 profile 尚未验证数组 DAQ”，不因选中 A2L 自动切换 profile；`CHARACTERISTIC`、多维／非连续／位字段／未知宽度数组仍不允许进入 DAQ。
3. 不修改 XCPlite Slave、XCP 协议命令、`thirdparty/a2llib` 或 `thirdparty/a2l-sdk`：首期复用 SDK 已存在的元素换算接口。若核查发现现有接口不足，应返回设计评审而不是擅自扩大第三方修改范围。
4. **不支持跨 ODT／跨 DTO 拆分单个数组**，也不宣称跨不同 DTO 的样本属于同一周期。大数组需要周期关联、缺帧／乱序处理及有限缓冲，另立里程碑；POLL 可读大数组不代表 DAQ 同样可以。

## 3. 架构与兼容性方案

- 在 `include/libxcp/measurement/measurement_database.hpp` 的核心自有符号信息中显式表达“数组”以及经过验证的元素数、宽度；**一元素数组仍是数组**。核心不 include A2L／第三方类型。`A2lMeasurementDatabase::Find(...)` 对新增的数组路径验证 MEASUREMENT 类型、单维 `extent > 0`、`byte_stride == element_size_bytes`、32 位地址和总字节数乘法边界及无位字段，不改变既有标量准入；AG 粒度依赖连接协商，交由 Planner／`watch` 的运行期检查。超过可表示范围（包括 `element_count > 255`）明确失败，禁止截断。`EventBoundMeasurementDatabase::Find(...)` 保留数组形态并严格绑定唯一事件。
- 为核心 `IMeasurementDatabase` 增设按索引的元素换算入口，**采用有明确“不支持”错误的默认实现而非新增纯虚要求**；`A2lMeasurementDatabase` 委托现有 `IA2lDatabase::ToPhysicalElement(...)`，`EventBoundMeasurementDatabase` 必须转发。这样旧的标量 FakeDb 与自定义数据库可保持源码可编译，绝不把数组索引 0 偷换成标量 `ToPhysical(...)`。新字段／虚接口需要依赖方重新构建，**不承诺新旧二进制 ABI 混用**；实现前应列出并重建全部影响目标。
- 维持现有标量 `MeasurementValue` 和标量样本读取方式，不用 `name[0]` 冒充新的 A2L 符号；为 `MeasurementSample` 增加明确的数组形态及按索引排列的 `vector<MeasurementValue>`（或功能等价的有类型载体），raw 仍保留整段。数组样本在同一 DTO 中**全元素成功才 `valid=true`**；任一元素转换失败只将该数组样本判无效，同帧其他独立标量样本维持既有处理。不把错误样本送给 UI 当有效值。`MeasurementRoute`／切片保留整体 name 和准确尺寸，避免一数组误拆为多个 UI 符号。
- `IDaqLayout::Decode(...)` 并非可直接替代的数组物理换算器：`thirdparty/a2l-sdk/a2lbridge/src/daq_layout_impl.cpp:221-239` 当前只给单元素无维度 Entry 做物理换算；首期按核心现有动态 DAQ 路径扩展，不修改静态布局工具。

## 4. 数据流与操作语义

1. A2L 加载／实例事件继承 → 搜索结果的 SymbolInfo → F6 静态准入（含显式 XCPlite 模式、布局、唯一固定事件）；显示被拒绝的具体原因。F6 只改变观测集合，**不启动 DAQ**。
2. F7 输入 `watch` → 运行期 `GET_STATUS`、`GET_DAQ_PROCESSOR_INFO`、`GET_DAQ_RESOLUTION_INFO`、`GET_DAQ_EVENT_INFO` → XCPlite profile 中保留 Slave 返回的 Entry 粒度和最大 Entry 字节数 → 检查每个数组的总字节、地址扩展、ODT 净荷与时间戳／识别头、单 Entry 最大长度、32 位边界和规划结果。允许拒绝而不发送任何破坏性配置。所有检查通过后才展示现有默认取消的 `FREE_DAQ` 确认；明确确认才开始配置与采集。
3. 规划整数组一条 Entry → Slave 在事件上发送 DTO → `MeasurementSession` 根据 `(daq_list, odt, payload_offset, size)` 切完整数组 raw，逐元素用本数组规范名＋元素索引换算 → 同一帧内生成**一个完整数组样本** → Master 的帧队列按名称一次调用 `UpdateArrayValue` 并刷新展开行。标量继续走原有 `ToPhysical(...)`／`UpdateValue(...)`；F3 Details 始终只读元数据。
4. 对数组所在 DTO 做严格的有效载荷范围核查，短帧、缺切片、长度或元素换算错误均不得发表半个数组；在观测集合仍有效时保留最近成功快照并标旧值／错误。未收到 DTO 并不自动证明发生丢帧：首期不承诺帧序号、跨周期重组或每周期超时判定。Stop 时按既有逻辑阻止停止后的在途样本复活，并保留快照的停止标志；需纳入回归测试。

## 5. 立项时风险与保护条件

- `MAX_DTO` 净荷容量不等于 XCPlite 运行时 `MAX_SIZE_DAQ` 单 Entry 容量：当前 `XcpliteMeasurementRuntimeProfile` 尚未携带后者；`tools/xcp_master/main.cpp:2770-2828` 对 XCPlite 也尚未启用 `max_entry` 检查。把运行期报告的 `granularity_daq`／`max_odt_entry_size_daq` 明确带入预检，32 字节示例只是可行性证据，**不得将 248 硬编码为所有 ECU 常量**。所有观测列表成员加总后的 ODT 也不得超过已验证净荷。
- 地址扩展必须沿用真实 A2L／连接协商值，防整数乘法、起址＋长度、宽度和 AG 换算溢出；不从名称推断 `meas_fast` 的地址／事件；不把多于 255 个元素改写为 255 个。配置前能力检查不通过仍显示“已停止”，并在结果／日志面板给出准确失败原因。
- 接口变动可能触及测量模块公开头文件、已有适配器、事件包装层、FakeDb、测试与下游 ABI。实施时先盘点所有派生类及初始化方式并选择最小兼容扩展；保留标量转换路径，编译所有关联目标。首期不放宽 `generic` 和标定安全策略。
- 独立数组样本只保证**本次 DTO 内的数据完整性**；Slave 内存与快事件的时间一致性由 Slave 更新／事件触发顺序决定，工具不承诺多事件同步快照。队列溢出可能整体丢帧；不可谎称所有周期都有样本。

## 6. 测试与验收（已确认方案；执行结果见实施记录）

1. **单元／模型：**补 `tests/measurement_planner_test.cpp`、`tests/measurement_adapter_test.cpp`、`tests/measurement_session_test.cpp`、`tests/xcp_master_symbol_policy_test.cpp` 和 `tests/xcp_master_tui_model_test.cpp`。校验标量与单元素数组区分、事件 0/1、一维 `float[8]`／`uint32[8]` 单 Entry／单 ODT、同 ODT 混合标量、索引与换算精度、数组一次发布完整 8 值。缺少元素换算支持的旧 FakeDb 应明确拒绝而不伪成功。
2. **边界与安全：**非连续、多维、零长度、位字段、地址／乘法溢出、超过 255 个元素及运行期 Entry／ODT 限制必须在 `FREE_DAQ` 前拒绝；短帧／中途一个元素失败不能产生半个新快照；同帧其他标量不应因数组失败被覆盖；stop／重启不把旧帧误记为新采集。测试 `generic` 对任意数组的明确拒绝、`POLL` 行为不回退。
3. **真实 Slave 集成：**沿用 `tests/xcp_slave_demo_integration_test.cpp` 的真实生成 A2L 与跨进程 UDP fixture，显式 XCPlite，确认快 `samples[8]`、慢 `history[8]` 与至少一个标量都有合法事件、DTO 实际字节和经过元素换算的整数组值；确认 `FREE_DAQ` 后实际采集成功，而非仅证明 Planner 可计算。测试目标运行于 Release；相关聚焦 CTest 和静态格式／diff 检查均应记录数目与结果，不预填“通过”。
4. **真实终端人工验收：**Master 启动 XCPlite → F6 加入两个数组与标量 → F7 `watch` → 按确认流程开始 → 展开并观察数组各元素、时间戳和快慢更新 → Stop／重启；同时检查 `generic` 拒绝和配置前容量失败。当前工具运行环境此前不能分配 PTY；若仍无法完成，报告明确标为**未验证**，不以测试模拟界面操作冒充人工终端验收。Windows/Linux、全仓 clean Release 是否执行也须在实施记录如实写明。

## 7. 设计自查与已批准边界

- 设计时文档区分已证实的“32 字节在示例 248 字节单 Entry 上限内”与当时尚未实现的“可正常显示数组 DAQ”；静态 F6 接纳不能冒充 ECU 实际启动采样，实施记录须分别提供证据。
- 首期只一个 DTO 形成一个数组样本，不含跨 ODT 重组、丢帧推断或假定有统一多事件周期；数组发布遵守全元素成功的原子规则。
- 改动核心公开接口不等于保证 ABI 兼容；默认“不支持”只减轻旧数据库实现的**源码**兼容风险。除本阶段已确认的范围外，不隐含修改第三方 SDK／XCPlite、协议命令或 generic 数组支持。
- 已确认测试方案包含单元、真实跨进程和人工终端；用户已批准开始实施。若单 Entry／单 ODT 前提被真实 Slave 推翻，必须重新设计评审，不静默放宽限制；人工终端验收若不可执行，应在实施记录中如实列出。
