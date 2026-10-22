# XCP on TCP/IP 传输——实施计划（批次 B-T1～B-T5）

日期：2026-10-22　　状态：**修订完成，待实施批准**（尚未修改生产代码）

规范基线：

- `docs/ASAM-AE-MCD-1-XCP-AS-Ethernet-Transport-Layer-V1-5-0.pdf`：唯一规范真值，XCP on Ethernet Transport Layer V1.5；
- `docs/XCP_-Part_3-_Transport_Layer_Specification_XCP_on_Ethernet_(TCP_IP_and_UDP_IP)_-1.0.pdf`：历史 Version 1.0，仅用于交叉核对，不覆盖 V1.5；
- A2L `XCP_ON_TCP_IP` 语义以当前 V1.5 AML、上游 `a2llib` 数据模型和本项目桥接结果共同校验。

已确认约束：

1. 保持 B-T1～B-T5 五批结构；
2. Frame codec 泛化后由 UDP/TCP 共享；
3. 本期包含 Master TCP 客户端、测试 TCP Slave 和 CLI；
4. 不做自动重连或 RESUME；
5. 生产实现限定 IPv4 单播，但远端允许 IPv4 literal 或可解析为 IPv4 的 DNS hostname；IPv6 返回明确的 `UnsupportedFeature`；
6. 每批代码变更同步增加测试和实施记录，不把接口测试统一推迟到后续批次。

---

## 0. 总体目标、范围和完成定义

当前生产库只有 `UdpTransport`。本计划新增 `TcpTransport : IXcpTransport`，实现 XCP Master 通过 TCP/IP 与 Slave 通信。

生产侧只实现 TCP 客户端：Slave 是 listener，Master 主动建链。测试目录提供 loopback TCP Slave，作为协议和故障注入对手件，不进入生产库。

### 0.1 完成定义

只有同时满足以下条件才可声明本期完成：

- Frame 的 LEN/CTR、字节序、PACKET_ALIGNMENT 和 Tail 符合 Ethernet TL V1.5；
- TCP byte stream 下分片、粘包、可选末尾 Tail、FIN/RST 均有明确状态机和测试；
- Open/Send/Close/ReceiveLoop 在线程安全和对象生命周期上无句柄复用、自连接 join、重复回调风险；
- TCP CTR 使用严格连续策略，不复制 UDP 丢包容错语义；
- CLI 的命令行端点与 A2L TCP 端点优先级可执行且有测试；
- Release 清理构建和相关全量测试通过；
- 至少完成一次与独立 XCP-on-TCP 实现的互操作冒烟；若客观条件不足，必须在实施记录中明确标为未验证，不能用自有 Master/Slave 闭环代替。

### 0.2 明确不做

- 生产级 TCP Slave/listener；
- 自动重连、RESUME 和断线会话恢复；
- 多 Frame 主动聚合成一次 write；
- Optional TL Subcommand：`GET_SLAVE_ID`、`GET_SLAVE_ID_EXTENDED`、`SET_SLAVE_IP_ADDRESS`、`GET_DAQ_CLOCK_MULTICAST`；
- IPv6 连接；
- TLS；
- 多 Master 或连接池。

“不做”项必须体现在能力矩阵和错误诊断中，不得静默降级。

---

## 1. 规范不变量和项目 Profile

### 1.1 Frame 格式

- Header 固定 4 字节：`LEN` WORD + `CTR` WORD；均为 little-endian；
- `LEN` 是 XCP Packet 长度，不含 Header 和 Tail；合法协议域为 `1..0xFFFF`；空 XCP Packet 在本库拒绝；
- 通用 Ethernet codec 的 XCP Packet 上限为 `65535`；
- UDP 仍受 IPv4 UDP payload 限制：`65507 - 4 = 65503`；这个限制只存在于 UDP wrapper/config，禁止再命名为通用 Ethernet 上限；
- `PACKET_ALIGNMENT` 为 8/16/32 bit；Tail 不计入 LEN；
- TCP 是 byte stream，没有 write/read 边界语义。

常量分层：

```cpp
constexpr std::size_t kEthernetHeaderSize = 4U;
constexpr std::size_t kEthernetMaxXcpPacket = 0xFFFFU;
constexpr std::size_t kEthernetMaxAlignmentBytes = 4U;
constexpr std::size_t kEthernetMaxFrameSize = 65540U;

// 仅 UDP：保持原有限制和兼容符号。
constexpr std::size_t kUdpMaxDatagramSize = 65507U;
constexpr std::size_t kUdpMaxXcpPacket = 65503U;
```

`kEthernetMaxFrameSize` 是 `4 + LEN + Tail` 在 LEN 域和 32-bit alignment 下的最大值。

### 1.2 TCP Tail 接收契约

规范允许单个 message 的尾部 Tail 省略，但后继 XCP Packet 的起点必须满足 alignment。为避免 byte stream 中“最后一帧是否还会有后继”的歧义，接收器采用显式有状态解析：

1. `NeedHeader`：累计 4 字节 Header；
2. `NeedPayload`：按 LEN 累计 Payload；Payload 完整后立即且仅投递一次，不等待可选末尾 Tail；
3. `NeedInterFramePadding(N)`：若 N=0，立即回到 `NeedHeader`；若 N>0：
   - 连接继续且后续数据到达时，先消费完整 N 字节 Tail，再解析下一 Header；
   - EOF 在尚未收到任何 Tail 字节时到达，视为合法省略整个末帧 Tail；若已收到完整 N 字节，则按正常状态结束；
   - EOF 时只有 `1..N-1` 个 Tail 字节，或 Header/Payload 不完整，均视为 truncated frame，告警并关闭；
4. Tail 内容不解释，但 Tail 长度超过 N、LEN 越界或状态机不可能转移均视为流失步并关闭。

发送器始终补足 Tail，作为本库的确定性发送策略；这不把“规范允许末帧省略 Tail”改成接收端强制要求。

Alignment origin 定义为当前 TCP 连接上首个 Ethernet Header 的 byte offset 0；之后每个 Header 起点相对该 origin 对齐。

### 1.3 CTR 契约

- 发送和接收 CTR 相互独立；
- 每次成功 Open 后发送 CTR 从 0 开始；每生成一个新 Frame 后 `+1 mod 65536`；
- 接收首个合法 Frame 建立基线，之后只接受 `current == previous + 1 mod 65536`；
- duplicate、forward jump、backward 均是 peer protocol violation，不解释为 TCP 网络丢包；
- 本期策略：发出 `OnTransportWarning` 后关闭连接，不继续在可能失步的流上工作；
- 重开连接时清除接收基线并重置发送 CTR；
- UDP 原有半域容错策略不变，TCP 禁止复用。

### 1.4 地址族和名称解析

- `remote_host` 允许 IPv4 literal 或 DNS hostname；
- 使用 `getaddrinfo(AF_INET, SOCK_STREAM)`，按返回顺序尝试 IPv4 地址；解析完成后的所有 connect 尝试共享一个 monotonic deadline，不能每个地址重新获得完整超时；同步 `getaddrinfo` 的系统 DNS 耗时不纳入可移植的 `connect_timeout_ms` 保证，须在 API 注释中明示；
- IPv6 literal、A2L IPv6-only endpoint 和显式本地 IPv6 bind 返回 `UnsupportedFeature`，消息明确说明“当前 Profile 仅支持 IPv4”；
- `local_host` 只允许 IPv4 literal 或 `0.0.0.0`，避免本地绑定 DNS 的不确定性；
- 显式 bind 失败是致命 `TransportError`，不得告警后让 OS 偷换源地址/端口；仅 `0.0.0.0:0` 表示允许 OS 自选。

---

## 2. 文件布局与公共接口

| 文件 | 动作 | 说明 |
|---|---|---|
| `include/libxcp/ethernet_header_codec.hpp` / `src/ethernet_header_codec.cpp` | 新增 | 通用 LEN/CTR、Tail 计算和完整 Frame 编码；不承载 TCP 流状态 |
| `include/libxcp/udp_header_codec.hpp` / `src/udp_header_codec.cpp` | 修改 | 兼容转发层；保留旧类型、函数和 UDP 上限 |
| `include/libxcp/tcp_stream_decoder.hpp` / `src/tcp_stream_decoder.cpp` | 新增 | TCP 有状态流解码器，实现 §1.2 |
| `include/libxcp/tcp_transport_config.hpp` | 新增 | TCP 配置及校验 |
| `include/libxcp/tcp_transport.hpp` / `src/tcp_transport.cpp` | 新增 | Master TCP 客户端 |
| `src/socket_util.hpp` / `src/socket_util.cpp` | 新增 | internal 跨平台 socket 工具；不进入公共 include |
| `CMakeLists.txt` | 修改 | 新生产源文件入列 |
| `tests/ethernet_header_codec_test.cpp` | 新增 | 通用 codec 边界和 UDP 兼容测试 |
| `tests/tcp_stream_decoder_test.cpp` | 新增 | 状态机分片、粘包、Tail、EOF 测试 |
| `tests/tcp_transport_test.cpp` | 新增 | B-T2 同批的接口、并发和 socket 行为测试 |
| `tests/tcp_test_slave.hpp/.cpp` | 新增 | B-T3 loopback TCP Slave 和故障注入 |
| `tests/xcp_tcp_loopback_test.cpp` | 新增 | XcpMaster 集成测试 |
| `tests/CMakeLists.txt` | 修改 | 新测试入列 |
| `tools/xcp_master/main.cpp` | 修改 | `--transport udp|tcp`、端点合并、alignment 选择 |
| `thirdparty/a2l-sdk/**` | B-T4 单独批准后修改 | TCP endpoint 导出和桥接；不改 `thirdparty/a2llib` |
| `src/xcp_master.cpp` | 修改文案/能力边界 | 去除“本库只承诺 IPv4 UDP 单播”的过期表述，改为 IPv4 UDP/TCP Profile |
| `docs/modules/传输层.md`、`README.md` | 修改 | 文档和能力矩阵同步 |
| `code-plan/TCP传输_实施记录_B-Tn_*.md` | 每批新增 | 记录变更、测试命令、结果及未验证项 |

分层：

```text
XcpCore -> IXcpTransport -> UdpTransport / TcpTransport
                              |             |
                              +-- Ethernet Frame Codec
                                            |
                                      TcpStreamDecoder
```

`SendCtr()`、`LastReceiveCtr()`不是 `IXcpTransport` 虚接口。若保留观测能力，它们是 `TcpTransport` 与 `UdpTransport` 各自的非虚扩展 API，计划、注释和测试不得称为基类接口。

---

## 3. TcpTransportConfig

建议接口：

```cpp
namespace calmcar::xcp {

/// @brief XCP on TCP/IPv4 Master 客户端配置。
struct TcpTransportConfig {
    std::string remote_host;                    ///< IPv4 literal 或 DNS hostname。
    std::uint16_t remote_port{0};               ///< Slave 监听端口，1..65535。
    std::string local_host{"0.0.0.0"};          ///< IPv4 本地绑定地址。
    std::uint16_t local_port{0};                ///< 0 表示由 OS 分配。
    std::uint32_t connect_timeout_ms{3000};     ///< DNS 解析后全部 connect 尝试共享的超时。
    std::uint32_t io_poll_interval_ms{100};     ///< 接收及可取消发送的轮询上界。
    std::uint32_t send_timeout_ms{3000};        ///< 完整 Frame 写出总超时。
    std::size_t max_frame_packet_size{kEthernetMaxXcpPacket};
    PacketAlignment packet_alignment{PacketAlignment::Unknown};
};

}  // namespace calmcar::xcp
```

校验：

- `remote_host` 非空；
- `remote_port != 0`；
- `local_host` 是 IPv4 literal；
- `connect_timeout_ms/io_poll_interval_ms/send_timeout_ms > 0`，并检查转换到平台 API 类型时不溢出；
- `1 <= max_frame_packet_size <= 65535`；
- `packet_alignment` 是 Bit8/Bit16/Bit32，Unknown 拒绝 Open；
- IPv6 literal 返回 `UnsupportedFeature`，其他语法错误返回 `InvalidArgument`。

新 TCP API 无兼容负担，因此默认 Unknown，禁止静默猜测 Bit8。CLI/A2L 层必须明确提供来源；来源规则见 §8。

---

## 4. 批次 B-T1：Frame codec 与 TCP Stream Decoder

### 4.1 通用 Frame codec

新增：

- `EthernetHeader { std::uint16_t len; DatagramCtr ctr; }`；
- `EthernetFrame { Bytes data; }`；
- `EncodeEthernetFrameAligned(BytesView, DatagramCtr, PacketAlignment)`；
- Header decode、Tail 长度和 Frame 总长度的 checked helper；
- 所有长度运算先做 overflow-safe 检查；
- Packet 超过 65535、空 Packet、Unknown alignment 抛 `InvalidArgument`。

`udp_header_codec` 保留既有 API，通过别名/薄转发调用通用 codec；UDP wrapper 在调用通用编码前继续执行 `<=65503` 限制。禁止保留两份 LEN/CTR/Tail 算法。

### 4.2 TcpStreamDecoder

状态由对象内部持有，不用“每次对整个 vector 重做无状态解析”的方式猜测 Tail。建议接口表达如下，最终签名前必须读取现有 Bytes/BytesView 约定再落地：

```cpp
class TcpStreamDecoder {
public:
    explicit TcpStreamDecoder(PacketAlignment alignment,
                              std::size_t max_packet_size);
    DecodeResult Push(BytesView bytes);
    DecodeResult Finish();  // 收到 FIN 时校验剩余 Header/Payload/Tail 状态。
    void Reset();
};
```

`DecodeResult` 可返回零到多个拥有稳定存储的 Packet；禁止返回在内部 buffer erase 后悬空的 `BytesView`。Transport 在监听器回调完成前必须保证 payload 生命周期有效。

### 4.3 B-T1 测试 Gate

同批新增测试，至少覆盖：

- LEN/CTR little-endian golden vector；
- LEN=1、65503、65504、65535；LEN=0 和 65536 拒绝；
- Bit8/16/32 的 0/1/2/3 字节 Tail；
- 65535 长度 + 32-bit alignment，Frame 总长和缓冲不越界；
- 对 Header、Payload、Tail 的每一个 byte boundary 做逐字节分片；
- 2/3 帧粘包；一次 Push 同时结束旧帧并包含下一帧 Header；
- Payload 完整即投递一次，后续 Tail 不得重复投递；
- 完整末帧后 FIN：Tail 完全省略或完整 N 字节时合法，`1..N-1` 字节部分 Tail 失败；半 Header/半 Payload 后 FIN 失败；
- UDP 旧 API 行为和 `udp_*` 测试保持一致；
- ASan/UBSan 可用时运行 decoder 边界用例。

B-T1 验收：Release 清理构建通过，codec/decoder 新测试和全部 UDP 回归通过，生成实施记录。

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

监听器回调必须使用真实接口名：

```cpp
listener->OnPacketReceived(packet);
listener->OnTransportWarning(message);
listener->OnTransportClosed(reason);
```

不得出现不存在的 `OnXcpPacket()`。

### 5.2 Socket 模式与 Open

连接 socket 在整个生命周期保持 non-blocking，避免 Send 无限阻塞：

1. `getaddrinfo(AF_INET, SOCK_STREAM)` 解析远端；
2. 依次尝试 IPv4 地址，使用同一个 monotonic connect deadline；
3. 创建 socket；显式 local bind 失败立即关闭并抛错；
4. `connect` 返回进行中时用 `select/poll` 等待 writable，再以 `SO_ERROR` 判定；处理 EINTR 并用剩余 deadline 重试；
5. 设置 `TCP_NODELAY`、`SO_KEEPALIVE`；Linux 使用 `MSG_NOSIGNAL`，macOS 设置 `SO_NOSIGPIPE`；
6. 初始化 decoder、发送 CTR=0、清接收基线；
7. 状态转换为 Open 后启动 ReceiveLoop；
8. 任一步失败都回收句柄、清 listener、恢复 Closed，不发 `OnTransportClosed`。

不能用公网或不可路由地址作为确定性 timeout 测试；测试通过 socket abstraction 注入“持续 EINPROGRESS/超时”。

### 5.3 生命周期状态机和锁序

状态：`Closed -> Opening -> Open -> Closing -> Closed`。

约束：

- Open/Close 由 lifecycle mutex 串行；重复 Open 抛 `InvalidState`；Close 幂等；
- Send 可从任意业务线程调用，使用 send mutex 保证 Frame 顺序；
- Close 先原子进入 Closing，并对稳定 socket 句柄调用 shutdown，使正在进行的 send/recv 退出；禁止先等待 send mutex 再 shutdown；
- shutdown 后等待 send 路径退出，再由单一所有者 close 句柄，防止句柄复用；
- ReceiveLoop 是 `OnTransportClosed` 的唯一发出者；主动 Close、peer FIN、RST、协议错误均恰好回调一次；Close 本身不得再次回调；
- `OnPacketReceived`、warning、closed 回调均不持 send/CTR/lifecycle mutex；
- CTR 锁内只更新状态和形成诊断，释放锁后再调用监听器；
- `Close()` 不标 `noexcept`；若从 ReceiveLoop 的监听器回调线程直接调用 Close，返回 `InvalidState`，避免 self-join。公开 Doxygen 必须说明监听器应把 Close 调度到其他线程；
- Close 返回后不再有任何监听器回调，并清除 listener 指针；
- 析构从非接收线程调用 Close；对象销毁前上层必须停止回调线程。

### 5.4 Send

1. 检查 Open、packet 非空及长度上限；
2. 在 send mutex 内取得 CTR 并编码完整 Frame；
3. 用 non-blocking send + writable wait 循环写完，统一受 `send_timeout_ms` monotonic deadline 控制；
4. 处理 partial write、EINTR、EAGAIN/EWOULDBLOCK；
5. Close 导致的 shutdown 必须可取消发送；
6. timeout、EPIPE、RST 等进入统一断链流程并抛 `TransportError`；
7. CTR 只在 Frame 已进入发送事务时消费，失败不复用旧值；
8. 日志只记录长度和 CTR，不记录载荷。

### 5.5 ReceiveLoop

- 使用 readable wait + `io_poll_interval_ms` 检查关闭状态；
- `recv>0` 输入 `TcpStreamDecoder`；
- 每个完整 Packet 先进行严格 CTR 校验，再在锁外调用 `OnPacketReceived`；
- `recv==0` 调用 decoder `Finish()`：允许仅缺可选末尾 Tail，不允许半 Header/半 Payload；
- RST/socket error、decoder malformed、CTR discontinuity 均进入统一断链收尾；
- Packet callback 抛出的异常不得逃逸工作线程；记录 warning 后按现有 Transport 监听器策略继续或关闭，选择必须在实现记录中与 UDP 对齐；
- 缓冲上限按 `kEthernetMaxFrameSize` 和 decoder 状态计算，不使用 UDP 65507 常量。

### 5.6 B-T2 测试 Gate

B-T2 同批至少增加：

- 配置校验：DNS、IPv4、IPv6 Unsupported、Unknown alignment、所有 timeout=0、长度边界；
- Open success/refused/timeout、getaddrinfo 失败、多 IPv4 candidate 共用 deadline；
- 显式 bind 端口占用失败；成功后用 `getsockname` 验证源端点；
- partial send、EINTR、EAGAIN、send timeout、peer RST、Close 取消 send；
- Send 与 Close 并发、Open 与 Close 串行、重复 Close、重开后 CTR/decoder 状态清零；
- 主动 Close、peer FIN、RST、协议错误的 `OnTransportClosed` 恰好一次且在线程契约内；
- Close 返回后无 Packet callback；
- listener 回调中读取 `LastReceiveCtr()` 不死锁；回调线程直接 Close 得到稳定 `InvalidState`；
- Linux/macOS 不产生 SIGPIPE；
- 合法 LEN=65535 Frame 完整收发。

为使 partial write/EINTR/timeout 测试确定且跨平台，`socket_util` 提供 internal 可注入 `SocketOps`/函数表，仅供测试替换系统调用；不进入公共 API。

B-T2 验收：Release 清理构建通过，B-T1/B-T2 测试和 UDP 回归通过，生成实施记录。

---

## 6. 批次 B-T3：测试 Slave 与 XcpMaster 集成

### 6.1 TCP Test Slave

- 绑定 `127.0.0.1:0` 并通过 `getsockname` 暴露端口；
- accept 一条活动连接；第二连接测试必须由 Slave accept 后立即显式拒绝/关闭，不能依赖 listen backlog“不再 accept”；
- 使用通用 Frame codec 和独立的测试侧流重组逻辑；关键 golden vector 不能全部复用生产 decoder，否则自实现同错无法暴露；
- 支持最小 XCP 会话：CONNECT、DISCONNECT、GET_STATUS、GET_COMM_MODE_INFO、SET_MTA、UPLOAD、SHORT_UPLOAD、SYNCH；
- 为 Dynamic DAQ 测试补齐实际需要的 FREE_DAQ、ALLOC_DAQ、ALLOC_ODT、ALLOC_ODT_ENTRY、SET_DAQ_PTR、WRITE_DAQ、SET_DAQ_LIST_MODE、START_STOP_DAQ_LIST、START_STOP_SYNCH 以及 DTO 推送；如果不补齐，则从验收中删除“Dynamic DAQ”，不能用描述代替实现；
- Stop 使用 shutdown + join，测试退出不遗留线程。

故障注入：

- Header/Payload/Tail 任意边界分片；
- 多帧单 write；
- 末帧无 Tail、完整 Tail，以及必须被拒绝的部分 Tail；
- 半 Header/半 Payload 后 FIN；
- 完整帧后 FIN；
- RST；
- duplicate/jump/backward CTR；
- corrupt LEN；
- 延迟读取，触发 Master partial send/send timeout；
- 第 N 个响应后断链。

### 6.2 集成场景

1. `XcpMaster(std::unique_ptr<TcpTransport>)` Connect；
2. 验证 GET_VERSION 可用和不可用两种路径；
3. Polling 测量：SET_MTA + UPLOAD/SHORT_UPLOAD；
4. 标定写入并回读；
5. 若 Test Slave 命令面已补齐，则执行 Dynamic DAQ 配置和 DTO 接收；
6. `XcpMaster::Disconnect()` 必须先发送 XCP DISCONNECT，再关闭 TCP；Slave 验证命令顺序；
7. Peer FIN/RST/CTR violation 后，当前命令失败且会话不可复用；
8. 新建连接后状态重新初始化；不自动恢复旧会话。

### 6.3 B-T3 Gate

- 所有 `Tcp*` 单元和集成测试通过；
- 全量 `libxcp_tests` 通过；
- 测试不得依赖公网、固定端口或不确定 backlog 行为；
- 自有 Test Slave 闭环只证明内部一致性；实施记录必须单列独立实现互操作状态。

---

## 7. 批次 B-T4：A2L SDK 与桥接

> 本批修改 `thirdparty/a2l-sdk`，实施前必须获得用户对 thirdparty 修改的单独批准。若不批准，跳过本批，CLI TCP 仅接受命令行端点，并在能力矩阵标记“A2L TCP endpoint 自动提取未支持”。

### 7.1 数据语义

上游 `a2llib` 已独立保存 TCP 的：

- `HOST_NAME`；
- `ADDRESS`；
- `IPV6`；
- `PORT`；
- `PACKET_ALIGNMENT`；
- Optional TL Subcommand 列表。

SDK/bridge 不得把 TCP 数据塞入 `udp_*` 字段，也不得只保留一个字符串而丢失 hostname/address/IPv6 的来源。

### 7.2 兼容方案

采用“追加 selected endpoint 字段，保留旧 UDP 字段一个兼容周期”的方案：

```text
endpoint_port
endpoint_host_name
endpoint_ipv4_address
endpoint_ipv6_address
endpoint_packet_alignment
endpoint_alignment_declared
endpoint_sub_commands
endpoint_instance_count
```

- 当选中 UdpIp 时，同时填充旧 `udp_*` 和新 `endpoint_*`；
- 当选中 TcpIp 时只填 `endpoint_*`；
- 旧 `udp_*` 标为 deprecated，但本期不删除，避免无必要的源码破坏；
- 追加字段改变 DTO 大小，按项目 ABI 门禁将 v8 bump 到 v9；理由是结构布局/大小变化，而不是字段改名；
- 导出、DiffIfData、桥接、golden 和 ABI gate 全部纳入变更清单。

桥接 `TransportEndpoint` 增加 hostname、IPv4、IPv6、alignment 声明状态和实例数量。`remote_host` 可作为最终选择后的兼容展示字段，但不得成为唯一原始证据。

### 7.3 Endpoint 选择政策

- CLI 显式 host+port 优先，完全覆盖 A2L 地址；
- 未显式指定时，A2L TCP endpoint 优先使用 `ADDRESS`，没有 ADDRESS 再使用 `HOST_NAME`；
- 只有 IPV6 时返回明确 Unsupported；
- ADDRESS 与 HOST_NAME 都存在时不做 DNS 失败后的隐式跨来源重试，避免连接对象与诊断来源不确定；选择来源写入日志；
- TCP 实例数大于 1 且 CLI 未覆盖时返回 ambiguous error，不静默 `front()`；
- Optional TL Subcommand 只透出元数据，本期不执行。

### 7.4 Alignment 来源

DTO 必须区分：

- A2L 明确声明 Bit8/16/32；
- A2L 未声明。

未声明时不能被上游枚举默认值悄悄伪装成“明确 Bit8”。CLI 层按 §8 要求显式 `--packet-alignment`；本期不设隐式默认。

### 7.5 B-T4 测试 Gate

新增或更新：

- TCP-only A2L：HOST_NAME、ADDRESS、IPV6、PORT、16-bit alignment；
- ADDRESS+HOST_NAME 的固定优先级；
- hostname-only；
- IPv6-only Unsupported；
- alignment 缺失与明确 Bit8 可区分；
- 多 TCP 实例 ambiguous；
- UDP-only golden 完全回归；
- plain/plus 传输类型冲突；
- SDK ABI v9 gate、DiffIfData 文本和 bridge 映射；
- 全仓搜索 DTO 字段引用，实施记录列出迁移清单，不能用“仅三处”作为未经验证的前提。

---

## 8. 批次 B-T5：CLI、能力文案与互操作

### 8.1 CLI 选项

新增：

```text
--transport udp|tcp
--packet-alignment 8|16|32
```

保留既有 `--host`、`--port`，但修正 TCP 校验流程：

1. `--host` 与 `--port` 必须成对出现；只出现一个立即报错；
2. 两者都出现时使用 CLI endpoint；
3. 两者都未出现时，在完成 A2L 加载后选择 `Kind::TcpIp` endpoint；
4. 两个来源都没有时才报“缺少 TCP endpoint”；
5. 禁止用 UDP endpoint 填充 TCP；
6. alignment 优先级：CLI 显式值 > 对应 TCP A2L 明确声明值；两者都无则报错；
7. endpoint 和 alignment 必须来自同一 transport kind；
8. UDP 默认行为保持兼容，不能因 TCP 接入改变现有 CLI 用法。

当前 CLI 在解析阶段强制 `--host --port`，B-T5 必须删除这项过早校验，把合并校验移动到 A2L 加载之后；否则 A2L endpoint 永远不可达。

### 8.2 能力和错误文案

- 更新 `XcpMaster` 中“只承诺 IPv4 UDP 单播”的静态提示，改为“IPv4 UDP/TCP”；
- README、传输层文档和能力矩阵明确：TCP IPv4、DNS→IPv4、无 IPv6、无自动重连、无 Optional TL Subcommand；
- 所有 Unsupported/InvalidArgument/TransportError 给出可操作信息；
- 不新增 TCP 示例程序；CLI 作为手工入口，测试 Slave 作为自动化入口。

### 8.3 互操作 Gate

至少选择一个不复用本库 codec/decoder 的独立对端：XCPlite TCP、商业 ECU、CANape 可连接模拟器或其他已知实现。验证：

- Connect/Disconnect；
- GET_STATUS 或 Polling Upload；
- 8/16/32 alignment 中对端实际支持的组合；
- 多个连续响应的 CTR；
- 对端主动关闭；
- DNS hostname（若对端环境可配置）。

无法获得独立对端时，本批仍可结束，但发布声明必须写“TCP 自有 loopback 已验证，独立实现互操作未验证”，不得宣称完整合规。

---

## 9. 批次与验收汇总

| 批次 | 内容 | 必须通过的 Gate | 实施记录 |
|---|---|---|---|
| **B-T1** | 通用 codec、UDP 兼容层、TcpStreamDecoder | Release 清理构建；codec/decoder 边界测试；UDP 全回归 | `code-plan/TCP传输_实施记录_B-T1_codec与流解码.md` |
| **B-T2** | socket_util、Config、TcpTransport、同批接口测试 | 生命周期/并发/partial send/FIN-RST/CTR/DNS/bind 测试；UDP 回归 | `code-plan/TCP传输_实施记录_B-T2_TcpTransport.md` |
| **B-T3** | Test Slave、XcpMaster loopback | XCP Disconnect 顺序、Polling/标定、可选 Dynamic DAQ、断链测试；全量测试 | `code-plan/TCP传输_实施记录_B-T3_集成验证.md` |
| **B-T4** | A2L SDK v9、endpoint DTO、bridge、golden | thirdparty 单独批准；ABI/golden/smoke/bridge 全绿 | `code-plan/TCP传输_实施记录_B-T4_A2L端点.md` |
| **B-T5** | CLI、能力文案、文档、独立互操作 | CLI 优先级矩阵；文档评审；互操作结果或明确未验证声明 | `code-plan/TCP传输_实施记录_B-T5_CLI与发布.md` |

依赖顺序：

```text
B-T1 -> B-T2 -> B-T3
                  |
B-T4 -------------+-> B-T5
```

B-T4 可在 B-T2 后并行准备，但合并时必须先解决 `tests/CMakeLists.txt`、DTO ABI 和 CLI 数据流交叉修改。

---

## 10. 完整测试矩阵

| 维度 | 必测值 |
|---|---|
| LEN | 1、65503、65504、65535、0、越界 |
| Alignment | 8、16、32、Unknown |
| Stream | Header/Payload/Tail 每字节分片、2/3 帧粘包、尾 Tail 省略/完整/部分非法、半帧 FIN |
| CTR | 首帧、严格 +1、FFFF→0、duplicate、jump、backward、双向独立、重开归零 |
| Socket | success、refused、timeout、DNS failure、多地址、bind failure、partial send、EINTR/EAGAIN、RST、FIN |
| 生命周期 | 重复 Open/Close、Send/Close 并发、主动/被动关闭竞态、回调恰一次、Close 后零回调 |
| 平台 | Windows/Linux/macOS；SIGPIPE 抑制；socket 错误映射 |
| A2L | ADDRESS、HOST_NAME、IPV6-only、缺 alignment、多实例、UDP/TCP 冲突 |
| CLI | CLI 全覆盖、A2L fallback、host/port 缺一项、transport 不匹配、alignment 优先级 |
| XCP | CONNECT、DISCONNECT、Polling、标定写回读、可选 Dynamic DAQ、peer close |

测试必须确定性运行：不依赖公网黑洞地址、不依赖 listen backlog、不以“大 payload 大概率 partial write”为断言依据。

---

## 11. 风险登记

| 风险 | 缓解措施 |
|---|---|
| TCP 最后一帧 Tail 可选造成解析歧义 | 有状态 decoder：Payload 完整立即投递；Padding 与 EOF 独立处理 |
| UDP/TCP 共用 codec 导致 UDP 行为回归 | 保留 UDP wrapper 和上限；B-T1 同批跑全部 UDP 回归 |
| Send/Close 句柄竞态 | shutdown-first、稳定句柄、明确锁序、唯一 close 所有者、并发测试 |
| 回调触发 Close 导致 self-join | Close 非 noexcept；接收线程调用返回 InvalidState；Doxygen 和测试固定行为 |
| DNS 多地址导致总超时倍增 | 所有 candidate 共享 monotonic deadline |
| 显式 bind 被静默忽略 | bind 失败 fatal，并用 getsockname 验收 |
| SIGPIPE 导致进程退出 | Linux `MSG_NOSIGNAL`、macOS `SO_NOSIGPIPE`、平台测试 |
| 自有 Master/Slave 同错 | decoder golden vector独立构造；B-T5 增独立实现互操作 |
| A2L 未声明与明确 Bit8 混淆 | DTO 增 `endpoint_alignment_declared`，CLI 缺证据时报错 |
| thirdparty ABI 影响外部消费者 | B-T4 独立批准、ABI v9、保留旧 UDP 字段一个兼容周期、迁移清单 |
| Dynamic DAQ 测试描述超过 Slave 命令面 | 要么补齐全部 DAQ 命令，要么从本期验收明确删除，不允许伪验收 |
| 半帧长期滞留 | 命令层 timeout 可触发外部 Close；Transport 依靠 Close/shutdown 取消。若未来需要连接级 idle timeout，另立需求，不伪称缓冲上限能解决 |

---

## 12. 实施纪律与最终发布声明

每批实施必须：

1. 修改前读取实际接口和字段签名；
2. 新接口同步写 Doxygen 中文注释和测试；
3. 不修改未获批准的 thirdparty；
4. 完整构建前先清理 Release 目录；
5. 不运行 install/Debug/bin 下全部测试；
6. 在对应实施记录中列出：变更文件、规范依据、测试命令、通过/失败数、平台、已知限制；
7. 若实现与本计划发生偏离，先更新计划并说明理由，不能让文档追认代码。

满足 B-T1～B-T5 且完成独立互操作后，可声明：

> 支持 XCP on TCP/IPv4 Master 基础 Profile，支持 DNS 解析到 IPv4、8/16/32-bit Packet Alignment、标准 LEN/CTR Frame、TCP 流分片/粘包和显式断链；不支持 IPv6、自动重连/RESUME、Optional TL Subcommand 和生产级 Slave。

若独立互操作未完成，声明必须附加：

> 已通过自有 loopback 测试，尚未完成独立 XCP-on-TCP 实现互操作验证。
