/**
 * @file udp_transport.cpp
 * @brief UdpTransport 实现：跨平台 IPv4 UDP Socket、接收线程与双向 CTR。
 *
 * 平台差异集中在文件顶部的少量适配代码（Socket 句柄类型、关闭唤醒方式、
 * 错误码），其余逻辑三平台共用（计划文档 §11.5）。
 */

#include "libxcp/udp_transport.hpp"

#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 平台 Socket 适配层
// ---------------------------------------------------------------------------
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace calmcar::xcp {
namespace {

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

/// @brief 最后一次平台 Socket 错误的字符串描述
std::string lastSocketError() {
    const int code = static_cast<int>(WSAGetLastError());
    char buffer[256] = {0};
    ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                     nullptr, static_cast<DWORD>(code), 0, buffer,
                     static_cast<DWORD>(sizeof(buffer)), nullptr);
    return "WSAErr=" + std::to_string(code) + " " + std::string(buffer);
}

/// @brief 是否属于"接收超时"（非故障，用于轮询关闭标记）
bool isTimeoutError() { return WSAGetLastError() == WSAETIMEDOUT; }

/// @brief 是否属于"Socket 已被关闭/中断"（退出接收循环）
bool isInterruptedError() {
    const int code = WSAGetLastError();
    return code == WSAEINTR || code == WSAEBADF || code == WSAENOTCONN ||
           code == WSAEINVAL || code == WSAECONNRESET || code == WSAESHUTDOWN;
}

void closeSocket(SocketHandle handle) { ::closesocket(handle); }
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

std::string lastSocketError() {
    return "errno=" + std::to_string(errno) + " " + std::strerror(errno);
}

bool isTimeoutError() {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

bool isInterruptedError() {
    return errno == EBADF || errno == EINVAL || errno == ENOTCONN;
}

void closeSocket(SocketHandle handle) { ::close(handle); }
#endif

/// @brief Windows 进程内 WSAStartup/WSACleanup 引用计数（仅本文件使用）
#if defined(_WIN32)
class WinsockSession {
public:
    WinsockSession() {
        if (s_ref_count_ == 0) {
            WSADATA data;
            s_ok_ = (::WSAStartup(MAKEWORD(2, 2), &data) == 0);
        } else {
            s_ok_ = true;
        }
        ++s_ref_count_;
    }

    ~WinsockSession() {
        if (--s_ref_count_ == 0 && s_ok_) {
            ::WSACleanup();
        }
    }

    WinsockSession(const WinsockSession&) = delete;
    WinsockSession& operator=(const WinsockSession&) = delete;

    [[nodiscard]] bool ok() const noexcept { return s_ok_; }

private:
    static inline int s_ref_count_{0};  ///< 活动引用数
    static inline bool s_ok_{false};    ///< WSAStartup 结果
};
#endif

/// @brief 接收缓冲区大小（字节）；一次 recvfrom 读取一个完整 Datagram
constexpr std::size_t kReceiveBufferSize = 65536;

/// @brief 将点分 IPv4 文本转为网络序地址；失败返回 false
bool parseIpv4(const std::string& text, in_addr* out) {
    if (text.empty()) {
        return false;
    }
#if defined(_WIN32)
    return ::inet_pton(AF_INET, text.c_str(), out) == 1;
#else
    return ::inet_pton(AF_INET, text.c_str(), out) == 1;
#endif
}

/**
 * @brief 模 65536 的前向距离：从 base 前进多少步到达 value
 * @return 0..65535；value == base 时为 0
 */
int ctrForwardDistance(DatagramCtr base, DatagramCtr value) noexcept {
    return static_cast<int>(
        (static_cast<unsigned int>(value) - static_cast<unsigned int>(base)) &
        0xFFFFU);
}

}  // namespace

// ---------------------------------------------------------------------------
// SocketImpl：拥有平台 Socket 句柄与 Windows 生命周期
// ---------------------------------------------------------------------------
struct UdpTransport::SocketImpl {
#if defined(_WIN32)
    WinsockSession winsock;  ///< RAII 引用 WSAStartup/WSACleanup
#endif
    SocketHandle handle{kInvalidSocket};  ///< Socket 句柄
    bool bound{false};                    ///< 是否已成功绑定
    sockaddr_in remote{};                 ///< open() 时解析并缓存的远端端点
};

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

UdpTransport::UdpTransport(UdpTransportConfig config)
    : m_config_(std::move(config)) {}

UdpTransport::~UdpTransport() {
    // 析构路径不允许抛异常；close() 内部已吞掉回调异常
    try {
        Close();
    } catch (...) {
    }
}

// ---------------------------------------------------------------------------
// Open / Close
// ---------------------------------------------------------------------------

void UdpTransport::Open(IPacketListener& listener) {
    Close();  // 幂等：允许在已打开的实例上重新打开

    // ---- 配置校验（设计文档 §7 / 计划文档 §4.8）----
    if (m_config_.m_remote_port_ == 0U) {
        throw detail::MakeInvalidArgument(
            "UdpTransportConfig::remotePort 不能为 0");
    }
    if (m_config_.m_max_frame_packet_size_ == 0U ||
        m_config_.m_max_frame_packet_size_ > kUdpMaxXcpPacket) {
        throw detail::MakeInvalidArgument("maxFramePacketSize 必须在 1.." +
                                          std::to_string(kUdpMaxXcpPacket) +
                                          " 之间");
    }
    if (m_config_.m_max_datagram_size_ < kUdpHeaderSize + 1 ||
        m_config_.m_max_datagram_size_ > kUdpMaxDatagramSize) {
        throw detail::MakeInvalidArgument(
            "maxDatagramSize 必须在 " + std::to_string(kUdpHeaderSize + 1) +
            ".." + std::to_string(kUdpMaxDatagramSize) + " 之间");
    }

    auto impl = std::make_unique<SocketImpl>();
#if defined(_WIN32)
    if (!impl->winsock.ok()) {
        throw detail::MakeTransportError("Winsock 初始化失败",
                                         lastSocketError());
    }
#endif

    impl->handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl->handle == kInvalidSocket) {
        throw detail::MakeTransportError("创建 UDP Socket 失败",
                                         lastSocketError());
    }

    // ---- 绑定本地端点 ----
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(m_config_.m_local_port_);
    if (!parseIpv4(m_config_.m_local_host_.empty() ? std::string("0.0.0.0")
                                                   : m_config_.m_local_host_,
                   &local.sin_addr)) {
        closeSocket(impl->handle);
        throw detail::MakeInvalidArgument("无法解析本地绑定地址: " +
                                          m_config_.m_local_host_);
    }
    if (::bind(impl->handle, reinterpret_cast<sockaddr*>(&local),
               sizeof(local)) != 0) {
        closeSocket(impl->handle);
        throw detail::MakeTransportError(
            "绑定本地端点失败: " + m_config_.m_local_host_ + ":" +
                std::to_string(m_config_.m_local_port_),
            lastSocketError());
    }
    impl->bound = true;

    // ---- 记录实际绑定的本地端口（OS 分配临时端口时需回填，供诊断）----
    sockaddr_in actual_local{};
    socklen_t actual_len = sizeof(actual_local);
    if (::getsockname(impl->handle, reinterpret_cast<sockaddr*>(&actual_local),
                      &actual_len) == 0) {
        if (m_config_.m_local_port_ == 0U) {
            m_config_.m_local_port_ = ntohs(actual_local.sin_port);
        }
    }

    // ---- 远端端点 ----
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(m_config_.m_remote_port_);
    if (!parseIpv4(m_config_.m_remote_host_, &remote.sin_addr)) {
        closeSocket(impl->handle);
        throw detail::MakeInvalidArgument("无法解析远端地址: " +
                                          m_config_.m_remote_host_);
    }

    // ---- 接收超时：使阻塞接收可周期性检查关闭标记，保证 close() 及时生效 ----
#if defined(_WIN32)
    DWORD tv_ms = m_config_.m_receive_poll_interval_ms_;
    ::setsockopt(impl->handle, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&tv_ms), sizeof(tv_ms));
#else
    timeval tv{};
    tv.tv_sec = m_config_.m_receive_poll_interval_ms_ / 1000U;
    tv.tv_usec = (m_config_.m_receive_poll_interval_ms_ % 1000U) * 1000U;
    ::setsockopt(impl->handle, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    // 解析后的远端 sockaddr 缓存到 SocketImpl，避免每次发送重复解析字符串。
    impl->remote = remote;

    m_socket_ = std::move(impl);
    m_listener_ = &listener;

    // ---- CTR 复位（设计决策 D1：每次 open() 发送 CTR 置 0、清空接收基线）----
    m_send_ctr_.store(0, std::memory_order_relaxed);
    {
        const std::lock_guard<std::mutex> lock(m_recv_mutex_);
        m_last_recv_ctr_.reset();
        m_recv_baseline_established_ = false;
    }

    m_running_.store(true, std::memory_order_release);
    try {
        m_receive_thread_ = std::thread([this] { ReceiveLoop(); });
    } catch (const std::system_error& e) {
        m_running_.store(false, std::memory_order_release);
        closeSocket(m_socket_->handle);
        m_socket_.reset();
        m_listener_ = nullptr;
        throw detail::MakeTransportError("启动接收线程失败", e.what());
    }
}

void UdpTransport::Close() {
    // 1. 通知接收线程退出
    m_running_.store(false, std::memory_order_release);

    // 2. 关闭 Socket 唤醒可能阻塞在 recvfrom 的线程（POSIX 下依赖 RCVTIMEO
    // 轮询）
    if (m_socket_ && m_socket_->handle != kInvalidSocket) {
        closeSocket(m_socket_->handle);
        m_socket_->handle = kInvalidSocket;
    }

    // 3. 等待接收线程结束（其退出前负责回调 onTransportClosed）
    if (m_receive_thread_.joinable()) {
        m_receive_thread_.join();
    }

    m_socket_.reset();
    m_listener_ = nullptr;
}

// ---------------------------------------------------------------------------
// send
// ---------------------------------------------------------------------------

void UdpTransport::Send(BytesView packet) {
    std::lock_guard<std::mutex> lock(m_send_mutex_);

    if (!IsOpen()) {
        throw detail::MakeInvalidState("UdpTransport 未打开时调用 send()");
    }
    if (packet.empty()) {
        throw detail::MakeInvalidArgument("XCP Packet 不能为空");
    }
    if (packet.size() > m_config_.m_max_frame_packet_size_) {
        throw detail::MakeInvalidArgument(
            "XCP Packet 长度 " + std::to_string(packet.size()) +
            " 超过配置上限 maxFramePacketSize=" +
            std::to_string(m_config_.m_max_frame_packet_size_));
    }

    // 取 CTR 并按模 65536 递增（每个 XCP Frame 消耗一个值，而非每个 Datagram）
    const auto ctr = m_send_ctr_.load(std::memory_order_relaxed);
    m_send_ctr_.store(static_cast<DatagramCtr>(ctr + 1U),
                      std::memory_order_relaxed);

    UdpFrame frame;
    try {
        frame = encodeUdpFrame(packet, ctr);
    } catch (...) {
        // 编码失败不回退 CTR：保持"发送方向每 Frame 独立消耗一个
        // CTR"的可观察语义
        throw;
    }

    if (frame.m_data_.size() > m_config_.m_max_datagram_size_) {
        throw detail::MakeInvalidArgument(
            "编码后 Datagram 长度 " + std::to_string(frame.m_data_.size()) +
            " 超过配置上限 maxDatagramSize=" +
            std::to_string(m_config_.m_max_datagram_size_));
    }

    const int sent = ::sendto(
        m_socket_->handle, reinterpret_cast<const char*>(frame.m_data_.data()),
        static_cast<int>(frame.m_data_.size()), 0,
        reinterpret_cast<const sockaddr*>(&m_socket_->remote),
        sizeof(m_socket_->remote));
    if (sent < 0) {
        throw detail::MakeTransportError("发送 UDP Datagram 失败",
                                         lastSocketError());
    }
    if (static_cast<std::size_t>(sent) != frame.m_data_.size()) {
        throw detail::MakeTransportError(
            "UDP Datagram 部分发送: " + std::to_string(sent) + "/" +
            std::to_string(frame.m_data_.size()));
    }
}

bool UdpTransport::IsOpen() const noexcept {
    return m_running_.load(std::memory_order_acquire);
}

DatagramCtr UdpTransport::SendCtr() const noexcept {
    return m_send_ctr_.load(std::memory_order_relaxed);
}

std::optional<DatagramCtr> UdpTransport::LastReceiveCtr() const noexcept {
    const std::lock_guard<std::mutex> lock(m_recv_mutex_);
    return m_last_recv_ctr_;
}

// ---------------------------------------------------------------------------
// 接收线程
// ---------------------------------------------------------------------------

void UdpTransport::ReceiveLoop() {
    IPacketListener* listener = m_listener_;
    std::vector<std::uint8_t> buffer(kReceiveBufferSize);
    std::string close_reason = "Transport 正常关闭";

    while (m_running_.load(std::memory_order_acquire)) {
        sockaddr_in src{};
        socklen_t src_len = sizeof(src);
        const int received =
            ::recvfrom(m_socket_ ? m_socket_->handle : kInvalidSocket,
                       reinterpret_cast<char*>(buffer.data()),
                       static_cast<int>(buffer.size()), 0,
                       reinterpret_cast<sockaddr*>(&src), &src_len);

        if (received > 0) {
            char ip_text[INET_ADDRSTRLEN] = {0};
#if defined(_WIN32)
            ::inet_ntop(AF_INET, &src.sin_addr, ip_text, INET_ADDRSTRLEN);
#else
            ::inet_ntop(AF_INET, &src.sin_addr, ip_text, sizeof(ip_text));
#endif
            const bool keep_running = HandleDatagram(
                buffer.data(), static_cast<std::size_t>(received),
                std::string(ip_text), ntohs(src.sin_port));
            if (!keep_running) {
                break;
            }
            continue;
        }

        if (received == 0) {
            // UDP 下空 Datagram：按畸形处理并丢弃，继续运行
            if (listener != nullptr) {
                listener->OnTransportWarning(
                    "收到长度为 0 的 UDP Datagram，已丢弃");
            }
            continue;
        }

        // received < 0
        if (!m_running_.load(std::memory_order_acquire)) {
            break;  // close() 触发，正常退出
        }
        if (isTimeoutError()) {
            continue;  // 轮询周期到，检查关闭标记
        }
        if (isInterruptedError()) {
            close_reason = "Socket 已关闭或失效，接收线程退出";
            break;
        }
        // 其它错误：报告警告并继续（避免瞬时错误终止会话）
        if (listener != nullptr) {
            listener->OnTransportWarning("recvfrom 失败: " + lastSocketError());
        }
    }

    m_running_.store(false, std::memory_order_release);

    // 退出前统一交付关闭事件；此后不再有 onPacketReceived 回调
    if (listener != nullptr) {
        try {
            listener->OnTransportClosed(close_reason);
        } catch (...) {
            // 监听器异常不得逃逸出 Transport 线程
        }
    }
}

bool UdpTransport::IsRemoteMatch(const std::string& src_ip,
                                 std::uint16_t src_port) const {
    if (src_ip != m_config_.m_remote_host_) {
        return false;
    }
    if (m_config_.m_strict_remote_port_ &&
        src_port != m_config_.m_remote_port_) {
        return false;
    }
    return true;
}

bool UdpTransport::HandleDatagram(const std::uint8_t* data, std::size_t size,
                                  const std::string& src_ip,
                                  std::uint16_t src_port) {
    IPacketListener* listener = m_listener_;

    if (!IsRemoteMatch(src_ip, src_port)) {
        // 来源过滤：项目安全策略（D3），丢弃并诊断
        if (listener != nullptr) {
            listener->OnTransportWarning(
                "丢弃来自非配置远端的 Datagram: " + src_ip + ":" +
                std::to_string(src_port));
        }
        return true;
    }

    if (size > m_config_.m_max_datagram_size_) {
        if (listener != nullptr) {
            listener->OnTransportWarning("Datagram 长度 " +
                                         std::to_string(size) +
                                         " 超过配置上限，已丢弃");
        }
        return true;
    }

    const BytesView view{data, size};
    auto frames = decodeUdpDatagram(view);
    if (!frames) {
        // 原子性策略：任一 Frame 非法则整个 Datagram 不交付（§8.1 第 7 条）
        if (listener != nullptr) {
            listener->OnTransportWarning(
                "畸形 UDP Datagram（Header/LEN 越界或残留字节），整体丢弃");
        }
        return true;
    }

    for (const auto& frame : *frames) {
        if (frame.m_header_.m_len_ > m_config_.m_max_frame_packet_size_) {
            if (listener != nullptr) {
                listener->OnTransportWarning(
                    "Frame 内 XCP Packet 长度 " +
                    std::to_string(frame.m_header_.m_len_) +
                    " 超过配置上限，该 Frame 已丢弃");
            }
            continue;
        }
        if (!HandleFrame(frame)) {
            continue;  // CTR 规则丢弃
        }
        if (listener != nullptr) {
            try {
                listener->OnPacketReceived(frame.m_xcp_packet_);
            } catch (...) {
                // 上层回调异常不得终止接收线程
            }
        }
    }
    return true;
}

bool UdpTransport::HandleFrame(const UdpFrameView& frame) {
    const std::lock_guard<std::mutex> lock(m_recv_mutex_);
    IPacketListener* listener = m_listener_;

    if (!m_recv_baseline_established_) {
        // 首个合法 Frame 建立基线（§8.1 第 2 条）
        m_recv_baseline_established_ = true;
        m_last_recv_ctr_ = frame.m_header_.m_ctr_;
        return true;
    }

    const DatagramCtr previous = m_last_recv_ctr_.value_or(0U);
    const DatagramCtr expected = static_cast<DatagramCtr>(previous + 1U);

    if (frame.m_header_.m_ctr_ == expected) {
        m_last_recv_ctr_ = frame.m_header_.m_ctr_;  // §8.1 第 3 条
        return true;
    }

    // 以期望值为基准计算前向距离：0 表示重复，1..32767 表示前向跳号（缺包），
    // >= 32768 表示后向乱序或方向歧义。
    const int delta_from_expected =
        ctrForwardDistance(expected, frame.m_header_.m_ctr_);

    if (delta_from_expected == 0) {
        // 与期望值相同不可能走到这里（上面已判等），保留为防御分支
        m_last_recv_ctr_ = frame.m_header_.m_ctr_;
        return true;
    }
    if (delta_from_expected == 0x8000) {
        // 恰好相差 0x8000：方向歧义，丢弃（§8.1 第 6 条 / D5）
        if (listener != nullptr) {
            listener->OnTransportWarning(
                "CTR 与期望值差 0x8000，方向歧义，已丢弃: " +
                std::to_string(frame.m_header_.m_ctr_));
        }
        return false;
    }
    if (delta_from_expected > 0 && delta_from_expected < 0x8000) {
        // 前向跳号：接收、报告缺口、推进（§8.1 第 4 条）
        if (listener != nullptr) {
            listener->OnTransportWarning(
                "CTR 前向跳号，疑似缺包：期望 " + std::to_string(expected) +
                "，实收 " + std::to_string(frame.m_header_.m_ctr_) + "，缺口 " +
                std::to_string(delta_from_expected));
        }
        m_last_recv_ctr_ = frame.m_header_.m_ctr_;
        return true;
    }

    // 后向（distance 落在 0x8001..0xFFFF）：重复或迟到乱序，丢弃（§8.1 第 5
    // 条）
    if (listener != nullptr) {
        listener->OnTransportWarning(
            "丢弃重复/后向乱序 CTR: " + std::to_string(frame.m_header_.m_ctr_) +
            "，最近值 " + std::to_string(previous));
    }
    return false;
}

}  // namespace calmcar::xcp
