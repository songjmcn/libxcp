/**
 * @file udp_test_slave.cpp
 * @brief UdpTestSlave 实现：最小 XCP Slave 语义 + XCP 1.1 UDP 端点绑定 +
 * 故障注入。
 */

#include "udp_test_slave.hpp"

#include <algorithm>
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
/// @brief Seed/Key 分段的固定头部字节数：PID(1) + Length(1)
constexpr std::size_t kSeedKeyHeaderBytes = 2U;
/// @brief 支持 Seed&Key 的资源位集合（CAL/PAG | DAQ | STIM | PGM）
constexpr std::uint8_t kSupportedResourceBits = 0x1DU;
/// @brief 测试默认 Seed 内容（4 字节，单帧即可容纳）
constexpr std::uint8_t kDefaultSeed[] = {0x01, 0x02, 0x03, 0x04};

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

UdpTestSlave::UdpTestSlave(std::uint16_t fixed_port) {
    auto impl = std::make_unique<SocketImpl>();
    impl->handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl->handle == kInvalidSocket) {
        throw detail::MakeTransportError("测试 Slave 创建 Socket 失败");
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(fixed_port);  // 0=OS 临时端口；非0=固定端口（E2E）
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
    // 先 join 再关 Socket：接收线程可能仍在 sendto() 响应，若在 join 之前关闭
    // Socket，其句柄号会被 OS 立即回收并分配给其他测试新建的 Socket，
    // 导致这个迟到的 sendto 把包投递到别的端点（跨用例串扰）。
    // recvfrom 带 SO_RCVTIMEO，最多阻塞一个轮询周期后即可看到停止标记。
    if (m_receive_thread_.joinable()) {
        m_receive_thread_.join();
    }
    if (m_socket_ && m_socket_->handle != kInvalidSocket) {
        closeSocket(m_socket_->handle);
        m_socket_->handle = kInvalidSocket;
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

void UdpTestSlave::SetProtectedResources(ResourceMask protected_resources) {
    const std::lock_guard<std::mutex> lock(m_state_mutex_);
    m_protected_resources_ = protected_resources;
    // 配置变化后清空解锁位与进行中的 Seed/Key 序列，避免新旧状态错配
    m_unlocked_resources_ = 0;
    m_seed_in_progress_ = false;
    m_seed_resource_ = Resource::None;
    m_seed_offset_ = 0;
    m_key_buffer_.clear();
    m_key_total_ = 0;
    m_key_received_ = 0;
    m_key_prev_length_ = 0;
}

void UdpTestSlave::SetSeedContent(const Bytes& seed_content) {
    const std::lock_guard<std::mutex> lock(m_state_mutex_);
    m_seed_content_ = seed_content.empty() ? Bytes{std::begin(kDefaultSeed),
                                                   std::end(kDefaultSeed)}
                                           : seed_content;
    // 内容变化后作废进行中的序列，避免偏移指向已失效的内容
    m_seed_in_progress_ = false;
    m_seed_offset_ = 0;
}

ResourceMask UdpTestSlave::EffectiveProtection() const {
    // 当前仍生效的保护 = 配置的保护位中尚未成功解锁的部分
    return static_cast<ResourceMask>(m_protected_resources_ &
                                     ~m_unlocked_resources_);
}

Bytes TestKeyAlgorithm(Resource resource, BytesView seed) {
    // resource 仅占位以匹配 SeedKeyCalculator 签名（测试算法与资源无关）
    (void)resource;
    // 简单可逆的确定性算法（仅供测试）：key[i] = seed[i] ^ 0x5A ^ i 低 8 位
    Bytes key;
    key.reserve(seed.size());
    for (std::size_t i = 0; i < seed.size(); ++i) {
        key.push_back(static_cast<std::uint8_t>(seed[i] ^ 0x5AU ^
                                                static_cast<std::uint8_t>(i)));
    }
    return key;
}

std::size_t UdpTestSlave::CommandCount() const {
    const std::lock_guard<std::mutex> lock(m_state_mutex_);
    return m_command_count_;
}

DatagramCtr UdpTestSlave::NextSendCtr() const {
    const std::lock_guard<std::mutex> lock(m_state_mutex_);
    return m_send_ctr_;
}

// ---------------------------------------------------------------------------
// 报文构造辅助
// ---------------------------------------------------------------------------

Bytes UdpTestSlave::MakeRes(std::initializer_list<std::uint8_t> body) {
    Bytes packet;
    packet.reserve(1 + body.size());
    packet.push_back(static_cast<std::uint8_t>(PacketType::Res));
    packet.insert(packet.end(), body.begin(), body.end());
    return packet;
}

Bytes UdpTestSlave::MakeErr(ErrorCode code) {
    return Bytes{static_cast<std::uint8_t>(PacketType::Err),
                 static_cast<std::uint8_t>(code)};
}

bool UdpTestSlave::AdvanceMta(ElementCount elements) {
    const auto bytes = static_cast<std::uint64_t>(elements) * kTestAgBytes;
    const auto sum = static_cast<std::uint64_t>(m_mta_) + bytes;
    if (sum > 0xFFFFFFFFULL) {
        return false;
    }
    m_mta_ = static_cast<Address>(sum);
    return true;
}

std::optional<Bytes> UdpTestSlave::ReadAtMta(ElementCount elements) {
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
// Seed&Key 模拟（批次 7）
// ---------------------------------------------------------------------------

Bytes UdpTestSlave::HandleGetSeed(BytesView xcp_packet) {
    // GET_SEED: [F8][mode][resource]
    if (xcp_packet.size() < 3U) {
        return MakeErr(ErrorCode::CmdSyntax);
    }
    const auto mode = xcp_packet[1];
    const auto resource_byte = xcp_packet[2];

    if (mode == 0U) {
        // Mode=0：资源必须恰为单个支持位，否则 ERR_OUT_OF_RANGE（规范
        // §7.5.1.8）
        const bool is_single_bit =
            resource_byte != 0U && (resource_byte & (resource_byte - 1U)) == 0U;
        if (!is_single_bit || (resource_byte & ~kSupportedResourceBits) != 0U) {
            return MakeErr(ErrorCode::OutOfRange);
        }
        if ((m_protected_resources_ & resource_byte) == 0U) {
            // 资源未保护：Length=0 表示无需 UNLOCK，且不启动分段序列
            return MakeRes({0x00});
        }
        // 启动（或重启）分段序列并记录目标资源
        m_seed_in_progress_ = true;
        m_seed_resource_ = static_cast<Resource>(resource_byte);
        m_seed_offset_ = 0;
    } else if (!m_seed_in_progress_) {
        // 未先 Mode=0 就请求续段 → ERR_SEQUENCE（规范 §7.5.1.8）
        return MakeErr(ErrorCode::Sequence);
    }

    // 按 MAX_CTO-2 发送当前偏移处的 Seed 分段
    const auto total = m_seed_content_.size();
    const auto remaining = total - m_seed_offset_;
    const auto segment = std::min<std::size_t>(
        remaining, static_cast<std::size_t>(kTestMaxCto) - kSeedKeyHeaderBytes);
    // Length 字段：Mode=0 帧=Seed 总长，Mode=1 帧=发送前剩余长度
    const auto length_field = static_cast<std::uint8_t>(total - m_seed_offset_);

    Bytes response;
    response.reserve(2 + segment);
    response.push_back(static_cast<std::uint8_t>(PacketType::Res));
    response.push_back(length_field);
    response.insert(
        response.end(),
        m_seed_content_.begin() + static_cast<std::ptrdiff_t>(m_seed_offset_),
        m_seed_content_.begin() +
            static_cast<std::ptrdiff_t>(m_seed_offset_ + segment));
    m_seed_offset_ += segment;
    if (m_seed_offset_ >= total) {
        // 分段读取完成：复位序列标记，此后多余的 Mode=1 将收到 ERR_SEQUENCE
        m_seed_in_progress_ = false;
    }
    return response;
}

Bytes UdpTestSlave::HandleUnlock(BytesView xcp_packet) {
    // UNLOCK: [F7][length][key...]
    if (xcp_packet.size() < 2U) {
        return MakeErr(ErrorCode::CmdSyntax);
    }
    const auto length_field = xcp_packet[1];
    const BytesView key_part = xcp_packet.subspan(2);

    // 首帧判定（与 OpenBLT 同款语义）：Length >= 上帧 Length 视为新 Key
    // 序列的开始（首帧=总长必然大于此前的剩余长度）
    if (m_key_received_ == 0U || length_field >= m_key_prev_length_) {
        m_key_total_ = length_field;
        m_key_buffer_.clear();
        m_key_received_ = 0;
    }
    m_key_prev_length_ = length_field;

    const auto segment = std::min<std::size_t>(
        key_part.size(),
        static_cast<std::size_t>(kTestMaxCto) - kSeedKeyHeaderBytes);
    m_key_buffer_.insert(
        m_key_buffer_.end(), key_part.begin(),
        key_part.begin() + static_cast<std::ptrdiff_t>(segment));
    m_key_received_ += segment;

    if (m_key_received_ >= m_key_total_) {
        // 全部 Key 字节收满后才校验（规范 §7.5.1.9）
        m_key_prev_length_ = 0;
        m_key_total_ = 0;
        m_key_received_ = 0;
        if (m_key_buffer_ !=
            TestKeyAlgorithm(m_seed_resource_, BytesView{m_seed_content_})) {
            // Key 错误：ERR_ACCESS_LOCKED 且 Slave 主动断开会话
            Bytes err = MakeErr(ErrorCode::AccessLocked);
            m_connected_ = false;
            m_seed_in_progress_ = false;
            m_seed_resource_ = Resource::None;
            m_key_buffer_.clear();
            return err;
        }
        // 解锁成功：目标资源来自最近一次 Mode=0 记录的资源
        m_unlocked_resources_ |= static_cast<ResourceMask>(m_seed_resource_);
        m_seed_resource_ = Resource::None;
        m_key_buffer_.clear();
    }
    // 每帧 UNLOCK 均返回当前生效的保护掩码（末帧为解锁后的最终值）
    return MakeRes({EffectiveProtection()});
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
        const auto frames = DecodeUdpDatagram(
            BytesView{buffer.data(), static_cast<std::size_t>(received)});
        if (!frames) {
            continue;  // 畸形 Datagram：测试 Slave 直接忽略
        }
        for (const auto& frame : *frames) {
            HandleCommand(frame.xcp_packet, src_ip, src_port);
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
            // 新会话：清空已解锁位与 Seed/Key 序列状态（受保护配置保留，
            // 资源在每次新会话上恢复锁定）
            m_unlocked_resources_ = 0;
            m_seed_in_progress_ = false;
            m_seed_resource_ = Resource::None;
            m_seed_offset_ = 0;
            m_key_buffer_.clear();
            m_key_total_ = 0;
            m_key_received_ = 0;
            m_key_prev_length_ = 0;
            // RESOURCE=0x15(CAL/PAG+DAQ+PGM),
            // COMM_MODE_BASIC=0xC0(Intel/BYTE/Block/Optional), MAX_CTO=8,
            // MAX_DTO=8(小端), ProtoVer=0x10, TransportVer=0x10
            response =
                MakeRes({0x15, 0xC0, kTestMaxCto,
                         static_cast<std::uint8_t>(kTestMaxDto & 0xFFU),
                         static_cast<std::uint8_t>((kTestMaxDto >> 8) & 0xFFU),
                         0x10, 0x10});
            destination = std::make_pair(source_ip, source_port);
        } else if (!m_connected_ || !IsCurrentSessionSource(source_ip)) {
            // 未建立会话，或来源 IP 不是 CONNECT 来源：忽略该命令（D8）
            return;
        } else if (cmd == static_cast<std::uint8_t>(CommandCode::Disconnect)) {
            m_connected_ = false;
            response = MakeRes({});
            destination =
                std::make_pair(*m_connect_source_ip_, *m_connect_source_port_);
        } else if (cmd == static_cast<std::uint8_t>(CommandCode::GetStatus)) {
            // Session Status=0x00, Protection=当前生效掩码（批次 7 动态化）,
            // STATE_NUMBER=0x01, ConfigID=0x0007(小端)
            response = MakeRes({0x00, EffectiveProtection(), 0x01, 0x07, 0x00});
            destination =
                std::make_pair(*m_connect_source_ip_, *m_connect_source_port_);
        } else if (cmd == static_cast<std::uint8_t>(CommandCode::Synch)) {
            // SYNCH 始终以 ERR_CMD_SYNCH 应答
            response = MakeErr(ErrorCode::CmdSynch);
            destination =
                std::make_pair(*m_connect_source_ip_, *m_connect_source_port_);
        } else if (cmd ==
                   static_cast<std::uint8_t>(CommandCode::GetCommModeInfo)) {
            // reserved, COMM_MODE_OPTIONAL, reserved, MAX_BS, MIN_ST,
            // QUEUE_SIZE, DRIVER_VER
            response = MakeRes({0x00, 0x0E, 0x00, 0x04, 0x02, 0x08, 0x13});
            destination =
                std::make_pair(*m_connect_source_ip_, *m_connect_source_port_);
        } else if (cmd == static_cast<std::uint8_t>(CommandCode::GetSeed)) {
            // GET_SEED: [F8][mode][resource]（批次 7 Seed&Key）
            response = HandleGetSeed(xcp_packet);
            destination =
                std::make_pair(*m_connect_source_ip_, *m_connect_source_port_);
        } else if (cmd == static_cast<std::uint8_t>(CommandCode::Unlock)) {
            // UNLOCK: [F7][length][key...]（批次 7 Seed&Key）
            response = HandleUnlock(xcp_packet);
            destination =
                std::make_pair(*m_connect_source_ip_, *m_connect_source_port_);
        } else if (cmd == static_cast<std::uint8_t>(CommandCode::SetMta)) {
            if (xcp_packet.size() < 7U) {
                response = MakeErr(ErrorCode::CmdSyntax);
            } else {
                m_mta_extension_ = xcp_packet[2];
                // Address 按 Session Byte Order（测试 Slave 固定 Intel 小端）
                m_mta_ = static_cast<Address>(xcp_packet[3]) |
                         (static_cast<Address>(xcp_packet[4]) << 8) |
                         (static_cast<Address>(xcp_packet[5]) << 16) |
                         (static_cast<Address>(xcp_packet[6]) << 24);
                response = MakeRes({});
            }
            destination =
                std::make_pair(*m_connect_source_ip_, *m_connect_source_port_);
        } else if (cmd == static_cast<std::uint8_t>(CommandCode::Upload)) {
            if (xcp_packet.size() < 2U) {
                response = MakeErr(ErrorCode::CmdSyntax);
            } else {
                const auto elements = static_cast<ElementCount>(xcp_packet[1]);
                if (elements == 0U ||
                    elements + 1U > kTestMaxCto / kTestAgBytes) {
                    response = MakeErr(ErrorCode::OutOfRange);
                } else if (EffectiveProtection() != 0U) {
                    // 存在未解锁的受保护资源：读操作按 ERR_ACCESS_LOCKED
                    // 拒绝（批次 7，供端到端解锁用例使用）
                    response = MakeErr(ErrorCode::AccessLocked);
                } else if (auto data = ReadAtMta(elements)) {
                    // UPLOAD 的 RES 为 [FF][data...]，长度 ==
                    // elements*AG，不含计数字节 （依据
                    // docs/XCP_1.3.0_document.md §12.4 示例：UPLOAD(6) -> 6
                    // 字节 "XCPSIM"）
                    response.clear();
                    response.push_back(
                        static_cast<std::uint8_t>(PacketType::Res));
                    response.insert(response.end(), data->begin(), data->end());
                    AdvanceMta(elements);
                } else {
                    response = MakeErr(ErrorCode::AccessDenied);
                }
            }
            destination =
                std::make_pair(*m_connect_source_ip_, *m_connect_source_port_);
        } else if (cmd == static_cast<std::uint8_t>(CommandCode::ShortUpload)) {
            if (xcp_packet.size() < 8U) {
                response = MakeErr(ErrorCode::CmdSyntax);
            } else {
                const auto elements = static_cast<ElementCount>(xcp_packet[1]);
                const auto extension = xcp_packet[3];
                const auto address =
                    static_cast<Address>(xcp_packet[4]) |
                    (static_cast<Address>(xcp_packet[5]) << 8) |
                    (static_cast<Address>(xcp_packet[6]) << 16) |
                    (static_cast<Address>(xcp_packet[7]) << 24);
                if (elements == 0U || elements > kTestMaxCto / kTestAgBytes) {
                    response = MakeErr(ErrorCode::OutOfRange);
                } else if (EffectiveProtection() != 0U) {
                    // 存在未解锁的受保护资源：读操作按 ERR_ACCESS_LOCKED
                    // 拒绝（批次 7，供端到端解锁用例使用）
                    response = MakeErr(ErrorCode::AccessLocked);
                } else {
                    const Address saved_mta = m_mta_;
                    const AddressExtension saved_ext = m_mta_extension_;
                    m_mta_ = address;
                    m_mta_extension_ = extension;
                    if (auto data = ReadAtMta(elements)) {
                        // SHORT_UPLOAD 的 RES 同样为 [FF][data...]
                        // （依据 docs/XCP_1.3.0_document.md §12.5：size=4 -> 4
                        // 字节数据）
                        response.clear();
                        response.push_back(
                            static_cast<std::uint8_t>(PacketType::Res));
                        response.insert(response.end(), data->begin(),
                                        data->end());
                        // SHORT_UPLOAD 之后 MTA 仍前进到数据块末尾之后
                        AdvanceMta(elements);
                    } else {
                        response = MakeErr(ErrorCode::AccessDenied);
                        m_mta_ = saved_mta;
                        m_mta_extension_ = saved_ext;
                    }
                }
            }
            destination =
                std::make_pair(*m_connect_source_ip_, *m_connect_source_port_);
        } else {
            response = MakeErr(ErrorCode::CmdUnknown);
            destination =
                std::make_pair(*m_connect_source_ip_, *m_connect_source_port_);
        }
    }

    if (response.empty() || !destination) {
        return;
    }
    SendResponseTo(BytesView{response}, destination->first,
                   destination->second);
}

void UdpTestSlave::SendResponse(BytesView xcp_packet) {
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
    SendResponseTo(xcp_packet, ip, port);
}

void UdpTestSlave::SendResponseTo(BytesView xcp_packet, const std::string& ip,
                                  std::uint16_t port) {
    DatagramCtr ctr = 0;
    FaultInjection fault;
    bool drop = false;
    std::uint32_t delay_ms = 0;
    bool corrupt_len = false;
    bool jump_ctr = false;
    bool duplicate_ctr = false;
    int ctr_offset = 0;

    {
        const std::lock_guard<std::mutex> lock(m_state_mutex_);
        ctr = m_send_ctr_;
        ++m_send_ctr_;
        ++m_response_count_;
        fault = m_fault_;
        const std::size_t n = m_response_count_;

        drop = fault.drop_response_n && *fault.drop_response_n == n;
        corrupt_len = fault.corrupt_len_n && *fault.corrupt_len_n == n;
        jump_ctr = fault.jump_ctr_n && *fault.jump_ctr_n == n;
        duplicate_ctr = fault.duplicate_ctr_n && *fault.duplicate_ctr_n == n;
        if (fault.delay_response_n && fault.delay_response_n->first == n) {
            delay_ms = fault.delay_response_n->second;
        }
        if (fault.ctr_offset_n && fault.ctr_offset_n->first == n) {
            // 精确偏移：按模 65536 施加有符号增量，可构造任意 CTR 差值（D5）
            ctr_offset = fault.ctr_offset_n->second;
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
    if (ctr_offset != 0) {
        // 先加 65536 保证中间值为正，再取模，使负偏移也得到正确的回绕结果
        ctr = static_cast<DatagramCtr>(
            (static_cast<long>(ctr) + 65536L + ctr_offset) % 65536L);
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
        datagram = EncodeUdpFrame(xcp_packet, ctr).data;
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
            const auto frame = EncodeUdpFrame(packet, ctr);
            if (datagram.size() + frame.data.size() > kUdpMaxDatagramSize) {
                throw detail::MakeInvalidArgument(
                    "SendPackedFrames: Datagram 总长超过上限");
            }
            datagram.insert(datagram.end(), frame.data.begin(),
                            frame.data.end());
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
