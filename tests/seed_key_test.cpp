/**
 * @file seed_key_test.cpp
 * @brief Seed&Key（批次 7）的 Mock 层测试：
 *        C 层 CommandExecutor 原语 + D 层 XcpMaster::Unlock 编排。
 *
 * 端到端（真实 UDP Socket）用例见 xcp_udp_loopback_test.cpp 的
 * SeedKeyEndToEnd 组；报文黄金向量见 command_codec_test.cpp /
 * response_parser_test.cpp。 报文布局依据 docs/XCP_1.3.0_document.md 第 7.5.1.8
 * / 7.5.1.9 节， 字段顺序经 OpenBLT 与 robotjatek/XCP 双源交叉验证。
 */

#include "libxcp/command_executor.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "libxcp/session.hpp"
#include "libxcp/xcp_master.hpp"
#include "mock_transport.hpp"
#include "udp_test_slave.hpp"

namespace calmcar::xcp {
namespace {

/// @brief 由初始化列表构造 Bytes
Bytes BytesOf(std::initializer_list<std::uint8_t> init) {
    return Bytes(init.begin(), init.end());
}

/// @brief 断言报文与期望字节序列完全一致（含长度，防止多余尾字节）
void ExpectPacket(const Bytes& actual,
                  const std::vector<std::uint8_t>& expected) {
    EXPECT_EQ(actual, Bytes(expected.begin(), expected.end()));
}

/// @brief 组装 CONNECT 响应（Intel/BYTE/MAX_CTO=8/MAX_DTO=8/Optional）
Bytes MakeConnectResponse() {
    return Bytes{static_cast<std::uint8_t>(PacketType::Res),
                 0x15,
                 0x80,
                 0x08,
                 0x08,
                 0x00,
                 0x10,
                 0x10};
}

/// @brief 构造 ERR 报文
Bytes ErrBytes(ErrorCode code) {
    return Bytes{static_cast<std::uint8_t>(PacketType::Err),
                 static_cast<std::uint8_t>(code)};
}

/**
 * @brief 按命令排队响应的脚本 Slave
 *
 * GET_SEED 的分段读取需要同一命令连续返回不同的帧，故响应按先进先出队列
 * 消费；队列耗尽后回退到默认持续响应。记录每个命令的调用次数。
 */
class QueuedSlave {
public:
    /// @brief 为某命令追加一个响应（FIFO）
    void Push(CommandCode cmd, Bytes response) {
        m_queues_[cmd].push_back(std::move(response));
    }

    /// @brief 设置命令的默认持续响应（队列为空时使用）
    void SetDefault(CommandCode cmd, Bytes response) {
        m_defaults_[cmd] = std::move(response);
    }

    /// @brief 某命令被调用的次数
    [[nodiscard]] int Count(CommandCode cmd) const {
        const auto it = m_counts_.find(cmd);
        return it == m_counts_.end() ? 0 : it->second;
    }

    /// @brief MockTransport 的响应脚本入口
    Bytes operator()(BytesView packet) {
        if (packet.empty()) {
            return {};
        }
        const auto cmd = static_cast<CommandCode>(packet[0]);
        ++m_counts_[cmd];
        const auto queue_it = m_queues_.find(cmd);
        if (queue_it != m_queues_.end() && !queue_it->second.empty()) {
            Bytes front = std::move(queue_it->second.front());
            queue_it->second.pop_front();
            return front;
        }
        const auto default_it = m_defaults_.find(cmd);
        if (default_it != m_defaults_.end()) {
            return default_it->second;
        }
        return Bytes{static_cast<std::uint8_t>(PacketType::Res)};
    }

private:
    std::map<CommandCode, std::deque<Bytes>> m_queues_;
    std::map<CommandCode, Bytes> m_defaults_;
    std::map<CommandCode, int> m_counts_;
};

/// @brief 为脚本 Slave 装上标准连接期响应
void InstallSessionDefaults(QueuedSlave& slave) {
    slave.SetDefault(CommandCode::Connect, MakeConnectResponse());
    slave.SetDefault(CommandCode::GetStatus,
                     BytesOf({0xFF, 0x00, 0x00, 0x01, 0x07, 0x00}));
    slave.SetDefault(CommandCode::GetCommModeInfo,
                     BytesOf({0xFF, 0x00, 0x0E, 0x00, 0x04, 0x02, 0x08, 0x13}));
    slave.SetDefault(CommandCode::Synch, ErrBytes(ErrorCode::CmdSynch));
}

// ===========================================================================
// C 层：CommandExecutor 原语（ExecuteGetSeed / ExecuteUnlock）
// ===========================================================================

/// @brief 裸执行器脚手架（不经 XcpMaster，直接驱动单命令原语）
struct ExecutorRig {
    ExecutorRig()
        : executor(transport, session,
                   CommandTimeouts{std::chrono::milliseconds(300),
                                   std::chrono::milliseconds(300), 2}) {
        InstallSessionDefaults(slave);
        transport.SetResponse([this](BytesView p) { return slave(p); });
        transport.Open(executor);
    }

    /// @brief 走完 CONNECT，进入 Connected
    void Connect() { (void)executor.ExecuteConnect(0x00); }

    test::MockTransport transport;  ///< 确定性传输替身
    Session session;                ///< 被测会话状态机
    CommandExecutor executor;       ///< 被测命令执行器
    QueuedSlave slave;              ///< 脚本从机
};

TEST(SeedKeyExecutor, GetSeedFirstModeGolden) {
    ExecutorRig rig;
    rig.Connect();
    rig.slave.Push(CommandCode::GetSeed,
                   BytesOf({0xFF, 0x04, 0x01, 0x02, 0x03, 0x04}));

    const auto before = rig.transport.SentPackets().size();
    const GetSeedResponse result =
        rig.executor.ExecuteGetSeed(Resource::CalPag, SeedMode::First);

    EXPECT_EQ(result.length, 0x04U);
    EXPECT_EQ(result.seed, BytesOf({0x01, 0x02, 0x03, 0x04}));

    const auto sent = rig.transport.SentPackets();
    ASSERT_EQ(sent.size(), before + 1);
    ExpectPacket(sent[before], {0xF8, 0x00, 0x01});  // [F8][mode=0][CalPag]
}

TEST(SeedKeyExecutor, GetSeedRemainderModeEncoded) {
    ExecutorRig rig;
    rig.Connect();
    rig.slave.Push(CommandCode::GetSeed, BytesOf({0xFF, 0x02, 0x05, 0x06}));

    const auto before = rig.transport.SentPackets().size();
    const GetSeedResponse result =
        rig.executor.ExecuteGetSeed(Resource::Daq, SeedMode::Remainder);

    EXPECT_EQ(result.length, 0x02U);
    EXPECT_EQ(result.seed, BytesOf({0x05, 0x06}));

    const auto sent = rig.transport.SentPackets();
    ASSERT_EQ(sent.size(), before + 1);
    ExpectPacket(sent[before], {0xF8, 0x01, 0x04});  // [F8][mode=1][DAQ]
}

TEST(SeedKeyExecutor, GetSeedZeroLengthMeansUnprotected) {
    ExecutorRig rig;
    rig.Connect();
    rig.slave.Push(CommandCode::GetSeed, BytesOf({0xFF, 0x00}));

    const GetSeedResponse result =
        rig.executor.ExecuteGetSeed(Resource::Pgm, SeedMode::First);
    EXPECT_EQ(result.length, 0U);
    EXPECT_TRUE(result.seed.empty());
}

TEST(SeedKeyExecutor, GetSeedTruncatedResponseRejected) {
    ExecutorRig rig;
    rig.Connect();
    rig.slave.Push(CommandCode::GetSeed, BytesOf({0xFF}));  // 无 Length 字节

    try {
        (void)rig.executor.ExecuteGetSeed(Resource::CalPag, SeedMode::First);
        FAIL() << "截断的 GET_SEED 响应应抛 MalformedPacket";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::MalformedPacket);
    }
}

TEST(SeedKeyExecutor, UnlockSegmentGolden) {
    ExecutorRig rig;
    rig.Connect();
    rig.slave.Push(CommandCode::Unlock, BytesOf({0xFF, 0x1C}));

    const auto before = rig.transport.SentPackets().size();
    const UnlockResponse result =
        rig.executor.ExecuteUnlock(0x04, BytesOf({0xAA, 0xBB, 0xCC, 0xDD}));

    EXPECT_EQ(result.resource_protection, 0x1CU);

    const auto sent = rig.transport.SentPackets();
    ASSERT_EQ(sent.size(), before + 1);
    ExpectPacket(sent[before],
                 {0xF7, 0x04, 0xAA, 0xBB, 0xCC, 0xDD});  // [F7][len][key...]
}

TEST(SeedKeyExecutor, UnlockAccessLockedFailsSession) {
    ExecutorRig rig;
    rig.Connect();
    // Key 校验失败：ERR_ACCESS_LOCKED（规范 §7.5.1.9，Slave 随后断开）
    rig.slave.Push(CommandCode::Unlock, ErrBytes(ErrorCode::AccessLocked));

    try {
        (void)rig.executor.ExecuteUnlock(0x01, BytesOf({0x00}));
        FAIL() << "UNLOCK 收到 ERR_ACCESS_LOCKED 应抛 ProtocolError";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::ProtocolError);
        EXPECT_EQ(e.GetErrorCode(),
                  std::optional<ErrorCode>(ErrorCode::AccessLocked));
        EXPECT_EQ(e.GetCommandCode(),
                  std::optional<CommandCode>(CommandCode::Unlock));
    }
    // Slave 已主动断开：本地 Session 必须标记 Failed 且保留原因
    EXPECT_EQ(rig.session.State(), SessionState::Failed);
    EXPECT_FALSE(rig.session.FailReason().empty());
}

TEST(SeedKeyExecutor, GetSeedOutOfRangePropagates) {
    ExecutorRig rig;
    rig.Connect();
    rig.slave.Push(CommandCode::GetSeed, ErrBytes(ErrorCode::OutOfRange));

    try {
        (void)rig.executor.ExecuteGetSeed(Resource::CalPag, SeedMode::First);
        FAIL() << "应抛 ProtocolError(OutOfRange)";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::ProtocolError);
        EXPECT_EQ(e.GetErrorCode(),
                  std::optional<ErrorCode>(ErrorCode::OutOfRange));
    }
}

TEST(SeedKeyExecutor, GetSeedSequenceErrorPropagates) {
    ExecutorRig rig;
    rig.Connect();
    rig.slave.Push(CommandCode::GetSeed, ErrBytes(ErrorCode::Sequence));

    try {
        (void)rig.executor.ExecuteGetSeed(Resource::CalPag,
                                          SeedMode::Remainder);
        FAIL() << "应抛 ProtocolError(Sequence)";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::ProtocolError);
        EXPECT_EQ(e.GetErrorCode(),
                  std::optional<ErrorCode>(ErrorCode::Sequence));
    }
}

// ===========================================================================
// D 层：XcpMaster::Unlock 编排（分段、防御与本地预检）
// ===========================================================================

/// @brief Master + MockTransport + 脚本从机 的组合脚手架
struct MasterRig {
    MasterRig() {
        auto transport = std::make_unique<test::MockTransport>();
        transport_ptr = transport.get();
        InstallSessionDefaults(slave);
        transport_ptr->SetResponse([this](BytesView p) { return slave(p); });
        master = std::make_unique<XcpMaster>(
            std::move(transport),
            CommandTimeouts{std::chrono::milliseconds(300),
                            std::chrono::milliseconds(300), 2});
    }

    QueuedSlave slave;  ///< 脚本从机
    /// @brief Transport 视图（所有权已移交 master）
    test::MockTransport* transport_ptr{nullptr};
    /// @brief 被测对象
    std::unique_ptr<XcpMaster> master;
};

TEST(SeedKeyUnlock, SkipsCallbackAndCommandsWhenUnprotected) {
    MasterRig rig;
    rig.slave.Push(CommandCode::GetSeed, BytesOf({0xFF, 0x00}));
    rig.master->Connect();
    const auto before = rig.transport_ptr->SentPackets().size();

    bool callback_called = false;
    const UnlockResult result =
        rig.master->Unlock(Resource::CalPag, [&](Resource, BytesView) {
            callback_called = true;
            return Bytes{};
        });

    EXPECT_TRUE(result.was_already_unlocked);
    EXPECT_FALSE(result.resource_protection.has_value())
        << "GET_SEED 响应协议上不含保护掩码，不伪造值";
    EXPECT_FALSE(callback_called) << "Length=0 时不应调用算法回调";
    EXPECT_EQ(rig.slave.Count(CommandCode::Unlock), 0) << "不应发送 UNLOCK";

    // Unlock 期间只发出一帧 GET_SEED
    const auto sent = rig.transport_ptr->SentPackets();
    ASSERT_EQ(sent.size(), before + 1);
    ExpectPacket(sent[before], {0xF8, 0x00, 0x01});
}

TEST(SeedKeyUnlock, RejectsInvalidResourceWithoutSending) {
    MasterRig rig;
    rig.master->Connect();
    const auto before = rig.transport_ptr->SentPackets().size();
    const auto calculator = [](Resource, BytesView) { return Bytes{}; };

    // 0=无资源位、0x05=多资源位（CalPag|Daq）、0x02=保留位
    for (const auto bad :
         {static_cast<Resource>(0x00), static_cast<Resource>(0x05),
          static_cast<Resource>(0x02)}) {
        try {
            (void)rig.master->Unlock(bad, calculator);
            FAIL() << "非法 resource 应抛 InvalidArgument: "
                   << static_cast<int>(bad);
        } catch (const XcpException& e) {
            EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
        }
    }
    // 本地预检必须发生在任何发包之前
    EXPECT_EQ(rig.transport_ptr->SentPackets().size(), before);
    EXPECT_EQ(rig.slave.Count(CommandCode::GetSeed), 0);
}

TEST(SeedKeyUnlock, RejectsNullCalculator) {
    MasterRig rig;
    rig.master->Connect();
    try {
        (void)rig.master->Unlock(Resource::CalPag, nullptr);
        FAIL() << "空回调应抛 InvalidArgument";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
    }
    EXPECT_EQ(rig.slave.Count(CommandCode::GetSeed), 0);
}

TEST(SeedKeyUnlock, FullFlowSingleSegment) {
    MasterRig rig;
    rig.slave.Push(CommandCode::GetSeed,
                   BytesOf({0xFF, 0x04, 0x01, 0x02, 0x03, 0x04}));
    rig.slave.Push(CommandCode::Unlock, BytesOf({0xFF, 0x1C}));
    rig.master->Connect();
    const auto before = rig.transport_ptr->SentPackets().size();

    const UnlockResult result =
        rig.master->Unlock(Resource::CalPag, test::TestKeyAlgorithm);

    EXPECT_FALSE(result.was_already_unlocked);
    ASSERT_TRUE(result.resource_protection.has_value());
    EXPECT_EQ(*result.resource_protection, 0x1CU);
    EXPECT_EQ(rig.slave.Count(CommandCode::GetSeed), 1);
    EXPECT_EQ(rig.slave.Count(CommandCode::Unlock), 1);

    // key[i] = seed[i] ^ 0x5A ^ i（由 TestKeyAlgorithm 计算，勿手算）：
    // {01,02,03,04} -> {5B,59,5B,5D}
    const auto sent = rig.transport_ptr->SentPackets();
    ASSERT_EQ(sent.size(), before + 2);
    ExpectPacket(sent[before], {0xF8, 0x00, 0x01});
    ExpectPacket(sent[before + 1], {0xF7, 0x04, 0x5B, 0x59, 0x5B, 0x5D});
}

TEST(SeedKeyUnlock, MultiSegmentSeedAndKey) {
    MasterRig rig;
    // Seed 16 字节：MAX_CTO=8 -> 每段 6 字节 -> GET_SEED 3 帧
    // （length 字段依次为 总长16 / 剩余10 / 剩余4）
    rig.slave.Push(CommandCode::GetSeed,
                   BytesOf({0xFF, 0x10, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06}));
    rig.slave.Push(CommandCode::GetSeed,
                   BytesOf({0xFF, 0x0A, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C}));
    rig.slave.Push(CommandCode::GetSeed,
                   BytesOf({0xFF, 0x04, 0x0D, 0x0E, 0x0F, 0x10}));
    rig.slave.SetDefault(CommandCode::Unlock, BytesOf({0xFF, 0x00}));
    rig.master->Connect();
    const auto before = rig.transport_ptr->SentPackets().size();

    const Bytes expected_seed =
        BytesOf({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
                 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10});
    const Bytes expected_key =
        test::TestKeyAlgorithm(Resource::CalPag, BytesView{expected_seed});

    const UnlockResult result =
        rig.master->Unlock(Resource::CalPag, test::TestKeyAlgorithm);

    EXPECT_FALSE(result.was_already_unlocked);
    ASSERT_TRUE(result.resource_protection.has_value());
    EXPECT_EQ(*result.resource_protection, 0U);
    EXPECT_EQ(rig.slave.Count(CommandCode::GetSeed), 3);
    EXPECT_EQ(rig.slave.Count(CommandCode::Unlock), 3);

    // 完整黄金命令序列：3 帧 GET_SEED + 3 帧 UNLOCK（16 = 6+6+4）
    const auto sent = rig.transport_ptr->SentPackets();
    ASSERT_EQ(sent.size(), before + 6);
    ExpectPacket(sent[before], {0xF8, 0x00, 0x01});
    ExpectPacket(sent[before + 1], {0xF8, 0x01, 0x01});
    ExpectPacket(sent[before + 2], {0xF8, 0x01, 0x01});
    // 首帧 Length=Key 总长 16，后续帧 Length=剩余 10 / 4
    Bytes unlock_first{0xF7, 0x10};
    unlock_first.insert(unlock_first.end(), expected_key.begin(),
                        expected_key.begin() + 6);
    ExpectPacket(
        sent[before + 3],
        std::vector<std::uint8_t>(unlock_first.begin(), unlock_first.end()));
    Bytes unlock_second{0xF7, 0x0A};
    unlock_second.insert(unlock_second.end(), expected_key.begin() + 6,
                         expected_key.begin() + 12);
    ExpectPacket(
        sent[before + 4],
        std::vector<std::uint8_t>(unlock_second.begin(), unlock_second.end()));
    Bytes unlock_third{0xF7, 0x04};
    unlock_third.insert(unlock_third.end(), expected_key.begin() + 12,
                        expected_key.end());
    ExpectPacket(
        sent[before + 5],
        std::vector<std::uint8_t>(unlock_third.begin(), unlock_third.end()));
}

TEST(SeedKeyUnlock, RejectsEmptyKeyWithoutUnlock) {
    MasterRig rig;
    rig.slave.Push(CommandCode::GetSeed,
                   BytesOf({0xFF, 0x04, 0x01, 0x02, 0x03, 0x04}));
    rig.master->Connect();

    try {
        (void)rig.master->Unlock(Resource::CalPag, [](Resource, BytesView) {
            return Bytes{};
        });  // 空 Key
        FAIL() << "空 Key 应抛 InvalidArgument";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
    }
    EXPECT_EQ(rig.slave.Count(CommandCode::Unlock), 0)
        << "空 Key 不允许发出任何 UNLOCK";
}

TEST(SeedKeyUnlock, RejectsOversizedKeyWithoutUnlock) {
    MasterRig rig;
    rig.slave.Push(CommandCode::GetSeed,
                   BytesOf({0xFF, 0x04, 0x01, 0x02, 0x03, 0x04}));
    rig.master->Connect();

    try {
        (void)rig.master->Unlock(Resource::CalPag, [](Resource, BytesView) {
            return Bytes(256, 0xAA);  // 超过 Length 字段 255 上限
        });
        FAIL() << "256 字节 Key 应抛 InvalidArgument";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
    }
    EXPECT_EQ(rig.slave.Count(CommandCode::Unlock), 0);
}

TEST(SeedKeyUnlock, CallbackExceptionPropagates) {
    MasterRig rig;
    rig.slave.Push(CommandCode::GetSeed,
                   BytesOf({0xFF, 0x04, 0x01, 0x02, 0x03, 0x04}));
    rig.master->Connect();

    // 回调抛出的异常必须原样传播，不得被包装或吞掉
    EXPECT_THROW((void)rig.master->Unlock(Resource::CalPag,
                                          [](Resource, BytesView) -> Bytes {
                                              throw std::runtime_error("boom");
                                          }),
                 std::runtime_error);
    EXPECT_EQ(rig.slave.Count(CommandCode::Unlock), 0);
}

TEST(SeedKeyUnlock, MalformedEmptyRemainderSegmentRejected) {
    MasterRig rig;
    // 首帧声明总长 8 并给出 6 字节；续帧 length=2 却不带任何 Seed 字节
    rig.slave.Push(CommandCode::GetSeed,
                   BytesOf({0xFF, 0x08, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06}));
    rig.slave.Push(CommandCode::GetSeed, BytesOf({0xFF, 0x02}));
    rig.master->Connect();

    try {
        (void)rig.master->Unlock(Resource::CalPag, test::TestKeyAlgorithm);
        FAIL() << "空续段应抛 MalformedPacket（防死循环）";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::MalformedPacket);
    }
    EXPECT_EQ(rig.slave.Count(CommandCode::Unlock), 0);
}

TEST(SeedKeyUnlock, AccessLockedMarksSessionFailed) {
    MasterRig rig;
    rig.slave.Push(CommandCode::GetSeed,
                   BytesOf({0xFF, 0x04, 0x01, 0x02, 0x03, 0x04}));
    rig.slave.Push(CommandCode::Unlock, ErrBytes(ErrorCode::AccessLocked));
    rig.master->Connect();

    try {
        (void)rig.master->Unlock(Resource::CalPag, test::TestKeyAlgorithm);
        FAIL() << "Key 错误应抛 ProtocolError";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::ProtocolError);
        EXPECT_EQ(e.GetErrorCode(),
                  std::optional<ErrorCode>(ErrorCode::AccessLocked));
    }
    // Slave 已主动断开会话：本地必须停止于 Failed（再次 Connect 自动重建）
    EXPECT_EQ(rig.master->GetSessionState(), SessionState::Failed);
    EXPECT_FALSE(rig.master->IsConnected());
}

}  // namespace
}  // namespace calmcar::xcp
