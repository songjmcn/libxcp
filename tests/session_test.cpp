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
    resp.resourceMask = 0x15U;
    resp.byteOrder = byte_order;
    resp.addressGranularity = ag;
    resp.slaveBlockModeSupported = true;
    resp.optionalCommModeAvailable = true;
    resp.maxCto = max_cto;
    resp.maxDto = max_dto;
    resp.protocolLayerVersion = 0x10;
    resp.transportLayerVersion = 0x10;
    return resp;
}

/// @brief 走完 Disconnected -> Connecting -> Connected
void Connect(Session& session) {
    session.beginConnecting();
    session.establishConnection(ValidConnect());
}

// --------------------------------------------------------------------------
// 初始状态与默认参数
// --------------------------------------------------------------------------

TEST(SessionInitial, StartsDisconnectedWithDefaults) {
    Session session;
    EXPECT_EQ(session.state(), SessionState::Disconnected);
    EXPECT_FALSE(session.isConnected());
    EXPECT_FALSE(session.hasPendingCommand());
    EXPECT_EQ(session.maxCto(), 0U);
    EXPECT_EQ(session.maxDto(), 0U);
    EXPECT_EQ(session.byteOrder(), ByteOrder::Intel);
    EXPECT_EQ(session.addressGranularity(), AddressGranularity::Byte);
    EXPECT_TRUE(session.failReason().empty());
    // 未连接时不应认为 SHORT_UPLOAD 已被验证可用，但字段保持默认值不影响流程
    EXPECT_TRUE(session.parameters().shortUploadAvailable);
    EXPECT_FALSE(session.parameters().commModeInfo.has_value());
    EXPECT_FALSE(session.parameters().status.has_value());
}

// --------------------------------------------------------------------------
// 正常连接流程
// --------------------------------------------------------------------------

TEST(SessionLifecycle, ConnectEstablishesParameters) {
    Session session;
    session.beginConnecting();
    EXPECT_EQ(session.state(), SessionState::Connecting);

    const auto connect = ValidConnect(0x10, 0x0020, AddressGranularity::DWord,
                                      ByteOrder::Motorola);
    session.establishConnection(connect);

    EXPECT_EQ(session.state(), SessionState::Connected);
    EXPECT_TRUE(session.isConnected());
    EXPECT_EQ(session.maxCto(), 0x10U);
    EXPECT_EQ(session.maxDto(), 0x0020U);
    EXPECT_EQ(session.byteOrder(), ByteOrder::Motorola);
    EXPECT_EQ(session.addressGranularity(), AddressGranularity::DWord);
    EXPECT_EQ(session.parameters().connect.resourceMask, 0x15U);
    EXPECT_TRUE(session.parameters().connect.optionalCommModeAvailable);
}

TEST(SessionLifecycle, DisconnectClearsNegotiatedParameters) {
    Session session;
    Connect(session);
    GetStatusResponse status;
    status.stateNumber = 7;
    session.updateStatus(status);
    ASSERT_TRUE(session.parameters().status.has_value());

    session.beginDisconnecting();
    EXPECT_EQ(session.state(), SessionState::Disconnecting);
    session.completeDisconnection();

    EXPECT_EQ(session.state(), SessionState::Disconnected);
    EXPECT_FALSE(session.isConnected());
    EXPECT_EQ(session.maxCto(), 0U);
    EXPECT_EQ(session.maxDto(), 0U);
    EXPECT_FALSE(session.parameters().status.has_value());
    EXPECT_FALSE(session.hasPendingCommand());
}

TEST(SessionLifecycle, ReconnectAfterDisconnectAllowed) {
    Session session;
    Connect(session);
    session.beginDisconnecting();
    session.completeDisconnection();
    EXPECT_NO_THROW(Connect(session));
    EXPECT_EQ(session.state(), SessionState::Connected);
}

// --------------------------------------------------------------------------
// 非法状态迁移
// --------------------------------------------------------------------------

TEST(SessionInvalidTransitions, CannotConnectTwice) {
    Session session;
    session.beginConnecting();
    EXPECT_THROW(session.beginConnecting(), XcpException);
    try {
        session.beginConnecting();
    } catch (const XcpException& e) {
        EXPECT_EQ(e.category(), ErrorCategory::InvalidState);
    }
}

TEST(SessionInvalidTransitions, CannotConnectWhenAlreadyConnected) {
    Session session;
    Connect(session);
    EXPECT_THROW(session.beginConnecting(), XcpException);
}

TEST(SessionInvalidTransitions, EstablishRequiresConnectingState) {
    Session session;
    EXPECT_THROW(session.establishConnection(ValidConnect()), XcpException);
    Connect(session);
    // 已 Connected 时重复处理 CONNECT 响应同样非法
    EXPECT_THROW(session.establishConnection(ValidConnect()), XcpException);
}

TEST(SessionInvalidTransitions, DisconnectRequiresConnected) {
    Session session;
    EXPECT_THROW(session.beginDisconnecting(), XcpException);
    session.beginConnecting();
    EXPECT_THROW(session.beginDisconnecting(), XcpException);
}

TEST(SessionInvalidTransitions, RecoveryRequiresConnected) {
    Session session;
    EXPECT_THROW(session.beginRecovery(), XcpException);
    Connect(session);
    session.beginRecovery();
    EXPECT_EQ(session.state(), SessionState::Recovering);
    // Recovering 中不允许再次进入恢复或断开
    EXPECT_THROW(session.beginRecovery(), XcpException);
    EXPECT_THROW(session.beginDisconnecting(), XcpException);
    session.completeRecovery();
    EXPECT_EQ(session.state(), SessionState::Connected);
}

TEST(SessionInvalidTransitions, FailedCannotSendCommands) {
    Session session;
    Connect(session);
    session.fail("Transport 意外关闭");
    EXPECT_EQ(session.state(), SessionState::Failed);
    EXPECT_EQ(session.failReason(), "Transport 意外关闭");
    EXPECT_FALSE(session.isConnected());
    EXPECT_THROW(session.markCommandSent(CommandCode::GetStatus), XcpException);
    // Failed 必须先 reset() 才能重新连接
    EXPECT_THROW(session.beginConnecting(), XcpException);
    session.reset();
    EXPECT_EQ(session.state(), SessionState::Disconnected);
    EXPECT_NO_THROW(session.beginConnecting());
}

TEST(SessionInvalidTransitions, ResetFromAnyState) {
    for (int step = 0; step < 4; ++step) {
        Session session;
        if (step >= 1) {
            session.beginConnecting();
        }
        if (step >= 2) {
            session.establishConnection(ValidConnect());
        }
        if (step >= 3) {
            session.markCommandSent(CommandCode::Upload);
        }
        session.reset();
        EXPECT_EQ(session.state(), SessionState::Disconnected);
        EXPECT_FALSE(session.hasPendingCommand());
        EXPECT_EQ(session.maxCto(), 0U);
    }
}

// --------------------------------------------------------------------------
// CONNECT 参数校验（设计文档 / 计划文档 4.2）
// --------------------------------------------------------------------------

TEST(SessionValidation, RejectsMaxCtoBelowMinimum) {
    Session session;
    session.beginConnecting();
    EXPECT_THROW(session.establishConnection(ValidConnect(0x07, 0x0008)),
                 XcpException);
    // 校验失败不得污染状态：仍在 Connecting，且参数未生效
    EXPECT_EQ(session.state(), SessionState::Connecting);
    EXPECT_EQ(session.maxCto(), 0U);
}

TEST(SessionValidation, AcceptsMaxCtoAtMinimumBoundary) {
    Session session;
    Connect(session);  // 默认 MAX_CTO = 0x08
    EXPECT_EQ(session.maxCto(), 0x08U);
}

TEST(SessionValidation, RejectsMaxDtoBelowMinimum) {
    Session session;
    session.beginConnecting();
    EXPECT_THROW(session.establishConnection(ValidConnect(0x08, 0x0007)),
                 XcpException);
    EXPECT_EQ(session.state(), SessionState::Connecting);
}

TEST(SessionValidation, RejectsMaxCtoNotDivisibleByAg) {
    Session session;
    session.beginConnecting();
    // MAX_CTO=9, AG=WORD => 9 % 2 != 0
    EXPECT_THROW(session.establishConnection(
                     ValidConnect(0x09, 0x0008, AddressGranularity::Word)),
                 XcpException);
}

TEST(SessionValidation, RejectsMaxDtoNotDivisibleByAg) {
    Session session;
    session.beginConnecting();
    // MAX_CTO=8 可被 4 整除，但 MAX_DTO=12 不可被 AG=DWORD(4)? 12%4==0 -> 用 14
    EXPECT_THROW(session.establishConnection(
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
        session.beginConnecting();
        session.establishConnection(ValidConnect(c.max_cto, c.max_dto, c.ag));
        EXPECT_TRUE(session.isConnected())
            << "max_cto=" << c.max_cto << " dto=" << c.max_dto;
        EXPECT_EQ(session.addressGranularity(), c.ag);
    }
}

TEST(SessionValidation, ValidationErrorIsInvalidArgument) {
    Session session;
    session.beginConnecting();
    try {
        session.establishConnection(ValidConnect(0x01, 0x0008));
        FAIL() << "应抛出异常";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.category(), ErrorCategory::InvalidArgument);
        EXPECT_NE(std::string(e.what()).find("MAX_CTO"), std::string::npos);
    }
}

// --------------------------------------------------------------------------
// 单 Outstanding Command（Standard Communication Model）
// --------------------------------------------------------------------------

TEST(SessionPendingCommand, TracksSingleOutstandingCommand) {
    Session session;
    Connect(session);
    EXPECT_FALSE(session.pendingCommand().has_value());

    session.markCommandSent(CommandCode::Upload);
    EXPECT_TRUE(session.hasPendingCommand());
    EXPECT_EQ(session.pendingCommand(),
              std::optional<CommandCode>(CommandCode::Upload));

    // 第二条命令必须被拒绝
    EXPECT_THROW(session.markCommandSent(CommandCode::GetStatus), XcpException);
    try {
        session.markCommandSent(CommandCode::GetStatus);
    } catch (const XcpException& e) {
        EXPECT_EQ(e.category(), ErrorCategory::InvalidState);
    }
    // 拒绝不应改变原有 Pending
    EXPECT_EQ(session.pendingCommand(),
              std::optional<CommandCode>(CommandCode::Upload));

    session.clearPendingCommand();
    EXPECT_FALSE(session.hasPendingCommand());
    EXPECT_NO_THROW(session.markCommandSent(CommandCode::GetStatus));
    // clearPendingCommand 幂等
    session.clearPendingCommand();
    session.clearPendingCommand();
    EXPECT_FALSE(session.hasPendingCommand());
}

TEST(SessionPendingCommand, AllowedInConnectingAndRecovering) {
    Session session;
    // CONNECT 报文本身在 Connecting 状态下发送
    session.beginConnecting();
    EXPECT_NO_THROW(session.markCommandSent(CommandCode::Connect));
    session.clearPendingCommand();
    session.establishConnection(ValidConnect());
    session.beginRecovery();
    EXPECT_NO_THROW(session.markCommandSent(CommandCode::Synch));
}

TEST(SessionPendingCommand, ClearedByFailAndReset) {
    Session session;
    Connect(session);
    session.markCommandSent(CommandCode::Upload);
    session.fail("超时恢复耗尽");
    EXPECT_FALSE(session.hasPendingCommand());

    // Failed 状态必须先 reset() 才能重新发起连接（见 FailedCannotSendCommands）
    session.reset();
    Connect(session);
    session.markCommandSent(CommandCode::Upload);
    session.reset();
    EXPECT_FALSE(session.hasPendingCommand());
}

// --------------------------------------------------------------------------
// 能力降级与可选查询结果
// --------------------------------------------------------------------------

TEST(SessionParameters, OptionalQueryResultsAreAdditive) {
    Session session;
    Connect(session);

    GetCommModeInfoResponse info;
    info.maxBs = 4;
    info.minSt = 2;
    info.queueSize = 8;
    info.driverVersionMajor = 1;
    info.driverVersionMinor = 3;
    session.updateCommModeInfo(info);

    GetStatusResponse status;
    status.daqRunning = true;
    status.resourceProtection = 0x15U;
    session.updateStatus(status);

    const auto params = session.parameters();
    ASSERT_TRUE(params.commModeInfo.has_value());
    EXPECT_EQ(params.commModeInfo->maxBs, 4U);
    ASSERT_TRUE(params.status.has_value());
    EXPECT_TRUE(params.status->daqRunning);
    // 快照为拷贝，修改返回值不影响内部状态
    EXPECT_TRUE(params.connect.slaveBlockModeSupported);
}

TEST(SessionParameters, DisableShortUploadPersistsAcrossQueries) {
    Session session;
    Connect(session);
    EXPECT_TRUE(session.parameters().shortUploadAvailable);
    session.disableShortUpload();
    EXPECT_FALSE(session.parameters().shortUploadAvailable);

    // 后续 GET_STATUS 更新不应意外恢复 SHORT_UPLOAD 能力
    session.updateStatus(GetStatusResponse{});
    EXPECT_FALSE(session.parameters().shortUploadAvailable);

    // 只有重新建立连接才恢复能力判定
    session.beginDisconnecting();
    session.completeDisconnection();
    Connect(session);
    EXPECT_TRUE(session.parameters().shortUploadAvailable);
}

TEST(SessionParameters, ReturnedSnapshotIsImmutableCopy) {
    Session session;
    Connect(session);
    SessionParameters copy = session.parameters();
    copy.connect.maxCto = 0xEE;
    EXPECT_EQ(session.maxCto(), 0x08U);
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
                    status.stateNumber = static_cast<std::uint8_t>(n & 0xFF);
                    session.updateStatus(status);
                } else {
                    volatile auto s = session.state();
                    (void)s;
                    (void)session.parameters();
                }
            }
        });
    }
    for (auto& t : workers) {
        t.join();
    }
    EXPECT_EQ(session.state(), SessionState::Connected);
}

}  // namespace
}  // namespace calmcar::xcp
