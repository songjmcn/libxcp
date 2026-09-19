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

void Session::validateConnectParams(const ConnectResponse& resp) const {
    // MAX_CTO 范围校验
    if (resp.maxCto < kMaxCtoMinimum) {
        throw detail::makeInvalidArgument(
            "CONNECT 返回的 MAX_CTO 小于协议下限 0x08: " +
            std::to_string(resp.maxCto));
    }

    // MAX_DTO 范围校验
    if (resp.maxDto < kMaxDtoMinimum) {
        throw detail::makeInvalidArgument(
            "CONNECT 返回的 MAX_DTO 小于协议下限 0x0008: " +
            std::to_string(resp.maxDto));
    }

    // AG 整除校验：MAX_CTO mod AG == 0
    const auto ag_bytes = agToBytes(resp.addressGranularity);
    if (ag_bytes == 0U || (resp.maxCto % ag_bytes) != 0U) {
        throw detail::makeInvalidArgument(
            "MAX_CTO 不能被 Address Granularity 整除: MAX_CTO=" +
            std::to_string(resp.maxCto) + ", AG=" + std::to_string(ag_bytes));
    }
    // AG 整除校验：MAX_DTO mod AG == 0
    if ((resp.maxDto % ag_bytes) != 0U) {
        throw detail::makeInvalidArgument(
            "MAX_DTO 不能被 Address Granularity 整除: MAX_DTO=" +
            std::to_string(resp.maxDto) + ", AG=" + std::to_string(ag_bytes));
    }
}

SessionState Session::state() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool Session::isConnected() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return state_ == SessionState::Connected;
}

bool Session::hasPendingCommand() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return pending_command_.has_value();
}

std::string Session::failReason() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return fail_reason_;
}

void Session::beginConnecting() {
    const std::lock_guard<std::mutex> lock(mutex_);
    // 仅允许从 Disconnected 发起连接；Failed 必须先 reset()
    if (state_ != SessionState::Disconnected) {
        throw detail::makeInvalidState(std::string("当前状态不允许发起连接: ") +
                                       std::string(sessionStateName(state_)));
    }
    state_ = SessionState::Connecting;
    pending_command_.reset();
    fail_reason_.clear();
}

void Session::establishConnection(const ConnectResponse& connect_response) {
    // 先做无锁的参数校验（校验函数本身不访问成员，但为一致性仍在锁内调用）
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != SessionState::Connecting) {
            throw detail::makeInvalidState(
                std::string("CONNECT 响应到达时不在 Connecting 状态: ") +
                std::string(sessionStateName(state_)));
        }
        validateConnectParams(connect_response);

        params_ = SessionParameters{};
        params_.connect = connect_response;
        params_.shortUploadAvailable = true;
        state_ = SessionState::Connected;
        pending_command_.reset();
        fail_reason_.clear();
    }
}

void Session::beginDisconnecting() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != SessionState::Connected) {
        throw detail::makeInvalidState(std::string("当前状态不允许断开: ") +
                                       std::string(sessionStateName(state_)));
    }
    state_ = SessionState::Disconnecting;
}

void Session::completeDisconnection() {
    const std::lock_guard<std::mutex> lock(mutex_);
    // 断连后清空协商参数与能力（含 MTA 相关能力），回到干净的未连接状态
    params_ = SessionParameters{};
    pending_command_.reset();
    state_ = SessionState::Disconnected;
}

void Session::beginRecovery() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != SessionState::Connected) {
        throw detail::makeInvalidState(std::string("当前状态不允许进入恢复: ") +
                                       std::string(sessionStateName(state_)));
    }
    state_ = SessionState::Recovering;
}

void Session::completeRecovery() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == SessionState::Recovering) {
        state_ = SessionState::Connected;
    }
}

void Session::fail(std::string_view reason) {
    const std::lock_guard<std::mutex> lock(mutex_);
    state_ = SessionState::Failed;
    fail_reason_ = std::string(reason);
    params_ = SessionParameters{};
    pending_command_.reset();
}

void Session::reset() {
    const std::lock_guard<std::mutex> lock(mutex_);
    state_ = SessionState::Disconnected;
    params_ = SessionParameters{};
    pending_command_.reset();
    fail_reason_.clear();
}

void Session::markCommandSent(CommandCode cmd) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (pending_command_.has_value()) {
        throw detail::makeInvalidState(
            "已有待响应的命令，Standard Communication Model 只允许一条 "
            "Outstanding Command");
    }
    // Failed / Disconnected 状态下不得发送业务命令
    if (state_ == SessionState::Failed ||
        state_ == SessionState::Disconnected) {
        throw detail::makeInvalidState(std::string("当前状态不允许发送命令: ") +
                                       std::string(sessionStateName(state_)));
    }
    pending_command_ = cmd;
}

void Session::clearPendingCommand() {
    const std::lock_guard<std::mutex> lock(mutex_);
    pending_command_.reset();
}

std::optional<CommandCode> Session::pendingCommand() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return pending_command_;
}

SessionParameters Session::parameters() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return params_;
}

void Session::updateCommModeInfo(const GetCommModeInfoResponse& info) {
    const std::lock_guard<std::mutex> lock(mutex_);
    params_.commModeInfo = info;
}

void Session::updateStatus(const GetStatusResponse& status) {
    const std::lock_guard<std::mutex> lock(mutex_);
    params_.status = status;
}

void Session::disableShortUpload() {
    const std::lock_guard<std::mutex> lock(mutex_);
    params_.shortUploadAvailable = false;
}

ByteOrder Session::byteOrder() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return params_.connect.byteOrder;
}

AddressGranularity Session::addressGranularity() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return params_.connect.addressGranularity;
}

std::uint8_t Session::maxCto() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return params_.connect.maxCto;
}

std::uint16_t Session::maxDto() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return params_.connect.maxDto;
}

}  // namespace calmcar::xcp
