/**
 * @file xcp_master.cpp
 * @brief XcpMaster 实现：连接编排、失败清理与内存读取转发。
 */

#include "libxcp/xcp_master.hpp"

#include <utility>

namespace calmcar::xcp {

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
        if (connect.m_optional_comm_mode_available_) {
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

}  // namespace calmcar::xcp
