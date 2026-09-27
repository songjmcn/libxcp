# XCPlite 协议调试后续路线 —— 批次18-21 任务拆解计划

> 前置：批次17 已完成第一/第二阶段全部可交付（376 项测试 100% 绿，见
> `XCPlite_Slave_协议调试_实施记录_批次17_对手端集成.md`）。
> 本计划将批次17 §6 遗留与计划内"属后续里程碑"项拆解为 4 个批次，
> **推荐按 18 → 19 → 20 → 21 顺序执行**（用户未指定偏好，依据见 §0）。
> 每一批次方案需经确认后才进入编码（AGENTS 硬规则）。

## 0. 排序依据

| 序 | 批次 | 理由 |
|---|---|---|
| 1 | 批次18 STRUCTLEAF | 直接补齐**原始需求第一阶段验收缺口**："A2L 能正常读取嵌套类型（结构体嵌套+数组嵌套结构体）"目前只在协议层（基址+offset）覆盖，成员级叶子路径是批次16 的欠账；且批次16 的硬门"缺真实语料"**已被批次17 的 Slave 运行时 A2L 解锁**（含标量成员/数组成员/两层嵌套/非零 INSTANCE 扩展），现在做阻力最小、价值最高 |
| 2 | 批次19 Seed&Key | 关闭原计划 Phase2-03 唯一未成项（当前只测了"实然报错"）；机制已核证可行（`XCPLITE_CFG_OVERRIDE`，不改 thirdparty） |
| 3 | 批次20 DAQ 实时采集 | 计划明确"属后续里程碑"；是最大的新增能力面（预定义列表取证 + DTO 解码 + 账本），依赖批次18 的叶子能力才能做"结构体成员进 ODT"，故排后 |
| 4 | 批次21 工程性收口 | 跨平台实跑、GET_ID 拉 A2L、磁盘治理等，可并行穿插，不单方面阻塞 |

## 1. 批次18 —— STRUCTURE/INSTANCE 叶子路径（接管并解冻批次16）

> **✅ 已完成（2026-07-29）**：18-0～18-F 全部交付，含实施中新发现的 18-A′（CalSeg
> TYPEDEF_CHARACTERISTIC 标量事实并入，见 §1 任务表下方增补行）。证据：
> `XCPlite_Slave_协议调试_实施记录_批次18_结构叶子展开.md`（F13–F17 核证、ON 377/OFF 352、
> A2lGolden 38+1SKIP、Xcplite 25/25、双 prepared root）；批次16 计划已回写关闭（B16-P3）；
> R4 设计文档新增 **R4.9** 行；B 类决策文档 B-12 行已更新。

**目标**：`MODULE::INSTANCE.member`、`...array[idx]`、`...nested.member`、
`...nested_array[idx].member` 四类叶子路径可 Find/寻址/换算/读写；
批次17 的 `XcpliteA2lReadTest.StructMemberLeafPathsPendingBatch16` 从 SKIP 转 PASS。

| 任务 | 内容 | 完成证据 |
|---|---|---|
| 18-0 语料门启用 | 将 Slave 运行时生成的 `xcp_test_slave.a2l`（含脱敏性=纯测试符号）入库为批次16 的 16-0001 真实语料：`tests/data/a2l/xcplite_structleaf_corpus.a2l`（偏移做定性说明：语料用于**结构**判定，不锁地址数值）；补一个"不可执行"变体（缺 TYPEDEF 引用/递归环）由 Slave typedef 手工样本构造 | 语料文件 + 来源说明 + 覆盖清单（对照批次16 §3 16-0001 要求逐项打勾） |
| 18-A SDK 事实导出（ABI v5→v6） | 按批次16 §3.16-A：`TypedefMeasurementDto`、`ListTypedefMeasurements`、STRUCTURE 成员字段补全（引用名/偏移/MATRIX_DIM/extension）、ABI 版本位递增、快照采集与稳定排序。**实施中修订**：`StructInfoDto` 增补 `matrix_dim`（INSTANCE 级结构体数组实例的逐元素展开依据，v6 内含；TYPEDEF_STRUCTURE 恒空） | SDK 单测：正常/空/缺失引用/ABI 不匹配 |
| 18-A′ CalSeg 标量事实导出（新增，探针驱动） | 真实语料 `kDefaultCalParams` 成员引用 `TYPEDEF_CHARACTERISTIC`（`C_cal_factor`/`C_cal_offset`），原注册表（仅 TYPEDEF_MEASUREMENT）双查不到 → 仅 2 条 InvalidLayout 告警、cal 无叶子。改动：标量类型事实注册表合并 `module.TypedefCharacteristics()`——元素类型经 **Deposit RECORD_LAYOUT FNC_VALUES 与 CHARACTERISTIC 符号同一可证口径**证明（provable→宽度/换算/单位/limits；不可证→宽度 0 由 resolver 按 B-3 拒绝）。动机：CalSeg 是 Phase2 校准写的核心通路，且 XCPlite 语料里 cal struct 成员**只有** TYPEDEF_CHARACTERISTIC 一种写法，真实对手端必须可解 | 语料回归：cal 叶子 `kDefaultCalParams.cal_factor/cal_offset` 地址/宽度/单位（"mm"）断言；负例：cal Deposit 指向不存在 RECORD_LAYOUT → 告警口径保持 |
| 18-B 桥接 `StructureLayoutResolver` | 按 16-B：递归展开（深度上限+DFS 环检测）、标量/固定数组/嵌套/数组嵌结构体四形态、冲突路径拒绝、叶子索引一次性发布 | 解析表输出 + 环/缺失/越界负例 |
| 18-C 查询与读写语义 | 按 16-C：`Find` 支持限定叶子路径；`SymbolInfo` 填地址/extension/宽度；`ByteSizeOf`/`ComputeElementAddress`（偏移+步长，B-1 不乘 AG、32 位溢出检查）；`ToPhysical/FromPhysical` 仅对可证叶子；本体 STRUCTURE/INSTANCE 整块操作**继续拒绝**（B-12 不变） | golden + 单元用例 |
| 18-D 真实 Slave E2E | `xcplite_a2l_read_test.cpp`：解除 SKIP，成员级 Find→读→换算（`g_simple_struct.simple_i16`、`g_outer.nested_struct.simple_u32`、`g_struct_array[2].simple_u8`、`nested_array[1]`）；`xcplite_write_test.cpp`：成员级 FromPhysical→写→读回（协议层"基址+offset"结果作**交叉校验基准**，两种通路必须一致） | ctest -R Xcplite 全绿、0 SKIP |
| 18-E 双 ABI 门禁 | Release/Debug prepared 同步重建（含批次17 §3.2 两处修复 + v6 新 DTO）；ON/OFF ctest、A2lIsolation/P9、clang-format | 门禁记录 |
| 18-F 文档回写 | 批次16 计划的 §1.2/§6 状态更新（语料门解除说明）+ 批次18 实施记录 | code-plan |

**依赖**：无新外部依赖。**风险**：上游 TYPEDEF_MEASUREMENT 字段语义与 16-A 预期不符（批次16 §5 风险1）→ 18-0 语料核证不过即停在验证阶段，纪律同批次16。

## 2. 批次19 —— Seed&Key 真实闭环（原 Phase2-03 成案）

**目标**：Master `Unlock()` 对**编译了 Seed&Key 的** XCPlite Slave 走通
GET_SEED（含分段）→ 算法回调 → UNLOCK → 受保护写 → 再连恢复；错误 Key 的断开/重建行为实测。

| 任务 | 内容 | 完成证据 |
|---|---|---|
| 19-0 机制核证 | ① 新建本树 `tests/xcp_test_slave/xcplite_seckey_cfg.h`（override 头，仅 `#define XCP_ENABLE_SEED_KEY` 等宏），经 `XCPLITE_CFG_OVERRIDE` 注入——不改 thirdparty 任何文件；② 核证 Slave 侧回调挂点（`ApplXcpGetSeed`/`ApplXcpCompareKey` 的声明位置/签名/默认弱符号行为）与资源锁存语义（CONNECT 后保护位、UNLOCK 后 GET_STATUS） | 核证表（文件:行 + 结论）；不通过则本批停在设计 |
| 19-1 双配置 Slave 构建 | xcplite 库一次构建目录只编译一种配置 → 用第二个 `add_subdirectory`（显式 `BINARY_DIR` 隔离）+ override 头产出 `xcplite_seckey` 变体目标；测试 Slave 程序加 `--seckey` 启动参数（或第二目标 `xcp_test_slave_seckey`），密钥算法与 `TestKeyAlgorithm`（seed^0x5A^i）对齐，保证确定性 | ON 时两个 Slave exe 均可启动/服务 |
| 19-2 正闭环用例 | `XcpliteSeckeyWriteTest`：连接→受保护资源读→`ERR_ACCESS_LOCKED`；`Unlock(CalPag)`→GET_SEED 多段（>6B seed）→Key 分段 UNLOCK→写 CalSeg/全局变量→读回；GET_STATUS 保护位前后变化 | 全绿 |
| 19-3 负例 | 错误 Key→实然行为（按规范 Slave 断会话：Session Failed + 重连 Reset）；未解锁直接 DOWNLOAD→AccessLocked 且不自动解锁（计划 §6.4 纪律） | 断言+会话状态双证据 |
| 19-4 门禁 | 新目录变体不进默认构建（`LIBXCP_BUILD_XCPLITE_SLAVE` 内子开关或无条件构建但仅 ON 时测试）；OFF 基线不变；记录 | ctest 汇总 |

**风险**：XCPlite 的 seed 长度/资源位映射若与 libxcp 分段假设（MAX_CTO-2）冲突 → 19-0 先证；override 机制与 `XCPLITE_CONFIGURATION=default` 的互斥约束（其 CMakeLists 已核证：override 仅 default 配置可用，恰为我们所用）。

## 3. 批次20 —— DAQ 实时采集端到端（后续里程碑成案）

**目标**：Master 对真实 Slave 完成"取证/配置 → START → 事件触发 → DTO 流 → 桥接解码 → 物理值序列"。

| 任务 | 内容 |
|---|---|
| 20-0 行为核证 | Slave DAQ 模型实测：其 A2L `DAQ DYNAMIC ... IDENTIFICATION_FIELD_TYPE_RELATIVE_BYTE / ADDRESS_EXTENSION_FREE` 的运行时真值（GET_DAQ_PROCESSOR_INFO）；ALLOC_DAQ/ALLOC_ODT 动态分配通路 vs 预定义列表；事件触发→DTO 帧格式（PID/timestamp/计数器位）；CASDD 扩展在 ODT entry 的透传 |
| 20-1 编排补齐 | libxcp 侧若需 ALLOC/FREE 编排（现批次14 只有可配置 STATIC 通路）——按 20-0 结论决定是否新增 `ExecuteAllocDaq` 等（**新增接口须按 AGENTS 附新测试**）；DTO 接收→账本（B-6）→桥接 DaqLayout 解码 E2E |
| 20-2 实时性用例 | 1ms 事件周期下收 N 帧计数、CTR 连续性、STOP 后无新帧、重配代际（generation）拒绝陈旧解码；结构体成员进 ODT（依赖批次18） |
| 20-3 负例/边界 | 未知 PID 拒绝不猜（已有 UdpTestSlave 版，真实 Slave 复证）；PREDEFINED 列表拒写（若 Slave 存在预定义形态）；overrun/丢帧在 UDP CTR 层的诊断 |
| 20-4 门禁+记录 | 全量回归 + 批次20 实施记录 |

**依赖**：批次18（成员级符号才可"结构体进 DAQ"，计划 §2-20 注记）。**范围外**：STIM、包模式 PACKED_MODE、时间戳相关度（GET_DAQ_CLOCK/时间同步）。

## 4. 批次21 —— 工程性收口与跨平台（可拆分穿插）

| 任务 | 内容 | 备注 |
|---|---|---|
| 21-1 Linux/macOS 实跑 | 夹具 POSIX 分支（fork/exec/SIGTERM）、ELF 链接期事件 ID 路径、`xcp_test_slave` 在 gcc/clang 的编译与全套 ctest | 批次17 仅 Windows 实测；这是交付面的硬缺口 |
| 21-2 GET_ID 拉取 A2L | Slave 侧经 override 启用 `OPTION_ENABLE_A2L_UPLOAD`，Master 走 `GET_ID(MODE=1)+UPLOAD` 拉取 A2L 文本（免"Slave 落盘+Master 本地读盘"，贴近真实 ECU）；需 libxcp 补 GET_ID/UPLOAD 文件通路（新接口+新测试） | 中期价值；不阻塞 |
| 21-3 磁盘与并发治理 | `xcplite_runs/` 陈旧 run 目录修剪策略；ctest 并行 `-j` 下端口游标防冲突压测 | 低危 |
| 21-4 使用文档 | `docs/`新增"真实对手端协议调试指南"（拓扑/构建开关/常见负响应定位表：ERR_CMD_SYNTAX↔SET_MTA 案例式条目） | 可维护资产 |
| 21-5 SDK Debug prepared | 若 18-E 已完成则销账；否则合入 | 批次17 遗留 |

## 5. 全局约束（延续批次17，各批次必须遵守）

1. 不修改 `thirdparty/XCPlite`、`thirdparty/a2llib` 任何文件；配置差异一律走 override 头/启动参数/测试侧适配。
2. `thirdparty/a2l-sdk` 为本项目自维护子工程，修改需 ABI 评估 + 双侧 prepared 重建 + A2lIsolation 门禁。
3. 新接口必附 tests 用例；每次修改落 code-plan 记录；doxygen 中文注释；clang-format 收口。
4. 协议行为断言以**实然**为准（对手端不合规处钉住事实 + 注释指回核证，如批次17 F7/F9），不静默适配、不猜测。

## 6. 验收映射（对原始需求）

| 原始需求 | 当前状态 | 批次18-21 后 |
|---|---|---|
| XCP 协议全走通（第一阶段） | 基础命令 ✅（批次17） | + Seed&Key 真闭环（19）+ DAQ 实时（20）→ 完整 |
| A2L 走通（第一阶段） | 加载/端点/标量/数组/INSTANCE 基址+**成员级叶子路径**（18 ✅） | ✅ 完整 |
| 基本/结构体/数组/嵌套变量读取 | 协议层 ✅；A2L 层成员叶子 ✅（18） | 双层全 ✅ |
| 修改 ECU 变量（第二阶段） | 标量/数组/成员/嵌套/CalSeg ✅（批次17） | + 受保护资源写（19）+ DAQ/STIM 面向实时（20） |

## 7. 变更记录

| 轮次 | 内容 |
|---|---|
| P18-21-1 | 依批次17 遗留表 + 计划内"后续里程碑"拆解为 4 批次，给出排序依据、任务表、依赖链（18→19→20）与全局约束；18 同时充当批次16 的解冻与接管批次 |
| P18-21-2 | **批次18 完成（2026-07-29）**：18-0～18-F 全交付，实施中新发现并落地 18-A′（CalSeg TYPEDEF_CHARACTERISTIC 标量事实并入，真实语料 InvalidLayout 2→0）；§6 验收映射"A2L 走通（第一阶段）"与"基本/结构体/数组/嵌套变量读取"两行改判 ✅ 完整；§1 任务表加 18-A′ 行；批次16 计划关闭（B16-P3）、R4 设计文档新增 R4.9、B 类决策 B-12 行更新；证据 `XCPlite_Slave_协议调试_实施记录_批次18_结构叶子展开.md`（ON 377/OFF 352、A2lGolden 38+1SKIP、Xcplite 25/25、双 prepared root、clang-format 0）。下一步：批次19 Seed&Key（19-0 机制核证先行） |
