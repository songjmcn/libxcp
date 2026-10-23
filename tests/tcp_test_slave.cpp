/**
 * @file tcp_test_slave.cpp
 * @brief `TcpTestSlave` 实现（B-T3）
 *
 * 见 `tcp_test_slave.hpp` 文件头对本类目的、双 Profile 独立 framing 的理由，
 * 以及 XCPlite AsamV14 语义的源级取证。
 */

#include "tcp_test_slave.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "libxcp/ethernet_header_codec.hpp"
#include "libxcp/tcp_wire_profile.hpp"
#include "libxcp/xcp_error.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace calmcar::xcp::test {
namespace {

// ---- 平台 Socket 辅助 ----

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

/**
 * @brief 关闭一个原生 Socket 句柄
 * @param handle 句柄；`kInvalidSocket` 时为空操作
 */
void CloseNative(NativeSocket handle) {
    if (handle == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    ::closesocket(handle);
#else
    ::close(handle);
#endif
}

/**
 * @brief 打断一个 Socket 上阻塞的收发
 * @param handle 句柄
 * @details 用 `shutdown` 而非 `close`：`close` 在另一线程阻塞于 `accept`/`recv`
 *          时的行为依平台而异（POSIX 下可能不唤醒），`shutdown` 是规范定义的
 *          "唤醒阻塞调用"手段。真正释放句柄仍走 `CloseNative`。
 */
void ShutdownNative(NativeSocket handle) {
    if (handle == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    ::shutdown(handle, SD_BOTH);
#else
    ::shutdown(handle, SHUT_RDWR);
#endif
}

/// @brief 抑制 SIGPIPE（POSIX 下向已关闭连接写入会触发该信号）
void SuppressSigpipe(NativeSocket handle) {
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(handle, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)handle;
#endif
}

/// @brief 关闭 Nagle，让分片注入用例的写出边界与观测一致
void DisableNagle(NativeSocket handle) {
    int one = 1;
    ::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&one), sizeof(one));
}

/// @brief 初始化 Windows Winsock（其它平台为空操作）
void InitWinsockOnce() {
#ifdef _WIN32
    static const bool init = [] {
        WSADATA data{};
        (void)::WSAStartup(MAKEWORD(2, 2), &data);
        return true;
    }();
    (void)init;
#endif
}

// ---- 字节序辅助（帧头为小端） ----

/**
 * @brief 把 16 位值以小端写入缓冲区
 */
void WriteLe16(std::uint8_t* out, std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>(value & 0xFFU);
    out[1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
}

/**
 * @brief 从小端缓冲区读出 16 位值
 */
std::uint16_t ReadLe16(const std::uint8_t* in) {
    return static_cast<std::uint16_t>(in[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[1]) << 8U);
}

}  // namespace

// ---- SocketImpl：把所有平台句柄收进 .cpp，头文件不泄漏任何平台类型 ----

/**
 * @brief `TcpTestSlave` 的平台 Socket 持有体
 * @details 头文件中只有不完整类型声明，故 `sockaddr`/`SOCKET` 等平台类型
 *          不会出现在公开头（与 B-T2 用户裁决 T1 同一原则）。
 */
struct TcpTestSlave::SocketImpl {
    /// @brief 监听 Socket
    NativeSocket listener{kInvalidSocket};
    /// @brief 当前活动连接 Socket
    NativeSocket connection{kInvalidSocket};
};

// ---- 构造 / 析构 ----

TcpTestSlave::TcpTestSlave(TcpWireProfile profile, std::uint16_t fixed_port)
    : m_profile_(profile),
      m_config_port_(fixed_port),
      m_socket_(std::make_unique<SocketImpl>()) {
    // Profile 必须显式指定：绝不自动选择（方案 §1.4）。
    if (!IsTcpWireProfileResolved(profile)) {
        throw detail::MakeInvalidArgument(
            "TcpTestSlave: TcpWireProfile 不得为 Unknown（必须显式指定线格式档）");
    }
    InitWinsockOnce();
}

TcpTestSlave::~TcpTestSlave() {
    Stop();
}

// ---- 控制 ----

void TcpTestSlave::Start() {
    if (m_running_.load()) {
        return;  // 幂等
    }

    // 1) 建立监听 Socket
    const NativeSocket listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kInvalidSocket) {
        throw detail::MakeTransportError("TcpTestSlave: socket() 失败");
    }
    m_socket_->listener = listener;

    int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(m_config_port_);

    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        CloseNative(m_socket_->listener);
        m_socket_->listener = kInvalidSocket;
        throw detail::MakeTransportError("TcpTestSlave: bind() 失败");
    }
    if (::listen(listener, 4) != 0) {
        CloseNative(m_socket_->listener);
        m_socket_->listener = kInvalidSocket;
        throw detail::MakeTransportError("TcpTestSlave: listen() 失败");
    }

    // 2) 取回实际端口（m_config_port_ == 0 时由 OS 分配）
    sockaddr_in bound{};
#ifdef _WIN32
    int bound_len = sizeof(bound);
#else
    socklen_t bound_len = sizeof(bound);
#endif
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&bound),
                      &bound_len) != 0) {
        CloseNative(m_socket_->listener);
        m_socket_->listener = kInvalidSocket;
        throw detail::MakeTransportError("TcpTestSlave: getsockname() 失败");
    }
    m_config_port_ = ntohs(bound.sin_port);

    // 3) 启动接收线程
    m_running_.store(true);
    m_thread_ = std::thread([this] { ReceiveLoop(); });
}

void TcpTestSlave::Stop() {
    if (!m_running_.exchange(false)) {
        // 即使未运行也要清理可能残留的句柄
        CloseConnection();
        if (m_socket_ && m_socket_->listener != kInvalidSocket) {
            CloseNative(m_socket_->listener);
            m_socket_->listener = kInvalidSocket;
        }
        return;
    }

    // shutdown 打断阻塞中的 accept/recv，再 join —— 不遗留线程。
    if (m_socket_) {
        ShutdownNative(m_socket_->listener);
        ShutdownNative(m_socket_->connection);
    }
    if (m_thread_.joinable()) {
        m_thread_.join();
    }

    CloseConnection();
    if (m_socket_->listener != kInvalidSocket) {
        CloseNative(m_socket_->listener);
        m_socket_->listener = kInvalidSocket;
    }
    m_has_connection_.store(false);
}

std::uint16_t TcpTestSlave::Port() const {
    return m_config_port_;
}

bool TcpTestSlave::HasActiveConnection() const {
    return m_has_connection_.load();
}

bool TcpTestSlave::WaitForConnection(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (m_has_connection_.load()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return m_has_connection_.load();
}

// ---- 测试配置 ----

void TcpTestSlave::SetMemory(Address address, BytesView data) {
    std::lock_guard<std::mutex> lock(m_mutex_);
    // 逐字节展开存储，使任意地址都能被独立查询
    for (std::size_t i = 0; i < data.size(); ++i) {
        m_memory_[address + i] = data[i];
    }
}

void TcpTestSlave::SetFaultInjection(const TcpFaultInjection& fault) {
    std::lock_guard<std::mutex> lock(m_mutex_);
    m_fault_ = fault;
}

void TcpTestSlave::SetInitialSendCtr(DatagramCtr ctr) {
    m_send_ctr_.store(ctr);
}

void TcpTestSlave::SetPacketAlignment(PacketAlignment alignment) {
    std::lock_guard<std::mutex> lock(m_mutex_);
    m_alignment_ = alignment;
}

// ---- 只读观测 ----

std::size_t TcpTestSlave::CommandCount() const {
    return m_command_count_.load();
}

bool TcpTestSlave::IsConnected() const {
    return m_connected_.load();
}

DatagramCtr TcpTestSlave::NextSendCtr() const {
    return m_send_ctr_.load();
}

std::uint64_t TcpTestSlave::BytesWritten() const {
    return m_bytes_written_.load();
}

void TcpTestSlave::CloseActiveConnection() {
    CloseConnection();
    m_has_connection_.store(false);
}

void TcpTestSlave::CloseConnection() {
    if (m_socket_ && m_socket_->connection != kInvalidSocket) {
        ShutdownNative(m_socket_->connection);
        CloseNative(m_socket_->connection);
        m_socket_->connection = kInvalidSocket;
    }
}

// ---- 接收线程 ----

void TcpTestSlave::ReceiveLoop() {
    // 监听 Socket 置为非阻塞：这样每轮循环都能用 select 同时观察
    // "是否有新连接待 accept" 与 "活动连接是否可读"。
    //
    // 为什么必须持续 accept（不能像生产 TcpTransport 那样 accept 一次就
    // 停止监听）：计划 §6.1 要求"第二连接需 accept 后显式拒绝/关闭"。
    // 若只在无活动连接时 accept，第二条连接会一直滞留在内核 backlog 里
    // ——客户端 connect() 竟然成功（三次握手由内核完成），却永远收不到
    // 任何响应，表现为"静默挂起"。这正是本用例最初 hang 住的根因。
#ifdef _WIN32
    u_long nonblocking = 1;
    ::ioctlsocket(m_socket_->listener, FIONBIO, &nonblocking);
#else
    const int flags = ::fcntl(m_socket_->listener, F_GETFL, 0);
    if (flags >= 0) {
        (void)::fcntl(m_socket_->listener, F_SETFL, flags | O_NONBLOCK);
    }
#endif

    while (m_running_.load()) {
        // 1) 排空 pending 连接：第一条接受，其余立即关闭（显式拒绝）
        AcceptPendingConnections();

        if (!m_running_.load()) {
            break;
        }

        // 2) 无可读数据时短暂休眠，避免忙等烧 CPU
        if (!m_has_connection_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        // 3) 用 select 等可读，超时后回到循环顶继续 accept 新连接
        const NativeSocket conn = m_socket_->connection;
        if (conn == kInvalidSocket) {
            m_has_connection_.store(false);
            continue;
        }
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(conn, &read_set);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 20000;  // 20 ms
#ifdef _WIN32
        const int ready = ::select(0, &read_set, nullptr, nullptr, &tv);
#else
        const int ready =
            ::select(conn + 1, &read_set, nullptr, nullptr, &tv);
#endif
        if (ready <= 0) {
            continue;  // 超时或 select 被打断：回到循环顶再 accept
        }

        Bytes packet;
        const int rc = ReadFrame(packet);
        if (rc == 0) {
            // 对端关闭：本 Slave 不做自动重连，关闭后回到 accept 等待新连接
            CloseConnection();
            m_has_connection_.store(false);
            continue;
        }
        if (rc < 0) {
            // 帧畸形或读错误：TCP 是字节流，无法在不丢弃字节的前提下重新同步，
            // 故终止该连接。这与生产 TcpStreamDecoder 的"不可恢复即失败"一致。
            CloseConnection();
            m_has_connection_.store(false);
            continue;
        }

        (void)HandleCommand(packet);
    }
}

void TcpTestSlave::AcceptPendingConnections() {
    const NativeSocket listener = m_socket_->listener;
    if (listener == kInvalidSocket) {
        return;
    }

    // 循环 accept 直到 EWOULDBLOCK：一次可能有多条连接排队
    for (;;) {
        sockaddr_in peer{};
#ifdef _WIN32
        int peer_len = sizeof(peer);
#else
        socklen_t peer_len = sizeof(peer);
#endif
        const NativeSocket conn = ::accept(
            listener, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (conn == kInvalidSocket) {
#ifdef _WIN32
            const int err = ::WSAGetLastError();
            const bool would_block = (err == WSAEWOULDBLOCK);
#else
            const bool would_block = (errno == EAGAIN || errno == EWOULDBLOCK);
#endif
#ifdef _WIN32
            (void)err;
#endif
            if (!would_block) {
                // 真正 accept 失败（如 Stop() 关闭了监听）：退出循环
            }
            return;
        }

        if (m_has_connection_.load()) {
            // **显式拒绝**第二条连接：立即关闭，绝不静默占用
            ShutdownNative(conn);
            CloseNative(conn);
            continue;
        }

        DisableNagle(conn);
        SuppressSigpipe(conn);
        m_socket_->connection = conn;
        m_has_connection_.store(true);
    }
}

int TcpTestSlave::ReadExact(std::uint8_t* out, std::size_t size) {
    std::size_t got = 0;
    while (got < size) {
        if (!m_running_.load()) {
            return -1;
        }
        const NativeSocket conn = m_socket_->connection;
        if (conn == kInvalidSocket) {
            return 0;
        }
#ifdef _WIN32
        const int n = ::recv(conn, reinterpret_cast<char*>(out + got),
                             static_cast<int>(size - got), 0);
#else
        const ssize_t n = ::recv(conn, out + got, size - got, 0);
#endif
        if (n == 0) {
            return 0;  // 对端关闭（FIN）
        }
        if (n < 0) {
            return -1;
        }
        got += static_cast<std::size_t>(n);
    }
    return static_cast<int>(got);
}

int TcpTestSlave::ReadFrame(Bytes& out_packet) {
    // 1) 读 4 字节 Transport Header
    std::array<std::uint8_t, kEthernetHeaderSize> header{};
    const int hdr_rc = ReadExact(header.data(), header.size());
    if (hdr_rc <= 0) {
        return hdr_rc;
    }

    const std::uint16_t len = ReadLe16(header.data());
    if (len == 0U) {
        return -1;  // LEN=0 无意义（每个 Frame 至少 1 字节 Packet）
    }

    // 2) 按 LEN 精确读 Packet（AsamV14 与 AsamV15 在此一致）
    out_packet.resize(len);
    const int pkt_rc = ReadExact(out_packet.data(), len);
    if (pkt_rc <= 0) {
        return pkt_rc;
    }

    // 3) 仅 AsamV15 读 LEN 外的 Tail FILL，使后继 Packet 起始偏移对齐。
    //    AsamV14（XCPlite 语义）**不读** Tail —— 见 xcpEthTl.c:332-335 的精确读取。
    if (m_profile_ == TcpWireProfile::AsamV15) {
        PacketAlignment alignment;
        {
            std::lock_guard<std::mutex> lock(m_mutex_);
            alignment = m_alignment_;
        }
        const std::uint8_t tail =
            TailBytesForAlignment(alignment, kEthernetHeaderSize + len);
        if (tail > 0U) {
            std::array<std::uint8_t, kEthernetMaxAlignmentBytes> filler{};
            const int tail_rc = ReadExact(filler.data(), tail);
            if (tail_rc <= 0) {
                return tail_rc;
            }
        }
    }

    m_command_count_.fetch_add(1);
    return 1;
}

// ---- framing（Slave 侧独立实现） ----

Bytes TcpTestSlave::EncodeFrame(BytesView packet, DatagramCtr ctr) const {
    if (packet.empty() || packet.size() > kEthernetMaxXcpPacket) {
        throw detail::MakeInvalidArgument(
            "TcpTestSlave::EncodeFrame: XCP Packet 长度须在 1..65535");
    }

    // Tail 只有 AsamV15 才计算；AsamV14 恒定无 Tail（XCPlite 的 p.dlc = size）。
    std::uint8_t tail = 0;
    if (m_profile_ == TcpWireProfile::AsamV15) {
        PacketAlignment alignment;
        {
            std::lock_guard<std::mutex> lock(m_mutex_);
            alignment = m_alignment_;
        }
        tail = TailBytesForAlignment(alignment, kEthernetHeaderSize + packet.size());
    }

    Bytes frame;
    frame.reserve(kEthernetHeaderSize + packet.size() + tail);
    frame.resize(kEthernetHeaderSize);
    WriteLe16(frame.data(), static_cast<std::uint16_t>(packet.size()));
    WriteLe16(frame.data() + 2, ctr);
    frame.insert(frame.end(), packet.begin(), packet.end());
    // FILL 内容规范定为 "do not care"，本库固定 0x00 便于比对
    frame.insert(frame.end(), tail, 0x00U);
    return frame;
}

// ---- 命令处理 ----

Bytes TcpTestSlave::DispatchCommand(BytesView xcp_packet, bool* should_reply) {
    *should_reply = true;
    if (xcp_packet.empty()) {
        // ReadFrame 已挡掉空包；这里是防御性分支
        *should_reply = false;
        return {};
    }

    const std::uint8_t pid = xcp_packet[0];
    Bytes out;

    switch (pid) {
        case 0xFFU:  // CONNECT
        {
            // 响应数据域布局（PID 之后，共 7 字节，见
            // ResponseParser::ParseConnectResponse，kConnectResMinSize=7）：
            //   [RESOURCE][COMM_MODE_BASIC][MAX_CTO][MAX_DTO(2)][PL][TL]
            // 注意 **没有**独立的 AG 字节：地址粒度编码在 COMM_MODE_BASIC
            // 的 bit1-2（src/response_parser.cpp:201-203），故 AG=BYTE 对应
            // COMM_MODE_BASIC 低两位为 0b00。
            // 与 udp_test_slave.cpp:972 的参考构造逐字段一致。
            out.push_back(0xFFU);
            // RESOURCE = 0x11 = CAL/PAG(bit0) + PGM(bit4)，**不含** DAQ(bit2)。
            // 与 udp_test_slave.cpp:971（未开 DAQ 模拟时的 0x11）一致：
            // 声明了却未实现的 Mandatory 命令会被 Master 误认为可用。
            out.push_back(0x11U);
            out.push_back(0xC0U);  // COMM_MODE_BASIC: Intel/BYTE/Block/Optional
            out.push_back(0x08U);  // MAX_CTO = 8
            out.push_back(0xFFU);  // MAX_DTO = 0xFFFF 小端低字节
            out.push_back(0xFFU);  // MAX_DTO 小端高字节
            out.push_back(0x10U);  // Protocol Layer 1.0
            out.push_back(0x10U);  // Transport Layer 1.0
            m_connected_.store(true);
            break;
        }
        case 0xFEU:  // DISCONNECT
        {
            out.push_back(0xFFU);  // 正向响应 PID 为 CONNECT
            m_connected_.store(false);
            break;
        }
        case 0xFDU:  // GET_STATUS
        {
            // 响应数据域（PID 之后，共 5 字节，kGetStatusResMinSize=5）：
            // [SESSION_STATUS][PROTECTION][STATE_NUMBER][SESSION_CONFIG(2)]
            // 与 udp_test_slave.cpp:1000 的参考构造一致。
            out.push_back(0xFFU);
            out.push_back(0x00U);  // SESSION_STATUS（未跑 DAQ）
            out.push_back(0x00U);  // PROTECTION
            out.push_back(0x01U);  // STATE_NUMBER
            out.push_back(0x07U);  // SESSION_CONFIG 小端低字节
            out.push_back(0x00U);  // SESSION_CONFIG 小端高字节
            break;
        }
        case 0xF8U:  // GET_COMM_MODE_INFO
        {
            // 响应数据域（PID 之后，共 7 字节，kGetCommModeInfoResMinSize=7）：
            // [reserved][COMM_MODE_OPTIONAL][reserved][MAX_BS][MIN_ST]
            // [QUEUE_SIZE][DRIVER_VER]
            // 与 udp_test_slave.cpp:1012 的参考构造一致。
            out.push_back(0xFFU);
            out.push_back(0x00U);  // reserved
            out.push_back(0x0EU);  // COMM_MODE_OPTIONAL
            out.push_back(0x00U);  // reserved
            out.push_back(0x04U);  // MAX_BS
            out.push_back(0x02U);  // MIN_ST
            out.push_back(0x08U);  // QUEUE_SIZE
            out.push_back(0x13U);  // DRIVER_VER
            break;
        }
        case 0xF6U:  // SET_MTA
        {
            // 请求：[F6][MODE][reserved][Extension][Address(4)] = 8 字节
            // 与 CommandCodec::EncodeSetMta（src/command_codec.cpp:132-136）一致。
            if (xcp_packet.size() < 8U) {
                *should_reply = false;
                return {};
            }
            const Address addr =
                static_cast<Address>(xcp_packet[4]) |
                (static_cast<Address>(xcp_packet[5]) << 8U) |
                (static_cast<Address>(xcp_packet[6]) << 16U) |
                (static_cast<Address>(xcp_packet[7]) << 24U);
            m_mta_ = addr;
            out.push_back(0xFFU);
            break;
        }
        case 0xF5U:  // UPLOAD
        {
            if (xcp_packet.size() < 2U) {
                *should_reply = false;
                return {};
            }
            const std::size_t count = xcp_packet[1];
            std::lock_guard<std::mutex> lock(m_mutex_);
            out.push_back(0xFFU);
            for (std::size_t i = 0; i < count; ++i) {
                auto it = m_memory_.find(m_mta_ + i);
                // 未配置的地址回 0x00（确定性优于随机）
                out.push_back(it != m_memory_.end() ? it->second : 0x00U);
            }
            m_mta_ += count;
            break;
        }
        case 0xF4U:  // SHORT_UPLOAD
        {
            // 请求：[F4][NumberOfElements][reserved][Extension][Address(4)] = 8 字节
            // 与 CommandCodec::EncodeShortUpload（src/command_codec.cpp:166-170）一致。
            if (xcp_packet.size() < 8U) {
                *should_reply = false;
                return {};
            }
            const std::size_t count = xcp_packet[1];
            const Address addr =
                static_cast<Address>(xcp_packet[4]) |
                (static_cast<Address>(xcp_packet[5]) << 8U) |
                (static_cast<Address>(xcp_packet[6]) << 16U) |
                (static_cast<Address>(xcp_packet[7]) << 24U);
            std::lock_guard<std::mutex> lock(m_mutex_);
            out.push_back(0xFFU);
            for (std::size_t i = 0; i < count; ++i) {
                auto it = m_memory_.find(addr + i);
                out.push_back(it != m_memory_.end() ? it->second : 0x00U);
            }
            break;
        }
        case 0xF0U:  // DOWNLOAD
        {
            // 请求：[F0][NumberOfElements][data...]
            // 计划 §6.1 的命令子集只要求 CONNECT/DISCONNECT/GET_STATUS/
            // GET_COMM_MODE_INFO/SET_MTA/UPLOAD/SHORT_UPLOAD/DOWNLOAD/SYNCH。
            // DOWNLOAD 只做**最小写回**：按 NumberOfElements 从 data 区取字节
            // 写到 MTA。不做块传输（Block Mode），因为本 Slave 已在 CONNECT
            // 里把 COMM_MODE_BASIC 的 Block 位置位却不实现块语义——
            // 这属于测试设施与生产能力的差异，故 DOWNLOAD 仅按单帧处理。
            if (xcp_packet.size() < 2U) {
                *should_reply = false;
                return {};
            }
            const std::size_t count = xcp_packet[1];
            std::lock_guard<std::mutex> lock(m_mutex_);
            for (std::size_t i = 0; i < count && (2U + i) < xcp_packet.size();
                 ++i) {
                m_memory_[m_mta_ + i] = xcp_packet[2U + i];
            }
            m_mta_ += count;
            out.push_back(0xFFU);
            break;
        }
        case 0xFCU:  // SYNCH：无响应
        {
            *should_reply = false;
            return {};
        }
        default:
        {
            // 未实现的命令：ERR_CMD_UNKNOWN 负响应
            // 负响应 PID 为 0xFE（ERR），错误码 0x20
            out.push_back(0xFEU);
            out.push_back(0x20U);
            break;
        }
    }
    return out;
}

bool TcpTestSlave::HandleCommand(BytesView xcp_packet) {
    bool should_reply = false;
    const Bytes response = DispatchCommand(xcp_packet, &should_reply);
    if (!should_reply) {
        return true;  // 如 SYNCH：无需响应
    }
    return WriteResponse(response);
}

bool TcpTestSlave::WriteResponse(BytesView packet) {
    // 快照故障配置与该响应序号（响应序号从 1 开始）
    TcpFaultInjection fault;
    PacketAlignment alignment;
    {
        std::lock_guard<std::mutex> lock(m_mutex_);
        fault = m_fault_;
        alignment = m_alignment_;
    }
    const std::size_t n = m_response_count_.fetch_add(1) + 1;

    // 1) 丢弃：Master 应超时
    if (fault.drop_response_n && *fault.drop_response_n == n) {
        return true;
    }

    // 2) 延迟：Master 应超时或恰好收到
    if (fault.delay_response_n && fault.delay_response_n->first == n) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(fault.delay_response_n->second));
    }

    Bytes frame_packet(packet.begin(), packet.end());

    // 3) 用另一 Profile 编码本帧（验证 Master 不做自动探测）
    if (fault.wrong_profile_frame_n && *fault.wrong_profile_frame_n == n) {
        // 故意用相反的 Profile 布局：V15 用无 Tail、V14 用带 Tail
        Bytes swapped;
        swapped.reserve(kEthernetHeaderSize + frame_packet.size() +
                        kEthernetMaxAlignmentBytes);
        swapped.resize(kEthernetHeaderSize);
        WriteLe16(swapped.data(), static_cast<std::uint16_t>(frame_packet.size()));
        WriteLe16(swapped.data() + 2, m_send_ctr_.load());
        swapped.insert(swapped.end(), frame_packet.begin(), frame_packet.end());
        if (m_profile_ == TcpWireProfile::AsamV14) {
            // V14 Slave 发 V15 风格帧：附加对齐 Tail
            const std::uint8_t tail = TailBytesForAlignment(
                alignment, kEthernetHeaderSize + frame_packet.size());
            swapped.insert(swapped.end(), tail, 0x00U);
        }
        // 写出后推进 CTR 并计数（与正常路径一致）
        m_send_ctr_.fetch_add(1);
        SendRaw(BytesView(swapped));
        m_bytes_written_.fetch_add(swapped.size());
        return true;
    }

    // 4) 畸形 LEN（Master 应判定流不可恢复）
    if (fault.corrupt_len_n && *fault.corrupt_len_n == n) {
        Bytes bad;
        bad.reserve(kEthernetHeaderSize + frame_packet.size());
        bad.resize(kEthernetHeaderSize);
        // 故意把 LEN 写大：Master 会认为后继字节不足而失步
        WriteLe16(bad.data(),
                  static_cast<std::uint16_t>(frame_packet.size() + 64U));
        WriteLe16(bad.data() + 2, m_send_ctr_.load());
        bad.insert(bad.end(), frame_packet.begin(), frame_packet.end());
        m_send_ctr_.fetch_add(1);
        SendRaw(BytesView(bad));
        m_bytes_written_.fetch_add(bad.size());
        return true;
    }

    // 5) CTR 注入：跳号 / 重复 / 有符号偏移
    DatagramCtr ctr = m_send_ctr_.load();
    if (fault.jump_ctr_n && *fault.jump_ctr_n == n) {
        ctr = static_cast<DatagramCtr>(ctr + 5U);
    }
    if (fault.duplicate_ctr_n && *fault.duplicate_ctr_n == n) {
        // 复用前一 CTR（后向 -1，模 65536）
        ctr = static_cast<DatagramCtr>(ctr - 1U);
    }
    if (fault.ctr_offset_n && fault.ctr_offset_n->first == n) {
        const int off = fault.ctr_offset_n->second;
        ctr = static_cast<DatagramCtr>(
            (static_cast<long long>(ctr) + off) & 0xFFFFLL);
    }

    // 6) 正常编码（Profile 语义由 EncodeFrame 决定）
    Bytes frame;
    {
        // EncodeFrame 使用成员 CTR；这里需要注入后的 CTR，故就地构造。
        std::uint8_t tail = 0;
        if (m_profile_ == TcpWireProfile::AsamV15) {
            tail = TailBytesForAlignment(
                alignment, kEthernetHeaderSize + frame_packet.size());
        }
        frame.reserve(kEthernetHeaderSize + frame_packet.size() + tail);
        frame.resize(kEthernetHeaderSize);
        WriteLe16(frame.data(),
                  static_cast<std::uint16_t>(frame_packet.size()));
        WriteLe16(frame.data() + 2, ctr);
        frame.insert(frame.end(), frame_packet.begin(), frame_packet.end());
        frame.insert(frame.end(), tail, 0x00U);
    }

    // CTR 计数器按"正常推进"语义前进（注入只改写本帧的实际值，不影响规律）
    m_send_ctr_.fetch_add(1);

    // 7) 分片注入：逐字节 / 固定块，然后写出
    const NativeSocket conn = m_socket_->connection;
    if (conn == kInvalidSocket) {
        return false;
    }

    auto write_all = [&](BytesView data) -> bool {
        std::size_t sent = 0;
        while (sent < data.size()) {
            if (!m_running_.load()) {
                return false;
            }
#ifdef _WIN32
            const int n2 = ::send(conn,
                                  reinterpret_cast<const char*>(data.data() + sent),
                                  static_cast<int>(data.size() - sent), 0);
#else
            const ssize_t n2 = ::send(conn, data.data() + sent,
                                      data.size() - sent, MSG_NOSIGNAL);
#endif
            if (n2 <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(n2);
        }
        return true;
    };

    BytesView frame_view(frame);
    bool ok = true;

    if (fault.split_bytes_n && *fault.split_bytes_n == n) {
        // 逐字节写出：验证 Master 的"recv 边界与 Frame 边界无关"
        for (std::size_t i = 0; i < frame.size(); ++i) {
            if (!write_all(frame_view.subspan(i, 1))) {
                ok = false;
                break;
            }
        }
    } else if (fault.chunk_size_n && fault.chunk_size_n->first == n &&
               fault.chunk_size_n->second > 0U) {
        const std::size_t chunk = fault.chunk_size_n->second;
        for (std::size_t off = 0; off < frame.size(); off += chunk) {
            const std::size_t n2 = std::min(chunk, frame.size() - off);
            if (!write_all(frame_view.subspan(off, n2))) {
                ok = false;
                break;
            }
        }
    } else {
        ok = write_all(frame_view);
    }

    if (!ok) {
        return false;
    }
    m_bytes_written_.fetch_add(frame.size());

    // 8) 写完即关闭连接（模拟对端 FIN）
    if (fault.close_after_response_n && *fault.close_after_response_n == n) {
        CloseConnection();
        m_has_connection_.store(false);
        return false;
    }
    return true;
}

void TcpTestSlave::SendRaw(BytesView raw) {
    const NativeSocket conn = m_socket_->connection;
    if (conn == kInvalidSocket) {
        throw detail::MakeInvalidState(
            "TcpTestSlave::SendRaw: 当前没有活动连接");
    }
    std::size_t sent = 0;
    while (sent < raw.size()) {
#ifdef _WIN32
        const int n = ::send(conn, reinterpret_cast<const char*>(raw.data() + sent),
                             static_cast<int>(raw.size() - sent), 0);
#else
        const ssize_t n = ::send(conn, raw.data() + sent, raw.size() - sent,
                                 MSG_NOSIGNAL);
#endif
        if (n <= 0) {
            throw detail::MakeTransportError("TcpTestSlave::SendRaw: send() 失败");
        }
        sent += static_cast<std::size_t>(n);
    }
    m_bytes_written_.fetch_add(raw.size());
}

}  // namespace calmcar::xcp::test