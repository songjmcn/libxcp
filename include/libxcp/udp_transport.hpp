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
#include <condition_variable>
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
    ///        OnTransportClosed
    /// @note 可安全重复调用，也可从任意线程调用（**含监听器回调内**）。从
    ///       监听器回调内调用时只发出关闭请求并立即返回：不 join、不阻塞，
    ///       避免"接收线程 join 自己"触发 `std::terminate`；句柄关闭与唤醒对
    ///       释放等收尾工作由析构函数或后续任意线程的 `Close()` 完成。
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
    /// @brief 生命周期状态机：只允许 Closed→Open→Closing→Closed 单向前进
    /// @details Closing 表示"关闭已开始、收尾尚未完成"（接收线程可能仍在
    ///          运行，或收尾被延后到析构函数）。进入 Closed 前必须已完成
    ///          线程 join 与资源释放。
    enum class LifecycleState { Closed, Open, Closing };

    /// @brief 当前线程是否是接收线程
    /// @return true 表示调用方就是接收线程（此时禁止 join 自己）
    [[nodiscard]] bool IsReceiveThread() const noexcept;

    /// @brief 发出关闭请求：置运行标记并唤醒阻塞中的接收线程
    /// @details 不 join、不释放资源，可在持 m_lifecycle_mutex_ 时调用
    void SignalShutdown() noexcept;

    /// @brief 释放唤醒 Socket 对（读端/写端在此关闭）
    void ReleaseWakeupPair();

    /// @brief 释放全部通道资源（Socket、唤醒对、监听器引用）
    /// @pre 接收线程已 join，且调用方持有 m_lifecycle_mutex_
    void ReleaseResources();

    /// @brief 安全上报一条告警：吞掉监听器抛出的异常
    /// @param message 告警文本
    /// @note 禁止在持有 m_recv_mutex_ 时调用，否则监听器重入读
    ///       LastReceiveCtr() 会因二次加锁自死锁/terminate
    void NotifyWarning(const std::string& message) noexcept;

    /// @brief 接收线程主体：select 同时等待数据 Socket 与唤醒 Socket，
    ///        唤醒 Socket 可读或数据到达时即返回，交由 HandleDatagram 处理，
    ///        退出前回调 OnTransportClosed
    void ReceiveLoop();

    /// @brief 唤醒 Socket 句柄对：用于让 Close() 立即打断阻塞中的接收线程
    /// @details 采用"回环 UDP 自连"实现（跨平台统一）：`read_handle` 绑定到
    ///          `127.0.0.1:0` 由 OS 分配端口，`write_handle` 连向该端口。
    ///          Close() 向 `write_handle` 发一字节，select() 立即可读，
    ///          接收线程无需等待轮询周期即可退出。
    /// @note 不使用 `socketpair`（Windows 无此原语），也不回退到"先关数据
    ///       Socket"（见 Close() 注释所述句柄复用风险）。
    struct WakeupPair;

    /// @brief 创建回环 UDP 自连唤醒对；失败返回 nullptr（调用方回退到轮询）
    /// @return 已就绪的唤醒对，或 nullptr 表示当前平台/环境不支持唤醒
    static std::unique_ptr<WakeupPair> CreateWakeupPair();

    /// @brief 触发一次唤醒（幂等、非阻塞、线程安全）；Close() 调用
    /// @param pair 唤醒对；为空时本函数不做任何事
    static void SignalWakeup(WakeupPair* pair) noexcept;

    /// @brief 排空唤醒 Socket 中的待读字节，避免误当作数据
    /// @param pair 唤醒对；为空时本函数不做任何事
    static void DrainWakeup(WakeupPair* pair) noexcept;

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
    /// @details 用 shared_ptr 承载：发送路径取一份快照后即可安全使用，
    ///          不必与 Close() 争用同一把锁，也不会在 reset() 后被解引用。
    std::shared_ptr<SocketImpl> m_socket_;
    /// @brief 唤醒 Socket 对（非拥有指针的 pimpl）；Open() 创建、Close() 释放
    /// @details 为空表示唤醒不可用，此时 ReceiveLoop 退化为纯 SO_RCVTIMEO
    ///          轮询（退出延迟为一个轮询周期），功能仍正确。
    std::unique_ptr<WakeupPair> m_wakeup_;
    /// @brief 接收线程；仅在打开期间 joinable
    std::thread m_receive_thread_;
    /// @brief 接收线程运行标记，Close() 置 false 后由唤醒 Socket 立即打断
    std::atomic<bool> m_running_{false};
    /// @brief 保护生命周期状态机与收尾认领，禁止与接收线程 join 同时持有
    mutable std::mutex m_lifecycle_mutex_;
    /// @brief 关闭完成通知：等待"收尾已被别人认领"的调用方在此挂起
    std::condition_variable m_close_cv_;
    /// @brief 当前生命周期状态（受 m_lifecycle_mutex_ 保护）
    LifecycleState m_state_{LifecycleState::Closed};
    /// @brief 收尾工作是否已被某个线程认领（保证只 join 一次）
    bool m_finalize_owned_{false};
    /// @brief 接收线程标识；用于识别"回调内关闭"（线程不能 join 自己）
    std::thread::id m_receive_tid_{};
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