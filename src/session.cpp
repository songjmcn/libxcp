/**
 * @file session.cpp
 * @brief Session 状态机、参数校验与单 Outstanding Command 约束的实现。
 *
 * 校验规则依据 code-plan/XCP_1.3.0_最小协议核心实现计划.md 第 4.2 节：
 * MAX_CTO ∈ [0x08,0xFF]、MAX_DTO ∈ [0x0008,0xFFFF]、
 * MAX_CTO mod AG == 0、MAX_DTO mod AG == 0；关键字段非法时不进入 Connected。
 */

#include "libxcp/session.hpp"

#include <string>

namespace calmcar::xcp {

void Session::ValidateConnectParams(const ConnectResponse& resp) const {
    // MAX_CTO 范围校验
    if (resp.max_cto < kMaxCtoMinimum) {
        throw detail::MakeInvalidArgument(
            "CONNECT 返回的 MAX_CTO 小于协议下限 0x08: " +
            std::to_string(resp.max_cto));
    }

    // MAX_DTO 范围校验
    if (resp.max_dto < kMaxDtoMinimum) {
        throw detail::MakeInvalidArgument(
            "CONNECT 返回的 MAX_DTO 小于协议下限 0x0008: " +
            std::to_string(resp.max_dto));
    }

    // AG 整除校验：MAX_CTO mod AG == 0
    const auto ag_bytes = AgToBytes(resp.address_granularity);
    if (ag_bytes == 0U || (resp.max_cto % ag_bytes) != 0U) {
        throw detail::MakeInvalidArgument(
            "MAX_CTO 不能被 Address Granularity 整除: MAX_CTO=" +
            std::to_string(resp.max_cto) + ", AG=" + std::to_string(ag_bytes));
    }
    // AG 整除校验：MAX_DTO mod AG == 0
    if ((resp.max_dto % ag_bytes) != 0U) {
        throw detail::MakeInvalidArgument(
            "MAX_DTO 不能被 Address Granularity 整除: MAX_DTO=" +
            std::to_string(resp.max_dto) + ", AG=" + std::to_string(ag_bytes));
    }
}

SessionState Session::State() const {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    return m_state_;
}

bool Session::IsConnected() const {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    return m_state_ == SessionState::Connected;
}

bool Session::HasPendingCommand() const {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    return m_pending_command_.has_value();
}

std::string Session::FailReason() const {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    return m_fail_reason_;
}

void Session::BeginConnecting() {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    // 仅允许从 Disconnected 发起连接；Failed 必须先 reset()
    if (m_state_ != SessionState::Disconnected) {
        throw detail::MakeInvalidState(std::string("当前状态不允许发起连接: ") +
                                       std::string(SessionStateName(m_state_)));
    }
    m_state_ = SessionState::Connecting;
    m_pending_command_.reset();
    m_fail_reason_.clear();
}

void Session::EstablishConnection(const ConnectResponse& connect_response) {
    // 先做无锁的参数校验（校验函数本身不访问成员，但为一致性仍在锁内调用）
    {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        if (m_state_ != SessionState::Connecting) {
            throw detail::MakeInvalidState(
                std::string("CONNECT 响应到达时不在 Connecting 状态: ") +
                std::string(SessionStateName(m_state_)));
        }
        ValidateConnectParams(connect_response);

        m_params_ = SessionParameters{};
        m_params_.connect = connect_response;
        m_params_.short_upload_available = true;
        m_state_ = SessionState::Connected;
        m_pending_command_.reset();
        m_fail_reason_.clear();
    }
}

void Session::BeginDisconnecting() {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    if (m_state_ != SessionState::Connected) {
        throw detail::MakeInvalidState(std::string("当前状态不允许断开: ") +
                                       std::string(SessionStateName(m_state_)));
    }
    m_state_ = SessionState::Disconnecting;
}

void Session::CompleteDisconnection() {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    // 断连后清空协商参数与能力（含 MTA 相关能力），回到干净的未连接状态
    m_params_ = SessionParameters{};
    m_pending_command_.reset();
    m_state_ = SessionState::Disconnected;
}

void Session::BeginRecovery() {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    if (m_state_ != SessionState::Connected) {
        throw detail::MakeInvalidState(std::string("当前状态不允许进入恢复: ") +
                                       std::string(SessionStateName(m_state_)));
    }
    m_state_ = SessionState::Recovering;
}

void Session::CompleteRecovery() {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    if (m_state_ == SessionState::Recovering) {
        m_state_ = SessionState::Connected;
    }
}

void Session::Fail(std::string_view reason) {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    m_state_ = SessionState::Failed;
    m_fail_reason_ = std::string(reason);
    m_params_ = SessionParameters{};
    m_pending_command_.reset();
}

void Session::Reset() {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    m_state_ = SessionState::Disconnected;
    m_params_ = SessionParameters{};
    m_pending_command_.reset();
    m_fail_reason_.clear();
}

void Session::MarkCommandSent(CommandCode cmd) {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    if (m_pending_command_.has_value()) {
        throw detail::MakeInvalidState(
            "已有待响应的命令，Standard Communication Model 只允许一条 "
            "Outstanding Command");
    }
    // Failed / Disconnected 状态下不得发送业务命令
    if (m_state_ == SessionState::Failed ||
        m_state_ == SessionState::Disconnected) {
        throw detail::MakeInvalidState(std::string("当前状态不允许发送命令: ") +
                                       std::string(SessionStateName(m_state_)));
    }
    m_pending_command_ = cmd;
}

void Session::ClearPendingCommand() {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    m_pending_command_.reset();
}

std::optional<CommandCode> Session::PendingCommand() const {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    return m_pending_command_;
}

SessionParameters Session::Parameters() const {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    return m_params_;
}

void Session::UpdateCommModeInfo(const GetCommModeInfoResponse& info) {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    m_params_.comm_mode_info = info;
}

void Session::UpdateStatus(const GetStatusResponse& status) {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    m_params_.status = status;
}

void Session::DisableShortUpload() {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    m_params_.short_upload_available = false;
}

ByteOrder Session::GetByteOrder() const {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    return m_params_.connect.byte_order;
}

AddressGranularity Session::GetAddressGranularity() const {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    return m_params_.connect.address_granularity;
}

std::uint8_t Session::MaxCto() const {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    return m_params_.connect.max_cto;
}

std::uint16_t Session::MaxDto() const {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    return m_params_.connect.max_dto;
}

}  // namespace calmcar::xcp
