# A2L 集成 R4 实施记录 —— 批次12：ABI v3（include 链结构化 + UDP 端点透传）

日期：2026-09-26　　状态：已完成并全量验证
依据：会话内获批方案（四节：范围与接口契约 / 实现落点 / 测试方案 / 文档修订与执行顺序），
　　　即设计文档 §9.1 **R4.6 行**所指证据。
本批范围（用户裁决）：候选项 ①+②；候选 ③异步进度 / ④STRUCTURE / ⑤VAL_BLK-ASCII / ⑥T9-T10-Debug SDK
　　　**全部登记延后**（设计 §6.3 四行），10.4 核心 DAQ + 写回（P0）另批立项。

## 0. 写码前核证结论（全部工具实测，非规范推测）

| 核证项 | 结论 |
|---|---|
| IDoc 查询惯例 | 既有 `ListSymbols(std::vector<SymbolDto>*)` / `GetIfDataXcp(IfDataXcpDto*)` 一律 **out-param**，函数体在 DLL TU 内分配 → 新方法沿用同惯例（§5.3.3 R2/R3），不引入新边界风险 |
| `PathNameUtf8`（export.cpp:335） | 只取 `p.filename()` → 现有 message 链文本**不含目录**，结构化字段必须另取全路径才有定位价值（否则同名不同目录不可区分） |
| 上游 UDP 选项语法 | `udp_ip_options: %empty \| udp_ip_options udp_ip_option`（xcpdataparser.y:1207）→ **顺序自由**，但 `transport_layer_instance` 必须殿后 |
| `PACKET_ALIGNMENT` | 语法为 `PACKET_ALIGNMENT IDENT`（.y:1225）；`XcpOnUdpIp::SetPacketAlignment`（xcponudpip.cpp:19-29）只认 `PACKET_ALIGNMENT_8/16/32` 三个字面量，**未命中静默保持默认 8** |
| 子命令关键字 | **不是 `SUB_CMD` 而是 `OPTIONAL_TL_SUBCMD IDENT`**（.y:1228）；`AddSubCmd` 查表码值 `0xFA+index`，表内 0xFB/0xFE 为空槽 → 实际可用名仅 4 个（GET_DAQ_CLOCK_MULTICAST=0xFA / SET_SLAVE_IP_ADDRESS=0xFC / GET_SLAVE_ID_EXTENDED=0xFD / GET_SLAVE_ID=0xFF），未识别名**静默丢弃** |
| 多 UDP 实例 | SDK 现取 `udps.front()`（port/host 同限制）→ 本批沿用，不出现"端口取 front、子命令取全体"的混合口径 |
| 上游错误码归并现状 | `ErrorCode` 无 B 决策具名的 5 个码；逐个实测其归并落点与 Phase（写入 B 文档 §5 映射表） |
| `record_layout_impl.cpp` | `CheckRecordLayoutExecutable`/`RecordLayoutInfo` 全仓 **0 调用点**（已写未接线） |
| B-12 现状 | `SymbolKindDto::kStructure` 有枚举、`a2l_dto_map.cpp:234` 有映射分支，但 SDK 从不采集 `TypedefStructures()/Instances()` → **B-12 没有对应代码路径**（不是"静默放行"），映射表如实登记 |
| `UdpTransportConfig` | 主树 `udp_transport_config.hpp:26-43` 无 alignment / sub-command 概念 → 本批 ② 为**纯元数据落地，零传输行为变化** |

## 1. 变更清单

### 1.1 SDK（liba2l **ABI 2→3**）
- `include/liba2l/liba2l_api.hpp`：
  - `kLibA2lAbiVersion = 3u` + v3 变更注释块；
  - `IDoc` 追加 `virtual ErrorCode LastErrorChain(std::vector<std::string>* out) const noexcept = 0`（插在 `LastErrorLine()` 之后，纯追加不改虚表既有次序）；
  - `IfDataXcpDto` 追加 `udp_packet_alignment`（上游原始码 0/1/2）+ `udp_sub_commands`，注释写明上游静默降级语义。
- `src/liba2l_export.cpp`：
  - 新增 `PathFullUtf8()`（canonical 全路径窄化）与 `FillChainOut(out, chain, tail)` helper；
  - `WalkIncludes` 增出参 `out_chain`，**四个失败点**全部填链：入口超深（链=chain）、父层超深（chain+目标）、循环（chain+回指节点）、越根（chain+越根目标）；`message` 文本**一字未改**（批次11 口径）；
  - `PreScanIncludes` 透传 `out_chain`（入口先 clear）；`Load` 预扫描失败点把链 move 进新成员 `last_include_chain_`；`ResetState()`/`ClearError()` 同步复位（成功路径不残留）；
  - `Doc::LastErrorChain`：`out==nullptr` → `kBadArgument` 且**不改写** `last_code_/last_error_`；正常时输出快照，分配异常吞在 noexcept 内返回 `kInternal`；
  - UDP 提取（单实例 `front()`）补 `GetPacketAlignment()` / `GetSubCmds()`；
  - `DiffIfData` 追加 `PACKET_ALIGNMENT`（位宽文本）与 `SUB_CMDS`（逗号连接码值）两条 §6.3-A Info 冲突。

### 1.2 桥接层
- `a2l_result.hpp`：`Error` 追加 `std::vector<std::string> include_chain`（+ `<vector>` 包含），注释锁定"canonical 全路径 / 主文件→触发点 / 仅 include 类错误非空 / 与 message 文本互不替代"。
- `a2l_bridge.cpp`：
  - 同步 `Load` 失败分支取链（kOk 且非空才 move）；
  - 异步 completed_cb 失败分支：message 由硬编码 `"异步解析失败"` 改为 **SDK `LastError()` 文本**（空则回落 `"SDK 异步装载失败"`），固定标记进 `cause`，并同样取链 —— 与同步分支对称（B-19）；
- `a2l_dto_map.cpp`：`TransportEndpoint` 仅在 `kind==UdpIp` 时填 `packet_alignment = (raw<=2) ? 8u<<raw : 0`（位宽语义）与 `sub_commands`；其它传输层不拿 UDP 专属字段凑数。
- `if_data_xcp.hpp`：`packet_alignment` 默认 `1 → 8`（消除"=1 ///< 8/16/32 bit"自相矛盾）、两处"未建模"注释改为实际语义 + 单实例限制说明。

### 1.3 生成器与样本
- `tests/a2l_gen/gen_a2l.py`：新增 `UDP_ALIGNMENTS` / `UDP_SUBCMDS` 常量表（逐条取自上游源码，含"表内空槽不可用"事实）；XCPonUDP/IP 发射器支持 `packet_alignment` / `sub_commands`；**超范围取值直接 `SystemExit` 报错**——因上游是静默降级，生成器若同样静默就永远测不到差异（实测拒绝 `PACKET_ALIGNMENT_64` 生效）；文件头语法事实清单追加第 5 条。
- `golden_spec_mask.json`：+`PACKET_ALIGNMENT_32` + `OPTIONAL_TL_SUBCMD GET_SLAVE_ID / SET_SLAVE_IP_ADDRESS`（正向）。
- `golden_spec_xcpplus.json`：plain 块 `_16` + `[GET_SLAVE_ID_EXTENDED]`，plus 块 `_32` + `[GET_SLAVE_ID_EXTENDED, GET_DAQ_CLOCK_MULTICAST]`（制造 §6.3-A 双新项冲突）。
- `golden_spec_basic.json`：**故意不改** → 充当"未声明即默认 8bit / 空子命令"负例。

### 1.4 测试与接线（无新测试目标，断言全部落在既有用例）
- `tests/a2l_smoke_test.cpp`：
  - G1 注释随 bump 更新（v3 可建 / v2、v4 必拒，`±1` 逻辑自动跟随）；
  - **新增 G1b `Gate1bLastErrorChainChannel()`**：空指针→`kBadArgument`、**错误通道不被污染**、未加载时正常入参→`kOk`+空链；
  - IF_DATA 快照门禁补 `packet_alignment==8` + `sub_commands` 为空。
- `tests/a2l_golden_test.cpp`：
  - `ExpectLoadError` 参数扩至 8 个（新增 `chain_tail_name` / `chain_size` / `expect_chain_text`），**文本面与结构化面分离**；链元素数用**等值**而非下限；非 include 错误新增"不得带链"负向锁定；
  - `IncludeCycleRejectedBeforeParse`：链恰 4 元素 + `chain[1]==chain[3]`（回指证据）；
  - `IncludeDepthExceed32Rejected`：链**恰 33** 元素、尾 `d32.a2l`；
  - `IncludeEscapeRejectedUnlessAllowed`：链**恰 2** 元素、尾 `escape_target.a2l`，且**不断言箭头文本**（批次11 口径保持 `token (from 文件)`）；
  - `UdpEndpointAndProtocolLayer`：basic 负例（8/空）+ mask 正例（32 / `{0xFF,0xFC}`，顺序 = A2L 声明顺序）；
  - `XcpPlusPriorityAndConflictReport`：`plus_conflicts` 由 1 条扩为 **3 条**（MAX_CTO / PACKET_ALIGNMENT `16vs32` / SUB_CMDS `253 vs 253,250`）+ 选定块端点 `32` / `{0xFD,0xFA}`；
  - T7 异步：场景 2 断言 message 来自 SDK 原文且 `include_chain` 为空；**新增场景 2b**——异步加载 `cycle_main.a2l`，断言 `ParseFailed` + 箭头文本 + 链 4 元素 + 回指，证明链通道在 SDK 工作线程同样成立。

## 2. 编译期与测试期问题（3 处，全部实录）

| # | 现象 | 根因 | 处置 |
|---|---|---|---|
| 1 | 首轮 ON 全量 `A2lGolden` 失败 1 例：`IncludeEscapeRejectedUnlessAllowed` 断言 `message` 含 `" -> "` 不成立 | 我把"文本链"与"结构化链"塞进同一个开关，而越根错误的 message 按批次11 口径**本就保持** `token (from 文件)` | 拆为 `expect_chain_text`（循环/深度 true，越根 false）与 `expect_include_chain`（三者皆 true），并在参数注释写明理由 |
| 2 | `-j 4` 构建 EXIT=1 且日志只剩 MSBuild 版本行 | 本机 MSBuild 在代理环境变量下抛 `MSB6001`（build-sdk.ps1 注释早已记录） | 改串行 `cmake --build`（EXIT=0）；未改任何源码 |
| 3 | 设计文档 §9.1 编辑误伤 | 追加 R4.6 行的 `edit` 把 R4.5 行首吞掉、且插到其前 | 脚本化按行修复（恢复 `| **R4.5** | …` 行首、R4.6 移到其后），复核 LF 未被改成 CRLF、无 BOM |

另：链元素数（4/33/2）先以实测等值断言验证通过后才写入记录——未采用"先猜常量再调测试"。

## 3. 测试结果

| 项 | 结果 |
|---|---|
| `build-sdk.ps1 -Config Release`（含 format 后二次重建，prepared root 刷新） | ✅ EXIT=0（三次：编码后 / format 后） |
| 主树 ON 全量 ctest | ✅ **292/292 Passed**（A2L 子集 4/4：a2l_gen / A2lSmoke / A2lGolden / A2lE2E） |
| A2lGolden 内部 | ✅ 30 Passed + 1 Skipped（UTF-16 已知缺口 §6.3）+ 0 Failed |
| G7 OFF 门禁 | ✅ 构建 0 error、**288/288**（四个 A2L 测试整体消失）；恢复 ON 复验 **292/292** |
| clang-format（8 个改动 C++ 文件） | ✅ 违规 0；格式化后 SDK 重建 + 主树重编 + 全量复跑 292/292 |
| P9 隔离扫描 | ✅ `a2lbridge/**.{cpp,hpp}` 中 `#include <a2l/` **0 命中**；主树 `include/`+`src/` 中 `liba2l\|a2lbridge\|calmcar::xcp::a2l` **0 命中**（首轮 3 处命中系大小写不敏感误报的 `A2L` 字样注释）；`libxcp` 仍只链 `Threads::Threads` + `ws2_32` |
| 生成器自校验 | ✅ 非法 `PACKET_ALIGNMENT_64` → `SystemExit`，EXIT=1（非静默） |
| 改动面 | 13 文件 / +442 −40；**`thirdparty/a2llib` 零改动**、`include/libxcp`+`src` 零改动、CMake 无新目标 |

## 4. 边界与遗留（与 R4.6 行一致）

- **PID_OFF / TIME_UNIT / source_lower_bound** 三项维持批次10/11 口径，本批未触碰；
- **③异步进度**：`progress_notify_percent` 仍无消费者、`Progress()` 加载中恒 0（§6.3 新行）；
- **④STRUCTURE（B-12）**：SDK 仍不采集 `TypedefStructures/Instances`，B 文档 §5 明确"该路径尚不存在"；`record_layout_impl.cpp` 仍**已写未接线**（不擅自删）；
- **⑤VAL_BLK / ASCII（B-11 剩余）**：维持显式拒绝；无 ASCII 样本；
- **⑥T9 性能基线 / T10 隔离自动化用例 / Debug cfg SDK**：延后（§6.3 新行）；
- **UDP 多实例**：仍取 `front()`（port/host/alignment/sub_cmds 同一实例，口径一致）；
- **上游静默降级**（非法 alignment / 未识别 subcmd）：只透传不伪造检测，需真实需求时向上游提 PR；
- **§4.4 草案块漂移已顺带收口**（R4.6 ⑧）：`DtoFrameLayout` 文档块按实现回写为
  `timestamp_size_bits` + `overflow_indicator` + `header_bytes`，**未改任何代码**；
  并确立跨文档引用用字段名锚点、不用绝对行号；
- **B 文档 §5 映射表为归并口径唯一权威**：后续如需 UI 按类别分流，另批补具名码 + 改断言；
- **git 提交**：本批 13 个改动文件 + 1 个新记录文件**未提交**（AGENTS 要求明确指示）。
  批次12 单批体量适中，建议一次入库（与批次10/11 不同，无需拆分）。
