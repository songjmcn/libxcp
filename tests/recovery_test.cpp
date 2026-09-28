/**
 * @file recovery_test.cpp
 * @brief CommandExecutor 的超时恢复、EV_CMD_PENDING 与错误分派测试。
 *
 * 覆盖计划文档 §9.4（Session 与恢复）与 §9.5（Mock 集成）第 4~7 条。
 */

#include "libxcp/command_executor.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "libxcp/session.hpp"
#include "mock_transport.hpp"

namespace calmcar::xcp {
namespace {

Bytes BytesOf(std::initializer_list<std::uint8_t> init) {
    return Bytes(init.begin(), init.end());
}

/// @brief 作用域退出时确保线程被 join（避免异常路径下 joinable 线程析构触发
/// terminate）
class ThreadJoiner {
public:
    explicit ThreadJoiner(std::thread& thread) : m_thread_(thread) {}
    ~ThreadJoiner() {
        if (m_thread_.joinable()) {
            m_thread_.join();
        }
    }
    ThreadJoiner(const ThreadJoiner&) = delete;
    ThreadJoiner& operator=(const ThreadJoiner&) = delete;

private:
    std::thread& m_thread_;
};

/// @brief 记录 IEventListener 回调，供断言使用
class RecordingEvents : public IEventListener {
public:
    void OnEvent(const EventPacket& event) override {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_events_.push_back(event);
    }
    void OnService(const ServicePacket& service) override {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_services_.push_back(service);
    }
    void OnDto(const DtoPacket& dto) override {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_dtos_.push_back(dto);
    }

    [[nodiscard]] std::size_t eventCount() const {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        return m_events_.size();
    }
    [[nodiscard]] std::size_t serviceCount() const {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        return m_services_.size();
    }
    [[nodiscard]] std::size_t dtoCount() const {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        return m_dtos_.size();
    }
    [[nodiscard]] bool hasEvent(EventCode code) const {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        for (const auto& e : m_events_) {
            if (e.event_code && *e.event_code == code) {
                return true;
            }
        }
        return false;
    }

private:
    mutable std::mutex m_mutex_;
    std::vector<EventPacket> m_events_;
    std::vector<ServicePacket> m_services_;
    std::vector<DtoPacket> m_dtos_;
};

/// @brief 组装 CONNECT 响应报文（避免与 calmcar::xcp::ConnectResponse
/// 结构体同名）
Bytes MakeConnectResponse(AddressGranularity ag = AddressGranularity::Byte,
                          ByteOrder order = ByteOrder::Intel,
                          std::uint8_t max_cto = 8U, std::uint16_t max_dto = 8U,
                          bool optional = true) {
    std::uint8_t comm_mode =
        static_cast<std::uint8_t>(AgToCommModeBasicField(ag) << 1);
    if (order == ByteOrder::Motorola) {
        comm_mode |= 0x01U;
    }
    if (optional) {
        comm_mode |= 0x80U;
    }
    Bytes out{static_cast<std::uint8_t>(PacketType::Res), 0x15, comm_mode,
              max_cto};
    if (order == ByteOrder::Intel) {
        out.push_back(static_cast<std::uint8_t>(max_dto & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((max_dto >> 8) & 0xFFU));
    } else {
        out.push_back(static_cast<std::uint8_t>((max_dto >> 8) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>(max_dto & 0xFFU));
    }
    out.push_back(0x10U);
    out.push_back(0x10U);
    return out;
}

/// @brief 每个命令返回 ERR 的通用负响应
Bytes ErrBytes(ErrorCode code) {
    return Bytes{static_cast<std::uint8_t>(PacketType::Err),
                 static_cast<std::uint8_t>(code)};
}

/**
 * @brief 可用脚本编排的测试用 Slave
 *
 * 支持：
 *  - 对指定命令返回指定响应（可一次性或持续）；
 *  - 对指定命令的前 N 次调用不回响应（模拟超时）；
 *  - 记录每个命令的调用次数与顺序。
 */
class ScriptedSlave {
public:
    /// @brief 设置某个命令的持续响应
    void SetResponse(CommandCode cmd, Bytes response) {
        m_responses_[cmd] = std::move(response);
    }

    /// @brief 设置某个命令的一次性响应（消费后回退到持续响应）
    void SetOnceResponse(CommandCode cmd, Bytes response) {
        m_once_[cmd] = std::move(response);
    }

    /// @brief 令某个命令的第 n 次调用（1 起）不产生响应
    void DropNthCall(CommandCode cmd, std::size_t n) { m_drop_[cmd] = n; }

    /// @brief 令某个命令的前 n 次调用都不产生响应
    void DropFirstCalls(CommandCode cmd, std::size_t n) {
        m_drop_first_[cmd] = n;
    }

    [[nodiscard]] int Count(CommandCode cmd) const {
        const auto it = m_counts_.find(cmd);
        return it == m_counts_.end() ? 0 : it->second;
    }
    [[nodiscard]] const std::vector<CommandCode>& Order() const {
        return m_order_;
    }
    void ResetCounts() {
        m_counts_.clear();
        m_order_.clear();
    }

    Bytes operator()(BytesView packet) {
        if (packet.empty()) {
            return {};
        }
        const auto cmd = static_cast<CommandCode>(packet[0]);
        ++m_counts_[cmd];
        m_order_.push_back(cmd);

        const auto drop_it = m_drop_.find(cmd);
        if (drop_it != m_drop_.end() &&
            static_cast<std::size_t>(m_counts_[cmd]) == drop_it->second) {
            return {};  // 该次调用不回响应
        }
        const auto first_it = m_drop_first_.find(cmd);
        if (first_it != m_drop_first_.end() &&
            static_cast<std::size_t>(m_counts_[cmd]) <= first_it->second) {
            return {};
        }

        const auto once_it = m_once_.find(cmd);
        if (once_it != m_once_.end() &&
            static_cast<std::size_t>(m_counts_[cmd]) == 1) {
            return once_it->second;
        }
        const auto it = m_responses_.find(cmd);
        if (it != m_responses_.end()) {
            return it->second;
        }
        // 默认：Positive Response
        return Bytes{static_cast<std::uint8_t>(PacketType::Res)};
    }

private:
    std::map<CommandCode, Bytes> m_responses_;
    std::map<CommandCode, Bytes> m_once_;
    std::map<CommandCode, std::size_t> m_drop_;
    std::map<CommandCode, std::size_t> m_drop_first_;
    std::map<CommandCode, int> m_counts_;
    std::vector<CommandCode> m_order_;
};

/// @brief 构造短超时的执行器脚手架
struct Fixture {
    explicit Fixture(CommandTimeouts timeouts =
                         CommandTimeouts{std::chrono::milliseconds(120),
                                         std::chrono::milliseconds(120), 2})
        : executor(transport, session, timeouts, &events) {
        slave.SetResponse(CommandCode::Connect, MakeConnectResponse());
        slave.SetResponse(CommandCode::GetStatus,
                          Bytes{static_cast<std::uint8_t>(PacketType::Res),
                                0x00, 0x00, 0x01, 0x07, 0x00});
        slave.SetResponse(CommandCode::GetCommModeInfo,
                          Bytes{static_cast<std::uint8_t>(PacketType::Res),
                                0x00, 0x0E, 0x00, 0x04, 0x02, 0x08, 0x13});
        slave.SetResponse(CommandCode::Synch, ErrBytes(ErrorCode::CmdSynch));
        transport.SetResponse([this](BytesView p) { return slave(p); });
        transport.Open(executor);
    }

    /// @brief 走完 CONNECT，进入 Connected
    void Connect() { (void)executor.ExecuteConnect(0x00); }

    test::MockTransport transport;
    ScriptedSlave slave;
    Session session;
    RecordingEvents events;
    CommandExecutor executor;
};

// --------------------------------------------------------------------------
// 超时 -> SYNCH -> 重试成功
// --------------------------------------------------------------------------

TEST(RecoveryTimeout, TimeoutTriggersSynchThenRetrySucceeds) {
    Fixture f;
    f.Connect();

    // UPLOAD 第 1 次超时，之后正常
    f.slave.DropNthCall(CommandCode::Upload, 1);
    f.slave.SetResponse(
        CommandCode::Upload,
        Bytes{static_cast<std::uint8_t>(PacketType::Res), 0xAB, 0xCD});

    const Bytes got = f.executor.ExecuteUpload(2);
    EXPECT_EQ(got, BytesOf({0xAB, 0xCD}));
    EXPECT_EQ(f.slave.Count(CommandCode::Synch), 1) << "超时后应先发 SYNCH";
    EXPECT_EQ(f.slave.Count(CommandCode::Upload), 2) << "原命令应被重试一次";
    EXPECT_TRUE(f.session.IsConnected()) << "恢复成功后应回到 Connected";
}

TEST(RecoveryTimeout, SynchPrecedesRetryInCommandOrder) {
    Fixture f;
    f.Connect();
    f.slave.ResetCounts();

    f.slave.DropNthCall(CommandCode::Upload, 1);
    // UPLOAD(1) 的正常 RES 必须携带 1 字节数据（RES = [FF][data...]）
    f.slave.SetResponse(
        CommandCode::Upload,
        Bytes{static_cast<std::uint8_t>(PacketType::Res), 0x5A});

    const Bytes got = f.executor.ExecuteUpload(1);
    EXPECT_EQ(got, BytesOf({0x5A}));

    const auto& order = f.slave.Order();
    // 期望顺序：Upload(超时) -> Synch -> Upload(成功)
    ASSERT_GE(order.size(), 3U);
    EXPECT_EQ(order[0], CommandCode::Upload);
    EXPECT_EQ(order[1], CommandCode::Synch);
    EXPECT_EQ(order[2], CommandCode::Upload);
}

TEST(RecoveryTimeout, ErrCmdSynchOnlyCountsAsRecoveryConfirmation) {
    Fixture f;
    f.Connect();

    // 正常命令收到 ERR_CMD_SYNCH 时应作为普通错误上报，而不是被当成恢复成功
    f.slave.SetResponse(CommandCode::GetStatus, ErrBytes(ErrorCode::CmdSynch));
    try {
        (void)f.executor.ExecuteGetStatus();
        FAIL() << "非恢复场景的 ERR_CMD_SYNCH 应作为协议错误上报";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::ProtocolError);
        EXPECT_EQ(e.GetErrorCode(),
                  std::optional<ErrorCode>(ErrorCode::CmdSynch));
    }
}

TEST(RecoveryTimeout, RetryExhaustionReportsRecoveryFailed) {
    Fixture f{CommandTimeouts{std::chrono::milliseconds(80),
                              std::chrono::milliseconds(80), 2}};
    f.Connect();

    // 所有 UPLOAD 都不回响应 -> 首次 + 2 次重试后耗尽
    f.slave.DropFirstCalls(CommandCode::Upload, 99);
    try {
        (void)f.executor.ExecuteUpload(1);
        FAIL() << "重试耗尽应抛 RecoveryFailed";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::RecoveryFailed);
        EXPECT_EQ(e.RetryCount(), 2);
    }
    EXPECT_EQ(f.slave.Count(CommandCode::Upload), 3) << "首次 + 2 次重试";
    EXPECT_EQ(f.slave.Count(CommandCode::Synch), 2) << "每次重试前一次 SYNCH";
}

TEST(RecoveryTimeout, SynchFailureStopsRecovery) {
    Fixture f;
    f.Connect();
    // SYNCH 不回响应 -> 恢复失败
    f.slave.DropFirstCalls(CommandCode::Synch, 99);
    f.slave.DropFirstCalls(CommandCode::Upload, 99);

    try {
        (void)f.executor.ExecuteUpload(1);
        FAIL() << "SYNCH 无法确认时应报恢复失败";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::RecoveryFailed);
    }
}

TEST(RecoveryTimeout, TransportClosedPreventsSynch) {
    Fixture f;
    f.Connect();
    f.slave.ResetCounts();

    // Transport 关闭后不得发送 SYNCH。
    // 关闭后 send()
    // 本身即失败（TransportError），若在等待期间关闭则走恢复路径并报
    // RecoveryFailed；两种分类都表明"未执行 SYNCH 恢复"这一核心语义成立。
    f.transport.Close();
    try {
        (void)f.executor.ExecuteUpload(1);
        FAIL() << "Transport 已关闭时应报错";
    } catch (const XcpException& e) {
        EXPECT_TRUE(e.Category() == ErrorCategory::RecoveryFailed ||
                    e.Category() == ErrorCategory::TransportError)
            << "实际分类: " << ErrorCategoryName(e.Category());
    }
    EXPECT_EQ(f.slave.Count(CommandCode::Synch), 0)
        << "Transport 断开时不得发送 SYNCH";
}

TEST(RecoveryTimeout, ZeroRetriesFailsImmediately) {
    Fixture f{CommandTimeouts{std::chrono::milliseconds(80),
                              std::chrono::milliseconds(80), 0}};
    f.Connect();
    f.slave.DropFirstCalls(CommandCode::Upload, 99);

    try {
        (void)f.executor.ExecuteUpload(1);
        FAIL() << "max_retries=0 时应立即失败";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::RecoveryFailed);
    }
    EXPECT_EQ(f.slave.Count(CommandCode::Upload), 1);
    EXPECT_EQ(f.slave.Count(CommandCode::Synch), 0);
}

TEST(RecoveryTimeout, SessionReturnsToConnectedAfterSuccessfulRecovery) {
    Fixture f;
    f.Connect();
    f.slave.DropNthCall(CommandCode::Upload, 1);
    f.slave.SetResponse(
        CommandCode::Upload,
        Bytes{static_cast<std::uint8_t>(PacketType::Res), 0x01});

    (void)f.executor.ExecuteUpload(1);
    EXPECT_EQ(f.session.State(), SessionState::Connected);
    EXPECT_FALSE(f.session.HasPendingCommand());
}

// --------------------------------------------------------------------------
// EV_CMD_PENDING：重启 Timer，不重发原命令（计划 §6.3 / §9.4）
// --------------------------------------------------------------------------

TEST(EventCmdPending, RestartsTimerWithoutResendingCommand) {
    Fixture f{CommandTimeouts{std::chrono::milliseconds(400),
                              std::chrono::milliseconds(150), 2}};
    f.Connect();
    f.slave.ResetCounts();

    // UPLOAD 的 send 不直接回响应，改由后台线程注入 EV_CMD_PENDING 与最终 RES
    std::atomic<int> upload_sends{0};  // 统计 UPLOAD 实际被发送的次数
    f.transport.SetResponse([&](BytesView p) -> Bytes {
        const auto cmd = static_cast<CommandCode>(p[0]);
        if (cmd != CommandCode::Upload) {
            return f.slave(p);
        }
        ++upload_sends;
        return {};  // 本轮 send 不直接投递响应
    });

    std::thread injector([&f] {
        for (int i = 0; i < 2; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            const Bytes ev{static_cast<std::uint8_t>(PacketType::Ev),
                           static_cast<std::uint8_t>(EventCode::CmdPending)};
            f.transport.InjectPacket(BytesView{ev});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        // RES = [FF][data...]：请求 2 元素（AG=BYTE），故 2 字节数据
        const Bytes res{static_cast<std::uint8_t>(PacketType::Res), 0x55, 0x66};
        f.transport.InjectPacket(BytesView{res});
    });
    ThreadJoiner joiner(injector);

    const Bytes got = f.executor.ExecuteUpload(2);  // 总耗时超过单次 400ms 超时
    injector.join();

    EXPECT_EQ(got, BytesOf({0x55, 0x66}));
    EXPECT_EQ(upload_sends.load(), 1) << "EV_CMD_PENDING 不得导致原命令被重发";
    EXPECT_EQ(f.slave.Count(CommandCode::Synch), 0) << "不应触发恢复";
    EXPECT_GE(f.events.eventCount(), 1U) << "EV_CMD_PENDING 仍应上报为事件";
    EXPECT_GE(f.events.eventCount(), 1U) << "EV_CMD_PENDING 仍应上报为事件";
}

TEST(EventCmdPending, IsReportedToEventListener) {
    Fixture f;
    f.Connect();
    const Bytes ev{static_cast<std::uint8_t>(PacketType::Ev),
                   static_cast<std::uint8_t>(EventCode::CmdPending)};
    f.transport.InjectPacket(BytesView{ev});
    EXPECT_TRUE(f.events.hasEvent(EventCode::CmdPending));
}

// --------------------------------------------------------------------------
// EV / SERV / DTO 分流：不打乱响应匹配（计划 §9.5 第 4 条）
// --------------------------------------------------------------------------

TEST(EventDispatch, AsyncPacketsDoNotDisturbResponseMatching) {
    Fixture f;
    f.Connect();
    f.slave.ResetCounts();

    f.transport.SetResponse([&](BytesView p) -> Bytes {
        if (static_cast<CommandCode>(p[0]) == CommandCode::GetStatus) {
            // 先注入 EV、SERV、DTO，再给最终 RES
            const Bytes ev{static_cast<std::uint8_t>(PacketType::Ev),
                           static_cast<std::uint8_t>(EventCode::ResumeMode)};
            f.transport.InjectPacket(BytesView{ev});
            const Bytes serv{static_cast<std::uint8_t>(PacketType::Serv), 0x02,
                             0x99};
            f.transport.InjectPacket(BytesView{serv});
            const Bytes dto{0x20, 0x11, 0x22};
            f.transport.InjectPacket(BytesView{dto});
            return Bytes{static_cast<std::uint8_t>(PacketType::Res),
                         0x00,
                         0x00,
                         0x02,
                         0x09,
                         0x00};
        }
        return f.slave(p);
    });

    const GetStatusResponse status = f.executor.ExecuteGetStatus();
    EXPECT_EQ(status.state_number, 0x02U);
    EXPECT_EQ(status.session_config_id, 0x0009U);
    EXPECT_TRUE(f.events.hasEvent(EventCode::ResumeMode));
    EXPECT_EQ(f.events.serviceCount(), 1U);
    EXPECT_EQ(f.events.dtoCount(), 1U) << "DTO 本阶段仅识别并上报";
}

// --------------------------------------------------------------------------
// 错误分派（计划 §6.4 表格）
// --------------------------------------------------------------------------

TEST(ErrorDispatch, AccessLockedMapsToProtocolErrorWithUnlockHint) {
    Fixture f;
    f.Connect();
    f.slave.SetResponse(CommandCode::Upload, ErrBytes(ErrorCode::AccessLocked));
    try {
        (void)f.executor.ExecuteUpload(1);
        FAIL() << "ERR_ACCESS_LOCKED 应映射为 ProtocolError（批次 7）";
    } catch (const XcpException& e) {
        // 批次 7：解锁功能已落地，读命令遇锁不再映射 UnsupportedFeature，
        // 而是以 ProtocolError 报告并指引显式调用 Unlock()（计划 §6.4）
        EXPECT_EQ(e.Category(), ErrorCategory::ProtocolError);
        EXPECT_EQ(e.GetErrorCode(),
                  std::optional<ErrorCode>(ErrorCode::AccessLocked));
        EXPECT_EQ(e.GetCommandCode(),
                  std::optional<CommandCode>(CommandCode::Upload));
        EXPECT_NE(std::string(e.what()).find("Unlock"), std::string::npos)
            << "错误消息应指引调用方先执行 Unlock()";
    }
}

TEST(ErrorDispatch, CmdBusyReportedAsProtocolErrorWithoutRetry) {
    Fixture f;
    f.Connect();
    f.slave.ResetCounts();
    f.slave.SetResponse(CommandCode::GetStatus, ErrBytes(ErrorCode::CmdBusy));

    EXPECT_THROW((void)f.executor.ExecuteGetStatus(), XcpException);
    EXPECT_EQ(f.slave.Count(CommandCode::GetStatus), 1)
        << "ERR_CMD_BUSY 不应重试";
    EXPECT_EQ(f.slave.Count(CommandCode::Synch), 0);
}

TEST(ErrorDispatch, OutOfRangePreservesContext) {
    Fixture f;
    f.Connect();
    f.slave.SetResponse(CommandCode::Upload, ErrBytes(ErrorCode::OutOfRange));
    try {
        (void)f.executor.ExecuteUpload(99);
        FAIL() << "应抛 ProtocolError";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::ProtocolError);
        EXPECT_EQ(e.GetErrorCode(),
                  std::optional<ErrorCode>(ErrorCode::OutOfRange));
    }
}

TEST(ErrorDispatch, UnknownErrorCodePreservedInException) {
    Fixture f;
    f.Connect();
    // 0x99 不在标准错误码表中：必须保留原始码，不得顶替为 Generic
    f.slave.SetResponse(
        CommandCode::GetStatus,
        Bytes{static_cast<std::uint8_t>(PacketType::Err), 0x99});
    try {
        (void)f.executor.ExecuteGetStatus();
        FAIL() << "未知错误码应抛 ProtocolError";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::ProtocolError);
        EXPECT_FALSE(e.GetErrorCode().has_value())
            << "未知码不应被映射为已知枚举";
        EXPECT_NE(std::string(e.what()).find("0x99"), std::string::npos)
            << "错误消息应保留原始错误码";
    }
}

TEST(ErrorDispatch, ErrAdditionalInfoPreserved) {
    // 直接验证解析层保留附加信息（执行器不做丢弃）
    ResponseParser parser(ByteOrder::Intel);
    const auto parsed = parser.Parse(BytesOf({0xFE, 0x22, 0x01, 0x02, 0x03}),
                                     CommandCode::Upload);
    ASSERT_TRUE(parsed.has_value());
    const auto* err = std::get_if<NegativeResponse>(&*parsed);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->additional_info, BytesOf({0x01, 0x02, 0x03}));
}

// --------------------------------------------------------------------------
// GET_COMM_MODE_INFO / SHORT_UPLOAD 的 ERR_CMD_UNKNOWN 降级
// --------------------------------------------------------------------------

TEST(Degradation, GetCommModeInfoUnknownReturnsNullopt) {
    Fixture f;
    f.Connect();
    f.slave.SetResponse(CommandCode::GetCommModeInfo,
                        ErrBytes(ErrorCode::CmdUnknown));

    const auto info = f.executor.ExecuteGetCommModeInfo();
    EXPECT_FALSE(info.has_value()) << "可选命令不可用时应降级为 nullopt";
    EXPECT_TRUE(f.session.IsConnected()) << "降级不应影响会话";
}

TEST(Degradation, GetCommModeInfoSuccessUpdatesSession) {
    Fixture f;
    f.Connect();
    const auto info = f.executor.ExecuteGetCommModeInfo();
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->max_bs, 0x04U);
    EXPECT_EQ(info->min_st, 0x02U);
    EXPECT_EQ(info->driver_version_major, 1U);
    EXPECT_EQ(info->driver_version_minor, 3U);
    EXPECT_TRUE(f.session.Parameters().comm_mode_info.has_value());
}

TEST(Degradation, ShortUploadUnknownMarksSessionDegraded) {
    Fixture f;
    f.Connect();
    f.slave.SetResponse(CommandCode::ShortUpload,
                        ErrBytes(ErrorCode::CmdUnknown));

    EXPECT_THROW((void)f.executor.ExecuteShortUpload(1, 0x00, 0x1000),
                 XcpException);
    EXPECT_FALSE(f.session.Parameters().short_upload_available)
        << "ERR_CMD_UNKNOWN 应标记 SHORT_UPLOAD 不可用";
}

// --------------------------------------------------------------------------
// Transport 错误与单 Pending 约束
// --------------------------------------------------------------------------

TEST(ExecutorGuards, SendFailurePropagatesAsTransportError) {
    Fixture f;
    f.Connect();
    f.transport.FailNextSend();
    try {
        (void)f.executor.ExecuteGetStatus();
        FAIL() << "发送失败应抛 TransportError";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::TransportError);
    }
    // 失败后 Pending 必须被清理，否则后续命令会被永久拒绝
    EXPECT_FALSE(f.session.HasPendingCommand());
    EXPECT_NO_THROW((void)f.executor.ExecuteGetStatus());
}

TEST(ExecutorGuards, ResponseSlotOccupiedDoesNotCorruptNextResponse) {
    Fixture f;
    f.Connect();
    // 一条命令期间注入两个 RES：第二个必须被丢弃，最终返回第一个
    f.transport.SetResponse([&](BytesView p) -> Bytes {
        if (static_cast<CommandCode>(p[0]) == CommandCode::GetStatus) {
            const Bytes first{static_cast<std::uint8_t>(PacketType::Res),
                              0x00,
                              0x00,
                              0x11,
                              0x01,
                              0x00};
            const Bytes second{static_cast<std::uint8_t>(PacketType::Res),
                               0x00,
                               0x00,
                               0x22,
                               0x02,
                               0x00};
            f.transport.InjectPacket(BytesView{first});
            f.transport.InjectPacket(BytesView{second});
            return {};
        }
        return f.slave(p);
    });

    const GetStatusResponse status = f.executor.ExecuteGetStatus();
    EXPECT_EQ(status.state_number, 0x11U) << "多余 RES 不得覆盖已收到的响应";
}

TEST(ExecutorGuards, CommandsRejectedAfterTransportClosed) {
    Fixture f;
    f.Connect();
    f.transport.Close();
    // 关闭后等待必然立即返回，然后进入恢复 -> 因 Transport 关闭而失败
    try {
        (void)f.executor.ExecuteGetStatus();
        FAIL() << "Transport 关闭后命令应失败";
    } catch (const XcpException& e) {
        EXPECT_TRUE(e.Category() == ErrorCategory::RecoveryFailed ||
                    e.Category() == ErrorCategory::TransportError);
    }
}

// --------------------------------------------------------------------------
// CONNECT / DISCONNECT 流程
// --------------------------------------------------------------------------

TEST(ConnectFlow, ConnectParsesNegotiatedParameters) {
    Fixture f;
    f.slave.SetResponse(
        CommandCode::Connect,
        MakeConnectResponse(AddressGranularity::DWord, ByteOrder::Motorola,
                            0x20U, 0x0040U));
    const ConnectResponse connect = f.executor.ExecuteConnect(0x00);

    EXPECT_EQ(connect.address_granularity, AddressGranularity::DWord);
    EXPECT_EQ(connect.byte_order, ByteOrder::Motorola);
    EXPECT_EQ(connect.max_cto, 0x20U);
    EXPECT_EQ(connect.max_dto, 0x0040U);
    EXPECT_EQ(f.session.State(), SessionState::Connected);
    EXPECT_EQ(f.session.GetByteOrder(), ByteOrder::Motorola);
}

TEST(ConnectFlow, InvalidConnectParametersFailAndDoNotEnterConnected) {
    Fixture f;
    // MAX_CTO=7 低于协议下限 0x08 -> 校验失败
    f.slave.SetResponse(CommandCode::Connect,
                        MakeConnectResponse(AddressGranularity::Byte,
                                            ByteOrder::Intel, 0x07U, 0x0008U));
    EXPECT_THROW((void)f.executor.ExecuteConnect(0x00), XcpException);
    EXPECT_FALSE(f.session.IsConnected());
    EXPECT_EQ(f.session.State(), SessionState::Failed);
}

TEST(ConnectFlow, MalformedConnectResponseRejected) {
    Fixture f;
    // RES 负载不足 7 字节
    f.slave.SetResponse(
        CommandCode::Connect,
        Bytes{static_cast<std::uint8_t>(PacketType::Res), 0x15, 0xC0, 0x08});
    try {
        (void)f.executor.ExecuteConnect(0x00);
        FAIL() << "长度不足的 CONNECT 响应应抛 MalformedPacket";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::MalformedPacket);
    }
    EXPECT_FALSE(f.session.IsConnected());
}

TEST(ConnectFlow, DisconnectClearsSession) {
    Fixture f;
    f.Connect();
    f.slave.SetResponse(CommandCode::Disconnect,
                        Bytes{static_cast<std::uint8_t>(PacketType::Res)});
    EXPECT_NO_THROW(f.executor.ExecuteDisconnect());
    EXPECT_EQ(f.session.State(), SessionState::Disconnected);
    EXPECT_EQ(f.session.MaxCto(), 0U);
}

TEST(ConnectFlow, DisconnectFailureStillClearsLocalState) {
    Fixture f;
    f.Connect();
    // Slave 以 ERR_CMD_BUSY 拒绝断开
    f.slave.SetResponse(CommandCode::Disconnect, ErrBytes(ErrorCode::CmdBusy));
    EXPECT_THROW(f.executor.ExecuteDisconnect(), XcpException);
    // 计划 §5.4：即使 DISCONNECT 失败也释放本地资源
    EXPECT_EQ(f.session.State(), SessionState::Disconnected);
    EXPECT_FALSE(f.session.HasPendingCommand());
}

TEST(ConnectFlow, RepeatedDisconnectIsIdempotentAfterReset) {
    Fixture f;
    f.Connect();
    f.slave.SetResponse(CommandCode::Disconnect,
                        Bytes{static_cast<std::uint8_t>(PacketType::Res)});
    f.executor.ExecuteDisconnect();
    // 已 Disconnected：重复断开应由上层（XcpMaster）幂等处理；
    // 直接调用 Executor 时会因状态非法而抛
    // InvalidState，这也是一种明确的失败语义。
    EXPECT_THROW(f.executor.ExecuteDisconnect(), XcpException);
}

// --------------------------------------------------------------------------
// 并发约束：第二条命令在有 Pending 时被拒绝
// --------------------------------------------------------------------------

TEST(ExecutorGuards, ConcurrentCommandRejectedWhilePending) {
    Session session;
    test::MockTransport transport;
    RecordingEvents events;
    CommandExecutor executor(transport, session, CommandTimeouts{}, &events);
    transport.Open(executor);

    // 手工把 Session 置为 Connected 并占用 Pending
    ConnectResponse negotiated;
    negotiated.max_cto = 0x08U;
    negotiated.max_dto = 0x0008U;
    negotiated.address_granularity = AddressGranularity::Byte;
    negotiated.byte_order = ByteOrder::Intel;
    session.BeginConnecting();
    session.EstablishConnection(negotiated);
    session.MarkCommandSent(CommandCode::Upload);

    // 此时任何命令都应被拒绝
    transport.SetResponse([](BytesView) { return Bytes{}; });
    try {
        (void)executor.ExecuteGetStatus();
        FAIL() << "已有 Pending 时应拒绝新命令";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidState);
    }
    session.ClearPendingCommand();
}

}  // namespace
}  // namespace calmcar::xcp
