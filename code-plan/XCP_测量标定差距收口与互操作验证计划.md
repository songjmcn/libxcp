# XCP 测量与标定差距收口及互操作验证计划

> 性质：现状复核后的分阶段计划；P0、P1 已按确认范围实施并记录，P2-P4 仍是待执行/待设备门禁的目标。目标平台 Windows / Linux / macOS，C++20。后续实施仍须按仓库 `AGENTS.md` 逐节确认方案、测试与范围；不擅自修改 `thirdparty/`，不重构已完成模块。

## 1. 目标与边界

目标是把“当前 XCPlite 上测量与非保护写回已跑通”推进到**明确声明适用范围、可靠取证、真实对手端验证的测量/标定交付**；不把“支持全部 XCP 1.3.0 命令/全部 Slave 方言”当作现有事实。

分两级验收：

- **G1 可交付能力**：原最小核心稳定；XCPlite 的读取、动态 DAQ、物理值采集、非保护写回保持全绿；标定新增命令完成代码复核与实际支持的对手端互操作；不支持的功能明确拒绝。原计划 Phase1-06 以经批准的**结构语料 + 运行时一致性**替代逐字节 A2L 静态文件。
- **G2 受保护标定与跨设备兼容**：使用*确实实现* Seed&Key/资源保护的独立 Slave 或 ECU，实测“锁定拒写→解锁→写回→读回”和错 Key 行为；测量通过经取证的事件/DTO/时间戳配置接入至少一种非 XCPlite 对手端。G2 必须有测试设备/软件可用性和用户选定的兼容范围，否则明确记为未验证，不能用 G1 代替。

**本计划不含** STIM、Flash Programming、NVM 持久化、所有传输层/包模式、任意厂商全部 IF_DATA 变体、CANape 认证。若“完整 XCP”指这些能力，须另立范围、规范矩阵与验收计划；不要以本计划完成宣称全协议完成。

## 2. 现状及对既有结论的修正

| 主题 | 已有证据与实际边界 |
|---|---|
| 最小核心 | `code-plan/XCP_1.3.0_最小协议核心实现计划.md` §10 的 8 命令、Session、UDP/恢复已有实现和 Mock/Loopback；前轮 `build-v09` Release CTest **466/466，0 失败、1 已知 SKIP**。仅为该构建树当时的执行结果，未等同于当前工作区重建，更不是 ECU/CANape 互操作。UDP 一 datagram 多 frame 为后续扩展，需使旧计划/实施记录口径一致。 |
| XCPlite 测量 | `tests/xcplite_daq_test.cpp` 与 `tests/xcplite_measurement_test.cpp` 已跑动态 DAQ、上传 A2L、物理值帧、写后实时刷新；后者 `ResolveEventChannel`/`EventBoundDatabase` 为**测试内**事件绑定，且显式调用 `SetEnvelopeMode(RelativeByte,4)`、`SetTimestampFirstOdtOnly(true)`、`SetTimestampUnit(1)`。`include/libxcp/measurement/measurement_session.hpp` 已提供配置 API，不要重复实现。 |
| 非保护标定 | `tests/xcplite_write_test.cpp` 覆盖标量/数组/成员/大块 DOWNLOAD/CalSeg 写读。当前工作区另有**未提交** `code-plan/XCP_1.3.0_变量标定实现计划.md`、`code-plan/XCP_1.3.0_变量标定修改说明.md` 及相应源码/测试修改：规划/实现了 MODIFY_BITS + 8 条 Page Switching 命令；`tests/xcp_master_integration_test.cpp` 有 `XcpCalibration` Mock 用例。它们不能当作尚待从零实现，也不能只凭说明书声称已完成真实 XCPlite page-switch E2E；应单独重建复核。 |
| 预置 A2L | 原 `code-plan/XCPlite_Slave_协议调试集成计划.md` §Phase1-06 要求 `tests/data/a2l/xcplite_test_slave.a2l`，该文件不存在；**但不能简单判为功能完全空缺**：`code-plan/XCPlite_Slave_协议调试集成计划_复核记录.md` §3 记录运行时基址/偏移不稳定，已以 `tests/data/a2l/xcplite_structleaf_corpus.a2l` 的结构回归 + 现场 `RuntimeConsistencyNoErrors` 替代。后续要正式回写原计划的变更理由及等效验收，是否还需规范化快照应先证实稳定字段。 |
| Seed&Key | Master 侧 `XcpMaster::Unlock` 及 `tests/seed_key_test.cpp`、`tests/xcp_udp_loopback_test.cpp` 的模拟对手端用例已存在。**vendored XCPlite 不可通过配置宏启用**：`code-plan/XCPlite_Slave_协议调试_实施记录_批次19_SeedKey设计停止.md` 记录 `thirdparty/XCPlite/src/xcplite.c` 的 GET_SEED/UNLOCK 位于 `#if 0`；`tests/xcplite_write_test.cpp` 当前仅断言 ERR_CMD_UNKNOWN。因此旧计划 Phase2-03 的“真实 XCPlite 解锁写”必须撤销或标注不可行，不得再次尝试单纯 override 宏。 |
| 跨平台 | Windows 本机用例已跑；POSIX 夹具已写但 `code-plan/XCPlite_Slave_协议调试集成计划_复核记录.md` §4 明确未取得 Linux/macOS 真机实测。 |

> 说明：先前审查把“缺逐字节预置 A2L”直接称作 Phase1 功能缺失，遗漏了仓库已记载的等效结构语料替代；本计划纠正为**文档验收口径未同步**。先前把 Seed&Key 作为“Master 标定功能缺失”也不准确：缺的是 XCPlite 真实受保护对手端闭环及跨设备验证。

## 3. 分阶段实施（每阶段先审设计，后编码）

### P0：冻结基线、统一计划与验证口径（首批，不改协议行为）

1. 核对当前未提交标定修改与其计划/修改说明：按命令逐条列出 Codec、Parser、Executor、Master、Mock/真实对手端覆盖，核对规范 `docs/XCP_1.3.0_document.md`，重点复核 GET_SEGMENT_INFO 变长、CAL/PAG 能力/保护位区别和 MODIFY_BITS 的超时语义。不得覆盖/回滚现有未提交修改。
2. 回写 `code-plan/XCPlite_Slave_协议调试集成计划.md` Phase1-06/Phase2-03 与 §7 DoD：列明逐字节文件不可复现的证据、结构语料替代的等效检查清单；Seed&Key 改为“XCPlite ERR_CMD_UNKNOWN + Master loopback”，真实受保护闭环转到 P2 的其他对手端，不得写成 XCPlite 已支持。
3. 完整 Release **干净构建**按 `AGENTS.md` 先清理已有目标再编译；分别测可选门 OFF、A2L+XCPlite ON；记录配置、编译器/系统、通过/失败/跳过、测试用例名及构建目录。前轮 466 项结果仅作为对照，不预设重建仍全绿。Linux/macOS 实测列单独门禁，无法跑则记为未验证。

**交付**：计划修订、能力/测试矩阵、该批实施记录；**DoD**：旧计划和新口径无相互矛盾；现有已通过测试无回归；未经重建或真实设备验证的项不标 PASS。

**本轮 A2L OFF 复核（Windows Release）**：`build-p0-off` 配置为 `LIBXCP_BUILD_A2L=OFF`、`LIBXCP_BUILD_TESTS=ON`、`LIBXCP_BUILD_XCPLITE_SLAVE=OFF`；先 clean 后全目标 Release 构建成功；全量 CTest 455 项中 454 通过、1 项既有 Skip、0 失败（`AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`）。该结果确认当前通用核心与测试可在不含 A2L/XCPlite 的配置下构建运行，不代表 P0 全部跨平台 DoD 完成。

**A2L+XCPlite ON 复核（Windows Release，历史全量干净构建基线）**：`build-v09` 配置为 `LIBXCP_BUILD_A2L=ON`、`LIBXCP_BUILD_TESTS=ON`、`LIBXCP_BUILD_XCPLITE_SLAVE=ON`；全目标 clean build 成功；当时 CTest 503 项中 502 通过、1 项既有 Skip、0 失败。构建输出含 MSVC C4244/C4267（测试中的 A2L 地址/长度窄化转换）和 C4100（未使用参数）警告；未在本轮改动这些警告。该历史结果由下方 P1 最新 513 项全量结果 supersede；均仅为 Windows 本地/模拟及 XCPlite 自有对手端证据，非外部 ECU 或跨平台互操作 PASS。

### P1：测量元数据取证与方言配置（以最小增量为原则）

1. 先审 `adapter/a2l/a2l_measurement_database.cpp`、现有 A2L SDK/IF_DATA 解析器和 `XcpMaster::QueryDaqProcessorInfo`/`QueryDaqResolutionInfo`/`QueryDaqEventInfo` 的真实返回字段。为事件通道建立**显式绑定来源**：可证的 A2L EVENT/IF_DATA 与 Slave 运行时查询交叉校验；变量到事件的对应关系如 A2L 无法证明，则由调用方提供绑定表，未知或歧义必须报错而非默认 0。不要把测试里的按文本匹配 `testev` 直接搬进核心。
2. 保持核心 `include/libxcp` + `src` 与 A2L 隔离；元数据收集放适配器/上层，保留 `IMeasurementDatabase` 可注入。DTO identification field 模式与长度取自可验证协商/配置；时间戳宽度、单位和“仅首 ODT”策略分别注明来源。XCPlite 专有行为作为可选 profile；无来源时 `timestamp_valid=false`，不能假造物理时间或默默猜方言。
3. 若当前 API 足够，优先写上层 profile/配置使用样例和测试，不新增公共 API；确需新增接口先提交字段来源、兼容性/ABI 评估并获得批准。对非法事件号、未知识别模式、时间戳单位缺失、过大 ODT、陈旧代际和队列丢帧做负例。

**涉及**：`adapter/a2l/`、必要时 `include/libxcp/measurement/` 与 `src/measurement/`；`tests/xcplite_measurement_test.cpp`、测量单测、A2L 隔离测试。**DoD**：XCPlite 无测试专用硬编码依赖仍能形成正确帧；至少一个非 XCPlite 的不同 DTO/事件配置跑通（真实端尚不可用时仅标模拟 PASS，G2 不完成）；现有显式配置用法保持兼容。

**实施状态（Windows Release 验证）**：P1 首轮硬化与经确认的适配器复用 T-02 均已完成，详情见 `code-plan/XCP_测量标定差距收口_P1实施记录.md`。A2L+XCPLITE ON 最新全量 CTest 为 513 项（512 通过、1 个既有 SKIP、0 失败）；A2L/XCPLITE OFF 为 460 项（459 通过、同一既有 SKIP、0 失败）。首轮历史结果 496/451 已由以上更新结果取代。代码包括显式 symbol-event binding、XCPlite EVENT/DAQ/timestamp runtime profile、测量生命周期/DTO 负例、GET_DAQ_EVENT_INFO reserved-byte 修正；`measurement_demo` 复用 adapter utility。此状态仅代表 Windows、本地 Mock/UDP Loopback 与 vendored XCPlite；真实非 XCPlite 对手端和 Linux/macOS 仍未验证。

### P2：受保护标定验证（外部对手端先行）

1. **前置选择门**：由用户选定具有可核实 Seed&Key、CAL/PAG 锁状态、写权限的外部 Slave/ECU 及允许的测试密钥/地址；若无，先仅完善 Master 协议测试并将“真实受保护标定”列为待验证。禁止未经明确批准修改 `thirdparty/XCPlite` 或虚构回调，不能把当前 XCPlite 改宏当作备选捷径。
2. 在对手端侧核对 CONNECT RESOURCE 与 GET_STATUS protection 分离、GET_SEED 分段、正确/错误 Key、锁定后 DOWNLOAD 的实际错误、断线后的重连与保护状态、页/段可写范围。密钥以测试算法/外部配置注入，不在源码或日志保存生产密钥；错误码保留，不自动解锁。除标定页外，禁止对真 ECU 的生产区写入。
3. 设计正负 E2E：锁定拒写 `ERR_ACCESS_LOCKED` → `Unlock(CalPag, calculator)` → 写已知测试 CAL 地址 → 读回/必要时复位恢复；错误 Key 拒绝及会话终态；无保护 Slave 应保持 XCPlite 原有直接写与 `ERR_CMD_UNKNOWN` 断言。保留现有 UdpTestSlave/Mock 测试，不把模拟成功写成真实 ECU 成功。

**涉及**：新对手端夹具/配置与 `tests/` 下新增测试、部署说明；Master 代码仅在可复现缺陷证实后最小修复。**DoD**：独立实现的可保护 Slave 正负闭环均 PASS，日志不泄密；设备不可得则明确“P2 未完成”，不能将替代覆盖算为本阶段通过。

**模拟补充（Windows Release）**：新增 `SeedKeyEndToEnd.ProtectedCalibrationWriteRequiresUnlockAndRelocks`，覆盖锁定 DOWNLOAD 拒绝、测试算法解锁、原值核对、写后读回及新 Master CONNECT 后重新锁定。此项仅为 UDP Loopback 模拟证据，不满足本阶段独立对手端 DoD，P2 真实验证仍未完成。另：同一 `XcpMaster` 实例在 DISCONNECT 后立即再次 CONNECT 的实验触发 `当前状态不允许进入恢复: Connecting`；本测试以新 Master 实例验证 Slave 新会话锁状态，不把该行为擅自扩展为修复范围。

### P3：分页/位标定真实闭环与非幂等超时安全

1. 以当前未提交的 `XCP_1.3.0_变量标定实现计划.md` 和 `XCP_1.3.0_变量标定修改说明.md` 为**已开展工作**复核，不重写 9 命令。先确认对手端真实支持的 Page Switching/MODIFY_BITS/COPY_CAL_PAGE 子集和 CalSeg 地址映射，再选至少一种支持者做 GET_PAG_PROCESSOR_INFO、段/页信息、ECU/XCP page 切换与读回、MODIFY_BITS 掩码读回、COPY_CAL_PAGE 成功/写保护负例；不支持的命令保留 `ERR_CMD_UNKNOWN` 测试，不能以 Mock 通过伪称 XCPlite E2E。
2. **风险门禁**：修改位、下载、复制/切页可能改变状态。审查 `src/command_executor.cpp` 在超时→SYNCH 后对 `MODIFY_BITS` 等操作重试是否会重复生效；特别是 `(Value & mask) XOR xor` 重复执行一般并非幂等。必须制定可证明的确认/对账策略或返回“操作结果未知”，而不是无条件再次发送。对可读回场景做响应丢失注入，对不可确认场景明确错误语义并保证下次 MTA/页状态可靠；方案在编码前单独确认。
3. 验证可选命令能力缓存/Session 断线后清空、Resource 缺失本地拒绝、锁定错误直达、字节序/AG 与预期一致；保留最小核心和 DAQ 回归。

**涉及**：现有标定文件、`tests/xcp_master_integration_test.cpp`、新独立对手端测试；仅修复已证实问题。**DoD**：规范核对有记录、支持命令真实 E2E PASS、不支持命令显式跳过/负例；超时状态不发生静默双写；无独立可分页 Slave 时不宣称“Page Switching 已完成互操作”。

**模拟实施（Windows Release）**：经用户确认，MODIFY_BITS、DOWNLOAD、SHORT_DOWNLOAD、SET_CAL_PAGE、SET_SEGMENT_MODE、COPY_CAL_PAGE 在响应超时后均不盲目重发；允许先发 SYNCH，但以 `ErrorCategory::OperationOutcomeUnknown` 报告结果不确定。Mock 覆盖六种命令已执行但响应丢失，且验证不重发；Download 执行前丢响应回归也更新为结果未知。`libxcp_tests` Release 构建通过；最终全量 CTest 508 项中 507 通过、1 项既有 Skip、0 失败。首次全量测试中的两个 A2L 性能阈值超限在修正旧 Download 预期后重跑均通过，详细波动数据见实施记录。详情见 `code-plan/XCP_测量标定差距收口_P3模拟实施记录.md`。此结果只关闭本地超时重试安全策略的模拟风险项；真实 Page Switching/MODIFY_BITS/COPY_CAL_PAGE 对手端闭环与 POSIX 验证仍未完成，P3/P4 整体不得标记完成。

### P4：跨平台与互操作发布门禁

- Windows/MSVC、Linux/GCC 或 Clang、macOS/Clang 各对 Release 构建、核心/DAQ/测量/A2L、XCPlite 真实进程用例实跑，记录架构、网络环境、端口、失败与重试；不可用平台标“未验证”，不得推断通过。测试隔离端口、就绪探测、进程退出与清理。
- 在至少一种非自有对手端完成抓包核对：CONNECT 参数、UDP LEN/CTR、DAQ DTO 信封/时间戳、CAL 资源与写回负响应；差异入兼容配置清单，不为单个 Slave 改坏标准路径。可选 CANape 对照只作为旁证，不冒充认证。
- 发布 `docs/` 使用/能力限制说明及 `code-plan/` 每批修改记录：测试数量和跳过项、已确认/未确认能力清单、设备与软件版本、抓包脱敏、回滚方法。

**Windows 本地硬化子项（不代表 P4 整体完成）**：经批准修正 UDP 接收轮询间隔为 0 时可能禁用 socket 接收超时、导致 `Close()` 等待接收线程的问题；配置校验拒绝 0，并检查 `SO_RCVTIMEO` 设置结果、保留平台错误信息。新增零间隔负例。干净 Release 构建成功；当时全量 CTest 502 项中 501 通过、1 项既有 Skip、0 失败。详见 `code-plan/XCP_测量标定差距收口_P4本地硬化实施记录.md`。

**XCPlite 子进程就绪身份绑定子项（不代表 P4 整体完成）**：就绪需同时满足进程存活、本次启动 token 标记存在且 CONNECT/DISCONNECT 成功；新增独占端口占用竞争回归。全 Release 目标增量构建成功；全量 CTest 503 项中 502 通过、1 项既有 Skip、0 失败。详见 `code-plan/XCP_测量标定差距收口_P4子进程就绪探测实施记录.md`。当前 Windows 本地测试通过，但 Linux/macOS 仍未验证（此前 WSL 返回 `Wsl/E_ACCESSDENIED`），非自有设备互操作也未完成；`setsockopt` 故障分支未做注入测试，故 P4 仍未完成。

## 4. 最终验收矩阵与停工条件

| 验收 | 可接受证据 | 未达标时结论 |
|---|---|---|
| 核心回归 | 干净 Release 构建 + Mock/UDP Loopback 全量 CTest（记录跳过原因） | G1 不通过 |
| XCPlite 测量/写回 | A2L 上传、DAQ 物理值、实时更新、CalSeg 写读全绿 | G1 不通过 |
| A2L 回归口径 | 结构语料展开结果 + 每次现场 A2L 的语义一致性；原计划已同步修订 | 文档/回归口径待收口 |
| 受保护 CAL 写 | 独立可保护 Slave 的正确/错误 Key、未解锁拒写、正确解锁写读实测 | G2 未验证；不能写“完整受保护标定” |
| 分页/位操作 | 支持该命令的真实对手端成功/负例 + 丢响应状态核证 | 仅说明 Master Mock 已测或受支持子集已测 |
| 泛化测量 | 非 XCPlite 实测 DTO/事件/时间戳来源可审计，未知值不猜测 | 仅 XCPlite profile 已验证 |
| 跨平台 | 三平台独立 Release 实测记录 | 仅列已测试平台，不泛称三平台通过 |

**顺序**：P0 → P1；P2/P3 在选定独立对手端、明确受保护测试地址后推进；P4 累积执行、最后统一收口。任一关键协议字段无规范或抓包证据、设备无法保证写入安全、或第三方修改无授权时停在设计/验证阶段并提交阻塞记录，不用“测试跳过”冒充成功。
