/**
 * @file tcp_transport.cpp
 * @brief `TcpTransport` 实现：跨平台 IPv4 TCP Socket、连接建立与字节流解帧。
 *
 * 依据 code-plan/TCP传输_实施计划_B-T1至B-T5.md 第 5 节实现（B-T2）。
 *
 * @par 平台差异的处理方式（与 `udp_transport.cpp` 不同的选择）
 *   `udp_transport.cpp` 把平台适配直接写在文件顶部（一组自由函数 + `#if`）。
 *   本文件改用 `src/socket_util.hpp` 的 `SocketOps` 抽象，原因见该文件注释：
 *   TCP 的错误分支（短写、EINTR、EAGAIN、发送超时）**必须被确定性测试**，
 *   而真实 Socket 无法稳定制造这些条件。引入一层可替换函数表后，生产路径
 *   仍是零开销的直接调用，测试路径可以精确驱动每一条分支。
 */

#include "libxcp/tcp_transport.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "libxcp/log.hpp"

#include "socket_util.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

namespace calmcar::xcp {
namespace {

/// @brief 收包转发给监听器前使用的关闭原因上限（避免超长诊断文本）
constexpr std::size_t kMaxCloseReasonBytes = 512;

/**
 * @brief 把可写等待结果翻译为明确的发送超时异常
 * @param pending 尚未写出的字节数
 * @param timeout_ms 配置的发送超时
 * @return 可直接抛出的 TransportError
 */
XcpException MakeSendTimeoutError(std::size_t pending,
                                  std::uint32_t timeout_ms) {
    return detail::MakeTransportError(
        "TCP 发送超时：等待可写 " + std::to_string(timeout_ms) + " ms 后仍有 " +
        std::to_string(pending) + " 字节未写出（对端可能已停止读取）");
}

/// @brief 去掉诊断文本中的换行，避免日志按行伪造
std::string SanitizeReason(std::string reason) {
    if (reason.size() > kMaxCloseReasonBytes) {
        reason.resize(kMaxCloseReasonBytes);
        reason += "...";
    }
    for (char& ch : reason) {
        if (ch == '\n' || ch == '\r') {
            ch = ' ';
        }
    }
    return reason;
}

/**
 * @brief 模 65536 的前向距离：从 base 前进多少步到达 value
 * @return 0..65535；`value == base` 时为 0
 * @details 与 `src/udp_transport.cpp:152` 的 `ctrForwardDistance` 算法一致，
 *          但两者各自在匿名命名空间内，无法跨 TU 复用。此处按 TCP 侧策略
 *          重新实现：TCP 需区分"前向缺号（V14 容忍）"与"后向重复（一律
 *          断链）"，而 UDP 是"丢弃帧但不关连接"，语义不同，故不做共享抽象。
 */
int CtrForwardDistance(DatagramCtr base, DatagramCtr value) noexcept {
    return static_cast<int>(
        (static_cast<unsigned int>(value) - static_cast<unsigned int>(base)) &
        0xFFFFU);
}

/// @brief 将 CTR 格式化为 4 位十六进制（用于诊断文本，便于与 §1.5 条款对照）
std::string CtrToHex(DatagramCtr value) {
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
// SocketImpl：拥有已连接句柄与本地端点信息
// ---------------------------------------------------------------------------
struct TcpTransport::SocketImpl {
    /// @brief 已建立连接的 Socket 句柄（Slave 角色下为 accept 返回的句柄）
    detail::SocketHandle handle{detail::kInvalidSocketHandle};
    /// @brief 监听 Socket 句柄（仅 Slave 角色使用；accept 后不再需要）
    detail::SocketHandle listen_handle{detail::kInvalidSocketHandle};
    /// @brief 实际绑定的本地端点（用于诊断与 OS 分配端口回填）
    sockaddr_in bound_local{};
};

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

TcpTransport::TcpTransport(TcpTransportConfig config)
    : m_config_(std::move(config)) {}

TcpTransport::~TcpTransport() {
    // 析构路径不允许抛异常；Close() 内部已吞掉回调异常
    try {
        Close();
    } catch (...) {
    }
}

// ---------------------------------------------------------------------------
// Open / Close
// ---------------------------------------------------------------------------

void TcpTransport::Open(IPacketListener& listener) {
    Close();  // 幂等：允许在已打开的实例上重新打开

    auto& ops = detail::RealSocketOps();

    // ---- 配置校验（计划 §5.8）----
    // 校验顺序刻意与 UdpTransport 保持一致：先是"必须显式给出"的协议属性，
    // 再是数值范围，最后是能力边界，使错误文本对调用方稳定可预期。
    if (!IsTcpWireProfileResolved(m_config_.wire_profile)) {
        LIBXCP_LOG_ERROR("TCP open rejected because wire profile is unknown");
        throw detail::MakeInvalidArgument(
            "TcpTransportConfig::wire_profile 不能为 Unknown：TCP 线路布局"
            "必须显式选择（AsamV15 或 AsamV14），本库禁止自动推断");
    }
    if (m_config_.packet_alignment == PacketAlignment::Unknown) {
        LIBXCP_LOG_ERROR("TCP open rejected because packet alignment unknown");
        throw detail::MakeInvalidArgument(
            "TcpTransportConfig::packet_alignment 不能为 Unknown："
            "需要 A2L 的 PACKET_ALIGNMENT 或调用方显式指定 8/16/32");
    }
    if (m_config_.role == TcpRole::Master) {
        if (m_config_.remote_port == 0U) {
            LIBXCP_LOG_ERROR("TCP open rejected because remote_port is zero");
            throw detail::MakeInvalidArgument(
                "TcpTransportConfig::remote_port 不能为 0");
        }
        if (m_config_.remote_host.empty()) {
            LIBXCP_LOG_ERROR("TCP open rejected because remote_host is empty");
            throw detail::MakeInvalidArgument(
                "TcpTransportConfig::remote_host 不能为空");
        }
    }
    if (m_config_.connect_timeout_ms == 0U) {
        LIBXCP_LOG_ERROR("TCP open rejected because connect timeout is zero");
        throw detail::MakeInvalidArgument("connect_timeout_ms 必须大于 0");
    }
    if (m_config_.send_timeout_ms == 0U) {
        LIBXCP_LOG_ERROR("TCP open rejected because send timeout is zero");
        throw detail::MakeInvalidArgument("send_timeout_ms 必须大于 0");
    }
    if (m_config_.receive_chunk_size == 0U) {
        LIBXCP_LOG_ERROR("TCP open rejected because receive_chunk_size is zero");
        throw detail::MakeInvalidArgument("receive_chunk_size 必须大于 0");
    }
    if (m_config_.max_frame_packet_size == 0U ||
        m_config_.max_frame_packet_size > kEthernetMaxXcpPacket) {
        LIBXCP_LOG_ERROR(
            "TCP open rejected max_frame_packet_size={} maximum={}",
            m_config_.max_frame_packet_size, kEthernetMaxXcpPacket);
        throw detail::MakeInvalidArgument(
            "max_frame_packet_size 必须在 1.." +
            std::to_string(kEthernetMaxXcpPacket) + " 之间");
    }
    if (m_config_.max_packet_size == 0U ||
        m_config_.max_packet_size > kEthernetMaxXcpPacket) {
        LIBXCP_LOG_ERROR("TCP open rejected max_packet_size={} maximum={}",
                         m_config_.max_packet_size, kEthernetMaxXcpPacket);
        throw detail::MakeInvalidArgument(
            "max_packet_size 必须在 1.." +
            std::to_string(kEthernetMaxXcpPacket) + " 之间");
    }
    // 能力边界：本库只承诺 IPv4 TCP 单播；IPv6 必须显式报 Unsupported，
    // 不得退化为"地址非法"（与 UdpTransport 的 T-06 处理一致）
    if (detail::LooksLikeIpv6(m_config_.remote_host)) {
        LIBXCP_LOG_ERROR("TCP open rejected: IPv6 remote unsupported host={}",
                         m_config_.remote_host);
        throw detail::MakeUnsupportedFeature(
            "IPv6 TCP 传输尚未实现（本版本基础 Profile 仅承诺 IPv4 TCP "
            "单播）：remote_host=" + m_config_.remote_host);
    }
    if (detail::LooksLikeIpv6(m_config_.local_host)) {
        LIBXCP_LOG_ERROR("TCP open rejected: IPv6 local unsupported host={}",
                         m_config_.local_host);
        throw detail::MakeUnsupportedFeature(
            "IPv6 TCP 传输尚未实现（本版本基础 Profile 仅承诺 IPv4 TCP "
            "单播）：local_host=" + m_config_.local_host);
    }

    // ---- 解码器：构建期即校验，把配置错误挡在连接建立之前 ----
    // max_packet_size 与 max_frame_packet_size 取两者较小值：decoder 的职责是
    // 防止缓冲无界增长，必须遵守调用方给出的**任意一个**更严格上限。
    // 用括号包住 `std::min` 是刻意的：Windows 头会在未定义 NOMINMAX 时注入
    // `min` 宏，`std::min(a, b)` 会被预处理成 `std::((a)<(b)?...)` 而报
    // C2589/C2059；`(std::min)(a, b)` 不满足函数式宏的展开条件，两个平台通吃。
    const std::size_t decoder_limit =
        (std::min)(m_config_.max_packet_size, m_config_.max_frame_packet_size);
    m_decoder_ = std::make_unique<TcpStreamDecoder>(
        m_config_.wire_profile, m_config_.packet_alignment, decoder_limit);

    // ---- 创建 Socket 并建立连接 ----
    auto impl = std::make_unique<SocketImpl>();
    impl->handle = ops.create(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl->handle == detail::kInvalidSocketHandle) {
        const auto error = ops.last_error();
        LIBXCP_LOG_ERROR("TCP socket creation failed: {}", error);
        m_decoder_.reset();
        throw detail::MakeTransportError("创建 TCP Socket 失败", error);
    }
    m_socket_ = std::move(impl);

    try {
        EstablishConnection();
    } catch (...) {
        // 连接失败时不留半开状态：句柄与 decoder 一并释放，调用方可直接重试
        if (m_socket_ != nullptr) {
            if (m_socket_->handle != detail::kInvalidSocketHandle) {
                ops.close(m_socket_->handle);
                m_socket_->handle = detail::kInvalidSocketHandle;
            }
            if (m_socket_->listen_handle != detail::kInvalidSocketHandle) {
                ops.close(m_socket_->listen_handle);
                m_socket_->listen_handle = detail::kInvalidSocketHandle;
            }
        }
        m_socket_.reset();
        m_decoder_.reset();
        throw;
    }

    m_listener_ = &listener;
    m_send_ctr_.store(0, std::memory_order_relaxed);
    m_delivered_frames_.store(0, std::memory_order_relaxed);
    // 接收 CTR 基线随每次 Open() 清空（计划 §1.5 共同规则）：否则会用上一条
    // 连接的最后 CTR 去校验新连接的首帧，导致重连后首帧被误判为违约。
    // 与发送 CTR 的复位保持一致——两者都是"每次成功 Open 后重新开始"。
    m_last_recv_ctr_.reset();
    m_recv_baseline_established_ = false;

    m_running_.store(true, std::memory_order_release);
    try {
        m_receive_thread_ = std::thread([this] { ReceiveLoop(); });
    } catch (const std::system_error& e) {
        LIBXCP_LOG_ERROR("TCP receive thread creation failed: {}", e.what());
        m_running_.store(false, std::memory_order_release);
        const auto handle = m_socket_->handle;
        if (handle != detail::kInvalidSocketHandle) {
            ops.close(handle);
            m_socket_->handle = detail::kInvalidSocketHandle;
        }
        m_socket_.reset();
        m_decoder_.reset();
        m_listener_ = nullptr;
        throw detail::MakeTransportError("启动接收线程失败", e.what());
    }
    LIBXCP_LOG_INFO(
        "TCP transport open profile={} local={}:{} remote={}:{} role={}",
        TcpWireProfileName(m_config_.wire_profile), m_config_.local_host,
        m_bound_local_port_.load(std::memory_order_relaxed),
        m_config_.remote_host, m_config_.remote_port,
        m_config_.role == TcpRole::Master ? "Master" : "Slave");
}

void TcpTransport::EstablishConnection() {
    auto& ops = detail::RealSocketOps();

    // ---- 解析本地绑定地址 ----
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(m_config_.local_port);
    const std::string local_text = m_config_.local_host.empty()
                                       ? std::string("0.0.0.0")
                                       : m_config_.local_host;
    if (!detail::ParseIpv4Literal(local_text, &local.sin_addr)) {
        LIBXCP_LOG_ERROR("TCP invalid local bind address='{}'", local_text);
        throw detail::MakeInvalidArgument("无法解析本地绑定地址: " +
                                          m_config_.local_host);
    }

    // Service 角色的绑定/监听结果错误文本不同，故分开处理
    const bool is_master = (m_config_.role == TcpRole::Master);

    // Master 角色若不要求固定源端口，则不显式 bind——让内核在同一次 connect
    // 中选好源地址与端口，避免额外一次系统调用与潜在的端口冲突。
    // 但若调用方指定了 local_host 或 local_port，则必须显式绑定。
    const bool needs_explicit_bind =
        !is_master || m_config_.local_port != 0U || !m_config_.local_host.empty();

    if (needs_explicit_bind) {
        if (m_config_.reuse_address) {
            if (ops.set_int_option(m_socket_->handle, SOL_SOCKET, SO_REUSEADDR,
                                   1) != 0) {
                const auto error = ops.last_error();
                LIBXCP_LOG_WARNING("TCP SO_REUSEADDR setup failed: {}", error);
                // 非致命：仅影响 TIME_WAIT 期间的重绑定，继续尝试绑定
            }
        }
        if (ops.bind(m_socket_->handle, reinterpret_cast<const sockaddr*>(&local),
                     sizeof(local)) != 0) {
            const auto error = ops.last_error();
            LIBXCP_LOG_ERROR("TCP bind failed local={}:{} error={}", local_text,
                             m_config_.local_port, error);
            throw detail::MakeTransportError(
                "绑定本地端点失败: " + local_text + ":" +
                    std::to_string(m_config_.local_port),
                error);
        }
    }

    // ---- 回填实际绑定的本地端口（local_port == 0 时由 OS 分配）----
    sockaddr_in actual{};
    socklen_t actual_len = sizeof(actual);
    if (ops.getsockname(m_socket_->handle, reinterpret_cast<sockaddr*>(&actual),
                        &actual_len) == 0) {
        m_socket_->bound_local = actual;
        m_bound_local_port_.store(ntohs(actual.sin_port),
                                  std::memory_order_release);
    }

    if (is_master) {
        sockaddr_in remote{};
        remote.sin_family = AF_INET;
        remote.sin_port = htons(m_config_.remote_port);
        if (!detail::ParseIpv4Literal(m_config_.remote_host,
                                      &remote.sin_addr)) {
            LIBXCP_LOG_ERROR("TCP invalid remote address='{}'",
                             m_config_.remote_host);
            throw detail::MakeInvalidArgument(
                "无法解析远端地址: " + m_config_.remote_host);
        }
        ConnectWithTimeout(&remote, sizeof(remote));
    } else {
        ListenAndAccept();
    }

    // ---- 连接建立后的通用选项 ----
    // TCP_NODELAY：XCP 的命令-响应是"请求-应答"式的小报文交互，Nagle 算法会把
    // 小包攒起来等 ACK，直接给每一条命令增加一个 RTT 的额外延迟。XCP 的 DAQ
    // 上报也不依赖 Nagle 合并（我们会自行按 Frame 组织写入），故关闭它。
    // 失败仅告警：没有它就变慢，但不影响正确性。
    if (ops.set_int_option(m_socket_->handle, IPPROTO_TCP, TCP_NODELAY, 1) !=
        0) {
        LIBXCP_LOG_WARNING("TCP_NODELAY setup failed: {}",
                           ops.last_error());
    }
    if (m_config_.keep_alive) {
        if (ops.set_int_option(m_socket_->handle, SOL_SOCKET, SO_KEEPALIVE, 1) !=
            0) {
            LIBXCP_LOG_WARNING("TCP SO_KEEPALIVE setup failed: {}",
                               ops.last_error());
        }
    }
}

void TcpTransport::ConnectWithTimeout(const void* addr,
                                      std::size_t addr_len) {
    auto& ops = detail::RealSocketOps();

    // 参数经类型擦除传入（见头文件说明），这里恢复回平台类型。
    // 调用方保证指向有效的 sockaddr_in 且长度匹配，故无需再做校验。
    const auto* sa = static_cast<const sockaddr*>(addr);
    const auto sa_len = static_cast<socklen_t>(addr_len);

#if !defined(_WIN32) && defined(SO_NOSIGPIPE)
    // macOS/BSD：没有 MSG_NOSIGNAL，改用 SO_NOSIGPIPE 选项。必须在连接前设置，
    // 否则对端提前关闭时的 send 会向进程投递 SIGPIPE 并默认终止进程。
    if (ops.set_int_option(m_socket_->handle, SOL_SOCKET, SO_NOSIGPIPE, 1) !=
        0) {
        LIBXCP_LOG_WARNING("TCP SO_NOSIGPIPE setup failed: {}",
                           ops.last_error());
    }
#endif

    // 非阻塞 connect + 可写等待：这样 connect 的等待上界由我们控制。
    // 直接对阻塞 Socket 调用 connect 的等待时间由内核决定（Linux 上默认约
    // 2 分钟），期间既无法响应 Close() 也无法给出明确失败原因。
    if (ops.set_non_blocking(m_socket_->handle) != 0) {
        const auto error = ops.last_error();
        LIBXCP_LOG_ERROR("TCP set nonblocking failed: {}", error);
        throw detail::MakeTransportError("设置 TCP 非阻塞失败", error);
    }

    if (ops.connect(m_socket_->handle, sa, sa_len) != 0) {
        if (!ops.is_in_progress() && !ops.is_would_block()) {
            const auto error = ops.last_error();
            LIBXCP_LOG_ERROR("TCP connect failed: {}", error);
            throw detail::MakeTransportError(
                "连接远端失败: " + m_config_.remote_host + ":" +
                    std::to_string(m_config_.remote_port),
                error);
        }

        // 连接正在建立：等待可写（含超时）
        const int ready =
            ops.wait_writable(m_socket_->handle, m_config_.connect_timeout_ms);
        if (ready == 0) {
            LIBXCP_LOG_ERROR("TCP connect timeout after {} ms",
                             m_config_.connect_timeout_ms);
            throw detail::MakeTransportError(
                "连接远端超时: " + m_config_.remote_host + ":" +
                    std::to_string(m_config_.remote_port) + "（" +
                    std::to_string(m_config_.connect_timeout_ms) + " ms）");
        }
        if (ready < 0) {
            const auto error = ops.last_error();
            LIBXCP_LOG_ERROR("TCP connect wait failed: {}", error);
            throw detail::MakeTransportError("等待 TCP 连接建立失败", error);
        }
    }

    // 可写只说明"有了结果"，必须读 SO_ERROR 才知道是成功还是失败
    int so_error = 0;
    if (ops.get_so_error(m_socket_->handle, &so_error) != 0) {
        const auto error = ops.last_error();
        LIBXCP_LOG_ERROR("TCP SO_ERROR query failed: {}", error);
        throw detail::MakeTransportError("查询 TCP 连接结果失败", error);
    }
    if (so_error != 0) {
        LIBXCP_LOG_ERROR("TCP connect rejected so_error={}", so_error);
        throw detail::MakeTransportError(
            "连接远端失败: " + m_config_.remote_host + ":" +
                std::to_string(m_config_.remote_port),
            "SO_ERROR=" + std::to_string(so_error));
    }

    // 恢复阻塞模式：发送路径依赖"可写等待 + 阻塞 send"的组合，
    // 若保持非阻塞，send 会在缓冲满时返回 EAGAIN 而让 WriteAll 空转。
    if (ops.set_blocking(m_socket_->handle) != 0) {
        const auto error = ops.last_error();
        LIBXCP_LOG_ERROR("TCP restore blocking failed: {}", error);
        throw detail::MakeTransportError("恢复 TCP 阻塞模式失败", error);
    }
}

void TcpTransport::ListenAndAccept() {
    auto& ops = detail::RealSocketOps();

    if (ops.set_int_option(m_socket_->handle, SOL_SOCKET, SO_REUSEADDR, 1) !=
        0) {
        LIBXCP_LOG_WARNING("TCP listen SO_REUSEADDR setup failed: {}",
                           ops.last_error());
    }
    // backlog 取 1：XCP on TCP 是点对点协议，一个 Slave 端口只服务一条连接。
    // 允许多个排队连接只会让"连上却被静默忽略"的客户端更难诊断。
    if (ops.listen(m_socket_->handle, 1) != 0) {
        const auto error = ops.last_error();
        LIBXCP_LOG_ERROR("TCP listen failed: {}", error);
        throw detail::MakeTransportError(
            "监听本地端口失败: " + std::to_string(m_config_.local_port),
            error);
    }

    if (ops.set_non_blocking(m_socket_->handle) != 0) {
        const auto error = ops.last_error();
        LIBXCP_LOG_ERROR("TCP accept set nonblocking failed: {}", error);
        throw detail::MakeTransportError("设置 TCP 非阻塞失败", error);
    }

    const int ready =
        ops.wait_readable(m_socket_->handle, m_config_.connect_timeout_ms);
    if (ready == 0) {
        LIBXCP_LOG_ERROR("TCP accept timeout after {} ms",
                         m_config_.connect_timeout_ms);
        throw detail::MakeTransportError(
            "等待入站连接超时（" + std::to_string(m_config_.connect_timeout_ms) +
            " ms）");
    }
    if (ready < 0) {
        const auto error = ops.last_error();
        LIBXCP_LOG_ERROR("TCP accept wait failed: {}", error);
        throw detail::MakeTransportError("等待入站连接失败", error);
    }

    sockaddr_in peer{};
    socklen_t peer_len = sizeof(peer);
    const detail::SocketHandle accepted =
        ops.accept(m_socket_->handle, reinterpret_cast<sockaddr*>(&peer),
                   &peer_len);
    if (accepted == detail::kInvalidSocketHandle) {
        const auto error = ops.last_error();
        LIBXCP_LOG_ERROR("TCP accept failed: {}", error);
        throw detail::MakeTransportError("接受入站连接失败", error);
    }

    // 监听句柄已完成使命：连接已建立，后续收发包一律走 accepted。
    ops.close(m_socket_->handle);
    m_socket_->listen_handle = m_socket_->handle;
    m_socket_->handle = accepted;
    m_socket_->bound_local = peer;

    if (ops.set_blocking(m_socket_->handle) != 0) {
        const auto error = ops.last_error();
        LIBXCP_LOG_ERROR("TCP accepted restore blocking failed: {}", error);
        throw detail::MakeTransportError("恢复 TCP 阻塞模式失败", error);
    }
}

void TcpTransport::Close() {
    auto& ops = detail::RealSocketOps();

    // 1. 通知接收线程退出
    m_running_.store(false, std::memory_order_release);

    // 2. 立即打断阻塞中的 recv()。shutdown 不释放句柄号，因此不存在"句柄被
    //    OS 回收后分配给本进程新建 Socket"的复用风险（对比：直接 close 会有
    //    该风险，而接收线程此刻仍可能在使用这个句柄）。
    //    对端会因此观察到 FIN——这正是我们想要的：连接确实断了，而不是
    //    悄悄地不再读取。
    if (m_socket_ != nullptr &&
        m_socket_->handle != detail::kInvalidSocketHandle) {
        (void)ops.shutdown_socket(m_socket_->handle);
    }

    // 3. 等待接收线程结束（其退出前负责回调 OnTransportClosed）
    if (m_receive_thread_.joinable()) {
        m_receive_thread_.join();
    }

    // 4. 线程已停，此处关闭句柄不再存在竞态
    if (m_socket_ != nullptr) {
        if (m_socket_->handle != detail::kInvalidSocketHandle) {
            ops.close(m_socket_->handle);
            m_socket_->handle = detail::kInvalidSocketHandle;
        }
        if (m_socket_->listen_handle != detail::kInvalidSocketHandle) {
            ops.close(m_socket_->listen_handle);
            m_socket_->listen_handle = detail::kInvalidSocketHandle;
        }
    }
    m_socket_.reset();
    // 5. 解码器随之释放：重连时会重新构造，保证不携带上一条连接的状态
    m_decoder_.reset();
    m_listener_ = nullptr;
    m_bound_local_port_.store(0, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// 发送
// ---------------------------------------------------------------------------

void TcpTransport::Send(BytesView packet) {
    std::lock_guard<std::mutex> lock(m_send_mutex_);

    if (!IsOpen()) {
        LIBXCP_LOG_ERROR("TCP send rejected because transport is closed");
        throw detail::MakeInvalidState("TcpTransport 未打开时调用 send()");
    }
    if (packet.empty()) {
        LIBXCP_LOG_ERROR("TCP send rejected because XCP Packet is empty");
        throw detail::MakeInvalidArgument("XCP Packet 不能为空");
    }
    if (packet.size() > m_config_.max_packet_size) {
        LIBXCP_LOG_ERROR("TCP send rejected XCP packet bytes={} maximum={}",
                         packet.size(), m_config_.max_packet_size);
        throw detail::MakeInvalidArgument(
            "XCP Packet 长度 " + std::to_string(packet.size()) +
            " 超过配置上限 max_packet_size=" +
            std::to_string(m_config_.max_packet_size));
    }

    // CTR 在同一把锁下取值并递增：CTR 分配顺序必须与写入顺序严格一致，
    // 否则对端会看到乱序 CTR 并（按 §8.1 等价规则）丢弃我们的帧。
    const auto ctr = m_send_ctr_.load(std::memory_order_relaxed);
    m_send_ctr_.store(static_cast<DatagramCtr>(ctr + 1U),
                      std::memory_order_relaxed);

    if (packet.size() > m_config_.max_frame_packet_size) {
        LIBXCP_LOG_ERROR("TCP send rejected XCP packet bytes={} frame max={}",
                         packet.size(), m_config_.max_frame_packet_size);
        throw detail::MakeInvalidArgument(
            "XCP Packet 长度 " + std::to_string(packet.size()) +
            " 超过配置上限 max_frame_packet_size=" +
            std::to_string(m_config_.max_frame_packet_size));
    }

    WriteFrame(packet, ctr);
}

void TcpTransport::WriteFrame(BytesView packet, DatagramCtr ctr) {
    // 依据 Wire Profile 选择编码器：这是"显式配置驱动行为"的落点，
    // 不允许任何形式的内容嗅探。
    EthernetFrame frame;
    if (m_config_.wire_profile == TcpWireProfile::AsamV14) {
        frame = EncodeEthernetFrameUnpadded(packet, ctr);
    } else {
        frame = EncodeEthernetFrameAligned(packet, ctr,
                                           m_config_.packet_alignment);
    }

    WriteAll(frame.data.data(), frame.data.size());
    LIBXCP_LOG_INFO("TCP TX frame_bytes={} xcp_bytes={} ctr={} tail={}",
                    frame.data.size(), packet.size(), ctr, frame.tail_bytes);
}

void TcpTransport::WriteAll(const std::uint8_t* data, std::size_t size) {
    auto& ops = detail::RealSocketOps();
    std::size_t written = 0;

    while (written < size && m_running_.load(std::memory_order_acquire)) {
        const int sent = ops.send(m_socket_->handle, data + written,
                                  size - written, /*no_signal=*/true);
        if (sent > 0) {
            written += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent == 0) {
            // TCP 的 send 返回 0 不是"对端关闭"的正常表达（那会是 ECONNRESET
            // 或 EPIPE），按异常处理以免死循环。
            throw detail::MakeTransportError("TCP 发送返回 0 字节，连接状态异常");
        }

        // sent < 0
        if (ops.is_interrupted()) {
            continue;  // 被信号打断：立即重试，不计入超时
        }
        if (ops.is_connection_lost()) {
            const auto error = ops.last_error();
            LIBXCP_LOG_ERROR("TCP send failed: connection lost: {}", error);
            throw detail::MakeTransportError("TCP 发送失败：连接已断开", error);
        }
        if (ops.is_would_block()) {
            // 内核发送缓冲已满（对端停止读取）。等待可写，给出明确超时上界，
            // 而不是让调用线程无限期挂住。
            const int ready =
                ops.wait_writable(m_socket_->handle, m_config_.send_timeout_ms);
            if (ready > 0) {
                continue;
            }
            if (ready == 0) {
                LIBXCP_LOG_ERROR("TCP send timeout pending_bytes={} timeout={}",
                                 size - written, m_config_.send_timeout_ms);
                throw MakeSendTimeoutError(size - written,
                                           m_config_.send_timeout_ms);
            }
            const auto error = ops.last_error();
            LIBXCP_LOG_ERROR("TCP send wait failed: {}", error);
            throw detail::MakeTransportError("等待 TCP 可写失败", error);
        }
        // 其它错误：一律作为发送失败上报（不静默重试）
        const auto error = ops.last_error();
        LIBXCP_LOG_ERROR("TCP send failed: {}", error);
        throw detail::MakeTransportError("TCP 发送失败", error);
    }

    if (written < size) {
        // 走到这里说明 m_running_ 在发送过程中被置否：Close() 与本 Send()
        // 并发。这不是对端故障，按"通道已关闭"上报，避免调用方触发恢复。
        LIBXCP_LOG_ERROR("TCP send aborted by close pending_bytes={}",
                         size - written);
        throw detail::MakeInvalidState(
            "TCP 发送被 Close() 中断：" + std::to_string(size - written) +
            " 字节未写出");
    }
}

// ---------------------------------------------------------------------------
// 状态查询
// ---------------------------------------------------------------------------

bool TcpTransport::IsOpen() const noexcept {
    return m_running_.load(std::memory_order_acquire);
}

DatagramCtr TcpTransport::SendCtr() const noexcept {
    return m_send_ctr_.load(std::memory_order_relaxed);
}

std::uint16_t TcpTransport::LocalPort() const noexcept {
    return m_bound_local_port_.load(std::memory_order_acquire);
}

std::uint64_t TcpTransport::DeliveredFrameCount() const noexcept {
    return m_delivered_frames_.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// 接收线程
// ---------------------------------------------------------------------------

void TcpTransport::ReceiveLoop() {
    auto& ops = detail::RealSocketOps();
    IPacketListener* listener = m_listener_;
    std::string close_reason = "Transport 正常关闭";
    std::vector<std::uint8_t> buffer(m_config_.receive_chunk_size);

    while (m_running_.load(std::memory_order_acquire)) {
        const int received = ops.recv(m_socket_->handle, buffer.data(),
                                      buffer.size());

        if (received > 0) {
            LIBXCP_LOG_INFO("TCP RX bytes={}", received);
            // 字节流：把本次收到的全部字节交给状态机，由它决定能解出几帧。
            // Push() 返回的视图指向 decoder 内部缓冲，必须在下次 Push 前消费完。
            const TcpDecodeResult result =
                m_decoder_->Push(BytesView{buffer.data(),
                                           static_cast<std::size_t>(received)});
            if (!DispatchDecodeResult(result)) {
                // 失败原因可能是流畸形，也可能是 CTR 违约；两者都属"流已不可
                // 信"。畸形时 result.malformed_reason 有文本，CTR 违约时为空，
                // 此时退化为通用描述——具体 CTR 诊断已由 OnTransportWarning
                // 在 DispatchDecodeResult 内单独上报，不会丢失信息。
                close_reason = SanitizeReason(
                    "TCP 流不可恢复畸形，连接已终止：" +
                    (result.malformed_reason.empty()
                         ? std::string("接收 CTR 违约（详见此前的 warning）")
                         : result.malformed_reason));
                break;
            }
            continue;
        }

        if (received == 0) {
            // 对端正常关闭（FIN）。先让 decoder 做收尾校验：若最后还有一个
            // payload 完整、末尾 Tail 可选省略的帧，它应当仍被交付出去，
            // 而不是被当成"半截帧"丢掉。
            const TcpDecodeResult tail = m_decoder_->Finish();
            if (tail.malformed) {
                close_reason = SanitizeReason(
                    "TCP 连接结束时存在截断帧：" +
                    (tail.malformed_reason.empty()
                         ? std::string("未提供原因")
                         : tail.malformed_reason));
            } else if (listener != nullptr) {
                for (const auto& frame : tail.frames) {
                    // Finish() 交付的帧同样要通知监听器，否则流末整帧丢失
                    try {
                        listener->OnPacketReceived(frame.xcp_packet);
                        m_delivered_frames_.fetch_add(
                            1, std::memory_order_relaxed);
                    } catch (...) {
                        // 监听器异常不得终止接收线程
                    }
                }
            }
            if (!tail.malformed) {
                close_reason = "对端已关闭连接（EOF）";
            }
            break;
        }

        // received < 0
        if (!m_running_.load(std::memory_order_acquire)) {
            break;  // Close() 触发，正常退出
        }
        if (ops.is_interrupted()) {
            continue;  // 被信号打断：重试
        }
        if (ops.is_would_block()) {
            // 阻塞模式下不该出现；防御性续转，避免把瞬态误报成断链
            continue;
        }
        if (ops.is_connection_lost()) {
            const auto error = ops.last_error();
            close_reason = "TCP 连接被对端重置：" + error;
            break;
        }
        // 其它错误：上报后终止（TCP 是可靠流，无法"跳过"一个坏读继续同步）
        const auto error = ops.last_error();
        close_reason = "TCP 接收失败：" + error;
        break;
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

bool TcpTransport::CheckReceiveCtr(DatagramCtr ctr, std::string* out_reason) {
    // 1) 基线未建立：首个合法 Frame 建立基线，允许任意首值（计划 §1.5）。
    //    AsamV14 peer 的 CTR 可能在 TCP 断连/重连后继续增长，故**不得**
    //    要求首帧为 0。
    if (!m_recv_baseline_established_) {
        m_recv_baseline_established_ = true;
        m_last_recv_ctr_ = ctr;
        return true;
    }

    const DatagramCtr previous = m_last_recv_ctr_.value_or(0U);
    const DatagramCtr expected = static_cast<DatagramCtr>(previous + 1U);

    // 2) 严格连续：两种 Profile 都接受，且是 AsamV15 的**唯一**合法情形。
    if (ctr == expected) {
        m_last_recv_ctr_ = ctr;
        return true;
    }

    // 3) 以期望值为基准计算前向距离：1..0x7FFF 为前向缺号，
    //    0x8000 为方向歧义，0x8001..0xFFFF 为后向/重复。
    const int distance = CtrForwardDistance(expected, ctr);

    if (m_config_.wire_profile == TcpWireProfile::AsamV15) {
        // AsamV15：严格 CTR。任何偏离（缺号/重复/后向/歧义）一律断链——
        // 规范要求精确连续，缺号即意味流已不可信。
        if (out_reason != nullptr) {
            *out_reason = SanitizeReason(
                "AsamV15 接收 CTR 不连续：期望 " + CtrToHex(expected) +
                "，实收 " + CtrToHex(ctr) +
                "。严格 Profile 下缺号与重复均视为流已失步");
        }
        return false;
    }

    // ---- AsamV14 ----
    // 前向缺号：接受并推进。XCPlite 的发送队列丢弃会主动推进全局 CTR，
    // 缺号属对端已知行为，不构成"我方失步"（计划 §1.5）。
    if (distance != 0 && distance < 0x8000) {
        // 双通道上报：日志供事后追溯，listener 供调用方即时感知。
        // §1.5 要求"只记录丢帧诊断并继续"，diagnostic 的可观测面就是
        // OnTransportWarning——仅写日志会让上层无从得知已丢帧。
        const std::string gap_text =
            "AsamV14 接收 CTR 前向缺号，疑似对端丢帧：期望 " +
            CtrToHex(expected) + "，实收 " + CtrToHex(ctr) + "，缺口 " +
            std::to_string(distance);
        LIBXCP_LOG_WARNING("{}", gap_text);
        IPacketListener* listener = m_listener_;
        if (listener != nullptr) {
            try {
                listener->OnTransportWarning(SanitizeReason(gap_text));
            } catch (...) {
                // 监听器异常不得破坏接收路径
            }
        }
        m_last_recv_ctr_ = ctr;
        return true;
    }

    // 后向/重复（含 0x8000 歧义）：协议异常，断链。
    // 区分两者仅影响诊断文本，处理动作一致。
    if (out_reason != nullptr) {
        const bool ambiguous = (distance == 0x8000);
        *out_reason = SanitizeReason(
            ambiguous ? "AsamV14 接收 CTR 与期望值相差 0x8000，方向歧义：实收 "
                      : "AsamV14 接收 CTR 后向/重复，属协议异常：期望 ");
        *out_reason += CtrToHex(expected);
        *out_reason += "，实收 ";
        *out_reason += CtrToHex(ctr);
        *out_reason += "，最近值 ";
        *out_reason += CtrToHex(previous);
    }
    return false;
}

bool TcpTransport::DispatchDecodeResult(const TcpDecodeResult& result) {
    IPacketListener* listener = m_listener_;

    if (result.malformed) {
        if (listener != nullptr) {
            try {
                listener->OnTransportWarning(
                    "TCP 流失步：" +
                    (result.malformed_reason.empty()
                         ? std::string("未提供原因")
                         : result.malformed_reason));
            } catch (...) {
            }
        }
        return false;
    }

    for (const auto& frame : result.frames) {
        // CTR 校验先于长度上限：CTR 违约意味着流已不可信，此时"继续投递
        // 后续帧"没有意义，必须整条连接终止（计划 §1.5、§8.1 等价规则）。
        std::string ctr_reason;
        if (!CheckReceiveCtr(frame.header.ctr, &ctr_reason)) {
            if (listener != nullptr) {
                try {
                    listener->OnTransportWarning(ctr_reason);
                } catch (...) {
                }
            }
            return false;
        }
        // 单帧 Packet 长度上限：超过配置的帧直接丢弃，但**不断链**——
        // 这是"配置性上限"，不是流畸形，后续帧仍然可以正常解析。
        if (frame.header.len > m_config_.max_frame_packet_size) {
            if (listener != nullptr) {
                try {
                    listener->OnTransportWarning(
                        "Frame 内 XCP Packet 长度 " +
                        std::to_string(frame.header.len) +
                        " 超过配置上限，该 Frame 已丢弃");
                } catch (...) {
                }
            }
            continue;
        }
        LIBXCP_LOG_INFO("TCP RX frame ctr={} packet_bytes={} tail={}",
                        frame.header.ctr, frame.xcp_packet.size(),
                        frame.tail_bytes);
        if (listener != nullptr) {
            try {
                listener->OnPacketReceived(frame.xcp_packet);
                m_delivered_frames_.fetch_add(1, std::memory_order_relaxed);
            } catch (...) {
                // 上层回调异常不得终止接收线程
            }
        }
    }
    return true;
}

}  // namespace calmcar::xcp