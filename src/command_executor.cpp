/**
 * @file command_executor.cpp
 * @brief CommandExecutor 实现：命令调度、响应分派与 SYNCH 恢复。
 *
 * 错误策略依据 code-plan/XCP_1.3.0_最小协议核心实现计划.md 第 6.2~6.4 节表格。
 */

#include "libxcp/command_executor.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "libxcp/log.hpp"

namespace calmcar::xcp {

namespace {

/// @brief 将字节格式化为两位十六进制（用于诊断文本）
std::string toHex(std::uint8_t value) {
    constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out = "0x";
    out.push_back(kDigits[(value >> 4) & 0x0FU]);
    out.push_back(kDigits[value & 0x0FU]);
    return out;
}

#if LIBXCP_ENABLE_LOG
/// @brief 判断命令是否包含 Seed 或 Key 敏感内容。
bool IsSeedKeyCommand(CommandCode command) noexcept {
    return command == CommandCode::GetSeed || command == CommandCode::Unlock;
}

/// @brief 报文摘要及长度/脱敏元信息。
struct PacketLogSummary {
    std::string data;         ///< 十六进制摘要或脱敏标记。
    std::size_t shown_bytes;  ///< 实际显示的原始字节数。
    bool truncated;           ///< 是否因长度上限截断。
    bool redacted;            ///< 是否因敏感信息规则脱敏。
};

/// @brief 生成受长度上限与 Seed/Key 规则保护的报文摘要。
PacketLogSummary SummarizePacket(BytesView packet, bool redact) {
    if (redact) {
        return {"<redacted>", 0, false, true};
    }

    constexpr std::size_t kMaximumBytes = 64;
    const std::size_t shown = std::min(packet.size(), kMaximumBytes);
    std::string summary;
    for (std::size_t index = 0; index < shown; ++index) {
        if (!summary.empty()) {
            summary.push_back(' ');
        }
        summary += toHex(packet[index]);
    }
    if (packet.size() > shown) {
        summary += " ... (" + std::to_string(packet.size() - shown) +
                   " bytes omitted)";
    }
    return {std::move(summary), shown, packet.size() > shown, false};
}

/// @brief 返回 PID 的协议包类型名称。
std::string_view PacketTypeName(std::uint8_t pid) noexcept {
    switch (pid) {
        case static_cast<std::uint8_t>(PacketType::Res):
            return "RES";
        case static_cast<std::uint8_t>(PacketType::Err):
            return "ERR";
        case static_cast<std::uint8_t>(PacketType::Ev):
            return "EV";
        case static_cast<std::uint8_t>(PacketType::Serv):
            return "SERV";
        default:
            return "DTO";
    }
}
#endif

/// @brief 构造带命令上下文的 ProtocolError 消息
std::string protocolMessage(std::string_view prefix, CommandCode cmd,
                            const NegativeResponse& err) {
    std::string msg(std::string(prefix) + " 命令 ");
    msg += toHex(static_cast<std::uint8_t>(cmd));
    msg += " 收到 ERR ";
    // 已识别的错误码用规范名称；未知码保留原始字节的十六进制表示（计划 §6.4）
    msg += err.error_code ? std::string(ErrorCodeName(*err.error_code))
                          : toHex(err.raw_error_code);
    if (!err.additional_info.empty()) {
        msg += "（附加信息 " + std::to_string(err.additional_info.size()) +
               " 字节）";
    }
    return msg;
}

/// @brief 判断异常是否为"可选命令未被 Slave 实现"（批次14 降级判据）
/// @details docs L1631：未实现的可选命令必须回 ERR_CMD_UNKNOWN 且**不产生
///          副作用**，因此可安全解释为"能力缺失"而非链路故障。判据与既有
///          ExecuteGetCommModeInfo / ExecuteShortUpload 的降级分支同口径。
bool IsCmdUnknown(const XcpException& e) {
    return e.Category() == ErrorCategory::ProtocolError && e.GetErrorCode() &&
           *e.GetErrorCode() == ErrorCode::CmdUnknown;
}

/// @brief 判断响应超时后不可安全重放的标定变更命令。
/// @details 响应缺失无法证明 Slave 未执行；内存写、页面切换/冻结及页面复制
///          若被重放，可能覆盖并发变化或重复产生副作用。
bool HasUnknownOutcomeOnTimeout(CommandCode cmd) {
    switch (cmd) {
        case CommandCode::ModifyBits:
        case CommandCode::Download:
        case CommandCode::ShortDownload:
        case CommandCode::SetCalPage:
        case CommandCode::SetSegmentMode:
        case CommandCode::CopyCalPage:
            return true;
        default:
            return false;
    }
}

std::string_view NonReplayableCommandName(CommandCode cmd) {
    switch (cmd) {
        case CommandCode::ModifyBits:
            return "MODIFY_BITS";
        case CommandCode::Download:
            return "DOWNLOAD";
        case CommandCode::ShortDownload:
            return "SHORT_DOWNLOAD";
        case CommandCode::SetCalPage:
            return "SET_CAL_PAGE";
        case CommandCode::SetSegmentMode:
            return "SET_SEGMENT_MODE";
        case CommandCode::CopyCalPage:
            return "COPY_CAL_PAGE";
        default:
            return "标定变更命令";
    }
}

}  // namespace

std::string RecoveryPolicy::Validate() const {
    // 方案 §4.6 第 7 条：负数、零值、溢出在构造或 Connect 前失败。
    // 使用 milliseconds 的 rep（通常为 int64）判定，避免窄化比较。
    const auto busy_delay = busy_retry_delay.count();
    const auto total = busy_total_timeout.count();
    if (busy_retry_enabled) {
        if (busy_delay <= 0) {
            return "busy_retry_delay 必须为正数（当前 " +
                   std::to_string(busy_delay) + " ms）";
        }
        if (total <= 0) {
            return "busy_total_timeout 必须为正数（当前 " +
                   std::to_string(total) + " ms）";
        }
        // 总 deadline 必须足以容纳至少一次等待，否则配置自相矛盾。
        if (total < busy_delay) {
            return "busy_total_timeout(" + std::to_string(total) +
                   " ms) 小于 busy_retry_delay(" + std::to_string(busy_delay) +
                   " ms)：总体 deadline 无法容纳任何一次 BUSY 等待";
        }
    }
    return {};
}

CommandExecutor::CommandExecutor(IXcpTransport& transport, Session& session,
                                 CommandTimeouts timeouts,
                                 IEventListener* event_listener,
                                 RecoveryPolicy recovery_policy)
    : m_transport_(transport),
      m_session_(session),
      m_timeouts_(timeouts),
      m_recovery_policy_(recovery_policy),
      m_event_listener_(event_listener) {}

bool CommandExecutor::IsCancelled() const {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    return m_cancelled_;
}

bool CommandExecutor::IsBusyResponse(const ParsedPacket& response) {
    const auto* err = std::get_if<NegativeResponse>(&response);
    return err != nullptr && err->error_code.has_value() &&
           *err->error_code == ErrorCode::CmdBusy;
}

bool CommandExecutor::WaitBusyDelay(std::chrono::milliseconds delay) {
    if (delay.count() <= 0) {
        return !IsCancelled();
    }
    std::unique_lock<std::mutex> lock(m_mutex_);
    const auto deadline = std::chrono::steady_clock::now() + delay;
    // 复用 m_response_cv_：任一 Packet 到达、取消或关闭都会唤醒；被唤醒后
    // 循环顶部统一判定取消/关闭，未满足条件则继续睡到本段 deadline。
    while (!m_cancelled_ && !m_transport_closed_) {
        if (m_response_cv_.wait_until(lock, deadline) ==
            std::cv_status::timeout) {
            break;
        }
    }
    const bool ok = !m_cancelled_ && !m_transport_closed_;
    lock.unlock();
    if (!ok) {
        LIBXCP_LOG_INFO("XCP BUSY wait interrupted (cancelled or closed)");
    }
    return ok;
}

void CommandExecutor::CancelPendingOperation() {
    // 线程安全：置取消标志并唤醒全部等待者。被唤醒的等待循环看到标志后
    // 统一抛出 Cancelled（不伪装成 Timeout/ProtocolError）。
    {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_cancelled_ = true;
        m_cmd_pending_received_ = false;
    }
    m_response_cv_.notify_all();
    m_synch_cv_.notify_all();
    LIBXCP_LOG_INFO("XCP pending operation cancelled by caller");
}

RecoveryPolicy CommandExecutor::GetRecoveryPolicy() const { return m_recovery_policy_; }

CommandExecutor::~CommandExecutor() {
    // 唤醒任何仍在等待的调用方，避免析构后条件变量被永久阻塞
    {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_transport_closed_ = true;
        if (m_transport_close_reason_.empty()) {
            m_transport_close_reason_ = "CommandExecutor 已销毁";
        }
    }
    m_response_cv_.notify_all();
    m_synch_cv_.notify_all();
}

IPacketListener& CommandExecutor::AsListener() noexcept { return *this; }

void CommandExecutor::OnTransportReopened() noexcept {
    // 只清"通道级"状态。这里刻意**不清** m_cancelled_：
    // 取消语义由 RunCommand 在每条顶层命令开始前自行清除（见该函数注释），
    // 两处都清会让"重连前的取消意图"渗入新会话的判定，反而模糊责任边界。
    const std::lock_guard<std::mutex> lock(m_mutex_);
    m_transport_closed_ = false;
    m_transport_close_reason_.clear();
    // 迟到响应槽位：上一会话的残留响应绝不能喂给新会话的第一条命令，
    // 否则会对错包做出"成功"判定——比超时更危险，因此必须丢弃。
    m_pending_response_.reset();
    m_response_ready_ = false;
    m_cmd_pending_received_ = false;
    // 恢复/同步标志：二者本就在 SendSynch 入口与出口各自复位，命令之间正常
    // 情况下已是 false。这里显式清零属**防御性**收尾——它们跨会话存在没有
    // 意义，留着只会让"新会话从干净状态起步"这一契约依赖别处的时序假设。
    m_synch_confirmed_ = false;
    m_in_recovery_ = false;
    LIBXCP_LOG_INFO(
        "XCP CommandExecutor reset for reopened transport; stale channel "
        "state cleared");
}

bool CommandExecutor::IsTransportClosed() const noexcept {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    return m_transport_closed_;
}

void CommandExecutor::EnsureCodec(ByteOrder byte_order, AddressGranularity ag) {
    // AG 参与 Codec 重建判定：CONNECT 后 AG 才确定，且 DOWNLOAD 的
    // 对齐字节数依赖它（原始 PDF 第 142 页 Table 82），故 AG 变化必须重建。
    if (!m_codec_ || m_codec_->GetByteOrder() != byte_order ||
        m_codec_->GetAddressGranularity() != ag) {
        m_codec_ = std::make_unique<CommandCodec>(byte_order, ag);
    }
    // 解析器同样按 AG 重建：GET_ID 的 Length mod AG 判据（F-03c）依赖
    // 会话实际 AG，AG 变化后沿用旧解析器会按错误粒度判定。
    if (!m_parser_ || m_parser_->GetByteOrder() != byte_order ||
        m_parser_->GetAddressGranularity() != ag) {
        m_parser_ = std::make_unique<ResponseParser>(byte_order, ag);
    }
}

// ---------------------------------------------------------------------------
// IPacketListener：在 Transport 工作线程执行
// ---------------------------------------------------------------------------

void CommandExecutor::OnPacketReceived(BytesView packet) {
    // 解析必须在锁外完成：parser 无共享状态，且避免长时间持有 m_mutex_。
    // expectedCommand 仅在 PositiveResponse 中作上下文标记；响应匹配依赖
    // “单 Pending Command + RES/ERR PID”，不使用 CTR 匹配（计划 §4.7）。
    // 日志另行保留真实 Pending Command，不能把解析用的默认值当作真实上下文。
    const auto pending_command = m_session_.PendingCommand();
    const auto expected = pending_command.value_or(CommandCode::GetStatus);
    if (packet.empty()) {
        LIBXCP_LOG_WARNING("收到空 XCP Packet");
        return;
    }
    if (!m_parser_) {
        LIBXCP_LOG_WARNING("Parser 尚未就绪时收到 XCP Packet: pid={}, bytes={}",
                           toHex(packet.front()), packet.size());
        return;  // CONNECT 之前尚无 Parser：此时不应有任何 Slave 流量
    }
    const auto parsed = m_parser_->Parse(packet, expected);
    if (!parsed) {
        LIBXCP_LOG_WARNING("收到畸形 XCP Packet: pid={}, bytes={}",
                           toHex(packet.front()), packet.size());
        return;  // 空包/畸形包：报告但不终止会话
    }

#if LIBXCP_ENABLE_LOG
    const bool response_packet =
        packet.front() == static_cast<std::uint8_t>(PacketType::Res) ||
        packet.front() == static_cast<std::uint8_t>(PacketType::Err);
    // 响应没有事务关联 ID，不能根据当前 pending 命令判断其内容安全；迟到的
    // GET_SEED/UNLOCK 响应可能落入其他命令上下文，因此所有 RES/ERR 正文均脱敏。
    const bool redact_packet = response_packet;
    const PacketLogSummary packet_summary =
        SummarizePacket(packet, redact_packet);
    const std::string command_context =
        !response_packet
            ? "n/a"
            : (pending_command
                   ? toHex(static_cast<std::uint8_t>(*pending_command))
                   : "unknown");
    LIBXCP_LOG_INFO(
        "XCP RX {} pid={} bytes={} command={} shown_bytes={} truncated={} "
        "redacted={} data={}",
        PacketTypeName(packet.front()), toHex(packet.front()), packet.size(),
        command_context, packet_summary.shown_bytes, packet_summary.truncated,
        packet_summary.redacted, packet_summary.data);
#endif

    bool notify_response = false;
    bool notify_synch = false;
    std::optional<EventPacket> async_event;
    std::optional<ServicePacket> async_service;
    std::optional<DtoPacket> async_dto;
    bool notify_malformed = false;
    // 控制包的分流结论在锁内决定，在锁外记录；DTO 不增加逐帧处理日志。
    std::string_view control_route;

    {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        if (m_transport_closed_) {
            if (!std::holds_alternative<DtoPacket>(*parsed)) {
                LIBXCP_LOG_INFO(
                    "XCP RX ignored: transport already closed pid={}",
                    toHex(packet.front()));
            }
            return;  // 关闭后到达的迟到包忽略
        }

        if (const auto* res = std::get_if<PositiveResponse>(&*parsed)) {
            // 响应槽位已被占用说明协议失步：丢弃并上报畸形，绝不覆盖前一条响应
            if (m_response_ready_) {
                notify_malformed = true;
                control_route =
                    "duplicate RES discarded; response slot occupied";
            } else {
                m_pending_response_ = *res;
                m_response_ready_ = true;
                notify_response = true;
                control_route = "RES stored; waiter notified";
            }
        } else if (const auto* err = std::get_if<NegativeResponse>(&*parsed)) {
            if (m_in_recovery_ && err->error_code &&
                *err->error_code == ErrorCode::CmdSynch) {
                // ERR_CMD_SYNCH 仅在 Recovery 中视为成功确认（计划 §6.2 第 1
                // 条）
                m_synch_confirmed_ = true;
                notify_synch = true;
                control_route =
                    "ERR_CMD_SYNCH confirms recovery; SYNCH waiter notified";
            } else if (m_response_ready_) {
                notify_malformed = true;
                control_route =
                    "duplicate ERR discarded; response slot occupied";
            } else {
                m_pending_response_ = *err;
                m_response_ready_ = true;
                notify_response = true;
                control_route = "ERR stored; waiter notified";
            }
        } else if (const auto* ev = std::get_if<EventPacket>(&*parsed)) {
            if (ev->event_code && *ev->event_code == EventCode::CmdPending) {
                // EV_CMD_PENDING：重启当前命令 Timer，不重发原命令（计划
                // §6.3）。
                // 只置标记并唤醒等待循环，不占用响应槽位；事件本身仍需上报。
                m_cmd_pending_received_ = true;
                notify_response = true;
                control_route =
                    "EV_CMD_PENDING marked; waiter notified; command not "
                    "replayed";
            } else {
                control_route =
                    "EV delivered asynchronously; response slot unchanged";
            }
            async_event = *ev;
        } else if (const auto* serv = std::get_if<ServicePacket>(&*parsed)) {
            control_route =
                "SERV delivered asynchronously; response slot unchanged";
            async_service = *serv;
        } else if (const auto* dto = std::get_if<DtoPacket>(&*parsed)) {
            async_dto = *dto;
        }
    }

    if (!control_route.empty()) {
        LIBXCP_LOG_INFO("XCP control route: {}", control_route);
    }
    if (notify_response) {
        m_response_cv_.notify_all();
    }
    if (notify_synch) {
        m_synch_cv_.notify_all();
    }

    // 释放锁后再回调监听器（设计 §16.3）
    if (m_event_listener_ != nullptr) {
        try {
            if (async_event) {
                m_event_listener_->OnEvent(*async_event);
            } else if (async_service) {
                m_event_listener_->OnService(*async_service);
            } else if (async_dto) {
                m_event_listener_->OnDto(*async_dto);
            }
        } catch (...) {
        }
    }
    if (notify_malformed) {
        // 协议失步（响应槽位被占用时又收到 RES/ERR）：作为 Transport
        // 级警告上报， 由等待方按超时处理，不覆盖已有响应。
        OnTransportWarning("收到多余 RES/ERR，疑似协议失步，已丢弃");
    }
}

void CommandExecutor::OnTransportClosed(std::string_view reason) {
    LIBXCP_LOG_INFO("XCP Transport closed: {}", reason);
    {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_transport_closed_ = true;
        m_transport_close_reason_ = std::string(reason);
        // F-11（§4.6 第 6 条）：Transport Close 属「取消类」唤醒——等待者
        // 必须返回 Cancelled 而非 Timeout，避免调用方把通道关闭误判为对端
        // 无响应并触发无意义的恢复。
        m_cancelled_ = true;
    }
    m_response_cv_.notify_all();
    m_synch_cv_.notify_all();
}

void CommandExecutor::OnTransportWarning(std::string_view message) {
    // 关闭日志后宏不求值参数；此处仍明确消费参数，避免OFF构建未使用警告。
    (void)message;
    // Transport 层可恢复诊断（畸形 Datagram、CTR
    // 缺口等）只记录，不改变事务状态。
    LIBXCP_LOG_WARNING("XCP Transport warning: {}", message);
}

// ---------------------------------------------------------------------------
// 等待与分派
// ---------------------------------------------------------------------------

std::optional<ParsedPacket> CommandExecutor::WaitForResponse(
    std::chrono::milliseconds timeout) {
    LIBXCP_LOG_INFO("XCP response wait started timeout_ms={}", timeout.count());
    std::unique_lock<std::mutex> lock(m_mutex_);
    auto deadline = std::chrono::steady_clock::now() + timeout;

    while (true) {
        if (m_response_ready_ && m_pending_response_) {
            auto out = *m_pending_response_;
            m_pending_response_.reset();
            m_response_ready_ = false;
            m_cmd_pending_received_ = false;
            // 槽位状态在临界区内确定，解锁后再输出，避免 sink 阻塞接收线程。
            lock.unlock();
            LIBXCP_LOG_INFO(
                "XCP response slot consumed; pending event cleared");
            return out;
        }
        // F-11：调用方取消优先于超时判定——必须在临界区内确定并返回 nullopt，
        // 由 RunCommand 统一转换为 Cancelled（而非伪装成 Timeout）。
        if (m_cancelled_) {
            m_cmd_pending_received_ = false;
            lock.unlock();
            LIBXCP_LOG_INFO("XCP response wait interrupted: cancelled");
            return std::nullopt;
        }
        if (m_transport_closed_) {
            m_cmd_pending_received_ = false;
            lock.unlock();
            LIBXCP_LOG_WARNING(
                "XCP response wait interrupted: transport closed");
            return std::nullopt;
        }

        // 期间收到过 EV_CMD_PENDING：重启本轮 Timer，不重发原命令（计划 §6.3）
        if (m_cmd_pending_received_) {
            m_cmd_pending_received_ = false;
            deadline = std::chrono::steady_clock::now() + timeout;
            // 必须记录和 deadline 同一次原子状态决定，低频控制事件锁内输出。
            LIBXCP_LOG_INFO(
                "XCP EV_CMD_PENDING restarted response timer timeout_ms={}; "
                "command not replayed",
                timeout.count());
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            m_cmd_pending_received_ = false;
            lock.unlock();
            LIBXCP_LOG_WARNING("XCP response wait expired timeout_ms={}",
                               timeout.count());
            return std::nullopt;
        }
        if (m_response_cv_.wait_until(lock, deadline) ==
            std::cv_status::timeout) {
            continue;  // 回到循环顶部统一判定（可能同时有 Pending 标记待重启）
        }
    }
}

ParsedPacket CommandExecutor::DispatchResponse(CommandCode cmd,
                                               const ParsedPacket& response) {
    if (const auto* err = std::get_if<NegativeResponse>(&response)) {
        const std::string error_code =
            err->error_code ? std::string(ErrorCodeName(*err->error_code))
                            : toHex(err->raw_error_code);
        const bool optional_command_unsupported =
            err->error_code && *err->error_code == ErrorCode::CmdUnknown &&
            (cmd == CommandCode::GetCommModeInfo ||
             cmd == CommandCode::ShortUpload);
        if (optional_command_unsupported) {
            LIBXCP_LOG_WARNING(
                "Optional XCP command unsupported cmd={} code={} "
                "additional_bytes={}",
                toHex(static_cast<std::uint8_t>(cmd)), error_code,
                err->additional_info.size());
        } else {
            LIBXCP_LOG_ERROR("XCP RX ERR cmd={} code={} additional_bytes={}",
                             toHex(static_cast<std::uint8_t>(cmd)), error_code,
                             err->additional_info.size());
        }
        // 未知错误码：保留原值并上报 ProtocolError（计划 §6.4）；不使用 Generic
        // 顶替， 以免让上层误判为已识别的标准错误码。
        const std::string message =
            protocolMessage("Slave 返回负响应", cmd, *err);
        if (!err->error_code) {
            throw XcpException(
                ErrorCategory::ProtocolError,
                message + "（未知错误码 " + toHex(err->raw_error_code) + "）",
                cmd, std::nullopt);
        }
        const ErrorCode code = *err->error_code;

        // GET_COMM_MODE_INFO / SHORT_UPLOAD 遇 ERR_CMD_UNKNOWN：按替代路径降级
        if (code == ErrorCode::CmdUnknown) {
            if (cmd == CommandCode::GetCommModeInfo) {
                throw detail::MakeProtocolError(
                    "Slave 不支持 GET_COMM_MODE_INFO，按可选能力降级", cmd,
                    code);
            }
            if (cmd == CommandCode::ShortUpload) {
                m_session_.DisableShortUpload();
                throw detail::MakeProtocolError(
                    "Slave 不支持 SHORT_UPLOAD，已标记降级到 SET_MTA+UPLOAD",
                    cmd, code);
            }
        }
        // ERR_ACCESS_LOCKED（批次 7）：Seed&Key 解锁功能已落地，不再映射为
        // UnsupportedFeature。区分两种语义：
        //  - UNLOCK 命令：Key 校验失败，Slave 随后主动断开会话（规范
        //    §7.5.1.9），Session 标记 Failed 由 ExecuteUnlock 完成；
        //  - 其他命令：资源受 Seed&Key 保护，按计划 §6.4 只报告、不自动解锁，
        //    指引调用方显式调用 XcpMaster::Unlock()。
        if (code == ErrorCode::AccessLocked) {
            if (cmd == CommandCode::Unlock) {
                throw detail::MakeProtocolError(
                    "UNLOCK Key 校验失败，Slave 已主动断开会话", cmd, code);
            }
            throw detail::MakeProtocolError(
                protocolMessage("资源受 Seed&Key 保护，请先调用 Unlock() 解锁",
                                cmd, *err),
                cmd, code);
        }
        throw detail::MakeProtocolError(message, cmd, code);
    }

    if (const auto* res = std::get_if<PositiveResponse>(&response)) {
        return *res;
    }
    // EV/SERV/DTO 不会进入此函数（已在 OnPacketReceived 分流）
    throw detail::MakeMalformedPacket("等待最终响应时收到非预期 Packet 类型");
}

void CommandExecutor::CheckResLength(CommandCode cmd, PositiveResponse& res,
                                     ElementCount elements) {
    const auto ag = m_session_.GetAddressGranularity();
    const auto expected = static_cast<std::size_t>(elements) * AgToBytes(ag);
    if (res.data.size() == expected) {
        return;
    }

    // XCPlite 动态地址的异步响应可能按 4 字节对齐，且把尾部填充算入线上长度。
    // 只接受精确匹配“PID + 数据补齐到 4 字节”的长度，其他超长/短包仍视为畸形。
    const auto aligned_packet_size = ((expected + 4U) / 4U) * 4U;
    const auto padded_data_size = aligned_packet_size - 1U;
    if ((cmd == CommandCode::Upload || cmd == CommandCode::ShortUpload) &&
        res.data.size() == padded_data_size) {
        res.data.resize(expected);
        return;
    }

    LIBXCP_LOG_ERROR(
        "XCP 响应长度错误 cmd={} actual_bytes={} expected_bytes={}",
        toHex(static_cast<std::uint8_t>(cmd)), res.data.size(), expected);
    throw XcpException(
        ErrorCategory::MalformedPacket,
        "命令 " + std::to_string(static_cast<int>(cmd)) + " 响应数据长度 " +
            std::to_string(res.data.size()) +
            " 不等于期望的 elements*AG = " + std::to_string(expected),
        cmd);
}

// ---------------------------------------------------------------------------
// 单条命令流程（含超时恢复与有限重试）
// ---------------------------------------------------------------------------

std::optional<ParsedPacket> CommandExecutor::PerformAttempt(
    CommandCode cmd, BytesView encoded_packet) {
    m_session_.MarkCommandSent(cmd);
    try {
        m_transport_.Send(encoded_packet);
#if LIBXCP_ENABLE_LOG
        const PacketLogSummary packet_summary =
            SummarizePacket(encoded_packet, IsSeedKeyCommand(cmd));
        LIBXCP_LOG_INFO(
            "XCP TX cmd={} bytes={} shown_bytes={} truncated={} redacted={} "
            "data={}",
            toHex(static_cast<std::uint8_t>(cmd)), encoded_packet.size(),
            packet_summary.shown_bytes, packet_summary.truncated,
            packet_summary.redacted, packet_summary.data);
#endif
    } catch (...) {
        LIBXCP_LOG_ERROR("XCP TX failed cmd={} bytes={}",
                         toHex(static_cast<std::uint8_t>(cmd)),
                         encoded_packet.size());
        m_session_.ClearPendingCommand();
        throw;
    }
    const auto response = WaitForResponse(m_timeouts_.command_timeout);
    m_session_.ClearPendingCommand();
    return response;  // nullopt 表示超时或 Transport 关闭
}

void CommandExecutor::RestoreUploadMta() {
    std::optional<XcpAddress40> mta;
    {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        mta = m_last_mta_;
    }
    if (!mta) {
        LIBXCP_LOG_INFO("XCP recovery: no cached MTA to restore");
        return;  // 本 Session 未设置过 MTA，无可恢复的隐含状态
    }

    // 不记录地址、扩展或数据，避免泄露标定位置。
    LIBXCP_LOG_INFO("XCP recovery: restoring cached MTA");
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeSetMta(mta->extension, mta->address);
    const auto response = PerformAttempt(CommandCode::SetMta, encoded);
    if (!response) {
        // SET_MTA 自身也失败：不再掩盖，交由随后的 UPLOAD 重试把错误暴露出来
        return;
    }
    (void)DispatchResponse(CommandCode::SetMta, *response);
}

void CommandExecutor::SetSeedKeySegment(SeedKeySegment segment) {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    m_seed_key_segment_ = segment;
}

void CommandExecutor::ResetSeedKeyTransaction() {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    m_seed_key_segment_ = SeedKeySegment::NotSegmented;
    m_seed_key_restarts_ = 0;
}

void CommandExecutor::ClearSeedKeySegment() {
    const std::lock_guard<std::mutex> lock(m_mutex_);
    m_seed_key_segment_ = SeedKeySegment::NotSegmented;
}

void CommandExecutor::RunSegmentedRecovery(CommandCode cmd,
                                           SeedKeySegment segment) {
    const std::string command_name =
        cmd == CommandCode::GetSeed ? "GET_SEED" : "UNLOCK";

    // 末段：资源可能已经（对被解锁而言）或已经完整交付（对 GET_SEED 而言），
    // 属结果未知写操作。规范要求 Key 直到序列完成后才校验，因此末段既不能
    // 重放、也不能自动重连后重启，更不能继续受保护操作（方案 §2C.2）。
    if (segment == SeedKeySegment::Final) {
        LIBXCP_LOG_ERROR(
            "XCP segmented seed&key final segment timeout cmd={}",
            command_name);
        // 只清段位置：事务将由调用方放弃，重启计数由 ResetSeedKeyTransaction
        // 在事务层收尾时清零，此处保留以维持诊断信息
        ClearSeedKeySegment();
        throw XcpException(
            ErrorCategory::OperationOutcomeUnknown,
            command_name +
                " 末段响应超时，执行结果未知；已禁止自动重放，"
                "请由调用方决定是否断开重连",
            cmd, std::nullopt, 0);
    }

    // 非末段：事务级重启上限（与单命令 repeat 2 times 语义不同）
    constexpr int kMaxTransactionRestarts = 1;
    int restarts = 0;
    {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        restarts = m_seed_key_restarts_;
    }
    if (restarts >= kMaxTransactionRestarts) {
        LIBXCP_LOG_ERROR(
            "XCP segmented seed&key restart budget exhausted cmd={}",
            command_name);
        ClearSeedKeySegment();
        throw XcpException(ErrorCategory::RecoveryFailed,
                           command_name + " 分段事务重启次数已用尽（" +
                               std::to_string(restarts) + " 次）",
                           cmd, std::nullopt, restarts);
    }
    {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        ++m_seed_key_restarts_;
    }

    // UNLOCK 非末段：Slave 可能已缓存部分 Key，单独 SYNCH 不足以证明分段
    // 状态已复位（Key 最后才校验，失步不会立即暴露）。必须先断开重连。
    if (cmd == CommandCode::Unlock) {
        LIBXCP_LOG_WARNING(
            "XCP segmented UNLOCK mid-segment timeout; requiring reconnect");
        ClearSeedKeySegment();
        throw XcpException(
            ErrorCategory::OperationOutcomeUnknown,
            "UNLOCK 非末段响应超时，Slave 可能已缓存部分 Key；"
            "须先断开重连，再从 GET_SEED 完整重启解锁序列",
            cmd, std::nullopt, restarts);
    }

    // GET_SEED 任一段：清除本地分段状态后 SYNCH。SYNCH 成功后由事务层
    // （XcpMaster::Unlock）从 GET_SEED(Mode=0) 完整重启。
    LIBXCP_LOG_WARNING(
        "XCP segmented GET_SEED timeout; resetting transaction and SYNCH");
    // 保留重启计数：预算须在整个事务期间累积，只有事务层收尾才清零
    ClearSeedKeySegment();
    PerformRecovery(cmd);
}

void CommandExecutor::PerformRecovery(CommandCode cmd) {
    // Transport 已断开时不执行 SYNCH（计划 §6.2 第 5 条）
    if (!m_transport_.IsOpen()) {
        std::string reason;
        {
            const std::lock_guard<std::mutex> lock(m_mutex_);
            reason = m_transport_close_reason_;
        }
        LIBXCP_LOG_ERROR("XCP recovery unavailable: transport closed cmd={}",
                         toHex(static_cast<std::uint8_t>(cmd)));
        throw XcpException(ErrorCategory::RecoveryFailed,
                           "Transport 已关闭，无法执行 SYNCH 恢复", cmd,
                           std::nullopt, 0, reason);
    }
    if (!SendSynch()) {
        LIBXCP_LOG_ERROR("XCP SYNCH recovery failed for cmd={}",
                         toHex(static_cast<std::uint8_t>(cmd)));
        throw XcpException(ErrorCategory::RecoveryFailed,
                           "SYNCH 未得到 ERR_CMD_SYNCH 确认", cmd);
    }
}

bool CommandExecutor::SendSynch() {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes synch = m_codec_->EncodeSynch();

    {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_synch_confirmed_ = false;
        m_in_recovery_ = true;
        m_pending_response_.reset();
        m_response_ready_ = false;
    }
    LIBXCP_LOG_INFO(
        "XCP recovery initialized: response slot cleared; awaiting "
        "ERR_CMD_SYNCH");

    m_session_.BeginRecovery();
    bool confirmed = false;
    try {
        m_session_.MarkCommandSent(CommandCode::Synch);
        m_transport_.Send(synch);
#if LIBXCP_ENABLE_LOG
        const PacketLogSummary packet_summary = SummarizePacket(synch, false);
        LIBXCP_LOG_INFO(
            "XCP TX cmd={} bytes={} shown_bytes={} truncated={} redacted={} "
            "data={}",
            toHex(static_cast<std::uint8_t>(CommandCode::Synch)), synch.size(),
            packet_summary.shown_bytes, packet_summary.truncated,
            packet_summary.redacted, packet_summary.data);
#endif

        std::unique_lock<std::mutex> lock(m_mutex_);
        const auto deadline =
            std::chrono::steady_clock::now() + m_timeouts_.synch_timeout;
        while (!m_synch_confirmed_ && !m_transport_closed_ && !m_cancelled_) {
            if (m_synch_cv_.wait_until(lock, deadline) ==
                std::cv_status::timeout) {
                break;
            }
        }
        confirmed = m_synch_confirmed_;
    } catch (const XcpException&) {
        LIBXCP_LOG_WARNING("XCP SYNCH 发送或等待确认失败");
        confirmed = false;
    }

    {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_in_recovery_ = false;
    }
    m_session_.ClearPendingCommand();

    if (confirmed) {
        LIBXCP_LOG_INFO("XCP recovery confirmed by ERR_CMD_SYNCH");
        m_session_.CompleteRecovery();
    } else {
        LIBXCP_LOG_ERROR("XCP recovery unconfirmed; Session will fail");
        m_session_.Fail("SYNCH 恢复失败");
    }
    return confirmed;
}

ParsedPacket CommandExecutor::RunCommand(CommandCode cmd,
                                         const Bytes& encoded_packet) {
    // 前置状态检查：未连接/正在恢复/已有 Pending 均由 Session 抛出 InvalidState
    if (m_session_.HasPendingCommand()) {
        throw detail::MakeInvalidState("已有待响应的命令，拒绝并发发送");
    }

    // F-11：进入顶层命令前清除上一轮遗留的取消标志，使新操作不被历史取消
    // 影响。同时建立本次顶层操作的总体 deadline——自首次发送前开始计时，
    // 覆盖全部 BUSY 等待、重发与响应时间，**重试后不得重新计时**（§4.6 第 3 条）。
    {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_cancelled_ = false;
    }
    const auto busy_deadline =
        std::chrono::steady_clock::now() + m_recovery_policy_.busy_total_timeout;

    int retries = 0;
    while (true) {
        std::optional<ParsedPacket> response;
        try {
            response = PerformAttempt(cmd, encoded_packet);
        } catch (const XcpException&) {
            // TransportError / InvalidState 等：不做 SYNCH
            // 恢复，通道或状态本身不可用 （performAttempt 已清理 Pending）
            throw;
        } catch (const std::exception& e) {
            throw detail::MakeTransportError("发送命令时底层异常", e.what());
        }

        // F-11：取消优先于一切恢复策略。取消时不再 SYNCH、不再重试，直接以
        // Cancelled 返回给调用方（§4.6 第 6 条：不得与 Timeout 混用）。
        if (IsCancelled()) {
            LIBXCP_LOG_INFO("XCP command cancelled cmd={}",
                            toHex(static_cast<std::uint8_t>(cmd)));
            throw detail::MakeCancelled("命令等待被调用方取消", cmd);
        }

        if (response) {
            // F-11：ERR_CMD_BUSY 是**可恢复**错误——按规范恢复矩阵
            // 「ERR_CMD_BUSY → wait t7 → repeat ∞ times」，等待 t7 后重发。
            // 但库实现必须受总体 deadline 与取消控制，不能真正无限等待。
            if (IsBusyResponse(*response)) {
                if (!m_recovery_policy_.busy_retry_enabled) {
                    return DispatchResponse(cmd, *response);  // 关闭重试：旧行为
                }
                const auto now = std::chrono::steady_clock::now();
                if (now >= busy_deadline) {
                    LIBXCP_LOG_ERROR(
                        "XCP BUSY retry deadline exhausted cmd={} elapsed_ms={}",
                        toHex(static_cast<std::uint8_t>(cmd)),
                        m_recovery_policy_.busy_total_timeout.count());
                    throw XcpException(
                        ErrorCategory::RecoveryFailed,
                        "对端持续返回 ERR_CMD_BUSY，总体 deadline（" +
                            std::to_string(
                                m_recovery_policy_.busy_total_timeout.count()) +
                            " ms）已耗尽",
                        cmd, std::nullopt, retries);
                }
                // 等待 t7：取「剩余 deadline」与「配置间隔」的较小值，确保
                // 不会越过总体 deadline。等待可被取消/断连/析构唤醒。
                const auto remaining =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        busy_deadline - now);
                const auto wait_for =
                    (std::min)(remaining, m_recovery_policy_.busy_retry_delay);
                LIBXCP_LOG_WARNING(
                    "XCP ERR_CMD_BUSY; waiting t7 ms={} before retry cmd={}",
                    wait_for.count(),
                    toHex(static_cast<std::uint8_t>(cmd)));
                if (!WaitBusyDelay(wait_for)) {
                    // 等待期间被取消或 Transport 关闭：按取消语义返回。
                    throw detail::MakeCancelled(
                        "在 ERR_CMD_BUSY 等待期间被取消", cmd);
                }
                // 不增加 retries：BUSY 表示资源暂时不可用，不是超时恢复，
                // 其预算由上面的总体 deadline 约束，与「timeout 重试 2 次」
                // 是两套独立计数（§4.6 第 3 条）。
                continue;
            }
            // dispatchResponse 可能抛 ProtocolError 等结构化错误，交由上层处理
            return DispatchResponse(cmd, *response);
        }

        LIBXCP_LOG_WARNING("XCP command response timeout cmd={} retries={}",
                           toHex(static_cast<std::uint8_t>(cmd)), retries);

        // F-10：分段 Seed&Key 事务超时不得走通用重放路径。规范对「多段
        // 序列中途超时」未给出专门处理，通用路径的「SYNCH + 重放当前段」
        // 会因 Slave 侧段偏移不确定而静默失步（Key 直到序列末尾才校验，
        // 原始 PDF 第 130 页）。这里改为按段位置分派事务级恢复策略。
        {
            SeedKeySegment segment = SeedKeySegment::NotSegmented;
            {
                const std::lock_guard<std::mutex> lock(m_mutex_);
                segment = m_seed_key_segment_;
            }
            if (segment != SeedKeySegment::NotSegmented) {
                RunSegmentedRecovery(cmd, segment);
                // RunSegmentedRecovery 正常返回表示事务已重启，本段由事务层
                // 重新编排；此处不得重放原 encoded_packet。
                throw XcpException(
                    ErrorCategory::RecoveryFailed,
                    "分段 Seed&Key 事务已重启，原分段不再重放", cmd,
                    std::nullopt, retries);
            }
        }

        // 这些标定变更命令可能已在 Slave 生效；响应超时后即便 SYNCH 恢复
        // 成功也不得重放，否则可能重复写入或覆盖并发状态变化。报告结果未知，
        // 由调用方读回/对账。
        if (HasUnknownOutcomeOnTimeout(cmd)) {
            const std::string command_name(NonReplayableCommandName(cmd));
            if (retries >= m_timeouts_.max_retries) {
                LIBXCP_LOG_ERROR(
                    "XCP operation outcome unknown cmd={} retries={}",
                    command_name, retries);
                throw XcpException(
                    ErrorCategory::OperationOutcomeUnknown,
                    command_name +
                        " 响应超时，执行结果未知；恢复次数已耗尽，未重放命令",
                    cmd, std::nullopt, retries);
            }
            ++retries;
            LIBXCP_LOG_WARNING(
                "XCP outcome-unknown command requires SYNCH; will not replay "
                "cmd={} retry={}",
                command_name, retries);
            try {
                PerformRecovery(cmd);
            } catch (const XcpException& e) {
                LIBXCP_LOG_ERROR(
                    "XCP recovery failed for outcome-unknown cmd={} retry={}",
                    command_name, retries);
                throw XcpException(
                    ErrorCategory::OperationOutcomeUnknown,
                    command_name + " 响应超时，执行结果未知；SYNCH 恢复失败：" +
                        std::string(e.what()),
                    cmd, std::nullopt, retries,
                    std::string(e.TransportError()));
            }
            LIBXCP_LOG_ERROR(
                "XCP operation outcome unknown after recovery cmd={} retry={}",
                command_name, retries);
            throw XcpException(
                ErrorCategory::OperationOutcomeUnknown,
                command_name +
                    " 响应超时，执行结果未知；SYNCH 已恢复，命令未重放",
                cmd, std::nullopt, retries);
        }

        // 其他支持重试的命令超时：进入 SYNCH 恢复并有限重试（计划 §6.2）
        if (retries >= m_timeouts_.max_retries) {
            LIBXCP_LOG_ERROR(
                "XCP command recovery retries exhausted cmd={} retries={}",
                toHex(static_cast<std::uint8_t>(cmd)), retries);
            throw XcpException(ErrorCategory::RecoveryFailed,
                               "命令超时且恢复重试已耗尽（" +
                                   std::to_string(retries) + " 次）",
                               cmd, std::nullopt, retries);
        }
        ++retries;
        LIBXCP_LOG_WARNING("XCP starting recovery cmd={} retry={}",
                           toHex(static_cast<std::uint8_t>(cmd)), retries);
        PerformRecovery(cmd);
        // 可安全重试的 UPLOAD 依赖隐含 MTA，恢复后须重建 MTA（docs L1907）。
        // DOWNLOAD 在上方被归为不可重放命令，不会进入此恢复重试分支。
        if (cmd == CommandCode::Upload) {
            RestoreUploadMta();
        }
        // 标定变更命令在上方统一返回 OperationOutcomeUnknown，不得在此重放。
        // WRITE_DAQ / READ_DAQ 从隐含 DAQ 指针起算：docs L2783 要求恢复后
        // 重新 SET_DAQ_PTR（指针不可查询，只能重放本端记录）
        if (cmd == CommandCode::WriteDaq || cmd == CommandCode::ReadDaq) {
            RestoreDaqPtr();
        }
    }
}

// ---------------------------------------------------------------------------
// 公共命令入口
// ---------------------------------------------------------------------------

ConnectResponse CommandExecutor::ExecuteConnect(std::uint8_t mode) {
    // CONNECT 之前尚无 Session 字节序，按 Intel 编码（reserved/mode
    // 均为单字节， 不受字节序影响），成功后再按协商结果重建 Codec/Parser。
    // AG 此时也未协商，按 Byte 占位；CONNECT 成功后用协商值重建。
    EnsureCodec(ByteOrder::Intel, AddressGranularity::Byte);
    const Bytes encoded = m_codec_->EncodeConnect(mode);

    m_session_.BeginConnecting();
    {
        // 新会话：清空上一会话遗留的 MTA 与 DAQ 指针隐含状态
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_last_mta_.reset();
        m_last_daq_ptr_.reset();
    }
    ParsedPacket response;
    try {
        response = RunCommand(CommandCode::Connect, encoded);
    } catch (...) {
        m_session_.Fail("CONNECT 失败");
        throw;
    }

    const auto& res = std::get<PositiveResponse>(response);
    if (res.data.size() < 2U) {
        m_session_.Fail("CONNECT 响应格式非法");
        throw detail::MakeMalformedPacket(
            "CONNECT 响应过短，无法读取 COMM_MODE_BASIC（RES 数据 " +
            std::to_string(res.data.size()) + " 字节）");
    }
    // COMM_MODE_BASIC 是单字节字段，其位置与字节序无关；先据此确定 Session
    // 字节序， 再用该字节序解析 MAX_DTO 等多字节字段（设计文档 §17.2：MAX_DTO
    // 按 Session Byte Order）。
    const std::uint8_t comm_mode_basic = res.data[1];
    const ByteOrder negotiated_order = ((comm_mode_basic & 0x01U) != 0U)
                                           ? ByteOrder::Motorola
                                           : ByteOrder::Intel;

    ResponseParser connect_parser(negotiated_order);
    auto connect = connect_parser.ParseConnectResponse(BytesView{res.data});
    if (!connect) {
        m_session_.Fail("CONNECT 响应格式非法");
        throw detail::MakeMalformedPacket(
            "CONNECT 响应长度或字段非法（RES 数据 " +
            std::to_string(res.data.size()) + " 字节）");
    }
    try {
        m_session_.EstablishConnection(*connect);
    } catch (const XcpException&) {
        m_session_.Fail("CONNECT 参数校验失败");
        throw;
    }
    // 协商出的字节序立即生效；AG 同样来自 CONNECT 响应
    // （COMM_MODE_BASIC bit1-2），后续 DOWNLOAD 对齐依赖它
    EnsureCodec(connect->byte_order, m_session_.GetAddressGranularity());
    return *connect;
}

void CommandExecutor::ExecuteDisconnect() {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeDisconnect();
    m_session_.BeginDisconnecting();
    try {
        (void)RunCommand(CommandCode::Disconnect, encoded);
        m_session_.CompleteDisconnection();
    } catch (...) {
        // 即使 DISCONNECT 失败也释放本地 Session 状态（计划
        // §5.4），原异常继续上抛
        m_session_.Reset();
        throw;
    }
    // 断连后清空 MTA 隐含状态（计划 §3.1：断连后清空 MTA 与能力）
    {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_last_mta_.reset();
    }
}

GetStatusResponse CommandExecutor::ExecuteGetStatus() {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeGetStatus();
    auto response = RunCommand(CommandCode::GetStatus, encoded);
    const auto& res = std::get<PositiveResponse>(response);
    auto parsed = m_parser_->ParseGetStatusResponse(BytesView{res.data});
    if (!parsed) {
        throw detail::MakeMalformedPacket("GET_STATUS 响应长度不足（RES 数据 " +
                                          std::to_string(res.data.size()) +
                                          " 字节）");
    }
    m_session_.UpdateStatus(*parsed);
    return *parsed;
}

std::optional<GetCommModeInfoResponse>
CommandExecutor::ExecuteGetCommModeInfo() {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeGetCommModeInfo();

    ParsedPacket response;
    try {
        response = RunCommand(CommandCode::GetCommModeInfo, encoded);
    } catch (const XcpException& e) {
        // Slave 不支持该可选命令：记录为不可用并继续连接（计划 §4.4）
        if (e.Category() == ErrorCategory::ProtocolError && e.GetErrorCode() &&
            *e.GetErrorCode() == ErrorCode::CmdUnknown) {
            return std::nullopt;
        }
        throw;
    }

    const auto& res = std::get<PositiveResponse>(response);
    auto parsed = m_parser_->ParseGetCommModeInfoResponse(BytesView{res.data});
    if (!parsed) {
        throw detail::MakeMalformedPacket(
            "GET_COMM_MODE_INFO 响应长度不足（RES 数据 " +
            std::to_string(res.data.size()) + " 字节）");
    }
    m_session_.UpdateCommModeInfo(*parsed);
    return parsed;
}

void CommandExecutor::ExecuteSetMta(AddressExtension extension,
                                    Address address) {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeSetMta(extension, address);
    (void)RunCommand(CommandCode::SetMta, encoded);
    // 记录隐含状态，供 UPLOAD 超时恢复时重建 MTA
    {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_last_mta_ = XcpAddress40{address, extension};
    }
}

Bytes CommandExecutor::ExecuteUpload(ElementCount number_of_elements) {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeUpload(number_of_elements);
    auto response = RunCommand(CommandCode::Upload, encoded);
    auto& res = std::get<PositiveResponse>(response);
    CheckResLength(CommandCode::Upload, res, number_of_elements);
    return std::move(res.data);
}

Bytes CommandExecutor::ExecuteShortUpload(ElementCount number_of_elements,
                                          AddressExtension extension,
                                          Address address) {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded =
        m_codec_->EncodeShortUpload(number_of_elements, extension, address);
    auto response = RunCommand(CommandCode::ShortUpload, encoded);
    auto& res = std::get<PositiveResponse>(response);
    CheckResLength(CommandCode::ShortUpload, res, number_of_elements);
    // F-09：规范要求 SHORT_UPLOAD 成功后把 MTA 设为「上传数据块之后的第一个
    // 数据字节」。须同步本端隐含 MTA，否则此后若发生 UPLOAD 超时恢复，
    // 会按陈旧的 m_last_mta_ 重放 SET_MTA，导致读到的地址与预期错位。
    {
        const auto advanced = XcpAddress40{address, extension}.Advance(
            number_of_elements, m_session_.GetAddressGranularity());
        const std::lock_guard<std::mutex> lock(m_mutex_);
        if (advanced.has_value()) {
            m_last_mta_ = *advanced;
        } else {
            // 地址推进溢出 32 位：无法表达块末位置，作废本端记录而不是
            // 保留陈旧值（陈旧值会让恢复把 MTA 拉回块首，与本命令语义相反）。
            m_last_mta_.reset();
        }
    }
    return std::move(res.data);
}

GetIdResponse CommandExecutor::ExecuteGetId(std::uint8_t identification_type) {
    // GET_ID 在 XCPlite 侧是同步命令、无副作用恢复语义之外的分支：直接复用
    // RunCommand 的 SYNCH 恢复通路。
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeGetId(identification_type);
    auto response = RunCommand(CommandCode::GetId, encoded);
    const auto& res = std::get<PositiveResponse>(response);
    auto parsed = m_parser_->ParseGetId(BytesView{res.data});
    if (!parsed) {
        throw detail::MakeMalformedPacket("GET_ID 响应长度不足（RES 数据 " +
                                          std::to_string(res.data.size()) +
                                          " 字节）");
    }
    return *parsed;
}

CommandExecutor::GetVersionProbe CommandExecutor::ExecuteGetVersion() {
    // 批次 B1：GET_VERSION 是 optional 命令（原始 PDF 第 106 页 §7.4 /
    // 第 114 页 Table 44）。规范明文规定未实现的 optional 命令返回
    // ERR_CMD_UNKNOWN 且不产生副作用，该机制存在的目的就是让 Master 能
    // 低成本探测（P106："This lets the master device detect not implemented
    // optional commands easily."）。
    //
    // 因此本方法把"命令不可用"表达为正常返回值而非异常：三种不可用情形
    // （未实现命令 / 未实现子命令 / 超时）都返回 GetVersionProbe。
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeGetVersion();

    GetVersionProbe probe;
    ParsedPacket response;
    try {
        response = RunCommand(CommandCode::GetVersion, encoded);
    } catch (const XcpException& e) {
        // 超时：不得据此断言对端为老版本，仅表示版本未确认。
        if (e.Category() == ErrorCategory::Timeout) {
            probe.outcome = GetVersionOutcome::TimedOut;
            return probe;
        }
        // 恢复重试耗尽：`RunCommand` 在重试用尽后抛的是 RecoveryFailed 而非
        // Timeout（见本文件 "命令超时且恢复重试已耗尽" 分支），但成因仍是
        // **响应始终未到**。若如实上抛，连接期的自动探测会因一个慢对端直接
        // 让 CONNECT 失败——这正好违反本方法的契约（探测失败必须只降级为
        // "版本未确认"）。因此这部分同样归类为 TimedOut。
        // 注意：仍**不**断言对端版本低，只表达"未确认"。
        if (e.Category() == ErrorCategory::RecoveryFailed) {
            probe.outcome = GetVersionOutcome::TimedOut;
            return probe;
        }
        // ERR_CMD_UNKNOWN：对端不认识 0xC0 族（或其视为未实现的 optional 命令）
        if (e.Category() == ErrorCategory::ProtocolError && e.GetErrorCode() &&
            *e.GetErrorCode() == ErrorCode::CmdUnknown) {
            probe.outcome = GetVersionOutcome::UnimplementedCommand;
            return probe;
        }
        // ERR_SUBCMD_UNKNOWN：对端认识 0xC0 族但不认识 0x00 子命令
        if (e.Category() == ErrorCategory::ProtocolError && e.GetErrorCode() &&
            *e.GetErrorCode() == ErrorCode::SubcmdUnknown) {
            probe.outcome = GetVersionOutcome::UnimplementedSubcommand;
            return probe;
        }
        // 其余（传输故障等）不是"命令不可用"，如实上抛
        throw;
    }

    if (!std::holds_alternative<PositiveResponse>(response)) {
        // 负响应走到这里说明 RunCommand 未按预期转换；按畸形响应处理，
        // 不伪装成"不可用"以免掩盖真实协议故障。
        throw detail::MakeMalformedPacket(
            "GET_VERSION 返回了非正响应且未携带可识别错误码");
    }
    const auto& res = std::get<PositiveResponse>(response);
    auto parsed = m_parser_->ParseGetVersionResponse(BytesView{res.data});
    if (!parsed) {
        throw detail::MakeMalformedPacket(
            "GET_VERSION 响应长度不足（RES 数据 " +
            std::to_string(res.data.size()) +
            " 字节，原始 PDF 第 114 页 Table 45 要求 6 字节含 0xFF）");
    }
    probe.outcome = GetVersionOutcome::Available;
    probe.response = *parsed;
    return probe;
}

GetSeedResponse CommandExecutor::ExecuteGetSeed(Resource resource,
                                                SeedMode mode) {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeGetSeed(resource, mode);
    // F-10：标记段位置。Mode=Remainder 属多段序列的中间段，其超时不可由
    // 通用路径重放（Slave 侧段偏移不确定）。Mode=First 开启事务，本身可
    // 安全重启，不进入分段恢复分支。
    SetSeedKeySegment(mode == SeedMode::Remainder
                          ? SeedKeySegment::NonFinal
                          : SeedKeySegment::NotSegmented);
    auto response = RunCommand(CommandCode::GetSeed, encoded);
    SetSeedKeySegment(SeedKeySegment::NotSegmented);
    const auto& res = std::get<PositiveResponse>(response);
    auto parsed = m_parser_->ParseGetSeedResponse(BytesView{res.data});
    if (!parsed) {
        throw detail::MakeMalformedPacket("GET_SEED 响应长度不足（RES 数据 " +
                                          std::to_string(res.data.size()) +
                                          " 字节）");
    }
    return *parsed;
}

UnlockResponse CommandExecutor::ExecuteUnlock(std::uint8_t length_field,
                                              BytesView key_segment,
                                              bool is_final_segment) {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeUnlock(length_field, key_segment);
    // F-10：标记段位置。末段属结果未知（资源可能已解锁），超时后禁止自动
    // 重放；非末段则须先断开重连再从 GET_SEED 完整重启。
    SetSeedKeySegment(is_final_segment ? SeedKeySegment::Final
                                       : SeedKeySegment::NonFinal);

    ParsedPacket response;
    try {
        response = RunCommand(CommandCode::Unlock, encoded);
    } catch (const XcpException& e) {
        SetSeedKeySegment(SeedKeySegment::NotSegmented);
        // Key 校验失败：Slave 已主动断开会话（规范 §7.5.1.9），本地会话
        // 不可继续，标记 Failed（XcpMaster::Connect() 会自动 Reset 重连）
        if (e.Category() == ErrorCategory::ProtocolError && e.GetErrorCode() &&
            *e.GetErrorCode() == ErrorCode::AccessLocked) {
            m_session_.Fail("UNLOCK Key 校验失败，Slave 已主动断开会话");
        }
        throw;
    }
    SetSeedKeySegment(SeedKeySegment::NotSegmented);

    const auto& res = std::get<PositiveResponse>(response);
    auto parsed = m_parser_->ParseUnlockResponse(BytesView{res.data});
    if (!parsed) {
        throw detail::MakeMalformedPacket("UNLOCK 响应长度不足（RES 数据 " +
                                          std::to_string(res.data.size()) +
                                          " 字节）");
    }
    return *parsed;
}

// ---------------------------------------------------------------------------
// 批次14：DAQ 命令组与写回（T14-05）
// ---------------------------------------------------------------------------

void CommandExecutor::RestoreDaqPtr() {
    std::optional<DaqPointer> ptr;
    {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        ptr = m_last_daq_ptr_;
    }
    if (!ptr) {
        return;  // 未记录或被 CLEAR_DAQ_LIST 作废：无可重放的隐含状态
    }
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeSetDaqPtr(
        ptr->daq_list, ptr->odt_number, ptr->odt_entry);
    const auto response = PerformAttempt(CommandCode::SetDaqPtr, encoded);
    if (!response) {
        // SET_DAQ_PTR 自身也失败：不掩盖，交由随后的 WRITE/READ_DAQ 重试
        // 把错误暴露出来（与 RestoreUploadMta 同一处理口径）
        return;
    }
    (void)DispatchResponse(CommandCode::SetDaqPtr, *response);
}

void CommandExecutor::ExecuteSetDaqPtr(std::uint16_t daq_list,
                                       std::uint8_t odt_number,
                                       std::uint8_t odt_entry_number) {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded =
        m_codec_->EncodeSetDaqPtr(daq_list, odt_number, odt_entry_number);
    (void)RunCommand(CommandCode::SetDaqPtr, encoded);
    // 成功后记录隐含指针，供 WRITE_DAQ / READ_DAQ 超时恢复重放（docs L2783）
    {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_last_daq_ptr_ = DaqPointer{daq_list, odt_number, odt_entry_number};
    }
}

void CommandExecutor::ExecuteWriteDaq(std::uint8_t bit_offset,
                                      std::uint8_t size,
                                      AddressExtension extension,
                                      Address address) {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded =
        m_codec_->EncodeWriteDaq(bit_offset, size, extension, address);
    (void)RunCommand(CommandCode::WriteDaq, encoded);
    // Slave 侧指针自增，本端**不**跟进：编排层对每个 Entry 都显式
    // SET_DAQ_PTR，避免"写过 ODT 末位后指针未定义"（docs L2172）
}

void CommandExecutor::ExecuteClearDaqList(std::uint16_t daq_list) {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeClearDaqList(daq_list);
    (void)RunCommand(CommandCode::ClearDaqList, encoded);
    // CLEAR 后该列表的 Entry 全部复位，隐含指针语义不再成立 → 作废记录，
    // 强制调用方重新 SET_DAQ_PTR 才能继续配置
    {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        if (m_last_daq_ptr_ && m_last_daq_ptr_->daq_list == daq_list) {
            m_last_daq_ptr_.reset();
        }
    }
}

void CommandExecutor::ExecuteFreeDaq() {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeFreeDaq();
    (void)RunCommand(CommandCode::FreeDaq, encoded);
    // 全表作废：隐含指针记录一律失效（口径与 ExecuteClearDaqList 一致，
    // 只是作用域是全部 List）
    {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_last_daq_ptr_.reset();
    }
}

void CommandExecutor::ExecuteAllocDaq(std::uint16_t count) {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeAllocDaq(count);
    (void)RunCommand(CommandCode::AllocDaq, encoded);
    // 全表已被 FREE_DAQ 清空后才会走到这里（编排层时序，D9）；防御性地
    // 再作废一次隐含指针，保证本方法单独调用也留干净状态
    {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_last_daq_ptr_.reset();
    }
}

void CommandExecutor::ExecuteAllocOdt(std::uint16_t daq_list,
                                      std::uint8_t count) {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeAllocOdt(daq_list, count);
    (void)RunCommand(CommandCode::AllocOdt, encoded);
}

void CommandExecutor::ExecuteAllocOdtEntry(std::uint16_t daq_list,
                                           std::uint8_t odt_number,
                                           std::uint8_t count) {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded =
        m_codec_->EncodeAllocOdtEntry(daq_list, odt_number, count);
    (void)RunCommand(CommandCode::AllocOdtEntry, encoded);
}

void CommandExecutor::ExecuteSetDaqListMode(DaqListModeBit mode,
                                            std::uint16_t daq_list,
                                            std::uint16_t event_channel,
                                            std::uint8_t prescaler,
                                            std::uint8_t priority) {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeSetDaqListMode(
        mode, daq_list, event_channel, prescaler, priority);
    (void)RunCommand(CommandCode::SetDaqListMode, encoded);
}

StartStopDaqListResponse CommandExecutor::ExecuteStartStopDaqList(
    DaqListAction action, std::uint16_t daq_list) {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeStartStopDaqList(action, daq_list);
    auto response = RunCommand(CommandCode::StartStopDaqList, encoded);
    const auto& res = std::get<PositiveResponse>(response);
    auto parsed = m_parser_->ParseStartStopDaqListResponse(BytesView{res.data});
    if (!parsed) {
        throw detail::MakeMalformedPacket(
            "START_STOP_DAQ_LIST 响应长度不足（RES 数据 " +
            std::to_string(res.data.size()) + " 字节，期望 >= 1）");
    }
    return *parsed;
}

void CommandExecutor::ExecuteStartStopSynch(DaqSynchAction action) {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeStartStopSynch(action);
    (void)RunCommand(CommandCode::StartStopSynch, encoded);
}

std::optional<GetDaqListInfoResponse> CommandExecutor::ExecuteGetDaqListInfo(
    std::uint16_t daq_list) {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeGetDaqListInfo(daq_list);
    ParsedPacket response;
    try {
        response = RunCommand(CommandCode::GetDaqListInfo, encoded);
    } catch (const XcpException& e) {
        // Optional 命令未实现 → ERR_CMD_UNKNOWN 且无副作用（docs L1631）：
        // 视为"能力缺失"，由调用方回落 A2L 侧数据，不当作链路故障
        if (IsCmdUnknown(e)) {
            return std::nullopt;
        }
        throw;
    }
    const auto& res = std::get<PositiveResponse>(response);
    auto parsed = m_parser_->ParseGetDaqListInfo(BytesView{res.data});
    if (!parsed) {
        throw detail::MakeMalformedPacket(
            "GET_DAQ_LIST_INFO 响应长度不足（RES 数据 " +
            std::to_string(res.data.size()) + " 字节，期望 >= 5）");
    }
    return parsed;
}

std::optional<GetDaqEventInfoResponse> CommandExecutor::ExecuteGetDaqEventInfo(
    std::uint16_t event_channel) {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeGetDaqEventInfo(event_channel);
    ParsedPacket response;
    try {
        response = RunCommand(CommandCode::GetDaqEventInfo, encoded);
    } catch (const XcpException& e) {
        // Optional 命令未实现 → ERR_CMD_UNKNOWN 且无副作用（docs L1631）：
        // 视为"能力缺失"，由调用方回落，不当作链路故障
        if (IsCmdUnknown(e)) {
            return std::nullopt;
        }
        throw;
    }
    const auto& res = std::get<PositiveResponse>(response);
    auto parsed = m_parser_->ParseGetDaqEventInfo(BytesView{res.data});
    if (!parsed) {
        throw detail::MakeMalformedPacket(
            "GET_DAQ_EVENT_INFO 响应长度不足（RES 数据 " +
            std::to_string(res.data.size()) + " 字节，期望 >= 6）");
    }
    return parsed;
}

std::optional<GetDaqProcessorInfoResponse>
CommandExecutor::ExecuteGetDaqProcessorInfo() {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeGetDaqProcessorInfo();
    ParsedPacket response;
    try {
        response = RunCommand(CommandCode::GetDaqProcessorInfo, encoded);
    } catch (const XcpException& e) {
        if (IsCmdUnknown(e)) {
            return std::nullopt;
        }
        throw;
    }
    const auto& res = std::get<PositiveResponse>(response);
    auto parsed = m_parser_->ParseGetDaqProcessorInfo(BytesView{res.data});
    if (!parsed) {
        throw detail::MakeMalformedPacket(
            "GET_DAQ_PROCESSOR_INFO 响应长度不足（RES 数据 " +
            std::to_string(res.data.size()) + " 字节，期望 >= 7）");
    }
    return parsed;
}

std::optional<GetDaqResolutionInfoResponse>
CommandExecutor::ExecuteGetDaqResolutionInfo() {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeGetDaqResolutionInfo();
    ParsedPacket response;
    try {
        response = RunCommand(CommandCode::GetDaqResolutionInfo, encoded);
    } catch (const XcpException& e) {
        if (IsCmdUnknown(e)) {
            return std::nullopt;
        }
        throw;
    }
    const auto& res = std::get<PositiveResponse>(response);
    auto parsed = m_parser_->ParseGetDaqResolutionInfo(BytesView{res.data});
    if (!parsed) {
        throw detail::MakeMalformedPacket(
            "GET_DAQ_RESOLUTION_INFO 响应长度不足（RES 数据 " +
            std::to_string(res.data.size()) + " 字节，期望 >= 7）");
    }
    return parsed;
}

std::optional<ReadDaqResponse> CommandExecutor::ExecuteReadDaq() {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeReadDaq();
    ParsedPacket response;
    try {
        response = RunCommand(CommandCode::ReadDaq, encoded);
    } catch (const XcpException& e) {
        if (IsCmdUnknown(e)) {
            return std::nullopt;
        }
        throw;
    }
    const auto& res = std::get<PositiveResponse>(response);
    auto parsed = m_parser_->ParseReadDaq(BytesView{res.data});
    if (!parsed) {
        throw detail::MakeMalformedPacket("READ_DAQ 响应长度不足（RES 数据 " +
                                          std::to_string(res.data.size()) +
                                          " 字节，期望 >= 7）");
    }
    return parsed;
}

void CommandExecutor::ExecuteDownload(ElementCount number_of_elements,
                                      BytesView data) {
    // 显式传 AG：EncodeDownload 在 AG=DWORD 时须插 2 字节对齐
    // （原始 PDF 第 142 页 Table 82）
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeDownload(number_of_elements, data);
    (void)RunCommand(CommandCode::Download, encoded);
    // Slave 写完自动前移 MTA（docs L1983）：本端 m_last_mta_ 记录的仍是
    // 本块起始地址，超时恢复重放 SET_MTA 恰好把 MTA 拉回块首后重发本块，
    // 幂等且不会错位（MemoryAccess 每块都显式 SET_MTA，不依赖自增）。
}

void CommandExecutor::ExecuteShortDownload(ElementCount number_of_elements,
                                           AddressExtension extension,
                                           Address address, BytesView data) {
    // SHORT_DOWNLOAD 头固定 8 字节，数据自然 4 字节对齐，不需按 AG 插对齐
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeShortDownload(
        number_of_elements, extension, address, data);
    // 写回必须有确认：ERR_CMD_UNKNOWN 不降级为成功，原样上抛由
    // MemoryAccess 回落 SET_MTA + DOWNLOAD 通路
    (void)RunCommand(CommandCode::ShortDownload, encoded);
}

// ---------------------------------------------------------------------------
// 变量标定批次：MODIFY_BITS + Page Switching（docs §7.5.2.5 / §7.5.3）
// ---------------------------------------------------------------------------

void CommandExecutor::ExecuteModifyBits(std::uint8_t shift,
                                        std::uint16_t and_mask,
                                        std::uint16_t xor_mask) {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded =
        m_codec_->EncodeModifyBits(shift, and_mask, xor_mask);
    // CRM 仅 RES（xcp.h CRM_MODIFY_BITS_LEN=1），无数据字段可解析。
    // MTA 不变；响应超时后的执行结果由 RunCommand 标记为未知且不重放。
    (void)RunCommand(CommandCode::ModifyBits, encoded);
}

void CommandExecutor::ExecuteSetCalPage(CalPageModeBit mode,
                                       std::uint8_t segment,
                                       std::uint8_t page) {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeSetCalPage(mode, segment, page);
    (void)RunCommand(CommandCode::SetCalPage, encoded);
}

std::optional<GetCalPageResponse> CommandExecutor::ExecuteGetCalPage(
    CalPageAccessMode access_mode, std::uint8_t segment) {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeGetCalPage(access_mode, segment);
    ParsedPacket response;
    try {
        response = RunCommand(CommandCode::GetCalPage, encoded);
    } catch (const XcpException& e) {
        if (IsCmdUnknown(e)) {
            return std::nullopt;  // Optional 命令，Slave 不支持 Paging
        }
        throw;
    }
    const auto& res = std::get<PositiveResponse>(response);
    auto parsed = m_parser_->ParseGetCalPage(BytesView{res.data});
    if (!parsed) {
        throw detail::MakeMalformedPacket(
            "GET_CAL_PAGE 响应长度不足（RES 数据 " +
            std::to_string(res.data.size()) + " 字节，期望 >= 3）");
    }
    return parsed;
}

std::optional<GetPagProcessorInfoResponse>
CommandExecutor::ExecuteGetPagProcessorInfo() {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeGetPagProcessorInfo();
    ParsedPacket response;
    try {
        response = RunCommand(CommandCode::GetPagProcessorInfo, encoded);
    } catch (const XcpException& e) {
        if (IsCmdUnknown(e)) {
            return std::nullopt;  // Slave 不支持 Page Switching
        }
        throw;
    }
    const auto& res = std::get<PositiveResponse>(response);
    auto parsed = m_parser_->ParseGetPagProcessorInfo(BytesView{res.data});
    if (!parsed) {
        throw detail::MakeMalformedPacket(
            "GET_PAG_PROCESSOR_INFO 响应长度不足（RES 数据 " +
            std::to_string(res.data.size()) + " 字节，期望 >= 2）");
    }
    return parsed;
}

std::optional<GetSegmentInfoResponse> CommandExecutor::ExecuteGetSegmentInfo(
    SegmentInfoMode mode, std::uint8_t segment, SegmentInfoSelector info,
    std::uint8_t mapping_index) {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded =
        m_codec_->EncodeGetSegmentInfo(mode, segment, info, mapping_index);
    ParsedPacket response;
    try {
        response = RunCommand(CommandCode::GetSegmentInfo, encoded);
    } catch (const XcpException& e) {
        if (IsCmdUnknown(e)) {
            return std::nullopt;
        }
        throw;
    }
    const auto& res = std::get<PositiveResponse>(response);
    // 变长响应：把请求 Mode 传给 Parser 决定布局（计划 §11 风险 1）
    auto parsed = m_parser_->ParseGetSegmentInfo(BytesView{res.data}, mode);
    if (!parsed) {
        throw detail::MakeMalformedPacket(
            "GET_SEGMENT_INFO 响应长度不足（RES 数据 " +
            std::to_string(res.data.size()) +
            " 字节，Mode 0/2 期望 >= 4、Mode 1 期望 >= 5）");
    }
    return parsed;
}

std::optional<GetPageInfoResponse> CommandExecutor::ExecuteGetPageInfo(
    std::uint8_t segment, std::uint8_t page) {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeGetPageInfo(segment, page);
    ParsedPacket response;
    try {
        response = RunCommand(CommandCode::GetPageInfo, encoded);
    } catch (const XcpException& e) {
        if (IsCmdUnknown(e)) {
            return std::nullopt;
        }
        throw;
    }
    const auto& res = std::get<PositiveResponse>(response);
    auto parsed = m_parser_->ParseGetPageInfo(BytesView{res.data});
    if (!parsed) {
        throw detail::MakeMalformedPacket(
            "GET_PAGE_INFO 响应长度不足（RES 数据 " +
            std::to_string(res.data.size()) + " 字节，期望 >= 2）");
    }
    return parsed;
}

void CommandExecutor::ExecuteSetSegmentMode(SegmentModeBit mode,
                                           std::uint8_t segment) {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeSetSegmentMode(mode, segment);
    (void)RunCommand(CommandCode::SetSegmentMode, encoded);
}

std::optional<GetSegmentModeResponse> CommandExecutor::ExecuteGetSegmentMode(
    std::uint8_t segment) {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeGetSegmentMode(segment);
    ParsedPacket response;
    try {
        response = RunCommand(CommandCode::GetSegmentMode, encoded);
    } catch (const XcpException& e) {
        if (IsCmdUnknown(e)) {
            return std::nullopt;
        }
        throw;
    }
    const auto& res = std::get<PositiveResponse>(response);
    auto parsed = m_parser_->ParseGetSegmentMode(BytesView{res.data});
    if (!parsed) {
        throw detail::MakeMalformedPacket(
            "GET_SEGMENT_MODE 响应长度不足（RES 数据 " +
            std::to_string(res.data.size()) + " 字节，期望 >= 2）");
    }
    return parsed;
}

void CommandExecutor::ExecuteCopyCalPage(const CopyCalPageRequest& request) {
    EnsureCodec(m_session_.GetByteOrder(), m_session_.GetAddressGranularity());
    const Bytes encoded = m_codec_->EncodeCopyCalPage(request);
    (void)RunCommand(CommandCode::CopyCalPage, encoded);
}

}  // namespace calmcar::xcp
