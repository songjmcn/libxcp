# B2 标定与 DAQ 控制面实施记录

- 批次：B2（DAQ 控制面：批量写 Entry + DTO 计数器属性）
- 依据：`code-plan/XCP_V1.5_测量标定修复与测试实施计划.md` §3 B2、§6 API 设计草案、§6.2 内部职责
- 规范：`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.pdf`（权威）/ `docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.md`（检索用）
- 状态：代码、测试、文档三件套完成；实测通过（见 §5.4）

---

## 1. 本批范围

| 命令 | 码 | 规范类别 | 本批交付 |
| --- | --- | --- | --- |
| WRITE_DAQ_MULTIPLE | 0xC7 | Optional | `[C7][n]` + 每条目 8 字节批量写，同一 ODT 内按 `MAX_CTO` 切包 |
| DTO_CTR_PROPERTIES | 0xC5 | Optional | 属性查询（纯查询 MODIFIER=0）与 DAQ_MODE 修改 |

另含计划 §7 明确要求 B2 修正的**失败状态策略**：静态/动态 DAQ 配置中途失败后，本端账本回滚不再被当成"Slave 也已回滚"，改为把受影响的 List 登记为"Slave 侧未知"并拒绝启动。

本批同时完成计划 §3 B2 第 2 项的后半段——**打通 `MeasurementSession` 的 DTO 计数器**（原实现硬编码 `layout.counter_enabled = false`）：会话层可按需请求计数器、以 `GET_DAQ_LIST_MODE` 回读确认布局、逐帧上报计数值与相对丢帧数，并进入 `MeasurementStatistics`（见 §3.6）。

配套业务入口：
- `XcpMaster::SetWriteDaqMultipleEnabled(bool)`（批量写显式开关，默认关闭）；
- `XcpMaster::QueryDtoCounterProperties(std::uint16_t)`（属性查询）；
- `XcpMaster::ConfigureDaqCounterEvent(std::uint16_t, DaqCounterMode)`（带前置判定的修改）；
- `XcpMaster::IsDaqListStateUnknown(std::uint16_t) const`（失败状态查询，见 §4.5）。

明确排除（不在本批）：
- **STIM 方向的计数器校验**（`STIM_MODE`/`STIM_CTR_CPY` 只做只读解析，`ConfigureDaqCounterEvent` 不改这两项）；
- RELATED_EVENT 的重绑定（`modify_related_event` 未开放，理由见 §4.3）；
- 事件通道与 DAQ List 的绑定推导（**经查为既有实现，不在本批新增**）：`adapter/a2l/measurement_runtime_profile.cpp` 的 `EventBoundMeasurementDatabase` 在绑定缺失/冲突时分别报 `测量缺少显式 DAQ 事件通道绑定` / `测量事件通道绑定缺失或存在歧义`，`BuildMeasurementRuntimeProfile` 还会用 `GET_DAQ_EVENT_INFO` 交叉核对周期与单位，从不静默回落通道 0；裸 A2L 适配器 `adapter/a2l/a2l_measurement_database.cpp:149` 的 `event_channel = 0` 是待包装的占位默认值，属设计分工。既有 16 个 `tests/measurement_adapter_test.cpp` 用例已覆盖。

---

## 2. 规范要点核对

### 2.1 WRITE_DAQ_MULTIPLE 的线格式与边界

规范 §7.5.4.6（`docs/...md:4606-4651`）：

- CTO = `[0xC7][n = NoDAQ][BIT_OFFSET][Size][Address DWORD][AddressExtension][Dummy]`，**每个条目固定 8 字节**；
- **最后一个条目也带 Dummy**（这一点最容易写漏；少一个字节整帧就短 1）；
- `n` 受 `MAX_CTO` 限制，因此约定的使用前提是 `MAX_CTO >= 10`（至少能放下一个条目 + 2 字节头）；
- 所有条目必须落在**同一 ODT** 内，不得跨 ODT；
- 错误处理与 WRITE_DAQ 一致，但**Slave 无法定位是哪个条目出错，整份配置视为无效**——这是本批"绝不偷切包到别的 ODT"的直接依据；
- 该命令一次推进 Slave 的 Entry 指针，超时后无从得知落点（见 §4.2）。

### 2.2 DTO_CTR_PROPERTIES 的请求与响应

规范 §7.5.4.15（`docs/...md:5257-5345`）：

- CTO = `[0xC5][MODIFIER][Event WORD][RELATED_EVENT WORD][MODE]`；
- MODIFIER：bit0 `MODIFY_RELATED_EVENT`、bit1 `MODIFY_DAQ_MODE`、bit2 `MODIFY_STIM_MODE`，**置位才采用命令值，否则保持当前值**；未使用的参数应写 0；
- MODE：bit0 `DAQ_MODE`（0 = `INSERT_COUNTER` 用相关事件 CTR、1 = `INSERT_STIM_COUNTER_COPY`）、bit1 `STIM_MODE`（0 = `DO_NOT_CHECK_COUNTER`、1 = `CHECK_COUNTER`）；
- 正响应 Table 164 = `[0xFF][PROPERTIES BYTE][Current Related Event Channel WORD][MODE BYTE]`，**共 5 字节，剥掉 PID 后是 4 字节**；
- PROPERTIES 位（Table 165/166）中 `*_FIXED` 与 `*_PRESENT` **语义相反**：
  - `RELATED_EVENT_FIXED`(0x01)/`DAQ_MODE_FIXED`(0x02)/`STIM_MODE_FIXED`(0x04)：**1 = 不可修改**；
  - `RELATED_EVENT_PRESENT`(0x08)/`DAQ_MODE_PRESENT`(0x10)/`STIM_MODE_PRESENT`(0x20)：**1 = 支持**；
  - `STIM_CTR_CPY_PRESENT`(0x40)：1 = **不能**保存 DTO CTR 副本（又是一个"1 = 否定"）；
  - `EVT_CTR_PRESENT`(0x80)：1 = 有周期计数器。
- 被查询的事件通道号**不在响应中回显**，必须由调用方入参回填。

---

## 3. 代码变更

### 3.1 协议层（`include/libxcp/protocol_types.hpp` / `src/protocol_types.cpp`）

| 位置 | 内容 |
| --- | --- |
| `protocol_types.hpp:125-126` | `CommandCode::WriteDaqMultiple = 0xC7`、`DtoCtrProperties = 0xC5` |
| `protocol_types.hpp`（`HasDaqMode` 之后） | `struct WriteDaqElement { bit_offset; size; address; extension; }` |
| 同上 | `enum class DtoCtrModifierBit { kNone, kModifyRelatedEvent=0x01, kModifyDaqMode=0x02, kModifyStimMode=0x04 }` |
| 同上 | `enum class DaqCounterMode { InsertCounter=0x00, InsertStimCounterCopy=0x01 }` |
| `protocol_types.hpp:285-299` | `enum class DtoCtrPropertiesBit`（8 位全量）与 `HasDtoCtrProperty` 声明 |
| `protocol_types.hpp:987-990` | `HasDtoCtrProperty` / `HasDtoCtrModifier` / `operator|` 的 constexpr 定义 |

`WriteDaqElement` 复用 `DaqEntrySpec` 的字段语义但不合并类型：`DaqEntrySpec` 是配置意图（进账本），`WriteDaqElement` 是线格式（一次命令内的单条），两者将来可能分叉（比如 Entry 增加 A2L 侧元数据）。

### 3.2 Codec（`include/libxcp/command_codec.hpp:260,278` / `src/command_codec.cpp`）

- `Bytes EncodeWriteDaqMultiple(std::span<const WriteDaqElement>) const;`
  - 帧头 `[0xC7][n]`，随后逐条 `bit_offset / size / addr / ext / dummy`；
  - 拒绝空列表、拒绝 `size == 0`、拒绝 `n > 255`；
  - **不校验 `MAX_CTO`**：Codec 不知道会话参数，容量判断留给 Executor/Master（见 §4.1）。
- `Bytes EncodeDtoCtrProperties(DtoCtrModifierBit, std::uint16_t event_channel, std::uint16_t related_event_channel, DaqCounterMode, bool stim_mode) const;`
  - 一致性守卫：未置 `MODIFY_DAQ_MODE` 却传非默认 `DAQ_MODE` → 拒绝；未置 `MODIFY_STIM_MODE` 却传 `true` → 拒绝；未置 `MODIFY_RELATED_EVENT` 却传非零相关事件号 → 拒绝。
  - 依据是规范原文"Command parameters which are unused, as their modifier is not set, should be written as 0"——把这些参数当成有效值传进来，说明调用方误解了协议，静默忽略比报错危险。

### 3.3 Parser（`include/libxcp/response_parser.hpp:160,487` / `src/response_parser.cpp:38-42`）

- `struct DtoCtrPropertiesResponse { std::uint8_t properties; std::uint16_t related_event_channel; std::uint8_t mode; };`
- `std::optional<DtoCtrPropertiesResponse> ParseDtoCtrProperties(BytesView) const;`
  - `properties = res_data[0]`、`related = ReadU16(1)`、`mode = res_data[3]`；
  - **短帧返回 `nullopt`，不补默认值**——MODE 补 0 会被上层读成"两个方向都不插计数器"，这是实质性的语义污染。
- `kDtoCtrPropertiesResMinSize = 4`。**该常量初版误写为 5，是被测试抓出来的真实缺陷**（见 §5.3）。

### 3.4 Executor（`include/libxcp/command_executor.hpp:428,442` / `src/command_executor.cpp`）

- `void ExecuteWriteDaqMultiple(std::span<const WriteDaqElement>)`
  - 守卫：`MaxCto() >= 10` 且 `elements.size() <= (max_cto - 2) / 8`，超出抛 `InvalidArgument`；
  - **不偷切分**：一次调用就是一帧，切包责任在 Master 层（只有它知道 ODT 边界）。
- `std::optional<DtoCtrPropertiesResponse> ExecuteDtoCtrProperties(...)`
  - 只有 `IsCmdUnknown` 转 `nullopt`；短帧抛 `MalformedPacket`。
- `HasUnknownOutcomeOnTimeout()` / `NonReplayableCommandName()` 增加 `WriteDaqMultiple`（理由见 §4.2）；DTO_CTR_PROPERTIES 幂等，不加。

### 3.5 Master（`include/libxcp/xcp_master.hpp` / `src/xcp_master.cpp`）

| 位置 | 内容 |
| --- | --- |
| `xcp_master.hpp`（`XcpMasterOptions` 之前） | `struct DtoCounterProperties`（含 `supports_dto_counter`/`daq_mode_fixed`/`related_event_fixed` 等判定结果字段） |
| `xcp_master.hpp:399` | `void SetWriteDaqMultipleEnabled(bool enabled);` |
| `xcp_master.hpp:523` | `std::optional<DtoCounterProperties> QueryDtoCounterProperties(std::uint16_t event_channel);` |
| `xcp_master.hpp:542` | `DtoCounterProperties ConfigureDaqCounterEvent(std::uint16_t event_channel, DaqCounterMode mode);` |
| `xcp_master.hpp:795` | 私有 `void WriteOdtEntries(std::uint16_t daq_list, std::uint8_t odt_number, const std::vector<DaqEntrySpec>& entries);` |
| `xcp_master.cpp:714-780` | `WriteOdtEntries` 实现：默认逐条；开启且 `entries > 1` 时按 `per_frame = (max_cto-2)/8` 切包，每包先 `SET_DAQ_PTR` 再 `WRITE_DAQ_MULTIPLE` |
| `xcp_master.cpp:849,1173` | `ConfigureDaqList`（STATIC）与 `ConfigureDaqListsDynamicImpl`（DYNAMIC）的逐 Entry 双重循环统一改调 `WriteOdtEntries` |
| `xcp_master.cpp:1575` | `QueryDtoCounterProperties` |
| `xcp_master.cpp:1606` | `ConfigureDaqCounterEvent`（先查询做前置判定，再只发 `MODIFY_DAQ_MODE`） |
| `xcp_master.cpp:1670` | `SetWriteDaqMultipleEnabled` |

失败状态策略（`xcp_master.hpp` 成员 `m_daq_lists_unknown_state_`）：

| 位置 | 内容 |
| --- | --- |
| `xcp_master.hpp`（`DaqConfigGeneration` 之后） | `[[nodiscard]] bool IsDaqListStateUnknown(std::uint16_t daq_list) const;` |
| `xcp_master.hpp`（`ConfigureDaqListsDynamicImpl` 之后） | 私有 `MarkDaqListStateUnknown` / `ClearDaqListStateUnknown` / `ClearAllDaqListStateUnknown` |
| `xcp_master.cpp`（`ClearDynamicTableBestEffort` 之前） | 三个辅助与公开访问器的实现 |
| `src/xcp_master.cpp`（`Connect`） | 新会话清空全部"未知"登记 |
| `src/xcp_master.cpp`（静态 `ConfigureDaqList`） | 两条 catch 各加 `MarkDaqListStateUnknown(spec.daq_list)`；成功尾部 `ClearDaqListStateUnknown` |
| `src/xcp_master.cpp`（`ConfigureDaqListsDynamicImpl`） | `ExecuteFreeDaq()` 成功后 `ClearAllDaqListStateUnknown()`（免费复位是唯一能证实整表清空的操作）；两条 catch 各为 `specs` 全部 List 登记未知 |
| `src/xcp_master.cpp`（`StartDaqList`/`StartDaqSync`） | 命中未知登记即抛 `InvalidState`，且**在下发之前**拒绝 |
| `src/xcp_master.cpp`（`ClearDaqList`） | CLEAR_DAQ_LIST 确认后 `ClearDaqListStateUnknown` |

账本一致性：`WriteOdtEntries` 的两条路径共用同一个 `record` lambda 登记 `DaqLedgerEntry`，因此**解码布局与发送形态无关**——这是 B2 最重要的正确性不变量，测试 `EnabledPathPacksWithinOdtAndKeepsLedgerIdentical` 直接锁它。

### 3.6 测量会话（DTO 计数器贯通）

| 位置 | 内容 |
| --- | --- |
| `include/libxcp/measurement/measurement_sample.hpp` | `MeasurementFrame` 新增 `std::optional<std::uint8_t> counter`（DTO 原始计数，仅 DTO_CTR 置位时存在）与 `std::uint8_t counter_lost_before`（相对上一帧丢失的步数，模 256 差值减一） |
| `include/libxcp/measurement/measurement_session.hpp` | `MeasurementStatistics` 新增 `counter_gaps`（不连续次数，含重复值）与 `counter_lost`（推算丢帧总数）；公开 `void SetDtoCounterEnabled(bool enabled);` |
| `include/libxcp/measurement/measurement_planner.hpp` / `src/measurement/measurement_planner.cpp` | 构造函数尾部新增 `bool dto_counter = false, std::size_t counter_header_bytes = 0`；装箱时 `cur.spec.dto_counter = m_dto_counter_`，并按 `counter_header_bytes` 扣减 ODT 可用长度 |
| `src/measurement/measurement_session.cpp`（`Prepare`） | 计算 `counter_header_bytes = (m_dto_counter_enabled_ && !IdentificationFieldMergesCounter(m_id_field_type_)) ? 1U : 0U`，随规划结果一起下发 `SET_DAQ_LIST_MODE`；随后对每个 `DaqListSpec` 调 `QueryDaqListMode` 做**回读确认**，只有 `kDtoCounter` 位确实置起才置 `m_counter_active_` |
| `src/measurement/measurement_session.cpp`（`WorkerLoop`） | `layout.counter_enabled = m_counter_active_`（取代原 `= false` 硬编码）；逐 `daq_list` 维护 `last_counters`，按模 256 差值判定连续/重复/丢帧；`Statistics()` 回填两个新计数 |

两处关键判断：

1. **"是否请求计数器"与"是否多占 1 字节"分开传参。** `RelativeWordAligned` 下 CTR 取代 FILL 字节、不占额外长度（`IdentificationFieldMergesCounter`），把它们合成一个布尔会让该布局凭空多扣一字节、把单帧变量挤进第二个 ODT。测试 `MergedCounterTakesNoExtraByte` 锁这一点。
2. **回读不通过就不开计数器。** 请求了 DTO_CTR 但 Slave 未置位（或干脆不支持 `GET_DAQ_LIST_MODE`）时直接抛异常，而不是"按计数器布局去解码没有计数器的帧"——后者会把后面的时间戳与样本整体错位一字节以上，属于静默数据损坏。

计数器只上报、不重排、不补偿：缺口的成因（总线错误、被覆盖、迟到）仅凭计数器无法区分，库不做猜测。

---

## 4. 关键设计决策

### 4.1 批量写默认关闭，且容量判断放在 Master 层

`WRITE_DAQ_MULTIPLE` 是 Optional 命令，Slave 不支持时必须能退。默认关闭的理由与 B1 块传输同源：Optional 命令的可用性不能从"宣告能力"推断，只能从"实际成功过"推断。

三层职责切分：
1. **Codec** 只负责线格式，不知 `MAX_CTO`；
2. **Executor** 用 `MaxCto()` 做单帧容量硬校验（`>= 10` 且条目数不超），越界抛出而不是静默降级——因为调用方已经明确要求批量；
3. **Master** 决定切包点，因为只有它知道"同一 ODT"这个约束的边界在哪。

`MAX_CTO < 10` 时抛 `InvalidArgument` 而不回退逐条：开启开关是调用方的显式意图，静默回退会让"我开了批量写"这句话在我的日志里成立、在线上不成立。

### 4.2 WRITE_DAQ_MULTIPLE 计入"超时结果未知"

`HasUnknownOutcomeOnTimeout()` 加入该命令，理由：它一次写多条并推进 Slave 的 Entry 指针，超时后无从得知 Slave 已经接受了前几条还是整份拒绝。此时判 `OperationOutcomeUnknown`，禁止自动重发——重发会覆盖错误的 Entry（Slave 指针已前移）。

对比 `DTO_CTR_PROPERTIES`：它是幂等的属性赋值，超时重发语义等价，故不加。

### 4.3 不开放 RELATED_EVENT 重绑定

规范允许通过 `MODIFY_RELATED_EVENT` 改相关事件通道，本批只读不写。原因：
- 改相关事件会连带改变 CTR 来源，属于影响测量正确性的强语义操作；
- 客户端没有足够信息判断"目标事件是否已配置为可作 CTR 源"，`RELATED_EVENT_FIXED` 只是必要条件；
- 需要等 B3（时间关联）把事件/时钟模型建起来后再一并评估。

`ConfigureDaqCounterEvent` 因此固定只发 `kModifyDaqMode`，测试 `ConfigureSendsOnlyModifyDaqModeBit` 锁死 `MODIFIER == 0x02`。

### 4.4 `*_FIXED` 与 `*_PRESENT` 必须在类型层隔开

两者数值不重叠，但语义方向相反（1 = 不可改 vs 1 = 支持）。用同一个 `enum class DtoCtrPropertiesBit` 承载位定义、用 `HasDtoCtrProperty(bits, mask)` 读，避免裸整数比较；同时测试 `FixedAndPresentBitsAreOppositeSemantics` 显式断言 `kDaqModeFixed == 0x02`、`kDaqModePresent == 0x10`，并注明**请求侧 `kModifyDaqMode` 恰好也是 0x02 但方向相反**，禁止把响应 PROPERTIES 当请求 MODIFIER 复用。

### 4.5 失败状态策略：回滚本端账本 ≠ 回滚 Slave

计划 §7 原文点明了这个陷阱：*"未确认写自动重发、DAQ 账本'本地回滚'即视为对端回滚"* 是被禁止的模式。B2 之前的实现恰好踩在这条线上——静态 `ConfigureDaqList` 失败时只做 `rollback()` 清账本，然后打一行 `local ledger rolled back`；账本里看不见这个 List 了，但 Slave 上 `CLEAR_DAQ_LIST` 已执行、部分 `WRITE_DAQ` 可能已生效、`SET_DAQ_LIST_MODE` 可能根本没下发。

危险不在"配置失败"本身（异常已上抛），而在**之后的语义**：

- `StartDaqList` 只查账本 → 账本为空 → 报"未在本端登记"。这个提示会让人以为"只是没配好，配一下就行"，而真实的 Slave 状态是"半成品列表"；
- 若该 List 上一次配置仍在 Slave 里（重配失败的情形），账本残留旧条目不空 → `StartDaqList` 会**放行**，然后按旧布局解码一批按半新半旧配置产出的 DTO。

因此引入按 List 粒度的"Slave 侧状态未知"登记 `m_daq_lists_unknown_state_`：

| 事件 | 对登记的 effect |
| --- | --- |
| 静态/动态配置事务失败 | 登记受影响 List 为未知 |
| `ConfigureDaqList` 整表成功 | 撤销该 List 的登记（成功路径以 `CLEAR_DAQ_LIST` 开头，且全部 Entry 与 mode 都被确认） |
| `ClearDaqList` 确认 | 撤销该 List 的登记（CLEAR 是该 List 唯一的"回到已知空态"操作） |
| `FREE_DAQ` 确认 | 撤销**全部**登记（整表复位，这是唯一能证实"半成品都不存在了"的证据） |
| 重新 `Connect` | 撤销全部登记（对端可能已重启，旧登记失去证据） |
| `StartDaqList` / `StartDaqSync` | **拒绝**而非放行，且拒绝发生在下发之前 |

这里刻意不做自动修复（不回滚式 `CLEAR_DAQ_LIST` 重试）：一次失败的写之后立刻再发一条写，如果第一条其实部分成功，第二条只会让状态更糊。修复必须是调用方的显式决定。

### 4.6 `SetWriteDaqMultipleEnabled` 与 `SetBlockModeEnabled` 保持独立开关

两个开关分别对应两个 Optional 命令，可以独立成立（有的 Slave 支持批量写但不支持块下载）。刻意不做成一个 `SetCompatibilityLevel()` 之类的总开关——那会把"哪一条 Optional 命令可用"的实测信息压缩成一个无法诊断的布尔。

---

### 4.7 计数器以"回读确认"为生效前提

`SET_DAQ_LIST_MODE` 收到肯定应答只证明命令被接受，不证明 DTO_CTR 位真的生效——Slave 可能只接受它支持的位子集。因此 `Prepare()` 在下发后追加一次 `GET_DAQ_LIST_MODE`：

- 命令不支持（`nullopt`）→ 抛 `UnsupportedFeature`，消息说明"已要求 DTO 计数器但无法确认布局"；
- 回读位未置 → 抛 `ProtocolError`，消息点名是哪个 DAQ List。

不做"先按计数器解、错了再降级"的兜底：计数器存在与否改变整帧字段偏移，错了不会报错，只会输出一帧看似合法的错误物理量。

顺带定下一条类型纪律：请求侧的 `ALTERNATING` 与响应侧的 `SELECTED` 数值都是 `0x01`，语义却不同，故另立 `DaqListStatusBit`（B0 已引入），使"把回读值直接写回请求"在类型层就通不过。

### 4.8 计数器只上报缺口，不重排也不补偿

丢帧数由模 256 差值减一得出，`counter_gaps` 记录"不连续了几次"（含重复值 `delta == 0`），`counter_lost` 记录"推算一共丢了几帧"。库不据此重排样本、也不插占位帧——把两帧的样本按推断的时隙对齐，等于替 Slave 编造它没有发出的事实。调用方拿到 `counter_lost_before` 后可自行决定丢弃该帧或降级处理。

---

## 5. 测试

### 5.1 新增测试文件

`tests/daq_control_plane_test.cpp`（24 个 TEST，注册于 `tests/CMakeLists.txt:83`）。

装置：`ControlPlaneSlave`（脚本化模拟 Slave，未列出的命令一律回 `ERR_CMD_UNKNOWN`）+ `CodecFixture`（纯 Codec/Parser 断言，不依赖会话）+ `BlockHarness`（CONNECT + 构造被测 `XcpMaster`）。

`ControlPlaneSlave` 的 CONNECT 响应固定为 `Res({0x15, 0x00, max_cto, 0x08, 0x00, 0x10, 0x10})`：`comm_mode_basic = 0x00` 表示 Intel 字节序、AG=Byte，且**不声明** SLAVE_BLOCK_MODE / OPTIONAL——这样 CONNECT 阶段不会触发 GET_COMM_MODE_INFO，控制面断言只看到 B2 自己的命令。

### 5.2 用例分组

| 组 | 用例 | 锁什么 |
| --- | --- | --- |
| Codec | `WriteDaqMultipleFrameLayout` | 18 字节、两条目、**末条仍有 Dummy** |
| Codec | `WriteDaqMultipleMotorolaOrder` | 地址大端落盘、Dummy 位置不变 |
| Codec | `WriteDaqMultipleRejectsEmptyAndZeroSize` | 空列表 / `size==0` 拒绝 |
| Codec | `DtoCtrPropertiesQueryFrame` | 纯查询 `{C5,00,07,00,00,00,00}` |
| Codec | `DtoCtrPropertiesModifyDaqModeFrame` | 只置 bit1 时 `{C5,02,03,00,00,00,01}` |
| Codec | `DtoCtrPropertiesRejectsInconsistentArguments` | 未置 MODIFIER 却传值 → 拒绝 |
| Parser | `DtoCtrPropertiesResponseParse` | PROPERTIES/related/mode 三字段正确切分 |
| Parser | `DtoCtrPropertiesResponseMotorola` | 大端 WORD |
| Parser | `DtoCtrPropertiesShortResponseIsNullopt` | 3 字节 → `nullopt`；4 字节完整可用 |
| 位语义 | `FixedAndPresentBitsAreOppositeSemantics` | FIXED/PRESENT 相反、请求响应同值不同义 |
| Master | `DefaultPathUsesSingleWriteDaqPerEntry` | 默认 3 Entry → 3 WRITE_DAQ、0 批量 |
| Master | `EnabledPathPacksWithinOdtAndKeepsLedgerIdentical` | 一帧 26 字节、**账本与逐条路径完全一致** |
| Master | `EnabledPathSplitsAcrossFramesWithinSameOdt` | 5 Entry → 2 帧（26+18），第 2 帧 `SET_DAQ_PTR` 指向 entry=3 |
| Master | `EnabledPathRefusesWhenMaxCtoTooSmall` | `MAX_CTO=8` + 2 Entry → 抛，且**一帧都没发出去** |
| Master | `QueryDtoCounterPropertiesDecodesBits` | 事件号由入参回填、各位解码正确 |
| Master | `QueryUsesPureQueryModifier` | 查询帧 `MODIFIER == 0` |
| Master | `QueryReturnsNulloptOnlyWhenCommandUnknown` | CmdUnknown → `nullopt`；OutOfRange → 抛 |
| Master | `ConfigureRejectsWhenCounterUnsupported` | `DAQ_MODE_PRESENT==0` → `UnsupportedFeature`，且只发 1 帧（查询） |
| Master | `ConfigureRejectsWhenDaqModeFixed` | `DAQ_MODE_FIXED==1` → `UnsupportedFeature` |
| Master | `ConfigureSendsOnlyModifyDaqModeBit` | 2 帧：先查后改，`MODIFIER==0x02`、`MODE bit0==1` |
| 失败态 | `StaticConfigureFailureMarksListStateUnknown` | 写失败后账本已空，但 List 标未知、未涉及的 List 1 不受影响 |
| 失败态 | `StartRejectsListWithUnknownSlaveState` | 拒绝为 `InvalidState`，且 `START_STOP_DAQ_LIST` **一帧都没发** |
| 失败态 | `ReconfigureClearsUnknownSlaveState` | 故障恢复后整表重配 → 登记撤销、账本 2 条、启动放行 |
| 失败态 | `ClearDaqListClearsUnknownSlaveState` | 显式 CLEAR 确认 → 登记撤销 |

### 5.3 测试抓到的真实缺陷

`ParseDtoCtrProperties` 的最小长度常量初版写成 `5`，导致合法响应（剥掉 PID 后 4 字节）被判短帧 → 抛 `MalformedPacket`，7 个用例连锁失败：

```
unknown file: error: C++ exception with description
"DTO_CTR_PROPERTIES 响应长度不足（RES 数据 4 字节，期望 >= 5）" thrown in the test body.
[  FAILED  ] DaqControlPlane.QueryUsesPureQueryModifier
...
[  FAILED  ] DaqControlPlane.ConfigureRejectsWhenCounterUnsupported
  e.Category()  Which is: 4-byte object <04-00 00-00>   // MalformedPacket
  ErrorCategory::UnsupportedFeature
                Which is: 4-byte object <06-00 00-00>
```

按 Table 164 核对（`[0xFF][PROPERTIES][WORD][MODE]` = 5 字节含 PID）后改为 `4`。**这正是"规范表格必须回原文核对、不能凭印象记字节数"的实例**；同时把该用例补成"3 字节 nullopt / 4 字节完整"双向断言，防止下次改常量时再次摇摆。

### 5.3.1 测试抓到的第二处问题：模拟 Slave 的 START 响应缺 FIRST_PID

`ReconfigureClearsUnknownSlaveState` 需要真的启动一次列表，才暴露 `ControlPlaneSlave` 把 `START_STOP_DAQ_LIST` 正响应写成 `Res({})`（0 字节），而解析器要求至少 1 字节 FIRST_PID：

```
unknown file: error: C++ exception with description
"START_STOP_DAQ_LIST 响应长度不足（RES 数据 0 字节，期望 >= 1）" thrown in the test body.
```

前 20 个用例都不启动列表，所以这个错误一直藏着。修的是测试装置而不是产品代码——但这恰好说明"只测到下发、不测到启动"的覆盖盲区是真实存在的。

### 5.4 实测结果

命令面（本批前半段，加入计数器贯通之前）：

```
$ ./cmake-build-release/tests/libxcp_tests.exe --gtest_filter='DaqControlPlane*'
[----------] 24 tests from DaqControlPlane (22 ms total)
[  PASSED  ] 24 tests.

$ ./cmake-build-release/tests/libxcp_tests.exe
[==========] 896 tests from 157 test suites ran. (46705 ms total)
[  PASSED  ] 895 tests.
[  SKIPPED ] 1 test, listed below:
[  SKIPPED ] AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8
```

计数器贯通后（本批最终状态）：

```
$ ./cmake-build-release/tests/libxcp_tests.exe --gtest_filter='MeasurementSessionCounter*:MeasurementPlannerCounter*'
[==========] 12 tests from 2 test suites ran. (26 ms total)
[  PASSED  ] 12 tests.

$ ./cmake-build-release/tests/libxcp_tests.exe
[==========] 908 tests from 159 test suites ran. (45755 ms total)
[  PASSED  ] 907 tests.
[  SKIPPED ] 1 test, listed below:
[  SKIPPED ] AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8
```

- 命令面 24/24 通过；计数器贯通 12/12 通过；
- 全量 **908 用例、907 通过、0 失败、1 跳过**（该用例在 B0 之前即为 skip，与本批无关）；
- 构建 `EXIT=0`（`.tmp-build-tests.bat` → `b2c_build.log`）；
- 中间态也如实记录：加入失败状态策略前 20/20 与 892 全量通过；补登记逻辑后 23/24（模拟 Slave 缺 FIRST_PID）；修正装置后 24/24 与 896 全量通过；计数器首版编译失败 8 处（`DtoPacket` 不能从 initializer list 转换）；首轮运行 12 例中 4 失败（见 §5.5）；修正后 12/12 与 908 全量通过。

---

### 5.5 计数器贯通用例（`tests/measurement_counter_test.cpp`）

12 个用例，两组：

| 组 | 用例 | 锁什么 |
| --- | --- | --- |
| Planner | `SetsCounterBitAndReservesOneByte` | 非合并布局下 `dto_counter` 置位且可用长度减 1 |
| Planner | `MergedCounterTakesNoExtraByte` | `RelativeWordAligned` 下 6+2 字节变量恰满一帧（若误扣 1B 会开第二个 ODT） |
| Planner | `CounterByteAloneCannotExceedMaxDto` | 计数器字节不足以让变量超限时仍拒绝装箱 |
| Session | `RejectedWhenSlaveCannotReadListMode` | 不支持 `GET_DAQ_LIST_MODE` → `UnsupportedFeature` |
| Session | `RejectedWhenReadbackLacksCounterBit` | 回读位未置 → `ProtocolError` |
| Session | `PrepareRequestsCounterAndConfirmsByReadback` | 请求位确实下发且回读后 `m_counter_active_` 生效 |
| Session | `DefaultPathDoesNotQueryListMode` | 不请求计数器时**不**多发查询（零回归保证） |
| Session | `FramesCarryCounterAndNoGap` | 连续计数 → 帧带 `counter`、`counter_lost_before == 0`、两项统计为 0 |
| Session | `GapIsReportedAsLostFrames` | 跨步计数 → 按差值减一报丢帧 |
| Session | `DuplicateCounterCountsGapWithoutLoss` | 重复值 → gap+1、lost 不变 |
| Session | `SetterRejectedWhileRunning` | 运行中改设置抛 `InvalidState` |
| Session | `LegacyLayoutStillDecodesWithoutCounter` | 未启用的旧布局照常解码（`counter` 为空） |

**本组抓到两个真实缺陷：**

1. **模 256 差值的基准值选错。** 初版把 `expected_counters` 记成 `当前值 + 1`，再用 `当前值 - 期望值` 求 delta：重复帧（`0x30` 后又是 `0x30`）算出 `delta = 255`，被误判成"丢了 254 帧"。三例 session 测试因此同时失败。改为记**上一次观测值**后，`delta == 0` 才真正表示重复。
2. **测试自身的尺寸算错。** `MergedCounterTakesNoExtraByte` 原用 8+1 字节变量，在 `MAX_DTO=12`、识别字段 4B（可用 8B）下天然要开第二个 ODT，与"CTR 不占额外字节"这条被测性质无关。改成 6+2 字节恰满一帧后才有判别力。

## 6. 边界与遗留

1. **相关事件重绑定未开放**（§4.3）。`DtoCtrPropertiesBit::kRelatedEventFixed` 只用于拒绝修改，不用来选择新事件。
2. **STIM 侧计数器属性只读**。`stim_check_counter` 能查出来但 `ConfigureDaqCounterEvent` 不会改它——本项目的刺激（STIM）方向整体不在范围内。
3. **"事件通道 → DAQ List"的绑定校验在 A2L 适配层，不在 Master 层**。`EventBoundMeasurementDatabase` 已拒绝"缺绑定"和"绑定歧义"，并用 `GET_DAQ_EVENT_INFO` 交叉核对周期与单位；但 `XcpMaster::ConfigureDaqList` 本身不核对 `DaqListSpec::event_channel` 是否真实存在——直接用 Master API 的调用方需要自己先 `QueryDaqEventInfo`。这是有意的分工：Master 不该假设调用方手里一定有 A2L。
4. **`MAX_CTO` 只做上限校验，不做"对端实际接受几条"的探测**。Slave 可能比 `MAX_CTO` 更保守，此时整份配置无效（规范原话），Master 会把异常上抛，调用方需自行回退到 `SetWriteDaqMultipleEnabled(false)`。
5. **批量写的超时恢复只到"标记未知"**。`OperationOutcomeUnknown` 之后不做任何自动重配，调用方须重新 `ConfigureDaqList` 全量下发。
5.1 **失败状态登记是纯本端记录，不主动探测 Slave**。`IsDaqListStateUnknown` 为 true 时本库不会去问 Slave"你那边现在是什么"，因为没有任何 XCP 命令能一次性证明某个 List 的完整内容（`READ_DAQ` 只能逐 Entry 读，且读回来的 Entry 数也无法证明没有多余 ODT）。该标志只能由成功操作撤销，属于保守但不可绕过的设计。
5.2 **`ConfigureDaqListsDynamic` 失败后登记的是"本次 specs 里的全部 List"**，而不是"已下发到的那个"。因为 `ClearDynamicTableBestEffort()` 的二次 FREE 是 best-effort，无法确定哪些列表还残留。
6. **`bit_offset` 未在本批新增测试覆盖**。既有 `WRITE_DAQ` 已支持 `kDaqBitOffsetNone` 与非零 bit offset，批量写复用了同一 `EncodeWriteDaq` 的字段布局；但"批量写 + 非零 bit_offset"组合没有专门用例，归入后续补测。

7. **计数器只覆盖 DTO（测量）方向**。`MeasurementSession` 不开 STIM，因此 `INSERT_STIM_COUNTER_COPY` 与 `CHECK_COUNTER` 依旧只在 `DtoCounterProperties` 里可读、不可用。
8. **计数器缺口不参与时间关联**。`counter_lost_before` 告诉调用方"这一帧之前少了 N 帧"，但库不据此推算时间偏移——没有可靠时间基准时那样做只是把丢帧误差换成插值误差，属 B3 范围。
9. **计数器统计是进程级累加，不按 DAQ List 分开**。`counter_gaps`/`counter_lost` 汇总了所有列表；逐列表视图需要扩展 `MeasurementStatistics` 结构，本批未做。

---

## 7. 自审

- [x] 每条新增命令都回归了规范原文表格（CTO/RES 字段位置、Dummy 位置、未用参数写 0）。
- [x] 未实现的规范能力（RELATED_EVENT 重绑定、STIM 计数器）在文档里显式标注，不假装支持。
- [x] 新增公开 API 均带中文 doxygen；`DtoCounterProperties` 各字段注释说明"这是 Slave 上报的事实还是本地推断"。
- [x] 默认值保守：两个 Optional 命令的开关默认 `false`，行为与 B1 之前完全一致（全量 892 用例零回归）。
- [x] 测试覆盖两种字节序、`MAX_CTO` 边界、能力缺失、属性被固定四类失败路径。
- [x] 未修改 `thirdparty/`。
- [x] 缺陷记录诚实：§5.3 的常量错误是测试实际抓到的，不是事后补写。
- [x] 失败状态策略落地：本端回滚不再被当作 Slave 回滚，受影响 List 被显式登记并拒绝启动（§4.5），四类撤销路径都有用例。
- [x] 计数器贯通以"回读确认"为生效前提（§4.7），未确认即拒绝而非按猜测解码；不请求计数器时零额外命令（`DefaultPathDoesNotQueryListMode` 锁住）。
- [x] 计数器只上报缺口、不重排不补偿（§4.8），理由写在文档里而不是留给读者猜。
- [x] 计数器相关缺陷诚实记录：两处都是测试实际抓到的（§5.5），其中 delta 基准值错误会导致丢帧数完全失真。
- [ ] 已知未覆盖：批量写 + 非零 `bit_offset` 组合（§6.6）。