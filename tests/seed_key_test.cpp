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

    /// @brief 令某命令的第 n 次调用（1 起）不产生响应，模拟超时
    /// @details 返回空 Bytes 时 MockTransport 不投递任何包，执行器等待到
    ///          超时后进入恢复流程。
    void DropNthCall(CommandCode cmd, int n) { m_drop_[cmd] = n; }

    /// @brief 令某命令自第 from 次调用（1 起）起的所有调用都不产生响应
    /// @details 与 DropNthCall 不同，可用于同一命令需要多次超时的场景
    ///          （单条目 map 会互相覆盖）。
    void DropCallsFrom(CommandCode cmd, int from) { m_drop_from_[cmd] = from; }

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
        const auto drop_it = m_drop_.find(cmd);
        if (drop_it != m_drop_.end() && m_counts_[cmd] == drop_it->second) {
            return {};  // 该次调用不回响应，模拟超时
        }
        const auto from_it = m_drop_from_.find(cmd);
        if (from_it != m_drop_from_.end() && m_counts_[cmd] >= from_it->second) {
            return {};  // 自该次起全部不回响应，模拟连续超时
        }
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
    std::map<CommandCode, int> m_drop_;
    std::map<CommandCode, int> m_drop_from_;
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
    // 批次 B1：XcpMaster::Connect 会在 GET_STATUS 之后探测 GET_VERSION。
    // 本脚本 Slave 模拟的是 **不支持该 optional 命令** 的对端（V1.3 语义），
    // 按规范要求回 ERR_CMD_UNKNOWN —— 这让连接照常成功、版本进入
    // ConnectMajor + MajorOnly 保守状态，正是要测的既有行为。
    // 若不装此默认值，QueuedSlave 会回 1 字节裸 RES，导致解析报长度不足。
    slave.SetDefault(CommandCode::GetVersion, ErrBytes(ErrorCode::CmdUnknown));
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
        rig.executor.ExecuteUnlock(0x04, BytesOf({0xAA, 0xBB, 0xCC, 0xDD}),
                                    true);

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
        (void)rig.executor.ExecuteUnlock(0x01, BytesOf({0x00}), true);
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
        XcpMasterOptions options;
        options.timeouts = CommandTimeouts{std::chrono::milliseconds(300),
                                           std::chrono::milliseconds(300), 2};
        master = std::make_unique<XcpMaster>(std::move(transport), options);
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

// ===========================================================================
// F-10：分段 Seed&Key 超时的事务级恢复
// ===========================================================================
//
// 规范对「多段序列中途超时」未给出专门处理，通用路径的「SYNCH + 重放当前
// 段」会因 Slave 侧段偏移不确定而静默失步（Key 直到序列完成后才校验，
// 原始 PDF 第 130 页）。以下用例锁定「拒绝盲重放」这一安全属性。

/// @brief 场景二：GET_SEED 续段超时 → 从 Mode=0 完整重启一次并成功
TEST(SeedKeyTimeoutRecovery, GetSeedRemainderTimeoutRestartsFromScratch) {
    MasterRig rig;
    // 第一轮：首段正常（总长 8、给 6 字节），续段超时
    rig.slave.Push(CommandCode::GetSeed,
                   BytesOf({0xFF, 0x08, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06}));
    // 第 2 次 GET_SEED（续段）超时：SYNCH 之后事务层必须回到 Mode=0
    rig.slave.DropNthCall(CommandCode::GetSeed, 2);
    // 第二轮：完整重新收满 8 字节 Seed（首段 + 续段）
    rig.slave.Push(CommandCode::GetSeed,
                   BytesOf({0xFF, 0x08, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16}));
    rig.slave.Push(CommandCode::GetSeed, BytesOf({0xFF, 0x02, 0x17, 0x18}));
    rig.slave.SetDefault(CommandCode::Unlock, BytesOf({0xFF, 0x00}));
    rig.master->Connect();

    const UnlockResult result =
        rig.master->Unlock(Resource::CalPag, test::TestKeyAlgorithm);

    // 重启后用的是第二轮 Seed 算出的 Key
    EXPECT_FALSE(result.was_already_unlocked);
    EXPECT_EQ(rig.slave.Count(CommandCode::GetSeed), 4);   // 1 + 1(超时) + 2
    EXPECT_EQ(rig.slave.Count(CommandCode::Synch), 1);     // 仅一次 SYNCH

    // SYNCH 之后的第一条 GET_SEED 必须是 Mode=0（重新开始，而非重放续段）
    const auto sent = rig.transport_ptr->SentPackets();
    std::size_t synch_at = 0;
    for (std::size_t i = 0; i < sent.size(); ++i) {
        if (!sent[i].empty() &&
            static_cast<CommandCode>(sent[i][0]) == CommandCode::Synch) {
            synch_at = i;
            break;
        }
    }
    ASSERT_GT(synch_at, 0U);
    ASSERT_GT(sent.size(), synch_at + 1);
    // [F8][Mode][Resource]：重启后必须 Mode=0
    EXPECT_EQ(sent[synch_at + 1],
              BytesOf({0xF8, 0x00, 0x01}));
}

/// @brief 场景三：UNLOCK 末段超时 → 结果未知，禁止重放，不重连
TEST(SeedKeyTimeoutRecovery, UnlockFinalSegmentTimeoutIsOutcomeUnknown) {
    MasterRig rig;
    // 单段 Seed（4 字节，一帧即可），Key 同长 → UNLOCK 只有一帧（末段）
    rig.slave.Push(CommandCode::GetSeed,
                   BytesOf({0xFF, 0x04, 0x01, 0x02, 0x03, 0x04}));
    rig.slave.SetDefault(CommandCode::Unlock, BytesOf({0xFF, 0x00}));
    rig.slave.DropNthCall(CommandCode::Unlock, 1);  // 唯一一帧即末段，超时
    rig.master->Connect();

    try {
        (void)rig.master->Unlock(Resource::CalPag, test::TestKeyAlgorithm);
        FAIL() << "UNLOCK 末段超时应抛 OperationOutcomeUnknown";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::OperationOutcomeUnknown);
        EXPECT_EQ(e.GetCommandCode(),
                  std::optional<CommandCode>(CommandCode::Unlock));
    }
    // 安全属性：末段超时绝不重放 UNLOCK
    EXPECT_EQ(rig.slave.Count(CommandCode::Unlock), 1);
    // 安全属性：也不需要 SYNCH（不重连、不重启）
    EXPECT_EQ(rig.slave.Count(CommandCode::Synch), 0);
}

/// @brief 场景四：UNLOCK 非末段超时 → 结果未知，禁止重放且不自动 SYNCH
TEST(SeedKeyTimeoutRecovery, UnlockMidSegmentTimeoutRequiresReconnect) {
    MasterRig rig;
    // Seed 16 字节 → Key 16 字节 → UNLOCK 需 3 帧；令第 2 帧（非末段）超时
    rig.slave.Push(CommandCode::GetSeed,
                   BytesOf({0xFF, 0x10, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06}));
    rig.slave.Push(CommandCode::GetSeed,
                   BytesOf({0xFF, 0x0A, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C}));
    rig.slave.Push(CommandCode::GetSeed,
                   BytesOf({0xFF, 0x04, 0x0D, 0x0E, 0x0F, 0x10}));
    rig.slave.SetDefault(CommandCode::Unlock, BytesOf({0xFF, 0x00}));
    rig.slave.DropNthCall(CommandCode::Unlock, 2);  // 第 2 帧属非末段
    rig.master->Connect();

    try {
        (void)rig.master->Unlock(Resource::CalPag, test::TestKeyAlgorithm);
        FAIL() << "UNLOCK 非末段超时应抛 OperationOutcomeUnknown";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::OperationOutcomeUnknown);
    }
    // 安全属性：超时那一段不得被重放（Slave 只看到 2 帧：首帧 + 超时帧）
    EXPECT_EQ(rig.slave.Count(CommandCode::Unlock), 2);
    // 非末段要求调用方断开重连，执行器自身不发 SYNCH
    EXPECT_EQ(rig.slave.Count(CommandCode::Synch), 0);
}

/// @brief 场景五：分段事务重启预算用尽后不再无限重启
TEST(SeedKeyTimeoutRecovery, RestartBudgetExhaustedStopsRetrying) {
    MasterRig rig;
    // 首轮：续段超时 → 触发一次重启
    rig.slave.Push(CommandCode::GetSeed,
                   BytesOf({0xFF, 0x08, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06}));
    rig.slave.DropNthCall(CommandCode::GetSeed, 2);
    // 重启轮：首段正常，续段再次超时 → 预算已用尽，必须直接失败
    rig.slave.Push(CommandCode::GetSeed,
                   BytesOf({0xFF, 0x08, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16}));
    // 第 4 次调用（重启轮的续段）同样超时；DropNthCall 单条目会覆盖前值，
    // 故这里改用「全部剩余调用都丢弃」的表述
    rig.slave.DropCallsFrom(CommandCode::GetSeed, 4);
    rig.master->Connect();

    try {
        (void)rig.master->Unlock(Resource::CalPag, test::TestKeyAlgorithm);
        FAIL() << "重启预算用尽后应抛异常而非无限重试";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::RecoveryFailed);
    }
    // 两次超时各触发一次恢复：SYNCH 至多 2 次；GET_SEED 共 4 帧
    EXPECT_EQ(rig.slave.Count(CommandCode::GetSeed), 4);
    EXPECT_LE(rig.slave.Count(CommandCode::Synch), 2);
    EXPECT_EQ(rig.slave.Count(CommandCode::Unlock), 0);
}

/// @brief 分段状态不得泄漏：一次成功解锁后，后续调用仍按 Mode=0 起手
TEST(SeedKeyTimeoutRecovery, SegmentStateDoesNotLeakAcrossCalls) {
    MasterRig rig;
    rig.slave.SetDefault(CommandCode::GetSeed,
                         BytesOf({0xFF, 0x04, 0x01, 0x02, 0x03, 0x04}));
    rig.slave.SetDefault(CommandCode::Unlock, BytesOf({0xFF, 0x00}));
    rig.master->Connect();

    (void)rig.master->Unlock(Resource::CalPag, test::TestKeyAlgorithm);
    (void)rig.master->Unlock(Resource::CalPag, test::TestKeyAlgorithm);

    // 两次调用都必须以 Mode=0 起手（第二次若沿用非 NotSegmented 状态会走错分支）
    const auto sent = rig.transport_ptr->SentPackets();
    int mode0 = 0;
    for (const auto& p : sent) {
        if (p.size() == 3U && p[0] == 0xF8U && p[1] == 0x00U) {
            ++mode0;
        }
    }
    EXPECT_EQ(mode0, 2);
    EXPECT_EQ(rig.slave.Count(CommandCode::GetSeed), 2);
}

}  // namespace
}  // namespace calmcar::xcp
