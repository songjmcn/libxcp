# XCP V1.4 升级与 V1.3 向下兼容修复方案

> 方案日期：2026-10-21  
> 原始审计：`code-plan/XCP_1.3.0_规范符合性审计报告.md`  
> 现有规范基线：`docs/XCP_1.3.0_document.md`  
> V1.4 交互参考：`thirdparty/XCPlite/` 当前配置 `XCP_PROTOCOL_LAYER_VERSION=0x0104`；其实现不是规范符合性的最终裁决依据  
> 方案范围：协议目标升级至 V1.4、支持 V1.3 降级、修复 F-01～F-12、能力声明收口和验证体系  
> 本文只给出修复设计与验收标准，不包含代码修改

## 1. 已确认决策

1. **目标版本**：Master 的目标协议版本由 XCP V1.3 升级为 V1.4。
2. **低版本范围**：只承诺支持 V1.3～V1.4；V1.0～V1.2 不在本轮范围。
3. **版本发现**：CONNECT 后优先通过 V1.4 `LEVEL_1_COMMAND / GET_VERSION` 获取完整 Protocol/Transport Major、Minor；命令不支持且 A2L 无可靠版本时，按 V1.3 保守降级并标记为“推定版本”。
4. **高版本策略**：Slave 返回高于 V1.4、但 Protocol Major 仍兼容时允许连接，只启用 Master 已验证的 V1.4 能力上限并产生告警；不得自动尝试未知高版本功能。
5. **协议与厂商解耦**：Protocol Version 决定标准命令和字段语义；XCPlite Profile 只描述已证实的厂商差异，不能用 XCPlite Profile 代替 V1.4。
6. **V1.4 依据**：当前阶段参照 XCPlite 的 V1.4 交互设计和集成验证；在取得官方 XCP V1.4 规范前，只能声明“XCPlite V1.4 兼容”，不得声明完整 ASAM XCP V1.4 符合。
7. **原审计问题**：继续覆盖 F-01～F-12，并同步收口 README、模块文档和能力声明；未实现 Optional 能力只保留后续路线图。
8. **验证强度**：单元测试 + XCPlite V1.4 集成 + V1.3 对端回归 + 至少一个独立标准对端；没有官方 V1.4 规范和独立对端证据时不得作通用符合性声明。
9. **实施原则**：分批修复，每批独立验证、独立回滚；不得将版本升级与所有既有缺陷修复堆入一个批次。
10. **代码原则**：C++20、Windows/Linux/macOS；不修改 `thirdparty/`；新增/修改接口必须配套测试和中文 Doxygen 注释。

## 2. 目标与非目标

### 2.1 修复目标

- 增加 V1.4 `LEVEL_1_COMMAND / GET_VERSION` 的编码、执行和响应解析，得到完整 Protocol/Transport 版本。
- 建立 `ReportedVersion`、`EffectiveVersion` 和 `VersionSource`，明确区分 Slave 报告值、Master 实际采用语义以及版本证据来源。
- 以 V1.4 作为默认能力上限，同时对不支持 GET_VERSION 的 V1.3 Slave 保守降级。
- 消除 AG、DOWNLOAD、GET_STATUS、GET_ID、MIN_DAQ、DTO Identification、Byte Order 等确定的 wire-level 偏差，并重新判断哪些差异属于 V1.3/V1.4 标准演进，哪些才是 XCPlite 方言。
- 将版本、CONNECT、GET_DAQ_PROCESSOR_INFO、GET_DAQ_RESOLUTION_INFO 的协商结果形成单一可信能力快照，并贯穿 Codec、Parser、MemoryAccess、DAQ 编排和 MeasurementSession。
- 为分段 Seed & Key、隐式 MTA 和错误恢复建立可证明的命令级/事务级策略。
- 用版本化 golden vector、V1.3/V1.4 双版本集成和独立对端测试替代“自有实现验证自有实现”的闭环证据。

### 2.2 非目标

本轮不承诺：

- XCP V1.0～V1.2；
- XCP V1.5 或更高版本的新能力；高版本 Slave 仅按 V1.4 能力上限运行；
- 因升级 V1.4 而自动补齐所有 V1.4 Optional Command；除 GET_VERSION 外，是否实现 DTO_CTR_PROPERTIES、Packed Mode 等能力必须由明确需求驱动；
- SET_REQUEST、BUILD_CHECKSUM、DOWNLOAD_NEXT、DOWNLOAD_MAX、Block/Interleaved Mode；
- WRITE_DAQ_MULTIPLE、GET_DAQ_LIST_MODE、GET_DAQ_CLOCK；
- STIM 数据面、Programming 命令族、完整 Time Correlation；
- PID_OFF、DTO Counter 数据面、SERV_TEXT 自动重组等完整扩展能力。

这些能力必须在版本化能力矩阵中标为 Unsupported/Planned，不能因为 Slave 版本较高而自动视为可用。

## 3. 总体设计

### 3.1 协议版本与厂商 Profile 分层

建议增加以下强类型概念：

- `XcpProtocolVersion { major, minor }`：完整的协议版本；
- `VersionSource`：`GetVersion`、`A2l`、`Assumed13`；
- `ReportedVersion`：Slave/A2L 报告的原始版本；
- `EffectiveVersion`：Master 实际采用的标准语义，仅允许 V1.3 或 V1.4；
- `CompatibilityProfile::Standard`：不启用厂商差异；
- `CompatibilityProfile::Xcplite`：只启用经过测试确认的 XCPlite 差异。

版本选择规则：

1. CONNECT 先取得 Protocol/Transport Major、Byte Order、AG 和容量；CONNECT 的单字节版本不得被错误解释为完整 Minor。
2. Protocol Major 不为 1 时拒绝连接，并报告 `UnsupportedProtocolVersion`。
3. Major=1 时发送 V1.4 `LEVEL_1_COMMAND / GET_VERSION`：
   - 成功：保存完整 ReportedVersion；
   - `ERR_CMD_UNKNOWN`：继续检查可靠 A2L 版本；
   - 其他协议错误或畸形响应：连接失败，禁止静默降级；
   - 超时：按命令恢复矩阵处理；恢复后仍无确定结果则连接失败，不能把传输故障伪装成 V1.3。
4. GET_VERSION 不支持且 A2L 无可靠版本时，设置 `ReportedVersion=unknown`、`EffectiveVersion=1.3`、`VersionSource=Assumed13`，日志和 API 必须暴露“推定”状态。
5. 报告 V1.3：`EffectiveVersion=1.3`；报告 V1.4：`EffectiveVersion=1.4`；报告同 Major 且高于 V1.4：保留 ReportedVersion，但将 EffectiveVersion 封顶为 V1.4并告警。
6. 明确报告低于 V1.3时拒绝连接；不得按 V1.3 冒险运行。
7. Protocol 与 Transport 版本分别保存、分别检查；Transport 的允许组合需建立矩阵，不能只校验 Protocol。

Profile 约束：

- Profile 必须由调用方在连接前显式选择，默认 `Standard`；
- 版本可自动发现，Profile 不可根据单帧内容或版本号自动猜测；
- `Xcplite` 不等于 V1.4，Standard V1.4 与 Xcplite V1.4 必须是两个独立维度；
- Profile 不得改变 CONNECT 协商出的 Byte Order、AG、MAX_CTO/MAX_DTO；
- Profile 只允许描述已有证据的厂商差异；标准版本演进必须放在 Version Policy 中；
- 当前阶段 XCPlite V1.4 是默认集成目标，但公开 API 默认仍使用 Standard Profile；
- Profile 和 EffectiveVersion 在连接完成后冻结，断连后才能重新选择。

### 3.2 会话版本与能力快照

在现有 `SessionParameters` 基础上形成会话级只读快照：

| 能力 | 来源 | 消费者 |
|---|---|---|
| Protocol/Transport Major | CONNECT | Version Resolver |
| 完整 Protocol/Transport Version | GET_VERSION；可靠 A2L；V1.3 fallback | Version Policy、Codec、Parser、Capability Gate |
| Byte Order、AG、MAX_CTO、MAX_DTO、Resource | CONNECT | Codec、Parser、MemoryAccess、DAQ Planner、DTO Decoder |
| Block/Interleaved 参数 | GET_COMM_MODE_INFO | Executor/后续扩展；本轮仍不启用 |
| `MIN_DAQ`、DAQ Properties、`DAQ_KEY_BYTE` | GET_DAQ_PROCESSOR_INFO | Dynamic DAQ、MeasurementSession、DTO Layout |
| Entry Granularity、Max Entry Size、Timestamp Mode/Ticks | GET_DAQ_RESOLUTION_INFO | DAQ 预检、Planner、DTO Layout、Timestamp Converter |
| 当前状态/保护 | GET_STATUS | Session/API |
| 方言行为 | CompatibilityProfile | 仅已确认的 XCPlite 兼容分支 |

设计要求：

- 快照明确包含 ReportedVersion、EffectiveVersion、VersionSource、是否发生版本封顶以及版本告警；
- 版本解析成功或 V1.3 保守降级完成前，会话保持 Connecting，不得对外暴露为 Connected；
- 快照只能由成功响应或规定的 V1.3 fallback 更新；断连、Fail、Reset 时清空；
- V1.4 专属命令必须经 `EffectiveVersion >= 1.4` 和独立能力条件双重放行；版本高不代表 Optional 能力必然存在；
- A2L 与 GET_VERSION 冲突时，以运行时 GET_VERSION 为真值并记录 Error；受影响功能默认阻断，不能静默择一；
- MeasurementSession 不再要求调用方重复手工注入已经能从 Slave 取证的参数；
- 调用方显式设置与 Slave 协商结果冲突时立即抛 `InvalidArgument`/`InvalidState`；
- `DAQ_KEY_BYTE` 或 Resolution 不可用时，只允许执行不依赖该信息的 API；通用 MeasurementSession `Prepare()` 必须拒绝猜测。

### 3.3 连接与能力选择数据流

```text
CONNECT
  -> Major Version + ByteOrder/AG/MAX_CTO/MAX_DTO/Resource
  -> Major != 1: reject UnsupportedProtocolVersion
  -> Major == 1: LEVEL_1_COMMAND / GET_VERSION
       ├─ success: ReportedVersion = exact runtime version
       ├─ ERR_CMD_UNKNOWN + reliable A2L: ReportedVersion = A2L version
       ├─ ERR_CMD_UNKNOWN + no reliable A2L: EffectiveVersion = 1.3 (Assumed13)
       └─ malformed/other error/unrecoverable timeout: fail connection
  -> EffectiveVersion = clamp supported range [1.3, 1.4]
  -> apply Standard/Xcplite Profile independently
  -> [GET_DAQ_PROCESSOR_INFO]
  -> [GET_DAQ_RESOLUTION_INFO]
  -> freeze SessionCapabilities

Memory API
  -> version/capability gate
  -> byte_count/AG 校验
  -> ElementCount
  -> XCP 地址按 ElementCount 推进
  -> Codec 按 EffectiveVersion + AG 生成报文

MeasurementSession::Prepare
  -> 读取版本、Session 与 DAQ 能力快照
  -> 从 MIN_DAQ 开始规划 Dynamic Lists
  -> 从 DAQ_KEY_BYTE 生成版本化 DtoFrameLayout
  -> 应用 Profile 中经过声明的厂商差异
  -> 下发 Dynamic DAQ
  -> 冻结该代际 Layout/Route

DTO
  -> 按 EffectiveVersion + ByteOrder + DAQ_KEY_BYTE 解码
  -> 按 (DAQ List, ODT) 路由
  -> Payload Decoder
  -> Physical Conversion
```

## 4. 分批实施方案

## 批次 A：规范裁决与测试 Oracle

### 4.1 目的

先建立 V1.3/V1.4 的可验证差异基线，消除中文 V1.3 摘要、V1.4 新行为与 XCPlite 实现之间的歧义，避免把“版本演进”和“厂商方言”混为一谈。

### 4.2 工作项

1. 获取并逐表复核 ASAM XCP V1.4 官方英文规范，重点确认：
   - `LEVEL_1_COMMAND` 与 `GET_VERSION` 的命令码、请求/响应布局、错误语义和 Mandatory/Optional 属性；
   - CONNECT 版本字段在 V1.3/V1.4 中的定义；
   - Protocol Version 与 Transport Version 的兼容规则；
   - V1.3→V1.4 的所有 wire-level 变化及新增 Optional 能力。
2. 对照 ASAM XCP V1.3 官方英文规范继续裁决：
   - Address Granularity 与 MTA 地址单位；
   - DOWNLOAD 在 BYTE/WORD/DWORD AG 下的填充和最大元素数；
   - GET_ID 请求/响应全部模式；
   - GET_STATUS Current Session Status 位；
   - GET_DAQ_PROCESSOR_INFO 的 MIN_DAQ、DAQ_KEY_BYTE；
   - Absolute/Relative Byte/Relative Word/Aligned Identification Field；
   - Timestamp Byte Order；
   - Seed/Unlock 分段和错误恢复语义。
3. 建立 V1.3/V1.4“版本裁决表”：命令、字段 Position/Length/Byte Order、首次出现版本、适用前提、Mandatory/Optional、官方章节。
4. 将 XCPlite V1.4 实现另列为“实现参考差异表”，逐项标注：标准 V1.4、XCPlite 扩展、当前无法裁决。禁止把 `thirdparty/` 直接作为标准 oracle。
5. 为版本发现、F-01～F-12 及 V1.3/V1.4 差异准备 Intel/Motorola golden vectors；golden 数据必须来自规范表或独立标准实现，不能由待测 codec 自生成。

### 4.3 输出

- `code-plan/` 下增加 V1.3/V1.4 版本裁决记录；
- GET_VERSION 命令与兼容判定真值表；
- 测试夹具中的版本化 golden vector；
- GET_ID、Relative DTO 及其它疑似版本差异的最终归类。

### 4.4 完成门槛

- GET_VERSION 请求/响应和版本比较有官方表格级依据；
- F-01、F-02、F-03、F-06、F-07 均重新核对 V1.3/V1.4，不能沿用“仅 V1.3”的裁决；
- Standard V1.3、Standard V1.4 和 XCPlite Profile 三类行为不再混写；
- 若仍无法取得官方 V1.4 规范，允许继续实现“XCPlite V1.4 Compatibility”，但文档和 API 能力声明必须明确受限，通用 V1.4 符合性验收保持未完成。

---

## 批次 B：V1.4 版本发现与 V1.3 降级

### 版本命令编解码

1. 在命令模型中增加 `LEVEL_1_COMMAND` 与其 `GET_VERSION` 子命令；最终命令值和布局以批次 A 的 V1.4 裁决表为准。
2. `CommandCodec` 只负责编码；`ResponseParser` 按 V1.4 表解析完整 Protocol/Transport Major、Minor，不把 CONNECT 的单字节 Major 当完整版本。
3. Parser 必须拒绝长度不足、保留位非法和无法表示的版本响应；未知附加字段按规范决定保留还是拒绝。
4. 版本比较使用数值 Major/Minor，不使用字符串或 BCD 猜测；结构应可表达未来版本，但 EffectiveVersion 仅允许本库已验证版本。

### 版本解析器与会话状态

1. 增加集中式 Version Resolver，不允许各模块自行比较版本。
2. 连接阶段建议新增内部状态 `ConnectingVersion`；在版本裁决和必要能力查询结束前，`IsConnected()` 不得返回可执行常规业务命令的状态。
3. 保存：
   - Reported Protocol/Transport Version；
   - Effective Protocol/Transport Version；
   - VersionSource；
   - 是否从高版本封顶；
   - 是否为 V1.3 推定降级；
   - A2L/运行时冲突诊断。
4. 对外提供只读查询接口，使调用方可以区分“确认 V1.3”“推定 V1.3”“确认 V1.4”“高版本按 V1.4 运行”。

### 降级与错误处理

| 情况 | 处理 |
|---|---|
| CONNECT Protocol Major != 1 | 拒绝，`UnsupportedProtocolVersion` |
| GET_VERSION 成功，Protocol=1.3 | Effective=1.3 |
| GET_VERSION 成功，Protocol=1.4 | Effective=1.4 |
| GET_VERSION 成功，Protocol=1.x 且 x>4 | Effective=1.4，保留报告值并告警 |
| GET_VERSION 成功，Protocol<1.3 | 拒绝，不作冒险兼容 |
| GET_VERSION 返回 `ERR_CMD_UNKNOWN`，A2L 明确 1.3/1.4 | 使用 A2L，但标记 Source=A2l |
| GET_VERSION 返回 `ERR_CMD_UNKNOWN`，无可靠 A2L | Effective=1.3，Source=Assumed13 |
| GET_VERSION 畸形、返回其它错误 | 连接失败，不降级 |
| GET_VERSION 超时 | 执行可证明安全的 SYNCH 恢复；恢复后仍失败则连接失败 |
| A2L 与 GET_VERSION 冲突 | 运行时报告优先，记录错误并默认阻断受影响功能 |
| Transport Version 不在允许矩阵 | 拒绝或仅开放经验证子集；不得仅因 Protocol 可用而忽略 |

### 版本化能力门控

- 每个 V1.4 专属行为建立 `min_protocol_version`；未满足时在发帧前返回 `UnsupportedFeature`。
- Mandatory/Optional 与版本是两个维度：达到 V1.4 只说明命令存在于该版本，不代表所有 Optional 能力可用。
- 对 F-01～F-12 的解析和编码差异，统一由 EffectiveVersion 选择标准策略；只有非标准 XCPlite 差异才读取 Profile。
- 高版本 Slave 不允许触发未知命令或未知字段解释，只运行 V1.4 已验证策略。

### 涉及文件

- `include/libxcp/protocol_types.hpp`
- `src/protocol_types.cpp`
- `include/libxcp/command_codec.hpp`
- `src/command_codec.cpp`
- `include/libxcp/response_parser.hpp`
- `src/response_parser.cpp`
- `include/libxcp/session.hpp`
- `src/session.cpp`
- `include/libxcp/command_executor.hpp`
- `src/command_executor.cpp`
- `include/libxcp/xcp_master.hpp`
- `src/xcp_master.cpp`
- `src/a2l/a2l_parser.cpp`（只作为可选版本证据来源，不得伪造默认 0x0100）
- 对应 tests 文件

### 测试

- GET_VERSION 请求/响应 V1.4 golden vector；
- 成功返回 V1.3、V1.4、V1.5；
- Major 不兼容、明确低于 V1.3；
- `ERR_CMD_UNKNOWN` + 有/无 A2L；
- timeout、SYNCH 恢复、畸形长度、其它 ERR；
- A2L 与运行时一致/冲突；
- Protocol/Transport 版本组合矩阵；
- 连接未完成版本裁决前并发调用业务 API，应返回确定的 InvalidState；
- 断连重连时版本快照清空，并按新 Slave 重新探测；
- V1.3 Slave 不收到任何 V1.4 专属业务命令；
- V1.5 模拟 Slave 仅收到 V1.4 能力上限内命令。

### 批次 B 完成门槛

- 无论 XCPlite Profile 是否启用，都能独立报告 ReportedVersion 与 EffectiveVersion；
- V1.3 fallback 仅由规定路径触发，传输故障不会被误判为老版本；
- V1.3 与 V1.4 Mock/真实对端均完成连接和基础命令验证；
- 高版本封顶行为有日志、API 状态和测试证据；
- 现有 CONNECT API 的兼容影响已有迁移说明。

---

## 批次 C：基础传输语义修复（F-01、F-02、F-04）

### 4.5 F-01：AG 地址推进

#### 设计

XCP Address 字段按 Slave 地址单位计数；数据缓冲长度才按 `ElementCount × AG bytes` 计算。因此：

- `XcpAddress40::Advance` 应按 ElementCount 增加数值地址，不再乘 AG；
- 地址溢出检查按数值地址单位判断；
- byte API 仍检查 `byte_count % AG == 0`，再换算为 ElementCount；
- 分块读写下一块地址使用“已完成元素数”；
- Address Extension 不参与跨界进位，维持现有约束。

建议收敛接口，避免调用方再次传入 AG：将现有
`XcpAddress40::Advance(ElementCount elements, AddressGranularity ag) const noexcept`
改为只接受 ElementCount。该接口变更需全仓调用点和测试同步更新。

#### 涉及文件

- `include/libxcp/protocol_types.hpp`
- `src/protocol_types.cpp`
- `src/memory_access.cpp`
- `tests/protocol_types_test.cpp`
- `tests/memory_access_test.cpp`
- `tests/xcp_master_integration_test.cpp`

#### 测试

- AG=BYTE/WORD/DWORD，跨 2 个以上分块；
- 起始地址接近 `UINT32_MAX` 的合法/溢出边界；
- byte API 不可整除 AG 的负例；
- Mock Slave 记录每次 SET_MTA，断言地址序列按 ElementCount 前进。

### 4.6 F-02：DWORD DOWNLOAD 对齐

#### 设计

- DOWNLOAD 数据起始位置由 AG 决定，按批次 A 的官方表插入规定的 FILL；
- FILL 必须为 0；
- 最大元素数从“MAX_CTO - 命令头 - 对齐填充”计算，再除以 AG bytes；
- `EncodeDownload` 必须显式接收经过验证的 AG/布局上下文，不能继续只靠 Byte Order；
- SHORT_DOWNLOAD 是否有相同要求必须按官方表独立裁决，不能类推；
- MemoryAccess 和 CommandCodec 必须共用一个容量计算函数，避免一个扣填充、另一个不扣。

#### 涉及文件

- `include/libxcp/command_codec.hpp`
- `src/command_codec.cpp`
- `include/libxcp/memory_access.hpp`
- `src/memory_access.cpp`
- `src/command_executor.cpp`
- `tests/command_codec_test.cpp`
- `tests/memory_access_test.cpp`

#### 测试

- BYTE/WORD/DWORD × Intel/Motorola golden vectors；
- MAX_CTO 恰好容纳 N 个元素、差 1 字节、仅能容纳 1 个元素；
- FILL=0；
- 编码容量与分块容量完全一致；
- DWORD AG 真实 Slave/独立标准对端写入后读回一致。

### 4.7 F-04：GET_STATUS 位

#### 设计

将 `STORE_DAQ_REQ` 掩码修正为官方 bit 2；为全部 Session Status 位建立逐位测试，不再使用同时置多位的数据掩盖错误。

#### 涉及文件

- `src/response_parser.cpp`
- `tests/response_parser_test.cpp`
- 相关状态文档

#### 测试

分别只置 bit 7、6、3、2、0，断言只有对应字段为 true；保留位不应映射到任何已知字段。

### 4.8 批次 C 完成门槛

- F-01/F-02/F-04 新测试先失败、修复后通过；
- 原有 BYTE AG 行为无回归；
- AG=WORD/DWORD 跨块读写可通过独立协议对端验证；
- 不引入 XCPlite Profile 改动，便于独立回滚。

---

## 批次 D：版本化标准语义与 XCPlite Profile（F-03）

### 4.9 设计

1. Standard Profile 根据 EffectiveVersion 选择经批次 A 裁决的 V1.3 或 V1.4 GET_ID 布局；不得把所有版本统一解释为当前 XCPlite 布局。
2. 只有被证明不属于标准 V1.4 演进的 XCPlite 差异，才进入 `Xcplite` Profile；标准版本差异必须进入 Version Policy。
3. ResponseParser 不应在收到数据后试探多种布局；应由冻结的 EffectiveVersion + Profile 在解析前决定唯一策略。
4. `FetchA2lFromSlave`：
   - Standard V1.3/V1.4 分别支持其规范允许的 inline 和 MTA upload；
   - XCPlite Profile 只保留经 V1.4 集成验证的额外路径；
   - 压缩/加密若不实现，必须返回明确的 `UnsupportedFeature`，不能把标志位当普通长度处理。
5. Profile 与 EffectiveVersion 是正交输入；二者在连接完成后冻结。
6. 同一个 XcpMaster 实例断连后连接另一 Slave 时必须重新获取版本，不得复用旧解析策略。

### 4.10 涉及文件

- `include/libxcp/protocol_types.hpp` 或新增独立版本/兼容配置头
- `include/libxcp/xcp_master.hpp`
- `src/xcp_master.cpp`
- `include/libxcp/response_parser.hpp`
- `src/response_parser.cpp`
- `src/command_executor.cpp`
- `tests/response_parser_test.cpp`
- `tests/xcp_daq_test.cpp`
- `tests/xcplite_daq_test.cpp`

### 4.11 测试

- Standard Profile × EffectiveVersion V1.3/V1.4 的 GET_ID 全部模式 golden vectors；
- Xcplite V1.4 Profile 当前行为回归；
- 同一字节序列在不同版本/Profile 下得到预期结果或明确失败；
- 未知/压缩/加密模式负例；
- 连接后修改 Profile 或 EffectiveVersion 被拒绝；
- 断连后连接不同版本 Slave，解析策略按新版本重建；
- 禁止以解析失败触发自动 Profile fallback。

### 4.12 完成门槛

- 默认 Standard API 不再依赖 XCPlite 布局；
- V1.3 与 V1.4 标准路径均有独立 golden；
- XCPlite 集成测试显式构造 Xcplite Profile 后保持通过；
- 代码和文档不再使用“实然优先”替代版本化规范语义。

---

## 批次 E：DAQ 能力贯通与 DTO 标准化（F-05～F-08、F-12）

### 4.13 F-05：MIN_DAQ

#### 设计

- 缓存完整 `GetDaqProcessorInfoResponse`，不只缓存 Properties；
- Dynamic DAQ List 编号从 `MIN_DAQ` 开始；
- 规划器产出的列表号和 ALLOC_DAQ 的 count 分离：count 仍为分配数量，后续 ALLOC_ODT/ENTRY 和配置使用实际 List Number；
- 检查 `MIN_DAQ + count - 1` 的 16 位边界；
- Static/Predefined Lists 与 Dynamic 区间不能混用。

#### 测试

`MIN_DAQ` 取 0、1、255、接近上限；多列表；越界；Mock Slave 断言完整命令序列中的 List Number。

### 4.14 F-06/F-08：DAQ_KEY_BYTE 与标准 Identification Field

#### 设计

1. 用批次 A 的官方表修正 `IdentificationFieldType` 注释与 DTO 解码：
   - Absolute；
   - Relative Byte；
   - Relative Word；
   - Relative Word Aligned，包含标准 FILL/对齐。
2. `DtoFrameLayout` 不再由调用方手工提供 `id_field_bytes`；头长由标准 Identification 类型计算。
3. XCPlite 私有 `[relODT][0xAA][DAQ16 LE]` 必须成为 Profile 内单独的兼容布局，不得复用标准 RelativeByte 名称。
4. MeasurementSession::Prepare 自动读取 `DAQ_KEY_BYTE` 并构造布局。
5. `SetEnvelopeMode`：
   - 建议标记为兼容/高级覆盖入口，默认不再需要；
   - 传入值与已协商能力冲突时失败；
   - 不允许通过手填长度制造非标准布局。
6. Absolute 模式路由不再硬编码 DAQ List 0；根据 START 返回的 FIRST_PID 区间建立 PID→(List, ODT) 路由，并检查区间重叠。

#### 涉及文件

- `include/libxcp/protocol_types.hpp`
- `include/libxcp/daq/dto_envelope_types.hpp`
- `include/libxcp/daq/dto_envelope_decoder.hpp`
- `src/daq/dto_envelope_decoder.cpp`
- `include/libxcp/measurement/measurement_session.hpp`
- `src/measurement/measurement_session.cpp`
- `include/libxcp/measurement/measurement_planner.hpp`
- `src/measurement/measurement_planner.cpp`
- `include/libxcp/xcp_master.hpp`
- `src/xcp_master.cpp`
- DTO/Measurement/DAQ 相关测试

### 4.15 F-07：DTO Byte Order

#### 设计

- 用统一的 `ReadU16/ReadU32` 或等价强类型 helper 按 Session Byte Order 读取多字节 Identification/Timestamp；
- 单字节字段不受影响；
- Profile 不得覆盖 CONNECT 的 Byte Order；
- Timestamp 原始值解码与单位换算解耦，先正确读取 raw，再由 converter 使用 unit/ticks。

#### 测试

- Intel/Motorola × 8/16/32 bit timestamp；
- Intel/Motorola × Relative Word/Aligned；
- 同一数值不同字节序的镜像 golden vectors；
- 截断、对齐错误、保留位错误的负例。

### 4.16 F-12：Resolution 与模式约束

#### 设计

在下发 DAQ 之前统一预检：

- Entry 地址、长度与 DAQ Granularity 对齐；
- Entry 长度不超过 Max Entry Size；
- Timestamp Fixed 时禁止关闭 timestamp；
- 模式位组合满足官方约束；
- DTO header + payload 不超过 MAX_DTO；
- Unsupported 的 PID_OFF、DTO Counter、Alternating 应在首次配置前明确失败，不发送半套命令。

时间戳单位转换按官方 Unit/Ticks 计算；若规范取值仍无法权威映射，则保留 raw 并明确 `timestamp_valid=false`，不得猜测纳秒倍率。

### 4.17 批次 E 测试矩阵

| 维度 | 值 |
|---|---|
| 协议版本 | Effective V1.3 / Effective V1.4 |
| Profile | Standard / Xcplite |
| Byte Order | Intel / Motorola |
| Identification | Absolute / RelativeByte / RelativeWord / Aligned / XCPlite 私有 |
| MIN_DAQ | 0 / 非 0 / 边界 |
| Timestamp | 无 / 8 / 16 / 32 bit；fixed / configurable |
| DAQ List | 单列表 / 多列表 |
| Frame | 正常 / 截断 / 超长 / 错误 FILL / 未知 Key Byte |

### 4.18 批次 E 完成门槛

- Standard Profile 在 V1.3/V1.4 下适用的 Identification golden vectors 全通过；
- Motorola timestamp 正确；
- `MIN_DAQ != 0` 的动态配置成功；
- MeasurementSession 默认不再要求手工设置可协商的 Envelope 参数；
- XCPlite Profile 的集成测量保持通过；
- 独立 Standard V1.4 对端至少完成 Relative 与 Absolute 各一种闭环，V1.3 对端完成其适用布局回归。

---

## 批次 F：隐式状态与恢复（F-09～F-11）

### 4.19 F-09：SHORT_UPLOAD 后 MTA

#### 设计

- 成功 SHORT_UPLOAD 后，把 Executor 本地 MTA 更新到 `address + ElementCount`；
- 使用批次 C 修正后的地址单位；
- 失败/超时且结果未知时，不更新为“成功后的 MTA”；
- 若规范确认超时后的 Slave MTA 结果不可知，则标记本地 MTA invalid，下一条依赖 MTA 的命令必须先显式 SET_MTA。

#### 测试

SHORT_UPLOAD→UPLOAD、SHORT_UPLOAD→UPLOAD timeout/recovery、AG=BYTE/WORD/DWORD、地址边界。

### 4.20 F-10：Seed & Key 事务恢复

#### 设计

GET_SEED/UNLOCK 的分段状态属于事务级隐式上下文，不能由通用 `RunCommand` 盲重放当前分段。

建议策略：

- GET_SEED First 可按规范允许性决定是否安全重试；
- GET_SEED Remainder 超时：SYNCH 后从 First 重新启动整次 Seed 获取，不单独重放 Remainder；
- UNLOCK 任一中间分段超时：结果可能已推进，默认终止本次事务并报告 `OperationOutcomeUnknown`；如官方规范给出可安全重建方式，按其重建整次 Unlock；
- 错误 Key/Slave 断连继续维持 Session Failed；
- 为 Seed/Key 设置事务级最大重启次数，避免无限循环；
- Calculator 只在完整 Seed 成功获取后调用；事务重启后不得复用不确定的中间 Key offset。

最终策略必须以批次 A 的官方裁决为准。

### 4.21 F-11：命令恢复矩阵

为每个已实现命令维护显式元数据：

| 属性 | 含义 |
|---|---|
| ReplaySafety | Safe / UnknownOutcome / TransactionManaged |
| ImplicitState | None / MTA / DAQ Pointer / Seed Offset / Unlock Offset |
| PreAction | 重试前需执行的状态重建 |
| ErrorPolicy | BUSY/SEQUENCE/DAQ_ACTIVE 等错误的处理方式 |
| RetryBudget | 命令级或事务级重试上限 |

首轮只覆盖当前已实现命令。行为原则：

- 修改类命令响应超时不得盲重放；
- Query 类命令只有在重建隐式状态后才可重试；
- BUSY/PGM_ACTIVE 是否按 t7 重试由官方要求和产品策略决定，并设总时限；
- SEQUENCE 只有存在确定 Pre-Action 时才能自动恢复，否则向上报告；
- 所有自动恢复必须保留原始 Command、Error Code、重试次数和 Transport Error。

### 4.22 涉及文件

- `include/libxcp/command_executor.hpp`
- `src/command_executor.cpp`
- `src/xcp_master.cpp`
- `include/libxcp/xcp_error.hpp`
- `tests/recovery_test.cpp`
- `tests/seed_key_test.cpp`
- `tests/memory_access_test.cpp`

### 4.23 批次 F 完成门槛

- 每个已实现命令在恢复矩阵中有唯一策略；
- Seed/Unlock 每一个分段位置都有 timeout 测试；
- 不存在通用重试路径绕过事务管理器的情况；
- SHORT_UPLOAD 与 UPLOAD 的 MTA 状态连续且可验证；
- 超时、恢复失败、结果未知三种错误可由 API 调用方明确区分。

---

## 批次 G：声明收口与跨平台验收

### 4.24 文档调整

更新以下内容：

- `README.md`：定位改为“目标 XCP V1.4、向下兼容 V1.3 的 Master 已实现子集”，移除“完整”“封板”等超出证据的表述；
- `docs/modules/协议核心.md`：增加版本发现、降级、高版本封顶及实际命令矩阵；
- `docs/modules/daq解码.md`：区分 V1.3/V1.4 标准 Identification 与 XCPlite Profile；
- `docs/modules/测试.md`：说明 Unit/Mock、XCPlite V1.4、V1.3 回归和独立标准对端证据等级；
- `include/libxcp/protocol_types.hpp`：修正“枚举均已完整实现”的不实注释；
- 公开 API Doxygen：说明 Reported/Effective Version、VersionSource、默认 Standard Profile、兼容差异和 Unsupported 能力。

建议新增版本化能力矩阵：

| 功能 | Standard V1.3 | Standard V1.4 | Xcplite V1.4 Profile | 验证等级 |
|---|---|---|---|---|
| CONNECT/STATUS/Memory | Supported | Supported | Supported | Unit + 双版本 integration + independent |
| GET_VERSION | 不适用/可返回 Unknown | Supported | Supported | golden + XCPlite + independent V1.4 |
| GET_ID | Supported | Supported | Compatible dialect（若仍有差异） | versioned golden + peers |
| Dynamic DAQ/DTO | Supported subset | Supported subset | Compatible dialect | matrix + peers |
| STIM/Programming/完整 Time Correlation | Unsupported | Unsupported | Unsupported | N/A |

### 4.25 构建与验证

每次完整编译遵循仓库规则，先 clean，再构建 Release。最终至少完成：

1. Windows Release 全量核心测试；
2. Linux Release 全量核心测试；
3. macOS Release 全量核心测试；
4. XCPlite V1.4 Profile 的 UDP/A2L/DAQ/Measurement 集成；
5. V1.3 对端回归：CONNECT、版本 fallback、STATUS、Memory、GET_ID、DAQ/DTO；
6. 独立 Standard V1.4 对端：
   - CONNECT + GET_VERSION + STATUS；
   - AG=BYTE 和至少一种 AG>1 内存跨块读写；
   - GET_ID；
   - Dynamic DAQ，且 `MIN_DAQ != 0` 或可配置模拟；
   - 至少一种 Relative DTO、Absolute DTO 和 Motorola timestamp；
7. 高于 V1.4 的模拟/真实对端验证“报告原版本、Effective 封顶、无未知功能探测”；
8. Sanitizer/动态分析可作为附加门槛，但不能替代协议测试。

### 4.26 声明门槛

- 有官方 V1.4 规范裁决并通过独立 Standard V1.4 对端后，才能声明已验证能力“符合 XCP V1.4”；
- 若只有 XCPlite 证据，只能声明“XCPlite V1.4 Compatibility”；
- V1.3 向下兼容必须由真实或独立 V1.3 对端验证，不能只靠 Mock fallback；
- 未覆盖的版本、Identification、Byte Order、AG 或命令必须标为“未验证”，不能从相邻组合外推；
- XCPlite 测试结果不能替代标准符合性。

## 5. 版本升级与 F-01～F-12 追踪矩阵

| ID | 修复批次 | 核心动作 | 主要测试 | 完成判据 |
|---|---|---|---|---|
| V-01 | A/B | 获取 V1.4 规范依据并实现 GET_VERSION | request/response golden | 可读取完整 Protocol/Transport 版本 |
| V-02 | B | Reported/Effective/Source 版本解析 | V1.3/V1.4/V1.5/错误矩阵 | 版本状态唯一且可查询 |
| V-03 | B | V1.3 fallback 与高版本封顶 | ERR_CMD_UNKNOWN/A2L/timeout | 不把故障误判为低版本 |
| V-04 | B/G | 版本化能力门控与声明 | V1.3/V1.4 双对端 | 只发送适用版本命令 |
| F-01 | C | 地址按 Element 推进 | AG×跨块×边界 | SET_MTA 地址序列正确 |
| F-02 | C | DWORD DOWNLOAD 对齐与容量 | AG×ByteOrder golden | 标准对端写后读回一致 |
| F-03 | D | 版本化 GET_ID + Xcplite Profile | 版本×Profile golden/integration | 默认不再使用方言 |
| F-04 | C | `STORE_DAQ_REQ=bit2` | 逐位测试 | 每个位独立正确 |
| F-05 | E | Dynamic List 从 MIN_DAQ 起 | 非零/边界 MIN_DAQ | 全命令 List Number 正确 |
| F-06 | E | 重写标准 Relative DTO | 版本×四种 ID 矩阵 | 标准 vectors 全通过 |
| F-07 | E | DTO 多字节按 Byte Order | Intel/Motorola timestamp | raw value 一致 |
| F-08 | E | DAQ_KEY_BYTE 自动贯通 | Prepare 自动布局 | 无需手填协商信息 |
| F-09 | F | SHORT_UPLOAD 同步/失效 MTA | 组合命令+timeout | 恢复无旧 MTA |
| F-10 | F | Seed/Key 事务级恢复 | 每分段 timeout | 无 offset 盲重放 |
| F-11 | F | 显式命令恢复矩阵 | Error×Command 参数化 | 每命令策略唯一 |
| F-12 | E | Granularity/Size/Mode 前置校验 | 版本×正/负能力组合 | 非法配置上线前失败 |

## 6. API 与兼容性管理

### 6.1 预期 API 变化

可能涉及：

- `XcpMaster` 增加包含 `CompatibilityProfile`、版本策略和可选 A2L 版本证据的配置入口；
- 增加只读版本查询：Reported Protocol/Transport Version、Effective Version、VersionSource、警告状态；
- `SessionParameters` 增加版本与 DAQ Processor/Resolution 能力快照，或拆分为独立不可变结构；
- 新增 GET_VERSION 执行接口仅供连接流程/诊断使用，常规调用方不应绕过 Version Resolver 改写会话版本；
- `XcpAddress40::Advance` 移除 AG 参数；
- `CommandCodec::EncodeDownload` 增加版本、AG/布局上下文；
- MeasurementSession 默认自动取证，手工 Envelope 设置降级为高级兼容入口；
- DTO 类型增加明确的 XCPlite 私有 Identification 类型，避免冒用标准枚举。

### 6.2 源码兼容

- 优先通过默认配置保留现有构造调用的源码可编译性；默认目标为 V1.4，连接时自动探测并可降级 V1.3；
- 现有同步 `Connect()` 的成功语义将扩大为“CONNECT + 版本裁决 + 必要能力快照完成”，超时预算和错误类型可能变化，必须写迁移说明；
- 默认 Profile 从隐式 XCPlite 行为切换为 Standard，这是有意的行为修正；
- 依赖 XCPlite 的示例和测试必须显式选择 Profile；
- 以前读取 `SessionParameters::protocol_version` 单字节的代码必须迁移到完整版本接口；旧字段只能作为 CONNECT Major 原始值保留或废弃，不能继续表示完整版本；
- 若项目有稳定 ABI 承诺，应在实施前决定主版本升级或 PImpl；当前仓库未见明确 ABI 政策，不能自行假定。

### 6.3 迁移提示

发布说明必须包含：

- Master 默认协议目标升级为 V1.4、支持 V1.3 fallback 的具体条件；
- ReportedVersion 与 EffectiveVersion 的区别，以及高版本封顶告警；
- GET_VERSION 不支持且无 A2L 时会显示“Assumed V1.3”，不是 Slave 明确声明；
- 如何为现有 XCPlite 用户启用 Profile；
- `Connect()` 超时/错误语义和版本不兼容错误；
- `SetEnvelopeMode` 等手工配置入口的变化；
- AG 地址单位修正可能暴露旧调用方的错误假设；
- 新增的严格预检可能把过去由 Slave 拒绝的配置提前变成本地异常。

## 7. 测试文件规划

优先扩展现有测试，避免为同一模块建立重复目标：

- `tests/protocol_types_test.cpp`：完整版本比较、地址推进、能力类型；
- `tests/command_codec_test.cpp`：LEVEL_1/GET_VERSION、DOWNLOAD 和标准 DTO 相关 golden；
- `tests/response_parser_test.cpp`：GET_VERSION、STATUS、版本化 GET_ID、Processor/Resolution；
- `tests/session_test.cpp`：ConnectingVersion、版本快照冻结/清空、重连；
- `tests/xcp_master_test.cpp` 或版本专项测试：V1.3 fallback、V1.4、V1.5 封顶、A2L 冲突；
- `tests/memory_access_test.cpp`：AG 分块、MTA、版本门控；
- `tests/recovery_test.cpp`：GET_VERSION timeout/SYNCH 与通用恢复矩阵；
- `tests/seed_key_test.cpp`：分段 timeout；
- `tests/xcp_daq_test.cpp`：版本×MIN_DAQ、能力贯通、Absolute PID 路由；
- `tests/dto_envelope_decoder_test.cpp`：V1.3/V1.4 适用布局与 Byte Order；
- `tests/measurement_planner_test.cpp`：非零起始 List、Entry 限制；
- `tests/measurement_session_test.cpp` 与 `tests/measurement_session_hardening_test.cpp`：版本化自动取证、冲突配置、代际；
- `tests/xcplite_daq_test.cpp`、`tests/xcplite_measurement_test.cpp`：显式 XCPlite V1.4 Profile 回归；
- 独立 V1.3/V1.4 标准对端测试建议建立单独可选 CMake target，避免没有外部对端时阻塞常规 Unit Build。

测试用例命名应直接体现规范条件，例如：

- `GetVersionParsesProtocolAndTransportVersion`
- `UnknownGetVersionFallsBackToAssumedV13`
- `TimeoutDoesNotMasqueradeAsLegacyVersion`
- `NewerMinorVersionIsCappedAtV14WithWarning`
- `AddressAdvanceUsesElementsForWordAg`
- `DownloadDwordAgInsertsRequiredFill`
- `GetStatusStoreDaqReqUsesBitTwo`
- `DynamicDaqStartsAtNegotiatedMinDaq`
- `RelativeWordTimestampUsesMotorolaByteOrder`
- `GetSeedRemainderTimeoutRestartsTransaction`
- `StandardProfileNeverFallsBackToXcpliteGetId`

## 8. 风险与回滚

| 风险 | 缓解 |
|---|---|
| 缺少官方 V1.4 规范，误把 XCPlite 当标准 | 批次 A 将实现参考与规范依据分栏；未取得官方证据前只声明 XCPlite V1.4 Compatibility |
| GET_VERSION 本身在老 Slave 不支持 | 仅 `ERR_CMD_UNKNOWN` 进入 A2L/V1.3 fallback；timeout/畸形/其它 ERR 不降级 |
| CONNECT 时延和失败面增加 | 为版本阶段设置独立预算与诊断；文档化 `Connect()` 新语义 |
| 高版本 Slave 引入未知语义 | 保留 ReportedVersion，EffectiveVersion 封顶 V1.4，禁止未知功能探测 |
| A2L 与运行时版本冲突 | GET_VERSION 为运行时真值，记录冲突并阻断受影响功能 |
| 官方规范与中文 V1.3 摘要不一致 | 批次 A 先裁决；记录官方章节与版本 |
| 修复标准行为破坏 XCPlite | Standard/Profile 正交 + XCPlite V1.4 独立集成测试 |
| 能力快照改动范围过大 | 分批引入，只读快照先行，不同时重构无关模块 |
| Address 语义修复导致旧测试大量失败 | 将“旧假设测试”改为规范 golden，不为保测试保留错误语义 |
| DTO 组合矩阵膨胀 | 参数化测试覆盖版本×Profile×正交维度，少量独立端到端组合 |
| Seed/Key 事务恢复无法安全自动化 | 宁可返回明确 UnknownOutcome，也不盲重放 |
| 独立对端不可获得 | 标记验证阻塞；不得降低结论为“已符合” |

回滚单位为批次：B、C、D、E、F 不得互相混合提交。批次 E 内建议再按 MIN_DAQ、DTO standard、capability validation 三个提交拆分。

## 9. 实施顺序与依赖

```text
A V1.3/V1.4 规范裁决 / Golden Oracle
 └─> B GET_VERSION + Version Resolver + V1.3 fallback
       ├─> C AG + DOWNLOAD + STATUS
       └─> D 版本化标准语义 + Profile + GET_ID
             └─> E MIN_DAQ + DTO + Capability
C + B
 └─> F 隐式状态 + Recovery
B + C + D + E + F
 └─> G 文档收口 + 三平台 + V1.3/V1.4/XCPlite 验收
```

- A 是所有实现批次的硬前置，尤其必须先裁决 GET_VERSION 和 V1.3/V1.4 差异；
- B 是所有业务修复的版本基础，必须先稳定版本快照和门控；
- C 可在 B 接口冻结后并行开发，但测试必须覆盖 Effective V1.3/V1.4；
- D 依赖 A/B，负责把“版本差异”和“厂商差异”分开；
- E 依赖 A/B/D，DTO 布局必须同时接受版本和 Profile 输入；
- F 依赖 B/C 的版本与地址状态语义；
- G 必须基于最终代码与实测结果，不得提前填写“通过”。

## 10. Definition of Done

本修复方案全部完成需同时满足：

- [ ] 已实现 V1.4 GET_VERSION，并可分别报告 Protocol/Transport 的 ReportedVersion、EffectiveVersion 和 VersionSource；
- [ ] V1.3 明确版本与 `ERR_CMD_UNKNOWN` fallback 均通过测试，timeout/畸形响应不会误降级；
- [ ] 高于 V1.4 的同 Major Slave 按 V1.4 封顶运行并给出可观察告警；
- [ ] Standard 为默认 Profile，版本选择与 Profile 选择互相独立，不存在静默方言 fallback；
- [ ] F-01～F-12 每项都有 V1.3/V1.4 适用性判断、代码变更、规范依据、测试和实施记录；
- [ ] XCPlite V1.4 Profile 的既有读写、A2L、DAQ、Measurement 集成通过；
- [ ] Windows/Linux/macOS Release 全量构建与测试通过；
- [ ] V1.3 对端完成约定的连接、版本 fallback、内存、GET_ID、DAQ/DTO 回归；
- [ ] 独立 Standard V1.4 对端完成 CONNECT+GET_VERSION、内存、GET_ID、DAQ/DTO 测试；若缺少该证据，声明必须降级为 XCPlite V1.4 Compatibility；
- [ ] 所有新增公开接口和成员具备中文 Doxygen 注释；
- [ ] `thirdparty/` 无修改；
- [ ] README 与模块文档的版本/能力声明与实测证据一致；
- [ ] 每个实施批次在 `code-plan/` 有修改内容和测试结果记录；
- [ ] 审计报告中的高/中风险项经复审关闭，不能只以“单测通过”关闭。

## 11. 实施前仍需确认的外部条件

以下不是设计占位符，而是进入代码修改前的明确门禁：

1. ASAM XCP V1.3 与 V1.4 官方英文规范的合法访问来源；
2. 独立 V1.3、Standard V1.4 对端的选择、版本、许可证和自动化方式；
3. XCPlite 中 `XCP_PROTOCOL_LAYER_VERSION=0x0104` 对应的具体 commit/tag，避免参考分支后续漂移；
4. 项目是否有 ABI 稳定承诺；
5. XCPlite Profile 需要长期维护还是仅作为迁移兼容；
6. CI 当前可用的 Windows/Linux/macOS 环境与网络条件。

若官方 V1.4 规范或独立 Standard V1.4 对端暂时无法满足，可继续完成 XCPlite V1.4 兼容实现，但对应结论必须标为“Compatibility/未完成通用符合性验证”，不得用 XCPlite 或 Mock 结果替代最终标准符合性结论。
