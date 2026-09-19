/**
 * @file udp_transport.hpp
 * @brief 标准 XCP on UDP/IP Transport 实现（跨平台 IPv4 Socket + LEN/CTR
 * Header）。
 *
 * 依据 code-plan/XCP_1.3.0_详细设计_类接口与头文件.md 第 8 节实现。
 * 接收 CTR 策略见 §8.1，作为项目策略写入本文件注释与测试，不冒充标准条款。
 */

#ifndef CALMCAR_XCP_UDP_TRANSPORT_HPP_
#define CALMCAR_XCP_UDP_TRANSPORT_HPP_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "libxcp/ixcp_transport.hpp"
#include "libxcp/udp_header_codec.hpp"
#include "libxcp/udp_transport_config.hpp"

namespace calmcar::xcp {

/**
 * @brief 标准 XCP 1.1 Part 3 UDP/IP Transport 实现
 *
 * @par 接收 CTR 策略（与设计文档 §8.1 一致）
 *   1. 一个 UDP Datagram 按 LEN 顺序解出多个完整 Frame；每个 Frame 独立执行 CTR
 * 检查。
 *   2. 首个合法 Frame 建立 m_last_recv_ctr_ 基线，接收并推进。
 *   3. CTR == 期望值（m_last_recv_ctr_ + 1 mod 65536）→ 接收，推进。
 *   4. 前向跳号（模 65536 差值 1..32767）→ 接收当前 Frame，报告缺口，推进。
 *   5. 重复或后向乱序 → 丢弃该 Frame，报告诊断。
 *   6. 恰好相差 0x8000 → 歧义，丢弃该 Frame，报告诊断。
 *   7. Datagram 内任一 Frame 的 Header/LEN 越界或有尾部残留 → 整体丢弃该
 * Datagram。
 *   8. open() 时发送 CTR 置 0、清空接收基线；XCP Session reconnect 不单独重置。
 *   9. 不在 Transport 层重排或重传 Frame；命令超时与恢复由 CommandExecutor
 * 负责。
 */
class UdpTransport : public IXcpTransport {
public:
    explicit UdpTransport(UdpTransportConfig config);
    ~UdpTransport() override;

    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    void Open(IPacketListener& listener) override;
    void Close() override;
    void Send(BytesView packet) override;
    [[nodiscard]] bool IsOpen() const noexcept override;

    [[nodiscard]] DatagramCtr SendCtr() const noexcept;
    [[nodiscard]] std::optional<DatagramCtr> LastReceiveCtr() const noexcept;

private:
    void ReceiveLoop();

    bool HandleDatagram(const std::uint8_t* data, std::size_t size,
                        const std::string& src_ip, std::uint16_t src_port);

    bool HandleFrame(const UdpFrameView& frame);

    bool IsRemoteMatch(const std::string& src_ip, std::uint16_t src_port) const;

    struct SocketImpl;

    UdpTransportConfig m_config_;
    IPacketListener* m_listener_{nullptr};
    std::unique_ptr<SocketImpl> m_socket_;
    std::thread m_receive_thread_;
    std::atomic<bool> m_running_{false};
    mutable std::mutex m_send_mutex_;
    std::atomic<DatagramCtr> m_send_ctr_{0};
    mutable std::mutex m_recv_mutex_;
    std::optional<DatagramCtr> m_last_recv_ctr_;
    bool m_recv_baseline_established_{false};
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_UDP_TRANSPORT_HPP_