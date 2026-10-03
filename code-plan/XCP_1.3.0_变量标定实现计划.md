# XCP 1.3.0 变量标定（Calibration & Page Switching）实现计划

## 1. 目的与依据

本计划面向 **libxcp Master 协议核心库**，在已有最小协议核心、Seed&Key 解锁、DAQ 编排和内存写回（DOWNLOAD/SHORT_DOWNLOAD）的基础上，增加完整的 **变量标定** 能力。本文只规划，不实施代码。

依据：

- `docs/XCP_1.3.0_document.md`（§7.5.2.5 MODIFY_BITS、§7.5.3 Page Switching Command 全部 8 条）
- `code-plan/XCP_1.3.0_最小协议核心实现计划.md`（架构、分层、错误策略基线）
- `thirdparty/XCPlite/src/xcp.h`（CRO/CRM 布局参考）

实现语言为 C++20，目标平台为 Windows、Linux、macOS。本计划复用现有架构分层（ProtocolTypes → CommandCodec/ResponseParser → CommandExecutor/MemoryAccess → XcpMaster），不引入新依赖。

## 2. 已确认范围

### 2.1 包含

#### A. 标定值写入补充

1. **MODIFY_BITS (0xEC)**：对 MTA 指向的 32-bit 内存位置执行 AND/XOR Mask 原子位操作；Shift Value S 将两组 16-bit Mask 向高位移动；不修改 MTA。

#### B. Page Switching 全部 8 条命令

2. **SET_CAL_PAGE (0xEB)**：指定 Segment/Page 用于 ECU 访问、XCP 访问或两者同时；Mode 含 ECU(0x01)/XCP(0x02)/ALL(0x80)。
3. **GET_CAL_PAGE (0xEA)**：查询指定 Segment 当前激活的 Calibration Page；Access Mode 仅 0x01(ECU)/0x02(XCP)。
4. **GET_PAG_PROCESSOR_INFO (0xE9)**：返回 Paging 子系统 MAX_SEGMENT 和 PAG_PROPERTIES（FREEZE_SUPPORTED）。
5. **GET_SEGMENT_INFO (0xE8)**：按 Mode 查询 Segment 信息——Mode 0 基础地址/长度、Mode 1 标准属性、Mode 2 Address Mapping。
6. **GET_PAGE_INFO (0xE7)**：查询指定 Segment/Page 的访问属性和 INIT_SEGMENT。
7. **SET_SEGMENT_MODE (0xE6)**：设置 Segment 的 FREEZE 标志。
8. **GET_SEGMENT_MODE (0xE5)**：读取 Segment 当前 Mode（主要查询 FREEZE 状态）。
9. **COPY_CAL_PAGE (0xE4)**：将一个 Segment/Page 复制到另一个 Segment/Page。

#### C. Session 参数扩展

10. CONNECT 后检查 CAL/PAG Resource bit（Resource::CalPag = 0x01）；若未置位，标定命令应返回 UnsupportedFeature。
11. Session 缓存 GET_PAG_PROCESSOR_INFO 结果（MAX_SEGMENT、PAG_PROPERTIES），供后续 Page 命令做本地预校验。

#### D. 测试覆盖

12. 全部 9 条命令的 Codec 黄金报文测试（Intel/Motorola Byte Order）。
13. ResponseParser 正常响应解析 + 畸形包/截断包防护。
14. CommandExecutor 端到端流程测试（Mock Transport）。
15. XcpMaster 公共 API 集成测试。
16. XCPlite Slave Loopback 端到端验证（利用已有 UdpTestSlave 扩展或 XCPlite cpp_demo）。

### 2.2 不包含

- Flash Programming 流程（PROGRAM_START/PROGRAM_CLEAR/PROGRAM 等）。
- STORE_CAL_REQ / CLEAR_CAL_REQ 的具体 NVM 持久化逻辑（仅解析 GET_STATUS 中的相关状态位）。
- A2L 中 CALIBRATION_METHOD / COMPU_METHOD 的解析与生成。
- STIM 方向的 Page Switching 联动。
- 真实 ECU/CANape 互操作验证。

### 2.3 边界

- MODIFY_BITS 属于 Calibration 类命令，需要 CAL/PAG Resource 权限和 Seed&Key 解锁；Page Switching 同样需要 CAL/PAG Resource。
- 所有 Page Switching 命令均为 Optional；Slave 返回 ERR_CMD_UNKNOWN 时，Master 应标记该命令不可用并上报，不做降级替代。
- GET_SEGMENT_INFO 响应长度随 Mode 变化（Mode 0 = 8B, Mode 1 = 6B, Mode 2 = 8B），Parser 需按 Mode 分支解析。
- COPY_CAL_PAGE 可能因目标区域写保护返回 ERR_WRITE_PROTECTED，此时应提示走 Flash Programming 流程。

## 3. 架构扩展

```text
Application / future A2L
          |
     XcpMaster / Session
          |
   +------+-------+
   |              |
MemoryAccess  CommandExecutor
   |          request/response
   |          timeout/events
   +------+-------+
          |
 CommandCodec / ResponseParser
          |
     IXcpTransport
```

### 3.1 新增/扩展组件职责

- **ProtocolTypes**：新增 CommandCode 枚举值（ModifyBits/SetCalPage/GetCalPage/GetPagProcessorInfo/GetSegmentInfo/GetPageInfo/SetSegmentMode/GetSegmentMode/CopyCalPage）；新增 CalPageMode、PagProperties、PageProperties、SegmentMode 等强类型枚举/位域；新增各命令的请求/响应结构体。
- **CommandCodec**：新增 EncodeModifyBits、EncodeSetCalPage、EncodeGetCalPage、EncodeGetPagProcessorInfo、EncodeGetSegmentInfo、EncodeGetPageInfo、EncodeSetSegmentMode、EncodeGetSegmentMode、EncodeCopyCalPage；遵守 Byte Order 和 MAX_CTO 校验。
- **ResponseParser**：新增 ParseModifyBitsResponse、ParseSetCalPageResponse、ParseGetCalPageResponse、ParseGetPagProcessorInfoResponse、ParseGetSegmentInfoResponse（按 Mode 分支）、ParseGetPageInfoResponse、ParseSetSegmentModeResponse、ParseGetSegmentModeResponse、ParseCopyCalPageResponse。
- **CommandExecutor**：新增 ExecuteModifyBits、ExecuteSetCalPage、ExecuteGetCalPage、ExecuteGetPagProcessorInfo、ExecuteGetSegmentInfo、ExecuteGetPageInfo、ExecuteSetSegmentMode、ExecuteGetSegmentMode、ExecuteCopyCalPage；复用 RunCommand → PerformAttempt → WaitForResponse → DispatchResponse 链。
- **MemoryAccess**：新增 ModifyBits 方法（封装 SET_MTA + MODIFY_BITS 序列，因为 MODIFY_BITS 依赖 MTA）。
- **Session**：新增 PagProcessorInfo 缓存字段；CONNECT 后记录 CAL/PAG Resource 可用性。
- **XcpMaster**：新增 ModifyBits、SetCalPage、GetCalPage、GetPagProcessorInfo、GetSegmentInfo、GetPageInfo、SetSegmentMode、GetSegmentMode、CopyCalPage 公共 API。

## 4. 协议约束

### 4.1 MODIFY_BITS (0xEC)

```text
CRO (Master→Slave):
  BYTE 0: Command Code (0xEC)
  BYTE 1: Shift Value S (0..16)
  WORD 1: AND Mask (16-bit)
  WORD 2: XOR Mask (16-bit)
  Total: 6 Bytes

CRM (Slave→Master):
  BYTE 0: RES PID (0xFF)
  Total: 1 Byte (无附加数据)
```

约束：
- 操作对象为 MTA 指向的 32-bit 内存位置；执行前必须先 SET_MTA。
- 计算公式：`Result = (Value AND (AND_Mask << S)) XOR (XOR_Mask << S)`，其中 AND/XOR Mask 先零扩展到 32-bit 再左移 S 位。
- Shift Value S 有效范围 0..16；超出返回 ERR_OUT_OF_RANGE。
- 执行后 MTA 不变。
- 需要 CAL/PAG Resource 权限和 UNLOCKED 状态。

### 4.2 SET_CAL_PAGE (0xEB)

```text
CRO:
  BYTE 0: Command Code (0xEB)
  BYTE 1: Mode
    bit 0: ECU (0x01) — Page 供 ECU Application 使用
    bit 1: XCP (0x02) — Page 供 XCP Driver/Master 访问
    bit 7: ALL (0x80) — 忽略 Segment Number，对全部 Segment 应用
  BYTE 2: Segment Number
  BYTE 3: Page Number
  Total: 4 Bytes

CRM:
  BYTE 0: RES PID (0xFF)
  Total: 1 Byte
```

约束：
- Mode 中 ECU/XCP 可同时置位；ALL 置位时忽略 Segment Number。
- 不可行组合返回 ERR_MODE_NOT_VALID。
- Page/Segment 不存在分别返回 ERR_PAGE_NOT_VALID / ERR_SEGMENT_NOT_VALID。

### 4.3 GET_CAL_PAGE (0xEA)

```text
CRO:
  BYTE 0: Command Code (0xEA)
  BYTE 1: Access Mode (0x01=ECU, 0x02=XCP)
  BYTE 2: Segment Number
  Total: 3 Bytes

CRM:
  BYTE 0: RES PID (0xFF)
  BYTE 1: Reserved
  BYTE 2: Reserved
  BYTE 3: Page Number
  Total: 4 Bytes
```

约束：
- Access Mode 仅允许 0x01 或 0x02；其他值返回 ERR_MODE_NOT_VALID。
- 响应返回对应 Access Mode 下当前激活的逻辑 Page Number。

### 4.4 GET_PAG_PROCESSOR_INFO (0xE9)

```text
CRO:
  BYTE 0: Command Code (0xE9)
  Total: 1 Byte

CRM:
  BYTE 0: RES PID (0xFF)
  BYTE 1: MAX_SEGMENT (uint8)
  BYTE 2: PAG_PROPERTIES
    bit 0: FREEZE_SUPPORTED
  Total: 3 Bytes
```

### 4.5 GET_SEGMENT_INFO (0xE8)

```text
CRO:
  BYTE 0: Command Code (0xE8)
  BYTE 1: Mode (0/1/2)
  BYTE 2: Segment Number
  BYTE 3: Segment Info (Mode 0: 0=Address, 1=Length; Mode 2: 0=SrcAddr, 1=DestAddr, 2=Length)
  BYTE 4: Mapping Index (仅 Mode 2 使用)
  Total: 5 Bytes

CRM (Mode 0 — Basic Info):
  BYTE 0: RES PID
  DWORD 1: Segment Address (Mode 0, Info=0) 或 Segment Length (Mode 0, Info=1)
  Total: 8 Bytes (含 padding)

CRM (Mode 1 — Standard Properties):
  BYTE 0: RES PID
  BYTE 1: MAX_PAGES
  BYTE 2: ADDRESS_EXTENSION
  BYTE 3: MAX_MAPPING
  BYTE 4: Compression Method
  BYTE 5: Encryption Method
  Total: 6 Bytes

CRM (Mode 2 — Mapping Info):
  BYTE 0: RES PID
  DWORD 1: Mapping Source/Dest Address 或 Length
  Total: 8 Bytes (含 padding)
```

约束：
- 响应长度随 Mode 变化，Parser 必须按请求 Mode 分支解析。
- Segment 无效返回 ERR_OUT_OF_RANGE。
- Mode 2 中 MAPPING_INDEX 超出 MAX_MAPPING 返回 ERR_OUT_OF_RANGE。

### 4.6 GET_PAGE_INFO (0xE7)

```text
CRO:
  BYTE 0: Command Code (0xE7)
  BYTE 1: Reserved
  BYTE 2: Segment Number
  BYTE 3: Page Number
  Total: 4 Bytes

CRM:
  BYTE 0: RES PID (0xFF)
  BYTE 1: PAGE_PROPERTIES
    bits 0-1: ECU_ACCESS_TYPE (0=NONE, 1=WITHOUT_XCP, 2=WITH_XCP, 3=DONT_CARE)
    bits 2-3: XCP_READ_ACCESS_TYPE (同上)
    bits 4-5: XCP_WRITE_ACCESS_TYPE (同上)
  BYTE 2: INIT_SEGMENT (该 Page 初始化数据所在的 Segment Number)
  Total: 3 Bytes
```

### 4.7 SET_SEGMENT_MODE (0xE6)

```text
CRO:
  BYTE 0: Command Code (0xE6)
  BYTE 1: Mode
    bit 0: FREEZE (1=冻结, 0=取消冻结)
  BYTE 2: Segment Number
  Total: 3 Bytes

CRM:
  BYTE 0: RES PID (0xFF)
  Total: 1 Byte
```

约束：
- Segment 不存在返回 ERR_OUT_OF_RANGE。

### 4.8 GET_SEGMENT_MODE (0xE5)

```text
CRO:
  BYTE 0: Command Code (0xE5)
  BYTE 1: Reserved
  BYTE 2: Segment Number
  Total: 3 Bytes

CRM:
  BYTE 0: RES PID (0xFF)
  BYTE 1: Reserved
  BYTE 2: Mode
    bit 0: FREEZE
  Total: 3 Bytes
```

### 4.9 COPY_CAL_PAGE (0xE4)

```text
CRO:
  BYTE 0: Command Code (0xE4)
  BYTE 1: Source Segment Number
  BYTE 2: Source Page Number
  BYTE 3: Destination Segment Number
  BYTE 4: Destination Page Number
  Total: 5 Bytes

CRM:
  BYTE 0: RES PID (0xFF)
  Total: 1 Byte
```

约束：
- 目标区域写保护返回 ERR_WRITE_PROTECTED，应提示走 Flash Programming。
- Page/Segment 无效返回 ERR_PAGE_NOT_VALID / ERR_SEGMENT_NOT_VALID。

### 4.10 Resource 权限检查

- 所有 Calibration/Page 命令要求 CONNECT 响应中 RESOURCE.CAL_PAG (bit 0) 置位。
- 若 CAL_PAG 未置位，XcpMaster 应在调用时立即返回 UnsupportedFeature，不发送命令。
- 若 CAL_PAG 置位但资源被锁定，Slave 返回 ERR_ACCESS_LOCKED；Master 报告需要 Seed&Key 解锁，不自动解锁（与现有 DOWNLOAD 行为一致）。

## 5. 数据流

### 5.1 MODIFY_BITS

```text
modifyBits(address, extension, shift, andMask, xorMask)
 -> 校验 Connected、CAL/PAG Resource、UNLOCKED
 -> SET_MTA(extension, address)
 -> MODIFY_BITS(shift, andMask, xorMask)
 -> 校验 RES
 -> 返回成功（MTA 不变）
```

- Timeout 恢复时需重发 SET_MTA（与 UPLOAD 恢复策略一致）。
- Shift > 16 在本地拒绝，返回 InvalidArgument。

### 5.2 Page Switching 典型流程

```text
// 探测 Paging 能力
getPagProcessorInfo()
 -> GET_PAG_PROCESSOR_INFO
 -> 缓存 MAX_SEGMENT、PAG_PROPERTIES

// 查询 Segment/Page 详情
getSegmentInfo(segment, mode, info, mappingIndex)
 -> GET_SEGMENT_INFO
 -> 按 Mode 解析响应

getPageInfo(segment, page)
 -> GET_PAGE_INFO
 -> 解析 PAGE_PROPERTIES、INIT_SEGMENT

// 切换标定页
setCalPage(mode, segment, page)
 -> SET_CAL_PAGE
 -> 校验 RES

getCalPage(accessMode, segment)
 -> GET_CAL_PAGE
 -> 返回当前激活 Page Number

// 复制标定页
copyCalPage(srcSeg, srcPage, dstSeg, dstPage)
 -> COPY_CAL_PAGE
 -> 校验 RES（ERR_WRITE_PROTECTED 时提示 Flash Programming）
```

### 5.3 Session 初始化扩展

```text
CONNECT
 -> 校验 RESOURCE.CAL_PAG
 -> [CAL_PAG 置位] GET_PAG_PROCESSOR_INFO
      -> 成功：缓存 MAX_SEGMENT、PAG_PROPERTIES
      -> ERR_CMD_UNKNOWN：标记 Page Switching 不可用，继续
      -> 其他 ERR：报告但不阻断连接
 -> GET_STATUS
 -> Connected
```

- GET_PAG_PROCESSOR_INFO 仅在 CAL_PAG Resource 可用时调用。
- 失败不阻断连接流程，仅影响后续 Page 命令的本地预校验。

## 6. 错误与恢复

### 6.1 新增错误场景

| Error | 触发条件 | 策略 |
|---|---|---|
| ERR_MODE_NOT_VALID | SET_CAL_PAGE Mode 组合不可行；GET_CAL_PAGE Access Mode 非法 | 返回 ProtocolError，不重试 |
| ERR_PAGE_NOT_VALID | Page Number 超出范围 | 返回 ProtocolError，附带 Segment/Page 上下文 |
| ERR_SEGMENT_NOT_VALID | Segment Number 超出范围 | 返回 ProtocolError，附带 Segment 上下文 |
| ERR_WRITE_PROTECTED | COPY_CAL_PAGE 目标区域写保护 | 返回 ProtocolError，提示走 Flash Programming |
| ERR_OUT_OF_RANGE | MODIFY_BITS Shift > 16；GET_SEGMENT_INFO Segment/Mapping 越界 | 本地预检或返回 ProtocolError |
| ERR_ACCESS_LOCKED | CAL/PAG 资源被锁定 | 报告需要 Seed&Key，不自动解锁 |
| ERR_CMD_UNKNOWN | Page Switching 命令不被 Slave 支持 | 标记该命令当前 Session 不可用 |

### 6.2 恢复策略

- MODIFY_BITS 的 Timeout 恢复与 UPLOAD 相同：SYNCH → 重发 SET_MTA → 重发 MODIFY_BITS；最多两次恢复重试。
- Page Switching 命令均为单帧请求/响应，Timeout 恢复只需 SYNCH → 重发原命令；无需重建隐含状态。
- GET_PAG_PROCESSOR_INFO 在连接阶段失败不触发恢复，仅降级。

## 7. 建议目录变更

在现有目录基础上，不新增文件，仅在现有文件中扩展：

```text
include/libxcp/
  protocol_types.hpp       ← 新增命令码、枚举、请求/响应结构体
  command_codec.hpp        ← 新增 9 个 Encode 方法声明
  response_parser.hpp      ← 新增 9 个 Parse 方法声明
  command_executor.hpp     ← 新增 9 个 Execute 方法声明
  memory_access.hpp        ← 新增 ModifyBits 方法声明
  xcp_master.hpp           ← 新增 9 个公共 API 声明
  session.hpp              ← 新增 PagProcessorInfo 缓存字段
src/
  command_codec.cpp        ← 实现 9 个 Encode 方法
  response_parser.cpp      ← 实现 9 个 Parse 方法
  command_executor.cpp     ← 实现 9 个 Execute 方法
  memory_access.cpp        ← 实现 ModifyBits
  xcp_master.cpp           ← 实现 9 个公共 API
  session.cpp              ← CONNECT 后缓存 PAG 信息
tests/
  command_codec_test.cpp   ← 新增标定命令黄金报文测试
  response_parser_test.cpp ← 新增标定响应解析测试
  calibration_test.cpp     ← 新增：CommandExecutor/XcpMaster 标定流程集成测试
```

## 8. 实施阶段

### 阶段 C1：协议类型与常量

1. 在 `protocol_types.hpp` 中新增 CommandCode 枚举值：
   - `ModifyBits = 0xEC`
   - `SetCalPage = 0xEB`
   - `GetCalPage = 0xEA`
   - `GetPagProcessorInfo = 0xE9`
   - `GetSegmentInfo = 0xE8`
   - `GetPageInfo = 0xE7`
   - `SetSegmentMode = 0xE6`
   - `GetSegmentMode = 0xE5`
   - `CopyCalPage = 0xE4`
2. 新增强类型枚举/位域：
   - `CalPageMode`：Ecu(0x01)、Xcp(0x02)、All(0x80)
   - `CalPageAccessMode`：Ecu(0x01)、Xcp(0x02)
   - `PagProperties`：FreezeSupported(0x01)
   - `PageProperties`：EcuAccessType/XcpReadAccessType/XcpWriteAccessType 各 2-bit
   - `SegmentMode`：Freeze(0x01)
   - `SegmentInfoMode`：BasicInfo(0)、StandardProperties(1)、MappingInfo(2)
3. 新增请求/响应结构体（遵循现有命名约定）：
   - `ModifyBitsRequest { uint8_t shift; uint16_t and_mask; uint16_t xor_mask; }`
   - `SetCalPageRequest { CalPageMode mode; uint8_t segment; uint8_t page; }`
   - `GetCalPageRequest { CalPageAccessMode access_mode; uint8_t segment; }`
   - `GetCalPageResponse { uint8_t page; }`
   - `GetPagProcessorInfoResponse { uint8_t max_segment; PagProperties properties; }`
   - `GetSegmentInfoRequest { SegmentInfoMode mode; uint8_t segment; uint8_t segment_info; uint8_t mapping_index; }`
   - `GetSegmentInfoResponse`：variant 或 union，按 Mode 区分
   - `GetPageInfoRequest { uint8_t segment; uint8_t page; }`
   - `GetPageInfoResponse { PageProperties properties; uint8_t init_segment; }`
   - `SetSegmentModeRequest { SegmentMode mode; uint8_t segment; }`
   - `GetSegmentModeRequest { uint8_t segment; }`
   - `GetSegmentModeResponse { SegmentMode mode; }`
   - `CopyCalPageRequest { uint8_t src_segment; uint8_t src_page; uint8_t dst_segment; uint8_t dst_page; }`

完成标准：所有类型编译通过，枚举值与 XCP 1.3.0 文档一致。

### 阶段 C2：CommandCodec 编码

1. 实现 9 个 Encode 方法，逐字段对照 §4 布局。
2. Reserved 字节写 0。
3. 遵守 Session Byte Order（Intel/Motorola）。
4. 发送前检查 MAX_CTO。
5. 建立黄金报文测试向量（至少 Intel + Motorola 各一组）。

完成标准：编码输出与 XCPlite CRO 定义逐字节一致；非法参数（如 Shift > 16）在编码前拒绝。

### 阶段 C3：ResponseParser 解析

1. 实现 9 个 Parse 方法。
2. GET_SEGMENT_INFO 按 Mode 分支解析，校验响应长度。
3. 畸形包/截断包返回 MalformedPacket，不越界读取。
4. 保留 Error 附加信息。

完成标准：正常响应字段正确；截断/超长/错误长度均安全处理。

### 阶段 C4：Session 扩展与连接流程

1. Session 新增 `std::optional<GetPagProcessorInfoResponse> pag_processor_info_` 缓存。
2. CONNECT 成功后，若 RESOURCE.CAL_PAG 置位，调用 GET_PAG_PROCESSOR_INFO 并缓存结果。
3. GET_PAG_PROCESSOR_INFO 返回 ERR_CMD_UNKNOWN 时标记 Page Switching 不可用，不阻断连接。
4. 提供 `HasCalPagResource()` 和 `GetPagProcessorInfo()` 查询接口。

完成标准：连接流程兼容有无 CAL/PAG 资源的 Slave；缓存正确填充。

### 阶段 C5：CommandExecutor 执行方法

1. 实现 9 个 Execute 方法，复用 RunCommand 链。
2. MODIFY_BITS 前置 SET_MTA；Timeout 恢复时重发 SET_MTA。
3. Page Switching 命令直接发送，无隐含状态依赖。
4. Resource 权限检查在 Execute 入口执行。

完成标准：Mock Transport 下全部命令可成功执行；Timeout/SYNCH 恢复正确。

### 阶段 C6：MemoryAccess 与 XcpMaster 公共 API

1. MemoryAccess::ModifyBits 封装 SET_MTA + MODIFY_BITS 序列。
2. XcpMaster 新增 9 个公共方法，签名友好（Byte 级 API + AG 校验）。
3. CAL/PAG Resource 预检查；不可用时返回 UnsupportedFeature。
4. 中文 Doxygen 注释。

完成标准：公共 API 可被应用层直接调用；错误分类明确。

### 阶段 C7：测试收口

1. 单元测试：9 条命令黄金报文、Byte Order、边界值、畸形包。
2. 集成测试：Mock Transport 下完整标定流程（探测→查询→切换→写入→复制）。
3. Loopback 测试：XCPlite Slave 端到端验证 MODIFY_BITS + Page Switching。
4. Release 构建、格式检查、静态分析。

完成标准：全部测试通过；Release 构建无警告。

## 9. 测试计划

### 9.1 Codec 单元测试

- MODIFY_BITS：Shift=0/16 边界、AND/XOR Mask 全 0/全 F、Intel/Motorola 字节序。
- SET_CAL_PAGE：Mode=ECU/XCP/ALL/ECU+XCP、Segment=0/255、Page=0/255。
- GET_CAL_PAGE：AccessMode=ECU/XCP。
- GET_PAG_PROCESSOR_INFO：仅命令码，无参数。
- GET_SEGMENT_INFO：Mode=0/1/2、SegmentInfo=0/1/2、MappingIndex 边界。
- GET_PAGE_INFO：Segment/Page 边界。
- SET_SEGMENT_MODE：Mode=FREEZE/0。
- GET_SEGMENT_MODE：Segment 边界。
- COPY_CAL_PAGE：Src/Dst Segment/Page 组合。
- 所有命令 reserved 字节为 0。
- MAX_CTO 不足时拒绝编码。

### 9.2 Parser 单元测试

- 正常响应字段解析。
- GET_SEGMENT_INFO 三种 Mode 响应长度校验。
- 截断响应（少于预期长度）→ MalformedPacket。
- 超长响应 → 安全忽略多余字节。
- ERR 响应保留 Error Code 和附加信息。

### 9.3 CommandExecutor 集成测试（Mock Transport）

1. MODIFY_BITS 成功流程：SET_MTA → MODIFY_BITS → RES。
2. MODIFY_BITS Timeout → SYNCH → SET_MTA → MODIFY_BITS → RES。
3. MODIFY_BITS Shift > 16 → InvalidArgument（不发送）。
4. SET_CAL_PAGE → RES；ERR_MODE_NOT_VALID → ProtocolError。
5. GET_CAL_PAGE → RES 含 Page Number。
6. GET_PAG_PROCESSOR_INFO → RES 含 MAX_SEGMENT/PAG_PROPERTIES。
7. GET_SEGMENT_INFO Mode 0/1/2 分别解析。
8. GET_PAGE_INFO → RES 含 PAGE_PROPERTIES/INIT_SEGMENT。
9. SET_SEGMENT_MODE → RES。
10. GET_SEGMENT_MODE → RES 含 Mode。
11. COPY_CAL_PAGE → RES；ERR_WRITE_PROTECTED → ProtocolError。
12. CAL/PAG Resource 未置位 → UnsupportedFeature。
13. ERR_ACCESS_LOCKED → 报告需要 Seed&Key。
14. ERR_CMD_UNKNOWN → 标记命令不可用。

### 9.4 XcpMaster API 测试

1. 完整标定工作流：GetPagProcessorInfo → GetSegmentInfo → GetPageInfo → SetCalPage → GetCalPage → ModifyBits → CopyCalPage。
2. Resource 不可用时所有标定 API 返回 UnsupportedFeature。
3. 未连接时返回 InvalidState。

### 9.5 XCPlite Loopback 端到端测试

1. 连接 XCPlite cpp_demo Slave，验证 CAL/PAG Resource 探测。
2. GET_PAG_PROCESSOR_INFO → 验证 MAX_SEGMENT。
3. GET_SEGMENT_INFO Mode 0/1 → 验证 Segment 地址/属性。
4. SET_CAL_PAGE + GET_CAL_PAGE → 验证页切换生效。
5. MODIFY_BITS → 验证位操作结果（通过 SHORT_UPLOAD 读回验证）。
6. COPY_CAL_PAGE → 验证复制结果。

## 10. 验收标准

1. 9 条命令均有 Codec/Parser/Executor/XcpMaster 覆盖。
2. MODIFY_BITS 正确执行 AND/XOR Mask 位操作，Shift 范围 0..16。
3. Page Switching 命令正确处理 Mode/Segment/Page 参数。
4. GET_SEGMENT_INFO 按 Mode 正确解析变长响应。
5. CAL/PAG Resource 预检查有效；不可用时返回 UnsupportedFeature。
6. Session 缓存 PAG Processor Info；连接流程兼容无 CAL/PAG 的 Slave。
7. Timeout 恢复正确（MODIFY_BITS 重建 MTA；Page 命令直接重发）。
8. 全部错误码按 §6.1 策略处理。
9. Release 单元测试、Mock 集成测试和 Loopback 测试全部通过。
10. 中文 Doxygen 注释完整。
11. 不引入新依赖，不修改现有公共 API 签名。

## 11. 风险与依赖

1. **GET_SEGMENT_INFO 变长响应**：Parser 必须知道请求 Mode 才能正确解析响应；需要在 CommandExecutor 中将请求 Mode 传递给 Parser，或使用带 Mode 参数的专用 Parse 方法。
2. **XCPlite Slave 对标定命令的支持程度**：XCPlite 实现了完整的 Page Switching 和 MODIFY_BITS，但其 cpp_demo 是否暴露了足够的标定 Segment/Page 用于测试需在实施时确认；不足时需扩展 demo 配置。
3. **MODIFY_BITS 的 MTA 依赖**：MODIFY_BITS 本身不设 MTA，必须在调用前确保 MTA 已设置；MemoryAccess 层需封装 SET_MTA + MODIFY_BITS 原子序列，并在 Timeout 恢复时重建 MTA。
4. **COPY_CAL_PAGE 的 Slave 限制**：Slave 可能对源/目标组合施加额外限制（如不允许跨 Segment 复制）；Master 无法预知，只能依赖 ERR 响应。
5. **Page Switching 全部 Optional**：不同 Slave 支持的子集不同；Master 必须容忍 ERR_CMD_UNKNOWN 并优雅降级。
6. **规范校核**：所有命令布局以 `docs/XCP_1.3.0_document.md` 为准，XCPlite CRO/CRM 定义作为交叉验证；二者不一致时以文档为准并记录偏差。

## 12. 后续里程碑（本计划外）

1. Flash Programming 命令组（PROGRAM_START/PROGRAM_CLEAR/PROGRAM/PROGRAM_RESET）。
2. STORE_CAL_REQ / CLEAR_CAL_REQ 与 NVM 持久化联动。
3. A2L CALIBRATION_METHOD / COMPU_METHOD 解析与生成。
4. STIM 方向 Page Switching 联动。
5. 真实 ECU/CANape 标定互操作验证。
6. 标定参数的类型安全封装（结合 A2L 数据类型）。
