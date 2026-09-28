# A2L 集成 R4 实施记录 —— 批次14：XCP 核心 DAQ 命令组、写回与 B-6 实际账本

日期：2026-09-27　　状态：**已完成**（CP-14 五步门禁全通过）
依据：`A2L_集成_R4_剩余任务实施计划_批次13-14.md` §3（T14-01～T14-13 + CP-14）；
语义基线 `A2L_集成_a2llib选型与适配层详细设计_R4.md`（本批修订见 §9.1 **R4.8**）+
`A2L_接口语义_B类决策_R4.md`（B-5/B-6/B-7/B-16）。
配套：批次13（A2L 侧收尾）记录 `A2L_集成_R4_实施记录_批次13_A2L侧收尾.md`。

## 0. 写码前核证与两处权威来源裁决（全部工具实测）

| # | 核证项 | 实测结论 | 由此定稿 |
|---|---|---|---|
| C-1 | **R12 DTO 帧边界** | `include/libxcp/response_parser.hpp:55-59` 注释「DTO 数据（PID 之后的全部字节）」+ `src/response_parser.cpp:81,123-129`（`body = packet.subspan(1)`）→ `data` **不含 PID**；而 `a2lbridge/src/daq_layout_impl.cpp:86` 用 `dto[0]` 当 EPK、`daq_layout.hpp` 的 `Decode` 契约写"完整 DTO 字节（含 envelope 头）" | 用户裁决：**`data` 含 PID**（整帧交付），`pid` 字段保留作便捷读数；同步更新既有断言与 `IEventListener::OnDto` 注释 |
| C-2 | **字节偏移权威** | `docs/XCP_1.3.0_document.md` 全文只有字段顺序，`grep "Position [0-9]"` 的 20 处命中全在 §7.1.2（CTO/RES/ERR/EV/SERV 通则）与 §7.5 通用规则，**DAQ/DOWNLOAD 组无任何 Position 表**；仓库内唯一精确偏移是只读 `thirdparty/XCPlite/src/xcp.h` | 用户裁决：docs 定字段语义与 mandatory/optional，XCPlite 定字节偏移；逐条已在 `command_codec.hpp` 顶部对照表落文档。**XCPlite 的 SET_MTA 是 XCP 1.4 的 8 字节变体**（`xcp.h:549-551`）→ 明确禁止照抄，本库维持 7 字节（既有 `EncodeSetMta` + `GoldenLengthsMatchMaxCtoEightLayout` 为准） |
| C-3 | **计划文档 T14-04 有误** | 计划写「GET_DAQ_LIST_INFO 的 MAX_ODT/MAX_ODT_ENTRY/**FIRST_PID**」；docs L2452-2457 该命令返回四项**不含 FIRST_PID**，FIRST_PID 出自 `START_STOP_DAQ_LIST`（docs L2222-2226） | 按 docs 实现：`GetDaqListInfoResponse` 无 first_pid 字段，FIRST_PID 由 `StartDaqList()` 取；偏差已在此登记并在 §9.1 R4.8 ⑧ 回写设计文档 |
| C-4 | **Executor 复用边界** | `CommandExecutor::RunCommand` 是 private（`command_executor.hpp:190-191`），codec/parser 无 getter；`m_session_.MarkCommandSent` 提供单 Outstanding 拒绝 | 新命令一律走「Executor 内加 `Execute*` 包装」，Master 不自建事务循环（否则绕过单 Outstanding 与 SYNCH 恢复） |
| C-5 | **隐含状态恢复只覆盖 UPLOAD** | `command_executor.cpp:434-437` 仅 `cmd == Upload` 调 `RestoreUploadMta()`；docs L2783 要求「WRITE_DAQ 前重新 SET_DAQ_PTR」、L2172「写过末位后指针未定义」 | 恢复分支扩到 `Download`（重放 SET_MTA）与 `WriteDaq/ReadDaq`（新增 `RestoreDaqPtr()` 重放 SET_DAQ_PTR）；函数名沿用 `RestoreUploadMta` 以不动既有已验证代码，注释写明语义已扩展 |
| C-6 | **Slave 既有断言冲突** | `tests/udp_test_slave_test.cpp:345-350` 用 `0xF0`(DOWNLOAD) 断言 `ERR_CMD_UNKNOWN` | DAQ/DOWNLOAD 模拟**默认关闭** → 该用例语义仍成立；另把该用例的探测码换成始终未实现的 `0xD2`，并新增 `UdpTestSlaveCommands.DaqCommandsRejectedWhileSimulationDisabled` 专门锁"默认关闭"这条不变量 |

## 1. 变更清单（按任务）

### T14-01 命令码与位域常量（`protocol_types.hpp/.cpp`）

* `CommandCode` 追加 DAQ 组：`ClearDaqList=0xE3 / SetDaqPtr=0xE2 / WriteDaq=0xE1 / SetDaqListMode=0xE0 / StartStopDaqList=0xDE / StartStopSynch=0xDD / ReadDaq=0xDB / GetDaqResolutionInfo=0xD9 / GetDaqProcessorInfo=0xDA / GetDaqListInfo=0xD8`。
  **纪律：只登记 Encode/Parse/Execute 齐备的码**；`ALLOC_*/FREE_DAQ/GET_DAQ_EVENT_INFO/DTO_CTR_PROPERTIES/WRITE_DAQ_MULTIPLE` 未实现故不入枚举（避免死枚举）。
* 新增常量与位域：`kDtoPidMax=0xFB`、`kDaqBitOffsetNone=0xFF`（docs L1861/L2170）、`DaqListAction`（0/1/2）、`DaqSynchAction`（0/1/2）、`DaqListModeBit`（bit0/1/3/4/5）、`DaqListPropertyBit`、`DaqGranularity`、`DaqTimestampMode`+`ParseDaqTimestampMode`、`DaqKeyByte`+`ParseDaqKeyByte`；均配 `operator|`/`HasDaqMode`/`HasDaqProperty` constexpr 工具。
  位值一律交叉参考 `thirdparty/XCPlite/src/xcp.h:331-338 / 367-388 / 394-414 / 419-423`，并在头文件注明「docs 无位表」这一事实；`TIMESTAMP_MODE` 只拆位不换算（R13 外部阻塞维持）。

### T14-02 帧边界定稿（R12）

* `DtoPacket::data` → 完整帧（`data[0]==pid`），结构体注释写明定稿理由；`response_parser.cpp` 的 DTO 分支改为 `assign(packet.begin(), packet.end())`。
* `IEventListener::OnDto` 注释由「本阶段仅识别，不解析内容」改为「整帧交付，可直接喂 `IDaqLayout::Decode`」。
* 既有断言 `ResponseParserClassify.DtoOnlyIdentifiedNotDecoded` → 改名 `DtoDeliversWholeFrameWithPid`，同时断言 `data[0]==pid`。

### T14-03 / T14-04 编解码（`command_codec.*`、`response_parser.*`）

* Encode 12 条：`EncodeClearDaqList/SetDaqPtr/WriteDaq/ReadDaq/SetDaqListMode/StartStopDaqList/StartStopSynch/GetDaqListInfo/GetDaqResolutionInfo/GetDaqProcessorInfo/Download/ShortDownload`。
  `EncodeGetDaqProcessorInfo/GetDaqResolutionInfo/ReadDaq` 为 **1 字节无参**（`xcp.h:789/798/781`），其余按对照表带 reserved=0。
* Parse 5 条 + 4 个响应结构：`StartStopDaqListResponse(first_pid)`、`GetDaqListInfoResponse`、`GetDaqResolutionInfoResponse`、`GetDaqProcessorInfoResponse`、`ReadDaqResponse`；新增私有 `ReadU32`（READ_DAQ 地址）。
  **AG/MAX_CTO 上限刻意不放进 codec**（codec 不持 Session 参数，见 C-4），统一由 `MemoryAccess` 分块层校验，与 UPLOAD 侧同口径，避免两处标准不一致。

### T14-05 / T14-06 / T14-07 / T14-08 执行、写回、运行态、编排

* Executor：11 个 `Execute*` 包装；可选命令（`GET_DAQ_LIST_INFO/GET_DAQ_RESOLUTION_INFO/GET_DAQ_PROCESSOR_INFO/READ_DAQ`）遇 `ERR_CMD_UNKNOWN` 返回 `nullopt`（docs L1631「无副作用」），判据抽成文件内 `IsCmdUnknown()`，与既有 `ExecuteGetCommModeInfo` 同口径；`SHORT_DOWNLOAD` **不降级**（写回必须有确认）。
* 隐含 DAQ 指针：`DaqPointer{daq,odt,entry}` + `m_last_daq_ptr_`，SET_DAQ_PTR 成功后记录、CLEAR_DAQ_LIST 作废、CONNECT 时随 MTA 一起清空；`RunCommand` 重试前按 C-5 重放。
* `MemoryAccess` 写回：`WriteBytes`（唯一入口）/`DownloadChunked`；上限 `MaxDownloadElements()=(MAX_CTO-2)/AG`、`MaxShortDownloadElements()=(MAX_CTO-8)/AG`（docs L2024/L2026）；每块前显式 SET_MTA、块间用 `XcpAddress40::Advance`；任一块失败上抛带「已完成 x/y 元素」的上下文，不做部分成功。
* `Session` DAQ 运行态：`MarkDaqListStarted/Stopped`、`ClearStartedDaqLists`、`HasRunningDaqList`、`DaqConfigGeneration/BumpDaqConfigGeneration`，并在 `EstablishConnection/CompleteDisconnection/Fail/Reset` **四处一致清理**（C-4 风险项）。
* `XcpMaster`：`WriteMemoryBytes` + DAQ 编排 `ConfigureDaqList/StartDaqList/StopDaqList/StopDaq/ClearDaqList` + 账本 `DaqLedger()/DaqConfigGeneration()`；`Disconnect()` 在有 List 运行前先 `START_STOP_SYNCH(stop all)`（失败不阻断释放路径）；`Connect()` 清账本。
* **配置纪律**：`ConfigureDaqList` 对每个 Entry 都显式 `SET_DAQ_PTR`（不依赖 Slave 自增），失败整批回滚账本（不留半份账）。

### T14-09 Slave 模拟（默认关闭）

`UdpTestSlave::SetDaqSimulationEnabled/SetDaqListPredefined/SendDaqListDtos` + `WriteAtAddress/HandleDaqOrDownload`；`SlaveDaqList/SlaveDaqEntry/SlaveDaqPtr` 记账；
FIRST_PID 规则 `0x10 + n×MAX_ODT(4)` 保证列表间不串号；`GET_STATUS` 的 DAQ_RUNNING 位随运行态；PREDEFINED 列表 WRITE_DAQ → `ERR_WRITE_PROTECTED`；指针指向不存在的 Entry → `ERR_OUT_OF_RANGE`（不伪造数据）。

### T14-10 / T14-11 桥接层账本与比对

* `DaqLayoutSnapshot` 由不透明前置声明落地为真实类型（`source/generation/lists/routes`）；`DaqOdtRoute` 实现 **PID → 单个 ODT** 路由（docs L2225）；`OdtEntryLayout::symbol_aliases` + `DecodedDtoSample::symbol_aliases` 落地 B-6「保留 aliases」；`A2lDatabaseImpl::FindAllByAddress` 提供歧义地址的候选名单（`FindByAddress` 仍只答唯一归属）。
* `Decode` 三分支：有 routes → 按 PID 定位；来源是 LocalLedger/EcuReadback 但无 routes → `InvalidLayout`；纯 A2L PREDEFINED → 维持「EPK == 列表号」旧口径（批次10~13 行为不变）。
* 同 PID 被两个不同 (daq,odt) 声称 → 撤销该 PID 的路由（歧义即拒判，解码落 `InvalidLayout`），而不是保留第一条。
* 新入口 `A2lBridge::CreateDaqLayoutFromLedger` / `CreateDaqLayoutFromEcuReadback`；`XcpMaster` 侧四个薄转发取证方法（`QueryDaqProcessorInfo/QueryDaqResolutionInfo/QueryDaqListInfo/ReadDaqEntryAt`）。
* B-16：`RuntimeXcpParams` 增三项可选 DAQ 字段（`nullopt`=未查则跳过），比对全部 `Severity::Error`（识别字段/地址扩展/Entry 粒度决定 PID 解释与对齐，错一项即系统性错位）；`ADDRESS_EXTENSION` 域枚举↔原始码显式映射（`PerDaq → 3`，不是序号 2）。

### T14-12 / T14-13 测试

* 新增 `tests/xcp_daq_test.cpp`（**17 条**，MockTransport + 脚本化 DAQ Slave）：配置序列与账本内容、START 前 pid 必为 `nullopt`、失败整批回滚、非法入参零发包、未配置列表拒启动、CLEAR 删账本并递增代际、MODE 位组合、`WriteDaq`/`Download` 超时后分别重放 SET_DAQ_PTR / SET_MTA、SHORT_DOWNLOAD 优先/回落/写保护、断连先 STOP 再 DISCONNECT（报文顺序即证据）、无 List 时断连不多发命令。
* `command_codec_test.cpp`：**+9 条**黄金报文/边界/长度锁定（含 `GoldenLengthsMatchMaxCtoEightLayout` 扩展）。
* `response_parser_test.cpp`：**+7 条**（DAQ PR 布局、截断负例、字节序反解、位段拆解纯函数）。
* `memory_access_test.cpp`：**+8 条**写回用例（SHORT_DOWNLOAD 优先、MAX_CTO=8 不可用、分块上限 (MAX_CTO-2)/AG、回落、写保护、AG 不整除/空数据零发包）。
* `udp_test_slave_test.cpp`：**+10 条** Slave DAQ/DOWNLOAD 行为（9 条 `UdpTestSlaveDaq` + 1 条锁"默认关闭"不变量；含 PREDEFINED 拒写与指针越界负例）。
* `a2l_e2e_test.cpp`：**+6 条**端到端（写回闭环、**A2L 序与账本序相反仍按账本解码**、空账本整帧拒绝、PID 挪位必拒而正确 PID 可解、EcuReadback 快照解码、B-16 用 ECU 真值 + 篡改反向证据）。
* `a2l_golden_test.cpp`：**+3 条**（`StructureMetadataOnly`、`DaqRuntimeComparisonErrors`、`ParseLargeFileUnderBudget`）。

> 上表的每条差值都是用 `git show HEAD:<file>` 与工作树逐文件数 `TEST(`/`TEST_F(` 实测得到，
> 不是估算：核心侧合计 **+51**（= OFF 模式 288 → 339 的实测差值），A2L 侧合计 **+9**。


## 2. 测试结果

| 门禁 | 结果 |
|---|---|
| `build-sdk.ps1 -Config Release` | EXIT=0（ABI v4 与批次13 同一次 bump，本批未再 bump） |
| 主树 ON 全量 ctest | **346/346 通过，0 失败**（1 项按设计 SKIPPED）；基线 292 → 346 |
| G7 OFF（`LIBXCP_BUILD_A2L=OFF`） | 构建 0 error；**339/339 通过**（基线 288 → 339，增量全部是本批新增**核心侧**用例；OFF 不含任何 A2L 目标，故"OFF 不受 A2L 影响"由"无 A2L 目标 + 全绿"证明） |
| clang-format | 32 个改动/新增 C++ 文件违规 **0** |
| P9 / `A2lIsolation` | configure 期扫描 + ctest 自动化双跑，命中 **0**（S1 桥接层 19 文件 / S2 主树 22 文件） |
| 新增用例数（实测差值，`git show HEAD:` 对比） | 核心侧 **+51 条**（`xcp_daq_test` 17 / slave 10 / codec 9 / parser 7 / memory 8）+ A2L 侧 **+9 条**（golden 3 / e2e 6），全部通过 |

**一次偶发失败的取证**：OFF→ON 切换后的首次全量运行报 1 项失败，`Testing/Temporary/LastTestsFailed.log` 与
`ctest --rerun-failed` 定位为既有用例 `UdpTestSlaveFault.ResponseCounterStartsFreshAfterEachInjection`
（真实 UDP 回环 + `WaitUntil` 超时，负载敏感；本批未改该用例逻辑）。单独重跑 8/8 通过，随后连续 5 次全量
346/346 通过。判定为环境时序偶发，登记为观察项。

## 3. 边界与遗留

1. **DYNAMIC/ALTERNATING/PID_OFF 帧一律不做**（B-5/B-7）：`ALLOC_*`、`WRITE_DAQ_MULTIPLE`、`GET_DAQ_EVENT_INFO`、`DTO_CTR_PROPERTIES` 未实现也不进枚举；PID_OFF 即便配置，A2L 侧解码仍显式拒绝。
2. **A2L PREDEFINED 的解码仍按「EPK == 列表号」**：这是批次10~13 的既有口径，本批只在快照上打 `source=A2lPredefined` 标记，未改其行为（改动会同时影响既有 4 条 DAQ 用例的期望，属独立收口项）。
3. **`generation` 由调用方负责比对**：桥接层不持有 ECU 句柄，无法自行判定"当前代际"，故 Decode 不做代际校验——`DaqLayoutSnapshot::generation` 是留给调用方的失配检测抓手（已在类型注释写明）。
4. **位元素 ODT 不展开**：`WriteDaq` 只发 `BIT_OFFSET=0xFF`；位偏非 0 的 Entry 解码保留 raw 不猜测（A2L 侧同样只元数据，B-9）。
5. **Slave 的 DAQ 语义是"可预测的一种"**，不是真机等价物：写过 ODT 末位后本模拟让指针停在末位（真实 ECU 可能未定义/回绕）；靠"每 Entry 前显式 SET_DAQ_PTR"把这类差异挡在门外。
6. **既有缺口登记（不改）**：同一 `XcpMaster` 上 `Disconnect → Connect` 不可用（`CommandExecutor::m_transport_closed_` 全仓无复位点，grep 实测 `command_executor.cpp:65/185`）；本批测试显式避开该路径并说明理由，是否修属核心恢复语义决策，需单独立项。
7. **R13/R14/R15 维持外部阻塞**：EVENT `TIME_UNIT` 数值表、真实项目 A2L 样本、UTF-16/32 支持（uchardet+ICU）均未随本批变化。
