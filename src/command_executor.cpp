/**
 * @file command_executor.cpp
 * @brief CommandExecutor 实现：命令调度、响应分派与 SYNCH 恢复。
 *
 * 错误策略依据 code-plan/XCP_1.3.0_最小协议核心实现计划.md 第 6.2~6.4 节表格。
 */

#include "libxcp/command_executor.hpp"

#include <string>
#include <utility>

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

/// @brief 构造带命令上下文的 ProtocolError 消息
std::string protocolMessage(std::string_view prefix, CommandCode cmd,
                            const NegativeResponse& err) {
    std::string msg(std::string(prefix) + " 命令 ");
    msg += toHex(static_cast<std::uint8_t>(cmd));
    msg += " 收到 ERR ";
    // 已识别的错误码用规范名称；未知码保留原始字节的十六进制表示（计划 §6.4）
    msg += err.m_error_code_ ? std::string(ErrorCodeName(*err.m_error_code_))
                             : toHex(err.m_raw_error_code_);
    if (!err.m_additional_info_.empty()) {
        msg += "（附加信息 " + std::to_string(err.m_additional_info_.size()) +
               " 字节）";
    }
    return msg;
}

}  // namespace

CommandExecutor::CommandExecutor(IXcpTransport& transport, Session& session,
                                 CommandTimeouts timeouts,
                                 IEventListener* event_listener)
    : m_transport_(transport),
      m_session_(session),
      m_timeouts_(timeouts),
      m_event_listener_(event_listener) {}

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

void CommandExecutor::EnsureCodec(ByteOrder byte_order) {
    if (!m_codec_ || m_codec_->GetByteOrder() != byte_order) {
        m_codec_ = std::make_unique<CommandCodec>(byte_order);
    }
    if (!m_parser_ || m_parser_->GetByteOrder() != byte_order) {
        m_parser_ = std::make_unique<ResponseParser>(byte_order);
    }
}

// ---------------------------------------------------------------------------
// IPacketListener：在 Transport 工作线程执行
// ---------------------------------------------------------------------------

void CommandExecutor::OnPacketReceived(BytesView packet) {
    // 解析必须在锁外完成：parser 无共享状态，且避免长时间持有 m_mutex_。
    // expectedCommand 仅在 PositiveResponse 中作上下文标记；响应匹配依赖
    // “单 Pending Command + RES/ERR PID”，不使用 CTR 匹配（计划 §4.7）。
    const auto expected =
        m_session_.PendingCommand().value_or(CommandCode::GetStatus);
    if (!m_parser_) {
        return;  // CONNECT 之前尚无 Parser：此时不应有任何 Slave 流量
    }
    const auto parsed = m_parser_->Parse(packet, expected);
    if (!parsed) {
        return;  // 空包/畸形包：报告但不终止会话
    }

    bool notify_response = false;
    bool notify_synch = false;
    std::optional<EventPacket> async_event;
    std::optional<ServicePacket> async_service;
    std::optional<DtoPacket> async_dto;
    bool notify_malformed = false;

    {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        if (m_transport_closed_) {
            return;  // 关闭后到达的迟到包忽略
        }

        if (const auto* res = std::get_if<PositiveResponse>(&*parsed)) {
            // 响应槽位已被占用说明协议失步：丢弃并上报畸形，绝不覆盖前一条响应
            if (m_response_ready_) {
                notify_malformed = true;
            } else {
                m_pending_response_ = *res;
                m_response_ready_ = true;
                notify_response = true;
            }
        } else if (const auto* err = std::get_if<NegativeResponse>(&*parsed)) {
            if (m_in_recovery_ && err->m_error_code_ &&
                *err->m_error_code_ == ErrorCode::CmdSynch) {
                // ERR_CMD_SYNCH 仅在 Recovery 中视为成功确认（计划 §6.2 第 1
                // 条）
                m_synch_confirmed_ = true;
                notify_synch = true;
            } else if (m_response_ready_) {
                notify_malformed = true;
            } else {
                m_pending_response_ = *err;
                m_response_ready_ = true;
                notify_response = true;
            }
        } else if (const auto* ev = std::get_if<EventPacket>(&*parsed)) {
            if (ev->m_event_code_ &&
                *ev->m_event_code_ == EventCode::CmdPending) {
                // EV_CMD_PENDING：重启当前命令 Timer，不重发原命令（计划
                // §6.3）。
                // 只置标记并唤醒等待循环，不占用响应槽位；事件本身仍需上报。
                m_cmd_pending_received_ = true;
                notify_response = true;
            }
            async_event = *ev;
        } else if (const auto* serv = std::get_if<ServicePacket>(&*parsed)) {
            async_service = *serv;
        } else if (const auto* dto = std::get_if<DtoPacket>(&*parsed)) {
            async_dto = *dto;
        }
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
    {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_transport_closed_ = true;
        m_transport_close_reason_ = std::string(reason);
    }
    m_response_cv_.notify_all();
    m_synch_cv_.notify_all();
}

void CommandExecutor::OnTransportWarning(std::string_view /*message*/) {
    // Transport 层的可恢复诊断（畸形 Datagram、CTR 缺口等）不影响命令事务，
    // 本阶段不上抛；后续批次可按需接入日志设施。
}

// ---------------------------------------------------------------------------
// 等待与分派
// ---------------------------------------------------------------------------

std::optional<ParsedPacket> CommandExecutor::WaitForResponse(
    std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(m_mutex_);
    auto deadline = std::chrono::steady_clock::now() + timeout;

    while (true) {
        if (m_response_ready_ && m_pending_response_) {
            auto out = *m_pending_response_;
            m_pending_response_.reset();
            m_response_ready_ = false;
            m_cmd_pending_received_ = false;
            return out;
        }
        if (m_transport_closed_) {
            m_cmd_pending_received_ = false;
            return std::nullopt;
        }

        // 期间收到过 EV_CMD_PENDING：重启本轮 Timer，不重发原命令（计划 §6.3）
        if (m_cmd_pending_received_) {
            m_cmd_pending_received_ = false;
            deadline = std::chrono::steady_clock::now() + timeout;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            m_cmd_pending_received_ = false;
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
        // 未知错误码：保留原值并上报 ProtocolError（计划 §6.4）；不使用 Generic
        // 顶替， 以免让上层误判为已识别的标准错误码。
        const std::string message =
            protocolMessage("Slave 返回负响应", cmd, *err);
        if (!err->m_error_code_) {
            throw XcpException(ErrorCategory::ProtocolError,
                               message + "（未知错误码 " +
                                   toHex(err->m_raw_error_code_) + "）",
                               cmd, std::nullopt);
        }
        const ErrorCode code = *err->m_error_code_;

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
        // ERR_ACCESS_LOCKED：需要 Seed&Key，本阶段不支持（计划 §6.4）
        if (code == ErrorCode::AccessLocked) {
            throw XcpException(
                ErrorCategory::UnsupportedFeature,
                protocolMessage("资源被 Seed&Key 保护，本阶段不支持解锁", cmd,
                                *err),
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

void CommandExecutor::CheckResLength(CommandCode cmd,
                                     const PositiveResponse& res,
                                     ElementCount elements) {
    const auto ag = m_session_.GetAddressGranularity();
    const auto expected = static_cast<std::size_t>(elements) * AgToBytes(ag);
    if (res.m_data_.size() != expected) {
        throw XcpException(
            ErrorCategory::MalformedPacket,
            "命令 " + std::to_string(static_cast<int>(cmd)) + " 响应数据长度 " +
                std::to_string(res.m_data_.size()) +
                " 不等于期望的 elements*AG = " + std::to_string(expected),
            cmd);
    }
}

// ---------------------------------------------------------------------------
// 单条命令流程（含超时恢复与有限重试）
// ---------------------------------------------------------------------------

std::optional<ParsedPacket> CommandExecutor::PerformAttempt(
    CommandCode cmd, BytesView encoded_packet) {
    m_session_.MarkCommandSent(cmd);
    try {
        m_transport_.Send(encoded_packet);
    } catch (...) {
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
        return;  // 本 Session 未设置过 MTA，无可恢复的隐含状态
    }

    EnsureCodec(m_session_.GetByteOrder());
    const Bytes encoded =
        m_codec_->EncodeSetMta(mta->m_extension_, mta->m_address_);
    const auto response = PerformAttempt(CommandCode::SetMta, encoded);
    if (!response) {
        // SET_MTA 自身也失败：不再掩盖，交由随后的 UPLOAD 重试把错误暴露出来
        return;
    }
    (void)DispatchResponse(CommandCode::SetMta, *response);
}

void CommandExecutor::PerformRecovery(CommandCode cmd) {
    // Transport 已断开时不执行 SYNCH（计划 §6.2 第 5 条）
    if (!m_transport_.IsOpen()) {
        std::string reason;
        {
            const std::lock_guard<std::mutex> lock(m_mutex_);
            reason = m_transport_close_reason_;
        }
        throw XcpException(ErrorCategory::RecoveryFailed,
                           "Transport 已关闭，无法执行 SYNCH 恢复", cmd,
                           std::nullopt, 0, reason);
    }
    if (!SendSynch()) {
        throw XcpException(ErrorCategory::RecoveryFailed,
                           "SYNCH 未得到 ERR_CMD_SYNCH 确认", cmd);
    }
}

bool CommandExecutor::SendSynch() {
    EnsureCodec(m_session_.GetByteOrder());
    const Bytes synch = m_codec_->EncodeSynch();

    {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_synch_confirmed_ = false;
        m_in_recovery_ = true;
        m_pending_response_.reset();
        m_response_ready_ = false;
    }

    m_session_.BeginRecovery();
    bool confirmed = false;
    try {
        m_session_.MarkCommandSent(CommandCode::Synch);
        m_transport_.Send(synch);

        std::unique_lock<std::mutex> lock(m_mutex_);
        const auto deadline =
            std::chrono::steady_clock::now() + m_timeouts_.synch_timeout;
        while (!m_synch_confirmed_ && !m_transport_closed_) {
            if (m_synch_cv_.wait_until(lock, deadline) ==
                std::cv_status::timeout) {
                break;
            }
        }
        confirmed = m_synch_confirmed_;
    } catch (const XcpException&) {
        confirmed = false;
    }

    {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_in_recovery_ = false;
    }
    m_session_.ClearPendingCommand();

    if (confirmed) {
        m_session_.CompleteRecovery();
    } else {
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

        if (response) {
            // dispatchResponse 可能抛
            // ProtocolError/UnsupportedFeature，交由上层处理
            return DispatchResponse(cmd, *response);
        }

        // 超时或 Transport 关闭：进入 SYNCH 恢复并有限重试（计划 §6.2）
        if (retries >= m_timeouts_.max_retries) {
            throw XcpException(ErrorCategory::RecoveryFailed,
                               "命令超时且恢复重试已耗尽（" +
                                   std::to_string(retries) + " 次）",
                               cmd, std::nullopt, retries);
        }
        ++retries;
        PerformRecovery(cmd);
        // UPLOAD 依赖隐含的 MTA 状态：重试前必须重新建立（计划 §6.2 第 3 条）
        if (cmd == CommandCode::Upload) {
            RestoreUploadMta();
        }
    }
}

// ---------------------------------------------------------------------------
// 公共命令入口
// ---------------------------------------------------------------------------

ConnectResponse CommandExecutor::ExecuteConnect(std::uint8_t mode) {
    // CONNECT 之前尚无 Session 字节序，按 Intel 编码（reserved/mode
    // 均为单字节， 不受字节序影响），成功后再按协商结果重建 Codec/Parser。
    EnsureCodec(ByteOrder::Intel);
    const Bytes encoded = m_codec_->EncodeConnect(mode);

    m_session_.BeginConnecting();
    {
        // 新会话：清空上一会话遗留的 MTA 隐含状态
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_last_mta_.reset();
    }
    ParsedPacket response;
    try {
        response = RunCommand(CommandCode::Connect, encoded);
    } catch (...) {
        m_session_.Fail("CONNECT 失败");
        throw;
    }

    const auto& res = std::get<PositiveResponse>(response);
    if (res.m_data_.size() < 2U) {
        m_session_.Fail("CONNECT 响应格式非法");
        throw detail::MakeMalformedPacket(
            "CONNECT 响应过短，无法读取 COMM_MODE_BASIC（RES 数据 " +
            std::to_string(res.m_data_.size()) + " 字节）");
    }
    // COMM_MODE_BASIC 是单字节字段，其位置与字节序无关；先据此确定 Session
    // 字节序， 再用该字节序解析 MAX_DTO 等多字节字段（设计文档 §17.2：MAX_DTO
    // 按 Session Byte Order）。
    const std::uint8_t comm_mode_basic = res.m_data_[1];
    const ByteOrder negotiated_order = ((comm_mode_basic & 0x01U) != 0U)
                                           ? ByteOrder::Motorola
                                           : ByteOrder::Intel;

    ResponseParser connect_parser(negotiated_order);
    auto connect = connect_parser.ParseConnectResponse(BytesView{res.m_data_});
    if (!connect) {
        m_session_.Fail("CONNECT 响应格式非法");
        throw detail::MakeMalformedPacket(
            "CONNECT 响应长度或字段非法（RES 数据 " +
            std::to_string(res.m_data_.size()) + " 字节）");
    }
    try {
        m_session_.EstablishConnection(*connect);
    } catch (const XcpException&) {
        m_session_.Fail("CONNECT 参数校验失败");
        throw;
    }
    // 协商出的字节序立即生效
    EnsureCodec(connect->m_byte_order_);
    return *connect;
}

void CommandExecutor::ExecuteDisconnect() {
    EnsureCodec(m_session_.GetByteOrder());
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
    EnsureCodec(m_session_.GetByteOrder());
    const Bytes encoded = m_codec_->EncodeGetStatus();
    auto response = RunCommand(CommandCode::GetStatus, encoded);
    const auto& res = std::get<PositiveResponse>(response);
    auto parsed = m_parser_->ParseGetStatusResponse(BytesView{res.m_data_});
    if (!parsed) {
        throw detail::MakeMalformedPacket("GET_STATUS 响应长度不足（RES 数据 " +
                                          std::to_string(res.m_data_.size()) +
                                          " 字节）");
    }
    m_session_.UpdateStatus(*parsed);
    return *parsed;
}

std::optional<GetCommModeInfoResponse>
CommandExecutor::ExecuteGetCommModeInfo() {
    EnsureCodec(m_session_.GetByteOrder());
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
    auto parsed =
        m_parser_->ParseGetCommModeInfoResponse(BytesView{res.m_data_});
    if (!parsed) {
        throw detail::MakeMalformedPacket(
            "GET_COMM_MODE_INFO 响应长度不足（RES 数据 " +
            std::to_string(res.m_data_.size()) + " 字节）");
    }
    m_session_.UpdateCommModeInfo(*parsed);
    return parsed;
}

void CommandExecutor::ExecuteSetMta(AddressExtension extension,
                                    Address address) {
    EnsureCodec(m_session_.GetByteOrder());
    const Bytes encoded = m_codec_->EncodeSetMta(extension, address);
    (void)RunCommand(CommandCode::SetMta, encoded);
    // 记录隐含状态，供 UPLOAD 超时恢复时重建 MTA
    {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_last_mta_ = XcpAddress40{address, extension};
    }
}

Bytes CommandExecutor::ExecuteUpload(ElementCount number_of_elements) {
    EnsureCodec(m_session_.GetByteOrder());
    const Bytes encoded = m_codec_->EncodeUpload(number_of_elements);
    auto response = RunCommand(CommandCode::Upload, encoded);
    auto& res = std::get<PositiveResponse>(response);
    CheckResLength(CommandCode::Upload, res, number_of_elements);
    return std::move(res.m_data_);
}

Bytes CommandExecutor::ExecuteShortUpload(ElementCount number_of_elements,
                                          AddressExtension extension,
                                          Address address) {
    EnsureCodec(m_session_.GetByteOrder());
    const Bytes encoded =
        m_codec_->EncodeShortUpload(number_of_elements, extension, address);
    auto response = RunCommand(CommandCode::ShortUpload, encoded);
    auto& res = std::get<PositiveResponse>(response);
    CheckResLength(CommandCode::ShortUpload, res, number_of_elements);
    return std::move(res.m_data_);
}

}  // namespace calmcar::xcp
