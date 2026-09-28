/**
 * @file xcp_error.hpp
 * @brief libxcp 统一错误分类与异常类型。
 *
 * 依据 code-plan/XCP_1.3.0_详细设计_类接口与头文件.md 第 4 节实现。
 * 所有公共 API 通过抛出 XcpException 报告错误；异常对象保留机器可判定分类、
 * 协议码、重试次数和底层错误信息，不允许只有文本的错误。
 */

#ifndef CALMCAR_XCP_XCP_ERROR_HPP_
#define CALMCAR_XCP_XCP_ERROR_HPP_

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "libxcp/protocol_types.hpp"

namespace calmcar::xcp {

/**
 * @brief 错误分类（机器可判定）
 */
enum class ErrorCategory {
    InvalidArgument,     ///< 本地参数、AG 换算、长度非法
    InvalidState,        ///< 未连接、正在恢复或已有待响应命令
    TransportError,      ///< 打开、发送、接收、关闭失败
    Timeout,             ///< 规定时间内没有最终响应
    MalformedPacket,     ///< PID、长度、对齐或字段非法
    ProtocolError,       ///< Slave 返回 ERR
    UnsupportedFeature,  ///< 本阶段未实现的功能（如 DAQ、块模式）
    RecoveryFailed,      ///< SYNCH 恢复或重试耗尽
};

/// @brief 将错误分类转为字符串
[[nodiscard]] std::string_view ErrorCategoryName(ErrorCategory cat) noexcept;

/**
 * @brief XCP 库统一异常类型
 *
 * 所有公共 API 通过抛出此异常报告错误。异常对象保留机器可判定分类、协议码、
 * 重试次数和底层错误信息，便于上层按分类做决策而非解析文本。
 */
class XcpException : public std::runtime_error {
public:
    /**
     * @brief 构造异常
     * @param category 错误分类
     * @param message 人类可读的错误描述
     * @param command_code 相关命令码（可选，用于诊断）
     * @param error_code 协议错误码（仅 ProtocolError 时有效）
     * @param retry_count 恢复重试次数（仅恢复场景有效）
     * @param transport_error 底层 Transport 错误描述（可选）
     */
    explicit XcpException(
        ErrorCategory category, std::string message,
        std::optional<CommandCode> command_code = std::nullopt,
        std::optional<ErrorCode> error_code = std::nullopt, int retry_count = 0,
        std::string transport_error = "");

    /// @brief 获取错误分类
    [[nodiscard]] ErrorCategory Category() const noexcept;

    /// @brief 获取相关命令码
    [[nodiscard]] std::optional<CommandCode> GetCommandCode() const noexcept;

    /// @brief 获取协议错误码（仅 ProtocolError 有意义）
    [[nodiscard]] std::optional<ErrorCode> GetErrorCode() const noexcept;

    /// @brief 获取恢复重试次数
    [[nodiscard]] int RetryCount() const noexcept;

    /// @brief 获取底层 Transport 错误描述
    [[nodiscard]] std::string_view TransportError() const noexcept;

private:
    ErrorCategory m_category_;                   ///< 错误分类
    std::optional<CommandCode> m_command_code_;  ///< 相关命令码
    std::optional<ErrorCode> m_error_code_;      ///< 协议错误码
    int m_retry_count_;                          ///< 恢复重试次数
    std::string m_transport_error_;              ///< 底层 Transport 错误描述
};

}  // namespace calmcar::xcp

namespace calmcar::xcp::detail {

/// @brief 构造 InvalidArgument 异常（本地参数、AG 换算、长度非法）
[[nodiscard]] XcpException MakeInvalidArgument(std::string msg);

/// @brief 构造 InvalidState 异常（Session
/// 状态不允许当前操作，如未连接就发命令）
[[nodiscard]] XcpException MakeInvalidState(std::string msg);

/// @brief 构造 TransportError 异常（通道打开/发送/接收失败）
/// @param msg 错误描述
/// @param transport_detail 底层 Socket 错误细节，可为空
[[nodiscard]] XcpException MakeTransportError(
    std::string msg, std::string transport_detail = "");

/// @brief 构造 Timeout 异常（规定时间内没有最终响应）
/// @param msg 错误描述
/// @param cmd 超时的命令码（可为空）
/// @param retry 已使用的恢复重试次数
[[nodiscard]] XcpException MakeTimeout(std::string msg,
                                       std::optional<CommandCode> cmd,
                                       int retry);

/// @brief 构造 MalformedPacket 异常（响应长度/字段与协议布局不符）
[[nodiscard]] XcpException MakeMalformedPacket(std::string msg);

/// @brief 构造 ProtocolError 异常
[[nodiscard]] XcpException MakeProtocolError(std::string msg, CommandCode cmd,
                                             ErrorCode code);

/// @brief 构造 UnsupportedFeature 异常（本地未开放的能力，如 DAQ、块模式）
[[nodiscard]] XcpException MakeUnsupportedFeature(std::string msg);

/// @brief 构造 RecoveryFailed 异常（SYNCH 恢复未获确认或重试次数用尽）
/// @param msg 错误描述
/// @param cmd 触发恢复的命令码（可为空）
/// @param retry 已使用的重试次数
[[nodiscard]] XcpException MakeRecoveryFailed(std::string msg,
                                              std::optional<CommandCode> cmd,
                                              int retry);

}  // namespace calmcar::xcp::detail

#endif  // CALMCAR_XCP_XCP_ERROR_HPP_
