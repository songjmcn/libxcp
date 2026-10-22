# 通用 XCP Master 工具：DAQ 与轮询扫描频率修改计划

> 状态：**用户已确认整体计划并批准开始实施（m01561）**；以下设计原则与限制继续有效。用户已同意保留轮询并增加独立动态 DAQ 模式，要求从 A2L 自动确定事件，并单独许可为暴露逐符号 DAQ 事件声明对 `thirdparty/a2l-sdk/` 做必要的最小修改。用户已选定只使用 `--interval-ms` 配置轮询目标周期，并采用自动化测试加可用授权 Slave 时的可选手工验证。用户不要求预先穷举所有 DTO 帧头，新增方言在遇到可复现问题后另行评估；已有安全校验仍必须保留。本文件记录获批实施范围，实施与验证结果见本文末尾记录。

## 1. 目标与不变约束

- 在现有 `tools/xcp_master/` UDP CLI 上增设 `--mode poll|daq`，默认 `poll`，不破坏当前 `--a2l/--host/--port/--module/--interval-ms` 的轮询用法。
- 轮询模式允许按命令行参数设置**目标扫描频率/周期**；DAQ 模式由 Slave 事件驱动，不把 `--interval-ms` 或扫描频率错误解释成 DAQ 采样频率。
- 选择 A2L 测量变量，由程序读取该变量的逐符号 DAQ_EVENT 声明自动判定事件；不得默认所有变量属于事件 0、不得由符号名称或相同地址推断。
- 保留 `list/info/add/remove/read/watch/stop/write/quit` 可用性与写入确认策略；明确运行时状态、异常收尾及模式下命令适用范围。
- 不复用 `examples/` 代码；产品运行时不自动连接或依赖 `cpp_demo`；不扩展到 CAN/TCP、静态 PREDEFINED 列表、数组/结构体、Seed&Key 或未验证的 DTO 方言。**已确认同时纳入独立、经明确识别和验证的 XCPlite DAQ 兼容分支**，但不把该分支当作所有 Slave 的通用协议。可把 cpp_demo 仅作为可选手工验证对象。

## 2. 可行性证据与风险

| 事实 | 位置与影响 |
| --- | --- |
| 现有工具以 `Options::interval{500}` 配置轮询；`StartWatch` 每轮即时读选中变量并按周期起点等待，耗时过长时有效频率会下降 | `tools/xcp_master/main.cpp:46-56,427-465`；默认约 2 Hz 是**目标轮次**，不是每个变量实时保障 |
| `MeasurementSession` 要先于 `XcpMaster` 构造，再把 `&session` 作为监听器传给 Master 并 `Bind`；当前工具直接构造 Master | `include/libxcp/measurement/measurement_session.hpp:5-15,57-115`、`tools/xcp_master/main.cpp:532-541`；需调整对象创建顺序和生命周期 |
| 会话已有 `Add/Prepare/Start/Stop/Statistics`、DTO 有界队列和物理值回调；Prepare 用动态 DAQ 配置，Start 用同步启动 | `include/libxcp/measurement/measurement_session.hpp:90-150,186-200`、`src/measurement/measurement_session.cpp:168-269`；有可用组件，但并非对所有 Slave 即插即用 |
| A2L 测量适配器目前把 `event_channel` 设为 0，因为公开 `SymbolInfo` 缺逐符号事件 | `adapter/a2l/a2l_measurement_database.cpp:90-107`、`thirdparty/a2l-sdk/a2lbridge/include/libxcp/a2l/a2l_types.hpp:166-193`；零也可能是合法事件，不能用作“缺失”标志 |
| 当前样本 A2L 在各 MEASUREMENT 的 `IF_DATA XCP / DAQ_EVENT / FIXED_EVENT_LIST` 中显式声明事件 0/1/2；底层解析器已有 `GetDaqEvent()->GetFixedEventList()/GetAvailableEventList()/GetDefaultEventList()` 等 | `cmake-build-debug/tools/xcp_master/cpp_demo_V201.a2l:94-103`、`thirdparty/a2llib/include/a2l/xcp/commonparameters.h:80-89`、`thirdparty/a2llib/include/a2l/xcp/daqevent.h:13-40`；可以忠实抽取，但 SDK `BuildMeasurementSymbol` 尚未导出此事实 |
| `BuildXcpliteMeasurementRuntimeProfile` 只认 XCPlite 的识别字段、固定 32-bit 时间戳和专有时间单位映射 | `adapter/a2l/measurement_runtime_profile.hpp:51-65`、`adapter/a2l/measurement_runtime_profile.cpp:63-153`；通用工具不可直接套用 |
| 动态配置会 `FREE_DAQ` 重建整表；现有规划与动态配置容量预检只按最少 1 字节 DTO 识别字段预留，未必匹配任意 Slave 实际识别字段头宽 | `src/xcp_master.cpp:453-516`、`src/measurement/measurement_planner.cpp:88-112`；必须先验证目标 DTO 布局/容量并保守限制首版范围 |

**结论：** 可以实施受限、可取证、失败即拒绝的动态 DAQ 模式；不能承诺任意 XCP Slave 均支持。首版在无法唯一绑定事件、缺少必要查询、DTO 头布局不受支持或资源不足时，明确提示并拒绝 DAQ；用户仍可显式改用轮询。特别注意，A2L 存在事件声明不等于 Slave 必然支持动态 DAQ。

## 3. 交互及轮询扫描周期（分项决策和整体实施均已批准）

- 建议 `--mode poll|daq`，默认 `poll`；`poll` 时 `watch` 是主动轮询，`daq` 时 `watch` 是配置并启动 DAQ；`stop` 停止本模式的观测。`--mode daq` 不允许把 `--interval-ms` 用于事件周期控制。
- **已确认 XCPlite 分支显式选择：**建议新增 `--daq-profile generic|xcplite`，DAQ 模式默认 `generic`；仅用户明确指定 `xcplite` 时才尝试专属分支，同时继续校验 A2L EVENT 与 Slave 能力/时间戳事实，不以参数代替取证。轮询模式若显式指定 `--daq-profile` 则报告参数冲突；不自动识别厂商或静默回退到另一分支。
- **已确认只用 `--interval-ms <正整数>`：**沿用现有默认 500 ms（目标轮次频率约 2 Hz），不新增 `--scan-hz`。帮助文本说明目标频率约为 `1000 / interval_ms` Hz；轮询请求与处理耗时过长时实际频率会降低，不保证硬实时。该参数仅在 `poll` 生效；DAQ 模式显式传入时应报告参数冲突，不能影响 Slave 事件周期。
- `list/info` 可展示符号的逐符号事件声明、自动选择结果和 DAQ 资格；`add` 不接受手工事件号。**已确认事件规则：**仅当 `MEASUREMENT` 的 `FIXED_EVENT_LIST` 去重后恰有一个事件号（0 也合法）、且符号布局符合保守标量策略时，自动选该事件；只有 `DEFAULT_EVENT_LIST` 或 `AVAILABLE_EVENT_LIST`、fixed 缺失或 fixed 包含多个不同事件时拒绝 DAQ，不任取其一。现有轮询支持的 CHARACTERISTIC 不自动纳入 DAQ。
- **已确认 DAQ 命令边界：**DAQ 运行期间拒绝 `read`、`write`、`add/remove` 与任何 DAQ 重配，提示先执行 `stop`；DAQ 停止后可正常手动 `read`，`write` 仍按原有确认、写入和读回流程。调整选量后须重新 `watch` 准备 DAQ。轮询模式保留现有行为。
- DAQ 输出标明样本有效性、时间戳来源/可信度及 `dto_received/dto_dropped/decode_errors`；时间戳换算证据不足时只给原始值或本机接收时间，不把原始计数误标为秒。

## 4. 实施阶段（用户已于 m01561 授权）

### 4.1 A2L 逐符号事件透传

1. 仅在获许可的 `thirdparty/a2l-sdk/` 范围修改 `SymbolDto` 的 MEASUREMENT 事件载体及 `BuildMeasurementSymbol`：读取原始 `IF_DATA XCP` 的 fixed/available/default 候选，不合并不同语义；关注 XCP 与 XCPplus 来源优先级及模块隔离，不支持时明确标识。底层 `thirdparty/a2llib/` 不改动。该更改会影响 SDK/桥接层数据结构及运行 DLL，须同步重建、测试相关目标。
2. 桥接层把 fixed/default/available 事件声明分别透传到 `SymbolInfo`，不得将缺失转换成 0；核对 `EventBoundMeasurementDatabase` 对缺失、多候选和 event 0 的已有处理。仅选去重后唯一的 fixed 事件；仅有 default/available、fixed 多事件、与 A2L 事件目录矛盾或超范围则拒绝；不得默认取第一个。
3. 工具从已加载 A2L 的同一 module 读取 EVENT 元数据（`XcpInfo()->event_channels`）并建立规范 `module::name → 唯一 event_channel` 绑定；若 A2L 缺少证明链，输出可诊断失败。无需人工输入通道号。

### 4.2 CLI 参数与模式状态

1. 扩展参数解析和帮助：默认 poll；保留并解释 `--interval-ms`，检查 DAQ 模式下显式传入的参数冲突；新增 `--daq-profile generic|xcplite`（仅 DAQ 有效、默认 generic），从命令行显式区分采样模式与 XCPlite 兼容分支。
2. 分离选择集与活动态：`watch` 启动本模式、`stop` 停止，重复启动/停止可诊断；暂停后改变选择集需重新准备 DAQ。输出明确目标扫描频率与实际达成频率的区别。
3. DAQ 正在运行时本地拒绝 `read/write/add/remove` 与重配并提示 `stop`；停止后恢复手动读写，其中写入仍须确认和回读；保留断线时“不确定”报告原则。

### 4.3 运行时取证、配置与 DTO 消费

1. 在配置前查询 CONNECT 资源位、`QueryDaqProcessorInfo`、`QueryDaqResolutionInfo` 和所选 `QueryDaqEventInfo`；必要时检查 A2L EVENT 周期/单位原码与 Slave 返回值一致。严格校验动态配置、资源和事件容量；Optional 命令未知时不猜。
2. **已确认 DTO 分支范围及选择：**默认 `generic` 仅启用经运行时取证验证的通用 DTO 布局；用户显式 `--daq-profile xcplite` 才尝试独立的 XCPlite 兼容分支，不自动识别厂商或静默换分支。通用分支不得套用 XCPlite 的 `0xAA` 头部、首 ODT 时间戳策略或其时间单位表；XCPlite 分支必须同时核对 A2L EVENT 和 Slave processor/resolution/event 事实，参数本身不能代替运行时校验，不满足专属 profile 即拒绝 DAQ。容量预算用分支真实头宽；多 ODT/时间戳布局若无法证明安全则限制为每事件单 ODT。**不预先穷举所有 DTO 帧头/时间戳组合**：先实现并测试实际可核验的通用布局和已取证 XCPlite 分支；遇到新方言或无法确认的组合明确拒绝 DAQ，记录诊断，待具体问题出现后再扩展，不允许带着猜测输出数值。
3. 按正确生命周期建立 `A2lMeasurementDatabase`、显式事件绑定视图、`MeasurementSession`、`XcpMaster`，复用 `MeasurementSession::Prepare/Start/Stop` 与回调。所有 Master 操作串行；回调只做轻量、线程安全的结果提交，输出线程负责打印，避免阻塞 DTO 接收/解码。
4. 失败及退出执行 stop/join、尽力停止 Slave DAQ 并报告清理异常。**已确认重建流程：**每次 `watch` 若将执行 `FREE_DAQ`，须先完成本地检查和能力查询，然后明确提示它会清除 Slave 的整张动态 DAQ 表，要求当次输入 `y`；拒绝或输入结束则不发送重建命令。停止后再次需要重建时重新确认；`stop` 不承诺恢复被清除的旧表。

### 4.4 测试、验收、文档

- `tests/` 覆盖：事件 0、fixed/available/default 的处理、无声明/歧义/跨 module/越界、参数互斥/默认兼容、DAQ 资格过滤、运行时能力和 DTO 方言拒绝、正确布局收样/换算、时间戳可信度、启动失败及停止清理；更新既有轮询/写入回归测试。
- **已确认测试方案：**Release 构建、聚焦自动化测试与全量 CTest；如有可用且获授权的 Slave，再做可选的手工采样/停止回归。`cpp_demo` 只能作为可选验证对象，不构成产品运行依赖；无设备时明确标注手工验证未执行。
- 更新 `tools/xcp_master/README.md` 与本次 `code-plan/` 实施记录，写明实际验证过的布局及拒绝条件（不要求穷举所有方言）、频率为目标值而非实时保证、真实设备测试是否执行。禁止把自动化测试通过描述为“任意 Slave 已验证”。

## 5. 决策状态与待逐项确认项

- **已确认：**只保留 `--interval-ms`（不新增 Hz 参数）；默认 500 ms；自动化测试 + 有可用授权 Slave 时的可选手工验证；DAQ 自动事件仅选唯一的 `FIXED_EVENT_LIST`，其他事件候选不猜选；DAQ 运行期间拒绝手动 `read/write` 和选量变更，停止后恢复读写；每次执行整表 `FREE_DAQ` 前经检查、提示后取得当次确认，拒绝则不重建，停止不承诺恢复旧表；DTO 分通用布局与独立的 XCPlite 兼容分支，后者由显式参数 `--daq-profile xcplite` 选用（默认 generic，仍须运行时验证）。
- **范围决定：**不预先穷举 DTO 帧头/时间戳支持矩阵；先覆盖现有可取证且经过测试的通用布局与显式 XCPlite 分支，对未知组合拒绝 DAQ 并记录诊断，等真实问题出现再扩展。不能因为推迟完整矩阵就跳过运行时验证或错误帧拒绝。
- **实施状态：**用户已于 m01561 明确批准整体计划并要求开始实施；代码变更与验证状态持续更新于本文及独立实施记录。
