# XCP 1.3.0 变量标定功能修改说明

本文档记录按 [XCP_1.3.0_变量标定实现计划.md](XCP_1.3.0_变量标定实现计划.md) 完成的一轮代码修改（实施阶段 C1–C7），供评审与后续维护参考。

## 1. 概要

- **目标**：在 libxcp Master 协议核心库中实现完整的变量标定能力——MODIFY_BITS (0xEC) + Page Switching 全部 8 条命令（SET_CAL_PAGE/GET_CAL_PAGE/GET_PAG_PROCESSOR_INFO/GET_SEGMENT_INFO/GET_PAGE_INFO/SET_SEGMENT_MODE/GET_SEGMENT_MODE/COPY_CAL_PAGE）。
- **方式**：不新增源文件，全部在现有分层（ProtocolTypes → CommandCodec/ResponseParser → CommandExecutor/MemoryAccess → Session/XcpMaster）内扩展。
- **规模**：16 个文件修改，+1795 / -4 行；新增 27 个测试用例。
- **验证**：Release 构建零错误；全量 ctest **447/447 通过**（1 个既有参数化跳过项不变）。

## 2. 变更清单

| 文件 | 变更内容 |
|---|---|
| `include/libxcp/protocol_types.hpp` | 9 个 CommandCode 枚举值、标定域位域/枚举、14 个请求/响应结构体 |
| `include/libxcp/command_codec.hpp` | 9 个 Encode 方法声明（Doxygen 注释齐全） |
| `src/command_codec.cpp` | 9 个 Encode 实现（含本地参数预检，AND/XOR Mask 按 Session 字节序） |
| `include/libxcp/response_parser.hpp` | 5 个 Parse 方法声明（GET_* 系列响应） |
| `src/response_parser.cpp` | 5 个 Parse 实现（GET_SEGMENT_INFO 按 Mode 分支解析变长响应） |
| `include/libxcp/command_executor.hpp` | 9 个 Execute 方法声明 |
| `src/command_executor.cpp` | 9 个 Execute 实现 + 超时恢复钩子扩展（ModifyBits 重建 MTA） |
| `include/libxcp/memory_access.hpp` | ModifyBits 方法声明 |
| `src/memory_access.cpp` | ModifyBits 实现（SET_MTA + MODIFY_BITS 原子序列） |
| `include/libxcp/session.hpp` | PAG Processor Info 缓存访问器 |
| `src/session.cpp` | 缓存实现 + 四处会话清理点同步清空 |
| `include/libxcp/xcp_master.hpp` | 10 个公共 API 声明 + 探测缓存成员 |
| `src/xcp_master.cpp` | 10 个 API 实现 + Connect() 后 CAL/PAG 探测流程 |
| `tests/command_codec_test.cpp` | 8 个黄金报文/边界测试 |
| `tests/response_parser_test.cpp` | 8 个响应解析测试 |
| `tests/xcp_master_integration_test.cpp` | Mock Slave 扩展 + 11 个标定集成测试 |

## 3. 各层修改明细

### 3.1 ProtocolTypes（`include/libxcp/protocol_types.hpp`）

**命令码**（CommandCode 枚举，:86-94）：

```cpp
ModifyBits = 0xEC, SetCalPage = 0xEB, GetCalPage = 0xEA,
GetPagProcessorInfo = 0xE9, GetSegmentInfo = 0xE8, GetPageInfo = 0xE7,
SetSegmentMode = 0xE6, GetSegmentMode = 0xE5, CopyCalPage = 0xE4,
```

**位域/枚举**（沿用 DAQ 批次风格：`enum class XxxBit : uint8_t` + `constexpr operator|` + `HasXxx()`）：

- `CalPageModeBit`：kNone/kEcu(0x01)/kXcp(0x02)/kAll(0x80) + `HasCalPageMode()`
- `PagPropertyBit`：kFreezeSupported(0x01) + `HasPagProperty()`
- `SegmentModeBit`：kFreeze(0x01) + `HasSegmentMode()`
- `SegmentInfoMode`：BasicInfo(0)/StandardProperties(1)/MappingInfo(2)
- `SegmentInfoSelector`：SegmentAddress(0)/SegmentLength(1)/MappingLength(2)
- `PageAccessType`：NotAllowed/WithoutOtherAccess/WithConcurrentAccess/DontCare
- `PageProperties`：raw + ecu_access/xcp_read_access/xcp_write_access 三组拆解
- `CalPageAccessMode`：Ecu(0x01)/Xcp(0x02)
- `kModifyBitsMaxShift = 16`（docs §7.5.2.5 Shift 合法上界）
- `constexpr ParsePageProperties(raw)`（bits0-1/2-3/4-5 位拆解）

**结构体**：`ModifyBitsRequest`、`SetCalPageRequest`、`GetCalPageRequest/Response`、`GetPagProcessorInfoResponse{max_segment, properties}`、`GetSegmentInfoRequest`、`SegmentBasicInfo/SegmentStandardProperties/SegmentMappingInfo`、`using SegmentInfoData = variant<...>`、`GetSegmentInfoResponse{mode, data}`、`GetPageInfoRequest/Response`、`SetSegmentModeRequest`、`GetSegmentModeRequest/Response`、`CopyCalPageRequest{src_segment, src_page, dst_segment, dst_page}`。

### 3.2 CommandCodec（`src/command_codec.cpp` :405-552）

9 个 Encode 方法，报文布局逐字节对照 docs §7.5.2.5/§7.5.3 与 XCPlite `xcp.h` CRO 定义：

| 方法 | 报文布局 | 本地预检 |
|---|---|---|
| `EncodeModifyBits(shift, and, xor)` | `[EC][shift][AND W][XOR W]` 6B | shift>16 → InvalidArgument |
| `EncodeSetCalPage(mode, seg, page)` | `[EB][mode][seg][page]` 4B | mode 不含 ECU\|XCP 位 → InvalidArgument |
| `EncodeGetCalPage(access_mode, seg)` | `[EA][am][seg]` 3B | am ∉ {0x01,0x02} → InvalidArgument |
| `EncodeGetPagProcessorInfo()` | `[E9]` 1B | — |
| `EncodeGetSegmentInfo(mode, seg, info, mapidx)` | `[E8][mode][seg][info][mapidx]` 5B | mode>2 或 info>2 → InvalidArgument |
| `EncodeGetPageInfo(seg, page)` | `[E7][rsv][seg][page]` 4B | — |
| `EncodeSetSegmentMode(mode, seg)` | `[E6][mode][seg]` 3B | — |
| `EncodeGetSegmentMode(seg)` | `[E5][rsv][seg]` 3B | — |
| `EncodeCopyCalPage(request)` | `[E4][ss][sp][ds][dp]` 5B | — |

AND/XOR Mask 经 `WriteU16` 按 Session Byte Order（Intel/Motorola）写入；reserved 字节一律填 0。

### 3.3 ResponseParser（`src/response_parser.cpp` :421-523）

5 个 Parse 方法（入参均为剥掉 0xFF PID 后的 res_data，长度不足返回 nullopt）：

- `ParseGetCalPage`（≥3B，page@off2）、`ParseGetPagProcessorInfo`（≥2B）、`ParseGetPageInfo`（≥2B，PAGE_PROPERTIES 走 `ParsePageProperties` 位拆解）、`ParseGetSegmentMode`（≥2B，mode@off1）。
- `ParseGetSegmentInfo(res_data, mode)`：**按请求 Mode 分支**——Mode 0/2 ≥4B 读 DWORD 填 `SegmentBasicInfo`/`SegmentMappingInfo` variant；Mode 1 ≥5B 读 `[max_pages, address_extension, max_mapping, compression_method, encryption_method]`；非法 mode 防御性 nullopt。多余字节安全忽略。

### 3.4 Session（`src/session.cpp`）

- 新增 `std::optional<GetPagProcessorInfoResponse>` 缓存与 `SetPagProcessorInfo()`/`PagProcessorInfo()` 访问器（mutex 保护）。
- `EstablishConnection`/`CompleteDisconnection`/`Fail`/`Reset` 四处会话级清理点同步 `reset()` 缓存。

### 3.5 CommandExecutor（`src/command_executor.cpp` :952-1105）

- 9 个 Execute 方法复用既有 RunCommand → PerformAttempt → WaitForResponse → DispatchResponse 链。
- void 类（ModifyBits/SetCalPage/SetSegmentMode/CopyCalPage）直接 `(void)RunCommand(...)`；GET_* 五个解析响应，nullopt → MalformedPacket（消息含实际/期望长度）。
- **Optional 降级**：GET_CAL_PAGE/GET_PAG_PROCESSOR_INFO/GET_SEGMENT_INFO/GET_PAGE_INFO/GET_SEGMENT_MODE catch `IsCmdUnknown(e)` → 返回 `std::nullopt`（仿 GET_DAQ_PROCESSOR_INFO 先例），其他异常透传。
- **超时恢复钩子**（:446-453）追加：

```cpp
if (cmd == CommandCode::ModifyBits) { RestoreUploadMta(); }
```

即 SYNCH 成功后重放 SET_MTA 再重试 MODIFY_BITS（MODIFY_BITS 依赖 MTA 隐含状态，docs §7.5.2.5）。

### 3.6 MemoryAccess（`src/memory_access.cpp` :302-325）

`ModifyBits(address, extension, shift, and_mask, xor_mask)`：连接检查 → shift≤16 → 地址+4 不越 40bit 空间 → `ExecuteSetMta(extension, address)` + `ExecuteModifyBits(...)`。语义 `Result = (Value & (AND<<S)) ^ (XOR<<S)`，执行后 MTA 不变。

### 3.7 XcpMaster（`src/xcp_master.cpp`）

**Connect() 扩展**（:148-213）：GET_STATUS 之后，若 CONNECT RESOURCE 含 CAL/PAG(bit0)，探测 `GET_PAG_PROCESSOR_INFO` 并缓存（`m_pag_processor_info_queried_`/`m_pag_processor_info_` + `Session::SetPagProcessorInfo`）；Slave 回 ERR_CMD_UNKNOWN 或其他异常只清缓存、**不阻断连接**。

**公共 API**（:781-864，共 10 个）：

```cpp
bool HasCalPagResource() const;
std::optional<GetPagProcessorInfoResponse> QueryPagProcessorInfo();  // 懒查+缓存
void SetCalPage(CalPageModeBit, uint8 segment, uint8 page);
std::optional<GetCalPageResponse> GetCalPage(CalPageAccessMode, uint8 segment);
std::optional<GetSegmentInfoResponse> GetSegmentInfo(SegmentInfoMode, uint8 segment,
        SegmentInfoSelector, uint8 mapping_index = 0);
std::optional<GetPageInfoResponse> GetPageInfo(uint8 segment, uint8 page);
void SetSegmentFreeze(bool freeze, uint8 segment);   // 内部转 kFreeze/kNone
std::optional<GetSegmentModeResponse> GetSegmentMode(uint8 segment);
void CopyCalPage(const CopyCalPageRequest&);
void ModifyBits(Address, AddressExtension, uint8 shift, uint16 and_mask, uint16 xor_mask);
```

统一前置检查（匿名 namespace `RequireCalPag`）：未连接 → InvalidState；CAL/PAG 未置位 → UnsupportedFeature（本地拒绝，不发命令）。`ModifyBits` 委托 `MemoryAccess::ModifyBits`。

## 4. 测试

### 4.1 新增用例（27 个）

- **`tests/command_codec_test.cpp`**（8 个，:329-441）：ModifyBits Intel/Motorola 黄金报文与 shift 0/16 边界、shift>16 拒绝；SetCalPage 各 Mode 组合与不含 ECU|XCP 拒绝；GetCalPage 布局与非法 Access Mode 拒绝；E9/E8/E7/E6/E5/E4 报文布局；GetSegmentInfo mode/info 越界拒绝。
- **`tests/response_parser_test.cpp`**（8 个 `ParseCalibrationResponses`，:484-582）：GET_CAL_PAGE 逻辑页与截断防护、PAG_PROCESSOR_INFO 解码、GET_SEGMENT_INFO Mode0 Intel/Motorola、Mode1 五字段顺序、Mode2 variant、GET_PAGE_INFO 位拆解（0x27 三组访问者）、GET_SEGMENT_MODE off1 读取。
- **`tests/xcp_master_integration_test.cpp`**（11 个 `XcpCalibration`，:802-994）：Connect 探测 PAG 信息、无分页 Slave 降级不阻断、CAL/PAG 缺失时三类 API 本地拒绝且零发包、ModifyBits SET_MTA 序列 + 位操作读回验证（0xFFFFFFFF ∧ 0x00FF00 ⊕ 0x000F0000 = 0x000FFF00）、ModifyBits 超时→SYNCH→重放 SET_MTA→重试的完整报文顺序断言、SET_CAL_PAGE 线上黄金报文、GET_CAL_PAGE/GET_SEGMENT_INFO/GET_PAGE_INFO/GET_SEGMENT_MODE 往返、COPY_CAL_PAGE ERR_WRITE_PROTECTED 透传为 ProtocolError、未连接调用返回 InvalidState。

### 4.2 Mock Slave 测试基础设施扩展

`MockXcpSlave` 新增：`SetResourceMask(mask)`（CONNECT RESOURCE 可配）、`SetResponse(cmd, response)`（脚本注入优先于内置处理）、`DropNthCall(cmd, n)`（第 n 次调用不回包，用于精准制造超时）、内置 `modifyBits()` 处理（校验 len/s、Intel 解 AND/XOR、对 MTA 处 4 字节做位运算写回、MTA 不自增）。

### 4.3 顺带修复的既有隐患

`MockXcpSlave.connectResponse()` 首字节原硬编码 0x15，恰好同时是"合法的 protocol_layer 风格值"与"被 `ParseConnectResponse` 当作 resource_mask 读走的 res_data[0]"——RESOURCE 字节位置此前从未被测试暴露。现已改为 body[0]=`m_resource_mask_`（默认 0x15 = CAL/PAG|DAQ|PGM），尾部两字节摆正为 PROTOCOL_LAYER_VERSION(0x15)/TRANSPORT_LAYER_VERSION(0x10)。

## 5. 验证结果

```text
cmake --build cmake-build-release --config Release     # 零错误零警告新增
ctest --test-dir cmake-build-release -C Release        # 100% tests passed,
                                                       # 0 failed out of 447
```

（1 个 Skipped 为既有参数化用例 `AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`，与本批无关。）

## 6. 与计划的偏差与补充决策

1. **GET_SEGMENT_INFO 的 Mode 传递**：采用"executor 把请求 Mode 作为参数传给 `ParseGetSegmentInfo(res_data, mode)`"方案（计划 §11 风险 1 预留的两个选项之一），避免 variant 自描述歧义。
2. **XcpMaster API 命名微调**：`SetSegmentMode(bool, uint8)` 对外命名为 `SetSegmentFreeze(bool freeze, uint8 segment)`，语义更直白；SegmentMode 位转换封装在实现内。
3. **`QueryPagProcessorInfo()` 懒查缓存**：Connect 探测失败或未探测时，首次查询会补发一次 GET_PAG_PROCESSOR_INFO 并缓存结果（nullopt 表示 Slave 不支持），与既有 DAQ processor info 取证缓存先例一致。
4. **测试基线取法教训**：`MockTransport::Reset()` 会清空响应脚本，标定测试断言"本轮报文"一律改用 `SentPackets().size()` 基线偏移，不 Reset。
5. 其余均按计划 C1–C7 落地，无范围增减；未引入新依赖，未改动任何既有公共 API 签名。
