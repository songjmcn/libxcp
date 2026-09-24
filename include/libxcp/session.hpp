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

    /// @brief 是否处于 Connected 状态（Connecting/Recovering 等均返回 false）
    [[nodiscard]] bool IsConnected() const;

    /// @brief 是否有命令在等待响应（单 Outstanding Command 约束的判据）
    [[nodiscard]] bool HasPendingCommand() const;

    /// @brief 最近一次进入 Failed 状态时记录的原因；无失败时为空串
    [[nodiscard]] std::string FailReason() const;

    // ---- 状态迁移 ----

    /// @brief 进入 Connecting 状态
    /// @details 仅允许从 Disconnected 发起；Failed 必须先调用 Reset()。
    ///          同时清空挂起命令与失败原因。
    /// @throws XcpException(InvalidState) 当前状态不是 Disconnected
    void BeginConnecting();

    /// @brief 用 CONNECT 响应建立会话，进入 Connected
    /// @details 重置协商参数后写入 CONNECT 结果，并把 SHORT_UPLOAD 能力置为可用
    ///          （后续若 Slave 返回 ERR_CMD_UNKNOWN 再由 DisableShortUpload()
    ///          降级）。
    /// @param connect_response 已解析的 CONNECT 响应
    /// @throws XcpException(InvalidState) 响应到达时不在 Connecting 状态
    /// @throws XcpException(InvalidArgument) MAX_CTO/MAX_DTO 等参数低于协议下限
    void EstablishConnection(const ConnectResponse& connect_response);

    /// @brief 进入 Disconnecting 状态
    /// @throws XcpException(InvalidState) 当前不在 Connected
    void BeginDisconnecting();

    /// @brief 完成断开：清空协商参数与能力（含 MTA 相关），回到干净的
    /// Disconnected
    void CompleteDisconnection();

    /// @brief 进入 Recovering 状态（超时后开始 SYNCH 恢复）
    /// @throws XcpException(InvalidState) 当前不在 Connected
    void BeginRecovery();

    /**
     * @brief SYNCH 恢复成功，回到 Connected
     */
    void CompleteRecovery();

    /// @brief 转入 Failed 并记录原因，同时清空协商参数与挂起命令
    /// @param reason 失败原因描述
    void Fail(std::string_view reason);

    /// @brief 强制回到初始 Disconnected
    /// 干净状态（清空参数、挂起命令与失败原因）
    void Reset();

    // ---- Outstanding Command 管理 ----

    /// @brief 登记一条已发出的命令，占用唯一的 Outstanding Command 槽位
    /// @param cmd 命令码
    /// @throws XcpException(InvalidState) 已有挂起命令，或当前处于
    /// Failed/Disconnected
    void MarkCommandSent(CommandCode cmd);

    /// @brief 释放 Outstanding Command 槽位（收到最终响应后调用）
    void ClearPendingCommand();

    /// @brief 当前挂起的命令码；无挂起命令时返回 std::nullopt
    [[nodiscard]] std::optional<CommandCode> PendingCommand() const;

    // ---- 参数访问 ----

    /// @brief 获取当前协商参数快照
    [[nodiscard]] SessionParameters Parameters() const;

    /// @brief 写入 GET_COMM_MODE_INFO 的解析结果
    void UpdateCommModeInfo(const GetCommModeInfoResponse& info);

    /// @brief 写入 GET_STATUS 的解析结果
    void UpdateStatus(const GetStatusResponse& status);

    /// @brief 标记 SHORT_UPLOAD 不可用（收到 ERR_CMD_UNKNOWN 后的降级路径）
    void DisableShortUpload();

    /// @brief Session 字节序（来自 CONNECT 的 COMM_MODE_BASIC bit0）
    [[nodiscard]] ByteOrder GetByteOrder() const;

    /// @brief 地址粒度（来自 CONNECT 的 COMM_MODE_BASIC bit1-2）
    [[nodiscard]] AddressGranularity GetAddressGranularity() const;

    /// @brief 单个命令的最大长度（CONNECT 协商值）
    [[nodiscard]] std::uint8_t MaxCto() const;

    /// @brief 单个 DAQ 事件的最大长度（CONNECT 协商值）
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
