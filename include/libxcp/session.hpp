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
    [[nodiscard]] SessionState state() const;

    /// @brief 是否已连接（Connected 状态）
    [[nodiscard]] bool isConnected() const;

    /// @brief 是否有等待响应的命令（Outstanding Command）
    [[nodiscard]] bool hasPendingCommand() const;

    /// @brief 最近一次 fail() 记录的原因；未失败时为空串
    [[nodiscard]] std::string failReason() const;

    // ---- 状态迁移 ----

    /**
     * @brief 进入 Connecting 状态
     * @throws XcpException(InvalidState) 当前状态不允许发起连接
     */
    void beginConnecting();

    /**
     * @brief CONNECT 成功，校验并保存参数，进入 Connected
     * @param connect_response CONNECT 响应解析结果
     * @throws XcpException(InvalidArgument) 参数校验失败
     * @throws XcpException(InvalidState) 当前不在 Connecting 状态
     */
    void establishConnection(const ConnectResponse& connect_response);

    /**
     * @brief 进入 Disconnecting 状态
     * @throws XcpException(InvalidState) 当前状态不允许断开
     */
    void beginDisconnecting();

    /**
     * @brief DISCONNECT 成功，清理协商参数并进入 Disconnected
     */
    void completeDisconnection();

    /**
     * @brief 进入 Recovering 状态
     * @throws XcpException(InvalidState) 当前状态不允许恢复
     */
    void beginRecovery();

    /**
     * @brief SYNCH 恢复成功，回到 Connected
     */
    void completeRecovery();

    /**
     * @brief 标记 Session 为 Failed 并保留原因
     * @param reason 失败原因
     * @note 任何状态均可进入 Failed；协商参数一并清空，需重新连接。
     */
    void fail(std::string_view reason);

    /**
     * @brief 强制重置到 Disconnected（用于本地清理，不发送 DISCONNECT）
     */
    void reset();

    // ---- Outstanding Command 管理 ----

    /**
     * @brief 标记命令已发送，等待最终响应
     * @param cmd 命令码
     * @throws XcpException(InvalidState) 已有 Pending Command，或 Session 处于
     *         Disconnected/Failed 等不可发送状态
     */
    void markCommandSent(CommandCode cmd);

    /// @brief 标记命令响应已收到
    void clearPendingCommand();

    /// @brief 获取当前 Pending 命令码
    [[nodiscard]] std::optional<CommandCode> pendingCommand() const;

    // ---- 参数访问 ----

    /// @brief 获取 Session 参数快照
    [[nodiscard]] SessionParameters parameters() const;

    /// @brief 更新 GET_COMM_MODE_INFO 结果
    void updateCommModeInfo(const GetCommModeInfoResponse& info);

    /// @brief 更新 GET_STATUS 结果
    void updateStatus(const GetStatusResponse& status);

    /// @brief 标记 SHORT_UPLOAD 不可用（遇 ERR_CMD_UNKNOWN 后降级）
    void disableShortUpload();

    /// @brief 获取当前 Byte Order（未连接时返回 Intel）
    [[nodiscard]] ByteOrder byteOrder() const;

    /// @brief 获取当前 AG（未连接时返回 Byte）
    [[nodiscard]] AddressGranularity addressGranularity() const;

    /// @brief 获取 MAX_CTO（未连接时返回 0）
    [[nodiscard]] std::uint8_t maxCto() const;

    /// @brief 获取 MAX_DTO（未连接时返回 0）
    [[nodiscard]] std::uint16_t maxDto() const;

private:
    mutable std::mutex mutex_;                        ///< 保护以下全部字段
    SessionState state_{SessionState::Disconnected};  ///< 当前状态
    SessionParameters params_;                        ///< 协商参数
    std::optional<CommandCode> pending_command_;  ///< 单 Outstanding Command
    std::string fail_reason_;                     ///< 最近失败原因

    /**
     * @brief 校验 CONNECT 参数合法性（调用方须已持锁）
     * @throws XcpException(InvalidArgument) 参数非法
     */
    void validateConnectParams(const ConnectResponse& resp) const;
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_SESSION_HPP_
