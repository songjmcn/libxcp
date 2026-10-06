/**
 * @file xcp_error.cpp
 * @brief XcpException 与便捷构造函数的实现。
 */

#include "libxcp/xcp_error.hpp"

#include <utility>

namespace calmcar::xcp {

std::string_view ErrorCategoryName(ErrorCategory cat) noexcept {
    switch (cat) {
        case ErrorCategory::InvalidArgument:
            return "InvalidArgument";
        case ErrorCategory::InvalidState:
            return "InvalidState";
        case ErrorCategory::TransportError:
            return "TransportError";
        case ErrorCategory::Timeout:
            return "Timeout";
        case ErrorCategory::MalformedPacket:
            return "MalformedPacket";
        case ErrorCategory::ProtocolError:
            return "ProtocolError";
        case ErrorCategory::UnsupportedFeature:
            return "UnsupportedFeature";
        case ErrorCategory::RecoveryFailed:
            return "RecoveryFailed";
        case ErrorCategory::OperationOutcomeUnknown:
            return "OperationOutcomeUnknown";
    }
    return "Unknown";
}

XcpException::XcpException(ErrorCategory category, std::string message,
                           std::optional<CommandCode> command_code,
                           std::optional<ErrorCode> error_code, int retry_count,
                           std::string transport_error)
    : std::runtime_error(std::move(message)),
      m_category_(category),
      m_command_code_(command_code),
      m_error_code_(error_code),
      m_retry_count_(retry_count),
      m_transport_error_(std::move(transport_error)) {}

ErrorCategory XcpException::Category() const noexcept { return m_category_; }

std::optional<CommandCode> XcpException::GetCommandCode() const noexcept {
    return m_command_code_;
}

std::optional<ErrorCode> XcpException::GetErrorCode() const noexcept {
    return m_error_code_;
}

int XcpException::RetryCount() const noexcept { return m_retry_count_; }

std::string_view XcpException::TransportError() const noexcept {
    return m_transport_error_;
}

}  // namespace calmcar::xcp

namespace calmcar::xcp::detail {

XcpException MakeInvalidArgument(std::string msg) {
    return XcpException(ErrorCategory::InvalidArgument, std::move(msg));
}

XcpException MakeInvalidState(std::string msg) {
    return XcpException(ErrorCategory::InvalidState, std::move(msg));
}

XcpException MakeTransportError(std::string msg, std::string transport_detail) {
    return XcpException(ErrorCategory::TransportError, std::move(msg),
                        std::nullopt, std::nullopt, 0,
                        std::move(transport_detail));
}

XcpException MakeTimeout(std::string msg, std::optional<CommandCode> cmd,
                         int retry) {
    return XcpException(ErrorCategory::Timeout, std::move(msg), cmd,
                        std::nullopt, retry);
}

XcpException MakeMalformedPacket(std::string msg) {
    return XcpException(ErrorCategory::MalformedPacket, std::move(msg));
}

XcpException MakeProtocolError(std::string msg, CommandCode cmd,
                               ErrorCode code) {
    return XcpException(ErrorCategory::ProtocolError, std::move(msg), cmd,
                        code);
}

XcpException MakeUnsupportedFeature(std::string msg) {
    return XcpException(ErrorCategory::UnsupportedFeature, std::move(msg));
}

XcpException MakeRecoveryFailed(std::string msg, std::optional<CommandCode> cmd,
                                int retry) {
    return XcpException(ErrorCategory::RecoveryFailed, std::move(msg), cmd,
                        std::nullopt, retry);
}

}  // namespace calmcar::xcp::detail
