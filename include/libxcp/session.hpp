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

    // ---- DAQ 运行态（批次14，T14-07）----

    /**
     * @brief 登记一个已 START 的 DAQ List（幂等；重复登记不产生重复项）
     * @param daq_list DAQ List 号（EPK）
     * @details 用途只有一个：断连/析构前判断"是否还有 List 在跑"，从而补发
     *          START_STOP_SYNCH(stop all)。配置内容本身归 XcpMaster 的
     *          WRITE_DAQ 账本，不放这里（Session 只管会话级状态）。
     */
    void MarkDaqListStarted(std::uint16_t daq_list);

    /// @brief 注销一个 DAQ List（Stop 单列表成功时调用）
    void MarkDaqListStopped(std::uint16_t daq_list);

    /// @brief 清空 DAQ 运行态（StopAll 成功、或会话级清理时调用）
    void ClearStartedDaqLists();

    /// @brief 当前已 START 的 DAQ List 号（升序，快照拷贝）
    [[nodiscard]] std::vector<std::uint16_t> StartedDaqLists() const;

    /// @brief 是否有 DAQ List 处于运行态（决定断连前是否要补发 STOP）
    [[nodiscard]] bool HasRunningDaqList() const;

    /// @brief 配置代际：每次 CLEAR_DAQ_LIST / 重连递增，用于识别陈旧账本
    /// @details 账本（XcpMaster 侧）与代际不匹配时，解码器必须拒绝按旧布局
    ///          解释 DTO（B-6 禁止猜测），而不是继续用错位的 PID。
    [[nodiscard]] std::uint32_t DaqConfigGeneration() const;

    /// @brief 递增配置代际（CLEAR_DAQ_LIST 成功后调用）
    void BumpDaqConfigGeneration();

    // ---- 变量标定批次：PAG Processor 信息缓存 ----

    /**
     * @brief 写入 GET_PAG_PROCESSOR_INFO 的解析结果（Connect 探测成功后调用）
     * @param info 已解析的 PAG Processor 信息（MAX_SEGMENT / PAG_PROPERTIES）
     */
    void SetPagProcessorInfo(const GetPagProcessorInfoResponse& info);

    /// @brief 缓存的 PAG Processor 信息；未探测或不支持时返回 std::nullopt
    [[nodiscard]] std::optional<GetPagProcessorInfoResponse>
    PagProcessorInfo() const;

private:
    mutable std::mutex m_mutex_;                        ///< 保护以下全部字段
    SessionState m_state_{SessionState::Disconnected};  ///< 当前状态
    SessionParameters m_params_;                        ///< 协商参数
    std::optional<CommandCode> m_pending_command_;  ///< 单 Outstanding Command
    std::string m_fail_reason_;                     ///< 最近失败原因
    /**
     * @brief 已 START 的 DAQ List 号（批次14，T14-07）
     * @details 只用于"断连/析构前是否需要补发 START_STOP_SYNCH(stop)"的判据；
     *          会话级清理（EstablishConnection / CompleteDisconnection / Fail /
     *          Reset）四处必须一并清空，避免残留伪运行态。
     */
    std::vector<std::uint16_t> m_daq_running_lists_;
    /// @brief DAQ 配置代际（CLEAR_DAQ_LIST 或重连时递增；陈旧账本识别用）
    std::uint32_t m_daq_generation_ = 0U;
    /**
     * @brief GET_PAG_PROCESSOR_INFO 缓存（变量标定批次）
     * @details Connect 阶段仅当 CONNECT 响应 CAL_PAG 资源置位时探测填充；
     *          Slave 回 ERR_CMD_UNKNOWN（不支持 Paging）时保持 nullopt。
     *          会话级清理四处（EstablishConnection / CompleteDisconnection /
     *          Fail / Reset）必须一并清空，避免跨会话残留旧 ECU 的 Segment
     *          上限。
     */
    std::optional<GetPagProcessorInfoResponse> m_pag_processor_info_;

    /**
     * @brief 校验 CONNECT 参数合法性（调用方须已持锁）
     * @throws XcpException(InvalidArgument) 参数非法
     */
    void ValidateConnectParams(const ConnectResponse& resp) const;
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_SESSION_HPP_
