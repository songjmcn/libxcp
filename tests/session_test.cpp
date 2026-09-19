/**
 * @file session_test.cpp
 * @brief Session 状态机、CONNECT 参数校验与单 Outstanding Command 约束的测试。
 */

#include "libxcp/session.hpp"

#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace calmcar::xcp {
namespace {

/// @brief 构造一个合法的 CONNECT 协商结果（规范示例：MAX_CTO=8, MAX_DTO=8,
/// AG=BYTE）
ConnectResponse ValidConnect(std::uint8_t max_cto = 0x08,
                             std::uint16_t max_dto = 0x0008,
                             AddressGranularity ag = AddressGranularity::Byte,
                             ByteOrder byte_order = ByteOrder::Intel) {
    ConnectResponse resp;
    resp.m_resource_mask_ = 0x15U;
    resp.m_byte_order_ = byte_order;
    resp.m_address_granularity_ = ag;
    resp.m_slave_block_mode_supported_ = true;
    resp.m_optional_comm_mode_available_ = true;
    resp.m_max_cto_ = max_cto;
    resp.m_max_dto_ = max_dto;
    resp.m_protocol_layer_version_ = 0x10;
    resp.m_transport_layer_version_ = 0x10;
    resp.m_transport_layer_version_ = 0x10;
    return resp;
}

/// @brief 走完 Disconnected -> Connecting -> Connected
void Connect(Session& session) {
    session.BeginConnecting();
    session.EstablishConnection(ValidConnect());
}

// --------------------------------------------------------------------------
// 初始状态与默认参数
// --------------------------------------------------------------------------

TEST(SessionInitial, StartsDisconnectedWithDefaults) {
    Session session;
    EXPECT_EQ(session.State(), SessionState::Disconnected);
    EXPECT_FALSE(session.IsConnected());
    EXPECT_FALSE(session.HasPendingCommand());
    EXPECT_EQ(session.MaxCto(), 0U);
    EXPECT_EQ(session.MaxDto(), 0U);
    EXPECT_EQ(session.GetByteOrder(), ByteOrder::Intel);
    EXPECT_EQ(session.GetAddressGranularity(), AddressGranularity::Byte);
    EXPECT_TRUE(session.FailReason().empty());
    // 未连接时不应认为 SHORT_UPLOAD 已被验证可用，但字段保持默认值不影响流程
    EXPECT_TRUE(session.Parameters().m_short_upload_available_);
    EXPECT_FALSE(session.Parameters().m_comm_mode_info_.has_value());
    EXPECT_FALSE(session.Parameters().m_status_.has_value());
}

// --------------------------------------------------------------------------
// 正常连接流程
// --------------------------------------------------------------------------

TEST(SessionLifecycle, ConnectEstablishesParameters) {
    Session session;
    session.BeginConnecting();
    EXPECT_EQ(session.State(), SessionState::Connecting);

    const auto connect = ValidConnect(0x10, 0x0020, AddressGranularity::DWord,
                                      ByteOrder::Motorola);
    session.EstablishConnection(connect);

    EXPECT_EQ(session.State(), SessionState::Connected);
    EXPECT_TRUE(session.IsConnected());
    EXPECT_EQ(session.MaxCto(), 0x10U);
    EXPECT_EQ(session.MaxDto(), 0x0020U);
    EXPECT_EQ(session.GetByteOrder(), ByteOrder::Motorola);
    EXPECT_EQ(session.GetAddressGranularity(), AddressGranularity::DWord);
    EXPECT_EQ(session.Parameters().m_connect_.m_resource_mask_, 0x15U);
    EXPECT_TRUE(
        session.Parameters().m_connect_.m_optional_comm_mode_available_);
}

TEST(SessionLifecycle, DisconnectClearsNegotiatedParameters) {
    Session session;
    Connect(session);
    GetStatusResponse status;
    status.m_state_number_ = 7;
    session.UpdateStatus(status);
    ASSERT_TRUE(session.Parameters().m_status_.has_value());

    session.BeginDisconnecting();
    EXPECT_EQ(session.State(), SessionState::Disconnecting);
    session.CompleteDisconnection();

    EXPECT_EQ(session.State(), SessionState::Disconnected);
    EXPECT_FALSE(session.IsConnected());
    EXPECT_EQ(session.MaxCto(), 0U);
    EXPECT_EQ(session.MaxDto(), 0U);
    EXPECT_FALSE(session.Parameters().m_status_.has_value());
    EXPECT_FALSE(session.HasPendingCommand());
}

TEST(SessionLifecycle, ReconnectAfterDisconnectAllowed) {
    Session session;
    Connect(session);
    session.BeginDisconnecting();
    session.CompleteDisconnection();
    EXPECT_NO_THROW(Connect(session));
    EXPECT_EQ(session.State(), SessionState::Connected);
}

// --------------------------------------------------------------------------
// 非法状态迁移
// --------------------------------------------------------------------------

TEST(SessionInvalidTransitions, CannotConnectTwice) {
    Session session;
    session.BeginConnecting();
    EXPECT_THROW(session.BeginConnecting(), XcpException);
    try {
        session.BeginConnecting();
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidState);
    }
}

TEST(SessionInvalidTransitions, CannotConnectWhenAlreadyConnected) {
    Session session;
    Connect(session);
    EXPECT_THROW(session.BeginConnecting(), XcpException);
}

TEST(SessionInvalidTransitions, EstablishRequiresConnectingState) {
    Session session;
    EXPECT_THROW(session.EstablishConnection(ValidConnect()), XcpException);
    Connect(session);
    // 已 Connected 时重复处理 CONNECT 响应同样非法
    EXPECT_THROW(session.EstablishConnection(ValidConnect()), XcpException);
}

TEST(SessionInvalidTransitions, DisconnectRequiresConnected) {
    Session session;
    EXPECT_THROW(session.BeginDisconnecting(), XcpException);
    session.BeginConnecting();
    EXPECT_THROW(session.BeginDisconnecting(), XcpException);
}

TEST(SessionInvalidTransitions, RecoveryRequiresConnected) {
    Session session;
    EXPECT_THROW(session.BeginRecovery(), XcpException);
    Connect(session);
    session.BeginRecovery();
    EXPECT_EQ(session.State(), SessionState::Recovering);
    // Recovering 中不允许再次进入恢复或断开
    EXPECT_THROW(session.BeginRecovery(), XcpException);
    EXPECT_THROW(session.BeginDisconnecting(), XcpException);
    session.CompleteRecovery();
    EXPECT_EQ(session.State(), SessionState::Connected);
}

TEST(SessionInvalidTransitions, FailedCannotSendCommands) {
    Session session;
    Connect(session);
    session.Fail("Transport 意外关闭");
    EXPECT_EQ(session.State(), SessionState::Failed);
    EXPECT_EQ(session.FailReason(), "Transport 意外关闭");
    EXPECT_FALSE(session.IsConnected());
    EXPECT_THROW(session.MarkCommandSent(CommandCode::GetStatus), XcpException);
    // Failed 必须先 reset() 才能重新连接
    EXPECT_THROW(session.BeginConnecting(), XcpException);
    session.Reset();
    EXPECT_EQ(session.State(), SessionState::Disconnected);
    EXPECT_NO_THROW(session.BeginConnecting());
}

TEST(SessionInvalidTransitions, ResetFromAnyState) {
    for (int step = 0; step < 4; ++step) {
        Session session;
        if (step >= 1) {
            session.BeginConnecting();
        }
        if (step >= 2) {
            session.EstablishConnection(ValidConnect());
        }
        if (step >= 3) {
            session.MarkCommandSent(CommandCode::Upload);
        }
        session.Reset();
        EXPECT_EQ(session.State(), SessionState::Disconnected);
        EXPECT_FALSE(session.HasPendingCommand());
        EXPECT_EQ(session.MaxCto(), 0U);
    }
}

// --------------------------------------------------------------------------
// CONNECT 参数校验（设计文档 / 计划文档 4.2）
// --------------------------------------------------------------------------

TEST(SessionValidation, RejectsMaxCtoBelowMinimum) {
    Session session;
    session.BeginConnecting();
    EXPECT_THROW(session.EstablishConnection(ValidConnect(0x07, 0x0008)),
                 XcpException);
    // 校验失败不得污染状态：仍在 Connecting，且参数未生效
    EXPECT_EQ(session.State(), SessionState::Connecting);
    EXPECT_EQ(session.MaxCto(), 0U);
}

TEST(SessionValidation, AcceptsMaxCtoAtMinimumBoundary) {
    Session session;
    Connect(session);  // 默认 MAX_CTO = 0x08
    EXPECT_EQ(session.MaxCto(), 0x08U);
}

TEST(SessionValidation, RejectsMaxDtoBelowMinimum) {
    Session session;
    session.BeginConnecting();
    EXPECT_THROW(session.EstablishConnection(ValidConnect(0x08, 0x0007)),
                 XcpException);
    EXPECT_EQ(session.State(), SessionState::Connecting);
}

TEST(SessionValidation, RejectsMaxCtoNotDivisibleByAg) {
    Session session;
    session.BeginConnecting();
    // MAX_CTO=9, AG=WORD => 9 % 2 != 0
    EXPECT_THROW(session.EstablishConnection(
                     ValidConnect(0x09, 0x0008, AddressGranularity::Word)),
                 XcpException);
}

TEST(SessionValidation, RejectsMaxDtoNotDivisibleByAg) {
    Session session;
    session.BeginConnecting();
    // MAX_CTO=8 可被 4 整除，但 MAX_DTO=12 不可被 AG=DWORD(4)? 12%4==0 -> 用 14
    EXPECT_THROW(session.EstablishConnection(
                     ValidConnect(0x08, 0x000E, AddressGranularity::DWord)),
                 XcpException);
}

TEST(SessionValidation, AcceptsLegalAgCombinations) {
    struct Case {
        std::uint8_t max_cto;
        std::uint16_t max_dto;
        AddressGranularity ag;
    };
    const std::vector<Case> cases{
        {0x08, 0x0008, AddressGranularity::Byte},
        {0x40, 0x0040, AddressGranularity::Byte},
        {0xFF, 0xFFFF, AddressGranularity::Byte},
        {0x08, 0x0008, AddressGranularity::Word},
        {0x10, 0x0100, AddressGranularity::Word},
        {0x08, 0x0008, AddressGranularity::DWord},
        {0x40, 0x1000, AddressGranularity::DWord},
    };
    for (const auto& c : cases) {
        Session session;
        session.BeginConnecting();
        session.EstablishConnection(ValidConnect(c.max_cto, c.max_dto, c.ag));
        EXPECT_TRUE(session.IsConnected())
            << "max_cto=" << c.max_cto << " dto=" << c.max_dto;
        EXPECT_EQ(session.GetAddressGranularity(), c.ag);
    }
}

TEST(SessionValidation, ValidationErrorIsInvalidArgument) {
    Session session;
    session.BeginConnecting();
    try {
        session.EstablishConnection(ValidConnect(0x01, 0x0008));
        FAIL() << "应抛出异常";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
        EXPECT_NE(std::string(e.what()).find("MAX_CTO"), std::string::npos);
    }
}

// --------------------------------------------------------------------------
// 单 Outstanding Command（Standard Communication Model）
// --------------------------------------------------------------------------

TEST(SessionPendingCommand, TracksSingleOutstandingCommand) {
    Session session;
    Connect(session);
    EXPECT_FALSE(session.PendingCommand().has_value());

    session.MarkCommandSent(CommandCode::Upload);
    EXPECT_TRUE(session.HasPendingCommand());
    EXPECT_EQ(session.PendingCommand(),
              std::optional<CommandCode>(CommandCode::Upload));

    // 第二条命令必须被拒绝
    EXPECT_THROW(session.MarkCommandSent(CommandCode::GetStatus), XcpException);
    try {
        session.MarkCommandSent(CommandCode::GetStatus);
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidState);
    }
    // 拒绝不应改变原有 Pending
    EXPECT_EQ(session.PendingCommand(),
              std::optional<CommandCode>(CommandCode::Upload));

    session.ClearPendingCommand();
    EXPECT_FALSE(session.HasPendingCommand());
    EXPECT_NO_THROW(session.MarkCommandSent(CommandCode::GetStatus));
    // clearPendingCommand 幂等
    session.ClearPendingCommand();
    session.ClearPendingCommand();
    EXPECT_FALSE(session.HasPendingCommand());
}

TEST(SessionPendingCommand, AllowedInConnectingAndRecovering) {
    Session session;
    // CONNECT 报文本身在 Connecting 状态下发送
    session.BeginConnecting();
    EXPECT_NO_THROW(session.MarkCommandSent(CommandCode::Connect));
    session.ClearPendingCommand();
    session.EstablishConnection(ValidConnect());
    session.BeginRecovery();
    EXPECT_NO_THROW(session.MarkCommandSent(CommandCode::Synch));
}

TEST(SessionPendingCommand, ClearedByFailAndReset) {
    Session session;
    Connect(session);
    session.MarkCommandSent(CommandCode::Upload);
    session.Fail("超时恢复耗尽");
    EXPECT_FALSE(session.HasPendingCommand());

    // Failed 状态必须先 reset() 才能重新发起连接（见 FailedCannotSendCommands）
    session.Reset();
    Connect(session);
    session.MarkCommandSent(CommandCode::Upload);
    session.Reset();
    EXPECT_FALSE(session.HasPendingCommand());
}

// --------------------------------------------------------------------------
// 能力降级与可选查询结果
// --------------------------------------------------------------------------

TEST(SessionParameters, OptionalQueryResultsAreAdditive) {
    Session session;
    Connect(session);

    GetCommModeInfoResponse info;
    info.m_max_bs_ = 4;
    info.m_min_st_ = 2;
    info.m_queue_size_ = 8;
    info.m_driver_version_major_ = 1;
    info.m_driver_version_minor_ = 3;
    session.UpdateCommModeInfo(info);

    GetStatusResponse status;
    status.m_daq_running_ = true;
    status.m_resource_protection_ = 0x15U;
    session.UpdateStatus(status);

    const auto params = session.Parameters();
    ASSERT_TRUE(params.m_comm_mode_info_.has_value());
    EXPECT_EQ(params.m_comm_mode_info_->m_max_bs_, 4U);
    ASSERT_TRUE(params.m_status_.has_value());
    EXPECT_TRUE(params.m_status_->m_daq_running_);
    // 快照为拷贝，修改返回值不影响内部状态
    EXPECT_TRUE(params.m_connect_.m_slave_block_mode_supported_);
}

TEST(SessionParameters, DisableShortUploadPersistsAcrossQueries) {
    Session session;
    Connect(session);
    EXPECT_TRUE(session.Parameters().m_short_upload_available_);
    session.DisableShortUpload();
    EXPECT_FALSE(session.Parameters().m_short_upload_available_);

    // 后续 GET_STATUS 更新不应意外恢复 SHORT_UPLOAD 能力
    session.UpdateStatus(GetStatusResponse{});
    EXPECT_FALSE(session.Parameters().m_short_upload_available_);

    // 只有重新建立连接才恢复能力判定
    session.BeginDisconnecting();
    session.CompleteDisconnection();
    Connect(session);
    EXPECT_TRUE(session.Parameters().m_short_upload_available_);
}

TEST(SessionParameters, ReturnedSnapshotIsImmutableCopy) {
    Session session;
    Connect(session);
    SessionParameters copy = session.Parameters();
    copy.m_connect_.m_max_cto_ = 0xEE;
    EXPECT_EQ(session.MaxCto(), 0x08U);
}

// --------------------------------------------------------------------------
// 线程安全冒烟测试（真正的竞态检测依赖 TSan，此处仅确保无死锁/崩溃）
// --------------------------------------------------------------------------

TEST(SessionThreadSafety, ConcurrentReadersAndWritersDoNotDeadlock) {
    Session session;
    Connect(session);
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&session, i] {
            for (int n = 0; n < 500; ++n) {
                if (i % 2 == 0) {
                    GetStatusResponse status;
                    status.m_state_number_ =
                        static_cast<std::uint8_t>(n & 0xFF);
                    session.UpdateStatus(status);
                } else {
                    volatile auto s = session.State();
                    (void)s;
                    (void)session.Parameters();
                }
            }
        });
    }
    for (auto& t : workers) {
        t.join();
    }
    EXPECT_EQ(session.State(), SessionState::Connected);
}

}  // namespace
}  // namespace calmcar::xcp
