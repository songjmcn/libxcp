/**
 * @file get_daq_list_mode_test.cpp
 * @brief B0 批次：GET_DAQ_LIST_MODE(0xDF) 四层实现验收。
 *
 * 覆盖 Codec 字节布局、Parser 字段/端序/短帧、Executor 的 Optional 降级，
 * 以及 XcpMaster::QueryDaqListMode 的快照组装。重点锁三件事：
 *   1) CTO 为 [DF][reserved][DAQ(WORD)]，端序随会话；
 *   2) RES 七个字段按规范 Table 150 切分，缺失即判畸形；
 *   3) 响应的 mode 是**状态位**（SELECTED/RUNNING/RESUME），与
 *      SET_DAQ_LIST_MODE 的请求位（ALTERNATING）语义不同，不能原样回填。
 */

#include <gtest/gtest.h>

#include <chrono>
#include <initializer_list>
#include <memory>
#include <vector>

#include "libxcp/command_codec.hpp"
#include "libxcp/protocol_types.hpp"
#include "libxcp/response_parser.hpp"
#include "libxcp/xcp_master.hpp"
#include "mock_transport.hpp"

namespace calmcar::xcp {
namespace {

using test::MockTransport;

/// @brief 由初始化列表构造字节串（测试便捷函数）
Bytes BytesOf(std::initializer_list<std::uint8_t> init) { return Bytes(init); }

/// @brief 断言实际字节与期望完全一致
void ExpectBytes(const Bytes& actual,
                 const std::vector<std::uint8_t>& expected) {
    ASSERT_EQ(actual.size(), expected.size()) << "字节数不一致";
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(actual[i], expected[i]) << "第 " << i << " 字节不一致";
    }
}

/// @brief 构造 RES 报文（首字节 0xFF）
Bytes Res(const std::vector<std::uint8_t>& body) {
    Bytes out;
    out.reserve(1 + body.size());
    out.push_back(static_cast<std::uint8_t>(PacketType::Res));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

/// @brief 构造 ERR 报文（首字节 0xFE + 错误码）
Bytes Err(ErrorCode code) {
    return Bytes{static_cast<std::uint8_t>(PacketType::Err),
                 static_cast<std::uint8_t>(code)};
}

// ---------------------------------------------------------------------------
// 1. Codec：CTO 布局与端序
// ---------------------------------------------------------------------------

TEST(GetDaqListModeCodec, IssuesReservedByteThenDaqWord) {
    const CommandCodec intel(ByteOrder::Intel);
    ExpectBytes(intel.EncodeGetDaqListMode(7), {0xDF, 0x00, 0x07, 0x00});

    const CommandCodec motorola(ByteOrder::Motorola);
    ExpectBytes(motorola.EncodeGetDaqListMode(7), {0xDF, 0x00, 0x00, 0x07});
}

TEST(GetDaqListModeCodec, KeepsDaqListNumberRange) {
    const CommandCodec codec(ByteOrder::Intel);
    ExpectBytes(codec.EncodeGetDaqListMode(0x1234),
                {0xDF, 0x00, 0x34, 0x12});
}

// ---------------------------------------------------------------------------
// 2. Parser：七字段切分、端序与短帧
// ---------------------------------------------------------------------------

TEST(GetDaqListModeParser, SplitsStatusEventPrescalerPriority) {
    const ResponseParser parser(ByteOrder::Intel);
    // [Current Mode=0x41 (RUNNING|SELECTED)][reserved=0x0000]
    // [Event Channel=0x0002][Prescaler=0x01][Priority=0x00]
    const auto parsed = parser.ParseGetDaqListMode(
        BytesOf({0x41, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00}));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->mode, 0x41U);
    EXPECT_TRUE(HasDaqStatus(static_cast<DaqListStatusBit>(parsed->mode),
                             DaqListStatusBit::kRunning));
    EXPECT_TRUE(HasDaqStatus(static_cast<DaqListStatusBit>(parsed->mode),
                             DaqListStatusBit::kSelected));
    EXPECT_FALSE(HasDaqStatus(static_cast<DaqListStatusBit>(parsed->mode),
                              DaqListStatusBit::kResume));
    EXPECT_EQ(parsed->event_channel, 0x0002U);
    EXPECT_EQ(parsed->prescaler, 0x01U);
    EXPECT_EQ(parsed->priority, 0x00U);
}

TEST(GetDaqListModeParser, ReadsWordInMotorolaOrder) {
    const ResponseParser parser(ByteOrder::Motorola);
    const auto parsed = parser.ParseGetDaqListMode(
        BytesOf({0x02, 0x00, 0x00, 0x01, 0x23, 0x02, 0x05}));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->mode, 0x02U);
    EXPECT_EQ(parsed->event_channel, 0x0123U);
    EXPECT_EQ(parsed->prescaler, 0x02U);
    EXPECT_EQ(parsed->priority, 0x05U);
}

TEST(GetDaqListModeParser, RejectsTruncatedResponse) {
    const ResponseParser parser(ByteOrder::Intel);
    // 六个字段齐全但少一个字节 → 畸形（不是"字段默认 0"）
    EXPECT_FALSE(
        parser.ParseGetDaqListMode(BytesOf({0x41, 0x00, 0x00, 0x02, 0x00, 0x01}))
            .has_value());
    EXPECT_FALSE(parser.ParseGetDaqListMode(BytesView{}).has_value());
}

TEST(GetDaqListModeParser, StatusBitsAreNotSetModeBits) {
    // 规范事实：bit0 在 SET 请求里是 ALTERNATING，在 GET 响应里是 SELECTED；
    // 二者数值相同但语义不同，必须由不同枚举承载，禁止互转后回填。
    EXPECT_EQ(static_cast<std::uint8_t>(DaqListStatusBit::kSelected),
              static_cast<std::uint8_t>(DaqListModeBit::kAlternating));
    // 响应独有状态位
    EXPECT_EQ(static_cast<std::uint8_t>(DaqListStatusBit::kRunning), 0x40U);
    EXPECT_EQ(static_cast<std::uint8_t>(DaqListStatusBit::kResume), 0x80U);
}

// ---------------------------------------------------------------------------
// 3. Master：支持时返回快照、不支持时返回 nullopt
// ---------------------------------------------------------------------------

/// @brief 最小 Slave：只答 CONNECT 与 GET_DAQ_LIST_MODE
struct ModeSlave {
    std::uint8_t mode{0x41};          ///< 回给 Master 的 Current Mode
    std::uint16_t event_channel{2};   ///< 回给 Master 的 Event Channel
    std::uint8_t prescaler{1};        ///< 回给 Master 的 Prescaler
    std::uint8_t priority{0};         ///< 回给 Master 的 Priority
    bool supported{true};             ///< false 时回 ERR_CMD_UNKNOWN
    int query_count{0};               ///< GET_DAQ_LIST_MODE 被调次数

    Bytes operator()(BytesView packet) {
        if (packet.empty()) {
            return {};
        }
        const auto cmd = static_cast<CommandCode>(packet[0]);
        switch (cmd) {
            case CommandCode::Connect:
                // COMM_MODE_BASIC + reserved + MAX_CTO(0x10) + MAX_DTO(0x08)
                // + PROTOCOL_LAYER + TRANSPORT_LAYER
                return Res({0x15, 0xC0, 0x10, 0x08, 0x00, 0x10, 0x10});
            case CommandCode::GetStatus:
                // [当前会话状态][reserved][保护状态][会话状态][reserved]
                return Res({0x00, 0x00, 0x01, 0x07, 0x00});
            case CommandCode::GetDaqListMode:
                ++query_count;
                if (!supported) {
                    return Err(ErrorCode::CmdUnknown);
                }
                return Res({mode, 0x00, 0x00,
                            static_cast<std::uint8_t>(event_channel & 0xFFU),
                            static_cast<std::uint8_t>((event_channel >> 8U) &
                                                      0xFFU),
                            prescaler, priority});
            default:
                // 其余命令在本测试用例中不应出现；按规范回 ERR_CMD_UNKNOWN
                return Err(ErrorCode::CmdUnknown);
        }
    }
};

/// @brief 夹具：MockTransport + 已连接的 XcpMaster
struct Rig {
    Rig() {
        auto transport_owner = std::make_unique<MockTransport>();
        transport = transport_owner.get();
        transport->SetResponse([this](BytesView p) { return slave(p); });
        XcpMasterOptions options;
        options.timeouts = CommandTimeouts{std::chrono::milliseconds(150),
                                           std::chrono::milliseconds(150), 2};
        master = std::make_unique<XcpMaster>(std::move(transport_owner), options);
        master->Connect();
    }

    ModeSlave slave;
    MockTransport* transport{nullptr};
    std::unique_ptr<XcpMaster> master;
};

TEST(XcpMasterGetDaqListMode, ReturnsSlaveStateSnapshot) {
    Rig rig;
    rig.slave.mode = 0x41;  // RUNNING | SELECTED
    rig.slave.event_channel = 0x002A;
    rig.slave.prescaler = 3;
    rig.slave.priority = 7;

    const auto snapshot = rig.master->QueryDaqListMode(5);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->daq_list, 5U) << "入参回填，不由 Slave 响应回显";
    EXPECT_EQ(snapshot->mode, 0x41U);
    EXPECT_TRUE(HasDaqStatus(static_cast<DaqListStatusBit>(snapshot->mode),
                             DaqListStatusBit::kRunning));
    EXPECT_EQ(snapshot->event_channel, 0x002AU);
    EXPECT_EQ(snapshot->prescaler, 3U);
    EXPECT_EQ(snapshot->priority, 7U);
    EXPECT_EQ(rig.slave.query_count, 1);
}

TEST(XcpMasterGetDaqListMode, ReturnsNulloptOnCmdUnknown) {
    Rig rig;
    rig.slave.supported = false;
    const auto snapshot = rig.master->QueryDaqListMode(5);
    EXPECT_FALSE(snapshot.has_value())
        << "Optional 命令不支持时必须返回无值，不得用本地期望值补空";
    EXPECT_EQ(rig.slave.query_count, 1) << "仍应实际发出一次查询";
}

TEST(XcpMasterGetDaqListMode, EmptyEventChannelIsPreserved) {
    // 未绑定事件通道的 Slave 会回 0；这是有效观测，不是“缺失”信号。
    Rig rig;
    rig.slave.event_channel = 0;
    rig.slave.prescaler = 0;
    const auto snapshot = rig.master->QueryDaqListMode(0);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->daq_list, 0U);
    EXPECT_EQ(snapshot->event_channel, 0U);
    EXPECT_EQ(snapshot->prescaler, 0U);
}

}  // namespace
}  // namespace calmcar::xcp