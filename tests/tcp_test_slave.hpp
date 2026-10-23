/**
 * @file tcp_test_slave.hpp
 * @brief Loopback TCP 测试 Slave：真实 Socket，模拟最小 XCP Session
 * 并支持双 Wire Profile 与故障注入。
 *
 * 依据 code-plan/TCP传输_实施计划_B-T1至B-T5.md 第 6.1 节实现（B-T3）。
 *
 * @par 这是测试程序而非生产 Slave SDK
 *   与 `udp_test_slave.hpp` 同属测试设施（计划 §2.3）。
 *
 * @par 为什么两套 Profile 各自独立实现 framing（裁决 m01093 取 S1）
 *   计划 §6.1 明确要求"关键 golden 不全部复用生产 decoder"。若本 Slave 直接
 *   调用 `TcpStreamDecoder` 编解码，那么"Master 与 Slave 互通"只能证明本库
 *   **自洽**（同一个 bug 在两侧对称出现时测试照样绿），不能证明线格式正确。
 *   因此：
 *   - `AsamV14` 模式的 framing **独立实现**，且其字节布局严格复刻
 *     `thirdparty/XCPliteV1.2/src/xcpEthTl.c` 的收发热路径（见下）；
 *   - `AsamV15` 模式的 framing **独立实现**，按 ASAM TL V1.5 的 LEN 外 Tail 语义。
 *
 * @par XCPlite AsamV14 语义（源级取证，m01092）
 *   发送（`xcpEthTl.c:186-197`）：
 *   ```c
 *   tXcpCtoMessage p;        // #pragma pack(1): { uint16_t dlc; uint16_t ctr; uint8_t packet[]; }
 *   p.dlc = size;            // size = XCP Packet 语义长度
 *   p.ctr = gXcpTl.Ctr++;
 *   memcpy(p.packet, data, size);
 *   XcpEthTlSend(&p, size + XCPTL_TRANSPORT_LAYER_HEADER_SIZE, ...);
 *   ```
 *   接收（`:332-335`）：先读 4 字节头
 *   （`XCPTL_TRANSPORT_LAYER_HEADER_SIZE 4`，`xcptl_cfg.h:50`），
 *   再按 `msgBuf.dlc` **精确**读取，**不做**对齐跳过。
 *
 *   ⇒ 帧格式为 `[LEN=真实Packet长(u16le)][CTR(u16le)][Packet[LEN]]`，**无 LEN 外 Tail**。
 *   `XCPTL_PACKET_ALIGNMENT 4`（`xcptl_cfg.h:46`）在 TCP 路径上从未被引用。
 *
 * @par AsamV15 的验证性质（必须如实标注）
 *   V15 的 LEN 外 Tail 行为**没有第三方实现可对照**。因此 V15 的集成用例属于
 *   **自有闭环验证**，不得宣称"已与独立实现互操作"。与之相对，V14 的 golden
 *   样本可声称与 XCPlite 语义一致（逐字节对齐其收发路径）。
 */

#ifndef CALMCAR_XCP_TEST_TCP_TEST_SLAVE_HPP_
#define CALMCAR_XCP_TEST_TCP_TEST_SLAVE_HPP_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>

#include "libxcp/protocol_types.hpp"
#include "libxcp/tcp_wire_profile.hpp"

namespace calmcar::xcp::test {

/**
 * @brief TCP Test Slave 的故障注入配置
 *
 * @details `N` 为响应序号（从 1 开始）；`std::nullopt` 表示不启用该项。
 *          与 `udp_test_slave.hpp` 的同名结构刻意保持一致的语义，便于两侧用例
 *          对照阅读；TCP 专属项（断链、分片）见本结构后半部分。
 */
struct TcpFaultInjection {
    /// @brief 丢弃第 N 个响应（Master 侧应超时）
    std::optional<std::size_t> drop_response_n;

    /// @brief 延迟第 N 个响应（毫秒）
    std::optional<std::pair<std::size_t, std::uint32_t>> delay_response_n;

    /// @brief 对第 N 个响应写入错误 LEN（Master 应判定流不可恢复）
    std::optional<std::size_t> corrupt_len_n;

    /// @brief 对第 N 个响应的 CTR 加前向跳号
    std::optional<std::size_t> jump_ctr_n;

    /// @brief 对第 N 个响应复用前一 CTR（重复）
    std::optional<std::size_t> duplicate_ctr_n;

    /// @brief 对第 N 个响应的 CTR 施加有符号偏移（模 65536）
    std::optional<std::pair<std::size_t, int>> ctr_offset_n;

    /**
     * @brief 写完第 N 个响应后立即关闭连接（模拟对端 FIN）
     * @details 用于验证"第 N 个响应后断链"：Master 应在收到该响应后
     *          观察到通道关闭，且**不**自动重连。
     */
    std::optional<std::size_t> close_after_response_n;

    /**
     * @brief 把第 N 个响应按字节粒度切成多段写出
     * @details 验证 Master 侧"recv 边界与 Frame 边界无关"。与
     *          `chunk_size_n` 二选一；本项表示**逐字节**发送。
     */
    std::optional<std::size_t> split_bytes_n;

    /**
     * @brief 把第 N 个响应按固定块大小分多次写出
     * @details `(响应序号, 块字节数)`。用于构造"帧头跨两次 write"这类
     *          真实 TCP 分段场景。
     */
    std::optional<std::pair<std::size_t, std::size_t>> chunk_size_n;

    /**
     * @brief 对第 N 个响应发送一个非法（Unknown）Profile 的帧
     * @details 测试专用：Slave 正常只会发出自己 Profile 的帧，本注入故意
     *          发出另一种布局，验证 Master 不会"自动探测"而是按流失步处理。
     */
    std::optional<std::size_t> wrong_profile_frame_n;
};

/**
 * @brief TCP 测试 Slave（Loopback，双 Wire Profile）
 *
 * @par 连接行为
 *   - 绑定 `127.0.0.1:<port>`，`port == 0` 时由 OS 分配（`Port()` 取回）；
 *   - `Start()` 后 accept **一条**活动连接；第二条入站连接被 accept 后
 *     **立即关闭**（计划 §6.1："第二连接需 accept 后显式拒绝/关闭"）。
 *
 * @par 线程模型
 *   - 接收线程：阻塞在 accept，随后循环读取并处理命令；
 *   - `Stop()` 用 `shutdown` + join 打断阻塞，**不遗留线程**（计划 §6.1）。
 *
 * @par 支持的 XCP 命令
 *   CONNECT、DISCONNECT、GET_STATUS、GET_COMM_MODE_INFO、SET_MTA、UPLOAD、
 *   SHORT_UPLOAD、DOWNLOAD、SYNCH（计划 §6.1 必需子集）。
 *
 * @note **Dynamic DAQ 未实现**。计划 §6.1 写明"否则删除 Dynamic DAQ 验收"，
 *       本批次据此**显式删除**该验收项，而非留下未实现的命令返回假成功。
 */
class TcpTestSlave {
public:
    /**
     * @brief 构造
     * @param profile 本 Slave 使用的 Wire Profile；`Unknown` 会被拒绝
     * @param fixed_port 0 = OS 分配；非 0 = 绑定该端口
     * @throws XcpException(InvalidArgument) `profile == Unknown`
     */
    explicit TcpTestSlave(TcpWireProfile profile,
                          std::uint16_t fixed_port = 0);

    /// @brief 析构：自动 Stop（保证线程被 join）
    ~TcpTestSlave();

    // 禁止拷贝：持有 Socket 与线程
    TcpTestSlave(const TcpTestSlave&) = delete;
    TcpTestSlave& operator=(const TcpTestSlave&) = delete;

    // ---- 控制 ----

    /**
     * @brief 绑定、监听并启动接收线程
     * @throws XcpException(TransportError) 绑定/监听失败
     * @note 非阻塞返回：accept 在接收线程内完成，故调用方无需并发启动 Master
     */
    void Start();

    /// @brief 停止 Slave（幂等，shutdown + join，不遗留线程）
    void Stop();

    /// @brief 实际绑定的端口（用于 Master 连接）
    [[nodiscard]] std::uint16_t Port() const;

    /**
     * @brief 是否已接受一条活动连接
     * @details 轮询本方法等待 accept 完成，避免用例对时序做假设
     */
    [[nodiscard]] bool HasActiveConnection() const;

    /// @brief 等待直到已接受连接（或超时）
    [[nodiscard]] bool WaitForConnection(std::chrono::milliseconds timeout);

    // ---- 测试配置 ----

    /// @brief 设置模拟内存内容（覆盖 `[address, address+size)` 区间）
    void SetMemory(Address address, BytesView data);

    /// @brief 设置故障注入（须在 `Start()` 前或连接建立前设置）
    void SetFaultInjection(const TcpFaultInjection& fault);

    /**
     * @brief 设置本 Slave 发送 CRM 时使用的 CTR 起始值
     * @param ctr 首个响应使用的 CTR
     * @details 默认 0。用于验证 Master 的 RX CTR 期望值处理；
     *          与 XCPlite 的 `gXcpTl.Ctr` 自增语义一致。
     */
    void SetInitialSendCtr(DatagramCtr ctr);

    /**
     * @brief 设置 CONNECT 响应中宣告的 PACKET_ALIGNMENT
     * @param alignment 对齐位宽
     * @details 仅影响 **AsamV15** 模式的 Tail 计算；AsamV14 模式恒定不读不发
     *          LEN 外 Tail（这正是 XCPlite 的实际行为）。
     */
    void SetPacketAlignment(PacketAlignment alignment);

    // ---- 只读观测 ----

    /// @brief 已处理的 XCP 命令计数
    [[nodiscard]] std::size_t CommandCount() const;

    /// @brief 是否已建立模拟 XCP 会话（收到过 CONNECT）
    [[nodiscard]] bool IsConnected() const;

    /// @brief 下一个 Slave→Master 响应将使用的 CTR
    [[nodiscard]] DatagramCtr NextSendCtr() const;

    /**
     * @brief 累计向 Master 写出的字节数
     * @details 用于验证分片注入确实改变了写出的**分段方式**而非总字节数
     */
    [[nodiscard]] std::uint64_t BytesWritten() const;

    /**
     * @brief 主动发一段原始字节到 Master（测试注入用）
     * @param raw 原样写出的字节（调用方自行负责帧结构与畸形构造）
     * @throws XcpException(InvalidState) 尚无活动连接
     * @details 用于构造"半帧后 FIN""RST 后重连"等用例——这些场景无法由
     *          命令处理路径自然产生，必须绕过正常响应流程注入。
     */
    void SendRaw(BytesView raw);

    /**
     * @brief 关闭当前活动连接（不停止监听）
     * @details 用于模拟对端主动断链。关闭后 `HasActiveConnection()` 变 false。
     */
    void CloseActiveConnection();

private:
    struct SocketImpl;

    /// @brief 接收线程主循环：accept → 读帧 → 处理命令
    void ReceiveLoop();

    /// @brief 处理一条命令并写回响应
    /// @return false 表示应终止接收循环（对端关闭或致命错误）
    bool HandleCommand(BytesView xcp_packet);

    /// @brief 命令处理主体（不涉及 framing），产出响应 Packet
    /// @param xcp_packet 收到的 XCP Packet
    /// @param should_reply 输出：是否需要发送响应（SYNCH 无需响应）
    /// @return 响应 Packet 字节
    Bytes DispatchCommand(BytesView xcp_packet, bool* should_reply);

    /**
     * @brief 按本 Slave 的 Profile 把 XCP Packet 编码为一帧
     * @param packet 原始 XCP Packet
     * @param ctr 本帧 CTR
     * @return 待写出的完整帧字节
     * @details **独立实现**，不复用生产 `EncodeEthernetFrameAligned`：
     *          见文件头"为什么两套 Profile 各自独立实现 framing"。
     */
    [[nodiscard]] Bytes EncodeFrame(BytesView packet, DatagramCtr ctr) const;

    /**
     * @brief 写出一次响应（含全部故障注入）
     * @param packet 响应 Packet
     * @return false 表示应终止接收循环
     */
    bool WriteResponse(BytesView packet);

    /// @brief 从 Socket 精确读取 `size` 字节（处理短读与分片）
    /// @return 实际读到的字节数；0 表示对端关闭；-1 表示错误
    int ReadExact(std::uint8_t* out, std::size_t size);

    /**
     * @brief 读入当前 Profile 的一个完整帧
     * @param packet_out 输出：解出的 XCP Packet（**必须传引用**）
     * @return 1 成功、0 对端关闭、-1 错误或帧畸形
     * @note 参数为 `Bytes&` 而非 `Bytes`：按值传参会让函数填充一份私有副本，
     *       调用方手里的容器永远是空的——表现为"收到了帧但命令解析为空"。
     */
    int ReadFrame(Bytes& packet_out);

    /**
     * @brief 排空内核 backlog 中的待接受连接：第一条接受，其余显式拒绝
     * @details 监听 Socket 已被置为非阻塞，故本函数不会阻塞。必须**持续**
     *          调用（而非只 accept 一次）：否则第二条连接会滞留在 backlog 中，
     *          客户端 `connect()` 由内核完成三次握手而"看似成功"，却永远
     *          收不到响应——表现为静默挂起。
     */
    void AcceptPendingConnections();

    /// @brief 关闭活动连接句柄（幂等）
    void CloseConnection();

    /// @brief Wire Profile（构造时固定，连接级不可变）
    TcpWireProfile m_profile_;
    /// @brief 配置的绑定端口（0 = OS 分配）
    std::uint16_t m_config_port_{0};
    /// @brief 平台 Socket 实现（pimpl，隔离 Windows/POSIX 差异）
    std::unique_ptr<SocketImpl> m_socket_;

    /**
     * @brief 模拟内存：地址 → 单字节
     * @details 键为**逐字节粒度**而非起始地址块。这样任意地址都能直接查到，
     *          不必让读取侧知道"某块从哪个地址开始、有多长"。早期实现用
     *          `std::map<Address, Bytes>`（块存储）却在读取时只取块首字节，
     *          导致 `SetMemory(0x70012340, {CA,FE,BA,BE})` 之后
     *          `UPLOAD 4` 只返回 `CA 00 00 00`——块与字节两种粒度混用的
     *          典型错误。
     */
    std::map<Address, std::uint8_t> m_memory_;
    /// @brief 保护 m_memory_ 与故障配置（接收线程与控制线程并发）
    mutable std::mutex m_mutex_;

    /// @brief 当前故障注入配置
    TcpFaultInjection m_fault_;
    /// @brief 已处理命令数
    std::atomic<std::size_t> m_command_count_{0};
    /// @brief 是否已 CONNECT
    std::atomic<bool> m_connected_{false};
    /// @brief 下一个响应 CTR（XCPlite `gXcpTl.Ctr` 语义）
    std::atomic<DatagramCtr> m_send_ctr_{0};
    /// @brief 已写出响应数（用于故障注入的 N 匹配）
    std::atomic<std::size_t> m_response_count_{0};
    /// @brief 累计写出字节数
    std::atomic<std::uint64_t> m_bytes_written_{0};
    /// @brief 是否已有活动连接
    std::atomic<bool> m_has_connection_{false};
    /// @brief 运行标记；`Stop()` 置 false 并 shutdown 打断阻塞
    std::atomic<bool> m_running_{false};
    /// @brief 当前 MTA（SET_MTA 设置）
    Address m_mta_{0};
    /// @brief CONNECT 宣告的 PACKET_ALIGNMENT（仅 AsamV15 用于算 Tail）
    PacketAlignment m_alignment_{PacketAlignment::Bit8};
    /// @brief 接收线程
    std::thread m_thread_;
};

}  // namespace calmcar::xcp::test

#endif  // CALMCAR_XCP_TEST_TCP_TEST_SLAVE_HPP_