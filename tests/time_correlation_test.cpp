/**
 * @file time_correlation_test.cpp
 * @brief 批次 B3：基础时间关联（GET_DAQ_CLOCK / TIME_CORRELATION_PROPERTIES）
 *
 * 锁五件事：
 *   1) GET_DAQ_CLOCK 的请求是单字节，legacy 与 extended 是**两种布局**，
 *      不能用"先试长的再退回短的"猜测；
 *   2) PAYLOAD_FMT 中保留值 3 必须判为不可解析，不能当成 DWORD 或 DLONG；
 *   3) TIME_CORRELATION_PROPERTIES 的 SET_PROPERTIES 位打包，以及
 *      "RESPONSE_FMT=0 表示不改变"导致**无法降级回 legacy**；
 *   4) 查询**只能**把 ERR_CMD_UNKNOWN 降级为 nullopt，其余错误上抛；
 *   5) 上传时钟信息块要求 AG=BYTE 且必须先查询，否则拒绝而不是读越界。
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "libxcp/command_codec.hpp"
#include "libxcp/protocol_types.hpp"
#include "libxcp/response_parser.hpp"
#include "libxcp/xcp_error.hpp"
#include "libxcp/xcp_master.hpp"
#include "mock_transport.hpp"

namespace calmcar::xcp {
namespace {

using test::MockTransport;

/// @brief 把初始化列表转成报文容器（便于断言线格式）
Bytes BytesOf(std::initializer_list<std::uint8_t> values) {
    return Bytes(values.begin(), values.end());
}

/// @brief 构造正响应报文（自动加 0xFF 前缀）
Bytes Res(const std::vector<std::uint8_t>& body) {
    Bytes out;
    out.reserve(1 + body.size());
    out.push_back(static_cast<std::uint8_t>(PacketType::Res));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

/// @brief 构造只携带错误码的负响应报文
Bytes Err(ErrorCode code) {
    return Bytes{static_cast<std::uint8_t>(PacketType::Err),
                 static_cast<std::uint8_t>(code)};
}

/**
 * @brief 只实现 B3 相关命令的脚本化 Slave
 * @details `TIME_CORRELATION_PROPERTIES` 的响应内容可配，用来模拟
 *          "Slave 静默忽略不支持的格式位"；`GET_DAQ_CLOCK` 的原始字节也可配，
 *          让同一台 Slave 在不同用例里给出 legacy/extended 两种载荷。
 */
class ClockSlave {
public:
    /// @brief 构造
    /// @param comm_mode_basic CONNECT 的 COMM_MODE_BASIC（字节序/AG/特性位）
    explicit ClockSlave(std::uint8_t comm_mode_basic = 0x00U,
                        std::uint8_t max_cto = 0x10U)
        : m_comm_mode_basic_(comm_mode_basic), m_max_cto_(max_cto) {}

    /// @brief 设定 TIME_CORRELATION_PROPERTIES 的响应字节（剥掉 0xFF 后）
    void SetCorrelationBody(std::vector<std::uint8_t> body) {
        m_correlation_body_ = std::move(body);
    }

    /// @brief 设定 GET_DAQ_CLOCK 的响应字节（剥掉 0xFF 后）
    void SetClockBody(std::vector<std::uint8_t> body) {
        m_clock_body_ = std::move(body);
    }

    /// @brief 设定 UPLOAD 返回的字节
    void SetUploadBody(std::vector<std::uint8_t> body) {
        m_upload_body_ = std::move(body);
    }

    /// @brief 让指定命令持续回某个错误码
    void FailWith(CommandCode cmd, ErrorCode code) { m_errors_[cmd] = code; }

    /// @brief 收到的帧（按到达顺序）
    [[nodiscard]] const std::vector<Bytes>& Received() const {
        return m_received_;
    }

    /// @brief 某个命令被收到的次数
    [[nodiscard]] int Count(CommandCode cmd) const {
        int n = 0;
        for (const auto& packet : m_received_) {
            if (!packet.empty() && static_cast<CommandCode>(packet[0]) == cmd) {
                ++n;
            }
        }
        return n;
    }

    /// @brief 第 index 次收到的指定命令报文（不存在时返回空）
    [[nodiscard]] Bytes FrameOf(CommandCode cmd, std::size_t index) const {
        std::size_t seen = 0;
        for (const auto& packet : m_received_) {
            if (!packet.empty() && static_cast<CommandCode>(packet[0]) == cmd) {
                if (seen == index) {
                    return packet;
                }
                ++seen;
            }
        }
        return {};
    }

    /// @brief MockTransport 的响应脚本入口
    Bytes operator()(BytesView packet) {
        if (packet.empty()) {
            return {};
        }
        m_received_.emplace_back(packet.begin(), packet.end());
        const auto cmd = static_cast<CommandCode>(packet[0]);
        const auto err_it = m_errors_.find(cmd);
        if (err_it != m_errors_.end()) {
            return Err(err_it->second);
        }
        switch (cmd) {
            case CommandCode::Connect:
                return Res({0x15, m_comm_mode_basic_, m_max_cto_, 0x08, 0x00,
                            0x10, 0x10});
            case CommandCode::Disconnect:
            case CommandCode::SetMta:
                return Res({});
            case CommandCode::GetStatus:
                return Res({0x00, 0x00, 0x01, 0x07, 0x00});
            case CommandCode::TimeCorrelationProperties:
                return Res(m_correlation_body_);
            case CommandCode::GetDaqClock:
                return Res(m_clock_body_);
            case CommandCode::Upload:
                return Res(m_upload_body_);
            default:
                return Err(ErrorCode::CmdUnknown);
        }
    }

private:
    std::uint8_t m_comm_mode_basic_{0x00U};  ///< CONNECT 的 COMM_MODE_BASIC
    std::uint8_t m_max_cto_{0x10U};          ///< CONNECT 的 MAX_CTO
    std::vector<Bytes> m_received_;          ///< 收到的原始帧
    std::map<CommandCode, ErrorCode> m_errors_;  ///< 注入的错误码
    std::vector<std::uint8_t> m_correlation_body_{0x00, 0x00, 0x00, 0x00,
                                                  0x00, 0x00, 0x00};
    std::vector<std::uint8_t> m_clock_body_{0x00, 0x00, 0x00, 0x00, 0x00,
                                            0x00};
    std::vector<std::uint8_t> m_upload_body_;
};

/// @brief Codec / Parser 层断言用的最小上下文
struct CodecFixture {
    explicit CodecFixture(ByteOrder order = ByteOrder::Intel,
                          AddressGranularity ag = AddressGranularity::Byte)
        : codec(order, ag), parser(order, ag) {}

    CommandCodec codec;     ///< 被测编码器
    ResponseParser parser;  ///< 被测解析器
};

// ======================= Codec 线格式 =======================

TEST(TimeCorrelation, GetDaqClockFrameIsSingleByte) {
    CodecFixture f;
    // 规范 Table 132：CTO 只有命令码本身，没有任何参数
    EXPECT_EQ(f.codec.EncodeGetDaqClock(), BytesOf({0xDC}));
}

TEST(TimeCorrelation, QueryFrameKeepsAllChangeBitsZero) {
    CodecFixture f;
    const Bytes cto = f.codec.EncodeTimeCorrelationProperties(0x00U, 0x00U, 0U);
    // [C6][SET=0][GET=0][reserved=0][cluster(WORD)]
    ASSERT_EQ(cto.size(), 6U);
    EXPECT_EQ(cto[0], 0xC6U);
    EXPECT_EQ(cto[1], 0x00U);
    EXPECT_EQ(cto[2], 0x00U);
    EXPECT_EQ(cto[3], 0x00U);
    EXPECT_EQ(cto[4], 0x00U);
    EXPECT_EQ(cto[5], 0x00U);
}

TEST(TimeCorrelation, SetPropertiesBitPacking) {
    // bit0-1 RESPONSE_FMT、bit2-3 TIME_SYNC_BRIDGE、bit4 SET_CLUSTER_ID
    EXPECT_EQ(EncodeTimeCorrelationSetProperties(
                  ResponseFormatMode::Legacy, TimeSyncBridgeRequest::NoChange,
                  false),
              0x00U);
    EXPECT_EQ(EncodeTimeCorrelationSetProperties(
                  ResponseFormatMode::AllTriggers, TimeSyncBridgeRequest::NoChange,
                  false),
              0x02U);
    EXPECT_EQ(EncodeTimeCorrelationSetProperties(
                  ResponseFormatMode::AllTriggers, TimeSyncBridgeRequest::Enable,
                  true),
              0x16U);  // 0x02 | (0x01 << 2) | 0x10
    EXPECT_EQ(EncodeTimeCorrelationSetProperties(
                  ResponseFormatMode::TriggerInitiatorZeroTwoThree,
                  TimeSyncBridgeRequest::Disable, false),
              0x09U);  // 0x01 | (0x02 << 2)
}

TEST(TimeCorrelation, SetRequestCarriesClusterId) {
    CodecFixture f;
    const std::uint8_t set =
        EncodeTimeCorrelationSetProperties(ResponseFormatMode::AllTriggers,
                                          TimeSyncBridgeRequest::NoChange, true);
    const Bytes cto =
        f.codec.EncodeTimeCorrelationProperties(set, 0x01U, 0x1234U);
    ASSERT_EQ(cto.size(), 6U);
    EXPECT_EQ(cto[1], 0x12U);  // AllTriggers(2) | SET_CLUSTER_ID(0x10)
    EXPECT_EQ(cto[2], 0x01U);  // GET_CLK_INFO
    EXPECT_EQ(cto[4], 0x34U);  // Intel 小端
    EXPECT_EQ(cto[5], 0x12U);
}

// ======================= Parser：GET_DAQ_CLOCK =======================

TEST(TimeCorrelation, LegacyClockLayout) {
    CodecFixture f;
    // [reserved][TRIGGER_INFO][PAYLOAD_FMT][DWORD]
    const Bytes res = BytesOf({0x00, 0x0AU, 0x21U, 0x78, 0x56, 0x34, 0x12});
    const auto parsed = f.parser.ParseGetDaqClock(res, false);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->extended);
    EXPECT_EQ(parsed->trigger_info, 0x0AU);
    EXPECT_EQ(parsed->payload_fmt, 0x21U);
    EXPECT_EQ(parsed->slave_clock, 0x12345678U);
    EXPECT_EQ(parsed->slave_clock_bytes, 4U);
    EXPECT_FALSE(parsed->grandmaster_clock.has_value());
    EXPECT_FALSE(parsed->sync_state.has_value());
}

TEST(TimeCorrelation, ExtendedClockLayoutWithGrandmaster) {
    CodecFixture f;
    // PAYLOAD_FMT: FMT_XCP_SLV=1(DWORD), FMT_GRANDM=1(DWORD) → 0x05
    const Bytes res = BytesOf({0x00, 0x0AU, 0x05U,
                               0x78, 0x56, 0x34, 0x12,  // slave
                               0x01, 0x00, 0x00, 0x00,  // grandmaster
                               0x21U});                  // SYNC_STATE
    const auto parsed = f.parser.ParseGetDaqClock(res, true);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->extended);
    EXPECT_EQ(parsed->slave_clock, 0x12345678U);
    ASSERT_TRUE(parsed->grandmaster_clock.has_value());
    EXPECT_EQ(*parsed->grandmaster_clock, 0x00000001U);
    EXPECT_FALSE(parsed->ecu_clock.has_value());
    ASSERT_TRUE(parsed->sync_state.has_value());
    EXPECT_EQ(*parsed->sync_state, 0x21U);
}

TEST(TimeCorrelation, ExtendedClockWithDlongSlaveClock) {
    CodecFixture f;
    // FMT_XCP_SLV = 2 (DLONG, 8 字节)，无 grandmaster/ECU
    const Bytes res = BytesOf({0x00, 0x00, 0x02U, 0xEF, 0xCD, 0xAB, 0x89,
                               0x67, 0x45, 0x23, 0x01});
    const auto parsed = f.parser.ParseGetDaqClock(res, true);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->slave_clock, 0x0123456789ABCDEFULL);
    EXPECT_EQ(parsed->slave_clock_bytes, 8U);
}

TEST(TimeCorrelation, ReservedPayloadFormatIsNotGuessed) {
    CodecFixture f;
    // FMT_XCP_SLV = 3（保留）：不得按 DWORD 或 DLONG 硬解
    const Bytes res = BytesOf({0x00, 0x00, 0x03U, 0x78, 0x56, 0x34, 0x12});
    EXPECT_FALSE(f.parser.ParseGetDaqClock(res, true).has_value());
}

TEST(TimeCorrelation, ShortClockResponseIsNullopt) {
    CodecFixture f;
    const Bytes res = BytesOf({0x00, 0x00, 0x01U, 0x78, 0x56, 0x34});
    EXPECT_FALSE(f.parser.ParseGetDaqClock(res, false).has_value());
    EXPECT_FALSE(f.parser.ParseGetDaqClock(res, true).has_value());
}

// ======================= Parser：TIME_CORRELATION_PROPERTIES =======================

TEST(TimeCorrelation, CorrelationResponseParse) {
    CodecFixture f;
    // SLAVE_CONFIG=0x0E：RESPONSE_FMT=2(AllTriggers)、DAQ_TS_RELATION=1(ECU)、
    //                   TIME_SYNC_BRIDGE=1(有但禁用)
    // OBSERVABLE_CLOCKS=0x69：SLV=1(MaybeSynchronized)、GRANDM=2(EventOnly)、
    //                         ECU=1(RandomReadable)
    // SYNC_STATE=0x1F：SLV=7? → 用 0x13 让 SLV=3(Syntonized)、GRANDM=0、ECU=1
    const Bytes res = BytesOf({0x0EU, 0x69U, 0x13U, 0x1BU, 0x00U, 0x34U, 0x12U});
    const auto parsed = f.parser.ParseTimeCorrelationProperties(res);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->slave_config, 0x0EU);
    EXPECT_EQ(parsed->observable_clocks, 0x69U);
    EXPECT_EQ(parsed->sync_state, 0x13U);
    EXPECT_EQ(parsed->clock_info, 0x1BU);
    EXPECT_EQ(parsed->cluster_id, 0x1234U);
}

TEST(TimeCorrelation, CorrelationFieldsDecodeAsSpecified) {
    // 单独验证解码器，避免只测"字节原样搬运"
    EXPECT_EQ(ResponseFormatModeOf(0x0E), ResponseFormatMode::AllTriggers);
    EXPECT_EQ(DaqTimestampRelationOf(0x0E), DaqTimestampRelation::EcuClock);
    EXPECT_EQ(TimeSyncBridgeStateOf(0x0E), TimeSyncBridgeState::Disabled);

    EXPECT_EQ(SlaveClockObservabilityOf(0x69),
              SlaveClockObservability::MaybeSynchronized);
    EXPECT_EQ(GrandmasterClockObservabilityOf(0x69),
              GrandmasterClockObservability::EventOnly);
    // 0x69 >> 4 = 6, 6 & 3 = 2 → ECU 只能靠事件获得
    EXPECT_EQ(EcuClockObservabilityOf(0x69),
              EcuClockObservability::EventOnly);

    EXPECT_EQ(SlaveClockSyncStateOf(0x13), SlaveClockSyncState::Syntonized);
    EXPECT_EQ(GrandmasterSyncStateOf(0x13),
              GrandmasterSyncState::NotSynchronized);
    EXPECT_EQ(EcuSyncStateOf(0x13), EcuSyncState::Synchronized);
}

TEST(TimeCorrelation, ShortCorrelationResponseIsNullopt) {
    CodecFixture f;
    // 6 字节：少 Cluster ID 的一个字节
    const Bytes res = BytesOf({0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    EXPECT_FALSE(f.parser.ParseTimeCorrelationProperties(res).has_value());
}

// ======================= Parser：EV_TIME_SYNC =======================

TEST(TimeCorrelation, TimeSyncEventLegacyLayout) {
    CodecFixture f;
    const Bytes res = BytesOf({0x00, 0x00, 0x78, 0x56, 0x34, 0x12});
    const auto parsed =
        f.parser.ParseTimeSyncEvent(res, TimeSyncLayout::Legacy);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->extended);
    EXPECT_EQ(*parsed->slave_clock, 0x12345678U);
}

TEST(TimeCorrelation, TimeSyncEventExtendedMaxCto8HasNoPayloadFormat) {
    CodecFixture f;
    // [TRIGGER_INFO][counter][DWORD]：第二字节是计数器，**不是** PAYLOAD_FMT
    const Bytes res = BytesOf({0x0AU, 0x05U, 0x78, 0x56, 0x34, 0x12});
    const auto parsed =
        f.parser.ParseTimeSyncEvent(res, TimeSyncLayout::ExtendedMaxCto8);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->counter.has_value());
    EXPECT_EQ(*parsed->counter, 0x05U);
    EXPECT_EQ(*parsed->slave_clock, 0x12345678U);
    EXPECT_EQ(parsed->payload_fmt, 0x00U);
}

TEST(TimeCorrelation, TimeSyncEventExtendedPayloadFormat) {
    CodecFixture f;
    // PAYLOAD_FMT = 0x41：FMT_XCP_SLV=1(DWORD) + CLUSTER_IDENTIFIER
    const Bytes res = BytesOf({0x0AU, 0x41U,
                               0x78, 0x56, 0x34, 0x12,  // slave
                               0x34, 0x12,              // cluster id
                               0x07U,                   // counter
                               0x01U});                 // SYNC_STATE
    const auto parsed =
        f.parser.ParseTimeSyncEvent(res, TimeSyncLayout::ExtendedPayloadFormat);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->extended);
    ASSERT_TRUE(parsed->cluster_id.has_value());
    EXPECT_EQ(*parsed->cluster_id, 0x1234U);
    ASSERT_TRUE(parsed->counter.has_value());
    EXPECT_EQ(*parsed->counter, 0x07U);
    ASSERT_TRUE(parsed->sync_state.has_value());
    EXPECT_EQ(*parsed->sync_state, 0x01U);
}

TEST(TimeCorrelation, ClockInfoTotalBytesSumsSelectedBlocks) {
    ClockCorrelationSnapshot snapshot;
    EXPECT_EQ(snapshot.ClockInfoTotalBytes(), 0U);

    snapshot.clock_info_blocks = ClockInfoBlock::kSlaveClock;
    EXPECT_EQ(snapshot.ClockInfoTotalBytes(), 18U);

    snapshot.clock_info_blocks = ClockInfoBlock::kSlaveClock |
                                 ClockInfoBlock::kClockRelation;
    EXPECT_EQ(snapshot.ClockInfoTotalBytes(), 34U);

    snapshot.clock_info_blocks = ClockInfoBlock::kEcuGrandmasterClock;
    EXPECT_EQ(snapshot.ClockInfoTotalBytes(), 8U);

    // 全选：18 + 18 + 16 + 18 + 8 = 78
    snapshot.clock_info_blocks =
        ClockInfoBlock::kSlaveClock | ClockInfoBlock::kGrandmasterClock |
        ClockInfoBlock::kClockRelation | ClockInfoBlock::kEcuClock |
        ClockInfoBlock::kEcuGrandmasterClock;
    EXPECT_EQ(snapshot.ClockInfoTotalBytes(), 78U);
}

// ======================= Master 编排 =======================

/**
 * @brief B3 编排测试的公共装置
 * @details CONNECT 用 `comm_mode_basic` 控制 AG 等特性位；默认 0x00 表示
 *          Intel + AG=BYTE 且不声明可选特性，避免 CAPL 之外的额外查询。
 */
struct ClockHarness {
    /// @brief 构造并完成 CONNECT
    explicit ClockHarness(std::uint8_t comm_mode_basic = 0x00U)
        : transport(std::make_unique<MockTransport>()),
          slave(comm_mode_basic) {
        transport->SetResponse([this](BytesView packet) { return slave(packet); });
        master = std::make_unique<XcpMaster>(std::move(transport));
        master->Connect();
    }

    std::unique_ptr<MockTransport> transport;  ///< 传输（交给 Master 持有）
    ClockSlave slave;                          ///< 脚本化 Slave
    std::unique_ptr<XcpMaster> master;         ///< 被测对象
};

TEST(TimeCorrelation, QueryReportsSnapshotFromResponse) {
    ClockHarness h;
    h.slave.SetCorrelationBody({0x0EU, 0x69U, 0x13U, 0x1BU, 0x00U, 0x34U, 0x12U});

    const auto snapshot = h.master->QueryClockCorrelation();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->response_format, ResponseFormatMode::AllTriggers);
    EXPECT_EQ(snapshot->timestamp_relation, DaqTimestampRelation::EcuClock);
    EXPECT_EQ(snapshot->bridge, TimeSyncBridgeState::Disabled);
    EXPECT_EQ(snapshot->clock_info_blocks, static_cast<ClockInfoBlock>(0x1B));
    EXPECT_EQ(snapshot->cluster_id, 0x1234U);

    // 纯查询：SET_PROPERTIES 与 GET_PROPERTIES_REQUEST 必须都是 0
    const Bytes frame =
        h.slave.FrameOf(CommandCode::TimeCorrelationProperties, 0U);
    ASSERT_EQ(frame.size(), 6U);
    EXPECT_EQ(frame[1], 0x00U);
    EXPECT_EQ(frame[2], 0x00U);
}

TEST(TimeCorrelation, QueryNulloptOnlyWhenCommandUnknown) {
    ClockHarness h;
    // 显式让 Slave 回“命令未知”，验证降级为 nullopt
    h.slave.FailWith(CommandCode::TimeCorrelationProperties,
                     ErrorCode::CmdUnknown);
    EXPECT_FALSE(h.master->QueryClockCorrelation().has_value());

    ClockHarness h2;
    h2.slave.FailWith(CommandCode::TimeCorrelationProperties,
                      ErrorCode::OutOfRange);
    // 事件通道不存在之类的错误必须上抛，不能吞成"不支持"
    EXPECT_THROW(static_cast<void>(h2.master->QueryClockCorrelation()), XcpException);
}

TEST(TimeCorrelation, SetClockResponseFormatUpgradesAndReportsReadback) {
    ClockHarness h;
    h.slave.SetCorrelationBody({0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U});

    // Slave 采纳 AllTriggers 后回读应为 0x02
    h.slave.SetCorrelationBody({0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U});
    const auto snapshot =
        h.master->SetClockResponseFormat(ResponseFormatMode::AllTriggers);
    EXPECT_EQ(snapshot.response_format, ResponseFormatMode::AllTriggers);

    // 帧序：0 = SetClockResponseFormat 内部的前置查询，1 = 真正下发修改
    const Bytes frame =
        h.slave.FrameOf(CommandCode::TimeCorrelationProperties, 1U);
    ASSERT_EQ(frame.size(), 6U);
    EXPECT_EQ(frame[1], 0x02U);
    EXPECT_EQ(frame[2], 0x00U);
}

TEST(TimeCorrelation, SetClockResponseFormatRejectsLegacyLocally) {
    ClockHarness h;
    // RESPONSE_FMT=0 的含义是"不改变"，不存在"改回 legacy"的取值
    try {
        static_cast<void>(h.master->SetClockResponseFormat(ResponseFormatMode::Legacy));
        FAIL() << "应当拒绝降级到 legacy";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
    }
    // 本地拒绝：一帧都不应发出
    EXPECT_EQ(h.slave.Count(CommandCode::TimeCorrelationProperties), 0);
}

TEST(TimeCorrelation, SetClockResponseFormatRejectsUnsupportedCommand) {
    ClockHarness h;
    h.slave.FailWith(CommandCode::TimeCorrelationProperties,
                     ErrorCode::CmdUnknown);
    try {
        static_cast<void>(h.master->SetClockResponseFormat(ResponseFormatMode::AllTriggers));
        FAIL() << "应当报不支持";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::UnsupportedFeature);
    }
}

TEST(TimeCorrelation, SetClockResponseFormatRejectsSilentlyIgnoredRequest) {
    ClockHarness h;
    h.slave.SetCorrelationBody({0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U});
    static_cast<void>(h.master->QueryClockCorrelation());
    // Slave 保持 legacy：命令被接受但格式未变，绝不能报成功
    try {
        static_cast<void>(h.master->SetClockResponseFormat(ResponseFormatMode::AllTriggers));
        FAIL() << "回读格式与请求不一致时不能报成功";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::UnsupportedFeature);
    }
}

TEST(TimeCorrelation, ReadDaqClockUsesLegacyByDefault) {
    ClockHarness h;
    h.slave.SetClockBody({0x00U, 0x0AU, 0x21U, 0x78U, 0x56U, 0x34U, 0x12U});
    const auto parsed = h.master->ReadDaqClock();
    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->extended);
    EXPECT_EQ(parsed->slave_clock, 0x12345678U);
}

TEST(TimeCorrelation, ReadDaqClockFollowsNegotiatedFormat) {
    ClockHarness h;
    h.slave.SetCorrelationBody({0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U});
    static_cast<void>(h.master->QueryClockCorrelation());

    // 协商为 extended 后，即使没有 SYNC_STATE 也应按 extended 解析
    h.slave.SetClockBody({0x00U, 0x00U, 0x01U, 0x78U, 0x56U, 0x34U, 0x12U});
    const auto parsed = h.master->ReadDaqClock();
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->extended);
    EXPECT_EQ(parsed->slave_clock, 0x12345678U);
}

TEST(TimeCorrelation, ReadDaqClockNulloptWhenCommandUnknown) {
    ClockHarness h;
    h.slave.FailWith(CommandCode::GetDaqClock, ErrorCode::CmdUnknown);
    EXPECT_FALSE(h.master->ReadDaqClock().has_value());
}

TEST(TimeCorrelation, ReadDaqClockPropagatesTemporarilyNotAccessible) {
    ClockHarness h;
    h.slave.FailWith(CommandCode::GetDaqClock,
                     ErrorCode::ResourceTemporaryNotAccessible);
    // "此刻读不到"不是"不支持"：不能降级成 nullopt
    EXPECT_THROW(static_cast<void>(h.master->ReadDaqClock()), XcpException);
}

TEST(TimeCorrelation, UploadClockInfoRequiresPriorQuery) {
    ClockHarness h;
    try {
        static_cast<void>(h.master->UploadClockInfo());
        FAIL() << "未查询时间关联状态前不得上传";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidState);
    }
    EXPECT_EQ(h.slave.Count(CommandCode::TimeCorrelationProperties), 0);
}

TEST(TimeCorrelation, UploadClockInfoRejectsNonByteGranularity) {
    // COMM_MODE_BASIC bit1-2 = 10 → AG = DWORD（0x04）
    ClockHarness h(0x04U);
    h.slave.SetCorrelationBody({0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U});
    static_cast<void>(h.master->QueryClockCorrelation());
    try {
        static_cast<void>(h.master->UploadClockInfo());
        FAIL() << "AG > BYTE 时不得按字节读取数据块";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::UnsupportedFeature);
    }
    // 拒绝发生在下发之前：只有查询帧，没有 GET_CLK_INFO 与 UPLOAD
    EXPECT_EQ(h.slave.Count(CommandCode::TimeCorrelationProperties), 1);
    EXPECT_EQ(h.slave.Count(CommandCode::Upload), 0);
}

TEST(TimeCorrelation, UploadClockInfoReadsExactBlockBytes) {
    ClockHarness h;
    // CLOCK_INFO=0x01 → 只有 XCP Slave 时钟信息块（18 字节）
    h.slave.SetCorrelationBody({0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U});
    h.slave.SetUploadBody(std::vector<std::uint8_t>(18U, 0x5AU));
    static_cast<void>(h.master->QueryClockCorrelation());

    const auto bytes = h.master->UploadClockInfo();
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(bytes->size(), 18U);
    EXPECT_EQ((*bytes)[0], 0x5AU);

    // 第二帧必须是带 GET_CLK_INFO 的 TIME_CORRELATION_PROPERTIES
    const Bytes frame =
        h.slave.FrameOf(CommandCode::TimeCorrelationProperties, 1U);
    ASSERT_EQ(frame.size(), 6U);
    EXPECT_EQ(frame[1], 0x00U);  // 不改变格式
    EXPECT_EQ(frame[2], 0x01U);  // GET_CLK_INFO

    // UPLOAD 的计数应等于 18（AG=BYTE 时元素数==字节数）
    const Bytes upload = h.slave.FrameOf(CommandCode::Upload, 0U);
    ASSERT_FALSE(upload.empty());
    EXPECT_EQ(upload[0], 0xF5U);
}

TEST(TimeCorrelation, UploadClockInfoNulloptWhenCommandUnsupported) {
    ClockHarness h;
    h.slave.FailWith(CommandCode::TimeCorrelationProperties,
                     ErrorCode::CmdUnknown);
    static_cast<void>(h.master->QueryClockCorrelation());
    // 查询过但不支持：应给出 nullopt 而不是抛"未查询"
    EXPECT_FALSE(h.master->UploadClockInfo().has_value());
}

TEST(TimeCorrelation, LegacyAndExtendedLayoutsAreExplicitlyDifferent) {
    // 同一段字节在两种布局下解释不同：legacy 第 2 字节是 PAYLOAD_FMT，
    // MAX_CTO=8 的 extended 第 2 字节是 counter。用枚举而不是 bool 正是为
    // 了让调用方必须表态，这里固化该差异。
    CodecFixture f;
    const Bytes res = BytesOf({0x0AU, 0x05U, 0x78, 0x56, 0x34, 0x12});

    const auto as_max8 =
        f.parser.ParseTimeSyncEvent(res, TimeSyncLayout::ExtendedMaxCto8);
    ASSERT_TRUE(as_max8.has_value());
    EXPECT_EQ(*as_max8->counter, 0x05U);
    EXPECT_EQ(*as_max8->slave_clock, 0x12345678U);

    // 若误按"含 PAYLOAD_FMT"解析：0x05 → FMT_XCP_SLV=1、FMT_GRANDM=1，
    // 需要 8 字节以上时间戳，必然判为长度不足
    EXPECT_FALSE(
        f.parser.ParseTimeSyncEvent(res, TimeSyncLayout::ExtendedPayloadFormat)
            .has_value());
}

}  // namespace
}  // namespace calmcar::xcp
