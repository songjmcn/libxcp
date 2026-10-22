# XCP 1.3.0 规范符合性审计报告

> 审计日期：2026-10-21  
> 审计基线：`docs/XCP_1.3.0_document.md`  
> 审计对象：当前仓库全部 XCP Master、UDP、内存访问、Seed & Key、Calibration/Page、DAQ/DTO 与测量实现  
> 审计方式：静态逐项对照 + 现有 Release 测试产物验证；未修改生产代码或测试代码

## 1. 结论摘要

**总体结论：部分符合。**

当前仓库已实现一个质量较高的 XCP 1.3.0 Master **功能子集**：基础会话、标准通信模式、UDP、内存上传/下载、Seed & Key、部分 Calibration/Page、静态/动态 DAQ 配置，以及测量数据处理主流程。命令序列化、响应分类、超时恢复主路径和动态 DAQ 分配事务有较完整的单元测试。

但不能据此认定为“完整 XCP 1.3.0 Master”。审计发现以下会直接影响标准对端互操作的确定问题：

1. AG=WORD/DWORD 时，分块内存访问的地址推进单位错误；
2. AG=DWORD 时，DOWNLOAD 未实现规范要求的额外对齐字节；
3. GET_ID 响应解析明确采用 XCPlite 方言，而不是本审计基线描述的标准布局；
4. GET_STATUS 的 `STORE_DAQ_REQ` 位掩码错误；
5. 动态 DAQ 强制从 List 0 编号，忽略 `MIN_DAQ`；
6. 标准 DTO Relative Identification Field 解码错误/不完整，并固化了 XCPlite 私有格式；
7. DTO 多字节时间戳固定按小端解码，忽略 CONNECT 协商的 Byte Order；
8. `DAQ_KEY_BYTE` 已解析但未贯穿到 DTO/MeasurementSession 自动配置。

此外，STIM 数据面、Checksum、Programming、Time Correlation、PID_OFF、DTO Counter 等能力未实现或只存在枚举/模式位。它们多数属于 Optional 能力，**未实现本身不等同于协议违规**，但必须在产品声明中明确边界。

### 1.1 风险统计

| 分类 | 数量 | 说明 |
|---|---:|---|
| 高风险确定偏差 | 7 | 可导致标准 Slave 读写地址错误、报文布局错误或 DTO 无法解码 |
| 中风险确定偏差/恢复缺口 | 4 | 状态位误判、隐式 MTA 恢复错误、分段 Seed/Key 超时失步、错误恢复矩阵不足 |
| 能力边界 | 10+ | Optional 命令、STIM/PGM/时间相关、PID_OFF 等未实现 |
| 已确认符合的主干 | 8 类 | 基础 CTO、会话主流程、Seed/Key 正常流程、DAQ 编码与动态事务等 |

## 2. 审计口径

- `docs/XCP_1.3.0_document.md:1-7` 自身说明它是中文技术译文，不替代 ASAM 官方英文标准。因此本报告是相对该仓库指定文档的符合性审计；涉及文档摘要未给出完整字节表的命令，最终裁决仍应以官方 XCP 1.3.0 英文规范为准。
- 只把违反已实现功能的明确规范要求列为“不符合”。Optional 能力未实现，列为“能力边界”，不直接作为缺陷。
- 自有 Mock Master 与自有 Mock Slave 的闭环只能证明实现内部一致，不能单独证明与第三方 ECU/CANape 的互操作性。
- 本次没有修改任何源码、测试或第三方内容。

## 3. 确定的规范偏差

### F-01【高】AG>1 时分块地址推进单位错误

**规范依据**

- `docs/XCP_1.3.0_document.md:1761-1772`：一个 Slave 地址单位对应 AG 个字节；AG 为 BYTE/WORD/DWORD。
- `docs/XCP_1.3.0_document.md:1904-1916`、`:1985-1990`：UPLOAD/DOWNLOAD 后 MTA 增加传输的 Element 数。

**实现证据**

- `src/protocol_types.cpp:200-209`：`XcpAddress40::Advance` 使用 `address + elements * AgToBytes(ag)`。
- `src/memory_access.cpp:120-126`、`:288-297`：分块读写直接使用该推进结果作为下一块地址。
- `src/memory_access.cpp:42-54`、`:224-237`：地址空间边界也按 `elements * AG` 计算。

**影响**

AG=WORD/DWORD 时，数值地址会多推进 2/4 倍，造成跳读、跳写，或在仍合法时被错误判为地址溢出。现有 `tests/memory_access_test.cpp:496-523,588-593` 固化的是“字节地址”模型，因此测试通过不能消除该偏差。

### F-02【高】AG=DWORD 的 DOWNLOAD 缺少对齐字节

**规范依据**

`docs/XCP_1.3.0_document.md:1985-1990` 要求 AG=DWORD 时 DOWNLOAD 数据前增加额外对齐字节。

**实现证据**

- `src/command_codec.cpp:367-386` 固定编码为 `[F0][size][data...]`。
- `CommandCodec` 只持有 Byte Order，不持有 AG，无法针对 DWORD AG 插入填充。
- `src/memory_access.cpp:176-185` 固定按 `(MAX_CTO-2)/AG` 计算容量，也未扣除额外填充。

**影响**

DWORD AG Slave 会把数据解释到错误偏移；最大单包元素数也可能多算。现有测试没有覆盖 DWORD AG 的标准 DOWNLOAD 对齐报文。

### F-03【高】GET_ID 固化 XCPlite 方言

**规范依据**

`docs/XCP_1.3.0_document.md:1829-1846` 定义 GET_ID 的 Identification Type、Byte Length、内联/通过 MTA 上传两种模式及 Type 4 压缩/加密语义。

**实现证据**

- `src/response_parser.cpp:395-418` 明确按 XCPlite 的 `[MODE][reserved×2][LENGTH DWORD][DATA]` 解析，并写明“实然优先”。
- `include/libxcp/response_parser.hpp:181-190` 明确承认与规范的 WORD Length/偏移不同。
- `src/xcp_master.cpp:704-740` 的 A2L 上传流程完全依赖该解析结果。
- 当前流程拒绝内联模式，相关测试见 `tests/xcp_daq_test.cpp:1039-1094`。

**影响**

对非 XCPlite、严格遵循标准布局的 Slave 可能解析失败。当前 `tests/xcplite_daq_test.cpp:471-484` 只能证明与 XCPlite 互操作，不能证明通用 XCP 1.3 合规。

> 注：仓库中文规范的 GET_ID 小节没有完整逐字节表。该项应在修复前再以 ASAM 官方英文表格复核，但现有实现和头文件已经主动声明偏离规范，因此仍列为高风险。

### F-04【中】GET_STATUS 的 STORE_DAQ_REQ 位解析错误

**规范依据**

`docs/XCP_1.3.0_document.md:1793-1801`：`STORE_DAQ_REQ` 为 bit 2。

**实现证据**

`src/response_parser.cpp:52-57` 使用 `kStoreDaqReqMask = 0x02U`，即 bit 1。

**影响**

Master 会误报/漏报 DAQ 持久化请求状态。`tests/response_parser_test.cpp:211-226` 的测试数据与断言固化了当前错误，不能作为规范符合性证明。

### F-05【高】动态 DAQ 忽略 MIN_DAQ

**规范依据**

`docs/XCP_1.3.0_document.md:2291-2344`：GET_DAQ_PROCESSOR_INFO 返回 `MIN_DAQ`，用于界定可配置 DAQ List 区间。

**实现证据**

- `src/xcp_master.cpp:547-555` 强制 `specs[i].daq_list == i`，从 0 开始编号。
- Processor Info 虽被解析，但动态编排仅缓存 Properties，未消费 `MIN_DAQ`：`src/xcp_master.cpp:559-575`。

**影响**

对于 `MIN_DAQ != 0` 的标准 Slave，合法动态区间会被本地拒绝或错误寻址。

### F-06【高】DTO Relative Identification Field 不符合标准布局

**规范依据**

- `docs/XCP_1.3.0_document.md:1421-1451`：DTO 第一字节为 PID；Relative 模式中 PID 表示 relative ODT，随后是 BYTE/WORD DAQ List Number，WORD aligned 模式可有 FILL。
- `docs/XCP_1.3.0_document.md:2327-2344`：具体格式由 `DAQ_KEY_BYTE` 告知 Master。

**实现证据**

- `src/daq/dto_envelope_decoder.cpp:92-103` 的 RelativeByte 硬编码为 XCPlite 私有 `[relODT][0xAA][DAQ16 LE]`。
- `src/daq/dto_envelope_decoder.cpp:105-114` 的 RelativeWord/Aligned 把 `dto[0]` 当 DAQ、`dto[1]` 当 ODT，并把 DAQ 截为 8 bit。
- `include/libxcp/daq/dto_envelope_decoder.hpp:35-45` 和 `docs/modules/daq解码.md:34-39` 对标准布局的描述与实现相互矛盾。

**影响**

标准 RelativeByte/RelativeWord 对端的合法 DTO 会被误解码或判为畸形。`tests/dto_envelope_decoder_test.cpp:76-256` 主要验证当前私有语义，不是标准格式矩阵测试。

### F-07【高】DTO 多字节字段固定按小端解码

**规范依据**

- `docs/XCP_1.3.0_document.md:1757-1760`：CONNECT 协商的 Byte Order 驱动多字节参数解释。
- `docs/XCP_1.3.0_document.md:1489-1495,2346-2370`：Timestamp 可为 BYTE/WORD/DWORD。

**实现证据**

- `src/daq/dto_envelope_decoder.cpp:52-59` 只有 `ReadLe`。
- `src/daq/dto_envelope_decoder.cpp:63-64` 虽保存 `ByteOrder`，但 `:92-114,143-152` 未使用 `m_byte_order_`。

**影响**

Motorola/大端 Slave 的 timestamp 及私有 DAQ16 会被错误解释。现有测试缺少 Motorola DTO timestamp 用例。

### F-08【高】DAQ_KEY_BYTE 未贯穿到 DTO 配置

**规范依据**

`docs/XCP_1.3.0_document.md:1451,2327-2344` 要求 Master 按 Slave 返回的 `DAQ_KEY_BYTE` 解释/生成 DTO/STIM Identification Field。

**实现证据**

- `include/libxcp/protocol_types.hpp:855-863` 已能解析 `DAQ_KEY_BYTE`。
- `src/measurement/measurement_session.cpp:91-117` 仍要求调用方手工设置 envelope 类型和头宽。
- `src/xcp_master.cpp:559-575` 的动态路径未将 Processor Info 的 Key Byte 传给 MeasurementSession。

**影响**

即使 Slave 正确报告标准格式，库也不会自动选择，调用方配置稍有偏差即无法互操作。

## 4. 中风险缺口与健壮性问题

### F-09【中】SHORT_UPLOAD 后本地隐式 MTA 未同步

规范 `docs/XCP_1.3.0_document.md:1918-1936` 要求 SHORT_UPLOAD 完成后 MTA 更新到块末。`src/command_executor.cpp:888-898` 成功后未更新 `m_last_mta_`；`include/libxcp/command_codec.hpp:112-121` 还声称 SHORT_UPLOAD 不改变 MTA。若后续 UPLOAD 超时，`src/command_executor.cpp:540-560,731-735` 可能恢复旧 MTA。高层分块每次显式 SET_MTA 掩盖了该问题，但组合 API 仍存在风险。

### F-10【中】分段 Seed & Key 超时恢复可能失步

Seed/Key 正常分段编排位于 `src/xcp_master.cpp:811-874`，通用超时恢复位于 `src/command_executor.cpp:646-742`。Remainder GET_SEED 或中间 UNLOCK 响应丢失后，简单 SYNCH 并重发当前分段，可能与 Slave 已推进的内部偏移不一致。`docs/XCP_1.3.0_document.md:2754-2762,2789` 要求先通过 Pre-Action 重建确定状态。现有 `tests/seed_key_test.cpp` 没有任一 Seed/Unlock 分段 timeout 测试。

### F-11【中】错误恢复矩阵只实现部分策略

实现较好地覆盖 Timeout→SYNCH→重试、UPLOAD 重建 MTA、WRITE_DAQ 重建 DAQ pointer；但 `src/command_executor.cpp:407-470,563-742` 对多数 Error Code 直接抛出。规范建议的 BUSY/PGM_ACTIVE 等待 t7、SEQUENCE 重建上下文、Unknown alternative、动态 DAQ 重新初始化等策略见 `docs/XCP_1.3.0_document.md:2748-2824`。`tests/recovery_test.cpp:540-550` 还明确锁定 BUSY 不重试。评级为“部分符合”，不是所有策略都必须隐式自动执行，但当前行为应在 API 文档中明确。

### F-12【中】能力与模式约束未完整贯穿

GET_DAQ_RESOLUTION_INFO 已解析 granularity、max entry size、timestamp unit/ticks/fixed，但配置层主要只使用 timestamp width：`src/response_parser.cpp:353-373`、`src/xcp_master.cpp:474-490,577-592`。尚未完整前置校验 Entry 地址/长度对齐、最大 Entry、TIMESTAMP_FIXED、ALTERNATING+TIMESTAMP 等组合。Slave 返回标准错误时不会被吞，因此更适合定性为健壮性不足，而非必然的 wire-level 违规。

## 5. 已确认符合的主要实现

| 功能 | 判定 | 主要证据 |
|---|---|---|
| CMD/RES/ERR/EV/SERV/DTO 分类 | 符合 | `src/response_parser.cpp:113-173`；规范 `docs/...:1510-1637` |
| 标准单 Pending Command 模式 | 符合 | `src/session.cpp`、`src/command_executor.cpp:181-405`；规范 `docs/...:1076-1093` |
| EV_CMD_PENDING 重启计时且不重发 | 符合 | `src/command_executor.cpp:278-287,381-389`；规范 `docs/...:2795,2846-2848` |
| CTO 多字节 Byte Order | 符合 | `src/command_codec.cpp:24-46`、`src/response_parser.cpp:61-101` |
| Seed & Key 正常流程及错误 Key 失效 | 符合（正常路径） | `src/xcp_master.cpp:811-874`、`src/command_executor.cpp:916-956` |
| DAQ 请求命令码与主布局 | 符合 | `src/command_codec.cpp:187-365`；测试 `tests/command_codec_test.cpp:210-343` |
| Dynamic DAQ 分配顺序与失败回滚 | 符合（除 MIN_DAQ） | `src/xcp_master.cpp:597-667`；规范 `docs/...:2473-2519` |
| 每 Entry 重设 DAQ Pointer | 符合 | `src/xcp_master.cpp:626-649`、`src/command_executor.cpp:963-1007` |
| Select + START_STOP_SYNCH | 符合 | `src/xcp_master.cpp:674-734` |
| Processor/Resolution/List/Event 主响应解析 | 符合 | `src/response_parser.cpp:279-392` |
| DOWNLOAD 原子错误传播、SHORT_DOWNLOAD fallback | 基本符合 | `src/memory_access.cpp:176-325`；但受 F-01/F-02 限制 |
| UDP Header、CTR、重复/乱序/畸形 Datagram 处理 | 符合当前传输设计 | `src/udp_header_codec.cpp`、`src/udp_transport.cpp` 及相应测试 |
| Page Switching 八命令 | 字段语义基本符合 | `src/command_codec.cpp:435-560`、`src/response_parser.cpp:421-523`；中文基线缺完整字节表，需官方表最终确认 |

## 6. 未实现或受限能力（不直接判为协议缺陷）

| 能力 | 当前状态 | 说明 |
|---|---|---|
| SET_REQUEST | 仅命令枚举 | 无 codec/executor/master API |
| BUILD_CHECKSUM | 仅命令枚举 | 标定校验场景不可用 |
| TRANSPORT_LAYER_CMD / USER_CMD | 仅命令枚举 | Optional |
| DOWNLOAD_NEXT / DOWNLOAD_MAX | 仅命令枚举 | 未实现 Block Download |
| Block / Interleaved Mode | 未启用 | Standard 模式可正常工作 |
| WRITE_DAQ_MULTIPLE | 未实现 | Optional |
| GET_DAQ_LIST_MODE / GET_DAQ_CLOCK | 未实现 | Optional |
| DTO_CTR_PROPERTIES | 未实现 | Optional |
| PID_OFF | 主入口主动拒绝 | `src/xcp_master.cpp:72-75`、`src/daq/dto_envelope_decoder.cpp:68-71` |
| DTO Counter 测量 | MeasurementSession 固定关闭 | Optional 能力未贯通 |
| Absolute ID 多 DAQ List | 限制为 List 0 单列表 | `src/measurement/measurement_session.cpp:228-233,289-300` |
| STIM 数据面 | 只有 Mode 位，无发送 DTO API | 不能宣称支持 STIM 功能 |
| Programming | 未实现 | PROGRAM_START/CLEAR/PROGRAM/RESET 全族缺失 |
| Time Correlation | 未实现 | 无 GET_DAQ_CLOCK/高级 time correlation/multicast 流程 |
| SERV_TEXT 重组 / SERV_RESET 行为 | 仅原始事件透传 | `src/command_executor.cpp:293-320` |
| Static DAQ 的通用 MeasurementSession | 未实现 | MeasurementSession 固定走动态配置 |

这些缺口与 `include/libxcp/protocol_types.hpp:50-61`“只登记 Encode/Parse/Execute 齐备命令”的注释矛盾；实际枚举中包含多条未实现命令（`:69-82`）。

## 7. 声明与文档一致性

1. `README.md:1-12` 的“完整 Master 侧能力”“XCP 1.3.0 协议核心 ✅”容易被理解为完整协议支持，与本报告的功能子集事实不符。
2. `README.md:15` 的 Dynamic DAQ“v1.0 封板”未体现 MIN_DAQ、标准 DTO Key Byte、Absolute 多列表、Motorola timestamp 等边界。
3. `README.md:17` 把 Calibration 与 Programming 一起写为“不在 v1.0 承诺范围”已经陈旧：Calibration 写入、ModifyBits、Page Switching 已有公开 API；Programming 才是未实现。
4. `docs/modules/协议核心.md:9-35` 与 `docs/modules/测试.md:31-46` 对命令数量、读写能力及测试范围的描述落后于当前代码。

建议把产品定位改为“XCP 1.3.0 Master 已实现子集”，并维护一张明确的 Supported / Partial / Unsupported 能力矩阵。

## 8. 测试与证据评估

### 8.1 本次运行结果

运行现有 `cmake-build-release/tests/libxcp_tests`：

- 468 个测试；
- 467 通过；
- 1 跳过：`AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`；
- 进程退出码 0。

说明：本次按“只分析、不修改”原则未执行 clean/rebuild；上述结果来自工作区已有 Release 测试产物，应视为辅助证据，不等同于对当前源码进行全量重编译后的结果。

### 8.2 证据局限

- 默认测试主要是 Unit/Mock；`tests/CMakeLists.txt:95-150` 的 XCPlite L3 需显式开启 `LIBXCP_BUILD_XCPLITE_SLAVE`。
- 实际对端只有 XCPlite；没有覆盖多厂商 ECU、CANape 或标准一致性测试套件。
- GET_ID、RelativeByte DTO、AG 地址推进等测试复用了当前实现假设，属于“自洽测试”，不能作为独立规范 oracle。
- Page Switching 缺真实对端覆盖；没有 STIM、Checksum、Programming、Time Correlation 的协议测试。
- 缺少 `MIN_DAQ != 0`、标准四种 DAQ_KEY_BYTE、Motorola DTO timestamp、DWORD AG DOWNLOAD 对齐、Seed/Unlock 分段 timeout 等关键矩阵。

## 9. 建议优先级（仅分析建议，未实施）

### P0：互操作正确性

1. 统一澄清并修正 AG 地址单位，增加 AG=WORD/DWORD 跨块真实地址测试。
2. 按 AG 实现 DOWNLOAD 对齐和容量计算。
3. 以官方 XCP 1.3.0 字节表复核 GET_ID，并把标准布局与 XCPlite 兼容模式显式分离。
4. 修正 GET_STATUS `STORE_DAQ_REQ` 位掩码。
5. 动态 DAQ 使用 `MIN_DAQ`，不再假定从 0 开始。
6. 按 `DAQ_KEY_BYTE` 实现四种标准 DTO Identification Field；XCPlite 私有布局只能作为显式兼容 profile。
7. DTO 多字节字段使用会话 Byte Order。

### P1：能力贯通与恢复

1. 将 Processor/Resolution 的 Key Byte、granularity、max size、timestamp unit/ticks/fixed 自动贯穿到 MeasurementSession。
2. 增加 Standard RelativeByte/Word/Aligned、Absolute 多列表和 Motorola DTO 测试矩阵。
3. 为 Seed/Key 分段设计事务级超时恢复，而不是单帧盲重发。
4. 同步 SHORT_UPLOAD 后的隐式 MTA。
5. 明确 Error Code 恢复策略：自动恢复、向上抛出、或要求调用方动作。

### P2：能力扩展与声明收口

1. 根据产品目标选择实现 STIM、Checksum、Programming、Time Correlation 等能力，而不是仅保留枚举。
2. 更新 README、模块文档和 CommandCode 注释，避免“完整”“封板”等过度声明。
3. 引入至少一个非 XCPlite 的独立标准对端或一致性测试工具。

## 10. 最终判定

| 维度 | 判定 |
|---|---|
| 基础会话与 CTO 主流程 | 基本符合 |
| UDP Transport | 基本符合，现有测试较充分 |
| 内存读取/写入 | 部分符合；AG 地址与 DWORD DOWNLOAD 为高风险问题 |
| Seed & Key | 正常流程符合；分段超时恢复不完整 |
| Calibration / Page | 已实现较大子集；真实对端证据不足 |
| DAQ 配置命令与动态事务 | 基本符合；MIN_DAQ 与能力贯通存在问题 |
| DTO 标准互操作 | 不符合；布局、Key Byte 和 Byte Order 有确定偏差 |
| MeasurementSession | 对 XCPlite 特定配置可用；不是通用 XCP 1.3 DAQ 解码器 |
| Programming / Time Correlation / STIM | 未实现或仅有残片 |
| “完整 XCP 1.3.0 Master”声明 | 证据不足，不建议继续使用 |

**最终结论：当前实现适合作为面向 XCPlite/特定 ECU Profile 的 XCP 1.3.0 Master 子集，但尚不满足通用、完整 XCP 1.3.0 Master 的符合性声明。**
