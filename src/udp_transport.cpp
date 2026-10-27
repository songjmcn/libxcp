/**
 * @file udp_transport.cpp
 * @brief UdpTransport 实现：跨平台 IPv4 UDP Socket、接收线程与双向 CTR。
 *
 * Socket 操作与地址处理统一经 detail::SocketOps 完成。
 */

#include "libxcp/udp_transport.hpp"

#include <array>
#include <span>
#include <string>
#include <vector>

#include "detail/socket/socket_ops.hpp"
#include "event_poller.hpp"
#include "libxcp/log.hpp"

namespace calmcar::xcp {
namespace {

/// @brief 与平台无关的套接字句柄。
using SocketHandle = detail::SocketHandle;

/// @brief 无效套接字哨兵。
constexpr SocketHandle kInvalidSocket = detail::kInvalidSocketHandle;

/// @brief 接收缓冲区大小（字节）；一次 recvfrom 读取一个完整 Datagram
constexpr std::size_t kReceiveBufferSize = 65536;

/**
 * @brief 模 65536 的前向距离：从 base 前进多少步到达 value
 * @return 0..65535；value == base 时为 0
 */
int ctrForwardDistance(DatagramCtr base, DatagramCtr value) noexcept {
    return static_cast<int>(
        (static_cast<unsigned int>(value) - static_cast<unsigned int>(base)) &
        0xFFFFU);
}

/// @brief 将 CTR 格式化为 4 位十六进制（用于诊断文本，便于与 §8.1 条款对照）
std::string ctrToHex(DatagramCtr value) {
    constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(6);
    out += "0x";
    for (int shift = 12; shift >= 0; shift -= 4) {
        out.push_back(kDigits[(value >> shift) & 0x0FU]);
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// SocketImpl：保存 SocketOps 句柄与远端端点
// ---------------------------------------------------------------------------
/// @brief UDP 数据套接字状态。
struct UdpTransport::SocketImpl {
    SocketHandle handle{kInvalidSocket};  ///< Socket 句柄。
    bool bound{false};                    ///< 是否已成功绑定。
    detail::Ipv4Endpoint remote{};        ///< Open() 时解析并缓存的远端端点。
};

// ---------------------------------------------------------------------------
// WakeupPair：回环 UDP 自连唤醒对
// ---------------------------------------------------------------------------

/// @brief Close() 立即打断阻塞接收所用的唤醒 Socket 对
/// @details 为什么用"回环 UDP 自连"而不是别的方案：
///          - `socketpair()` 是 POSIX 专有，Windows 无对应原语；
///          - 平台专属等待事件需要分叉出两套等待逻辑，
///            回归面翻倍；
///          - 回环 UDP 在三个目标平台上语义一致，且与既有 UDP Socket 代码
///            共用同一套错误处理辅助函数。
///          为什么**不**回退到"Close() 先关数据 Socket"：见 Close() 内注释，
///          那样会让句柄被 OS 回收并可能被本进程新建 Socket 复用，导致接收
///          线程的迟到回调命中复用句柄，造成跨会话串扰。
struct UdpTransport::WakeupPair {
    SocketHandle read_handle{kInvalidSocket};   ///< 本端（绑定后等待可读）。
    SocketHandle write_handle{kInvalidSocket};  ///< 对端（用于写入唤醒字节）。
};

std::unique_ptr<UdpTransport::WakeupPair> UdpTransport::CreateWakeupPair() {
    auto pair = std::make_unique<WakeupPair>();
    auto& ops = detail::CurrentSocketOps();
    // 读端绑定回环临时端口，供写端发送唤醒字节。
    pair->read_handle = ops.create(detail::SocketKind::Udp);
    if (pair->read_handle == kInvalidSocket) {
        LIBXCP_LOG_WARNING("UDP wakeup socket create failed: {}",
                           ops.last_error());
        return nullptr;
    }
    detail::Ipv4Endpoint local{{127, 0, 0, 1}, 0};
    if (ops.bind(pair->read_handle, local) != 0) {
        LIBXCP_LOG_WARNING("UDP wakeup bind failed: {}", ops.last_error());
        ops.close(pair->read_handle);
        return nullptr;
    }
    if (ops.getsockname(pair->read_handle, &local) != 0) {
        LIBXCP_LOG_WARNING("UDP wakeup getsockname failed: {}",
                           ops.last_error());
        ops.close(pair->read_handle);
        return nullptr;
    }
    // 排空读端需非阻塞，避免最后一次读取挂起。
    if (ops.set_non_blocking(pair->read_handle) != 0) {
        LIBXCP_LOG_WARNING("UDP wakeup set nonblocking failed: {}",
                           ops.last_error());
        ops.close(pair->read_handle);
        return nullptr;
    }
    pair->write_handle = ops.create(detail::SocketKind::Udp);
    if (pair->write_handle == kInvalidSocket) {
        LIBXCP_LOG_WARNING("UDP wakeup writer create failed: {}",
                           ops.last_error());
        ops.close(pair->read_handle);
        return nullptr;
    }
    if (ops.set_option(pair->write_handle, detail::SocketOption::NoSigPipe,
                       true) != 0) {
        LIBXCP_LOG_WARNING("UDP wakeup SIGPIPE protection setup failed: {}",
                           ops.last_error());
    }
    if (ops.connect(pair->write_handle, local) != 0) {
        LIBXCP_LOG_WARNING("UDP wakeup connect failed: {}", ops.last_error());
        ops.close(pair->write_handle);
        ops.close(pair->read_handle);
        return nullptr;
    }
    return pair;
}

void UdpTransport::SignalWakeup(WakeupPair* pair) noexcept {
    if (pair == nullptr || pair->write_handle == kInvalidSocket) {
        return;
    }
    // 唤醒字节内容无意义，仅用于让读端变为可读；失败也不影响正确性
    // （ReceiveLoop 仍有接收超时轮询兜底），故显式忽略返回值。
    const std::uint8_t token = 0;
    (void)detail::CurrentSocketOps().send(pair->write_handle, &token, 1);
}

void UdpTransport::DrainWakeup(WakeupPair* pair) noexcept {
    if (pair == nullptr || pair->read_handle == kInvalidSocket) {
        return;
    }
    // 可能存在多次唤醒写入，循环读到空为止（读端已设为非阻塞）
    std::uint8_t buffer[64];
    for (;;) {
        if (detail::CurrentSocketOps().recv(pair->read_handle, buffer,
                                            sizeof(buffer)) <= 0) {
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

UdpTransport::UdpTransport(UdpTransportConfig config)
    : m_config_(std::move(config)) {}

UdpTransport::~UdpTransport() {
    // 析构路径不允许抛异常；Close() 内部已吞掉回调异常
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
    if (m_config_.remote_port == 0U) {
        LIBXCP_LOG_ERROR("UDP open rejected because remote_port is zero");
        throw detail::MakeInvalidArgument(
            "UdpTransportConfig::remote_port 不能为 0");
    }
    if (m_config_.max_frame_packet_size == 0U ||
        m_config_.max_frame_packet_size > kUdpMaxXcpPacket) {
        LIBXCP_LOG_ERROR(
            "UDP open rejected max_frame_packet_size={} maximum={}",
            m_config_.max_frame_packet_size, kUdpMaxXcpPacket);
        throw detail::MakeInvalidArgument("max_frame_packet_size 必须在 1.." +
                                          std::to_string(kUdpMaxXcpPacket) +
                                          " 之间");
    }
    if (m_config_.max_datagram_size < kUdpHeaderSize + 1 ||
        m_config_.max_datagram_size > kUdpMaxDatagramSize) {
        LIBXCP_LOG_ERROR(
            "UDP open rejected max_datagram_size={} allowed={}..{}",
            m_config_.max_datagram_size, kUdpHeaderSize + 1,
            kUdpMaxDatagramSize);
        throw detail::MakeInvalidArgument(
            "max_datagram_size 必须在 " + std::to_string(kUdpHeaderSize + 1) +
            ".." + std::to_string(kUdpMaxDatagramSize) + " 之间");
    }
    if (m_config_.receive_poll_interval_ms == 0U) {
        LIBXCP_LOG_ERROR(
            "UDP open rejected because receive poll interval is zero");
        throw detail::MakeInvalidArgument(
            "receive_poll_interval_ms 必须大于 0");
    }
    // PACKET_ALIGNMENT 允许为 Unknown（P1 起不再拒绝）：UDP 一个数据报就是
    // 一个承载单元，Frame 边界可直接观测，接收按边界推导 Tail、发送不补
    // Tail，因此不依赖对齐证据。给了对齐只用于一致性诊断。
    if (m_config_.packet_alignment == PacketAlignment::Unknown) {
        LIBXCP_LOG_INFO(
            "UDP open without packet alignment: using datagram-boundary "
            "framing (send unpadded, accept 0..3 tail bytes)");
    }
    // T-06 能力边界：本库只承诺 IPv4 UDP 单播；IPv6 显式报 Unsupported，
    // 不得退化为"地址非法"这类无法区分笔误与能力缺失的报错
    if (detail::LooksLikeIpv6(m_config_.remote_host)) {
        LIBXCP_LOG_ERROR("UDP open rejected: IPv6 remote unsupported host={}",
                         m_config_.remote_host);
        throw detail::MakeUnsupportedFeature(
            "IPv6 UDP 传输尚未实现（本版本基础 Profile 仅承诺 IPv4 UDP "
            "单播）：remote_host=" +
            m_config_.remote_host);
    }
    if (detail::LooksLikeIpv6(m_config_.local_host)) {
        LIBXCP_LOG_ERROR("UDP open rejected: IPv6 local unsupported host={}",
                         m_config_.local_host);
        throw detail::MakeUnsupportedFeature(
            "IPv6 UDP 传输尚未实现（本版本基础 Profile 仅承诺 IPv4 UDP "
            "单播）：local_host=" +
            m_config_.local_host);
    }

    auto impl = std::make_unique<SocketImpl>();
    auto& ops = detail::CurrentSocketOps();
    impl->handle = ops.create(detail::SocketKind::Udp);
    if (impl->handle == kInvalidSocket) {
        const auto error = ops.last_error();
        LIBXCP_LOG_ERROR("UDP socket creation failed: {}", error);
        throw detail::MakeTransportError("创建 UDP Socket 失败", error);
    }

    // ---- 绑定本地端点 ----
    detail::Ipv4Endpoint local{};
    local.port = m_config_.local_port;
    if (!detail::ParseIpv4Literal(m_config_.local_host.empty()
                                      ? std::string("0.0.0.0")
                                      : m_config_.local_host,
                                  &local.address)) {
        LIBXCP_LOG_ERROR("UDP invalid local bind address='{}'",
                         m_config_.local_host);
        ops.close(impl->handle);
        throw detail::MakeInvalidArgument("无法解析本地绑定地址: " +
                                          m_config_.local_host);
    }
    if (ops.bind(impl->handle, local) != 0) {
        const auto error = ops.last_error();
        LIBXCP_LOG_ERROR("UDP bind failed local={}:{} error={}",
                         m_config_.local_host, m_config_.local_port, error);
        ops.close(impl->handle);
        throw detail::MakeTransportError(
            "绑定本地端点失败: " + m_config_.local_host + ":" +
                std::to_string(m_config_.local_port),
            error);
    }
    impl->bound = true;

    // ---- 记录实际绑定的本地端口（OS 分配临时端口时需回填，供诊断）----
    detail::Ipv4Endpoint actual_local{};
    if (ops.getsockname(impl->handle, &actual_local) == 0 &&
        m_config_.local_port == 0U) {
        m_config_.local_port = actual_local.port;
    }

    // ---- 远端端点 ----
    detail::Ipv4Endpoint remote{};
    remote.port = m_config_.remote_port;
    if (!detail::ParseIpv4Literal(m_config_.remote_host, &remote.address)) {
        LIBXCP_LOG_ERROR("UDP invalid remote address='{}'",
                         m_config_.remote_host);
        ops.close(impl->handle);
        throw detail::MakeInvalidArgument("无法解析远端地址: " +
                                          m_config_.remote_host);
    }

    // ---- 接收超时：使阻塞接收可周期性检查关闭标记，保证 Close() 及时生效 ----
    if (ops.set_receive_timeout(impl->handle,
                                m_config_.receive_poll_interval_ms) != 0) {
        // 先读取失败文本，关闭套接字可能覆盖平台最近错误。
        const std::string error = ops.last_error();
        LIBXCP_LOG_ERROR("UDP receive timeout setup failed: {}", error);
        ops.close(impl->handle);
        throw detail::MakeTransportError("设置 UDP 接收超时失败", error);
    }

    // 解析后的远端端点缓存到 SocketImpl，避免每次发送重复解析字符串。
    impl->remote = remote;

    m_socket_ = std::move(impl);
    m_listener_ = &listener;

    // ---- CTR 复位（设计决策 D1：每次 Open() 发送 CTR 置 0、清空接收基线）----
    m_send_ctr_.store(0, std::memory_order_relaxed);
    {
        const std::lock_guard<std::mutex> lock(m_recv_mutex_);
        m_last_recv_ctr_.reset();
        m_recv_baseline_established_ = false;
    }

    m_running_.store(true, std::memory_order_release);
    // 创建唤醒对：使 Close() 能立即打断阻塞中的接收线程，而不必等满一个
    // 轮询周期。创建失败不致命——ReceiveLoop 会用接收超时轮询兜底，
    // 退出延迟退化为一个轮询周期（即修复前的既有行为）。
    m_wakeup_ = CreateWakeupPair();
    if (m_wakeup_ == nullptr) {
        LIBXCP_LOG_WARNING(
            "UDP wakeup pair unavailable; Close() latency falls back to "
            "receive_poll_interval_ms={}ms",
            m_config_.receive_poll_interval_ms);
    }
    try {
        m_receive_thread_ = std::thread([this] { ReceiveLoop(); });
    } catch (const std::system_error& e) {
        LIBXCP_LOG_ERROR("UDP receive thread creation failed: {}", e.what());
        m_running_.store(false, std::memory_order_release);
        detail::CurrentSocketOps().close(m_socket_->handle);
        m_socket_.reset();
        m_listener_ = nullptr;
        throw detail::MakeTransportError("启动接收线程失败", e.what());
    }
    LIBXCP_LOG_INFO("UDP transport open local={}:{} remote={}:{}",
                    m_config_.local_host, m_config_.local_port,
                    m_config_.remote_host, m_config_.remote_port);
}

void UdpTransport::Close() {
    // 1. 通知接收线程退出
    m_running_.store(false, std::memory_order_release);

    // 2. 立即唤醒阻塞在 select/recvfrom 的接收线程，使其无需等满一个轮询
    //    周期即可观察到 m_running_==false。
    SignalWakeup(m_wakeup_.get());

    // 3. 等待接收线程结束（其退出前负责回调 OnTransportClosed）。
    //    唤醒失效时仍有接收超时轮询兜底，因此不再需要先关 Socket；
    //    反过来，若在 join 之前关闭 Socket，其句柄号会被 OS
    //    立即回收并可能分配给 进程中其他新建的
    //    Socket，而接收线程此刻仍可能在处理入站包并回调监听器，
    //    迟到的操作就会命中被复用的句柄（跨会话/跨用例串扰）。
    if (m_receive_thread_.joinable()) {
        m_receive_thread_.join();
    }

    // 4. 线程已停，此处关闭 Socket 不再存在竞态
    if (m_socket_ && m_socket_->handle != kInvalidSocket) {
        detail::CurrentSocketOps().close(m_socket_->handle);
        m_socket_->handle = kInvalidSocket;
    }
    // 5. 释放唤醒对（读端/写端均在此关闭）
    if (m_wakeup_ != nullptr) {
        if (m_wakeup_->read_handle != kInvalidSocket) {
            detail::CurrentSocketOps().close(m_wakeup_->read_handle);
            m_wakeup_->read_handle = kInvalidSocket;
        }
        if (m_wakeup_->write_handle != kInvalidSocket) {
            detail::CurrentSocketOps().close(m_wakeup_->write_handle);
            m_wakeup_->write_handle = kInvalidSocket;
        }
        m_wakeup_.reset();
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
        LIBXCP_LOG_ERROR("UDP send rejected because transport is closed");
        throw detail::MakeInvalidState("UdpTransport 未打开时调用 send()");
    }
    if (packet.empty()) {
        LIBXCP_LOG_ERROR("UDP send rejected because XCP Packet is empty");
        throw detail::MakeInvalidArgument("XCP Packet 不能为空");
    }
    if (packet.size() > m_config_.max_frame_packet_size) {
        LIBXCP_LOG_ERROR("UDP send rejected XCP packet bytes={} maximum={}",
                         packet.size(), m_config_.max_frame_packet_size);
        throw detail::MakeInvalidArgument(
            "XCP Packet 长度 " + std::to_string(packet.size()) +
            " 超过配置上限 max_frame_packet_size=" +
            std::to_string(m_config_.max_frame_packet_size));
    }

    // 取 CTR 并按模 65536 递增（每个 XCP Frame 消耗一个值，而非每个 Datagram）
    const auto ctr = m_send_ctr_.load(std::memory_order_relaxed);
    m_send_ctr_.store(static_cast<DatagramCtr>(ctr + 1U),
                      std::memory_order_relaxed);

    UdpFrame frame;
    try {
        // 主站发送**不补 Tail**：TL V1.5 §4.3.2.1 规定单个 XCP 消息独占
        // Frame 时 Tail 可选；省略后不再依赖对端预期的 PACKET_ALIGNMENT，
        // 也就无需在配置里强制给出对齐位宽。
        frame = EncodeUdpFrame(packet, ctr);
    } catch (...) {
        LIBXCP_LOG_ERROR("UDP frame encoding failed ctr={} xcp_bytes={}", ctr,
                         packet.size());
        // 编码失败不回退 CTR：保持"发送方向每 Frame 独立消耗一个
        // CTR"的可观察语义
        throw;
    }

    if (frame.data.size() > m_config_.max_datagram_size) {
        LIBXCP_LOG_ERROR("UDP send rejected datagram bytes={} maximum={}",
                         frame.data.size(), m_config_.max_datagram_size);
        throw detail::MakeInvalidArgument(
            "编码后 Datagram 长度 " + std::to_string(frame.data.size()) +
            " 超过配置上限 max_datagram_size=" +
            std::to_string(m_config_.max_datagram_size));
    }

    const int sent = detail::CurrentSocketOps().send_to(
        m_socket_->handle, frame.data.data(), frame.data.size(),
        m_socket_->remote);
    if (sent < 0) {
        const auto error = detail::CurrentSocketOps().last_error();
        LIBXCP_LOG_ERROR("UDP sendto failed: {}", error);
        throw detail::MakeTransportError("发送 UDP Datagram 失败", error);
    }
    if (static_cast<std::size_t>(sent) != frame.data.size()) {
        LIBXCP_LOG_ERROR("UDP partial datagram send sent={} expected={}", sent,
                         frame.data.size());
        throw detail::MakeTransportError(
            "UDP Datagram 部分发送: " + std::to_string(sent) + "/" +
            std::to_string(frame.data.size()));
    }
    // 仅在完整发送成功后记录 UDP 元信息，不输出 XCP/Datagram 载荷。
    LIBXCP_LOG_INFO("UDP TX datagram_bytes={} xcp_bytes={} ctr={}",
                    frame.data.size(), packet.size(), ctr);
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
        // ---- 用 EventPoller 同时等待数据 Socket 与唤醒 Socket ----
        // 唤醒 Socket 可读即表示 Close() 已请求退出，可立即返回；
        // 数据 Socket 可读则进入 recvfrom 取包。
        // 超时仍用 receive_poll_interval_ms 作为**兜底轮询**：万一唤醒字节
        // 丢失（极端资源耗尽下 send 失败），退出延迟仍有一个明确上界。
        const SocketHandle data_handle =
            (m_socket_ ? m_socket_->handle : kInvalidSocket);
        const SocketHandle wake_handle =
            (m_wakeup_ ? m_wakeup_->read_handle : kInvalidSocket);
        const bool has_data = data_handle != kInvalidSocket;
        const bool has_wake = wake_handle != kInvalidSocket;
        if (!has_data && !has_wake) {
            break;  // 无可用句柄，无从等待
        }

        // 按需组装等待项：两个句柄都可能缺失，索引必须动态确定
        std::array<detail::WaitItem, 2> items{};
        std::size_t item_count = 0;
        std::size_t data_index = 0;
        std::size_t wake_index = 0;
        if (has_data) {
            data_index = item_count;
            items[item_count++] = detail::WaitItem{data_handle, true, false};
        }
        if (has_wake) {
            wake_index = item_count;
            items[item_count++] = detail::WaitItem{wake_handle, true, false};
        }

        std::vector<detail::WaitStatus> status(item_count);
        std::string wait_error;
        const int ready = detail::CurrentEventPoller().Wait(
            std::span<const detail::WaitItem>(items.data(), item_count),
            m_config_.receive_poll_interval_ms, status, &wait_error);
        if (ready < 0) {
            if (!m_running_.load(std::memory_order_acquire)) {
                break;  // Close() 触发，正常退出
            }
            // 等待失败时直接退出，避免失效句柄或中断产生无限警告忙循环。
            if (listener != nullptr) {
                listener->OnTransportWarning("等待数据 Socket 失败: " +
                                             wait_error);
            }
            close_reason = "等待数据 Socket 失败: " + wait_error;
            break;
        }
        // ready == 0 表示兜底轮询超时，回到 while 重新检查关闭标记
        if (ready == 0) {
            continue;
        }

        // 失效句柄不再进入下一轮等待，以免轮询器重复立即报错。
        if ((has_data && status[data_index].failed) ||
            (has_wake && status[wake_index].failed)) {
            if (!m_running_.load(std::memory_order_acquire)) {
                break;
            }
            close_reason = "Socket 已关闭或失效，接收线程退出";
            break;
        }
        // 唤醒 Socket 可读：排空并检查关闭标记
        if (has_wake && status[wake_index].Ready()) {
            DrainWakeup(m_wakeup_.get());
            if (!m_running_.load(std::memory_order_acquire)) {
                break;  // Close() 触发，正常退出
            }
            // 非关闭场景的意外唤醒：继续等待（不视为错误）
            continue;
        }
        if (!has_data || !status[data_index].Ready()) {
            continue;  // 无数据可读（理论上不可达），回到等待
        }

        // ---- 数据 Socket 可读：取出一个 Datagram ----
        detail::Ipv4Endpoint src{};
        const int received = detail::CurrentSocketOps().recv_from(
            m_socket_ ? m_socket_->handle : kInvalidSocket, buffer.data(),
            buffer.size(), &src);

        if (received > 0) {
            const std::string ip_text = detail::FormatIpv4(src.address);
            LIBXCP_LOG_INFO("UDP RX datagram_bytes={} peer={}:{}", received,
                            ip_text, src.port);
            const bool keep_running = HandleDatagram(
                buffer.data(), static_cast<std::size_t>(received), ip_text,
                src.port);
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
            break;  // Close() 触发，正常退出
        }
        auto& ops = detail::CurrentSocketOps();
        // 在读取错误文本或调用监听器之前先分类，避免平台错误码被覆盖。
        const bool timed_out = ops.is_timeout();
        const bool invalid = ops.is_invalid_socket();
        const bool interrupted = ops.is_interrupted();
        const bool connection_lost = ops.is_connection_lost();
        const auto error = ops.last_error();
        if (timed_out) {
            continue;  // 轮询周期到，检查关闭标记
        }
        if (invalid || interrupted || connection_lost) {
            close_reason = "Socket 已关闭或失效，接收线程退出";
            break;
        }
        // 其它错误：报告警告并继续（避免瞬时错误终止会话）
        if (listener != nullptr) {
            listener->OnTransportWarning("recvfrom 失败: " + error);
        }
    }

    m_running_.store(false, std::memory_order_release);

    // 退出前统一交付关闭事件；此后不再有 OnPacketReceived 回调
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
    if (src_ip != m_config_.remote_host) {
        return false;
    }
    if (m_config_.strict_remote_port && src_port != m_config_.remote_port) {
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

    if (size > m_config_.max_datagram_size) {
        if (listener != nullptr) {
            listener->OnTransportWarning("Datagram 长度 " +
                                         std::to_string(size) +
                                         " 超过配置上限，已丢弃");
        }
        return true;
    }

    const BytesView view{data, size};
    // 以数据报边界为准解码：UDP 一个数据报就是一个承载单元，Frame 边界可
    // 直接观测，因此末帧的 0..3 字节 FILL 无需知道 PACKET_ALIGNMENT 即可
    // 跳过（纯函数见 DecodeEthernetDatagramByBoundary）。
    auto frames = DecodeEthernetDatagramByBoundary(view, 3);
    if (!frames) {
        // 原子性策略：任一 Frame 非法则整个 Datagram 不交付（§8.1 第 7 条）
        if (listener != nullptr) {
            listener->OnTransportWarning(
                "畸形 UDP Datagram（Header/LEN 越界或残留字节），整体丢弃");
        }
        return true;
    }
    // 配置里给了对齐时做一次一致性诊断（不阻断）：Tail 与预期不符说明对端
    // 用的对齐与协商结果不同，属于需要可见但不致命的偏差。
    if (m_config_.packet_alignment != PacketAlignment::Unknown) {
        for (const auto& frame : *frames) {
            const auto expected = TailBytesForAlignment(
                m_config_.packet_alignment,
                kEthernetHeaderSize +
                    static_cast<std::size_t>(frame.header.len));
            if (frame.tail_bytes != expected && listener != nullptr) {
                listener->OnTransportWarning(
                    "UDP Frame 的 Tail 与协商对齐不一致（期望 " +
                    std::to_string(expected) + " 字节，实得 " +
                    std::to_string(frame.tail_bytes) +
                    " 字节）；已按数据报边界接受");
            }
        }
    }

    for (const auto& frame : *frames) {
        if (frame.header.len > m_config_.max_frame_packet_size) {
            if (listener != nullptr) {
                listener->OnTransportWarning("Frame 内 XCP Packet 长度 " +
                                             std::to_string(frame.header.len) +
                                             " 超过配置上限，该 Frame 已丢弃");
            }
            continue;
        }
        if (!HandleFrame(frame)) {
            continue;  // CTR 规则丢弃
        }
        // XCP 内容由 CommandExecutor 按敏感信息规则记录；UDP 层仅记录帧元信息。
        LIBXCP_LOG_INFO("UDP RX frame ctr={} packet_bytes={}", frame.header.ctr,
                        frame.xcp_packet.size());
        if (listener != nullptr) {
            try {
                listener->OnPacketReceived(frame.xcp_packet);
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
        m_last_recv_ctr_ = frame.header.ctr;
        return true;
    }

    const DatagramCtr previous = m_last_recv_ctr_.value_or(0U);
    const DatagramCtr expected = static_cast<DatagramCtr>(previous + 1U);

    if (frame.header.ctr == expected) {
        m_last_recv_ctr_ = frame.header.ctr;  // §8.1 第 3 条
        return true;
    }

    // 以期望值为基准计算前向距离：0 表示重复，1..32767 表示前向跳号（缺包），
    // >= 32768 表示后向乱序或方向歧义。
    const int delta_from_expected =
        ctrForwardDistance(expected, frame.header.ctr);

    if (delta_from_expected == 0) {
        // 与期望值相同不可能走到这里（上面已判等），保留为防御分支
        m_last_recv_ctr_ = frame.header.ctr;
        return true;
    }
    if (delta_from_expected == 0x8000) {
        // 恰好相差 0x8000：方向歧义，丢弃（§8.1 第 6 条 / D5）
        // 诊断文本中的差值使用十六进制，便于与 §8.1 条款和测试断言逐字对应。
        if (listener != nullptr) {
            listener->OnTransportWarning(
                "CTR 与期望值差 0x8000，方向歧义，已丢弃: " +
                std::to_string(frame.header.ctr));
        }
        return false;
    }
    if (delta_from_expected > 0 && delta_from_expected < 0x8000) {
        // 前向跳号：接收、报告缺口、推进（§8.1 第 4 条）
        if (listener != nullptr) {
            listener->OnTransportWarning(
                "CTR 前向跳号，疑似缺包：期望 " + ctrToHex(expected) +
                "，实收 " + ctrToHex(frame.header.ctr) + "，缺口 " +
                std::to_string(delta_from_expected));
        }
        m_last_recv_ctr_ = frame.header.ctr;
        return true;
    }

    // 后向（distance 落在 0x8001..0xFFFF）：重复或迟到乱序，丢弃（§8.1 第 5
    // 条）
    if (listener != nullptr) {
        listener->OnTransportWarning(
            "丢弃重复/后向乱序 CTR: " + ctrToHex(frame.header.ctr) +
            "，最近值 " + ctrToHex(previous));
    }
    return false;
}

}  // namespace calmcar::xcp
