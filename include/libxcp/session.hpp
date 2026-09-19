/**
 * @file session.hpp
 * @brief XCP 会话状态机与协商参数管理。
 *
 * 依据 code-plan/XCP_1.3.0_详细设计_类接口与头文件.md 第 11 节实现。
 * 线程安全：所有公共方法内部加锁；不依赖 Transport。
 */

#ifndef CALMCAR_XCP_SESSION_HPP_
#define CALMCAR_XCP_SESSION_HPP_

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "libxcp/protocol_types.hpp"
#include "libxcp/xcp_error.hpp"

namespace calmcar::xcp {

/// @brief MAX_CTO 协议有效下限（XCP on Ethernet 1.1 规定 0x08..0xFF）
constexpr std::uint8_t kMaxCtoMinimum = 0x08U;
/// @brief MAX_DTO 协议有效下限（XCP on Ethernet 1.1 规定 0x0008..0xFFFF）
constexpr std::uint16_t kMaxDtoMinimum = 0x0008U;

/**
 * @brief XCP 会话管理器
 *
 * 维护 Session 状态机、CONNECT 协商参数和 Standard Communication Model 的
 * 单 Outstanding Command 约束。断连后清空 MTA 相关能力与可选查询结果。
 */
class Session {
public:
    Session() = default;
    ~Session() = default;

    // 禁止拷贝（内部持有 mutex）
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // ---- 状态查询 ----

    /// @brief 获取当前 Session 状态
    [[nodiscard]] SessionState State() const;

    [[nodiscard]] bool IsConnected() const;

    [[nodiscard]] bool HasPendingCommand() const;

    [[nodiscard]] std::string FailReason() const;

    // ---- 状态迁移 ----

    void BeginConnecting();

    void EstablishConnection(const ConnectResponse& connect_response);

    void BeginDisconnecting();

    void CompleteDisconnection();

    void BeginRecovery();

    /**
     * @brief SYNCH 恢复成功，回到 Connected
     */
    void CompleteRecovery();

    void Fail(std::string_view reason);

    void Reset();

    // ---- Outstanding Command 管理 ----

    void MarkCommandSent(CommandCode cmd);

    void ClearPendingCommand();

    [[nodiscard]] std::optional<CommandCode> PendingCommand() const;

    // ---- 参数访问 ----

    [[nodiscard]] SessionParameters Parameters() const;

    void UpdateCommModeInfo(const GetCommModeInfoResponse& info);

    void UpdateStatus(const GetStatusResponse& status);

    void DisableShortUpload();

    [[nodiscard]] ByteOrder GetByteOrder() const;

    [[nodiscard]] AddressGranularity GetAddressGranularity() const;

    [[nodiscard]] std::uint8_t MaxCto() const;

    [[nodiscard]] std::uint16_t MaxDto() const;

private:
    mutable std::mutex m_mutex_;                        ///< 保护以下全部字段
    SessionState m_state_{SessionState::Disconnected};  ///< 当前状态
    SessionParameters m_params_;                        ///< 协商参数
    std::optional<CommandCode> m_pending_command_;  ///< 单 Outstanding Command
    std::string m_fail_reason_;                     ///< 最近失败原因

    /**
     * @brief 校验 CONNECT 参数合法性（调用方须已持锁）
     * @throws XcpException(InvalidArgument) 参数非法
     */
    void ValidateConnectParams(const ConnectResponse& resp) const;
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_SESSION_HPP_
