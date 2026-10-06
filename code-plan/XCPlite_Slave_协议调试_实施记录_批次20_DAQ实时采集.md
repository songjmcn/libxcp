# XCPlite Slave 协议调试 · 批次20 实施记录 —— DAQ 实时采集端到端

> 状态：**批次20 已完成**（20-1 命令层+编排+mock、20-2/20-3 E2E 与行为锁定、20-4 门禁全绿：单元 XcpDaq* 30/30、E2E XcpliteDaq* 3/3、全量 ctest 389/389（1 已知 SKIP）、clang-format 已跑）。
> 本文档随批次推进增量回写；§1 为只读核证事实表（全部相对 `thirdparty/XCPlite/`，未改任何文件）。

## 1. 20-0 Slave DAQ 实然核证表（D 系列）

| # | 事实 | 证据 |
|---|------|------|
| D1 | 传输入口是 `XcpEthServerInit(127.0.0.1, port, tcp=false, 64KB)`（slave main.cpp:147），库内 receive/transmit 双线程（src/xcpethserver.c:349/:382，循环 :469-543/:546-586）；不存在 XcpTransportLayerUdp/XcpUdpPoll | src/xcpethserver.c |
| D2 | 事件创建：`DaqCreateEvent(testev)` 宏（inc/xcplib.h:412-415），MSVC 下回退 `XcpCreateEvent(name,0,0)` → cycle=0（突发）、prio=0 | main.cpp:167 |
| D3 | 泵循环每 ~1ms `DaqTriggerEvent(testev)`（main.cpp:234-237）——**slave 零改动即可周期推 DTO**；触发→`XcpEventExt_Var`（xcplite.c:1859-1879）：isStarted 门 + isDaqRunning 门，未 START 零输出 | main.cpp / xcplite.c |
| D4 | 编译期容量：`OPTION_DAQ_EVENT_COUNT=16`、`OPTION_DAQ_MEM_SIZE=3072B` 静态池（xcplib_cfg.h:132/:136）；每 list 12B + 每 ODT 8B + 每 entry 6B（XcpCheckMemory xcplite.c:1112-1140），超→全表清零+CRC_MEMORY_OVERFLOW(0x30)；**无独立表条目上限常量** | src/xcplib_cfg.h / src/xcplite.c |
| D5 | `XCP_PROTOCOL_LAYER_VERSION=0x0104`（xcp_cfg.h:29）；CASDD 寻址：ext=0x00 SEG / 0x01 ABS / 0x80 APP（:159-167），DYN=0x02..0x0F（:173-175）；`XCP_ENABLE_DAQ_ADDREXT` 开（:364）→ 每 ODT entry 独立 ext | src/xcp_cfg.h |
| D6 | GET_DAQ_PROCESSOR_INFO（:2475-2518，8 字节）：MIN_DAQ=**0**（无预定义列表）、MAX_DAQ=运行时 daq_count（**ALLOC 前查询=0 属正常**）、MAX_EVENT=XcpGetEventCount()、DAQ_KEY_BYTE=**0xC0**（DAQ_HDR_ODT_FIL_DAQW + ADDRESS_EXTENSION_FREE）、PROPERTIES=**0x11**（CONFIG_TYPE|TIMESTAMP；PRESCALER/RESUME/BIT_STIM/OVERLOAD 均 0） | src/xcplite.c |
| D7 | GET_DAQ_RESOLUTION_INFO（:2520-2531，8 字节，**无 1.4 扩展段**）：granularity DAQ/STIM=1（字节粒度）、max=248、TIMESTAMP_MODE=**0x0C**（1ns|DWORD|FIXED）、TIMESTAMP_TICKS=1e9/CLOCK_TICKS_PER_S（xcp_cfg.h:429-433） | src/xcplite.c |
| D8 | GET_DAQ_EVENT_INFO 可用（XCP_ENABLE_DAQ_EVENT_INFO，:2533-2559）：PROPERTIES=DAQ|EVENT_CONSISTENCY、MAX_DAQ_LIST=0xFF、事件名经 MTA UPLOAD；**GET_DAQ_LIST_INFO(0xD8) 无 case→CRC_CMD_UNKNOWN** | src/xcplite.c |
| D9 | 动态分配是唯一通路且顺序严格：FREE_DAQ(0xD7)→ALLOC_DAQ(0xD5,n)→ALLOC_ODT(0xD4)→ALLOC_ODT_ENTRY(0xD3)；乱序→CRC_SEQUENCE（XcpAllocDaq :1143-1167 要求先 odt/entry=0）；运行中门只挡 ALLOC 族/SET_DAQ_LIST_MODE/WRITE_DAQ（:2569/:2577/:2586/:2609/:2645）；**FREE_DAQ 无运行门**（:2562-2564 直接 XcpClearDaq()=隐式停流+整表清空+游标失效），FREE-first 编排令 ALLOC 系运行门实际不可达→**运行中整表重配置=合法的停流重建**（E2E RunningReconfigureStopsStreamAndRebuilds 实测；B20-3 修正原"ALLOC/FREE 均拒"表述） | src/xcplite.c |
| D10 | WRITE_DAQ（:2634→XcpAddOdtEntry :1260-1346）：size 1..248；ext=1(ABS) → base_offset=addr 原样（XcpAddrDecodeAbsOffset）；ext=DYN → 高 10 位=eventId 并**自动绑事件到 list**（:1336-1337，冲突→OUT_OF_RANGE）；未知 ext→CRC_ACCESS_DENIED；游标 entry 自增不跨 ODT（:1344）——**跨 ODT 必须重新 SET_DAQ_PTR** | src/xcplite.c |
| D11 | SET_DAQ_LIST_MODE（:2606-2624 + XcpSetDaqListMode :1350-1421）：mode 含 ALTERNATING|DIRECTION|DTO_CTR|PID_OFF 任一→CRC_OUT_OF_RANGE；**不含 TIMESTAMP 位→CRC_CMD_SYNTAX（时间戳强制 on）**；prescaler>1→OUT_OF_RANGE；事件绑定唯一入口=本命令 EVENTCHANNEL 字段；已绑不同 event→CRC_DAQ_CONFIG(0x2A)；SET_DAQ_LIST(0x1A) 不存在 | src/xcplite.c |
| D12 | **START_STOP_DAQ_LIST mode=1（单列表 START）被拒**：XCP_ENABLE_TEST_CHECKS 已定义（xcp_cfg.h:455）→ :2668-2671 直接 CRC_MODE_NOT_VALID。**必须** mode=2(SELECT) 逐列表 → START_STOP_SYNCH(0xDD) mode=1(start selected)（:2701-2714：TEST_CHECKS 下已运行→DAQ_ACTIVE；XcpCheckDaqLists :1424-1480 校验 event 绑定+entry size+内存可读；先回 CRM 再启动）。SYNCH mode=0 全停并等待队列 flush ≤250ms（XCP_TRANSMIT_QUEUE_FLUSH_TIMEOUT_MS xcp_cfg.h:389）；mode=2 停 selected | src/xcplite.c |
| D13 | DTO 帧格式（XcpTriggerDaqList_ :1586-1674，与 KEY_BYTE=0xC0 一致）：每 ODT 一个 DTO；头 4B=ODT 相对号(b0)+**0xAA 填充**(b1)+DAQ 绝对号 WORD(b2-3)；**无 PID/OTW 字节**；仅每事件首个 ODT 带 4B 时间戳（clock 低 32 位，1ns×TICKS，回绕）；队列满→丢弃本事件剩余 ODT（无 OVERRUN 指示可观测）；RESYNC/RESUME 未实现 | src/xcplite.c |
| D14 | DTO 目的地址 = CONNECT 时记录的 **master UDP 源 addr:port**（xcpethl.c:348-352 记录、XcpEthTlSend :135-168 sendto）——CRO 无 measurement port 字段，单 socket 回程；且**已连接会话若来源 IP 或端口变化→静默断连回 accept 态**（xcpethl.c:314-327）→ **master 的命令与 DTO 接收必须同一本地 socket/端口** | src/xcpethl.c |
| D15 | TL 头 4B=ctr16+len16（xcpethl.c:897）；CTR 对 **CRM+DTO 连续统一计数**（XCPTL_EXCLUDE_CRM_FROM_CTR 未定义，xcptl_cfg.h:126）→ 丢包判断需计入命令响应占号；CRM 也走发送队列 | src/xcpethl.c / src/xcptl_cfg.h |
| D16 | 再次 CONNECT 会 `XcpClearDaq`（xcplite.c:2073）清全部 DAQ 表——**断线重连必须整表重建** | src/xcplite.c |
| D17 | 禁发清单（真实 Slave 上必错）：SET_DAQ_LIST、GET_DAQ_LIST_INFO、READ_DAQ、CLEAR_DAQ_LIST（A2L OPTIONAL_CMD 已注释三者，a2l_writer.c:132-150）、SYNCHRONIZE（永远回 CRC_CMD_SYNCH，xcplite.c:2121-2125）、START_STOP_DAQ_LIST mode=1 | src/xcplite.c / src/a2l_writer.c |
| D18 | **A2L 与协议实然矛盾点**：a2l_writer.c:180-186 的 DAQ 段写死 `OVERLOAD_INDICATION_PID` 字样，但 PROPERTIES OVERLOAD 位=0、DTO 无 PID 头（D13）；`IDENTIFICATION_FIELD_TYPE_RELATIVE_BYTE` 对应 b0=ODT 相对号。解码以 GET_DAQ_PROCESSOR_INFO 实响应为准（B-16 口径）；EVENT 段（:304-322）testev→id=0、timeCycle=0 突发；OPTION_ENABLE_PERSISTENCE（xcplib_cfg.h:100）可能使事件 id 跨运行继承→**EVENTCHANNEL 不硬编码，从 GET_DAQ_EVENT_INFO/A2L 取** | src/a2l_writer.c / src/xcplib_cfg.h |
| D19 | 无 PID 型头不受支持佐证：SET_DAQ_LIST_MODE 拒 PID_OFF 位、START_STOP_DAQ_LIST 响应 FIRST_PID=0（:2664）；XcpSendEvent(0xFD PID_EV) 是 EP 通路不产生 DTO；GET_COMM_MODE_INFO COMMOPTIONAL=0 未声明 EP | src/xcplite.c |
| D20 | 命令字段布局已对照 ASAM 标准逐一验证（CRM_BYTE(n)=b[n]、CRM_WORD(n)=bytes 2n-2n+1）：CONNECT MAX_DTO=w2、SET_DAQ_PTR、WRITE_DAQ(SIZE=b2,EXT=b3,ADDR=dw1)、SET_DAQ_LIST_MODE(MODE=b1,DAQ=w1,EVENT=w2,PRESCALER=b6,PRIORITY=b7)、START_STOP(MODE=b1,DAQ=w1)、ALLOC 三件套（:314-315/:479-485/:689-735/:836-850）——master 编码可按标准布局直发 | src/xcplite.c / src/xcplite.h |
| D21 | **DTO 上线补白（B20-3 新增）**：queueAcquire 把 payload 尺寸向上取整到 XCPTL_PACKET_ALIGNMENT=4（queue.h:63），且 fill 计入 wire dlc——queue32m.c:291-296 TODO 明说 "The fill is included in the message dlc... the XCP client sees trailing filler bytes"→5B ODT1 帧上线实测 8B（尾 3B 填充）、13B combo 帧 16B；master udp_transport 按 TL len 交付不截尾；解码/断言必须按声明长度做**前缀核** | src/queue32m.c / src/queue.h / src/xcplite.c |

## 2. Master 侧既有 DAQ 面（M 系列，来源：子任务 4f9577f3 只读调研，file:line 已核实）

| # | 事实 | 证据 |
|---|------|------|
| M0 | **前提更正**：`ExecuteDaqConfigOrchestration`、`MakeConnectedUdp`、measurement/command/response 端口三元组均不存在；编排入口=`XcpMaster::ConfigureDaqList`，UDP **单 socket 模型** | 全库 grep 无匹配 |
| M1 | 编排结构体：`DaqEntrySpec{address,extension,size(AG 元素数),bit_offset}` xcp_master.hpp:55-60；`DaqOdtSpec{entries}` :63-65；`DaqListSpec{daq_list,event_channel,prescaler=1,priority,stim_direction,dto_counter,timestamp,pid_off,odts}` :74-84（注释：ALTERNATING/DYNAMIC/PREDEFINED 改写不支持 B-5；pid_off 解码侧必拒）；`DaqLedgerEntry` :92-103（pid=FIRST_PID+odt 号，未 START=nullopt） | include/libxcp/xcp_master.hpp |
| M2 | `ConfigureDaqList` 实然序列（xcp_master.cpp:172-301）：入参预检→pid_off 拒→`ExecuteGetDaqProcessorInfo` 缓存→**PROPERTIES bit0 CONFIG_TYPE(DYNAMIC) 置位即抛 MakeUnsupportedFeature"只支持 STATIC DAQ"**→MAX_DTO 预检（header=1+(dto_counter?1:0)，**不含 timestamp**，:220-222）→`ExecuteClearDaqList`+Bump→逐 entry `ExecuteSetDaqPtr`+`ExecuteWriteDaq`+追加账本→`ExecuteSetDaqListMode`（按 spec 布尔位）；失败 rollback erase | src/xcp_master.cpp |
| M3 | `StartDaqList` :303-325=`ExecuteStartStopDaqList(Start)`+FIRST_PID 回填；`StopDaq` :332-336=**StartStopSynch StopAll**（✓ 与 XCPlite 兼容）；`StopDaqList`=stop 单 list（XCPlite mode=0 允许）；`ClearDaqList`=CLEAR+Bump+MarkStopped+erase；`ReadDaqEntryAt`=SetDaqPtr+ReadDaq。**全链无 FREE/ALLOC_\* 调用** | src/xcp_master.cpp |
| M4 | 单命令面 command_executor.hpp：SetDaqPtr(:224,0xE2)/WriteDaq(:240,0xE1)/ClearDaqList(:251,0xE3)/SetDaqListMode(:263,0xE0)/StartStopDaqList(:275,0xDE)/StartStopSynch(:283,0xDD)/GetDaqListInfo(:292,0xD8,optional)/GetDaqProcessorInfo(:303,0xDA)/GetDaqResolutionInfo(:311,0xD9)/ReadDaq(:322,0xDB)；DaqPointer 重放 m_last_daq_ptr_ :442/:404。**缺失**（CommandCode protocol_types.hpp:85-95 未登记，纪律"只登记已实现命令"）：0xD6 FREE_DAQ、0xD5/D4/D3 ALLOC 三件套、0xDF GET_DAQ_LIST_MODE、0xE4-E7 | include/libxcp/command_executor.hpp |
| M5 | DTO 接收：ResponseParser::Parse 按首字节分派，**0x00-0xFB→DtoPacket{pid=data[0], data=整帧含PID}**（response_parser.cpp:109-170，R12 不剥头）；command_executor OnPacketReceived :94-180 → `m_event_listener_->OnDto`（:169-171，**transport 工作线程直回，无队列**）；IEventListener::OnDto :66，监听器仅构造期注入（XcpMaster ctor :119-121）。核内不做 PID 校验、不解析净荷（注释 :62-64 归桥接） | src/response_parser.cpp / src/command_executor.cpp |
| M6 | B-6：账本权威在 XcpMaster::m_daq_ledger_；代际在 Session（session.hpp:147-167，Bump 于 CLEAR 后/重连清态）；**未知 PID 拒绝在桥接解码层**（daq_layout_impl.cpp:128-153 路由未命中→NotFound，routes 空→InvalidLayout；"列表号==PID"回退禁用 :140-142）；generation 比对是调用方纪律（Decode 内不比对） | src/daq_layout_impl.cpp |
| M7 | 桥接 DaqLayout：DtoFrameLayout{identification_field_type,first_odt,counter_enabled,timestamp_enabled,timestamp_size_bits,overflow_indicator,pid_off,header_bytes}（daq_layout.hpp:127-139）；envelope=header_bytes(1)+counter(+1)+timestamp(+(bits+7)/8)（impl :37-43/:73-242）；A2lPredefined（A2L DAQ 段完整解析：OdtEntryDto/OdtDto/DaqListDto/DaqCapsDto + ListDaqLists）与 LocalLedger（CreateDaqLayoutFromLedger，路由 pid=*first_pid+i）两来源 Decode 全链已实现；EcuReadback 有入口。EventChannelInfo.cycle_time_us 恒 0（TIME_UNIT 无码表） | thirdparty/a2l-sdk/a2lbridge |
| M8 | UdpTransport：单 socket（local_port=0 OS 分配，RES+DTO 同端口收回）；`strict_remote_port=true` 默认→**来源过滤要求 DTO 源端口==remote_port**（udp_transport.cpp:463-472）——XCPlite 同 socket 回程则兼容（其 receive/transmit 双线程共用 bound socket，批次17-19 CRM 全绿已实证）；UDP TL 层 CTR 连续性检测（跳号报缺口/重复丢弃） | src/udp_transport.cpp |
| M9 | 既有测试锁定：xcp_daq_test.cpp:355 `DynamicProcessorPropertyFailsBeforeDaqWrite`（**DYNAMIC→master 抛——本批要改的正是这条**）；udp_test_slave 纯静态预定义不支持 ALLOC（:634 注释）且 SendDaqListDtos 手动触发；a2l_e2e_test.cpp:401/:493/:542/:594 真实 UDP e2e（模拟 slave）；golden 字节序 response_parser_test :366-464 / command_codec_test :200-248；xcp_udp_loopback **无 DAQ 用例** | tests/ |

## 3. 20-0 综合结论 + 20-1 设计

**阻断点 G1**（能力模型互斥）：真实 XCPlite 无条件 DYNAMIC（D6）∧ master 见 bit0 即抛（M2）→ 现码第一步即失败。缺口收敛为：
1. **核命令层**（20-1a）：CommandCode 登记 0xD6/0xD5/0xD4/0xD3（+可选 0xDF GET_DAQ_LIST_MODE 回读）；command_codec 编解码（ALLOC 三件套响应仅 RES；CRO 布局见 D20/xcplite.c:836-850）；command_executor ExecuteFreeDaq/ExecuteAllocDaq(n uint16)/ExecuteAllocOdt(daq,n)/ExecuteAllocOdtEntry(daq,odt,n)。
2. **master 动态编排**（20-1b）：新增 `ConfigureDaqListsDynamic(const std::vector<DaqListSpec>&)`：FREE_DAQ→逐 list ALLOC_DAQ(1)→逐 ODT ALLOC_ODT→逐 ODT_ENTRY→SET_DAQ_PTR(0,0,0) 复位游标→复用既有 WRITE_DAQ 循环与账本→SET_DAQ_LIST_MODE（**强制 timestamp 位**，D11；pid_off/stim/ctr/alt 仍拒）；`StartDaqSync()`：逐 list StartStopDaqList(Select)→StartStopSynch(StartSelected)（绕开 D12 单列表 START 被拒）。STATIC 通路 `ConfigureDaqList` 原样保留（模拟 slave 兼容），**xcp_daq_test.cpp:355 语义改判**：DYNAMIC 从"抛 unsupported"改"引导到动态编排或明确分派"——该测试按 AGENTS.md 新增接口附新测试纪律同步更新。
3. **G3 MAX_DTO 预检修正**：header 计入 timestamp（(timestamp_size_bits+7)/8），两路共享。
4. **envelope 适配 ODT/FIL/DAQ16**（D13/M7）：XCPlite 头 = ODTrel(1)+0xAA(1)+DAQabs(2) = 4 字节、无单字节 PID。桥接 Decode 现模型 header_bytes 固定 1→20-2 需核 `identification_field_type` 是否已建模 RELATIVE 型 4 字节头；不足则扩 DtoFrameLayout（a2l-sdk 自有，ABI 评估：DtoFrameLayout 若过 ABI 面需 bump）。路由键改 (daq_list,odt_number) 二元组。**若扩面成本过高，降级方案：20-2 E2E 先按 4 字节头手工切帧断言（协议级），桥接解码扩面记批次21/后续**——以实施时代码实况定。
5. 事件通道不硬编码（D18 persistence 风险）：E2E 从 slave A2L EVENT 段/GET_DAQ_EVENT_INFO 取 testev id。
6. G4 高频流：OnDto 线程直回→测试侧 listener 自持 mutex+vector 缓冲（a2l_e2e DtoListener 模式照搬）。
7. 重连清表（D16）：断言重连后 generation 变化/旧布局拒用（G7 调用方纪律演示）。

初判 Slave 端到端通路（全部命令与接收同一 UDP socket）：
CONNECT → GET_DAQ_PROCESSOR_INFO（PROPERTIES=0x11/KEY=0xC0；MAX_DAQ=0 正常）→ GET_DAQ_RESOLUTION_INFO（0x0C）→（GET_DAQ_EVENT_INFO(0) 取 testev id）→ FREE_DAQ → ALLOC_DAQ(n) → ALLOC_ODT → ALLOC_ODT_ENTRY → [SET_DAQ_PTR+WRITE_DAQ…] 逐 entry（ext=1，addr=A2L 绝对地址）→ SET_DAQ_LIST_MODE（mode 含 0x10 TIMESTAMP，prescaler=1，EVENTCHANNEL=核证值）→ START_STOP_DAQ_LIST mode=2 逐 list → START_STOP_SYNCH mode=1 → 收 DTO（4B 头 ODT/0xAA/DAQ16 + 首 ODT 4B 时间戳）→ SYNCH mode=0 停采（≤250ms flush）→ 断言停流。

## 4. 用例

**单元（tests/xcp_daq_test.cpp，XcpDaq* 全 5 suite 30 用例；新增 XcpDaqDynamic 9 条 :700-884）**：
- WholeTableOrchestrationIssuesFreeAllocChain：FREE→ALLOC_DAQ(n)→逐表 ALLOC_ODT→全表再 ALLOC_ODT_ENTRY（时序门）→逐 entry SET_DAQ_PTR+WRITE_DAQ→逐表 SET_DAQ_LIST_MODE（0x10 强制），CRO 字节精确（[D5][00][02 00] 等）。
- LedgerCarriesFullTriplesWithoutPid：账本带全三元组、pid 恒 nullopt（RELATIVE 信封无 Absolute 推导，B-16）。
- StaticSlaveRefusesDynamicOrchestrationBeforeFree：无 DYNAMIC 声明→unsupported 且 FREE 前不沾线。
- MisnumberedOrEmptySpecsRejectedBeforeWire：daq_list≠下标/空 specs/空 odts/size=0/pid_off → 预检拒、零命令上线。
- MidTransactionFailureFreesWholeTableAndDropsLedger：中途失败→best-effort FREE 整表+账本作废+代际 bump。
- FreeRejectionWhileRunningPropagatesAndKeepsTable：注入式 FREE 被拒（真实 XCPlite 无此门，见 E2E 与 D9 修正）→master 止步、账本不动。
- StartDaqSyncSelectsEveryListThenSynchStarts / StartDaqSyncWithoutLedgerRefused：逐表 Select→SYNCH(StartSelected)；空账本拒。
- StaticTimestampPrecheckUsesDeclaredWidthOnly：静态路径 G3——declared 4B 参与 MAX_DTO 预检；resolution 缺失→宽度 0 跳过（不猜，B-3）。
**E2E（tests/xcplite_daq_test.cpp，XcpliteDaqTest 3 用例，每例独立 slave 进程）**：
- DynamicAcqEndToEnd（1.6s）：建表→StartDaqSync→信封切分 [ODTrel][0xAA][DAQ16 LE]+首 ODT 4B ts（1ns tick）→值断言 0xDEADBEEF/0x42→WriteMemoryBytes 0xCAFEBABE 后实时刷新→StopDaq 停流（帧数不再增长）→二次重配置单 ODT 双 Entry（13B 上线补白 16B，前缀核）→停流断链。
- RunningReconfigureStopsStreamAndRebuilds（3.1s）：**D9 修正实证**——运行中整表重配置成功（FREE 无运行门=隐式停流+重建），重建后不启动零新帧、StartDaqSync 即恢复且值正确。
- MemoryOverflowFailsTransactionAndLeavesUsableTable（0.25s）：250 ODT 超 3072B 池→协议类错误→master 账本清空→同会话合法重建成功。
公共件：DtoCollector（G4：OnDto 为 transport 线程直回，mutex+vector 快照）、ResolveEventChannel（D18：testev 通道号扫 A2L EVENT 段而非硬编码）、BytesPrefixEq（D21 补白容忍）。

## 5. 门禁

- 构建：cmake --build cmake-build-xcplite --config Release（libxcp_tests + XcpliteIntegration）0 error。
- 单元：libxcp_tests.exe --gtest_filter=XcpDaq* → 30/30 PASS。
- E2E：XcpliteIntegration.exe --gtest_filter=XcpliteDaq* → 3/3 PASS。
- 全量 ctest（Release）：**389/389 PASS**（1 已知 SKIP：AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8），Total 30.25s。
- clang-format 22.1.8：批次 9 个改动文件全部跑过（4 个有格式收敛，复验构建+测试绿）。

## 6. 遗留

- D18 A2L `OVERLOAD_INDICATION_PID` 字样与实然矛盾：bridge 的 DaqLayout 若从 A2L DAQ 段推导 identification 方式须以协议响应校正（真实 ECU 预定义场景另议）。
- 队列满丢事件在 Slave 侧不可观测（无 OVERRUN 位），负例只能靠 CTR 缺口间接发现（D15）。
- 批次20 走 §3 降级方案：E2E 手工切信封断言；DtoFrameLayout 对 RELATIVE 4B 头（ODTrel+0xAA+DAQ16）的桥接解码扩面未做，记后续批次议题。

## 7. 变更记录

- B20-1（2026-07-30）：建档；回写 20-0 Slave 侧核证表 D1-D20（来源：子任务 c01804fa 只读调研报告 + 本会话直读印证，路径/行号经抽查复核）。
- B20-2（2026-07-30）：20-1a 命令层（CommandCode 0xD6/D5/D4/D3 + codec cpp:288-338 + executor :720-758）；20-1b master 编排（ConfigureDaqListsDynamic/StartDaqSync/共享预检 TimestampModeSizeBytes/PreflightDaqListSpec/DaqModeBitsFromSpec/ValidateDaqListAgainstMaxDto/G3 resolution 缓存）；20-1c mock XcpDaqDynamic 9 用例，XcpDaq* 30/30。
- B20-3（2026-07-30）：20-2/20-3 E2E（xcplite_daq_test.cpp 3 用例）落地；两处实然推翻纸面核证——①FREE_DAQ 无运行门（xcplite.c:2562-2564，D9 修正）；②队列 wire 补白客户可见（queue32m.c:291-296，新增 D21）。断言改前缀口径后 3/3 绿。
- B20-4（2026-07-30）：20-4 门禁收口——全量 ctest 389/389（1 SKIP 已知）、clang-format 全量、批次20 提交。
