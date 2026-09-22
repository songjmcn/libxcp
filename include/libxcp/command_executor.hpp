/**
 * @file command_executor.hpp
 * @brief 命令执行器：编码 -> 发送 -> 等待响应 -> 解析 -> 超时恢复。
 *
 * 依据 code-plan/XCP_1.3.0_详细设计_类接口与头文件.md 第 12 节实现。
 * 实现 IPacketListener，回调在 Transport 工作线程执行。
 */

#ifndef CALMCAR_XCP_COMMAND_EXECUTOR_HPP_
#define CALMCAR_XCP_COMMAND_EXECUTOR_HPP_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "libxcp/command_codec.hpp"
#include "libxcp/ixcp_transport.hpp"
#include "libxcp/response_parser.hpp"
#include "libxcp/session.hpp"
#include "libxcp/xcp_error.hpp"

namespace calmcar::xcp {

/**
 * @brief 命令超时配置
 *
 * 数值由调用方提供；XCP 的 t1..t7 通常来自 A2L，本阶段不宣称协议固定值
 * （计划文档 §11.8）。默认值为项目保守取值。
 */
struct CommandTimeouts {
    /// @brief 普通命令超时（毫秒）
    std::chrono::milliseconds command_timeout{1000};

    /// @brief SYNCH 恢复超时（毫秒）
    std::chrono::milliseconds synch_timeout{1000};

    /// @brief 最大恢复重试次数（不含首次尝试，设计决策 D2）
    int max_retries{2};
};

/**
 * @brief 事件观察者接口（可选，用于上层接收异步 EV/SERV/DTO）
 *
 * 回调在 Transport 工作线程执行；实现者不得在其中阻塞等待命令响应，
 * 否则会与单 Outstanding Command 模型自锁。
 */
class IEventListener {
public:
    virtual ~IEventListener() = default;

    /// @brief 收到异步 Event
    virtual void OnEvent(const EventPacket& event) = 0;

    /// @brief 收到异步 Service Request
    virtual void OnService(const ServicePacket& service) = 0;

    /// @brief 收到 DTO（本阶段仅识别，不解析内容）
    virtual void OnDto(const DtoPacket& dto) = 0;
};

/**
 * @brief 命令执行器
 *
 * 负责命令的发送-等待-解析-恢复。Standard Communication Model：同一时刻最多
 * 一个 Pending Command。Codec/Parser 在 CONNECT 协商出字节序后按该字节序创建。
 *
 * @par 死锁规避（设计 §16.3）
 *   OnPacketReceived() 在工作线程执行，先在锁内取出响应/事件，释放锁后再回调
 *   IEventListener；不在持锁状态下回调外部监听器。
 */
class CommandExecutor : public IPacketListener {
public:
    /**
     * @brief 构造执行器
     * @param transport Transport 实例（非拥有）
     * @param session Session 实例（非拥有）
     * @param timeouts 超时配置
     * @param event_listener 事件监听器（可选，可为 nullptr）
     */
    explicit CommandExecutor(IXcpTransport& transport, Session& session,
                             CommandTimeouts timeouts = {},
                             IEventListener* event_listener = nullptr);

    /// @brief 析构；唤醒并终止内部等待
    ~CommandExecutor() override;

    // 禁止拷贝
    CommandExecutor(const CommandExecutor&) = delete;
    CommandExecutor& operator=(const CommandExecutor&) = delete;

    // ---- IPacketListener 实现 ----

    void OnPacketReceived(BytesView packet) override;
    void OnTransportClosed(std::string_view reason) override;
    void OnTransportWarning(std::string_view message) override;

    [[nodiscard]] IPacketListener& AsListener() noexcept;

    // ---- 命令执行 ----

    /**
     * @brief 执行 CONNECT 命令并按响应字节序建立 Codec/Parser
     * @param mode 0x00=普通, 0x01=用户自定义
     * @throws XcpException 超时、协议错误、参数非法或恢复失败
     */
    [[nodiscard]] ConnectResponse ExecuteConnect(std::uint8_t mode = 0x00);
    void ExecuteDisconnect();
    [[nodiscard]] GetStatusResponse ExecuteGetStatus();
    [[nodiscard]] std::optional<GetCommModeInfoResponse>
    ExecuteGetCommModeInfo();
    void ExecuteSetMta(AddressExtension extension, Address address);
    [[nodiscard]] Bytes ExecuteUpload(ElementCount number_of_elements);
    [[nodiscard]] Bytes ExecuteShortUpload(ElementCount number_of_elements,
                                           AddressExtension extension,
                                           Address address);
    bool SendSynch();

private:
    /// @brief 单条命令的执行流程：状态检查 -> 发送 -> 等待 -> 错误分派 ->
    /// 恢复重试
    [[nodiscard]] ParsedPacket RunCommand(CommandCode cmd,
                                          const Bytes& encoded_packet);
    [[nodiscard]] std::optional<ParsedPacket> PerformAttempt(
        CommandCode cmd, BytesView encoded_packet);
    [[nodiscard]] std::optional<ParsedPacket> WaitForResponse(
        std::chrono::milliseconds timeout);
    [[nodiscard]] ParsedPacket DispatchResponse(CommandCode cmd,
                                                const ParsedPacket& response);
    void PerformRecovery(CommandCode cmd);
    void RestoreUploadMta();
    void EnsureCodec(ByteOrder byte_order);
    void CheckResLength(CommandCode cmd, const PositiveResponse& res,
                        ElementCount elements);

    // ---- 依赖 ----
    IXcpTransport& m_transport_;        ///< 传输层（非拥有）
    Session& m_session_;                ///< 会话（非拥有）
    CommandTimeouts m_timeouts_;        ///< 超时与重试配置
    IEventListener* m_event_listener_;  ///< 事件监听器（非拥有，可空）

    std::unique_ptr<CommandCodec>
        m_codec_;  ///< 命令编码器（按 Session 字节序创建）
    std::unique_ptr<ResponseParser>
        m_parser_;  ///< 响应解析器（按 Session 字节序创建）

    // ---- 同步状态 ----
    mutable std::mutex m_mutex_;                      ///< 保护以下字段
    std::condition_variable m_response_cv_;           ///< 最终响应到达通知
    std::condition_variable m_synch_cv_;              ///< SYNCH 确认到达通知
    std::optional<ParsedPacket> m_pending_response_;  ///< 待取走的最终响应
    bool m_response_ready_{false};                    ///< 响应槽位已被填充
    bool m_synch_confirmed_{false};                   ///< 已收到 ERR_CMD_SYNCH
    bool m_in_recovery_{false};             ///< 当前正在等待 SYNCH 确认
    bool m_transport_closed_{false};        ///< Transport 已关闭
    std::string m_transport_close_reason_;  ///< 关闭原因
    bool m_cmd_pending_received_{
        false};  ///< 本轮收到 EV_CMD_PENDING，需重启计时
    std::optional<XcpAddress40>
        m_last_mta_;  ///< 最近一次成功 SET_MTA 的地址（隐含状态）
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_COMMAND_EXECUTOR_HPP_
