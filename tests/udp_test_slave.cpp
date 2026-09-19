/**
 * @file udp_test_slave.cpp
 * @brief UdpTestSlave 实现：最小 XCP Slave 语义 + XCP 1.1 UDP 端点绑定 +
 * 故障注入。
 */

#include "udp_test_slave.hpp"

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "libxcp/udp_header_codec.hpp"
#include "libxcp/xcp_error.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace calmcar::xcp::test {

namespace {

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
void closeSocket(SocketHandle handle) { ::closesocket(handle); }
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
void closeSocket(SocketHandle handle) { ::close(handle); }
#endif

/// @brief 测试用 AG（BYTE），与默认 CONNECT 响应保持一致
constexpr std::uint8_t kTestAgBytes = 1U;
/// @brief 测试 Slave 的 MAX_CTO / MAX_DTO（与规范示例一致）
constexpr std::uint8_t kTestMaxCto = 8U;
constexpr std::uint16_t kTestMaxDto = 8U;
/// @brief 接收缓冲区大小
constexpr std::size_t kReceiveBufferSize = 65536;

/// @brief Windows 进程内 WSAStartup 引用计数
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

private:
    static inline int s_ref_count_{0};
    static inline bool s_ok_{false};
};
#endif

}  // namespace

// ---------------------------------------------------------------------------
// SocketImpl
// ---------------------------------------------------------------------------
struct UdpTestSlave::SocketImpl {
#if defined(_WIN32)
    WinsockSession winsock;
#endif
    SocketHandle handle{kInvalidSocket};
};

// ---------------------------------------------------------------------------
// 构造 / 析构 / 生命周期
// ---------------------------------------------------------------------------

UdpTestSlave::UdpTestSlave() {
    auto impl = std::make_unique<SocketImpl>();
    impl->handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl->handle == kInvalidSocket) {
        throw detail::MakeTransportError("测试 Slave 创建 Socket 失败");
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = 0;  // 由 OS 分配临时端口，测试不依赖固定端口
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(impl->handle, reinterpret_cast<sockaddr*>(&local),
               sizeof(local)) != 0) {
        closeSocket(impl->handle);
        throw detail::MakeTransportError("测试 Slave 绑定 Loopback 失败");
    }

    sockaddr_in actual{};
    socklen_t len = sizeof(actual);
    if (::getsockname(impl->handle, reinterpret_cast<sockaddr*>(&actual),
                      &len) == 0) {
        m_port_ = ntohs(actual.sin_port);
    }

    // 接收超时：使阻塞 recvfrom 周期性检查停止标记
#if defined(_WIN32)
    DWORD tv = 50;
    ::setsockopt(impl->handle, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 50 * 1000;
    ::setsockopt(impl->handle, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    m_socket_ = std::move(impl);
}

UdpTestSlave::~UdpTestSlave() {
    try {
        Stop();
    } catch (...) {
    }
}

void UdpTestSlave::Start() {
    if (m_running_.load(std::memory_order_acquire)) {
        return;  // 幂等
    }
    m_running_.store(true, std::memory_order_release);
    m_receive_thread_ = std::thread([this] { ReceiveLoop(); });
}

void UdpTestSlave::Stop() {
    m_running_.store(false, std::memory_order_release);
    if (m_socket_ && m_socket_->handle != kInvalidSocket) {
        closeSocket(m_socket_->handle);
        m_socket_->handle = kInvalidSocket;
    }
    if (m_receive_thread_.joinable()) {
        m_receive_thread_.join();
    }
}

std::uint16_t UdpTestSlave::Port() const { return m_port_; }

bool UdpTestSlave::IsConnected() const {
    const std::lock_guard<std::mutex> lock(m_state_mutex_);
    return m_connected_;
}

// ---------------------------------------------------------------------------
// 测试配置
// ---------------------------------------------------------------------------

void UdpTestSlave::SetMemory(Address address, BytesView data) {
    const std::lock_guard<std::mutex> lock(m_memory_mutex_);
    m_memory_[address] = Bytes(data.begin(), data.end());
}

void UdpTestSlave::SetFaultInjection(const FaultInjection& fault) {
    const std::lock_guard<std::mutex> lock(m_state_mutex_);
    m_fault_ = fault;
    m_response_count_ = 0;
}

std::size_t UdpTestSlave::CommandCount() const {
    const std::lock_guard<std::mutex> lock(m_state_mutex_);
    return m_command_count_;
}

// ---------------------------------------------------------------------------
// 报文构造辅助
// ---------------------------------------------------------------------------

Bytes UdpTestSlave::makeRes(std::initializer_list<std::uint8_t> body) {
    Bytes packet;
    packet.reserve(1 + body.size());
    packet.push_back(static_cast<std::uint8_t>(PacketType::Res));
    packet.insert(packet.end(), body.begin(), body.end());
    return packet;
}

Bytes UdpTestSlave::makeErr(ErrorCode code) {
    return Bytes{static_cast<std::uint8_t>(PacketType::Err),
                 static_cast<std::uint8_t>(code)};
}

bool UdpTestSlave::advanceMta(ElementCount elements) {
    const auto bytes = static_cast<std::uint64_t>(elements) * kTestAgBytes;
    const auto sum = static_cast<std::uint64_t>(m_mta_) + bytes;
    if (sum > 0xFFFFFFFFULL) {
        return false;
    }
    m_mta_ = static_cast<Address>(sum);
    return true;
}

std::optional<Bytes> UdpTestSlave::readAtMta(ElementCount elements) {
    const auto want = static_cast<std::size_t>(elements) * kTestAgBytes;
    const std::lock_guard<std::mutex> lock(m_memory_mutex_);
    for (const auto& [base, content] : m_memory_) {
        if (m_mta_ >= base && m_mta_ < base + content.size()) {
            const auto offset = static_cast<std::size_t>(m_mta_ - base);
            if (offset + want > content.size()) {
                return std::nullopt;  // 跨越内存块边界：视为不可读
            }
            return Bytes(
                content.begin() + static_cast<std::ptrdiff_t>(offset),
                content.begin() + static_cast<std::ptrdiff_t>(offset + want));
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// 接收循环与命令处理
// ---------------------------------------------------------------------------

void UdpTestSlave::ReceiveLoop() {
    std::vector<std::uint8_t> buffer(kReceiveBufferSize);

    while (m_running_.load(std::memory_order_acquire)) {
        sockaddr_in src{};
        socklen_t src_len = sizeof(src);
        const int received =
            ::recvfrom(m_socket_ ? m_socket_->handle : kInvalidSocket,
                       reinterpret_cast<char*>(buffer.data()),
                       static_cast<int>(buffer.size()), 0,
                       reinterpret_cast<sockaddr*>(&src), &src_len);

        if (received <= 0) {
            continue;  // 超时或 Socket 已关闭：由 running_ 标记决定是否退出
        }

        char ip_text[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &src.sin_addr, ip_text, INET_ADDRSTRLEN);
        const std::string src_ip(ip_text);
        const std::uint16_t src_port = ntohs(src.sin_port);

        // 按 XCP 1.1：一个 Datagram 可能包含多个完整 Frame，逐个解析后依序处理
        const auto frames = decodeUdpDatagram(
            BytesView{buffer.data(), static_cast<std::size_t>(received)});
        if (!frames) {
            continue;  // 畸形 Datagram：测试 Slave 直接忽略
        }
        for (const auto& frame : *frames) {
            HandleCommand(frame.m_xcp_packet_, src_ip, src_port);
        }
    }
}

bool UdpTestSlave::IsCurrentSessionSource(const std::string& source_ip) const {
    if (!m_connect_source_ip_) {
        return false;
    }
    return source_ip == *m_connect_source_ip_;
}

void UdpTestSlave::HandleCommand(BytesView xcp_packet,
                                 const std::string& source_ip,
                                 std::uint16_t source_port) {
    if (xcp_packet.empty()) {
        return;
    }

    Bytes response;
    std::optional<std::pair<std::string, std::uint16_t>>
        destination;  // 响应目的端点

    {
        const std::lock_guard<std::mutex> lock(m_state_mutex_);
        ++m_command_count_;

        const std::uint8_t cmd = xcp_packet[0];

        if (cmd == static_cast<std::uint8_t>(CommandCode::Connect)) {
            // 未连接时接受 CONNECT 并记录来源端点；重复 CONNECT 亦刷新端点
            m_connected_ = true;
            m_connect_source_ip_ = source_ip;
            m_connect_source_port_ = source_port;
            m_mta_ = 0U;
            m_mta_extension_ = 0U;
            // RESOURCE=0x15(CAL/PAG+DAQ+PGM),
            // COMM_MODE_BASIC=0xC0(Intel/BYTE/Block/Optional), MAX_CTO=8,
            // MAX_DTO=8(小端), ProtoVer=0x10, TransportVer=0x10
            response =
                makeRes({0x15, 0xC0, kTestMaxCto,
                         static_cast<std::uint8_t>(kTestMaxDto & 0xFFU),
                         static_cast<std::uint8_t>((kTestMaxDto >> 8) & 0xFFU),
                         0x10, 0x10});
            destination = std::make_pair(source_ip, source_port);
        } else if (!m_connected_ || !IsCurrentSessionSource(source_ip)) {
            // 未建立会话，或来源 IP 不是 CONNECT 来源：忽略该命令（D8）
            return;
        } else if (cmd == static_cast<std::uint8_t>(CommandCode::Disconnect)) {
            m_connected_ = false;
            response = makeRes({});
            destination =
                std::make_pair(*m_connect_source_ip_, *m_connect_source_port_);
        } else if (cmd == static_cast<std::uint8_t>(CommandCode::GetStatus)) {
            // Session Status=0x00, Protection=0x00, STATE_NUMBER=0x01,
            // ConfigID=0x0007(小端)
            response = makeRes({0x00, 0x00, 0x01, 0x07, 0x00});
            destination =
                std::make_pair(*m_connect_source_ip_, *m_connect_source_port_);
        } else if (cmd == static_cast<std::uint8_t>(CommandCode::Synch)) {
            // SYNCH 始终以 ERR_CMD_SYNCH 应答
            response = makeErr(ErrorCode::CmdSynch);
            destination =
                std::make_pair(*m_connect_source_ip_, *m_connect_source_port_);
        } else if (cmd ==
                   static_cast<std::uint8_t>(CommandCode::GetCommModeInfo)) {
            // reserved, COMM_MODE_OPTIONAL, reserved, MAX_BS, MIN_ST,
            // QUEUE_SIZE, DRIVER_VER
            response = makeRes({0x00, 0x0E, 0x00, 0x04, 0x02, 0x08, 0x13});
            destination =
                std::make_pair(*m_connect_source_ip_, *m_connect_source_port_);
        } else if (cmd == static_cast<std::uint8_t>(CommandCode::SetMta)) {
            if (xcp_packet.size() < 7U) {
                response = makeErr(ErrorCode::CmdSyntax);
            } else {
                m_mta_extension_ = xcp_packet[2];
                // Address 按 Session Byte Order（测试 Slave 固定 Intel 小端）
                m_mta_ = static_cast<Address>(xcp_packet[3]) |
                         (static_cast<Address>(xcp_packet[4]) << 8) |
                         (static_cast<Address>(xcp_packet[5]) << 16) |
                         (static_cast<Address>(xcp_packet[6]) << 24);
                response = makeRes({});
            }
            destination =
                std::make_pair(*m_connect_source_ip_, *m_connect_source_port_);
        } else if (cmd == static_cast<std::uint8_t>(CommandCode::Upload)) {
            if (xcp_packet.size() < 2U) {
                response = makeErr(ErrorCode::CmdSyntax);
            } else {
                const auto elements = static_cast<ElementCount>(xcp_packet[1]);
                if (elements == 0U ||
                    elements + 1U > kTestMaxCto / kTestAgBytes) {
                    response = makeErr(ErrorCode::OutOfRange);
                } else if (auto data = readAtMta(elements)) {
                    // UPLOAD 的 RES 为 [FF][data...]，长度 ==
                    // elements*AG，不含计数字节 （依据
                    // docs/XCP_1.3.0_document.md §12.4 示例：UPLOAD(6) -> 6
                    // 字节 "XCPSIM"）
                    response.clear();
                    response.push_back(
                        static_cast<std::uint8_t>(PacketType::Res));
                    response.insert(response.end(), data->begin(), data->end());
                    advanceMta(elements);
                } else {
                    response = makeErr(ErrorCode::AccessDenied);
                }
            }
            destination =
                std::make_pair(*m_connect_source_ip_, *m_connect_source_port_);
        } else if (cmd == static_cast<std::uint8_t>(CommandCode::ShortUpload)) {
            if (xcp_packet.size() < 8U) {
                response = makeErr(ErrorCode::CmdSyntax);
            } else {
                const auto elements = static_cast<ElementCount>(xcp_packet[1]);
                const auto extension = xcp_packet[3];
                const auto address =
                    static_cast<Address>(xcp_packet[4]) |
                    (static_cast<Address>(xcp_packet[5]) << 8) |
                    (static_cast<Address>(xcp_packet[6]) << 16) |
                    (static_cast<Address>(xcp_packet[7]) << 24);
                if (elements == 0U || elements > kTestMaxCto / kTestAgBytes) {
                    response = makeErr(ErrorCode::OutOfRange);
                } else {
                    const Address saved_mta = m_mta_;
                    const AddressExtension saved_ext = m_mta_extension_;
                    m_mta_ = address;
                    m_mta_extension_ = extension;
                    if (auto data = readAtMta(elements)) {
                        // SHORT_UPLOAD 的 RES 同样为 [FF][data...]
                        // （依据 docs/XCP_1.3.0_document.md §12.5：size=4 -> 4
                        // 字节数据）
                        response.clear();
                        response.push_back(
                            static_cast<std::uint8_t>(PacketType::Res));
                        response.insert(response.end(), data->begin(),
                                        data->end());
                        // SHORT_UPLOAD 之后 MTA 仍前进到数据块末尾之后
                        advanceMta(elements);
                    } else {
                        response = makeErr(ErrorCode::AccessDenied);
                        m_mta_ = saved_mta;
                        m_mta_extension_ = saved_ext;
                    }
                }
            }
            destination =
                std::make_pair(*m_connect_source_ip_, *m_connect_source_port_);
        } else {
            response = makeErr(ErrorCode::CmdUnknown);
            destination =
                std::make_pair(*m_connect_source_ip_, *m_connect_source_port_);
        }
    }

    if (response.empty() || !destination) {
        return;
    }
    sendResponseTo(BytesView{response}, destination->first,
                   destination->second);
}

void UdpTestSlave::sendResponse(BytesView xcp_packet) {
    std::string ip;
    std::uint16_t port = 0;
    {
        const std::lock_guard<std::mutex> lock(m_state_mutex_);
        if (!m_connect_source_ip_ || !m_connect_source_port_) {
            throw detail::MakeInvalidState(
                "UdpTestSlave 尚未记录 CONNECT 来源端点");
        }
        ip = *m_connect_source_ip_;
        port = *m_connect_source_port_;
    }
    sendResponseTo(xcp_packet, ip, port);
}

void UdpTestSlave::sendResponseTo(BytesView xcp_packet, const std::string& ip,
                                  std::uint16_t port) {
    DatagramCtr ctr = 0;
    FaultInjection fault;
    bool drop = false;
    std::uint32_t delay_ms = 0;
    bool corrupt_len = false;
    bool jump_ctr = false;
    bool duplicate_ctr = false;

    {
        const std::lock_guard<std::mutex> lock(m_state_mutex_);
        ctr = m_send_ctr_;
        ++m_send_ctr_;
        ++m_response_count_;
        fault = m_fault_;
        const std::size_t n = m_response_count_;

        drop = fault.m_drop_response_n_ && *fault.m_drop_response_n_ == n;
        corrupt_len = fault.m_corrupt_len_n_ && *fault.m_corrupt_len_n_ == n;
        jump_ctr = fault.m_jump_ctr_n_ && *fault.m_jump_ctr_n_ == n;
        duplicate_ctr =
            fault.m_duplicate_ctr_n_ && *fault.m_duplicate_ctr_n_ == n;
        if (fault.m_delay_response_n_ &&
            fault.m_delay_response_n_->first == n) {
            delay_ms = fault.m_delay_response_n_->second;
        }
        if (duplicate_ctr) {
            // 重复 CTR：本响应复用上一个响应的 CTR 值（计数器本身仍已递增，
            // 因此后续响应继续按正常序列推进，不会永久卡住）
            ctr = static_cast<DatagramCtr>(ctr - 1U);
        }
    }

    if (drop) {
        return;  // 模拟丢包：不发送任何响应
    }
    if (delay_ms > 0U) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
    if (jump_ctr) {
        ctr = static_cast<DatagramCtr>(ctr + 5U);  // 制造缺口
    }

    Bytes datagram;
    if (corrupt_len) {
        // 错误 LEN：Header 声明长度与实际不符，Master 应整体丢弃该 Datagram
        datagram.resize(kUdpHeaderSize + xcp_packet.size());
        datagram[0] =
            static_cast<std::uint8_t>((xcp_packet.size() + 9U) & 0xFFU);
        datagram[1] =
            static_cast<std::uint8_t>(((xcp_packet.size() + 9U) >> 8) & 0xFFU);
        datagram[2] = static_cast<std::uint8_t>(ctr & 0xFFU);
        datagram[3] = static_cast<std::uint8_t>((ctr >> 8) & 0xFFU);
        std::memcpy(datagram.data() + kUdpHeaderSize, xcp_packet.data(),
                    xcp_packet.size());
    } else {
        datagram = encodeUdpFrame(xcp_packet, ctr).m_data_;
    }

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    ::inet_pton(AF_INET, ip.c_str(), &dst.sin_addr);
    ::sendto(m_socket_->handle, reinterpret_cast<const char*>(datagram.data()),
             static_cast<int>(datagram.size()), 0,
             reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
}

void UdpTestSlave::SendRawPayloadTo(BytesView payload,
                                    const std::string& dst_ip,
                                    std::uint16_t dst_port) {
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(dst_port);
    ::inet_pton(AF_INET, dst_ip.c_str(), &dst.sin_addr);
    const int sent = ::sendto(
        m_socket_->handle, reinterpret_cast<const char*>(payload.data()),
        static_cast<int>(payload.size()), 0,
        reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
    if (sent < 0 || static_cast<std::size_t>(sent) != payload.size()) {
        throw detail::MakeTransportError("UdpTestSlave 发送原始 Payload 失败");
    }
}

void UdpTestSlave::SendRawPayload(BytesView payload) {
    std::string ip;
    std::uint16_t port = 0;
    {
        const std::lock_guard<std::mutex> lock(m_state_mutex_);
        if (!m_connect_source_ip_ || !m_connect_source_port_) {
            throw detail::MakeInvalidState(
                "UdpTestSlave 尚未记录 CONNECT 来源端点");
        }
        ip = *m_connect_source_ip_;
        port = *m_connect_source_port_;
    }
    SendRawPayloadTo(payload, ip, port);
}

void UdpTestSlave::SendPackedFrames(std::span<const BytesView> xcp_packets) {
    std::string ip;
    std::uint16_t port = 0;
    {
        const std::lock_guard<std::mutex> lock(m_state_mutex_);
        if (!m_connect_source_ip_ || !m_connect_source_port_) {
            throw detail::MakeInvalidState(
                "UdpTestSlave 尚未记录 CONNECT 来源端点");
        }
        ip = *m_connect_source_ip_;
        port = *m_connect_source_port_;
    }

    Bytes datagram;
    {
        const std::lock_guard<std::mutex> lock(m_state_mutex_);
        for (const auto& packet : xcp_packets) {
            if (packet.size() > kUdpMaxXcpPacket) {
                throw detail::MakeInvalidArgument(
                    "SendPackedFrames: Frame 超过单包上限");
            }
            // 每个 Frame 独立消耗一个 CTR（设计 §15.3 第 5 条）
            const auto ctr = m_send_ctr_;
            ++m_send_ctr_;
            const auto frame = encodeUdpFrame(packet, ctr);
            if (datagram.size() + frame.m_data_.size() > kUdpMaxDatagramSize) {
                throw detail::MakeInvalidArgument(
                    "SendPackedFrames: Datagram 总长超过上限");
            }
            datagram.insert(datagram.end(), frame.m_data_.begin(),
                            frame.m_data_.end());
        }
    }

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    ::inet_pton(AF_INET, ip.c_str(), &dst.sin_addr);
    const int sent = ::sendto(
        m_socket_->handle, reinterpret_cast<const char*>(datagram.data()),
        static_cast<int>(datagram.size()), 0,
        reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
    if (sent < 0 || static_cast<std::size_t>(sent) != datagram.size()) {
        throw detail::MakeTransportError("UdpTestSlave 发送打包 Datagram 失败");
    }
}

}  // namespace calmcar::xcp::test
