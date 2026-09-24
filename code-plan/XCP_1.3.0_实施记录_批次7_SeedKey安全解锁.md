# XCP 1.3.0 实施记录 · 批次 7：Seed & Key 安全解锁

日期：批次 6 之后（**批次 6 与本批次改动均尚未提交**——工作区当前累计批次 6+7
共 25 个改动文件与 3 个新文件；沿用惯例由用户提交取得回滚点）
分支：`feat_add_xcp`
触发：用户指令「根据计划文件与修改的结果文件，分析下一步的任务」→ 从批次 6 §6 的
A~E 五个候选方向中选择 **B. Seed & Key 安全解锁**。

---

## 0. 前置流程（AGENTS.md 创造性工作前置流程的实际执行）

1. **探索项目上下文**：通读设计文档（2610 行）、总体计划、批次 5/6 实施记录，
   并用检索扫过批次 1~4 与审核记录中全部「待办/未做/下一步」登记项，确认
   **已批准设计范围内欠账为 0**（接口差集 0/0、注释 172/0、命名 230 条全绿、
   253/253 测试），下一步只能是范围扩展或外部验证 → 提交 A~E 选项。
2. **逐一澄清（一次一问，均选择题）**：
   | # | 问题 | 用户决策 |
   |---|---|---|
   | Q1 | 算法接入方式 | **回调注入 `std::function`**（外部 DLL/SO 加载留后续里程碑） |
   | Q2 | 解锁触发策略 | **仅显式 API，维持计划 §6.4**「ERR_ACCESS_LOCKED 只报告、不自动解锁」 |
   | Q3 | 测试方案 | **Mock + UdpTestSlave 双层** |
3. **分节呈现设计（5 节逐节批准）**：§1 架构组件（初次要求补充详细说明后批准）、
   §2 数据流、§3 错误处理（采用推荐方案：拆分 ERR_ACCESS_LOCKED 映射）、
   §4 测试、§5 验收——全部获用户确认后才开始写代码。
4. **范围变更登记**：Seed&Key 原在计划 §2.2「不包含」清单内，本批次经用户批准
   转入范围；设计文档文末已就此补登记（见 §5）。

---

## 1. 实施前的关键查证（防幻觉：双源交叉验证，纠正了一处凭摘要的错误推断）

本地 `docs/` 只有 GET_SEED/UNLOCK 的语义描述（§7.5.1.8/7.5.1.9），**没有字节级
布局表**。按「不确定就验证」，用两个独立第三方实现仲裁：

| 报文 | 布局 | 证据 |
|---|---|---|
| GET_SEED CTO | **`[F8][Mode][Resource]`（Mode 在前）** | OpenBLT `XcpCmdGetSeed`：`data[1]==0` 判 mode、`data[2]` 取 resource；robotjatek/XCP `GetSeedPacket`：`MODE=0x00, RESOURCE=0x01`（偏移相对 PID）。⚠️ **中文摘要「GET_SEED(resource)」曾误导为 Resource 在前，查证纠正** |
| GET_SEED RES | `[FF][Length][Seed...]`，每段 ≤ MAX_CTO−2 | OpenBLT：`ctoData[1]=剩余长度（赋值在减法前，故 First 帧=总长）`；Length=0=未保护免解锁 |
| UNLOCK CTO | `[F7][Length][Key...]`，首帧=总长、后续帧=剩余 | OpenBLT `data[1]`/`data[2..]`；robotjatek `REMAINING_KEY_LENGTH=0x00, KEY=0x01` |
| UNLOCK RES | `[FF][Current Resource Protection Status]`，**每帧（含中间帧）都回 RES** | OpenBLT `ctoLen=2`；规范 §7.5.1.9「成功响应包含新的 Protection Status」 |
| Key 错误 | `ERR_ACCESS_LOCKED` **且 Slave 主动进入 DISCONNECTED** | 规范 §7.5.1.9 + OpenBLT `connected=0` 双源一致 |

其他写前读：`src/command_executor.cpp:258` 确认旧映射对所有命令**无差别**抛
`UnsupportedFeature`（消息「本阶段不支持解锁」）；`XcpMaster::Connect()` 开头
已 `m_session_.Reset()` → **Failed 后直接再次 Connect() 即可重建**（设计 §3 的
处置路径按实测修正）；析构对 Failed 状态安全（`IsConnected()` 为 false 跳过
DISCONNECT）。

---

## 2. 代码改动（7 个新函数 + 3 个新类型，落在既有 5 个头文件内，依赖图零变化）

| 文件 | 改动 |
|---|---|
| `include/libxcp/protocol_types.hpp` | 新增 `enum class SeedMode { First, Remainder }` |
| `include/libxcp/command_codec.hpp` + `src/command_codec.cpp` | `EncodeGetSeed(resource, mode)`、`EncodeUnlock(length_field, key_segment)`（唯一静态校验：`length >= 本段字节数`） |
| `include/libxcp/response_parser.hpp` + `src/response_parser.cpp` | `struct GetSeedResponse{length, seed}`、`struct UnlockResponse{resource_protection}`、`ParseGetSeedResponse`/`ParseUnlockResponse`（不扩展 ParsedPacket variant，沿用专用解析模式） |
| `include/libxcp/command_executor.hpp` + `src/command_executor.cpp` | `ExecuteGetSeed`/`ExecuteUnlock`（各自 = Encode → RunCommand → 专用 Parse，**零成本继承**单 Outstanding Command、超时、SYNCH 恢复、EV/SERV 分流）；`ExecuteUnlock` 捕获 Key 错误后 `Session::Fail()` 再上抛；**`DispatchResponse` 的 ERR_ACCESS_LOCKED 拆分映射**（§4）；**不新增任何成员变量**（命名审计成员总数只因测试设施新增而增长） |
| `include/libxcp/xcp_master.hpp` + `src/xcp_master.cpp` | `using SeedKeyCalculator = std::function<Bytes(Resource, BytesView)>`（参数序对齐规范 §9.2）、`struct UnlockResult{was_already_unlocked, optional<resource_protection>}`、`Unlock()` 编排：本地预检 → GET_SEED(First) → Length=0 短路 → Remainder 循环（空段/超长双重防御，防死循环）→ 回调（异常原样传播、Key 空/超 255 拒绝）→ 分段 UNLOCK（首帧总长/续帧剩余） |
| `include/libxcp/xcp_error.hpp` | `UnsupportedFeature` 注释举例由「Seed&Key」改为「DAQ、块模式」（原例已失效） |
| `tests/udp_test_slave.hpp` + `tests/udp_test_slave.cpp` | `SetProtectedResources`（**默认 0=无保护 → 既有用例零影响**）、`SetSeedContent`（默认 4 字节固定内容）、`HandleGetSeed`（单资源位校验/Length=0/ERR_SEQUENCE/分段推进）、`HandleUnlock`（首帧判定按 OpenBLT 同款 `length >= 上帧 length` 语义、收满才校验、Key 错→ERR_ACCESS_LOCKED+断开）、`EffectiveProtection`（GET_STATUS 动态 Protection、读命令受保护拒绝改为 ERR_ACCESS_LOCKED）、CONNECT 时清空解锁位与序列状态；新增 10 个 `m_<snake>_` 成员 |
| `tests/udp_test_slave.hpp/.cpp` | `TestKeyAlgorithm(Resource, BytesView)`：`key[i] = seed[i] ^ 0x5A ^ i`，**Slave 校验与 Master 回调共用同一函数**（消除两端误解） |

**未改**：`Session`（Fail 为调用既有方法，非改接口）、`MemoryAccess`、
`IXcpTransport`、UDP 栈；计划文档 §6.4 原文「报告需要后续 Seed&Key、不自动解锁」
与新方案语义本就兼容，**计划文档未动**。

## 3. 测试（253 → 288，新增 35 项，五层全覆盖）

| 层 | 文件 | 数量 | 代表用例 |
|---|---|---|---|
| A 编码黄金 | `command_codec_test.cpp` | 6 | `GetSeedFirstModeGolden`（`F8 00 01` 逐字节）、`SeedKeyCommandsAreByteOrderIndependent`（Motorola≡Intel）、`UnlockRejectsLengthSmallerThanSegment` |
| B 解析 | `response_parser_test.cpp` | 7 | `ZeroLengthMeansUnprotected`、`ByteOrderIndependent`、截断拒绝 |
| C 执行器原语 | `seed_key_test.cpp`（新文件） | 8 | `GetSeedRemainderModeEncoded`（`F8 01 04`）、`UnlockAccessLockedFailsSession`（ProtocolError + Failed + FailReason）、ERR_OUT_OF_RANGE/ERR_SEQUENCE 透传 |
| D Unlock 编排 | `seed_key_test.cpp` | 10 | `SkipsCallbackAndCommandsWhenUnprotected`（断言回调未调、零 UNLOCK）、`MultiSegmentSeedAndKey`（16B seed→3 帧 GET_SEED + 16B key→3 帧 UNLOCK 的**完整黄金序列**）、非法 resource 零发包、空 Key/256B Key 拒绝、回调异常传播、空续段 MalformedPacket |
| E 端到端 | `xcp_udp_loopback_test.cpp` | 4 | `UnlockProtectedResourceRestoresRead`（保护位可见→读被拒→Unlock→读成功→保护位清除）、`MultiSegmentSeedKeyOverRealSocket`（真实 Socket 上 6+6+4 分段）、`WrongKeyDisconnectsSlaveAndFailsSession` |

**既有用例更新（2 处）**：`recovery_test.cpp` 与 `xcp_master_integration_test.cpp`
中断言 `ACCESS_LOCKED→UnsupportedFeature` 的用例改为 ProtocolError，并新增
**消息含 "Unlock" 指引**的断言。grep 定位确认全仓仅此 2 处依赖旧映射。

## 4. 错误处理变更（设计决策 D9，已登记进设计文档 §18.2）

| 场景 | 旧（批次 ≤6） | 新（批次 7） |
|---|---|---|
| 读/标定命令收到 `ERR_ACCESS_LOCKED` | `UnsupportedFeature`「本阶段不支持解锁」 | `ProtocolError`（`GetErrorCode()=AccessLocked`）+ 消息指引先调用 `Unlock()`；§6.4「只报告、不自动解锁」保持不变 |
| **UNLOCK 命令**收到 `ERR_ACCESS_LOCKED` | 同上（无差别） | `ProtocolError`「Key 校验失败，Slave 已主动断开会话」+ `ExecuteUnlock` 捕获后 `Session::Fail(reason)`；重新 `Connect()` 自动 Reset 重建 |
| `UnsupportedFeature` 分类 | 兼管 Seed&Key | 回归「本阶段未实现功能」本义（DAQ、块模式），`xcp_error.hpp` 注释同步 |

## 5. 验证（最终复测值）

| 核对项 | 工具/命令 | 结果 |
|---|---|---|
| 构建 | `cmake --build cmake-build-release --config Release -j 4` | **exit 0**，`: error`/`warning C` 命中 0 行 |
| 测试 | `ctest -C Release`（连续 2 轮全量 + 1 轮定向） | **100% tests passed，288/288**（1 项 `Ag1_Cto8` 按设计跳过）；`Total Test time ≈ 21 s` |
| 注释覆盖 | `tools_check_doxygen_coverage.py` | **已注释 185 / 缺注释 0**（批次 6 为 172/0，新增 13 条声明全部带中文 Doxygen），自检 3 项 OK |
| 成员命名 | `tools_audit_member_naming.py code include src tests` | 255 条记录（230+25=UdpTestSlave 10 + QueuedSlave 3 + GetSeedResponse/UnlockResponse/UnlockResult 5 + ExecutorRig 4 + MasterRig 3），两级自检全 OK，**违规 0**（struct `m_` 归零、class 裸名归零保持） |
| 字段一致性 | `tools_audit_diff.py 文档 include tests` | **19/19 struct 字段完全一致**（16+新增 3），仅命名待改 0、真实差异 0 |
| 接口差集 | `tools_audit_iface_gap.py` | 类型差集 **0/0**；待实现成员函数 **0**；仅代码 0；仅文档 6 条均为已判定示例伪差（`master`/`move` 等） |
| 格式 | `clang-format --style=file --output-replacements-xml` | 22 个改动源文件**替换数 0**（首检 8 文件 81 处 → `-i` 修复后复查 0） |
| 行尾 | 字节级裸 LF 扫描 | 全部改动文件（含新建）**裸 LF=0** |
| markdown | `tools_check_md_fences.py` | 围栏 90 行成对闭合、行内三连反引号 0 |

## 6. 本批次自查纠错（4 次，全部当场抓回）

| # | 问题 | 如何发现 | 处置 |
|---|---|---|---|
| 1 | **`TestKeyAlgorithm` 签名实现成单参 `Bytes(BytesView)`**，与 `SeedKeyCalculator`（双参，对齐规范 §9.2）不匹配 → 7 处调用点 C2664 | 首轮编译 | 改回设计批准的双参签名（参数序 `Resource, BytesView`），`(void)resource` 占位 |
| 2 | `FullFlowSingleSegment` 黄金值**手算错误**（`0x03^0x5A^0x02` 算成 0x59 实为 0x5B）——实现是对的、期望是错的 | 首轮 ctest | 修正期望值并注明「由 TestKeyAlgorithm 计算，勿手算」；同文件 MultiSegment 用例因**动态计算** expected_key 而从未出错，反证黄金值不该手算 |
| 3 | **`tools_audit_iface_gap.py` 自身盲区**：tests 循环用 `_` 丢弃自由函数集合，`TestKeyAlgorithm` 被误报「仅文档存在」 | 接口差集复核 | 修正工具（tests 与 include 同口径并入 `c_free`），复跑回到 6 条已判定伪差；批次 6 时 tests 头文件无自由函数故盲区未暴露 |
| 4 | 行尾污染：`xcp_master.hpp`/`tests/CMakeLists.txt`/`command_codec_test.cpp` 被写成 LF、新建 `seed_key_test.cpp` 为 LF | 裸 LF 扫描 | 4 文件字节级转回 CRLF，复查 0；clang-format 后再复查仍 0 |

另：测试断言 `UdpTestSlaveFault.ResponseCounterStartsFreshAfterEachInjection`
在首轮全量运行中失败、单独重跑与后续两轮全量均通过——即设计文档 §16.4
**登记在案的已知偶发竞态**（满负载时间歇失败），非本批次引入，未做改动。

## 7. 未做与限制（如实登记）

1. **外部 DLL/SO 动态加载**（`XCP_ComputeKeyFromSeed` 的 LoadLibrary/dlopen
   加载机制）与 **A2L `SEED_AND_KEY_EXTERNAL_FUNCTION` 解析**：范围外，后续里程碑。
2. **自动解锁**：按 Q2 决策不做，`ERR_ACCESS_LOCKED` 仍由调用方决定时机（§6.4）。
3. **DAQ/STIM/PGM 对应的操作命令**仍不实现——本批次只解锁掩码位，不新增资源操作。
4. **测试 seed 为固定内容**（`SetSeedContent`）：确定性优先，不模拟随机 Seed。
5. **UdpTestSlave 的受保护语义是测试自定义**：让 UPLOAD/SHORT_UPLOAD 在存在
   未解锁受保护资源时返回 `ERR_ACCESS_LOCKED`（真实 ECU 行为依厂商而异，
   如 OpenBLT 对 PGM 读保护是返回全零而非报错）——这是测试设施行为，不是协议断言。
6. `SessionParameters.status.resource_protection` 在 Unlock 后是**旧快照**
   （`UnlockResult` 返回新值，权威刷新走 `QueryStatus()`），已写入 `UnlockResult`
   字段注释与设计文档 §14。
7. 本批次改动**尚未提交**，且工作区尚含**批次 6 的未提交改动**（`git status`
   实测：批次 6 实施记录仍为未跟踪状态）——建议一并提交以取得正式回滚点；
   `scripts/`
   仍在 `.gitignore` 内，本批次只修改了既有只读工具 1 个（`tools_audit_iface_gap.py`
   盲区修正），未新增工具。
8. 外部互操作验证（验收标准 15）仍为唯一跨批次缺口，与本批次无关。

## 8. 下一步建议（需用户批准其一后再动代码）

沿批次 6 §6 的选项表，扣除本批次已完成的 B 后：

| 选项 | 内容 | 备注 |
|---|---|---|
| **A. 外部互操作验证**（推荐） | 与真实 XCP Slave（CANape/ECU）或第三方实现对照 | 验收标准 15 唯一缺口；不改接口 |
| C. A2L 最小集 | IF_DATA 解析 + 记录布局 | 大，需先出设计 |
| D. XCP on CAN / CAN FD | 新增 Transport | 大，需硬件/模拟总线，先出设计 |
| E. Linux/macOS CI | 跨平台构建与测试流水线 | 若无外部环境则推荐此项：三平台声明仅在 Windows 验证过 |
