/**
 * @file xcp_master.cpp
 * @brief XcpMaster 实现：连接编排、失败清理与内存读取转发。
 */

#include "libxcp/xcp_master.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace calmcar::xcp {

namespace {

/// @brief Seed/Key 分段的固定头部字节数：PID(1) + Length(1)
constexpr std::size_t kSeedKeyHeaderBytes = 2;
/// @brief Key 的 Length 字段为单字节，最大 255 字节（规范 §9.2 建议上限）
constexpr std::size_t kMaxKeyLength = 255;
/// @brief 可参与 Seed&Key 解锁的合法资源位集合
constexpr ResourceMask kValidUnlockResources =
    static_cast<ResourceMask>(Resource::CalPag) |
    static_cast<ResourceMask>(Resource::Daq) |
    static_cast<ResourceMask>(Resource::Stim) |
    static_cast<ResourceMask>(Resource::Pgm);

}  // namespace

XcpMaster::XcpMaster(std::unique_ptr<IXcpTransport> transport,
                     CommandTimeouts timeouts, IEventListener* event_listener)
    : m_transport_(std::move(transport)) {
    if (!m_transport_) {
        throw detail::MakeInvalidArgument(
            "XcpMaster 需要非空的 IXcpTransport 实例");
    }
    m_executor_ = std::make_unique<CommandExecutor>(*m_transport_, m_session_,
                                                    timeouts, event_listener);
    m_memory_access_ = std::make_unique<MemoryAccess>(*m_executor_, m_session_);
}

XcpMaster::~XcpMaster() {
    // 析构路径不抛异常：尽力而为地关闭通道，本地状态随之释放
    try {
        if (m_session_.IsConnected()) {
            try {
                m_executor_->ExecuteDisconnect();
            } catch (...) {
                m_session_.Reset();
            }
        }
        m_transport_->Close();
    } catch (...) {
    }
    // Executor 持有 Transport 引用，必须先于 Transport 销毁
    m_memory_access_.reset();
    m_executor_.reset();
}

void XcpMaster::Connect() {
    // 已连接时拒绝重复连接（避免在活跃会话上重放 CONNECT）
    if (m_session_.IsConnected()) {
        throw detail::MakeInvalidState("Session 已处于 Connected 状态");
    }
    m_session_.Reset();  // 清理 Failed/残留状态

    // Transport.Open 的监听器即 CommandExecutor；Open 之后接收线程立即开始回调
    m_transport_->Open(m_executor_->AsListener());

    auto cleanup_on_failure = [this] {
        // CONNECT、参数校验或 GET_STATUS 失败时清除部分状态并关闭通道（计划
        // §5.1）
        m_session_.Reset();
        try {
            m_transport_->Close();
        } catch (...) {
        }
    };

    try {
        const ConnectResponse connect = m_executor_->ExecuteConnect(0x00);

        // 仅在 CONNECT 表明 Optional 信息可用时才查询扩展通信模式；
        // Slave 返回 ERR_CMD_UNKNOWN 时 ExecuteGetCommModeInfo 内部降级为
        // nullopt。
        if (connect.optional_comm_mode_available) {
            (void)m_executor_->ExecuteGetCommModeInfo();
        }

        // GET_STATUS 为 Mandatory，失败即视为连接失败
        (void)m_executor_->ExecuteGetStatus();
    } catch (const XcpException&) {
        cleanup_on_failure();
        throw;
    } catch (const std::exception& e) {
        cleanup_on_failure();
        throw detail::MakeTransportError("连接过程中发生非协议异常", e.what());
    }
}

void XcpMaster::Disconnect() {
    // 未连接：本地幂等，仅确保通道关闭
    if (m_session_.State() == SessionState::Disconnected) {
        m_transport_->Close();
        return;
    }

    std::optional<XcpException> failure;
    try {
        m_executor_->ExecuteDisconnect();
    } catch (const XcpException& e) {
        // 即使 DISCONNECT 返回 ERR_CMD_BUSY 或 Transport
        // 错误，也释放本地资源（计划 §5.4）
        failure = e;
        m_session_.Reset();
    }

    try {
        m_transport_->Close();
    } catch (const XcpException& e) {
        if (!failure) {
            failure = e;
        }
    }

    if (failure) {
        throw *failure;
    }
}

bool XcpMaster::IsConnected() const { return m_session_.IsConnected(); }

Bytes XcpMaster::ReadMemoryBytes(Address address, AddressExtension extension,
                                 ByteCount byte_count) {
    return m_memory_access_->ReadBytes(address, extension, byte_count);
}

Bytes XcpMaster::ReadMemory(Address address, AddressExtension extension,
                            ElementCount element_count) {
    return m_memory_access_->ReadElements(address, extension, element_count);
}

SessionParameters XcpMaster::GetSessionParameters() const {
    return m_session_.Parameters();
}

SessionState XcpMaster::GetSessionState() const { return m_session_.State(); }

GetStatusResponse XcpMaster::QueryStatus() {
    return m_executor_->ExecuteGetStatus();
}

UnlockResult XcpMaster::Unlock(Resource resource,
                               const SeedKeyCalculator& calculator) {
    // ---- 1) 本地预检（计划 §4.5：非法参数不发送）----
    const auto resource_mask = static_cast<ResourceMask>(resource);
    // 必须恰为单个资源位：非 0、无多余置位、且属于 CAL/PAG|DAQ|STIM|PGM
    const bool is_single_bit =
        resource_mask != 0U && (resource_mask & (resource_mask - 1U)) == 0U;
    if (!is_single_bit || (resource_mask & ~kValidUnlockResources) != 0U) {
        throw detail::MakeInvalidArgument(
            "Unlock 仅支持单个资源位（CAL_PAG/DAQ/STIM/PGM 之一），收到掩码 " +
            std::to_string(resource_mask));
    }
    if (!calculator) {
        throw detail::MakeInvalidArgument(
            "Unlock 的 SeedKeyCalculator 回调不能为空");
    }

    // Seed/Key 每帧最大载荷 = MAX_CTO - 2（协议已保证 MAX_CTO >= 8）
    const auto max_segment =
        static_cast<std::size_t>(m_session_.MaxCto()) - kSeedKeyHeaderBytes;

    // ---- 2) GET_SEED 首段，获得 Seed 总长度 ----
    const GetSeedResponse first =
        m_executor_->ExecuteGetSeed(resource, SeedMode::First);
    if (first.length == 0U) {
        // Length=0：资源未保护，无需 UNLOCK（规范 §7.5.1.8），
        // 不调用回调、不发送 UNLOCK
        return UnlockResult{true, std::nullopt};
    }

    // ---- 3) 分段收集 Seed（Remainder 帧）----
    Bytes seed(first.seed.begin(), first.seed.end());
    if (seed.empty()) {
        throw detail::MakeMalformedPacket("GET_SEED 声明 Length " +
                                          std::to_string(first.length) +
                                          " 但首段 Seed 为空");
    }
    if (seed.size() > first.length) {
        throw detail::MakeMalformedPacket(
            "GET_SEED 首段 Seed 字节数 " + std::to_string(seed.size()) +
            " 超过声明总长度 " + std::to_string(first.length));
    }
    while (seed.size() < first.length) {
        const GetSeedResponse part =
            m_executor_->ExecuteGetSeed(resource, SeedMode::Remainder);
        if (part.seed.empty()) {
            // 空续段无法推进进度，终止以防死循环
            throw detail::MakeMalformedPacket(
                "GET_SEED 续段返回空 Seed，无法收满声明总长度 " +
                std::to_string(first.length));
        }
        if (seed.size() + part.seed.size() > first.length) {
            throw detail::MakeMalformedPacket(
                "GET_SEED 续段累计字节数超过声明总长度 " +
                std::to_string(first.length));
        }
        seed.insert(seed.end(), part.seed.begin(), part.seed.end());
    }

    // ---- 4) 调用方算法计算 Key（异常原样传播）----
    Bytes key = calculator(resource, BytesView{seed});
    if (key.empty()) {
        throw detail::MakeInvalidArgument(
            "SeedKeyCalculator 返回空 Key，无法执行 UNLOCK");
    }
    if (key.size() > kMaxKeyLength) {
        throw detail::MakeInvalidArgument("SeedKeyCalculator 返回的 Key 长度 " +
                                          std::to_string(key.size()) +
                                          " 字节超过 Length 字段上限 255");
    }

    // ---- 5) 分段发送 UNLOCK：首帧 Length=Key 总长，后续帧=剩余长度 ----
    std::size_t offset = 0;
    UnlockResponse last{};
    do {
        const std::size_t remaining = key.size() - offset;
        const std::size_t segment = std::min(remaining, max_segment);
        const auto length_field =
            static_cast<std::uint8_t>(offset == 0U ? key.size() : remaining);
        last = m_executor_->ExecuteUnlock(
            length_field, BytesView{key}.subspan(offset, segment));
        offset += segment;
    } while (offset < key.size());

    return UnlockResult{false, last.resource_protection};
}

}  // namespace calmcar::xcp
