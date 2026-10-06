/**
 * @file get_daq_event_info_test.cpp
 * @brief GET_DAQ_EVENT_INFO（v0.3）的解析与执行器测试。
 *
 * 覆盖测量子系统代码增长计划 v0.3 的 L1（解析）与 L2（执行器，MockTransport
 * 脚本化 Slave）。XCPlite 实然比对（xcp.h:816-825）：
 *   CRO = [D7][事件通道 WORD@1..2]，LEN=4（**无** reserved 字节）；
 *   RES 数据（去掉 0xFF）6 字节 =
 *     [PROPERTIES][MAX_DAQ_LISTS][NAME_LENGTH][TIME_CYCLE][TIME_UNIT]
 *     [PRIORITY]；事件通道号**不在响应中回显**。
 */

#include "libxcp/command_executor.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "libxcp/protocol_types.hpp"
#include "libxcp/response_parser.hpp"
#include "libxcp/session.hpp"
#include "libxcp/xcp_error.hpp"
#include "mock_transport.hpp"

namespace calmcar::xcp {
namespace {

/// @brief 以初始列表构造 Bytes
Bytes BytesOf(std::initializer_list<std::uint8_t> init) {
    return Bytes(init.begin(), init.end());
}

/// @brief 组装 CONNECT 响应（复用 recovery_test 的实然字节）：
/// [0xFF][0x15][0xC0][0x08][0x00 0x08][0x10][0x10]（Intel/Byte/CTO8/DTO8）
Bytes MakeConnectResponse() {
    return BytesOf({static_cast<std::uint8_t>(PacketType::Res), 0x15, 0xC0,
                    0x08, 0x00, 0x08, 0x10, 0x10});
}

/// @brief 每个命令回 ERR 的通用负响应
Bytes ErrBytes(ErrorCode code) {
    return Bytes{static_cast<std::uint8_t>(PacketType::Err),
                 static_cast<std::uint8_t>(code)};
}

/**
 * @brief 脚本化测试用 Slave（命令 → 响应映射，带计数与最近 CTO 载荷记录）
 */
class ScriptedSlave {
public:
    void SetResponse(CommandCode cmd, Bytes response) {
        m_responses_[cmd] = std::move(response);
    }

    [[nodiscard]] Bytes operator()(BytesView packet) {
        if (packet.empty()) {
            return {};
        }
        const auto cmd = static_cast<CommandCode>(packet[0]);
        ++m_counts_[cmd];
        m_last_payload_[cmd] = Bytes(packet.begin(), packet.end());
        const auto it = m_responses_.find(cmd);
        if (it != m_responses_.end()) {
            return it->second;
        }
        return Bytes{static_cast<std::uint8_t>(PacketType::Res)};
    }

    [[nodiscard]] int Count(CommandCode cmd) const {
        const auto it = m_counts_.find(cmd);
        return it == m_counts_.end() ? 0 : it->second;
    }

    /// @brief 最近一次收到该命令的完整 CTO 载荷（含命令字节）
    [[nodiscard]] const Bytes& LastCto(CommandCode cmd) const {
        static const Bytes kEmpty;
        const auto it = m_last_payload_.find(cmd);
        return it == m_last_payload_.end() ? kEmpty : it->second;
    }

private:
    std::map<CommandCode, Bytes> m_responses_;
    std::map<CommandCode, int> m_counts_;
    std::map<CommandCode, Bytes> m_last_payload_;
};

/**
 * @brief 构造可执行 GET_DAQ_EVENT_INFO 的执行器脚手架（已 Connect）。
 * @note listener 传 nullptr：本用例只关心 CRO/RES 线格式，不验证 EV 事件。
 */
struct Fixture {
    Fixture() : executor(transport, session, CommandTimeouts{}, nullptr) {
        slave.SetResponse(CommandCode::Connect, MakeConnectResponse());
        slave.SetResponse(
            CommandCode::GetStatus,
            BytesOf({static_cast<std::uint8_t>(PacketType::Res), 0x00, 0x00,
                     0x01, 0x07, 0x00}));
        slave.SetResponse(
            CommandCode::GetCommModeInfo,
            BytesOf({static_cast<std::uint8_t>(PacketType::Res), 0x00, 0x0E,
                     0x00, 0x04, 0x02, 0x08, 0x13}));
        slave.SetResponse(CommandCode::Synch, ErrBytes(ErrorCode::CmdSynch));
        transport.SetResponse([this](BytesView p) { return slave(p); });
        transport.Open(executor);
        (void)executor.ExecuteConnect(0x00);
    }

    test::MockTransport transport;
    ScriptedSlave slave;
    Session session;
    CommandExecutor executor;
};

// ---------------------------------------------------------------------------
// L1：ResponseParser::ParseGetDaqEventInfo
// ---------------------------------------------------------------------------

TEST(ParseGetDaqEventInfo, ParsesSixXCpliteFields) {
    ResponseParser parser(ByteOrder::Intel);
    // [PROPERTIES=0x01][MAX_DAQ_LISTS=0xFF][NAME_LENGTH=0x05][TIME_CYCLE=100]
    // [TIME_UNIT=3][PRIORITY=0xFF]
    const auto res =
        parser.ParseGetDaqEventInfo(BytesOf({0x01, 0xFF, 0x05, 100, 3, 0xFF}));
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->properties, 0x01);
    EXPECT_EQ(res->max_daq_lists, 0xFF);
    EXPECT_EQ(res->name_length, 0x05);
    EXPECT_EQ(res->time_cycle, 100);
    EXPECT_EQ(res->time_unit, 3);
    EXPECT_EQ(res->priority, 0xFF);
}

TEST(ParseGetDaqEventInfo, TooShortReturnsNullopt) {
    ResponseParser parser(ByteOrder::Intel);
    EXPECT_FALSE(
        parser.ParseGetDaqEventInfo(BytesOf({0x01, 0xFF, 0x05})).has_value());
    EXPECT_FALSE(parser.ParseGetDaqEventInfo(BytesView{}).has_value());
}

// ---------------------------------------------------------------------------
// L2：CommandExecutor::ExecuteGetDaqEventInfo
// ---------------------------------------------------------------------------

TEST(ExecuteGetDaqEventInfo, IssuesD7WithEventChannelWordAndParsesResponse) {
    Fixture f;
    f.slave.SetResponse(
        CommandCode::GetDaqEventInfo,
        BytesOf({static_cast<std::uint8_t>(PacketType::Res), 0x01, 0xFF, 0x05,
                 100, 3, 0xFF}));

    const auto res = f.executor.ExecuteGetDaqEventInfo(0x0102);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->properties, 0x01);
    EXPECT_EQ(res->max_daq_lists, 0xFF);
    EXPECT_EQ(res->name_length, 0x05);
    EXPECT_EQ(res->time_cycle, 100);
    EXPECT_EQ(res->time_unit, 3);
    EXPECT_EQ(res->priority, 0xFF);

    // CRO 载荷（含命令字节）= [0xD7][0x00][0x02][0x01]；
    // reserved byte 在事件通道 WORD 之前（Intel LE，事件 0x0102）。
    const Bytes& sent = f.slave.LastCto(CommandCode::GetDaqEventInfo);
    ASSERT_EQ(sent.size(), 4u);
    EXPECT_EQ(sent[0], static_cast<std::uint8_t>(CommandCode::GetDaqEventInfo));
    EXPECT_EQ(sent[1], 0x00);
    EXPECT_EQ(sent[2], 0x02);
    EXPECT_EQ(sent[3], 0x01);
    EXPECT_EQ(f.slave.Count(CommandCode::GetDaqEventInfo), 1);
}

TEST(ExecuteGetDaqEventInfo, CmdUnknownReturnsNullopt) {
    Fixture f;
    f.slave.SetResponse(CommandCode::GetDaqEventInfo,
                        ErrBytes(ErrorCode::CmdUnknown));
    EXPECT_FALSE(f.executor.ExecuteGetDaqEventInfo(0x0000).has_value());
}

TEST(ExecuteGetDaqEventInfo, MalformedShortResponseThrows) {
    Fixture f;
    // RES 只给 3 字节（<6）：Exec 捕获后抛 MalformedPacket
    f.slave.SetResponse(
        CommandCode::GetDaqEventInfo,
        BytesOf({static_cast<std::uint8_t>(PacketType::Res), 0x01, 0xFF,
                 0x05}));
    EXPECT_THROW((void)f.executor.ExecuteGetDaqEventInfo(0x0000), XcpException);
}

}  // namespace
}  // namespace calmcar::xcp