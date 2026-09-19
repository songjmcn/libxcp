/**
 * @file xcp_error.cpp
 * @brief XcpException 与便捷构造函数的实现。
 */

#include "libxcp/xcp_error.hpp"

#include <utility>

namespace calmcar::xcp {

std::string_view errorCategoryName(ErrorCategory cat) noexcept {
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
    }
    return "Unknown";
}

XcpException::XcpException(ErrorCategory category, std::string message,
                           std::optional<CommandCode> command_code,
                           std::optional<ErrorCode> error_code, int retry_count,
                           std::string transport_error)
    : std::runtime_error(std::move(message)),
      category_(category),
      command_code_(command_code),
      error_code_(error_code),
      retry_count_(retry_count),
      transport_error_(std::move(transport_error)) {}

ErrorCategory XcpException::category() const noexcept { return category_; }

std::optional<CommandCode> XcpException::commandCode() const noexcept {
    return command_code_;
}

std::optional<ErrorCode> XcpException::errorCode() const noexcept {
    return error_code_;
}

int XcpException::retryCount() const noexcept { return retry_count_; }

std::string_view XcpException::transportError() const noexcept {
    return transport_error_;
}

}  // namespace calmcar::xcp

namespace calmcar::xcp::detail {

XcpException makeInvalidArgument(std::string msg) {
    return XcpException(ErrorCategory::InvalidArgument, std::move(msg));
}

XcpException makeInvalidState(std::string msg) {
    return XcpException(ErrorCategory::InvalidState, std::move(msg));
}

XcpException makeTransportError(std::string msg, std::string transport_detail) {
    return XcpException(ErrorCategory::TransportError, std::move(msg),
                        std::nullopt, std::nullopt, 0,
                        std::move(transport_detail));
}

XcpException makeTimeout(std::string msg, std::optional<CommandCode> cmd,
                         int retry) {
    return XcpException(ErrorCategory::Timeout, std::move(msg), cmd,
                        std::nullopt, retry);
}

XcpException makeMalformedPacket(std::string msg) {
    return XcpException(ErrorCategory::MalformedPacket, std::move(msg));
}

XcpException makeProtocolError(std::string msg, CommandCode cmd,
                               ErrorCode code) {
    return XcpException(ErrorCategory::ProtocolError, std::move(msg), cmd,
                        code);
}

XcpException makeUnsupportedFeature(std::string msg) {
    return XcpException(ErrorCategory::UnsupportedFeature, std::move(msg));
}

XcpException makeRecoveryFailed(std::string msg, std::optional<CommandCode> cmd,
                                int retry) {
    return XcpException(ErrorCategory::RecoveryFailed, std::move(msg), cmd,
                        std::nullopt, retry);
}

}  // namespace calmcar::xcp::detail
