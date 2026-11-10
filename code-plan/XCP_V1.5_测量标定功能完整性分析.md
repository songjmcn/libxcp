# XCP V1.5 测量与标定功能完整性分析

## 文档说明

本文档基于 ASAM MCD-1 XCP V1.5.0 协议规范（`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.md`），对 libxcp 当前代码实现的测量（DAQ）和标定（Calibration）功能进行完整性检查，识别已实现功能、缺失功能及其影响范围。

**检查范围：** 忽略其他功能（如编程、时间同步），仅聚焦测量与标定。

**检查日期：** 2026-11-03

### 纠偏记录（B0 批次修订）

初版本文档存在以下规范事实错误，已按 `docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.pdf`（权威原文，`desc/../md` 仅为可检索转换版）与其中 `Table 25`/`Table 27`/`Table 28` 更正：

1. **`DOWNLOAD_NEXT` 命令码**：初版写 `0xEF/0xCF`，实际 **`0xEF`**；`0xCF` 是 `PROGRAM_RESET`（编程命令，不在本文范围）。
2. **`DOWNLOAD_MAX` 命令码**：初版写 `0xEE/0xCE`，实际 **`0xEE`**；`0xCE` 是 `GET_PGM_PROCESSOR_INFO`（编程命令，不在本文范围）。
3. **`DOWNLOAD_MAX` 语义**：初版写“变长下载”，实际是**固定满帧下载**——CTO 无 SIZE 字段，Slave 固定按 `(MAX_CTO-1)/AG` 元素接收，不是变长。
4. **`SHORT_DOWNLOAD`(0xED)、`MODIFY_BITS`(0xEC)、`READ_DAQ`(0xDB) 的支持等级**：初版标为 Mandatory，`Table 25`/`Table 27` 实为 **optional**。真正 mandatory 的标定命令只有 `DOWNLOAD`(0xF0)，真正 mandatory 的 DAQ 基础命令是 `SET_DAQ_PTR`/`WRITE_DAQ`/`SET_DAQ_LIST_MODE`/`START_STOP_DAQ_LIST`/`START_STOP_SYNCH`/`CLEAR_DAQ_LIST`。
5. **综合评分 85%/90%**：这两个数字没有可复算的定义（分母是什么、权重如何），不构成验收依据。本次改为**按“能力→协商→配置→数据→失败恢复→测试证据”逐项列证据状态**，并明确“命令已编码”不等于“高层测量/标定完整”。

---

## 一、XCP V1.5 测量与标定命令规范概览

### 1.1 标定命令组（§7.5.2）

| 命令码 | 命令名称 | 类型 | 功能描述 |
|--------|----------|------|----------|
| 0xF0 | DOWNLOAD | Mandatory for CAL/PAG | 固定长度下载，最多 (MAX_CTO-2)/AG 元素 |
| 0xEF | DOWNLOAD_NEXT | Optional | Block Mode 续帧下载，无需重发地址；首帧携块总元素数，续帧携剩余元素数，仅末帧应答 |
| 0xEE | DOWNLOAD_MAX | Optional | 固定满帧下载，无 SIZE 字段，固定按 (MAX_CTO-1)/AG 元素接收 |
| 0xED | SHORT_DOWNLOAD | Optional | 短下载（含地址，最多 (MAX_CTO-8)/AG 元素） |
| 0xEC | MODIFY_BITS | Optional | 原子位修改（32-bit AND/XOR） |
| 0xEB | SET_CAL_PAGE | Optional | 切换标定页 |
| 0xEA | GET_CAL_PAGE | Optional | 查询当前标定页 |
| 0xE9 | GET_PAG_PROCESSOR_INFO | Optional | 查询分页处理器信息 |
| 0xE8 | GET_SEGMENT_INFO | Optional | 查询 Segment 信息（变长响应） |
| 0xE7 | GET_PAGE_INFO | Optional | 查询 Page 访问属性 |
| 0xE6 | SET_SEGMENT_MODE | Optional | 设置 Segment FREEZE 模式 |
| 0xE5 | GET_SEGMENT_MODE | Optional | 读取 Segment 当前模式 |
| 0xE4 | COPY_CAL_PAGE | Optional | 复制标定页 |

### 1.2 DAQ 配置命令组（§7.5.4）

| 命令码 | 命令名称 | 类型 | 功能描述 |
|--------|----------|------|----------|
| 0xE3 | CLEAR_DAQ_LIST | Mandatory for DAQ/STIM | 清除单个 DAQ List |
| 0xE2 | SET_DAQ_PTR | Mandatory for DAQ/STIM | 设置 DAQ 指针 |
| 0xE1 | WRITE_DAQ | Mandatory for DAQ/STIM | 写单个 ODT Entry（支持 bit_offset） |
| 0xC7 | WRITE_DAQ_MULTIPLE | Optional | 批量写 ODT Entry（V1.5 新增） |
| 0xE0 | SET_DAQ_LIST_MODE | Mandatory for DAQ/STIM | 配置 DAQ List 模式（时间戳/计数器/优先级等） |
| 0xDF | GET_DAQ_LIST_MODE | Optional | 查询 DAQ List 模式 |
| 0xDE | START_STOP_DAQ_LIST | Mandatory for DAQ/STIM | 启动/停止单个 DAQ List |
| 0xDD | START_STOP_SYNCH | Mandatory for DAQ/STIM | 同步启动/停止已选中的 DAQ Lists |
| 0xDC | GET_DAQ_CLOCK | Optional | 查询 Slave 时钟当前值（时间关联的原始观测之一，单条命令不构成同步） |
| 0xDB | READ_DAQ | Optional | 回读 ODT Entry 配置 |

### 1.3 DAQ 查询命令组（§7.5.4）

| 命令码 | 命令名称 | 类型 | 功能描述 |
|--------|----------|------|----------|
| 0xDA | GET_DAQ_PROCESSOR_INFO | Optional | 查询 DAQ 处理器能力（必备的运行时证据） |
| 0xD9 | GET_DAQ_RESOLUTION_INFO | Optional | 查询 Entry 粒度与时间戳模式 |
| 0xD8 | GET_DAQ_LIST_INFO | Optional | 查询单个 DAQ List 容量与固定事件 |
| 0xD7 | GET_DAQ_EVENT_INFO | Optional | 查询事件通道运行时信息 |

### 1.4 动态 DAQ 分配命令组（§7.5.4.16-21）

| 命令码 | 命令名称 | 类型 | 功能描述 |
|--------|----------|------|----------|
| 0xD6 | FREE_DAQ | Mandatory for DAQ/STIM | 释放全部动态 DAQ List |
| 0xD5 | ALLOC_DAQ | Mandatory for DAQ/STIM | 分配 DAQ Lists |
| 0xD4 | ALLOC_ODT | Mandatory for DAQ/STIM | 分配 ODT |
| 0xD3 | ALLOC_ODT_ENTRY | Mandatory for DAQ/STIM | 分配 ODT Entry 槽 |

### 1.5 V1.5 新增/增强命令

| 命令码 | 命令名称 | 功能描述 |
|--------|----------|----------|
| 0xC7 | WRITE_DAQ_MULTIPLE | 批量写 ODT Entry，减少 CRO 往返次数 |
| 0xC5 | DTO_CTR_PROPERTIES | 查询/修改 Event Channel 的 DTO 计数器属性 |
| 0xC0/0x01 | SET_DAQ_PACKED_MODE | 设置 Packed 模式（element-grouped/event-grouped） |
| 0xC0/0x02 | GET_DAQ_PACKED_MODE | 查询当前 Packed 模式配置 |

---

## 二、当前代码实现状态

### 2.1 已实现的核心功能

#### 2.1.1 命令编码器（`include/libxcp/command_codec.hpp`）

**已实现的标定命令（11 个）：**
- ✅ `EncodeDownload()` - DOWNLOAD (0xF0)
- ✅ `EncodeShortDownload()` - SHORT_DOWNLOAD (0xED)
- ✅ `EncodeModifyBits()` - MODIFY_BITS (0xEC)
- ✅ `EncodeSetCalPage()` - SET_CAL_PAGE (0xEB)
- ✅ `EncodeGetCalPage()` - GET_CAL_PAGE (0xEA)
- ✅ `EncodeGetPagProcessorInfo()` - GET_PAG_PROCESSOR_INFO (0xE9)
- ✅ `EncodeGetSegmentInfo()` - GET_SEGMENT_INFO (0xE8)
- ✅ `EncodeGetPageInfo()` - GET_PAGE_INFO (0xE7)
- ✅ `EncodeSetSegmentMode()` - SET_SEGMENT_MODE (0xE6)
- ✅ `EncodeGetSegmentMode()` - GET_SEGMENT_MODE (0xE5)
- ✅ `EncodeCopyCalPage()` - COPY_CAL_PAGE (0xE4)

**已实现的 DAQ 配置命令（7 个）：**
- ✅ `EncodeClearDaqList()` - CLEAR_DAQ_LIST (0xE3)
- ✅ `EncodeSetDaqPtr()` - SET_DAQ_PTR (0xE2)
- ✅ `EncodeWriteDaq()` - WRITE_DAQ (0xE1)，支持 bit_offset 位字段
- ✅ `EncodeSetDaqListMode()` - SET_DAQ_LIST_MODE (0xE0)
- ✅ `EncodeStartStopDaqList()` - START_STOP_DAQ_LIST (0xDE)
- ✅ `EncodeStartStopSynch()` - START_STOP_SYNCH (0xDD)
- ✅ `EncodeReadDaq()` - READ_DAQ (0xDB)

**已实现的 DAQ 查询命令（4 个）：**
- ✅ `EncodeGetDaqProcessorInfo()` - GET_DAQ_PROCESSOR_INFO (0xDA)
- ✅ `EncodeGetDaqResolutionInfo()` - GET_DAQ_RESOLUTION_INFO (0xD9)
- ✅ `EncodeGetDaqListInfo()` - GET_DAQ_LIST_INFO (0xD8)
- ✅ `EncodeGetDaqEventInfo()` - GET_DAQ_EVENT_INFO (0xD7)

**已实现的动态 DAQ 分配命令（4 个）：**
- ✅ `EncodeFreeDaq()` - FREE_DAQ (0xD6)
- ✅ `EncodeAllocDaq()` - ALLOC_DAQ (0xD5)
- ✅ `EncodeAllocOdt()` - ALLOC_ODT (0xD4)
- ✅ `EncodeAllocOdtEntry()` - ALLOC_ODT_ENTRY (0xD3)

**已实现的内存传输命令（3 个）：**
- ✅ `EncodeSetMta()` - SET_MTA (0xF6)
- ✅ `EncodeUpload()` - UPLOAD (0xF5)
- ✅ `EncodeShortUpload()` - SHORT_UPLOAD (0xF4)

#### 2.1.2 响应解析器（`include/libxcp/response_parser.hpp`）

**已实现的响应解析方法：**
- ✅ 标定响应：`ParseGetCalPage()` / `ParseGetPagProcessorInfo()` / `ParseGetSegmentInfo()` / `ParseGetPageInfo()` / `ParseGetSegmentMode()`
- ✅ DAQ 响应：`ParseGetDaqProcessorInfo()` / `ParseGetDaqResolutionInfo()` / `ParseGetDaqListInfo()` / `ParseGetDaqEventInfo()` / `ParseReadDaq()`
- ✅ 连接响应：`ParseConnect()` / `ParseGetCommModeInfo()` / `ParseGetId()` / `ParseGetVersion()`
- ✅ 内存传输响应：`ParseUpload()` / `ParseShortUpload()`

#### 2.1.3 高层 API（`include/libxcp/xcp_master.hpp`）

**DAQ 配置与管理 API：**
- ✅ `ConfigureDaqList(const DaqListSpec&)` - 配置单个 STATIC DAQ List（严格账本管理，失败回滚）
- ✅ `ConfigureDaqListsDynamic(const std::vector<DaqListSpec>&)` - 动态整表编排（FREE_DAQ → ALLOC 序列 → 配置）
- ✅ `StartDaqSync()` - 同步启动（绕开 XCPlite 拒绝单列表 START 的问题）
- ✅ `StartDaqList(daq_list)` - 启动单个 DAQ List，返回 FIRST_PID 并回填账本
- ✅ `StopDaqList(daq_list)` / `StopDaq()` / `ClearDaqList(daq_list)`
- ✅ `DaqLedger()` - 返回 WRITE_DAQ 账本快照（DTO 解码唯一权威布局来源）
- ✅ `DaqConfigGeneration()` - 代际管理，解码侧识别陈旧账本

**DAQ 查询 API：**
- ✅ `QueryDaqProcessorInfo()` / `QueryDaqResolutionInfo()` / `QueryDaqListInfo(daq_list)` / `QueryDaqEventInfo(event_channel)` / `ReadDaqEntryAt(daq_list, odt, entry)`
- 所有查询方法返回 `std::optional<Response>`，Slave 回 `ERR_CMD_UNKNOWN` 时为 `nullopt`

**标定 API：**
- ✅ `HasCalPagResource()` - 检查 Resource bit0，false 时本地拒绝全部标定 API
- ✅ `QueryPagProcessorInfo()` - 一次性取证缓存（MAX_SEGMENT/PAG_PROPERTIES 是唯一运行时真值）
- ✅ `SetCalPage(mode, segment, page)` / `GetCalPage(access_mode, segment)`
- ✅ `GetSegmentInfo(mode, segment, info, mapping_index)` / `GetPageInfo(segment, page)`
- ✅ `SetSegmentFreeze(freeze, segment)` / `GetSegmentMode(segment)`
- ✅ `CopyCalPage(request)`
- ✅ `ModifyBits(address, extension, shift, and_mask, xor_mask)` - 内部编排 SET_MTA + MODIFY_BITS

**内存访问封装（`include/libxcp/memory_access.hpp`）：**
- ✅ `ReadElements()` / `ReadBytes()` / `WriteElements()` / `WriteBytes()` - 自动选择 SHORT_UPLOAD/SHORT_DOWNLOAD 或 SET_MTA + 分块传输
- ✅ `ModifyBits(shift, and_mask, xor_mask)` - 封装 SET_MTA + MODIFY_BITS

**测量会话（`include/libxcp/measurement/measurement_session.hpp`）：**
- ✅ `MeasurementSession` - 完整测量工作流：
  - Add/Remove/Clear 变量
  - `Prepare()` - 规划 + `ConfigureDaqListsDynamic`
  - `Start(callback)` - 启动 DTO 流，RX 线程入队 → worker 线程解码 → 回调用户
  - 配置方法：`SetEnvelopeMode()` / `SetTimestampUnit()` / `SetTimestampEnabled()` / `SetTimestampFirstOdtOnly()`
  - 统计：`MeasurementStatistics`（dto_received/dto_dropped/decode_errors/timestamp_wraps）

**测量规划器（`include/libxcp/measurement/measurement_planner.hpp`）：**
- ✅ `MeasurementPlanner.Build(names[])` → `MeasurementPlan{daq_lists + routes}`
- ✅ 装箱策略：按 event_channel 分组、保持用户顺序、顺序填充 ODT、超限开新 ODT、一个变量不跨 ODT

### 2.2 未实现的功能

#### 2.2.1 缺失的标定命令（2 个）

| 命令码 | 命令名称 | 类型 | 影响评估 |
|--------|----------|------|----------|
| ❌ 0xEF | DOWNLOAD_NEXT | Optional | **中等影响**：Block Mode 标准续传路径，首帧携块总元素数、续帧携剩余元素数，仅末帧应答，中间帧无 RESPONSE；大块下载可省去逐块 SET_MTA 往返。当前只能用 DOWNLOAD 分块，每块都要 SET_MTA。|
| ❌ 0xEE | DOWNLOAD_MAX | Optional | **低影响**：固定满帧下载，无 SIZE 字段，Slave 固定按 (MAX_CTO-1)/AG 元素接收；比 DOWNLOAD 省 1 字节，边际收益有限。当前 `WriteElements()/WriteBytes()` 已经自动选择 SHORT_DOWNLOAD 或 SET_MTA + DOWNLOAD。|

#### 2.2.2 缺失的 DAQ 命令（5 个）

| 命令码 | 命令名称 | 类型 | 影响评估 |
|--------|----------|------|----------|
| ❌ 0xC7 | WRITE_DAQ_MULTIPLE | Optional | **中等影响**：批量写 ODT Entry，减少 CRO 往返次数，提升 DAQ 配置效率（尤其是大量 Entry 的场景）。当前 `ConfigureDaqList()` 逐 Entry 调用 WRITE_DAQ，动态列表配置可能较慢。|
| ❌ 0xDC | GET_DAQ_CLOCK | Optional | **中等影响**：查询 Slave 时钟当前值，是建立 Host-Slave 时间关联的关键原始观测；单独一条命令不构成同步，还需 TIME_CORRELATION_PROPERTIES 提供时钟域与同步状态。缺失导致无法建立可解释的时间关系。|
| ❌ 0xDF | GET_DAQ_LIST_MODE | Optional | **低影响**：回读 DAQ List 模式字，用于验证配置或调试。当前只能记住本端 `SET_DAQ_LIST_MODE` 发送的值，无法取证 Slave 实际状态。|
| ❌ 0xC5 | DTO_CTR_PROPERTIES | Optional | **中等影响**：查询/修改 Event Channel 的 DTO 计数器属性，控制计数器插入行为（DAQ_MODE: INSERT_COUNTER/INSERT_STIM_COUNTER_COPY；STIM_MODE: DO_NOT_CHECK_COUNTER/CHECK_COUNTER）。缺失导致无法动态调整计数器策略，只能依赖 Slave 默认行为。|
| ❌ 0xC0/0x01 0xC0/0x02 | SET_DAQ_PACKED_MODE / GET_DAQ_PACKED_MODE | Optional | **高影响**：V1.5 新增 Packed 模式（element-grouped/event-grouped），提升 DTO 带宽利用率。**代码已明确标记为 `UnsupportedCapability::DaqPackedMode`**（`xcp_master.hpp:637`），表明设计上已知此特性但未实现。|

#### 2.2.3 能力边界声明

代码在 `include/libxcp/xcp_master.hpp:627-673` 明确定义了 `UnsupportedCapability` 枚举：

```cpp
enum class UnsupportedCapability {
    DaqPackedMode,              // 0xC0 子命令
    Level1OtherSubcommand,      // 其他 Level-1 子命令（非 GET_VERSION）
    Ipv6Transport,              // IPv6 传输层
    OptionalTlSubcommand        // 可选 TL 子命令
};
```

- ✅ `IsCapabilityUnsupported(capability)` - 查询能力是否不支持
- ✅ `ThrowIfUnsupported(capability)` - 前置断路，避免无效操作

**设计意图：** 冻结 1.x-conservative 能力基线，明确对外表达不支持 Packed 模式等高级特性。

---

## 三、完整性评估与建议

### 3.1 测量功能完整性

| 功能领域 | 证据状态 | 说明 |
|----------|----------|------|
| **核心 DAQ 配置** | ✅ 代码实现 + 本地测试 | STATIC/DYNAMIC 列表配置、启动/停止、账本管理 |
| **DAQ 查询** | ✅ 代码实现 + 本地测试 | 处理器信息、分辨率、列表信息、事件信息、Entry 回读 |
| **动态分配** | ✅ 代码实现 + 本地测试 | FREE/ALLOC 序列完整，严格顺序校验 |
| **DTO 解码** | ✅ 代码实现；计数器未贯通 | 识别字段、时间戳、多线程解码、物理值转换；`MeasurementSession` 内部把 `counter_enabled` 固定为 false，底层可解不等于高层已用 |
| **高层会话** | ✅ 代码实现 + 本地测试 | MeasurementSession/MeasurementPlanner，工作流完整 |
| **批量配置优化** | ⚠️ 缺失（不影响正确性） | 缺 WRITE_DAQ_MULTIPLE，大量 Entry 配置时 CRO 往返更多 |
| **时间关联** | ❌ 缺失 | 缺 GET_DAQ_CLOCK 与 TIME_CORRELATION_PROPERTIES，无法建立 Host-Slave 时间关系 |
| **计数器控制** | ❌ 缺失 | 缺 DTO_CTR_PROPERTIES，无法查询/修改计数器插入策略 |
| **Packed 模式** | ❌ 本库不支持 | 已标记 `UnsupportedCapability::DaqPackedMode`；这是**本库能力声明**，不能推断 Slave 不支持 |

**评估口径：** 本节按“代码是否存在、是否协商、能否配置、能否解码、失败是否可恢复、有无测试证据”逐项判断，不再给出单一百分比（原 85% 无可复算定义，已撤销）。

### 3.2 标定功能完整性

| 功能领域 | 证据状态 | 说明 |
|----------|----------|------|
| **基础下载** | ✅ 代码实现 + 本地测试 | DOWNLOAD/SHORT_DOWNLOAD 完整，自动选择最优路径 |
| **位修改** | ✅ 代码实现 + 本地测试 | MODIFY_BITS 完整，支持原子位操作 |
| **分页切换** | ✅ 代码实现 + 本地测试 | SET/GET_CAL_PAGE 完整，支持 ECU/XCP/ALL 模式 |
| **分页查询** | ✅ 代码实现 + 本地测试 | GET_PAG_PROCESSOR_INFO/GET_SEGMENT_INFO/GET_PAGE_INFO 完整 |
| **Segment 管理** | ✅ 代码实现 + 本地测试 | SET/GET_SEGMENT_MODE 完整，支持 FREEZE 模式 |
| **页面复制** | ✅ 代码实现 + 本地测试 | COPY_CAL_PAGE 完整 |
| **Block Mode 连续下载** | ❌ 缺失 | 缺 DOWNLOAD_NEXT，大块数据只能逐块 SET_MTA + DOWNLOAD |
| **满帧下载** | ⚠️ 缺失（收益有限） | 缺 DOWNLOAD_MAX，仅省 1 字节 |
| **标定持久化请求** | ❌ 缺失 | 缺 SET_REQUEST(STORE_CAL_REQ)，无法请求 Slave 把当前页存入非易失存储 |
| **写后校验** | ❌ 缺失 | 缺 BUILD_CHECKSUM，只能用 UPLOAD/SHORT_UPLOAD 逐字节读回 |
| **标定对象元数据** | ⚠️ 部分 | A2L 复杂对象（数组/结构体/矩阵/轴）映射未实现，见 B5 |

**评估口径：** 同 3.1，按证据逐项列出，不再给单一百分比（原 90% 已撤销）。

### 3.3 优先级建议

#### 高优先级（影响核心功能）

1. **GET_DAQ_CLOCK (0xDC)**
   - **原因：** 获取 Slave 时钟当前值是建立 Host-Slave 时间关联的关键原始观测；单条命令本身不构成同步，还需 TIME_CORRELATION_PROPERTIES 提供的时钟域/同步状态配合。
   - **工作量：** 低（简单命令，响应解析直接，无复杂逻辑）。
   - **建议：** 添加 `EncodeGetDaqClock()` + `ParseGetDaqClock()` + `XcpMaster::QueryDaqClock()`。

#### 中优先级（提升性能与灵活性）

2. **WRITE_DAQ_MULTIPLE (0xC7)**
   - **原因：** 批量写 Entry，减少 CRO 往返，提升 DAQ 配置效率（尤其是 100+ Entry 的场景）。
   - **工作量：** 中（需修改 `ConfigureDaqList()`/`ConfigureDaqListsDynamic()` 的编排逻辑，批量打包 Entry）。
   - **建议：** 在 `ConfigureDaqListsDynamic()` 中优先尝试 WRITE_DAQ_MULTIPLE，Slave 不支持时回退到逐 Entry WRITE_DAQ。

3. **DTO_CTR_PROPERTIES (0xC5)**
   - **原因：** 控制计数器插入行为，灵活应对不同 Slave 的计数器策略。
   - **工作量：** 中（子命令编码，响应解析，状态管理）。
   - **建议：** 添加 `QueryDtoCtrProperties(event_channel)` / `SetDtoCtrProperties(event_channel, properties)`。

4. **DOWNLOAD_NEXT (0xEF)**
   - **原因：** Block Mode 标准续传路径，首帧携块总元素数、续帧携剩余元素数，仅末帧肯定应答，大块下载省去逐块 SET_MTA。
   - **工作量：** 中（需引入 Block Mode 状态机与“中间帧无应答”的收发时序，修改 `MemoryAccess::WriteBytes()` 逻辑）。
   - **建议：** 在 `WriteBytes()` 中检测连续写，满足条件时使用 DOWNLOAD_NEXT。

#### 低优先级（边际收益）

5. **GET_DAQ_LIST_MODE (0xDF)**
   - **原因：** 回读模式字，用于验证配置或调试，非功能性需求。
   - **工作量：** 低。
   - **建议：** 按需实现。

6. **DOWNLOAD_MAX (0xEE)**
   - **原因：** 固定满帧下载，无 SIZE 字段，仅比 DOWNLOAD 省 1 字节，边际收益有限。
   - **工作量：** 低。
   - **建议：** 低优先级，可选。

#### 设计决策类（需架构评审）

7. **DAQ Packed Mode (0xC0)**
   - **原因：** 已明确标记为 `UnsupportedCapability`，表明设计上不支持。
   - **工作量：** 高（涉及 DTO 编码格式、解码器、账本结构、MeasurementPlanner 逻辑全面调整）。
   - **建议：** 需架构评审，评估 Packed 模式的收益与复杂度，决定是否纳入路线图。

---

## 四、测试覆盖建议

### 4.1 现有测试验证重点

1. **DAQ 账本管理测试**
   - 验证 `ConfigureDaqList()` 失败回滚（不留半份账）。
   - 验证 `ConfigureDaqListsDynamic()` 代际管理与陈旧账本检测。

2. **Slave 能力探测测试**
   - 验证 `QueryDaqProcessorInfo()` 返回 `nullopt` 时的降级逻辑。
   - 验证 `HasCalPagResource()` 返回 `false` 时本地拒绝标定 API。

3. **错误恢复测试**
   - 模拟 `ERR_SEQUENCE`（动态分配顺序错误）。
   - 模拟 `ERR_CMD_UNKNOWN`（Optional 命令不支持）。

### 4.2 新增功能测试建议

1. **GET_DAQ_CLOCK 测试**
   - 正常响应解析。
   - Slave 不支持时返回 `nullopt`。

2. **WRITE_DAQ_MULTIPLE 测试**
   - 批量写 Entry，验证账本正确性。
   - Slave 不支持时回退到逐 Entry WRITE_DAQ。

3. **DTO_CTR_PROPERTIES 测试**
   - 查询/修改计数器属性，验证状态同步。

---

## 五、总结

### 5.1 核心发现

1. **测量功能：** 核心流程完整（配置/启动/解码/规划），缺失时间关联（GET_DAQ_CLOCK 与 TIME_CORRELATION_PROPERTIES）、批量配置优化（WRITE_DAQ_MULTIPLE）、计数器控制（DTO_CTR_PROPERTIES）和 Packed 模式（本库不支持）；此外 `MeasurementSession` 未把底层可解的 DTO 计数器贯通到高层。
2. **标定功能：** 核心流程完整（下载/位修改/分页切换/查询），缺失 Block Mode 连续下载（DOWNLOAD_NEXT）、满帧下载（DOWNLOAD_MAX）、持久化请求（SET_REQUEST STORE_CAL_REQ）与写后校验（BUILD_CHECKSUM）。
3. **能力基线：** 代码明确表达了 1.x-conservative 能力基线，通过 `UnsupportedCapability` 枚举对外承诺不支持的特性；该枚举只描述本库能力，不证明 Slave 能力。

### 5.2 推荐路线图

> 该路线图已由 `code-plan/XCP_V1.5_测量标定修复与测试实施计划.md` 细化为 B0–B5 批次，本节仅保留高层对应关系。

**B0（规范纠偏 + 回读）：**
- 修正本文档命令码、支持等级与评分口径。
- 实现 GET_DAQ_LIST_MODE（配置取证，防止“本地以为已设置”）。

**B1（标定闭环）：**
- 实现 DOWNLOAD_NEXT / DOWNLOAD_MAX（Block Mode 与满帧）。
- 实现 SET_REQUEST(STORE_CAL_REQ) 与 BUILD_CHECKSUM（持久化请求与写后校验）。

**B2（DAQ 控制面）：**
- 实现 WRITE_DAQ_MULTIPLE（配置效率）。
- 实现 DTO_CTR_PROPERTIES + Event Channel 绑定 + Counter 贯通。

**B3（基础时间关联）：**
- 实现 GET_DAQ_CLOCK 与 TIME_CORRELATION_PROPERTIES、Clock Info 上传。

**B4（Packed）：**
- SET/GET_DAQ_PACKED_MODE 与多采样解包全链路。

**B5（独立后续）：**
- A2L 复杂标定对象（数组/结构体/矩阵/轴）映射，需单独批准。

### 5.3 合规性声明

- ✅ 标定 mandatory 命令 `DOWNLOAD`(0xF0) 已实现；DAQ mandatory 命令 `SET_DAQ_PTR`/`WRITE_DAQ`/`SET_DAQ_LIST_MODE`/`START_STOP_DAQ_LIST`/`START_STOP_SYNCH`/`CLEAR_DAQ_LIST` 已实现。
- ⚠️ 部分 Optional 命令缺失（不影响已有功能可用性，但限制能力上限）。
- ✅ 代码已明确标记不支持的能力（`UnsupportedCapability`），符合 XCP 规范“可选特性可不实现”的原则。
- ⚠️ “命令已编码”不等于“测量/标定业务完整”：能力探测、协商、账本一致性、失败恢复与测试证据须逐项核对。

**结论：** libxcp 当前实现覆盖了 XCP V1.5 测量与标定的**核心 mandatory 命令与主要 optional 命令**，可完成基本 DAQ 测量与标定读写；缺口集中在时间关联、标定持久化/校验、Block Mode 传输优化与 Packed 模式。**不以百分比作为验收结论**，后续以实施计划 B0–B5 的验收条件与真实测试结果为准。
