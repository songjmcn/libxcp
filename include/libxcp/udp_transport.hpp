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
 *   8. Open() 时发送 CTR 置 0、清空接收基线；XCP Session 重连不单独重置。
 *   9. 不在 Transport 层重排或重传 Frame；命令超时与恢复由 CommandExecutor
 * 负责。
 */
class UdpTransport : public IXcpTransport {
public:
    /// @brief 构造 UDP Transport
    /// @param config 端点与行为配置（本地/远端地址端口、MAX_FRAME_PACKET_SIZE、
    ///               来源过滤策略等）；此处仅保存，合法性在 Open() 校验
    explicit UdpTransport(UdpTransportConfig config);

    /// @brief 析构；若仍处于打开状态则先执行 Close()，保证接收线程被 join
    ~UdpTransport() override;

    // 禁止拷贝
    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    /// @brief 打开通道：校验配置、创建并绑定 Socket、缓存远端 sockaddr、
    ///        启动接收线程，并把发送 CTR 置 0、清空接收基线（§8.1 第 8 条）
    /// @param listener 包监听器，生命周期须长于本 Transport 使用期
    /// @throws XcpException(InvalidArgument) 配置非法（端口为 0、尺寸越界等）
    /// @throws XcpException(TransportError) Socket 创建/绑定/选项设置失败
    /// @note 幂等：内部先调用 Close()，允许在同一实例上重新打开
    void Open(IPacketListener& listener) override;

    /// @brief 关闭通道：通知接收线程退出并 join，退出前由该线程回调
    ///        OnTransportClosed；可安全重复调用
    /// @note 不在 join 之前关闭 Socket——句柄号会被 OS 立即回收并可能分配给
    ///       进程内新建的 Socket，而接收线程此刻仍可能正在处理入站包
    void Close() override;

    /// @brief 发送一个完整 XCP CTO Packet（必要时切成多个 XCP 1.1 UDP Frame）
    /// @param packet 完整 XCP Packet 字节（不含 Transport Header）
    /// @throws XcpException(InvalidState) 未打开时调用
    /// @throws XcpException(InvalidArgument) packet 为空或超过单包上限
    /// @throws XcpException(TransportError) 发送失败
    /// @note 线程安全：发送 CTR 在 m_send_mutex_ 下逐 Frame 递增
    void Send(BytesView packet) override;

    /// @brief Transport 是否已打开
    [[nodiscard]] bool IsOpen() const noexcept override;

    /// @brief 当前发送计数器值，即下一个待分配的 Datagram CTR
    /// @details Open() 后为 0；每发出一个 Frame 递增一次（不是每条命令一次）
    [[nodiscard]] DatagramCtr SendCtr() const noexcept;

    /// @brief 最近一个通过 §8.1 CTR 连续性校验的入站 Frame 的 CTR
    /// @return 尚未收到任何有效 Frame 时返回 std::nullopt
    [[nodiscard]] std::optional<DatagramCtr> LastReceiveCtr() const noexcept;

private:
    /// @brief 接收线程主体：带 SO_RCVTIMEO 轮询收包，交由 HandleDatagram 处理，
    ///        退出前回调 OnTransportClosed
    void ReceiveLoop();

    /// @brief 处理一个收到的 Datagram：来源过滤 → 解码 Frame 序列 → 逐帧投递
    /// @param data 原始字节
    /// @param size 字节数
    /// @param src_ip 来源地址（字符串形式，用于来源过滤）
    /// @param src_port 来源端口
    /// @return 该 Datagram 是否已被处理完毕（畸形包与按策略丢弃的帧不视为错误）
    bool HandleDatagram(const std::uint8_t* data, std::size_t size,
                        const std::string& src_ip, std::uint16_t src_port);

    /// @brief 对单个 Frame 执行 §8.1 CTR 规则并投递其 XCP Packet
    /// @param frame 已解码的 Frame（含 header 与 packet 视图）
    /// @return false 表示该 Frame 被丢弃（CTR 与期望差 0x8000 方向歧义，或
    ///         重复/后向乱序）；true 表示已建立基线或已投递给监听器
    /// @note 监听器回调抛出的异常在此吞掉，不得终止接收线程
    bool HandleFrame(const UdpFrameView& frame);

    /// @brief 判断入站 Datagram 的来源是否匹配配置
    /// @details remote_host 必须相等；仅当 strict_remote_port 为真时才要求
    ///          端口也相等（项目安全策略 D3）
    /// @param src_ip 来源地址
    /// @param src_port 来源端口
    bool IsRemoteMatch(const std::string& src_ip, std::uint16_t src_port) const;

    struct SocketImpl;

    /// @brief 端点与行为配置（Open() 时校验，运行期只读）
    UdpTransportConfig m_config_;
    /// @brief 收包监听器（非拥有）；Open() 写入、Close() 置空
    IPacketListener* m_listener_{nullptr};
    /// @brief 平台 Socket 实现（pimpl，隔离 Windows/POSIX 差异）
    std::unique_ptr<SocketImpl> m_socket_;
    /// @brief 接收线程；仅在打开期间 joinable
    std::thread m_receive_thread_;
    /// @brief 接收线程运行标记，Close() 置 false 后线程依 SO_RCVTIMEO 轮询退出
    std::atomic<bool> m_running_{false};
    /// @brief 保护发送路径与发送 CTR
    mutable std::mutex m_send_mutex_;
    /// @brief 下一个待分配的 Datagram CTR（逐 Frame 递增）
    std::atomic<DatagramCtr> m_send_ctr_{0};
    /// @brief 保护接收基线与最近接收 CTR
    mutable std::mutex m_recv_mutex_;
    /// @brief 最近一个有效入站 Frame 的 CTR；未建立基线时为空
    std::optional<DatagramCtr> m_last_recv_ctr_;
    /// @brief 首包基线是否已建立（首个 Frame 只建立基线，不做连续性判断）
    bool m_recv_baseline_established_{false};
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_UDP_TRANSPORT_HPP_