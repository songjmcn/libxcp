# XCP 1.3.0 最小协议核心及 UDP Transport 实现计划

## 1. 目的与依据

本计划面向 **XCP Master 协议核心库**，目标是完成类似 CANape 的最小轮询读取能力；本文只规划，不实施代码。

依据：

- `docs/XCP_1.3.0_中文总结_面向CANape实现.md`
- `docs/XCP_1.3.0_document.md`
- `docs/XCP_-Part_3-_Transport_Layer_Specification_XCP_on_Ethernet_(TCP_IP_and_UDP_IP)_-1.0.pdf`

实现语言为 C++20，目标平台为 Windows、Linux、macOS。当前仓库仅有文档，没有源码、构建系统或测试框架，因此本文目录布局是实施建议，不代表已有接口。本计划增加标准 XCP on UDP/IP 1.0 Transport 和本地测试 Slave，用于验证真实 Socket、传输层封装及协议核心的端到端行为；Mock Transport 仍用于确定性的单元和协议流程测试。

## 2. 已确认范围

### 2.1 包含

1. XCP CTO 报文模型、协议常量和安全的字节读写。
2. 以下命令的编码、响应解析和调用流程：
   - `CONNECT (0xFF)`
   - `DISCONNECT (0xFE)`
   - `GET_STATUS (0xFD)`
   - `SYNCH (0xFC)`
   - `GET_COMM_MODE_INFO (0xFB)`（可选能力）
   - `SET_MTA (0xF6)`
   - `UPLOAD (0xF5)`
   - `SHORT_UPLOAD (0xF4)`
3. `RES/ERR/EV/SERV` 分类，DTO 仅识别并上报。
4. Session 参数：RESOURCE、COMM_MODE_BASIC、MAX_CTO、MAX_DTO、协议/传输层主版本、Byte Order、Address Granularity（AG）。
5. Standard Communication Model：每个连接最多一条等待最终响应的命令。
6. 优先 SHORT_UPLOAD，必要时使用 SET_MTA+UPLOAD 分块读取。
7. SYNCH 超时恢复、隐含状态恢复、有限重试和 EV_CMD_PENDING。
8. 抽象传输接口 `IXcpTransport`。
9. 标准 XCP on UDP/IP 1.0 Transport：IPv4 UDP Socket、4 字节 Transport Header、LEN/CTR 校验和端点配置。
10. 本地 `UdpTestSlave`：支持最小命令集和可控故障注入，仅作为测试设施。
11. 单元测试、Mock Transport 测试和 UDP Loopback 端到端测试。

### 2.2 不包含

- A2L Parser、变量名、ASAM 数据类型、COMPU_METHOD。
- XCP on CAN/CAN FD/TCP，以及 Vector、PEAK、SocketCAN 驱动。
- XCP on Ethernet 后续版本的 Discovery、`GET_SLAVE_ID`、`GET_DAQ_CLOCK_MULTICAST` 和 IPv6。
- DAQ、STIM、ODT、Timestamp、DTO 内容解码。
- Seed&Key、Calibration、Page、Programming、Checksum。
- Block/Interleaved Mode 的实际启用（只解析能力）。
- Qt、GUI、MDF 和真实 ECU 联调。

### 2.3 边界

Protocol Layer 与 UDP Transport Layer 保持分离：`IXcpTransport` 对协议核心仍只收发完整 XCP Packet；`UdpTransport` 负责标准 UDP/IP 1.0 Header、Socket 和远端端点。UDP Transport 依据新增的 XCP on Ethernet 1.0 Associated Standard 实现，不把 IP/UDP 细节泄漏到协议核心。

`UdpTestSlave` 是测试程序而不是生产 Slave SDK，也不是 CANape 替代品。Mock Transport 仍然必要：它用于无需端口、线程和操作系统调度的确定性单元测试；UDP Loopback 测试用于补充真实网络栈与标准 Transport 封装验证。

## 3. 架构

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
          |
     UdpTransport
          |
 UdpHeaderCodec + IPv4 UDP Socket

Tests:
  MockTransport          -> 确定性协议测试
  UdpTestSlave(loopback) -> 真实 Socket/封装端到端测试
```

### 3.1 组件职责

- **ProtocolTypes**：命令码、PID、错误码、事件码、资源掩码、Byte Order、AG、Session State；用 `ElementCount`、`ByteCount`、`XcpAddress` 等强类型避免单位混用；未知枚举值须保留。
- **CommandCodec**：生成 CTO；reserved 字节写 0；遵守 WORD/DWORD 对齐和 Session Byte Order；发送前检查 MAX_CTO。
- **ResponseParser**：分类 RES/ERR/EV/SERV/DTO；检查长度、对齐和边界；保留 Error 附加信息；禁止越界读取。
- **Session**：至少维护 Disconnected、Connecting、Connected、Disconnecting、Recovering；保存协商参数并限制单 Outstanding Command；断连后清空 MTA 和能力。
- **CommandExecutor**：发送请求、等待最终 RES/ERR、分流 EV/SERV、处理 Timeout/SYNCH/EV_CMD_PENDING；不启用 Block/Interleaved Mode。
- **MemoryAccess**：实现 SHORT_UPLOAD、SET_MTA+UPLOAD、AG 换算、分块、地址溢出检查和 MTA 恢复。
- **IXcpTransport**：打开/关闭通道，收发完整 XCP Packet，支持超时与取消，区分关闭、I/O 错误、Timeout；不暴露具体总线类型。
- **UdpHeaderCodec**：只负责编解码 XCP on Ethernet 1.0 固定 Header，并验证 LEN 与 Datagram 实际长度。
- **UdpTransport**：实现跨平台 IPv4 UDP Socket、绑定本地端点、配置远端 Slave IP/端口、收发 Datagram、维护发送/接收 CTR 诊断，并向 IXcpTransport 隐藏 4 字节 Header。
- **UdpTestSlave**：运行于 Loopback 的测试对端，解析 Transport Header，维护独立方向 CTR，模拟最小 Session 和内存，并提供丢包、延迟、重复、乱序、截断和错误 LEN 等故障注入。

## 4. 协议约束

### 4.1 Packet 分类

```text
Master -> Slave: 0xC0..0xFF CMD
Slave -> Master: 0x00..0xFB DTO, 0xFC SERV, 0xFD EV, 0xFE ERR, 0xFF RES
```

### 4.2 CONNECT Session 快照

成功 CONNECT 后保存并校验：

- CAL/PAG、DAQ、STIM、PGM Resource Mask；
- Intel/Motorola Byte Order；
- AG=1/2/4 Byte/Address；
- Slave Block Mode 与 Optional 信息能力位（只记录）；
- MAX_CTO 范围 `0x08..0xFF`；
- MAX_DTO 范围 `0x0008..0xFFFF`；
- Protocol/Transport 主版本；
- `MAX_CTO mod AG == 0`、`MAX_DTO mod AG == 0`。

关键字段非法时不进入 Connected，返回含字段上下文的协议错误并关闭通道。

### 4.3 GET_STATUS

解析 Current Session Status、Current Resource Protection Status、STATE_NUMBER、Session Configuration ID。至少解析 RESUME、DAQ_RUNNING、CLEAR_DAQ_REQ、STORE_DAQ_REQ、STORE_CAL_REQ。本阶段只报告 DAQ/RESUME/保护状态，不启动 DAQ 或自动解锁。

### 4.4 GET_COMM_MODE_INFO

仅在 CONNECT 表示 Optional 信息可用时调用；解析 COMM_MODE_OPTIONAL、MAX_BS、MIN_ST、QUEUE_SIZE、Driver Version。若返回 ERR_CMD_UNKNOWN，记录为不可用并继续连接；即使成功也不启用块模式或交错模式。

### 4.5 长度与地址

现有文档给出：

```text
UPLOAD:       1 <= NumberOfElements <= MAX_CTO / AG - 1
SHORT_UPLOAD: 1 <= NumberOfElements <= MAX_CTO / AG
```

- 返回 Byte 数应为 `NumberOfElements * AG`。
- 地址由 32-bit Address 和 8-bit Address Extension 组成。
- AG 是地址单位，不是变量类型；MTA 和读取数量按 Element 处理。
- Byte API 若存在，只接受可被 AG 整除的长度。
- 地址推进、乘法和分块偏移必须检查整数及 32-bit 地址溢出。
- 编码前必须从规范命令表逐 Byte 固化请求/响应布局、reserved 字段和对齐规则，并建立黄金报文测试。

### 4.6 XCP on UDP/IP 1.0 Transport Header

依据 XCP on Ethernet 1.0 规范，每个 UDP Payload 为：

```text
Offset  Size  Field
0       2     LEN，固定 Intel/little-endian
2       2     CTR，固定 Intel/little-endian
4       LEN   原始 XCP Packet
```

约束：

1. Header 固定 4 Byte，没有 Transport Tail。
2. LEN 只计算原始 XCP Packet，不包含 4 Byte Header、UDP/IP Header。
3. Transport Header 的 LEN/CTR 永远使用小端序，不受 CONNECT 返回的 Slave Byte Order 影响。
4. 接收时要求 `udpPayloadSize == 4 + LEN`；不匹配则丢弃并报告 Malformed Datagram，不交给协议解析器。
5. 每个 UDP Datagram 只承载一个完整 XCP Packet；Transport 不拼接多个 Datagram，也不把一个 Packet 拆成多个 Datagram。

### 4.7 CTR 与 Datagram 策略

- Master→Slave 与 Slave→Master 各自维护独立的 16-bit CTR；发送新 Datagram 后按模 65536 递增。
- CTR 用于检测缺包和序号异常，不是事务 ID；响应 CTR 不回显请求 CTR，禁止使用二者相等来匹配响应。
- 响应匹配仍依赖“单 Pending Command + XCP RES/ERR PID”。
- XCP on Ethernet 1.0 没有规定 UDP 重传、重排和响应缓存。本项目不在 Transport 层重排或重传 Datagram；命令 Timeout 和恢复仍由 CommandExecutor 负责。
- 本项目接收策略：首个合法 Datagram 建立接收基线；CTR 等于期望值时接收并推进；模 65536 判断为前向跳号时接收当前包、报告缺口并推进；重复或后向乱序包丢弃并诊断；恰好相差 `0x8000` 的歧义包丢弃并诊断。所有判断均先通过来源、Header、LEN 和配置上限校验。
- CTR 初值和复位点不是标准强制条款。本项目默认每次 `UdpTransport.open()` 将发送 CTR 置 0、清空接收基线；XCP Session reconnect 不单独重置 Transport CTR；该策略作为可测试行为写入接口文档。

### 4.8 UDP 端点、生命周期与长度上限

- Slave IPv4 地址、UDP 业务端口、本地绑定地址/端口均由配置提供；不硬编码示例端口。
- Master 打开 Socket 后绑定本地端点，并只接受配置的远端 IP/端口发来的业务 Datagram；来源不匹配的 Datagram 丢弃并诊断。
- XCP CONNECT/DISCONNECT 只管理逻辑 Session；UDP Socket 生命周期由 `UdpTransport.open()/close()` 管理。
- LEN 字段可表示 65535 Byte XCP Packet，但 IPv4 UDP Payload 的通常理论上限为 65507 Byte，扣除 4 Byte XCP Transport Header 后，单个 XCP Packet 通常不超过 65503 Byte。
- 提供可配置的最大 Datagram/XCP Packet 限制；默认值必须显式记录。若选择避免 IP 分片，还应允许调用方按路径 MTU 设置更小上限，不把 1468 Byte 当作 XCP 标准固定值。
- 最小版本不实现 Discovery、Multicast、IPv6 和 UDP 广播。

## 5. 数据流

### 5.1 连接

```text
Transport.open
 -> CONNECT(mode=0)
 -> 校验 Session 参数
 -> [能力允许] GET_COMM_MODE_INFO
      -> 成功：保存
      -> ERR_CMD_UNKNOWN：降级继续
 -> GET_STATUS
 -> Connected
```

CONNECT、参数校验或 GET_STATUS 失败时清除部分状态并关闭 Transport。

### 5.2 SHORT_UPLOAD

```text
read(address, extension, elements)
 -> 校验状态、AG、MAX_CTO、范围
 -> SHORT_UPLOAD
 -> 校验 RES 数据长度 == elements * AG
 -> 返回原始 Byte Sequence
```

若 ERR_CMD_UNKNOWN，将 SHORT_UPLOAD 标记为当前 Session 不可用，本次和后续读取降级到 SET_MTA+UPLOAD。

### 5.3 SET_MTA + UPLOAD

```text
SET_MTA(extension, startAddress)
 -> UPLOAD(chunk)
 -> UPLOAD(chunk)
 -> ...
 -> 拼接结果
```

- 每块不超过普通 UPLOAD 上限，前一块成功后才请求下一块。
- 每块严格检查响应长度。
- 失败时返回错误和已完成 Element 数，不把部分数据作为完整成功。
- Timeout 恢复时根据原始地址和已完成 Element 数重发 SET_MTA，不能假设 Slave MTA 未变化。

### 5.4 断开

`Connected -> DISCONNECT -> Transport.close -> Disconnected`。即使 DISCONNECT 返回 ERR_CMD_BUSY 或 Transport 错误，也释放本地资源和 Session 状态，同时向调用方返回断开错误；重复断开在本地幂等。

## 6. 错误与恢复

### 6.1 结构化错误

- `InvalidArgument`：参数、AG 换算、长度非法。
- `InvalidState`：未连接、正在恢复或已有待响应命令。
- `TransportError`：打开、发送、接收、关闭失败。
- `Timeout`：没有最终响应。
- `MalformedPacket`：PID、长度、对齐或字段非法。
- `ProtocolError`：Slave 返回 ERR；包含命令码、Error Code、附加信息。
- `UnsupportedFeature`：例如当前阶段不支持 Seed&Key。

错误不能只有文本，须保留机器可判定分类、协议码、重试次数和底层错误。

### 6.2 Timeout/SYNCH

```text
Timeout
 -> Recovering
 -> SYNCH
 -> 等待 ERR_CMD_SYNCH(0x00)
 -> 恢复隐含状态
 -> 重发原命令
```

1. ERR_CMD_SYNCH 仅在等待 SYNCH 时视为恢复成功。
2. 原命令最多两次恢复重试，不含首次尝试。
3. UPLOAD 重试前必须重发对应 SET_MTA。
4. 同步失败或重试耗尽后，返回恢复失败并要求重新连接或标记 Session 不可继续。
5. Transport 已断开时不执行 SYNCH。

### 6.3 EV_CMD_PENDING

收到 `EV_CMD_PENDING (0x05)` 时重启当前命令 Timer，不重发原命令、不发送下一条命令；Event 仍上报。调用方取消或 Transport 关闭可终止等待。

### 6.4 重点协议错误策略

| Error | 策略 |
|---|---|
| ERR_CMD_SYNCH | 仅作为 SYNCH 的成功确认 |
| ERR_CMD_BUSY | 返回错误，不无限等待 |
| ERR_CMD_UNKNOWN | GET_COMM_MODE_INFO/SHORT_UPLOAD 按替代路径降级 |
| ERR_CMD_SYNTAX | 视为实现或参数错误，不盲目重试 |
| ERR_OUT_OF_RANGE | 返回范围上下文，不以相同参数重试 |
| ERR_ACCESS_DENIED | 报告访问拒绝 |
| ERR_ACCESS_LOCKED | 报告需要后续 Seed&Key，不自动解锁 |
| ERR_SEQUENCE | UPLOAD 可恢复 MTA 后有限重试；其他场景直接报告 |
| ERR_RESOURCE_TEMPORARY_NOT_ACCESSIBLE | 标记可重试，由上层决定时机 |
| 未知错误码 | 保留原值并上报 ProtocolError |

## 7. 建议目录

```text
CMakeLists.txt
include/libxcp/
  protocol_types.hpp
  xcp_error.hpp
  ixcp_transport.hpp
  udp_transport.hpp
  udp_transport_config.hpp
  udp_header_codec.hpp
  command_codec.hpp
  response_parser.hpp
  session.hpp
  command_executor.hpp
  memory_access.hpp
  xcp_master.hpp
src/
  udp_transport.cpp
  udp_header_codec.cpp
  command_codec.cpp
  response_parser.cpp
  session.cpp
  command_executor.cpp
  memory_access.cpp
  xcp_master.cpp
tests/
  command_codec_test.cpp
  response_parser_test.cpp
  session_test.cpp
  memory_access_test.cpp
  recovery_test.cpp
  udp_header_codec_test.cpp
  udp_transport_test.cpp
  mock_transport.hpp
  udp_test_slave.hpp
  udp_test_slave.cpp
  xcp_master_integration_test.cpp
  xcp_udp_loopback_test.cpp
```

开始编码前再次确认仓库构建约定；公共接口使用中文 Doxygen 注释，格式遵循 `.clang-format`。

## 8. 实施阶段

### 阶段 0：工程基线

采用已有 CMake/测试框架/命名约定；不存在时建立最小 C++20 库和测试目标，不引入 Qt 或平台 API。完成标准：Release 空工程可构建、测试可运行。

### 阶段 1：协议类型与 Codec

逐字段整理 8 条命令布局；建立强类型、Byte Order 工具和有界 Reader/Writer；实现编码与 MAX_CTO 校验；建立 Intel/Motorola 黄金报文。完成标准：报文与规范表一致，非法参数不发送。

### 阶段 2：Parser、Session 与 Transport 抽象

定义 IXcpTransport 的 Packet 边界、超时、取消、诊断和错误语义；实现 Packet 分类、响应解析、CONNECT 参数校验、Session 状态和单命令约束；DTO 只识别上报。完成标准：畸形包不越界，非法参数不进入 Connected，协议核心不依赖 Socket API。

### 阶段 3：标准 UDP Transport 与测试对端

1. 依据 XCP on Ethernet 1.0 规范固化 LEN/CTR Header 表和黄金向量。
2. 实现 UdpHeaderCodec，确保 LEN/CTR 固定小端且与 Session Byte Order 解耦。
3. 实现跨平台 IPv4 UdpTransport、配置校验、Socket 打开/关闭、收发取消和远端过滤。
4. 实现两个方向独立 CTR、16-bit 回绕、缺口/重复/乱序诊断和 Malformed Datagram 丢弃。
5. 实现 Loopback `UdpTestSlave`：脚本化最小命令响应、可配置内存、独立发送 CTR 和故障注入。
6. 测试不得依赖固定端口：服务端绑定 Loopback 临时端口，并把实际端口传给 Master。

完成标准：可以在真实操作系统 UDP Socket 上交换标准 XCP on UDP/IP 1.0 Datagram；Header、端点过滤、CTR 和关闭/取消行为均有自动化测试。

### 阶段 4：Session 命令流程

实现 CONNECT、GET_STATUS、条件式 GET_COMM_MODE_INFO、DISCONNECT、降级和失败清理；先用 Mock 验证确定性状态迁移，再用 UdpTestSlave 验证“打开—连接—查询—断开”。

### 阶段 5：轮询读取

实现 SHORT_UPLOAD、SET_MTA+UPLOAD、AG/MAX_CTO 分块、降级缓存和溢出检查。完成标准：AG=1/2/4 下单包与多包结果均和 Mock 内存一致，并至少用 UDP Loopback 覆盖一组端到端读取。

### 阶段 6：异步与恢复

分流 EV/SERV；实现 EV_CMD_PENDING、SYNCH、最多两次恢复重试、UPLOAD MTA 重建和结构化错误。Mock 用于精确控制状态序列，UdpTestSlave 用于验证真实 Datagram 丢失、延迟和重复。完成标准：丢包、延迟、Pending、同步成败和耗尽均有确定结果。

### 阶段 7：质量收口

执行 Release 单元测试、Mock 集成测试和 UDP Loopback 测试；运行格式检查和项目已有静态分析；检查中文 Doxygen；记录构建环境、命令、结果和限制；按仓库要求追加实现与测试结果文档。

## 9. 测试计划

### 9.1 单元测试

- 8 条命令黄金报文、reserved 字节、字段对齐、Intel/Motorola。
- MAX_CTO 边界、ElementCount 极值、地址溢出。
- RES/ERR/EV/SERV/DTO 分类。
- CONNECT、GET_STATUS、GET_COMM_MODE_INFO 字段解析。
- 空包、截断包、错误长度、超限包、未知 Error/Event Code。
- Error 附加信息保留。

### 9.2 AG 参数化测试

对 AG=1/2/4 覆盖 MAX_CTO/MAX_DTO 整除校验、Element/Byte 换算、UPLOAD/SHORT_UPLOAD 边界、单包/跨包/尾块、不可整除长度、地址推进与溢出。

### 9.3 UDP Transport 单元测试

- Header 黄金向量：LEN/CTR 固定小端，且不受 Session Intel/Motorola 设置影响。
- LEN=0、正常边界、配置上限、Datagram 小于/大于 `4+LEN`、0~3 Byte 截断 Header。
- 发送和接收 CTR 独立；连续递增、间隙、重复、疑似乱序及 `0xFFFF -> 0x0000` 回绕。
- 响应 CTR 与请求 CTR 不相等时仍能按 pending command 正常处理。
- 非配置远端 IP/端口的 Datagram 被拒绝。
- Socket 打开失败、绑定失败、发送失败、接收 Timeout、取消、关闭中接收。
- 配置的 Datagram 上限、UDP/IP 理论上限及“不允许 IP 分片”策略边界。

### 9.4 Session 与恢复测试

- 未连接读取、连接各阶段失败清理、第二条并发命令拒绝。
- GET_COMM_MODE_INFO 成功、跳过、ERR_CMD_UNKNOWN 降级。
- DISCONNECT 成功、ERR_CMD_BUSY、Transport 中断、重复断开。
- Timeout → SYNCH/ERR_CMD_SYNCH → 重试成功。
- SYNCH 异常响应、两次恢复耗尽、Transport 断开时禁止 SYNCH。
- UPLOAD Timeout 后先恢复 MTA。
- EV_CMD_PENDING 不增加原命令发送次数。
- ERR_ACCESS_LOCKED 明确报告范围限制。

### 9.5 Mock Transport 集成测试

1. CONNECT → GET_COMM_MODE_INFO → GET_STATUS → SHORT_UPLOAD → DISCONNECT。
2. GET_COMM_MODE_INFO/SHORT_UPLOAD 返回 ERR_CMD_UNKNOWN 后降级。
3. 按 AG/MAX_CTO 多块 UPLOAD。
4. 等待 RES 时插入 EV/SERV，不打乱匹配。
5. 多个 EV_CMD_PENDING 后收到最终 RES。
6. 中间块 Timeout，经 SYNCH 和 SET_MTA 后成功。
7. 畸形包、协议错误、Transport 错误分类正确。

### 9.6 UDP Loopback 端到端测试

1. UdpTestSlave 绑定 `127.0.0.1` 临时端口，UdpTransport 使用动态取得的端口，避免端口冲突。
2. 真实 Datagram 完成 CONNECT → GET_STATUS → SHORT_UPLOAD → DISCONNECT。
3. 验证 UDP Payload 为 `LEN(u16le)+CTR(u16le)+XCP Packet`，并检查两个方向独立 CTR。
4. 验证 SET_MTA+多块 UPLOAD，确保 Transport Header 不进入协议数据。
5. 注入丢响应和延迟，验证 Timeout/SYNCH；注入 EV_CMD_PENDING，验证不重发原命令。
6. 注入 LEN 不一致、截断 Header、重复/跳号/乱序 CTR 和错误远端，验证丢弃及诊断。
7. 验证关闭/取消能及时结束阻塞接收，测试结束后不残留线程、Socket 或占用端口。
8. Windows、Linux、macOS 均执行；避免依赖广播、外网、防火墙例外和固定时序，降低 CI 偶发失败。

## 10. 验收标准

1. 8 条命令均有 Codec/Parser 覆盖。
2. 能建立/关闭 Session 并保存全部必需参数。
3. AG=1/2/4、Intel/Motorola 下正确轮询原始内存。
4. SHORT_UPLOAD 不可用时自动降级。
5. 严格单 Outstanding Command。
6. Timeout 执行 SYNCH，UPLOAD 重试前恢复 MTA。
7. EV_CMD_PENDING 不导致重复发送。
8. 输入和解析具备长度、范围、溢出保护。
9. Release 单元测试、Mock 集成测试和 UDP Loopback 测试全部通过。
10. UdpTransport 使用标准 4 Byte LEN/CTR Header，固定小端，并与协议层 Byte Order 解耦。
11. UdpTransport 不把 CTR 当事务 ID，能诊断丢包、重复和乱序，不在 Transport 层擅自重排或重传命令。
12. UDP 测试不依赖固定端口、外网或平台专用厂商库，并能可靠取消和释放资源。
13. 核心库不依赖 Qt、A2L 或具体 CAN 厂商库。
14. 未实现功能返回明确错误。
15. 不把 Loopback/Mock 通过表述为真实 ECU/CANape 互操作通过。

## 11. 风险与依赖

1. **Transport 版本边界**：当前依据 XCP on Ethernet 1.0；该版本没有 Discovery 和 GET_DAQ_CLOCK_MULTICAST，不能混入后续版本功能。
2. **CTR 处置存在实现策略**：规范用 CTR 检测缺包，但未定义完整的去重、重排、重传和复位规则；项目策略必须在接口文档和测试中明确，不能冒充标准条款。
3. **UDP 大包与 IP 分片**：LEN 可编码的范围大于普通 IPv4 UDP 可承载范围；配置校验必须结合 65503 Byte 理论上限和项目 MTU 策略。
4. **Loopback 不等于互操作**：UdpTestSlave 验证自有两端实现，可能共享同一误解；后续仍需第三方 ECU/CANape 抓包对照。
5. **跨平台 Socket 差异**：Windows 与 POSIX 在错误码、关闭唤醒、超时和端口复用上不同，需用小型平台适配层隔离并在三平台 CI 验证。
6. **仓库无源码基线**：实施前确认库名、命名空间、产物和测试框架。
7. **规范校核**：协议命令字段对照 Protocol Layer，UDP Header 对照 Ethernet Associated Standard；二者字节序不可混用。
8. **Timeout 数值来源**：t1..t7 通常来自 A2L；本阶段由调用方配置并记录默认值来源，不宣称协议固定值。

## 12. 后续里程碑（本计划外）

1. 最小 A2L：MEASUREMENT、地址、数据类型、Byte Order、COMPU_METHOD。
2. XCP on CAN/CAN FD 或 XCP on TCP/IP。
3. 与第三方真实 ECU/CANape 的 UDP 互操作。
4. Dynamic DAQ、ODT、DTO、Timestamp。
5. Ethernet 后续版本 Discovery、GET_SLAVE_ID、GET_DAQ_CLOCK_MULTICAST 和 IPv6。
6. Seed&Key、Calibration、MDF4、Programming、Advanced Time Correlation。
