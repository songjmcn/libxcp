/**
 * @file udp_test_slave.hpp
 * @brief Loopback UDP 测试 Slave：真实 Socket，模拟最小 XCP Session
 * 并支持故障注入。
 *
 * 依据 code-plan/XCP_1.3.0_详细设计_类接口与头文件.md 第 15.2 节实现。
 * 这是测试程序而非生产 Slave SDK（计划文档 §2.3）。
 */

#ifndef CALMCAR_XCP_TEST_UDP_TEST_SLAVE_HPP_
#define CALMCAR_XCP_TEST_UDP_TEST_SLAVE_HPP_

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

namespace calmcar::xcp::test {

/// @brief 故障注入配置（N 为响应序号，从 1 开始；0 表示不启用）
struct FaultInjection {
    /// @brief 丢弃第 N 个响应（用于验证 Timeout/SYNCH 恢复）
    std::optional<std::size_t> drop_response_n;

    /// @brief 延迟第 N 个响应（毫秒）
    std::optional<std::pair<std::size_t, std::uint32_t>> delay_response_n;

    /// @brief 对第 N 个响应使用错误 LEN（验证畸形 Datagram 丢弃）
    std::optional<std::size_t> corrupt_len_n;

    /// @brief 对第 N 个响应使用跳号 CTR（验证缺口诊断）
    std::optional<std::size_t> jump_ctr_n;

    /// @brief 对第 N 个响应使用重复 CTR（验证重复丢弃）
    std::optional<std::size_t> duplicate_ctr_n;

    /**
     * @brief 对第 N 个响应的 CTR 施加指定的有符号偏移（模 65536）
     * @details 用于精确构造设计决策 D5 / §8.1 第 6 条要求的"与期望值恰好相差
     *          0x8000"歧义 Frame —— 该差值无法由跳号（前向 +5）或重复（后向
     * -1） 注入得到，必须由发送侧直接指定偏移量。
     */
    std::optional<std::pair<std::size_t, int>> ctr_offset_n;
};

/**
 * @brief UDP 测试 Slave（Loopback）
 *
 * 绑定 127.0.0.1 临时端口，模拟最小 XCP Session 和内存读取，支持
 * CONNECT/DISCONNECT/GET_STATUS/GET_COMM_MODE_INFO/SET_MTA/UPLOAD/SHORT_UPLOAD/SYNCH。
 *
 * @par XCP 1.1 Part 3 UDP/IP 连接行为（设计决策 D8）
 *   - 未连接时对 CONNECT 的来源 IP:port 应答；
 *   - 连接后仅接受 CONNECT 来源 IP 的命令（源端口可变），忽略其他 IP；
 *   - 所有响应仍发往原 CONNECT 来源 IP:port。
 */
class UdpTestSlave {
public:
    /// @brief 构造并绑定 Loopback 临时端口（使用测试默认 CONNECT 响应参数）
    UdpTestSlave();

    /// @brief 析构，自动停止
    ~UdpTestSlave();

    // 禁止拷贝
    UdpTestSlave(const UdpTestSlave&) = delete;
    UdpTestSlave& operator=(const UdpTestSlave&) = delete;

    // ---- 控制 ----

    /// @brief 启动 Slave 接收线程
    void Start();

    /// @brief 停止 Slave（幂等，join 接收线程）
    void Stop();

    /// @brief 获取 Slave 绑定的端口（用于 Master 连接）
    [[nodiscard]] std::uint16_t Port() const;

    // ---- 测试配置 ----

    /// @brief 设置模拟内存内容（覆盖 [address, address+size) 区间）
    void SetMemory(Address address, BytesView data);

    /// @brief 设置故障注入
    void SetFaultInjection(const FaultInjection& fault);

    /// @brief 获取已处理的 XCP 命令计数
    [[nodiscard]] std::size_t CommandCount() const;

    /**
     * @brief 获取下一个 Slave→Master Frame 将使用的 CTR 值
     * @details 故障注入（跳号/重复/精确偏移）只改写实际发出的 Header，
     *          不改变本计数器的推进规律；测试据此验证注入后的真实 CTR。
     */
    [[nodiscard]] DatagramCtr NextSendCtr() const;

    /// @brief 是否已建立模拟 XCP Session（诊断用）
    [[nodiscard]] bool IsConnected() const;

    /**
     * @brief 向已连接 Master 发送一个包含多个 XCP Frame 的 UDP Datagram
     * @param xcp_packets 要按顺序打包的原始 XCP Packet 列表
     * @throws XcpException(InvalidState) 尚未记录 CONNECT 来源端点
     * @throws XcpException(InvalidArgument) Frame 或 Datagram 超过允许长度
     * @details 仅用于验证 Master 接收端对 XCP 1.1 多 Frame UDP 打包的解析能力。
     */
    void SendPackedFrames(std::span<const BytesView> xcp_packets);

    /**
     * @brief 原样发送一段已构造好的 UDP Payload 到 CONNECT 来源端点
     * @param payload 完整 UDP Payload（自行负责 LEN/CTR Header 与畸形注入）
     * @throws XcpException(InvalidState) 尚未记录 CONNECT 来源端点
     * @details 测试专用。因为只有 Slave 自身的 Socket
     * 能从"配置的远端端口"发出报文， 所以注入畸形 Datagram（错误
     * LEN、残留字节等）必须走本入口， 否则会被 Master 的来源过滤先拦掉。
     */
    void SendRawPayload(BytesView payload);

    /**
     * @brief 原样发送一段 UDP Payload 到指定端点（无需先建立 CONNECT 会话）
     * @param payload 完整 UDP Payload
     * @param dst_ip 目的 IPv4 文本
     * @param dst_port 目的 UDP 端口
     * @details 测试专用。报文仍从 Slave 自身的 Socket 发出，因此源端口等于
     * Slave 端口，可通过 Master 的"严格匹配远端端口"过滤。用于在 Master 未建立
     *          XCP 会话时精确注入指定 CTR 或畸形结构。
     */
    void SendRawPayloadTo(BytesView payload, const std::string& dst_ip,
                          std::uint16_t dst_port);

private:
    /// @brief 接收线程主循环：按 LEN 解析 Datagram 内全部完整 Frame，再依序处理
    void ReceiveLoop();

    /**
     * @brief 处理收到的 UDP Datagram 中的一个 XCP Frame
     * @param xcp_packet Frame 内的原始 XCP Packet
     * @param source_ip 发送方 IPv4 地址
     * @param source_port 发送方 UDP 端口
     */
    void HandleCommand(BytesView xcp_packet, const std::string& source_ip,
                       std::uint16_t source_port);

    /**
     * @brief 判断来源是否符合当前逻辑 XCP 会话
     * @details 未连接时只允许 CONNECT 建立会话；连接后仅匹配 CONNECT 来源 IP，
     *          不要求后续命令使用同一源端口。
     */
    [[nodiscard]] bool IsCurrentSessionSource(
        const std::string& source_ip) const;

    /// @brief 发送响应到 CONNECT 时记录的来源 IP:port
    void SendResponse(BytesView xcp_packet);

    /**
     * @brief 把单个 XCP Packet 编码为一个 Frame 并发送到指定端点（含故障注入）
     * @param ip 目的 IPv4 文本
     * @param port 目的 UDP 端口
     */
    void SendResponseTo(BytesView xcp_packet, const std::string& ip,
                        std::uint16_t port);

    /// @brief 生成 RES 前缀（0xFF + 数据）
    static Bytes MakeRes(std::initializer_list<std::uint8_t> body);

    /// @brief 生成 ERR 报文
    static Bytes MakeErr(ErrorCode code);

    /// @brief 按当前 MTA 读取指定元素数的模拟内存；越界返回 nullopt
    [[nodiscard]] std::optional<Bytes> ReadAtMta(ElementCount elements);

    /// @brief 把 MTA 前进指定元素数；溢出返回 false
    bool AdvanceMta(ElementCount elements);

    /// @brief 测试 Socket 私有实现声明
    struct SocketImpl;

    std::unique_ptr<SocketImpl> m_socket_;  ///< Loopback UDP Socket 资源拥有者
    std::uint16_t m_port_{0};               ///< Socket 实际绑定的临时端口
    std::atomic<bool> m_running_{false};    ///< 接收循环运行标记
    std::thread m_receive_thread_;          ///< 测试 Slave 接收线程

    mutable std::mutex m_memory_mutex_;  ///< 保护模拟内存
    std::map<Address, Bytes> m_memory_;  ///< 按起始地址存储的模拟 ECU 内存块

    mutable std::mutex m_state_mutex_;  ///< 保护以下逻辑会话状态
    bool m_connected_{false};           ///< 是否已建立模拟 XCP Session
    std::optional<std::string> m_connect_source_ip_;  ///< CONNECT 报文来源 IPv4
    std::optional<std::uint16_t>
        m_connect_source_port_;  ///< CONNECT 来源端口（响应固定目的端口）
    Address m_mta_{0};           ///< 当前 MTA 的 32 位地址部分
    AddressExtension m_mta_extension_{0};  ///< 当前 MTA 的地址扩展部分
    DatagramCtr m_send_ctr_{0};            ///< 下一个 Slave→Master Frame 的 CTR
    std::size_t m_command_count_{0};       ///< 已处理的 XCP 命令总数
    FaultInjection m_fault_;               ///< 当前故障注入配置
    std::size_t m_response_count_{0};  ///< 已生成的响应计数（故障注入定位用）
};

}  // namespace calmcar::xcp::test

#endif  // CALMCAR_XCP_TEST_UDP_TEST_SLAVE_HPP_
