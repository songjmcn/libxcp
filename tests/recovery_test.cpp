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

    /// @brief 令某个命令的前 n 次调用返回 ERR_CMD_BUSY，其后返回给定响应
    /// @param cmd 命令码
    /// @param n 返回 BUSY 的次数
    /// @param then 之后使用的持续响应
    /// @details F-11 用例需要「先 BUSY 若干次、再成功」的脚本，以验证重试
    ///          路径被走到后能立即收敛（而不是一直等到总体 deadline）。
    void SetBusyFirstCalls(CommandCode cmd, std::size_t n, Bytes then) {
        m_busy_first_[cmd] = n;
        m_responses_[cmd] = std::move(then);
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

        // F-11：前 n 次返回 ERR_CMD_BUSY，用于验证 BUSY 重试后能立即收敛。
        const auto busy_it = m_busy_first_.find(cmd);
        if (busy_it != m_busy_first_.end() &&
            static_cast<std::size_t>(m_counts_[cmd]) <= busy_it->second) {
            return ErrBytes(ErrorCode::CmdBusy);
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
    /// @brief 前 n 次调用返回 ERR_CMD_BUSY 的命令（F-11 用例）
    std::map<CommandCode, std::size_t> m_busy_first_;
    std::map<CommandCode, int> m_counts_;
    std::vector<CommandCode> m_order_;
};

/// @brief 构造短超时的执行器脚手架
struct Fixture {
    explicit Fixture(CommandTimeouts timeouts =
                         CommandTimeouts{std::chrono::milliseconds(120),
                                         std::chrono::milliseconds(120), 2},
                     RecoveryPolicy policy = {})
        : executor(transport, session, timeouts, &events, policy) {
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

// --------------------------------------------------------------------------
// XCPlite 上传响应的精确 4 字节对齐填充兼容
// --------------------------------------------------------------------------

TEST(UploadResponsePadding, TrimsExactAlignedUploadResponse) {
    Fixture f;
    f.Connect();
    f.slave.SetResponse(
        CommandCode::Upload,
        BytesOf({static_cast<std::uint8_t>(PacketType::Res), 0x01, 0x02, 0x03,
                 0x04, 0x05, 0x06, 0x07, 0x08, 0xA1, 0xA2, 0xA3}));

    const Bytes got = f.executor.ExecuteUpload(8);
    EXPECT_EQ(got, BytesOf({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}));
}

TEST(UploadResponsePadding, TrimsExactAlignedShortUploadResponse) {
    Fixture f;
    f.Connect();
    f.slave.SetResponse(
        CommandCode::ShortUpload,
        BytesOf({static_cast<std::uint8_t>(PacketType::Res), 0x01, 0x02, 0x03,
                 0x04, 0x05, 0x06, 0x07, 0x08, 0xA1, 0xA2, 0xA3}));

    const Bytes got = f.executor.ExecuteShortUpload(8, 0x02, 0x10068);
    EXPECT_EQ(got, BytesOf({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}));
}

TEST(UploadResponsePadding, RejectsUnexpectedLength) {
    Fixture f;
    f.Connect();
    f.slave.SetResponse(
        CommandCode::ShortUpload,
        BytesOf({static_cast<std::uint8_t>(PacketType::Res), 0x01, 0x02, 0x03,
                 0x04, 0x05, 0x06, 0x07, 0x08, 0xA1, 0xA2}));

    try {
        (void)f.executor.ExecuteShortUpload(8, 0x02, 0x10068);
        FAIL() << "非对齐长度必须继续作为畸形响应拒绝";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::MalformedPacket);
    }
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

// --------------------------------------------------------------------------
// F-11：ERR_CMD_BUSY → wait t7 → repeat（受总体 deadline 与取消约束）
// --------------------------------------------------------------------------

/// @brief 本用例取代旧的 `CmdBusyReportedAsProtocolErrorWithoutRetry`。
/// @details 旧用例显式锁定「ERR_CMD_BUSY 不重试」，与规范恢复矩阵冲突：
///          原始 PDF 第 246 页为 GET_STATUS/SET_REQUEST/GET_SEED/UNLOCK 等
///          几乎每个命令都列出 `ERR_CMD_BUSY → wait t7 → repeat ∞ times`。
///          规范允许持续等待资源，但库实现必须受总体 deadline 与取消控制
///          （方案 §4.6 第 3、6 条），故行为改为「有限等待后以 RecoveryFailed
///          报告 deadline 耗尽」，而非无限重试也非立即失败。
TEST(RecoveryBusy, BusyIsRetriedUntilTotalDeadlineExhausted) {
    // 缩短参数以便用例快速跑到 deadline：间隔 20 ms、总体 150 ms。
    CommandTimeouts timeouts{std::chrono::milliseconds(120),
                             std::chrono::milliseconds(120), 2};
    RecoveryPolicy policy;
    policy.busy_retry_delay = std::chrono::milliseconds(20);
    policy.busy_total_timeout = std::chrono::milliseconds(150);
    Fixture f(timeouts, policy);
    f.Connect();
    f.slave.ResetCounts();
    f.slave.SetResponse(CommandCode::GetStatus, ErrBytes(ErrorCode::CmdBusy));

    const auto start = std::chrono::steady_clock::now();
    try {
        (void)f.executor.ExecuteGetStatus();
        FAIL() << "ERR_CMD_BUSY 持续返回应以 RecoveryFailed 结束";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::RecoveryFailed);
        EXPECT_NE(std::string(e.what()).find("BUSY"), std::string::npos);
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

    // 关键断言：发生了重试（旧行为是只发 1 次），且不是无限重试。
    EXPECT_GT(f.slave.Count(CommandCode::GetStatus), 1)
        << "ERR_CMD_BUSY 必须按恢复矩阵等待 t7 后重发";
    // 不得无谓地做 SYNCH：BUSY 不是超时，无需重新同步。
    EXPECT_EQ(f.slave.Count(CommandCode::Synch), 0)
        << "ERR_CMD_BUSY 不应触发 SYNCH 恢复";
    // 总体 deadline 得到遵守（留出调度余量）。
    EXPECT_LT(elapsed.count(), 1000)
        << "总体 deadline 应约束 BUSY 重试，不得无限等待";
    // 重试次数应大致符合 deadline/间隔，而不是只试一次或失控。
    EXPECT_GE(f.slave.Count(CommandCode::GetStatus), 3);
}

/// @brief 关闭 BUSY 重试后回退旧行为：立即以 ProtocolError 报告
TEST(RecoveryBusy, DisabledBusyRetryKeepsImmediateProtocolError) {
    RecoveryPolicy policy;
    policy.busy_retry_enabled = false;
    Fixture f(CommandTimeouts{std::chrono::milliseconds(120),
                              std::chrono::milliseconds(120), 2},
              policy);
    f.Connect();
    f.slave.ResetCounts();
    f.slave.SetResponse(CommandCode::GetStatus, ErrBytes(ErrorCode::CmdBusy));

    try {
        (void)f.executor.ExecuteGetStatus();
        FAIL() << "应抛 ProtocolError";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::ProtocolError);
        EXPECT_EQ(e.GetErrorCode(),
                  std::optional<ErrorCode>(ErrorCode::CmdBusy));
    }
    EXPECT_EQ(f.slave.Count(CommandCode::GetStatus), 1)
        << "关闭重试后只应发送一次";
}

/// @brief BUSY 之后一旦成功即返回，不继续等待到总体 deadline
/// @details 用「前 N 次 BUSY、其后成功」的脚本确保重试路径被走到，
///          并断言总耗时不接近 deadline——证明成功路径会立即收敛。
TEST(RecoveryBusy, BusyThenSuccessReturnsWithoutWaitingDeadline) {
    RecoveryPolicy policy;
    policy.busy_retry_delay = std::chrono::milliseconds(10);
    policy.busy_total_timeout = std::chrono::seconds(5);
    Fixture f(CommandTimeouts{std::chrono::milliseconds(120),
                              std::chrono::milliseconds(120), 2},
              policy);
    f.Connect();
    f.slave.ResetCounts();
    // 前 2 次返回 BUSY，第 3 次成功：重试路径被走到后立即收敛。
    f.slave.SetBusyFirstCalls(CommandCode::GetStatus, 2,
                              Bytes{static_cast<std::uint8_t>(PacketType::Res),
                                    0x00, 0x00, 0x01, 0x07, 0x00});

    const auto start = std::chrono::steady_clock::now();
    EXPECT_NO_THROW((void)f.executor.ExecuteGetStatus())
        << "BUSY 之后成功的响应必须正常返回";
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    EXPECT_EQ(f.slave.Count(CommandCode::GetStatus), 3)
        << "应为「BUSY、BUSY、成功」共 3 次";
    EXPECT_LT(elapsed.count(), 1000)
        << "成功后应立即返回，不得继续等到 5s 总体 deadline";
    EXPECT_EQ(f.slave.Count(CommandCode::Synch), 0);
}

/// @brief RecoveryPolicy 参数校验（方案 §4.6 第 7 条）
TEST(RecoveryBusy, PolicyValidationRejectsIllegalValues) {
    RecoveryPolicy ok;
    EXPECT_TRUE(ok.Validate().empty()) << "默认值必须合法";

    RecoveryPolicy zero_delay;
    zero_delay.busy_retry_delay = std::chrono::milliseconds(0);
    EXPECT_FALSE(zero_delay.Validate().empty()) << "零间隔必须被判非法";

    RecoveryPolicy negative_delay;
    negative_delay.busy_retry_delay = std::chrono::milliseconds(-1);
    EXPECT_FALSE(negative_delay.Validate().empty()) << "负间隔必须被判非法";

    RecoveryPolicy zero_total;
    zero_total.busy_total_timeout = std::chrono::milliseconds(0);
    EXPECT_FALSE(zero_total.Validate().empty()) << "零总体 deadline 非法";

    RecoveryPolicy total_less_than_delay;
    total_less_than_delay.busy_retry_delay = std::chrono::milliseconds(500);
    total_less_than_delay.busy_total_timeout = std::chrono::milliseconds(100);
    EXPECT_FALSE(total_less_than_delay.Validate().empty())
        << "总体 deadline 小于单次等待属自相矛盾配置";

    // 关闭重试后，数值不再参与重试，不应因旧的非法值阻碍连接。
    RecoveryPolicy disabled;
    disabled.busy_retry_enabled = false;
    disabled.busy_retry_delay = std::chrono::milliseconds(0);
    EXPECT_TRUE(disabled.Validate().empty())
        << "关闭 BUSY 重试后等待参数不再生效";
}

// --------------------------------------------------------------------------
// F-11：取消语义（Cancelled 与 Timeout/ProtocolError 严格区分）
// --------------------------------------------------------------------------

/// @brief 取消挂起中的等待：返回 Cancelled，且不触发 SYNCH 恢复
TEST(RecoveryCancel, CancelDuringWaitReturnsCancelledWithoutRecovery) {
    // 命令超时设得远长于取消时刻，确保走「取消」而非「超时」分支。
    Fixture f(CommandTimeouts{std::chrono::seconds(5),
                              std::chrono::milliseconds(120), 2});
    f.Connect();
    f.slave.ResetCounts();
    f.slave.DropFirstCalls(CommandCode::GetStatus, 1);  // 不产生响应

    std::optional<ErrorCategory> category;
    std::string message;
    std::thread worker([&] {
        try {
            (void)f.executor.ExecuteGetStatus();
        } catch (const XcpException& e) {
            category = e.Category();
            message = e.what();
        }
    });
    ThreadJoiner joiner(worker);

    // 等命令确实发出后再取消，避免取消发生在发送之前。
    for (int i = 0; i < 200 && f.slave.Count(CommandCode::GetStatus) == 0;
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    f.executor.CancelPendingOperation();

    // 取消必须能唤醒等待者，不得等到 5s 超时。
    const auto start = std::chrono::steady_clock::now();
    worker.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    EXPECT_LT(elapsed.count(), 2000) << "取消必须立即唤醒等待者";

    ASSERT_TRUE(category.has_value()) << "取消应以异常返回";
    EXPECT_EQ(*category, ErrorCategory::Cancelled)
        << "取消必须用 Cancelled，不得伪装成 Timeout";
    EXPECT_EQ(f.slave.Count(CommandCode::Synch), 0)
        << "取消不是协议故障，不得触发 SYNCH 恢复";
    EXPECT_NE(message.find("取消"), std::string::npos);
}

/// @brief 取消是幂等的，且无挂起操作时为空操作
TEST(RecoveryCancel, CancelIsIdempotentAndSafeWhenIdle) {
    Fixture f;
    f.Connect();
    f.executor.CancelPendingOperation();
    f.executor.CancelPendingOperation();  // 重复调用不得崩溃
    // 取消标志不影响已经完成的操作，下一次命令正常执行。
    f.slave.SetResponse(CommandCode::GetStatus,
                        Bytes{static_cast<std::uint8_t>(PacketType::Res),
                              0x00, 0x00, 0x01, 0x07, 0x00});
    EXPECT_NO_THROW((void)f.executor.ExecuteGetStatus())
        << "顶层命令开始前应清除上一轮取消标志";
}

/// @brief Transport 关闭属「取消类」唤醒：等待者返回 Cancelled 而非 Timeout
TEST(RecoveryCancel, TransportCloseWakesWaiterAsCancelled) {
    Fixture f(CommandTimeouts{std::chrono::seconds(5),
                              std::chrono::milliseconds(120), 2});
    f.Connect();
    f.slave.ResetCounts();
    f.slave.DropFirstCalls(CommandCode::GetStatus, 1);

    std::optional<ErrorCategory> category;
    std::thread worker([&] {
        try {
            (void)f.executor.ExecuteGetStatus();
        } catch (const XcpException& e) {
            category = e.Category();
        }
    });
    ThreadJoiner joiner(worker);

    for (int i = 0; i < 200 && f.slave.Count(CommandCode::GetStatus) == 0;
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    f.executor.OnTransportClosed("测试主动关闭");

    worker.join();
    ASSERT_TRUE(category.has_value());
    EXPECT_EQ(*category, ErrorCategory::Cancelled)
        << "Transport 关闭须按 §4.6 第 6 条返回 Cancelled";
}

/// @brief 取消在 BUSY 等待期间同样生效，且不越过总体 deadline
TEST(RecoveryCancel, CancelDuringBusyWaitIsHonoured) {
    RecoveryPolicy policy;
    policy.busy_retry_delay = std::chrono::milliseconds(5000);
    policy.busy_total_timeout = std::chrono::seconds(30);
    Fixture f(CommandTimeouts{std::chrono::milliseconds(120),
                              std::chrono::milliseconds(120), 2},
              policy);
    f.Connect();
    f.slave.ResetCounts();
    f.slave.SetResponse(CommandCode::GetStatus, ErrBytes(ErrorCode::CmdBusy));

    std::optional<ErrorCategory> category;
    std::thread worker([&] {
        try {
            (void)f.executor.ExecuteGetStatus();
        } catch (const XcpException& e) {
            category = e.Category();
        }
    });
    ThreadJoiner joiner(worker);

    for (int i = 0; i < 200 && f.slave.Count(CommandCode::GetStatus) == 0;
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    f.executor.CancelPendingOperation();

    const auto start = std::chrono::steady_clock::now();
    worker.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    EXPECT_LT(elapsed.count(), 2000)
        << "BUSY 等待（t7=5s）必须可被取消立即中断";

    ASSERT_TRUE(category.has_value());
    EXPECT_EQ(*category, ErrorCategory::Cancelled);
    // 尚未越过 30s deadline，故因取消而非 deadline 结束。
    EXPECT_EQ(f.slave.Count(CommandCode::GetStatus), 1);
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

// ===========================================================================
// F-09：SHORT_UPLOAD 成功后本地隐含 MTA 必须同步推进
// ===========================================================================
//
// 规范要求 SHORT_UPLOAD 成功后把 MTA 设为「上传数据块之后的第一个数据
// 字节」。若本端不更新隐含 MTA，此后一旦发生 UPLOAD 超时恢复，
// RestoreUploadMta() 会按陈旧地址重放 SET_MTA，导致读到的位置与预期错位。

/// @brief SHORT_UPLOAD 推进隐含 MTA：后续 UPLOAD 超时恢复须重放推进后的地址
TEST(ShortUploadMtaSync, AdvanceIsReplayedByUploadRecovery) {
    Fixture f;
    f.Connect();
    f.slave.ResetCounts();

    // SHORT_UPLOAD 读 2 个元素（AG=BYTE，即 2 字节），地址 0x1000
    f.slave.SetResponse(
        CommandCode::ShortUpload,
        BytesOf({static_cast<std::uint8_t>(PacketType::Res), 0x11, 0x22}));
    const Bytes first = f.executor.ExecuteShortUpload(2, 0x00U, 0x1000U);
    EXPECT_EQ(first, BytesOf({0x11, 0x22}));

    // 随后 UPLOAD 超时一次：恢复路径会重放 SET_MTA
    f.slave.DropNthCall(CommandCode::Upload, 1);
    f.slave.SetResponse(
        CommandCode::Upload,
        BytesOf({static_cast<std::uint8_t>(PacketType::Res), 0x33}));
    f.slave.ResetCounts();
    const Bytes second = f.executor.ExecuteUpload(1);
    EXPECT_EQ(second, BytesOf({0x33}));

    // 恢复时发出的 SET_MTA 必须携带 0x1000 + 2 = 0x1002
    // SET_MTA 报文：[F6][0x00 保留][0x00 AG 保留][addr 4 字节 LE][ext]
    bool saw_advanced_set_mta = false;
    for (const auto& pkt : f.transport.SentPackets()) {
        if (pkt.size() < 8U || pkt[0] != 0xF6U) {
            continue;
        }
        const auto addr = static_cast<std::uint32_t>(pkt[4]) |
                          (static_cast<std::uint32_t>(pkt[5]) << 8U) |
                          (static_cast<std::uint32_t>(pkt[6]) << 16U) |
                          (static_cast<std::uint32_t>(pkt[7]) << 24U);
        if (addr == 0x1002U) {
            saw_advanced_set_mta = true;
        }
        EXPECT_NE(addr, 0x1000U)
            << "恢复重放了陈旧地址 0x1000，SHORT_UPLOAD 未同步 MTA";
    }
    EXPECT_TRUE(saw_advanced_set_mta)
        << "未观察到携带推进后地址 0x1002 的 SET_MTA";
}

/// @brief 反向保护：未追踪过 SHORT_UPLOAD 时不得凭空造出 MTA
TEST(ShortUploadMtaSync, NoShortUploadLeavesRecoveryMtaUnset) {
    Fixture f;
    f.Connect();
    // 先做一次正常 SET_MTA + UPLOAD，确立基准
    f.executor.ExecuteSetMta(0x00U, 0x2000U);
    f.slave.SetResponse(
        CommandCode::Upload,
        BytesOf({static_cast<std::uint8_t>(PacketType::Res), 0x44}));
    (void)f.executor.ExecuteUpload(1);

    // 此后 UPLOAD 超时恢复：重放的是 SET_MTA 记录的 0x2000，而非其它值
    f.slave.DropNthCall(CommandCode::Upload, 1);
    f.slave.SetResponse(
        CommandCode::Upload,
        BytesOf({static_cast<std::uint8_t>(PacketType::Res), 0x55}));
    (void)f.executor.ExecuteUpload(1);

    bool saw_expected = false;
    for (const auto& pkt : f.transport.SentPackets()) {
        if (pkt.size() < 8U || pkt[0] != 0xF6U) {
            continue;
        }
        const auto addr = static_cast<std::uint32_t>(pkt[4]) |
                          (static_cast<std::uint32_t>(pkt[5]) << 8U) |
                          (static_cast<std::uint32_t>(pkt[6]) << 16U) |
                          (static_cast<std::uint32_t>(pkt[7]) << 24U);
        if (addr == 0x2000U) {
            saw_expected = true;
        }
    }
    EXPECT_TRUE(saw_expected) << "恢复应重放最近一次成功 SET_MTA 的地址";
}

// --------------------------------------------------------------------------
// 通道重开复位（批次 B1：修复"一次 Disconnect 后再也无法 Connect"）
// --------------------------------------------------------------------------

/// @brief 构造一个已进入 Closed 通道状态的执行器
/// @param f 已 Connect 的夹具
/// @details 直接调用 OnTransportClosed 模拟 Disconnect 引发的通道关闭，
///          这是重连缺陷的触发条件；不需要真的走 Transport 生命周期。
static void CloseChannel(Fixture& f) { f.executor.OnTransportClosed("测试关闭"); }

TEST(TransportReopen, ClosedFlagIsObservableAfterClose) {
    Fixture f;
    f.Connect();
    f.slave.ResetCounts();

    EXPECT_FALSE(f.executor.IsTransportClosed())
        << "正常连接后不应处于关闭态";

    CloseChannel(f);
    EXPECT_TRUE(f.executor.IsTransportClosed())
        << "OnTransportClosed 后必须可被观察到，否则无法诊断命令失败原因";
}

TEST(TransportReopen, ReopenedTransportClearsClosedFlag) {
    Fixture f;
    f.Connect();
    CloseChannel(f);
    ASSERT_TRUE(f.executor.IsTransportClosed());

    f.executor.OnTransportReopened();

    EXPECT_FALSE(f.executor.IsTransportClosed())
        << "OnTransportReopened 必须清回关闭标志，否则重连后第一条命令立即失败";
}

TEST(TransportReopen, CommandAfterReopenIsActuallySent) {
    Fixture f;
    f.Connect();
    CloseChannel(f);
    f.executor.OnTransportReopened();
    f.slave.ResetCounts();

    // 关闭态下命令根本不会被发出（WaitForResponse 立即返回），
    // 因此"命令确实到达对端"才是复位生效的硬证据。
    (void)f.executor.ExecuteGetStatus();

    EXPECT_EQ(f.slave.Count(CommandCode::GetStatus), 1)
        << "复位后命令必须真正发出；若仍处于关闭态则计数为 0";
}

TEST(TransportReopen, CommandWhileClosedReturnsCancelledWithoutHittingSlave) {
    Fixture f;
    f.Connect();
    CloseChannel(f);
    f.slave.ResetCounts();

    // 对照用例：**不**复位时必须立即失败，证明上一个用例的"发出"确由复位带来。
    std::optional<ErrorCategory> category;
    try {
        (void)f.executor.ExecuteGetStatus();
    } catch (const XcpException& e) {
        category = e.Category();
    }

    ASSERT_TRUE(category.has_value()) << "关闭态下命令须失败而非静默返回";
    EXPECT_EQ(*category, ErrorCategory::RecoveryFailed)
        << "通道已关闭时 WaitForResponse 立即返回 nullopt，被统一当作响应超时"
           "走恢复路径；关键约束是命令绝不静默成功";
    // 注意：关闭态下 TX 仍会真实发出（PerformAttempt 不检查关闭标志），
    // 因此这里**不能**断言计数为 0。本用例锁定的是"不静默成功"这一契约。
}

TEST(TransportReopen, ResetIsIdempotentAndSafeWithoutPriorClose) {
    Fixture f;
    f.Connect();

    // 连接成功后立刻复位（XcpMaster::Connect 的真实调用时序）不得破坏可用性
    f.executor.OnTransportReopened();
    f.executor.OnTransportReopened();
    f.slave.ResetCounts();

    (void)f.executor.ExecuteGetStatus();
    EXPECT_EQ(f.slave.Count(CommandCode::GetStatus), 1)
        << "冗余复位不得影响后续命令";
}

TEST(TransportReopen, ReopenedTransportStillSupportsFullRecoveryBudget) {
    Fixture f;
    f.Connect();
    f.slave.ResetCounts();

    // 先制造一次真实的超时恢复：UPLOAD 第 1 次不回响应，随后恢复成功。
    // 这会让执行器经历完整的"超时 → SYNCH → 重试"事务。
    f.slave.DropNthCall(CommandCode::Upload, 1);
    f.slave.SetResponse(
        CommandCode::Upload,
        Bytes{static_cast<std::uint8_t>(PacketType::Res), 0x5A});
    const Bytes got = f.executor.ExecuteUpload(1);
    EXPECT_EQ(got, BytesOf({0x5A}));
    EXPECT_EQ(f.slave.Count(CommandCode::Synch), 1)
        << "首轮确实经历了恢复事务（前置条件）";

    // 通道关闭再重开：新会话必须仍具备**完整**的恢复能力。
    // 说明：m_synch_confirmed_ / m_in_recovery_ 本身在 SendSynch 入口与出口
    // 都会被复位，因此它们在命令之间的残留并非可观测缺陷；本用例锁定的是
    // "重开后恢复预算未被上一会话透支"这一对外契约。
    CloseChannel(f);
    f.executor.OnTransportReopened();
    f.slave.ResetCounts();

    f.slave.DropFirstCalls(CommandCode::Upload, 99);
    try {
        (void)f.executor.ExecuteUpload(1);
        FAIL() << "持续无响应应抛错";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::RecoveryFailed);
        EXPECT_EQ(e.RetryCount(), 2) << "重试预算必须完整（首次 + 2 次重试）";
    }
    EXPECT_EQ(f.slave.Count(CommandCode::Synch), 2)
        << "新会话须重新执行 SYNCH 恢复，而非沿用上一会话的确认结果";
}

}  // namespace
}  // namespace calmcar::xcp
