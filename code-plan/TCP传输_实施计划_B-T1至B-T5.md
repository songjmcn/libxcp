# XCP on TCP/IP 传输——双 Profile 实施计划（批次 B-T1～B-T5）

日期：2026-10-22　　状态：**双 Profile 修订完成，待实施批准**（尚未修改生产代码）

规范与兼容性基线：

- `docs/ASAM-AE-MCD-1-XCP-AS-Ethernet-Transport-Layer-V1-5-0.pdf`：ASAM Ethernet Transport Layer V1.5 规范真值；
- `docs/XCP_-Part_3-_Transport_Layer_Specification_XCP_on_Ethernet_(TCP_IP_and_UDP_IP)_-1.0.pdf`：历史 Version 1.0，仅用于交叉核对；
- `code-plan/V1.5规范实现与XCPlite兼容性评估.md`：差异输入材料，其中与真实源码冲突的结论不得直接作为实现依据；
- `thirdparty/XCPliteV1.2`：ASAM V1.4 兼容 Profile 的首要互操作参考实现；当前 checkout 明确配置 Protocol Layer `0x0102`、Transport Layer `0x0104`；
- A2L `XCP_ON_TCP_IP` 语义以当前 AML、上游 `a2llib` 数据模型和本项目桥接结果共同校验。

> **命名说明**：按项目决策，原拟议名称 `XcpLiteV12` 统一改为 `AsamV14`。`AsamV14` 表示 TCP/Ethernet **传输层 Profile V1.4**，不是产品名，也不表示当前参考 XCPlite 的 Protocol Layer 是 V1.4。当前源码证据仍是 PL 1.2 / TL 1.4；两层版本必须分别保存和裁决。

已确认约束：

1. 保持 B-T1～B-T5 五批结构；
2. 同时支持显式 `AsamV15` 与 `AsamV14` TCP Wire Profile，优先保证 `thirdparty/XCPliteV1.2` 互操作；
3. Profile 必须显式选择；禁止**连接内**自动探测、按首帧猜测、超时窥探或静默回退。
   > **P3 阶段的措辞收窄**（2026-10-25）：本条禁止的是"同一条连接内按字节内容猜测"，
   > 不禁止**连接级**候选试连。经批准新增的自动模式 `--tcp-profile auto` 会按固定阶梯
   > （`AsamV14/8bit → AsamV15/8bit → AsamV15/16bit → AsamV15/32bit`）为每个候选**新建**
   > 连接试一次，失败即断开，绝不污染已有流；该模式必须由调用方显式开启。
   > 详见 `code-plan/传输帧布局自动协商_实施计划.md` 与
   > `code-plan/传输帧布局自动协商_实施记录_P1至P3.md`。
4. Frame Header codec 由 UDP/TCP 共享，TCP stream framing 按 Profile 分离；
5. 本期包含 Master TCP 客户端、测试 TCP Slave、CLI 和真实 XCPlite 互操作；
6. 不做自动重连或 RESUME；
7. 生产实现限定 IPv4 单播；远端允许 IPv4 literal 或可解析为 IPv4 的 DNS hostname；IPv6 返回明确的 `UnsupportedFeature`；
8. 每批代码变更同步增加测试和实施记录，不把接口测试统一推迟到后续批次。

---

## 0. 总体目标、范围和完成定义

当前生产库只有 `UdpTransport`。本计划新增 `TcpTransport : IXcpTransport`，实现 XCP Master 通过 TCP/IP 与以下两类对端通信：

- **AsamV15**：标准 LEN/CTR、显式 Packet Alignment、LEN 外 Tail；
- **AsamV14**：兼容当前 XCPlite TL 1.4 wire 行为，严格按 `Header + LEN bytes` 切帧，不读取 LEN 外 Tail，并允许 DTO/异步 CRM 的 LEN 内扩长。

生产侧只实现 TCP 客户端。测试目录提供支持两种独立 Wire Profile 的 loopback TCP Slave；它只作为协议和故障注入对手件，不进入生产库。

### 0.1 完成定义

只有同时满足以下条件才可声明本期完成：

- Wire Profile 未显式确定时拒绝 Open/启动；
- AsamV15 的 LEN/CTR、字节序、PACKET_ALIGNMENT 和 Tail 符合 Ethernet TL V1.5；
- AsamV14 严格按 `4 + LEN` 解析，不消费 LEN 外 Tail；发送 CRO 永不追加 LEN 外 Tail；
- 对 XCPlite DTO/异步 CRM 的 LEN 内扩长完整保留，不按尾部零值通用裁剪；
- TCP byte stream 下分片、粘包、多 Frame 单次 recv、FIN/RST 均有明确状态机和测试；
- Open/Send/Close/ReceiveLoop 在线程安全和对象生命周期上无句柄复用、自连接 join、重复回调风险；
- CTR 发送/接收相互独立，并按 Profile 执行不同接收策略；
- 协议层版本下限扩展到 1.2，PL/TL 分开裁决，不能把 TL 1.4 当作 PL 1.4；
- CLI 的端点、Wire Profile、alignment、协议版本证据和 A2L 优先级可执行且有测试；
- Release 清理构建和相关全量测试通过；
- `thirdparty/XCPliteV1.2` 真实进程完成 Connect、Polling/标定、DAQ DTO、Disconnect 互操作；
- 至少另有一项 AsamV15 独立实现互操作，或在发布声明中明确标记未验证，不能用自有 Master/Slave 闭环代替。

### 0.2 明确不做

- 生产级 TCP Slave/listener；
- Wire Profile 自动识别或连接中切换；
- 自动重连、RESUME 和断线会话恢复；
- 多 Frame 主动聚合成一次 write；
- Optional TL Subcommand：`GET_SLAVE_ID`、`GET_SLAVE_ID_EXTENDED`、`SET_SLAVE_IP_ADDRESS`、`GET_DAQ_CLOCK_MULTICAST`；
- IPv6 连接；
- TLS；
- 多 Master 或连接池。

“不做”项必须体现在能力矩阵和错误诊断中，不得静默降级。

---

## 1. 规范不变量与双 Wire Profile

### 1.1 共同 Header 与长度域

- Header 固定 4 字节：`LEN` WORD + `CTR` WORD；均按 little-endian 编解码；
- `LEN` 合法域为 `1..0xFFFF`；空 XCP Packet 在本库拒绝；
- 通用 Ethernet Header codec 的 LEN 上限为 `65535`；
- UDP 仍受 IPv4 UDP payload 限制：`65507 - 4 = 65503`，该限制只存在于 UDP wrapper/config；
- TCP 是 byte stream，单次 write/recv 边界没有协议意义；
- 任何 decoder 都必须先校验 LEN 和配置上限，再扩容或等待数据。

```cpp
constexpr std::size_t kEthernetHeaderSize = 4U;
constexpr std::size_t kEthernetMaxXcpPacket = 0xFFFFU;
constexpr std::size_t kEthernetMaxAlignmentBytes = 4U;
constexpr std::size_t kEthernetMaxFrameSize = 65540U;

// 仅 UDP。
constexpr std::size_t kUdpMaxDatagramSize = 65507U;
constexpr std::size_t kUdpMaxXcpPacket = 65503U;
```

### 1.2 Wire Profile 类型

```cpp
enum class TcpWireProfile : std::uint8_t {
    Unknown = 0,
    AsamV15,
    AsamV14,
};
```

- `Unknown`：配置未完成，`Open()` 必须返回 `InvalidArgument`；
- `AsamV15`：LEN 是真实 XCP Packet 长度，不含 Header/Tail；Tail 位于 LEN 外；
- `AsamV14`：每帧严格为 `[LEN][CTR][LEN bytes]`，不读取或发送 LEN 外 Tail；
- Profile 是连接级不可变属性，Open 后不得修改；
- 禁止 `Auto` 枚举进入 decoder，禁止用版本号、A2L alignment 或首帧内容单独推断 Profile；
- 即使 alignment=8 时两种 wire 形式暂时等价，仍要求显式 Profile，以保持版本、CTR 和能力策略可审计。

### 1.3 AsamV15 Tail 契约

`PACKET_ALIGNMENT` 为 8/16/32 bit，Tail 不计入 LEN。发送端按：

```text
tail = (alignment_bytes - ((4 + LEN) mod alignment_bytes)) mod alignment_bytes
```

始终补足 Tail，作为本库确定性发送策略。

接收状态机：

1. `NeedHeader`：累计 4 字节；
2. `NeedPayload`：累计 LEN 字节；Payload 完整后仅投递一次；
3. `NeedInterFramePadding(N)`：连接继续且有后续数据时消费完整 N 字节 Tail；
4. EOF 在 Payload 完整且尚未收到任何可选末尾 Tail 时合法；EOF 时仅收到 `1..N-1` 字节 Tail、半 Header 或半 Payload 均为 truncated frame；
5. Alignment origin 是当前连接首个 Ethernet Header 的 byte offset 0；后续 Header 起点相对该 origin 对齐。

Tail 内容不解释。禁止以“值为 0”识别 Tail。

### 1.4 AsamV14 兼容契约

当前 XCPlite 参考实现的可验证 wire 行为作为本 Profile 的首要互操作基线：

- Master TX：只发送 `Header + 真实 CRO`，绝不附加 LEN 外 Tail；
- Slave RX：先精确读取 4 字节 Header，再精确读取 LEN 字节；多帧可连续出现在同一 TCP segment；
- 同步 CRM：LEN 通常为真实 CRM 长度；
- DTO/异步 CRM：队列可能把 message 总长按 4 字节扩长，并把扩长字节计入 LEN；这不是 AsamV15 的 LEN 外 Tail；
- decoder 每取得 `4 + LEN` 字节立即回到 `NeedHeader`，绝不进入 `NeedTail`；
- Transport 必须完整上交 LEN 字节；禁止按尾部零值裁剪；
- 已知命令响应只能由 response schema 忽略预期字段后的扩长；DAQ DTO 只能由已知 ODT layout 计算逻辑 payload；未知/原始包保持 wire payload 不变；
- 禁止“读完 LEN 后再等待 0.5 秒尝试吞 padding”等启发式逻辑，这会误吞下一帧 Header。

诊断模型必须区分：

```text
wire_payload_length     = LEN
logical_payload_length  = schema/layout 确认的长度；未知时等于 LEN
profile_padding_length  = 仅在 schema/layout 可证明时计算
```

不要求 Transport 改变 `IPacketListener` 的 packet 形态；若上层需要暴露上述诊断，应通过解析结果或 tracing 元数据承载，不能破坏原始 payload。

### 1.5 CTR 契约

共同规则：

- 发送和接收 CTR 相互独立；
- 本库每次成功 Open 后发送 CTR 从 0 开始，每生成一个新 Frame 后 `+1 mod 65536`；
- 接收首个合法 Frame 建立基线，允许首值任意；
- `0xFFFF -> 0` 是正常回绕；
- CRM、DTO、EV、SERV 共用同一方向的接收 CTR 序列，禁止按包类型分别维护。

Profile 差异：

- **AsamV15**：后续必须严格 `current == previous + 1 mod 65536`；duplicate/jump/backward 发 warning 后关闭连接；
- **AsamV14**：duplicate/backward 视为协议异常并关闭；forward gap 只记录丢帧诊断并继续，因为 XCPlite 的发送队列丢弃会主动推进全局 CTR；
- AsamV14 peer 的 CTR 可能在 TCP 断开/重连后继续增长，因此新连接首个 RX CTR 不要求为 0；
- 本库 TX CTR 是否从 0 开始是本地策略，不得用来反推 peer RX CTR；
- UDP 原有半域容错策略不变，禁止直接复用为 TCP 策略。

### 1.6 协议层与传输层版本

- Wire Profile、Protocol Layer 版本和 Transport Layer 版本是三个独立维度；
- 当前 `thirdparty/XCPliteV1.2` 声明 PL 1.2 / TL 1.4，CONNECT 只能确认 major=1；
- 当前 PL 1.2 构建中的 `GET_VERSION` 分支被 `#if XCP_PROTOCOL_LAYER_VERSION >= 0x0104` 排除，预期返回 `ERR_CMD_UNKNOWN`，不得把源码中存在未编译分支误判为运行时支持；
- libxcp 的协议支持下限必须从 1.3 扩展到 1.2，新增 `kProtocolVersion12`；
- “次版本未知”不得再固定保守到 1.3；AsamV14/XCPlite 场景应回落到显式或 A2L 的 PL 1.2 证据；
- A2L、显式配置和运行时 GET_VERSION 的冲突按现有证据优先级裁决并输出诊断；运行时可用时仍最高优先；
- AsamV14 Profile 本身只说明 TL wire policy，不能伪装成 PL 版本证据；CLI 兼容 preset 可原子展开为 `wire=AsamV14, expected PL=1.2, expected TL=1.4`，但各证据来源仍须保留。

### 1.7 地址族和名称解析

- `remote_host` 允许 IPv4 literal 或 DNS hostname；
- 使用 `getaddrinfo(AF_INET, SOCK_STREAM)`，全部 IPv4 connect 尝试共享一个 monotonic deadline；
- 同步 DNS 耗时不纳入可移植的 `connect_timeout_ms` 保证，须在 API 注释中明示；
- IPv6 literal、A2L IPv6-only endpoint 和显式本地 IPv6 bind 返回 `UnsupportedFeature`；
- `local_host` 只允许 IPv4 literal 或 `0.0.0.0`；
- 显式 bind 失败是致命 `TransportError`；只有 `0.0.0.0:0` 表示允许 OS 自选。

---

## 2. 文件布局与公共接口

| 文件 | 动作 | 说明 |
|---|---|---|
| `include/libxcp/ethernet_header_codec.hpp` / `src/ethernet_header_codec.cpp` | 新增 | 共同 LEN/CTR Header 和 checked length helper；不承载 Profile 状态 |
| `include/libxcp/udp_header_codec.hpp` / `src/udp_header_codec.cpp` | 修改 | 兼容转发层；保留 UDP 类型、函数和 65503 上限 |
| `include/libxcp/tcp_stream_decoder.hpp` / `src/tcp_stream_decoder.cpp` | 新增 | 持有显式 Wire Profile；两条确定性状态机 |
| `include/libxcp/tcp_transport_config.hpp` | 新增 | TCP 端点、Wire Profile、alignment 和 timeout |
| `include/libxcp/tcp_transport.hpp` / `src/tcp_transport.cpp` | 新增 | Master TCP 客户端 |
| `include/libxcp/protocol_version.hpp`、`src/protocol_version.cpp` | 修改 | 支持下限扩展到 PL 1.2 |
| `src/version_resolver.cpp`、`src/xcp_master.cpp` | 修改 | 1.2 证据与 conservative 策略；A2L 版本输入接线 |
| `src/socket_util.hpp` / `src/socket_util.cpp` | 新增 | internal 跨平台 socket 工具 |
| `CMakeLists.txt` | 修改 | 新生产源文件入列 |
| `tests/ethernet_header_codec_test.cpp` | 新增 | Header、长度和 UDP 兼容测试 |
| `tests/tcp_stream_decoder_test.cpp` | 新增 | 双 Profile 状态机 golden 测试 |
| `tests/tcp_transport_test.cpp` | 新增 | B-T2 同批接口、并发和 socket 行为测试 |
| `tests/tcp_test_slave.hpp/.cpp` | 新增 | B-T3 双 Profile loopback Slave |
| `tests/xcp_tcp_loopback_test.cpp` | 新增 | XcpMaster 双 Profile 集成测试 |
| `tests/CMakeLists.txt` | 修改 | 新测试入列 |
| `tools/xcp_master/main.cpp` | 修改 | 显式 `--tcp-profile`、端点、alignment、版本诊断 |
| `thirdparty/a2l-sdk/**` | B-T4 单独批准后修改 | TCP endpoint、版本和 Profile hint 来源；不改 `thirdparty/a2llib` |
| `docs/modules/传输层.md`、`README.md` | 修改 | 双 Profile 能力矩阵与限制 |
| `code-plan/TCP传输_实施记录_B-Tn_*.md` | 每批新增 | 记录变更、测试、平台和未验证项 |

```text
XcpCore -> IXcpTransport -> UdpTransport / TcpTransport
                               |             |
                               +-- Ethernet Header Codec
                                             |
                               TcpStreamDecoder(profile)
                                 |           |
                              AsamV15     AsamV14
```

`SendCtr()`、`LastReceiveCtr()`不是 `IXcpTransport` 虚接口；若保留，它们只是具体 Transport 的非虚诊断 API。

---

## 3. TcpTransportConfig 与选择契约

```cpp
namespace calmcar::xcp {

/// @brief XCP on TCP/IPv4 Master 客户端配置。
struct TcpTransportConfig {
    std::string remote_host;
    std::uint16_t remote_port{0};
    std::string local_host{"0.0.0.0"};
    std::uint16_t local_port{0};
    std::uint32_t connect_timeout_ms{3000};
    std::uint32_t io_poll_interval_ms{100};
    std::uint32_t send_timeout_ms{3000};
    std::size_t max_frame_packet_size{kEthernetMaxXcpPacket};
    TcpWireProfile wire_profile{TcpWireProfile::Unknown};
    PacketAlignment packet_alignment{PacketAlignment::Unknown};
};

}  // namespace calmcar::xcp
```

校验矩阵：

| 配置 | AsamV15 | AsamV14 |
|---|---|---|
| `wire_profile` | 必须显式 | 必须显式 |
| `packet_alignment` | 必须是 Bit8/16/32 | 必须为 Unknown；非 Unknown 作为冲突拒绝，防止误转成 LEN 外 Tail |
| `max_frame_packet_size` | 1..65535 | 1..65535；会话建立后还受 MAX_CTO/MAX_DTO 约束 |
| Profile 自动识别 | 禁止 | 禁止 |

其他校验：

- `remote_host` 非空，`remote_port != 0`；
- `local_host` 是 IPv4 literal；
- timeout 和 poll interval 大于 0，平台类型转换不溢出；
- IPv6 literal 返回 `UnsupportedFeature`，其他语法错误返回 `InvalidArgument`；
- Open 日志必须输出 resolved Wire Profile、Profile 来源、alignment、endpoint；不得记录 payload。

Profile 只允许来自 API/CLI 或部署配置中的显式字段。A2L/vendor hint 只能进入诊断，不能替调用方完成选择；无显式值时保持 Unknown 并 fail closed。

标准 A2L 的 `XCP_ON_TCP_IP`、PL/TL 版本、`PACKET_ALIGNMENT` 或 vendor hint 单独存在时都不得自动切换 Wire Profile。

---

## 4. 批次 B-T1：Header codec、双 Stream Decoder 与 PL 1.2 基线

### 4.1 通用 Header codec

新增：

- `EthernetHeader { std::uint16_t len; DatagramCtr ctr; }`；
- Header encode/decode；
- checked LEN、Tail 长度和 Frame 总长度 helper；
- AsamV15 aligned frame encoder；
- AsamV14 unpadded frame encoder；
- 所有长度运算先做 overflow-safe 检查；
- Packet 超过 65535、空 Packet、Profile/Alignment 冲突抛 `InvalidArgument`。

`udp_header_codec` 保留既有 API，通过别名/薄转发调用共同 Header/Tail helper；UDP wrapper 继续执行 `<=65503` 限制。禁止保留两份 LEN/CTR 算法。

### 4.2 TcpStreamDecoder

```cpp
class TcpStreamDecoder {
public:
    TcpStreamDecoder(TcpWireProfile profile,
                     PacketAlignment alignment,
                     std::size_t max_packet_size);
    DecodeResult Push(BytesView bytes);
    DecodeResult Finish();
    void Reset();
};
```

- AsamV15：`NeedHeader -> NeedPayload -> NeedInterFramePadding -> NeedHeader`；
- AsamV14：`NeedHeader -> NeedPayload -> NeedHeader`；
- Profile 在构造后不可变；
- 一次 Push 可返回零到多个拥有稳定存储的 Frame；
- 禁止返回在内部 buffer erase 后悬空的 `BytesView`；
- decoder 只做 wire framing，不按内容猜 CRM/DTO，也不裁剪 AsamV14 LEN 内扩长；
- 删除任何 opportunistic Tail consume、超时窥探、按填充值识别和逐帧自动识别设计。

### 4.3 PL 1.2 版本基线

同批修改版本模型：

- 新增 `kProtocolVersion12{1, 2}`；
- 显式版本校验允许 1.2..1.5；
- 为 PL 1.2 建立 capability set；至少禁止 1.3+/1.4+ 专属命令及 `START_STOP_DAQ_LIST` mode 3；
- GET_VERSION 对 PL 1.2 peer 的 `ERR_CMD_UNKNOWN` 是 unavailable，不是连接失败；
- 一般已连接 CRO 超时仍是 CommandTimeout/LinkSuspect，不能泛化成“老版本合法无响应”；
- RESOURCE 只作粗粒度资源门控；A2L OPTIONAL_CMD 与命令响应用于命令级能力判断。

### 4.4 B-T1 测试 Gate

共同：

- LEN/CTR little-endian golden vector；
- LEN=1、65503、65504、65535；LEN=0 和 65536 拒绝；
- Header/Payload 每个 byte boundary 分片；2/3 帧粘包；
- 半 Header/半 Payload 后 FIN 失败；
- UDP 旧 API 和全部 `udp_*` 测试回归；
- PL 1.2/1.3/1.4/1.5 版本比较、冲突和 capability gate。

AsamV15：

- Bit8/16/32 的 0/1/2/3 字节 Tail；
- 65535 + 32-bit alignment 总长不越界；
- Tail 每个边界分片；末帧 Tail 省略/完整合法、部分 Tail 非法；
- 连续非对齐 Frame golden：`frame+tail+next header` 不失步。

AsamV14：

- 连续非对齐 CRM golden：`frame+next header` 不失步；
- 一次 Push 中多个完整 Frame 和半个后续 Frame；
- DTO LEN 内扩长完整保留；尾部合法零数据不得被裁剪；
- 错误地附加 LEN 外 Tail 后必须稳定进入 malformed/长度保护路径，不得启发式恢复；
- AsamV15/AsamV14 交叉 Profile 负向测试。

B-T1 验收：Release 清理构建通过，双 decoder、版本模型和 UDP 回归全绿，生成实施记录。

---

## 5. 批次 B-T2：TcpTransport

### 5.1 类接口

```cpp
class TcpTransport final : public IXcpTransport {
public:
    explicit TcpTransport(TcpTransportConfig config);
    ~TcpTransport() override;

    void Open(IPacketListener& listener) override;
    void Close() override;
    void Send(BytesView packet) override;
    [[nodiscard]] bool IsOpen() const noexcept override;

    [[nodiscard]] DatagramCtr SendCtr() const noexcept;
    [[nodiscard]] std::optional<DatagramCtr> LastReceiveCtr() const;

private:
    void ReceiveLoop();
};
```

回调使用真实接口名：`OnPacketReceived`、`OnTransportWarning`、`OnTransportClosed`。

### 5.2 Open 与 socket

1. 配置校验先于 socket 创建；
2. `getaddrinfo(AF_INET, SOCK_STREAM)` 解析远端；
3. 所有 IPv4 candidate 共用 monotonic connect deadline；
4. 显式 local bind 失败立即关闭并抛错；
5. non-blocking connect + writable wait + `SO_ERROR`；处理 EINTR；
6. 设置 `TCP_NODELAY`、`SO_KEEPALIVE`；Linux 使用 `MSG_NOSIGNAL`，macOS 设置 `SO_NOSIGPIPE`；
7. 以显式 Profile 构造 decoder，初始化发送 CTR=0，清接收基线；
8. 状态 Open 后启动 ReceiveLoop；
9. 失败路径回收句柄、清 listener、恢复 Closed，不发 `OnTransportClosed`。

### 5.3 生命周期与锁序

状态：`Closed -> Opening -> Open -> Closing -> Closed`。

- Open/Close 由 lifecycle mutex 串行；重复 Open 抛 `InvalidState`；Close 幂等；
- Send 使用 send mutex 保序；
- Close 先进入 Closing 并 shutdown 稳定句柄，使 send/recv 退出；禁止先等待 send mutex 再 shutdown；
- shutdown 后等待 send 路径退出，再由单一所有者 close；
- ReceiveLoop 是 `OnTransportClosed` 的唯一发出者；主动 Close、peer FIN、RST、decoder/CTR 错误均恰好一次；
- 所有 listener 回调均不持 send/CTR/lifecycle mutex；
- Close 返回后不再有回调并清 listener；
- 从接收回调线程直接 Close 返回 `InvalidState`，避免 self-join；
- XCPlite peer 在旧连接关闭后约有 100ms 清理路径，立即重连测试必须使用 bounded backoff，而不是假定 backlog 代表会话可用。

### 5.4 Send

- 检查 Open、packet 非空及长度上限；
- 在 send mutex 内取得 CTR，并按 Profile 编码；
- AsamV15 追加确定性 LEN 外 Tail；AsamV14 只发送 `4+LEN`；
- non-blocking send + writable wait 循环写完，统一受 monotonic `send_timeout_ms` 控制；
- 处理 partial write、EINTR、EAGAIN/EWOULDBLOCK；
- Close/shutdown 可取消发送；
- timeout、EPIPE、RST 进入统一断链流程并抛 `TransportError`；
- CTR 在 Frame 进入发送事务时消费，失败不复用旧值；
- 日志只记录 Profile、长度和 CTR。

### 5.5 ReceiveLoop

- readable wait + `io_poll_interval_ms` 检查关闭状态；
- `recv>0` 输入绑定 Profile 的 decoder；
- AsamV15 严格 CTR；AsamV14 forward gap warning 后继续，duplicate/backward 关闭；
- 释放锁后调用 `OnPacketReceived`；
- `recv==0` 调用 `Finish()`，按 Profile 判定剩余状态；
- socket error、decoder malformed、严重 CTR violation 统一断链；
- callback 异常不得逃逸工作线程；
- 缓冲上限按配置、Profile 和 decoder 状态计算，不使用 UDP 65507 常量。

### 5.6 B-T2 测试 Gate

- Profile/Alignment 配置矩阵；Unknown Profile、AsamV15 Unknown alignment、AsamV14 非 Unknown alignment 全部拒绝；
- DNS、IPv4、IPv6 Unsupported、timeout=0、长度边界；
- Open success/refused/timeout、getaddrinfo 失败、多 candidate 共用 deadline；
- bind 失败及 `getsockname` 成功验证；
- partial send、EINTR、EAGAIN、send timeout、RST、Close 取消 send；
- Send/Close 并发、重复 Close、重开清 decoder 和本地 TX CTR；
- `OnTransportClosed` 恰好一次，Close 后零 callback；
- listener 回调读取 CTR 不死锁；回调线程 Close 稳定失败；
- Linux/macOS 无 SIGPIPE；
- 两种 Profile 的连续 CRM/DTO、分片、粘包和 LEN=65535；
- AsamV15 strict CTR 与 AsamV14 gap-diagnostic 策略分别测试；
- AsamV14 新连接首个 RX CTR 任意及跨 reconnect 非零。

`socket_util` 提供 internal 可注入 `SocketOps`/函数表，使 partial write、EINTR、timeout 测试确定性运行。

B-T2 验收：Release 清理构建通过，B-T1/B-T2 和 UDP 回归全绿，生成实施记录。

---

## 6. 批次 B-T3：双 Profile Test Slave 与 XcpMaster 集成

### 6.1 TCP Test Slave

- 显式选择 AsamV15 或 AsamV14，不支持 Auto；
- 绑定 `127.0.0.1:0`，暴露实际端口；
- accept 一条活动连接；第二连接需 accept 后显式拒绝/关闭；
- 两种模式使用独立测试侧 framing；关键 golden 不全部复用生产 decoder；
- 支持 CONNECT、DISCONNECT、GET_STATUS、GET_COMM_MODE_INFO、SET_MTA、UPLOAD、SHORT_UPLOAD、DOWNLOAD、SYNCH；
- 为 Dynamic DAQ 补齐 FREE_DAQ、ALLOC_DAQ、ALLOC_ODT、ALLOC_ODT_ENTRY、SET_DAQ_PTR、WRITE_DAQ、SET_DAQ_LIST_MODE、START_STOP_DAQ_LIST、START_STOP_SYNCH 和 DTO 推送；否则删除 Dynamic DAQ 验收；
- AsamV14 模式可生成同步 CRM 无扩长、异步 CRM/DTO LEN 内 4 字节扩长和 CTR gap；
- Stop 使用 shutdown + join，不遗留线程。

故障注入：

- Header/Payload/Tail 每个边界分片；
- 多帧单 write；
- AsamV15 末帧无 Tail/完整 Tail/部分 Tail；
- AsamV14 LEN 内扩长、无外部 Tail；
- 半帧 FIN、完整帧 FIN、RST；
- duplicate/jump/backward CTR；
- corrupt LEN；
- 延迟读取触发 partial send/timeout；
- 第 N 个响应后断链；
- Profile mismatch。

### 6.2 集成场景

两种 Profile 参数化执行：

1. `XcpMaster(std::unique_ptr<TcpTransport>)` Connect；
2. GET_VERSION 可用和 unavailable 两条路径；AsamV14/当前 XCPlite PL1.2 的 unknown-command 路径不得导致连接失败；
3. Polling：SET_MTA + UPLOAD/SHORT_UPLOAD；
4. 标定写入并回读；
5. Dynamic DAQ 使用 Select + START_STOP_SYNCH；PL1.2 禁止 mode 3；
6. CRM 与 DTO 同一 stream、共享 RX CTR；
7. `Disconnect()` 先发送 XCP DISCONNECT，再关闭 TCP；
8. peer FIN/RST/严重 framing 或 CTR error 后会话不可复用；
9. 新建连接不自动恢复旧会话。

### 6.3 B-T3 Gate

- 双 Profile 所有单元和 loopback 集成测试通过；
- 全量 `libxcp_tests` 通过；
- 不依赖公网、固定端口或 backlog；
- 实施记录区分“自有闭环通过”和“独立实现互操作状态”。

---

## 7. 批次 B-T4：A2L SDK、版本证据与桥接

> 本批修改 `thirdparty/a2l-sdk`，实施前必须再次获得 thirdparty 修改批准。若不批准，CLI 必须允许显式 endpoint、Profile、PL/TL 版本完成双 Profile 连接，并标记“A2L TCP 自动提取未支持”。

### 7.1 Endpoint 数据语义

保留 TCP 的 HOST_NAME、ADDRESS、IPV6、PORT、PACKET_ALIGNMENT、Optional TL Subcommand 列表和实例数；不得塞入 `udp_*` 字段。

追加 selected endpoint 字段并保留旧 UDP 字段一个兼容周期：

```text
endpoint_port
endpoint_host_name
endpoint_ipv4_address
endpoint_ipv6_address
endpoint_packet_alignment
endpoint_alignment_declared
endpoint_sub_commands
endpoint_instance_count
endpoint_protocol_version
endpoint_transport_version
endpoint_profile_hint
endpoint_profile_hint_declared
```

- UdpIp 同时填旧 `udp_*` 与新字段；TcpIp 只填新字段；
- 旧字段 deprecated，本期不删除；
- DTO 大小变化触发 ABI v8 -> v9；
- 导出、DiffIfData、桥接、golden 和 ABI gate 同步更新。

### 7.2 Profile 与版本来源

- 标准 A2L `PACKET_ALIGNMENT` 不等于 Wire Profile；
- XCPlite A2L 可能未声明 PACKET_ALIGNMENT，AML 默认值也不能作为“明确声明”；
- PL=1.2、TL=1.4 或 alignment=32 均不足以唯一识别 AsamV14；
- deployment/vendor hint 仅作为诊断候选并保留来源；无论是否存在 hint，都要求 CLI/API/部署配置显式选择，禁止 hint 自动生效；
- A2L PL/TL 版本必须真正接入 `VersionResolver::Inputs`，来源标记为 `A2l`，不得借 `Configured` 冒充；
- CONNECT 仅校验 major；GET_VERSION unavailable 时回落 A2L/显式证据；冲突结构化报告。

### 7.3 Endpoint 选择

- CLI host+port 优先；
- 否则 A2L TCP endpoint 优先 ADDRESS，其次 HOST_NAME；
- IPv6-only 明确 Unsupported；
- ADDRESS 与 HOST_NAME 同时存在时不做 DNS 失败后的隐式跨来源重试；
- 多个 TCP 实例且 CLI 未覆盖时 fail closed；
- Optional TL Subcommand 只透出元数据，本期不执行。

### 7.4 B-T4 测试 Gate

- TCP-only A2L：HOST_NAME、ADDRESS、IPV6、PORT、alignment；
- alignment 缺失与明确 Bit8 可区分；
- PL1.2/TL1.4 证据导出和 bridge；
- profile hint 声明/未声明可区分；无 hint 不自动选 Profile；
- A2L 与 CLI Profile/PL/TL 冲突矩阵；
- 多实例 ambiguous；UDP-only golden 回归；
- SDK ABI v9、DiffIfData、bridge mapping；
- 全仓 DTO 字段引用迁移清单。

---

## 8. 批次 B-T5：CLI、真实互操作与发布文案

### 8.1 CLI

新增：

```text
--transport udp|tcp
--tcp-profile asam-v1.5|asam-v1.4
--packet-alignment 8|16|32
--protocol-version 1.2|1.3|1.4|1.5
--transport-version 1.4|1.5
```

规则：

1. TCP 必须显式选择 `--tcp-profile`；A2L/vendor hint 只用于冲突诊断，不得代替该选项；
2. `asam-v1.5` 必须取得 alignment：CLI 显式值 > 对应 TCP A2L 明确声明值；都没有则失败；
3. `asam-v1.4` 禁止把 `--packet-alignment` 转成 LEN 外 Tail；同时给出该选项冲突时直接报错；
4. `--host` 与 `--port` 成对出现；都无时才从 A2L 选 TCP endpoint；
5. endpoint、alignment、Profile 必须属于同一 transport kind；
6. `--protocol-version` 与 `--transport-version` 是独立证据；不能因 `asam-v1.4` 自动声称 PL1.4；
7. 为当前 XCPlite 部署可提供内部 convenience preset，原子展开为 `tcp-profile=asam-v1.4, PL=1.2, TL=1.4`，但日志和 status 必须展示展开值及来源；
8. 现有 `--daq-profile generic|xcplite` 若存在，保持与 TCP Wire Profile 分离，不能隐式改变 framing；
9. UDP 默认行为保持兼容。

状态输出至少包含：resolved Wire Profile、来源、alignment、reported/effective PL/TL、GET_VERSION outcome、被降级能力和 endpoint 来源。

### 8.2 XCPlite 真实互操作 Gate（最高优先级）

以 `thirdparty/XCPliteV1.2` 构建并启动 TCP 进程，测试必须使用真实 socket 和真实 XCPlite framing，不得替换其 codec：

- Connect 并确认 PL major=1/TL major=1；
- GET_VERSION 预期 unavailable/`ERR_CMD_UNKNOWN`，随后依据显式或 A2L 证据解析为 PL1.2/TL1.4；
- GET_STATUS、Polling Upload；
- 标定写入和回读；
- Dynamic DAQ：Select + START_STOP_SYNCH，不发送 mode 3；
- 接收 DTO LEN 内扩长，依据 ODT layout 解码真实数据；
- CRM/DTO 共用 CTR，人工制造队列 lost 时 forward gap 只告警；
- 多 DTO 单次 send、任意 recv 分片；
- DISCONNECT 后 socket 确实关闭；bounded backoff 后重连；
- peer 主动关闭与异常退出。

任何无法由源码复核的外部 ECU “静默”现象必须标为外部抓包事实，不能泛化为 XCPlite 标准行为。普通已连接命令超时不能视为合法成功。

### 8.3 AsamV15 独立互操作 Gate

使用不复用本库 codec/decoder 的独立对端验证：

- Connect/Disconnect；
- GET_STATUS 或 Polling；
- 对端实际支持的 8/16/32 alignment；
- LEN 外 Tail 与连续多个 Frame；
- strict CTR；
- peer 主动关闭；
- DNS hostname（环境允许时）。

无法获得 AsamV15 独立对端时，本批可结束，但发布声明必须明确“V1.5 自有 loopback 已验证，独立实现未验证”。XCPlite 真实互操作是本期最高优先级 Gate，不得由 loopback 代替。

---

## 9. 批次与验收汇总

| 批次 | 内容 | 必须通过的 Gate | 实施记录 |
|---|---|---|---|
| **B-T1** | Header codec、UDP 兼容层、双 TcpStreamDecoder、PL1.2 基线 | Release 清理构建；双 Profile golden；版本/capability；UDP 回归 | [`code-plan/B-T1_实施记录_以太网通用帧层与双WireProfile解码器.md`](B-T1_实施记录_以太网通用帧层与双WireProfile解码器.md) |
| **B-T2** | socket_util、Config、TcpTransport | Profile 矩阵、生命周期/并发/partial send/FIN-RST/CTR/DNS/bind；UDP 回归 | [`code-plan/B-T2_实施记录_TCP传输与可注入Socket层.md`](B-T2_实施记录_TCP传输与可注入Socket层.md)、补漏 [`code-plan/B-T2补漏_TCP接收CTR校验.md`](B-T2补漏_TCP接收CTR校验.md) |
| **B-T3** | 双 Profile Test Slave、XcpMaster loopback | Polling/标定/DAQ、Disconnect 顺序、framing/CTR 错误；全量测试 | [`code-plan/B-T3_实施记录_双Profile测试Slave与集成.md`](B-T3_实施记录_双Profile测试Slave与集成.md) |
| **B-T4** | A2L SDK v9、版本/Profile 来源、bridge | thirdparty 单独批准；ABI/golden/smoke/bridge 全绿 | [`code-plan/B-T4_实施记录_A2L端点DTO的ABI-v9与TCP端点事实.md`](B-T4_实施记录_A2L端点DTO的ABI-v9与TCP端点事实.md) |
| **B-T5** | CLI、真实 XCPlite、AsamV15 互操作、发布文档 | 显式 Profile 矩阵；XCPlite E2E 必过；V1.5 互操作或明确未验证 | [`code-plan/B-T5_实施记录_CLI传输选择与XCPlite真实TCP互操作.md`](B-T5_实施记录_CLI传输选择与XCPlite真实TCP互操作.md)（补漏：[`B-T5补漏_多实例fail-closed与SDK死字段清理.md`](B-T5补漏_多实例fail-closed与SDK死字段清理.md)） |

```text
B-T1 -> B-T2 -> B-T3
                   |
B-T4 -------------+-> B-T5
```

B-T4 可在 B-T2 后并行准备，但合并时必须先解决 DTO ABI、版本证据和 CLI 数据流交叉修改。

---

## 10. 完整测试矩阵

| 维度 | 必测值 |
|---|---|
| Wire Profile | Unknown、AsamV15、AsamV14、交叉 mismatch |
| LEN | 1、65503、65504、65535、0、越界 |
| AsamV15 Alignment | 8、16、32、Unknown；LEN 外 Tail 0/1/2/3 |
| AsamV14 Framing | 无 LEN 外 Tail、多帧连续、DTO/异步 CRM LEN 内扩长、同步 CRM 无扩长 |
| Stream | Header/Payload/Tail 每字节分片、2/3 帧粘包、半帧 FIN、完整帧 FIN |
| CTR | 首值任意、+1、FFFF→0、duplicate、jump、backward、共享序列、双向独立、reconnect 非零 peer CTR |
| 版本 | PL1.2/1.3/1.4/1.5、TL1.4/1.5、GET_VERSION unavailable、A2L/显式/运行时冲突 |
| Socket | success、refused、timeout、DNS failure、多地址、bind failure、partial send、EINTR/EAGAIN、RST、FIN |
| 生命周期 | 重复 Open/Close、Send/Close 并发、回调恰一次、Close 后零回调、bounded reconnect |
| A2L | ADDRESS、HOST_NAME、IPV6-only、alignment/profile hint 声明状态、多实例、UDP/TCP 冲突 |
| CLI | Profile 必填、端点合并、Profile/alignment/version 冲突、DAQ Profile 独立性 |
| XCP | CONNECT、DISCONNECT、Polling、标定、Dynamic DAQ、PL1.2 capability gate、peer close |
| 互操作 | 真实 XCPlite AsamV14 必过；独立 AsamV15 或明确未验证 |

测试必须确定性运行：不依赖公网黑洞地址、不依赖 listen backlog、不以“大 payload 大概率 partial write”为断言依据。

---

## 11. 风险登记

| 风险 | 缓解措施 |
|---|---|
| 两种 Profile 逐帧自动猜测导致流失步 | 连接前显式选择，decoder 不含 Auto 和超时窥探 |
| AsamV14 名称被误解为 PL1.4 | 文档、类型注释和日志明确其为 TL Wire Profile；PL/TL 分开显示 |
| XCPlite LEN 内扩长被误当 Tail | AsamV14 只读 `4+LEN`；Transport 原样上交；schema/layout 才能裁剪逻辑长度 |
| 尾零裁剪破坏合法测量值 | 禁止按内容通用 trim；保留 wire payload |
| AsamV15 末帧 Tail 可选造成歧义 | 有状态 decoder；Payload 投递与 Padding/EOF 分离 |
| CTR 策略错误统一 | AsamV15 strict；AsamV14 forward gap warning-only；共享一条 RX 序列 |
| PL1.2 被 conservative 提升到 1.3 | 支持下限扩到 1.2；显式/A2L 证据接入 resolver |
| 未编译 GET_VERSION 分支被误判支持 | 以预处理条件和运行时响应为准，PL1.2 测 unknown-command |
| UDP/TCP 共用 codec 回归 UDP | 共同 Header、独立 wrapper；B-T1 同批 UDP 全回归 |
| Send/Close 句柄竞态 | shutdown-first、稳定句柄、明确锁序、唯一 close 所有者 |
| 回调 Close self-join | 接收线程调用返回 InvalidState；Doxygen 和测试固定 |
| DNS 多地址超时倍增 | candidate 共享 monotonic deadline |
| SIGPIPE | Linux `MSG_NOSIGNAL`、macOS `SO_NOSIGPIPE` |
| 自有 Master/Slave 同错 | 独立 golden；真实 XCPlite；AsamV15 独立互操作 |
| A2L 默认 alignment 冒充显式声明 | DTO 保存 declared 状态；缺 Profile fail closed |
| thirdparty ABI 影响消费者 | B-T4 独立批准、ABI v9、兼容周期和迁移清单 |
| Dynamic DAQ 描述超过 Test Slave 命令面 | 补齐全部命令，否则从验收删除 |

---

## 12. 实施纪律与最终发布声明

每批实施必须：

1. 修改前读取实际接口和字段签名；
2. 新接口同步写中文 Doxygen 和测试；
3. 不修改未获批准的 thirdparty；
4. 完整构建前先清理 Release 目录；
5. 不运行 `install/Debug/bin` 下全部测试；
6. 对生产代码的每次修改生成对应实施记录；
7. 实施记录列出变更文件、规范/源码依据、测试命令、通过/失败数、平台和限制；
8. 若实现偏离本计划，先更新计划并说明理由；
9. `code-plan/V1.5规范实现与XCPlite兼容性评估.md` 中“XCPlite 从不填充”“0.5s 尝试吞 Tail”“一般命令无响应可视为兼容”等结论不得作为实现依据。

满足 B-T1～B-T5、真实 XCPlite Gate 和 AsamV15 独立互操作后，可声明：

> 支持 XCP on TCP/IPv4 Master 的显式 AsamV15 与 AsamV14 Wire Profile；支持 DNS 解析到 IPv4、TCP 流分片/粘包、PL 1.2～1.5 版本裁决、XCPlite PL1.2/TL1.4 互操作和显式断链；不支持 Profile 自动识别、IPv6、自动重连/RESUME、Optional TL Subcommand 和生产级 Slave。

若 AsamV15 独立互操作未完成，声明必须附加：

> AsamV15 已通过自有 loopback，尚未完成独立 V1.5 实现互操作验证；AsamV14 已通过仓库 XCPlite 真实进程互操作。

---

## 13. 实施收口：与计划的偏离及更正

按 §12 第 8 条隐含要求，实施期间与本计划不一致之处在此登记（计划正文保留原文，
以本节为权威更正）。

1. **§8.2 GET_VERSION 假设成立（本条曾被误判为"不成立"，已更正）**。原文写
   "GET_VERSION 预期 unavailable/`ERR_CMD_UNKNOWN`，随后依据显式或 A2L 证据解析为
   PL1.2/TL1.4"——**原文正确**。
   - 误判来源：B-T5 的互操作对手端 `tests/xcp_test_slave` 链接的是
     `thirdparty/XCPlite`（**V2.2.2**），它实现了 GET_VERSION，
     `ProbeVersion()` 返回 `GetVersionOutcome::Available`，于是被误当作 V1.2 的行为。
   - 真实 V1.2（PL `0x0102`）的 GET_VERSION 分支被
     `thirdparty/XCPliteV1.2/src/xcpLite.c:3274` 的
     `#if XCP_PROTOCOL_LAYER_VERSION >= 0x0104` 排除，命令码 `0xC0` 落入
     `default: error(CRC_CMD_UNKNOWN)`；用 `examples/xcp_slave_demo_v12` 实测确认
     主站收到 `ERR_CMD_UNKNOWN`（`outcome=1`），而会话仍正常建立
     （`conservative=1`，生效 PL=1.2，DISCONNECT 正常）。
   - 结论：`XcpliteTcpInteropTest.GetVersionOverTcp` 验证的是 **V2.2** 的 Available
     路径；V1.2 的 unavailable 降级路径由 `examples/xcp_slave_demo_v12` 联调覆盖。
2. **§8.2 第 7、8 条用故障注入替代真实进程**。"CRM/DTO 共用 CTR + 队列 lost
   只告警""多 DTO 单次 send / 任意 recv 分片"在真实 XCPlite 上无法确定性触发，
   改由 `tests/tcp_test_slave.cpp` 的注入点覆盖，并用源码复核
   `thirdparty/XCPliteV1.2/src/xcpEthTl.c:651` 的 `gXcpTl.Ctr += (uint16_t)lost;`
   作为"丢帧时对端主动推进 CTR"的依据。
3. **§8.2 第 9 条"bounded backoff 后重连"落为本库不自动重连**。库维持
   "绝不静默重连"不变量（B-T2 既有设计），Gate 改为验证"DISCONNECT 后同端口
   可显式重建链"，重连节奏由应用层负责。
4. **§8.3 未执行**。本期无独立 AsamV15 对端，按 §8.3 末段允许收尾；发布声明
   已在 `README.md` 公开头文件章节与 B-T5 实施记录 §4 落地。
5. **§5 计划中 `thirdparty/XCPliteV1.2` 为未跟踪参考副本**，未修改其任何文件；
   B-T4 获准修改的第三方库为 `thirdparty/a2l-sdk` 与 `thirdparty/a2llib`。
6. **§7.3 多实例 fail closed 首版未落实，已补漏收口**。B-T5 首版只做"察觉 + 提示"，
   与本节要求不符；补漏批次改为按 §7.3 拒绝，并加"配置去重后全同则允许"的放宽
   （同类端点重复声明不阻断）。同批删除 SDK 中两个从未赋值的派生字段
   `endpoint_profile_hint`/`_declared`，并把"`plus_conflicts` 只报数值差异"的契约
   写死在 `DiffIfData()`。记录见
   [`code-plan/B-T5补漏_多实例fail-closed与SDK死字段清理.md`](B-T5补漏_多实例fail-closed与SDK死字段清理.md)。
