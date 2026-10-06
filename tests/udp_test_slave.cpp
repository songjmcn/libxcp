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

// ---- DAQ / DOWNLOAD 模拟参数（批次14，T14-09）----
// 只影响显式开启 DAQ 模拟的用例；默认关闭时这些常量完全不参与。

/// @brief 模拟 Slave 支持的 DAQ List 数（EPK 0..kTestDaqCount-1）
constexpr std::uint16_t kTestDaqCount = 4U;
/// @brief 每个 DAQ List 支持的 ODT 上限（GET_DAQ_LIST_INFO 的 MAX_ODT）
constexpr std::uint8_t kTestMaxOdt = 4U;
/// @brief 每个 ODT 支持的 Entry 上限（GET_DAQ_LIST_INFO 的 MAX_ODT_ENTRIES）
constexpr std::uint8_t kTestMaxOdtEntries = 8U;
/// @brief FIRST_PID 基址：list n 的 FIRST_PID = kTestFirstPidBase +
/// n*kTestMaxOdt
/// @details Absolute ODT Number 模式下 PID 全局唯一（docs L1421-1434），
///          每个列表预留 kTestMaxOdt 个连续 PID，避免列表间串号。
constexpr std::uint8_t kTestFirstPidBase = 0x10U;
/// @brief GET_DAQ_RESOLUTION_INFO 的 ODT Entry 粒度（字节）
constexpr std::uint8_t kTestDaqGranularity = 1U;
/// @brief GET_DAQ_RESOLUTION_INFO 的 ODT Entry 上限（字节）
constexpr std::uint8_t kTestDaqMaxEntrySize = 8U;
/// @brief GET_DAQ_RESOLUTION_INFO 的 TIMESTAMP_TICKS（时间戳节拍）
constexpr std::uint16_t kTestDaqTimestampTicks = 1000U;
/// @brief TIMESTAMP_MODE：非强制、位宽编码 1（1=BYTE）、单位编码 3
/// @details 单位编码只按原值回显，不做 μs 换算（R13 无权威码表）
constexpr std::uint8_t kTestDaqTimestampMode = 0x31U;
/// @brief DOWNLOAD / SHORT_DOWNLOAD 的 CTO 头字节数
constexpr std::size_t kDownloadHeaderBytes = 2U;
constexpr std::size_t kShortDownloadHeaderBytes = 8U;

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
// DAQ / DOWNLOAD 模拟（批次14，T14-09）
//
// 默认关闭：SetDaqSimulationEnabled(true) 之前，所有 DAQ 命令码与 DOWNLOAD
// 一律走既有的 `else → ERR_CMD_UNKNOWN` 分支，既有 253 项用例语义零变化。
// ---------------------------------------------------------------------------

void UdpTestSlave::SetDaqSimulationEnabled(bool enabled) {
    const std::lock_guard<std::mutex> lock(m_state_mutex_);
    m_daq_enabled_ = enabled;
    if (!enabled) {
        // 关闭时清空配置，避免"关过一次再开"残留旧布局
        m_daq_lists_.clear();
        m_daq_ptr_ = SlaveDaqPtr{};
        m_dto_ctr_ = 0U;
    }
}

bool UdpTestSlave::DaqSimulationEnabled() const {
    const std::lock_guard<std::mutex> lock(m_state_mutex_);
    return m_daq_enabled_;
}

void UdpTestSlave::SetDaqListPredefined(std::uint16_t daq_list,
                                        bool predefined) {
    const std::lock_guard<std::mutex> lock(m_state_mutex_);
    MutableDaqList(daq_list).predefined = predefined;
}

UdpTestSlave::SlaveDaqList& UdpTestSlave::MutableDaqList(
    std::uint16_t daq_list) {
    auto it = m_daq_lists_.find(daq_list);
    if (it == m_daq_lists_.end()) {
        SlaveDaqList list;
        // FIRST_PID：list n 的首个 ODT 号（预留 kTestMaxOdt 个连续 PID）
        list.first_pid = static_cast<std::uint8_t>(kTestFirstPidBase +
                                                   daq_list * kTestMaxOdt);
        it = m_daq_lists_.emplace(daq_list, std::move(list)).first;
    }
    return it->second;
}

bool UdpTestSlave::WriteAtAddress(Address address, BytesView data) {
    const std::lock_guard<std::mutex> lock(m_memory_mutex_);
    auto it = m_memory_.find(address);
    if (it == m_memory_.end()) {
        // 新块：写开辟一块模拟内存（等价于 ECU 在已分配段内写，测试语义）
        m_memory_.emplace(address, Bytes(data.begin(), data.end()));
        return true;
    }
    Bytes& block = it->second;
    if (static_cast<std::size_t>(address - it->first) + data.size() >
        block.size()) {
        // 与 READ 侧同一口径：跨块写入视为不可写（禁止"拆开写"的猜测）
        return false;
    }
    const std::size_t offset = static_cast<std::size_t>(address - it->first);
    std::copy(data.begin(), data.end(),
              block.begin() + static_cast<std::ptrdiff_t>(offset));
    return true;
}

std::optional<Bytes> UdpTestSlave::ReadDaqEntryPayload(
    const SlaveDaqEntry& entry) const {
    const auto want = static_cast<std::size_t>(entry.size) * kTestAgBytes;
    if (want == 0U) {
        return std::nullopt;
    }
    const std::lock_guard<std::mutex> lock(m_memory_mutex_);
    for (const auto& [base, content] : m_memory_) {
        if (entry.address >= base &&
            entry.address < base + static_cast<Address>(content.size())) {
            const auto offset = static_cast<std::size_t>(entry.address - base);
            if (offset + want > content.size()) {
                return std::nullopt;  // 跨块：不猜，直接判不可读
            }
            return Bytes(
                content.begin() + static_cast<std::ptrdiff_t>(offset),
                content.begin() + static_cast<std::ptrdiff_t>(offset + want));
        }
    }
    return std::nullopt;
}

UdpTestSlave::DaqDispatchResult UdpTestSlave::HandleDaqOrDownload(
    BytesView xcp_packet) {
    // 返回 handled=false 时，调用方沿用既有 ERR_CMD_UNKNOWN 分支
    DaqDispatchResult out;
    const std::uint8_t cmd = xcp_packet[0];
    const auto dest = [&](Bytes resp) {
        out.handled = true;
        out.response = std::move(resp);
        return out;
    };
    // 本函数内所有地址解码都按 Session 字节序（测试 Slave 固定 Intel 小端）。
    // 注意：unsigned 短类型参与 | 与 << 会被整型提升为 int，必须在外层再
    // static_cast 回目标类型，否则 brace-init 会触发收缩转换错误。
    const auto read_u16 = [&](std::size_t at) -> std::uint16_t {
        return static_cast<std::uint16_t>(
            static_cast<unsigned>(xcp_packet[at]) |
            (static_cast<unsigned>(xcp_packet[at + 1]) << 8));
    };
    const auto read_u32 = [&](std::size_t at) -> Address {
        return static_cast<Address>(
            static_cast<std::uint64_t>(xcp_packet[at]) |
            (static_cast<std::uint64_t>(xcp_packet[at + 1]) << 8) |
            (static_cast<std::uint64_t>(xcp_packet[at + 2]) << 16) |
            (static_cast<std::uint64_t>(xcp_packet[at + 3]) << 24));
    };

    switch (cmd) {
        case static_cast<std::uint8_t>(CommandCode::ClearDaqList): {
            // [E3][reserved][DAQ(WORD)]
            if (xcp_packet.size() < 4U) {
                return dest(MakeErr(ErrorCode::CmdSyntax));
            }
            const auto daq = read_u16(2);
            if (daq >= kTestDaqCount) {
                return dest(MakeErr(ErrorCode::OutOfRange));
            }
            auto& list = MutableDaqList(daq);
            // docs L2437-2444：复位为"无 Entry、BIT_OFFSET=0xFF"，并停止传输
            list.odts.clear();
            list.running = false;
            list.selected = false;
            m_daq_ptr_ = SlaveDaqPtr{daq, std::uint8_t{0}, std::uint8_t{0}};
            return dest(MakeRes({}));
        }
        case static_cast<std::uint8_t>(CommandCode::SetDaqPtr): {
            // [E2][reserved][DAQ(WORD)][ODT][ENTRY]
            if (xcp_packet.size() < 6U) {
                return dest(MakeErr(ErrorCode::CmdSyntax));
            }
            const auto daq = read_u16(2);
            const auto odt = xcp_packet[4];
            const auto entry = xcp_packet[5];
            if (daq >= kTestDaqCount || odt >= kTestMaxOdt ||
                entry >= kTestMaxOdtEntries) {
                // docs L2153：指定对象不存在 → ERR_OUT_OF_RANGE
                return dest(MakeErr(ErrorCode::OutOfRange));
            }
            m_daq_ptr_ = SlaveDaqPtr{daq, odt, entry};
            return dest(MakeRes({}));
        }
        case static_cast<std::uint8_t>(CommandCode::WriteDaq): {
            // [E1][BIT_OFFSET][SIZE][EXT][ADDR(DWORD)]
            if (xcp_packet.size() < 8U) {
                return dest(MakeErr(ErrorCode::CmdSyntax));
            }
            // 注意：必须走 MutableDaqList()，直接用 operator[] 会绕过
            // FIRST_PID 的分配规则（list n → 0x10 + n*kTestMaxOdt）
            const SlaveDaqList& list = MutableDaqList(m_daq_ptr_.daq);
            if (list.predefined) {
                // docs L2168：指向 PREDEFINED 列表 → ERR_WRITE_PROTECTED
                return dest(MakeErr(ErrorCode::WriteProtected));
            }
            const auto entry_size = xcp_packet[2];
            if (entry_size == 0U) {
                return dest(MakeErr(ErrorCode::OutOfRange));
            }
            auto& target = MutableDaqList(m_daq_ptr_.daq);
            while (target.odts.size() <= m_daq_ptr_.odt) {
                target.odts.emplace_back();
            }
            auto& odt = target.odts[m_daq_ptr_.odt];
            while (odt.size() <= m_daq_ptr_.entry) {
                odt.emplace_back();
            }
            odt[m_daq_ptr_.entry] = SlaveDaqEntry{xcp_packet[1], entry_size,
                                                  xcp_packet[3], read_u32(4)};
            // docs L2172：写成功后指针在同 ODT 内自增；越过末位后指针未定义
            // → 本模拟把 entry 停在末位（不自增到越界），由 Master 显式
            //   SET_DAQ_PTR（真实 ECU 行为各异，这里取可预测的一种）
            if (m_daq_ptr_.entry + 1U < kTestMaxOdtEntries) {
                ++m_daq_ptr_.entry;
            }
            return dest(MakeRes({}));
        }
        case static_cast<std::uint8_t>(CommandCode::ReadDaq): {
            // [DB]；RES [FF][BITOFFSET][SIZE][EXT][ADDR(DWORD)]
            if (m_daq_ptr_.daq >= kTestDaqCount) {
                return dest(MakeErr(ErrorCode::OutOfRange));
            }
            const auto it = m_daq_lists_.find(m_daq_ptr_.daq);
            if (it == m_daq_lists_.end() ||
                m_daq_ptr_.odt >= it->second.odts.size() ||
                m_daq_ptr_.entry >= it->second.odts[m_daq_ptr_.odt].size()) {
                // 指针指向不存在的 Entry：不猜，回 ERR_OUT_OF_RANGE
                return dest(MakeErr(ErrorCode::OutOfRange));
            }
            const auto& e = it->second.odts[m_daq_ptr_.odt][m_daq_ptr_.entry];
            Bytes body;
            body.push_back(e.bit_offset);
            body.push_back(e.size);
            body.push_back(e.extension);
            body.push_back(static_cast<std::uint8_t>(e.address & 0xFFU));
            body.push_back(static_cast<std::uint8_t>((e.address >> 8) & 0xFFU));
            body.push_back(
                static_cast<std::uint8_t>((e.address >> 16) & 0xFFU));
            body.push_back(
                static_cast<std::uint8_t>((e.address >> 24) & 0xFFU));
            Bytes resp;
            resp.push_back(static_cast<std::uint8_t>(PacketType::Res));
            resp.insert(resp.end(), body.begin(), body.end());
            if (m_daq_ptr_.entry + 1U < kTestMaxOdtEntries) {
                ++m_daq_ptr_.entry;
            }
            return dest(std::move(resp));
        }
        case static_cast<std::uint8_t>(CommandCode::SetDaqListMode): {
            // [E0][MODE][DAQ(WORD)][EVENT(WORD)][PRESCALER][PRIORITY]
            if (xcp_packet.size() < 8U) {
                return dest(MakeErr(ErrorCode::CmdSyntax));
            }
            const auto daq = read_u16(2);
            if (daq >= kTestDaqCount) {
                return dest(MakeErr(ErrorCode::OutOfRange));
            }
            const auto mode = xcp_packet[1];
            // docs L2198：ALTERNATING 与 TIMESTAMP 不能同时置位
            if ((mode & 0x01U) != 0U && (mode & 0x10U) != 0U) {
                return dest(MakeErr(ErrorCode::CmdSyntax));
            }
            auto& list = MutableDaqList(daq);
            list.mode = mode;
            list.event_channel = read_u16(4);
            list.prescaler = xcp_packet[6];
            list.priority = xcp_packet[7];
            return dest(
                MakeRes({mode, static_cast<std::uint8_t>(daq & 0xFFU),
                         static_cast<std::uint8_t>(daq >> 8), 0x00, 0x00}));
        }
        case static_cast<std::uint8_t>(CommandCode::StartStopDaqList): {
            // [DE][MODE][DAQ(WORD)]；RES [FF][FIRST_PID]
            if (xcp_packet.size() < 4U) {
                return dest(MakeErr(ErrorCode::CmdSyntax));
            }
            const auto action = xcp_packet[1];
            const auto daq = read_u16(2);
            if (daq >= kTestDaqCount) {
                return dest(MakeErr(ErrorCode::OutOfRange));
            }
            auto& list = MutableDaqList(daq);
            switch (action) {
                case 0x00U:
                    list.running = false;
                    break;
                case 0x01U:
                    list.running = true;
                    break;
                case 0x02U:
                    list.selected = true;
                    break;
                default:
                    return dest(MakeErr(ErrorCode::OutOfRange));
            }
            return dest(MakeRes({list.first_pid}));
        }
        case static_cast<std::uint8_t>(CommandCode::StartStopSynch): {
            // [DD][MODE]
            if (xcp_packet.size() < 2U) {
                return dest(MakeErr(ErrorCode::CmdSyntax));
            }
            const auto action = xcp_packet[1];
            for (auto& [number, list] : m_daq_lists_) {
                if (action == 0x00U) {
                    list.running = false;  // stop all
                } else if (action == 0x01U && list.selected) {
                    list.running = true;  // start selected
                } else if (action == 0x02U && list.selected) {
                    list.running = false;  // stop selected
                }
            }
            if (action <= 0x02U) {
                if (action != 0x00U) {
                    for (auto& [number, list] : m_daq_lists_) {
                        list.selected =
                            false;  // docs L2442：成功后清除 SELECTED
                    }
                }
                return dest(MakeRes({}));
            }
            return dest(MakeErr(ErrorCode::OutOfRange));
        }
        case static_cast<std::uint8_t>(CommandCode::GetDaqListInfo): {
            // [D8][reserved][DAQ(WORD)]
            // RES [FF][PROPERTIES][MAX_ODT][MAX_ODT_ENTRY][FIXED_EVENT(WORD)]
            if (xcp_packet.size() < 4U) {
                return dest(MakeErr(ErrorCode::CmdSyntax));
            }
            const auto daq = read_u16(2);
            if (daq >= kTestDaqCount) {
                return dest(MakeErr(ErrorCode::OutOfRange));
            }
            const auto& list = MutableDaqList(daq);
            std::uint8_t props = 0x00U;
            if (list.predefined) {
                props |= 0x01U;  // PREDEFINED
            }
            if (list.event_channel != 0U) {
                props |= 0x02U;  // EVENT_FIXED
            }
            props |= (HasDaqMode(static_cast<DaqListModeBit>(list.mode),
                                 DaqListModeBit::kStim))
                         ? 0x08U
                         : 0x04U;  // DIR_STIM / DIR_DAQ
            return dest(
                MakeRes({props, kTestMaxOdt, kTestMaxOdtEntries,
                         static_cast<std::uint8_t>(list.event_channel & 0xFFU),
                         static_cast<std::uint8_t>((list.event_channel >> 8) &
                                                   0xFFU)}));
        }
        case static_cast<std::uint8_t>(CommandCode::GetDaqProcessorInfo): {
            // [DA]；RES [FF][PROPERTIES][MAX_DAQ(WORD)][MAX_EVENT(WORD)]
            //   [MIN_DAQ][DAQ_KEY_BYTE]（docs L2292-2297；xcp.h:790-795）
            // PROPERTIES: bit0 DAQ_CONFIG_TYPE=0（Static；动态 ALLOC 流程不
            //   支持，与 B-5 一致）| bit1 PRESCALER_SUPPORTED
            // DAQ_KEY_BYTE: OPTIMISATION=DEFAULT(0) | ADDRESS_EXTENSION=DAQ
            //   (3<<4) | IDENTIFICATION=ABSOLUTE ODT(0<<6)
            // —— 后两项即 B-16 DAQ 侧比对的运行时真值（docs L1445）
            return dest(MakeRes(
                {0x02U, static_cast<std::uint8_t>(kTestDaqCount & 0xFFU),
                 static_cast<std::uint8_t>(kTestDaqCount >> 8), 0x02U, 0x00U,
                 0x00U, 0x30U}));
        }
        case static_cast<std::uint8_t>(CommandCode::GetDaqResolutionInfo): {
            // [D9]；RES 见 xcp.h:799-805
            return dest(MakeRes(
                {kTestDaqGranularity, kTestDaqMaxEntrySize, kTestDaqGranularity,
                 kTestDaqMaxEntrySize, kTestDaqTimestampMode,
                 static_cast<std::uint8_t>(kTestDaqTimestampTicks & 0xFFU),
                 static_cast<std::uint8_t>((kTestDaqTimestampTicks >> 8) &
                                           0xFFU)}));
        }
        case static_cast<std::uint8_t>(CommandCode::Download): {
            // [F0][SIZE][data...]：从当前 MTA 写，写完 MTA 自增（docs L1983）
            if (xcp_packet.size() < kDownloadHeaderBytes) {
                return dest(MakeErr(ErrorCode::CmdSyntax));
            }
            const auto elements = static_cast<ElementCount>(xcp_packet[1]);
            const auto want = static_cast<std::size_t>(elements) * kTestAgBytes;
            if (elements == 0U ||
                xcp_packet.size() - kDownloadHeaderBytes < want) {
                return dest(MakeErr(ErrorCode::CmdSyntax));
            }
            if (EffectiveProtection() != 0U) {
                // 未解锁的受保护资源：写回同样拒绝（与 UPLOAD 侧一致口径）
                return dest(MakeErr(ErrorCode::AccessLocked));
            }
            const BytesView payload =
                xcp_packet.subspan(kDownloadHeaderBytes, want);
            if (!WriteAtAddress(m_mta_, payload)) {
                return dest(MakeErr(ErrorCode::OutOfRange));
            }
            if (!AdvanceMta(elements)) {
                return dest(MakeErr(ErrorCode::MemoryOverflow));
            }
            return dest(MakeRes({}));
        }
        case static_cast<std::uint8_t>(CommandCode::ShortDownload): {
            // [ED][SIZE][reserved][EXT][ADDR(DWORD)][data...]
            if (xcp_packet.size() < kShortDownloadHeaderBytes) {
                return dest(MakeErr(ErrorCode::CmdSyntax));
            }
            const auto elements = static_cast<ElementCount>(xcp_packet[1]);
            const auto want = static_cast<std::size_t>(elements) * kTestAgBytes;
            const std::size_t available =
                xcp_packet.size() - kShortDownloadHeaderBytes;
            if (elements == 0U || available < want) {
                return dest(MakeErr(ErrorCode::CmdSyntax));
            }
            if (EffectiveProtection() != 0U) {
                return dest(MakeErr(ErrorCode::AccessLocked));
            }
            const auto address = read_u32(4);
            const BytesView payload =
                xcp_packet.subspan(kShortDownloadHeaderBytes, want);
            if (!WriteAtAddress(address, payload)) {
                return dest(MakeErr(ErrorCode::OutOfRange));
            }
            // docs L2022：SHORT_DOWNLOAD 之后 MTA 指向数据块末尾之后
            m_mta_ = address;
            m_mta_extension_ = xcp_packet[3];
            (void)AdvanceMta(elements);
            return dest(MakeRes({}));
        }
        default:
            return out;  // handled=false → 交回既有 ERR_CMD_UNKNOWN 分支
    }
}

std::size_t UdpTestSlave::SendDaqListDtos(std::uint16_t daq_list) {
    // 锁内只组帧，发帧必须在锁外（SendResponseTo 自身要拿 m_state_mutex_）
    std::vector<Bytes> frames;
    std::string ip;
    std::uint16_t port = 0U;
    {
        const std::lock_guard<std::mutex> lock(m_state_mutex_);
        if (!m_daq_enabled_) {
            throw detail::MakeInvalidState(
                "未开启 DAQ 模拟（SetDaqSimulationEnabled），不能发送 DTO");
        }
        if (!m_connect_source_ip_ || !m_connect_source_port_) {
            throw detail::MakeInvalidState(
                "尚无 CONNECT 来源端点，无法发送 DTO");
        }
        const auto it = m_daq_lists_.find(daq_list);
        if (it == m_daq_lists_.end() || it->second.odts.empty()) {
            return 0U;
        }
        const SlaveDaqList& list = it->second;
        for (std::size_t odt = 0; odt < list.odts.size(); ++odt) {
            Bytes frame;
            frame.push_back(static_cast<std::uint8_t>(list.first_pid + odt));
            if (HasDaqMode(static_cast<DaqListModeBit>(list.mode),
                           DaqListModeBit::kDtoCounter)) {
                frame.push_back(m_dto_ctr_);
            }
            if (HasDaqMode(static_cast<DaqListModeBit>(list.mode),
                           DaqListModeBit::kTimestamp)) {
                // 时间戳按 TIMESTAMP_MODE 的位宽编码 1（= 1 字节）发原样节拍
                frame.push_back(0x00U);
            }
            bool readable = true;
            for (const auto& entry : list.odts[odt]) {
                auto payload = ReadDaqEntryPayload(entry);
                if (!payload) {
                    readable = false;
                    break;
                }
                frame.insert(frame.end(), payload->begin(), payload->end());
            }
            if (!readable) {
                continue;  // 取不到净荷的 ODT 不发半帧（禁止伪造数据）
            }
            frames.push_back(std::move(frame));
        }
        ip = *m_connect_source_ip_;
        port = *m_connect_source_port_;
        if (HasDaqMode(static_cast<DaqListModeBit>(list.mode),
                       DaqListModeBit::kDtoCounter)) {
            ++m_dto_ctr_;
        }
    }
    for (const auto& frame : frames) {
        SendResponseTo(BytesView{frame}, ip, port);
    }
    return frames.size();
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
            // 新会话：DAQ
            // 配置与隐含指针归零（批次14；即使模拟未开启也无副作用）
            m_daq_lists_.clear();
            m_daq_ptr_ = SlaveDaqPtr{};
            m_dto_ctr_ = 0U;
            // DAQ 模拟开启时 RESOURCE=0x15(CAL/PAG+DAQ+PGM)，关闭时
            // 清除 DAQ 位为 0x11，避免声明未实现的 Mandatory 资源能力。
            // COMM_MODE_BASIC=0xC0(Intel/BYTE/Block/Optional), MAX_CTO=8,
            // MAX_DTO=8(小端), ProtoVer=0x10, TransportVer=0x10
            const std::uint8_t resource = m_daq_enabled_ ? 0x15U : 0x11U;
            response =
                MakeRes({resource, 0xC0, kTestMaxCto,
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
            // Session Status=DAQ_RUNNING 位（批次14：只在 DAQ 模拟开启且有
            // 列表在跑时置位，docs L2220）, Protection=当前生效掩码（批次 7
            // 动态化）, STATE_NUMBER=0x01, ConfigID=0x0007(小端)
            std::uint8_t session_status = 0x00U;
            for (const auto& [number, list] : m_daq_lists_) {
                if (list.running) {
                    session_status |= 0x40U;  // bit6 DAQ_RUNNING
                    break;
                }
            }
            response = MakeRes(
                {session_status, EffectiveProtection(), 0x01, 0x07, 0x00});
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
            // SET_MTA: [F6][MODE][reserved][EXT@3][ADDR@4..7]（XCP 1.3
            // 布局，与 SHORT_UPLOAD 地址域同构；XCPlite 对手端核证）
            if (xcp_packet.size() < 8U) {
                response = MakeErr(ErrorCode::CmdSyntax);
            } else {
                m_mta_extension_ = xcp_packet[3];
                // Address 按 Session Byte Order（测试 Slave 固定 Intel 小端）
                m_mta_ = static_cast<Address>(xcp_packet[4]) |
                         (static_cast<Address>(xcp_packet[5]) << 8) |
                         (static_cast<Address>(xcp_packet[6]) << 16) |
                         (static_cast<Address>(xcp_packet[7]) << 24);
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
        } else if (m_daq_enabled_) {
            // 批次14（T14-09）：DAQ 命令组与 DOWNLOAD 写回。开关关闭时
            // 根本不会进这里，既有"未实现命令 → ERR_CMD_UNKNOWN"的语义不变
            DaqDispatchResult dq = HandleDaqOrDownload(xcp_packet);
            if (dq.handled) {
                response = std::move(dq.response);
            } else {
                response = MakeErr(ErrorCode::CmdUnknown);
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
