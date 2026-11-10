/**
 * @file daq_packed_mode_test.cpp
 * @brief B4 批次：DAQ Packed Mode（0xC0,0x01 / 0xC0,0x02）编码、解析、
 *        Master 协商与测量会话采样拆分测试。
 *
 * 覆盖计划 §3 B4 的验收项：
 *   - 编码层：SET_DAQ_PACKED_MODE 8 字节布局（含 WORD 采样数）、
 *     GET_DAQ_PACKED_MODE 4 字节布局、采样数 0/1 与保留时间戳语义拒绝、
 *     mode=0 时尾部三字节必须为 0；
 *   - 解析层：mode=0 两字节响应、mode=1/2 五字节响应、未定义 mode 与
 *     保留时间戳位拒绝、短帧 nullopt、Motorola 端序；
 *   - Master：GET 回读、SET 必须回读一致才 active、列表/事件未声明
 *     PACKED 或非周期事件时拒绝、CanUseDaqPackedMode 取证；
 *   - 规划层：packed 时单 ODT 容量按 采样数 倍折算、stride 分别为
 *     Entry 宽度（element-grouped）与 ODT 单采样宽度（event-grouped）、
 *     非法采样数报 DaqConfigurationError；
 *   - 会话层：一个 packed DTO 拆成 S 个 MeasurementFrame（sample_index /
 *     sample_count / timestamp_source），按事件周期 × 降频因子重建
 *     非锚点采样时间戳，锚点采样保留 DTO 原始时间戳。
 *
 * 规范来源：docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.md
 *   §4.1.10（packed 概念/时间戳重建）、§7.5.4.7（SET_DAQ_PACKED_MODE）、
 *   §7.5.4.8（GET_DAQ_PACKED_MODE）、Table 172（DAQ_LIST_PROPERTIES
 *   bit4 PACKED）、Table 155（DAQ_EVENT_PROPERTIES bit4 PACKED）。
 */

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "libxcp/command_codec.hpp"
#include "libxcp/daq/dto_envelope_types.hpp"
#include "libxcp/measurement/measurement_database.hpp"
#include "libxcp/measurement/measurement_planner.hpp"
#include "libxcp/measurement/measurement_result.hpp"
#include "libxcp/measurement/measurement_sample.hpp"
#include "libxcp/measurement/measurement_session.hpp"
#include "libxcp/protocol_types.hpp"
#include "libxcp/response_parser.hpp"
#include "libxcp/xcp_error.hpp"
#include "libxcp/xcp_master.hpp"
#include "mock_transport.hpp"

namespace calmcar::xcp {
namespace {

using test::MockTransport;

// ---------------------------------------------------------------------------
// 通用小工具
// ---------------------------------------------------------------------------

/// @brief 构造字节序列
Bytes BytesOf(std::initializer_list<std::uint8_t> bytes) {
    return Bytes{bytes};
}

/// @brief 构造正响应报文（自动补 0xFF PID）
Bytes Res(const std::vector<std::uint8_t>& body) {
    Bytes out;
    out.reserve(1 + body.size());
    out.push_back(static_cast<std::uint8_t>(PacketType::Res));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

/// @brief 构造负响应报文
Bytes Err(ErrorCode code) {
    return Bytes{static_cast<std::uint8_t>(PacketType::Err),
                 static_cast<std::uint8_t>(code)};
}

/// @brief 编码 + 解析共用夹具
struct CodecFixture {
    CommandCodec codec{ByteOrder::Intel};
    ResponseParser parser{ByteOrder::Intel};
};

// ---------------------------------------------------------------------------
// 编码层
// ---------------------------------------------------------------------------

TEST(DaqPackedModeCodec, SetFrameIsEightBytesInIntelOrder) {
    CodecFixture fx;
    const Bytes frame = fx.codec.EncodeSetDaqPackedMode(
        0x1234U, DaqPackedMode::EventGrouped,
        DaqPackedTimestampMode::FirstSample, 7U);
    // [C0][01][DAQ_LIST WORD][MODE][TS_MODE][SAMPLE_COUNT WORD]
    EXPECT_EQ(frame, BytesOf({0xC0U, 0x01U, 0x34U, 0x12U, 0x02U, 0x01U, 0x07U,
                              0x00U}));
}

TEST(DaqPackedModeCodec, SetFrameFollowsMotorolaOrder) {
    CommandCodec codec{ByteOrder::Motorola};
    const Bytes frame = codec.EncodeSetDaqPackedMode(
        0x1234U, DaqPackedMode::ElementGrouped,
        DaqPackedTimestampMode::LastSample, 0x0102U);
    EXPECT_EQ(frame, BytesOf({0xC0U, 0x01U, 0x12U, 0x34U, 0x01U, 0x00U, 0x01U,
                              0x02U}));
}

TEST(DaqPackedModeCodec, GetFrameIsFourBytes) {
    CodecFixture fx;
    EXPECT_EQ(fx.codec.EncodeGetDaqPackedMode(0x1234U),
              BytesOf({0xC0U, 0x02U, 0x34U, 0x12U}));
}

TEST(DaqPackedModeCodec, RejectsSampleCountBelowTwo) {
    CodecFixture fx;
    // 规范：DPM_SAMPLE_COUNT 合法范围 [2, 0xFFFF]，0/1 非法
    EXPECT_THROW(static_cast<void>(fx.codec.EncodeSetDaqPackedMode(
                     0U, DaqPackedMode::ElementGrouped,
                     DaqPackedTimestampMode::LastSample, 0U)),
                 XcpException);
    EXPECT_THROW(static_cast<void>(fx.codec.EncodeSetDaqPackedMode(
                     0U, DaqPackedMode::ElementGrouped,
                     DaqPackedTimestampMode::LastSample, 1U)),
                 XcpException);
}

TEST(DaqPackedModeCodec, RejectsReservedTimestampMode) {
    CodecFixture fx;
    EXPECT_THROW(static_cast<void>(fx.codec.EncodeSetDaqPackedMode(
                     0U, DaqPackedMode::ElementGrouped,
                     static_cast<DaqPackedTimestampMode>(0x02U), 2U)),
                 XcpException);
}

TEST(DaqPackedModeCodec, NotPackedZeroesTrailingBytes) {
    CodecFixture fx;
    const Bytes frame = fx.codec.EncodeSetDaqPackedMode(
        0x0007U, DaqPackedMode::NotPacked, DaqPackedTimestampMode::LastSample,
        0U);
    ASSERT_EQ(frame.size(), 8U);
    EXPECT_EQ(frame[4], 0x00U);  // mode = 0
    EXPECT_EQ(frame[5], 0x00U);  // ts_mode
    EXPECT_EQ(frame[6], 0x00U);  // sample count low
    EXPECT_EQ(frame[7], 0x00U);  // sample count high
}

// ---------------------------------------------------------------------------
// 解析层
// ---------------------------------------------------------------------------

TEST(DaqPackedModeParser, NotPackedResponseIsTwoBytes) {
    CodecFixture fx;
    const auto parsed =
        fx.parser.ParseDaqPackedMode(BytesOf({0x00U, 0x00U}));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->mode, DaqPackedMode::NotPacked);
    EXPECT_EQ(parsed->sample_count, 0U);
}

TEST(DaqPackedModeParser, PackedResponseCarriesTimestampAndCount) {
    CodecFixture fx;
    // RES 去掉 PID：[reserved][MODE][TS_MODE][SAMPLE_COUNT WORD]
    const auto parsed =
        fx.parser.ParseDaqPackedMode(BytesOf({0x00U, 0x01U, 0x01U, 0x07U,
                                              0x00U}));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->mode, DaqPackedMode::ElementGrouped);
    EXPECT_EQ(parsed->timestamp_mode, DaqPackedTimestampMode::FirstSample);
    EXPECT_EQ(parsed->sample_count, 7U);
}

TEST(DaqPackedModeParser, PackedResponseFollowsMotorolaOrder) {
    CommandCodec codec{ByteOrder::Motorola};
    ResponseParser parser{ByteOrder::Motorola};
    const auto parsed =
        parser.ParseDaqPackedMode(BytesOf({0x00U, 0x01U, 0x00U, 0x01U, 0x02U}));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->sample_count, 0x0102U);
}

TEST(DaqPackedModeParser, RejectsUndefinedMode) {
    CodecFixture fx;
    EXPECT_FALSE(fx.parser.ParseDaqPackedMode(BytesOf({0x00U, 0x03U})).has_value());
}

TEST(DaqPackedModeParser, RejectsReservedTimestampMode) {
    CodecFixture fx;
    EXPECT_FALSE(fx.parser
                     .ParseDaqPackedMode(BytesOf({0x00U, 0x01U, 0x02U, 0x02U,
                                                  0x00U}))
                     .has_value());
}

TEST(DaqPackedModeParser, ShortPackedResponseIsNullopt) {
    CodecFixture fx;
    // mode=1 需要 5 字节，只给 4 字节
    EXPECT_FALSE(
        fx.parser.ParseDaqPackedMode(BytesOf({0x00U, 0x01U, 0x00U, 0x02U}))
            .has_value());
}

// ---------------------------------------------------------------------------
// Master 协商层
// ---------------------------------------------------------------------------

/// @brief 脚本化 Slave：可配置列表/事件属性与 Packed 回读行为
class PackedMasterSlave {
 public:
    Bytes operator()(BytesView packet) {
        if (packet.empty()) {
            return {};
        }
        const auto cmd = static_cast<CommandCode>(packet[0]);
        ++m_counts_[cmd];
        switch (cmd) {
            case CommandCode::Connect:
                return Res({0x15, 0x00, 0x10, 0x08, 0x00, 0x10, 0x10});
            case CommandCode::GetStatus:
                return Res({0x00, 0x00, 0x01, 0x07, 0x00});
            case CommandCode::GetDaqListInfo:
                // [PROPERTIES][MAX_ODT][MAX_ODT_ENTRIES][FIXED_EVENT WORD]
                return Res({list_properties, 0x01, 0x04, 0x00, 0x00});
            case CommandCode::GetDaqEventInfo:
                // [PROPERTIES][MAX_DAQ_LISTS][NAME_LEN][TIME_CYCLE]
                // [TIME_UNIT][PRIORITY]
                return Res({event_properties, 0xFF, 0x00, time_cycle, 0x03,
                            0x00});
            case CommandCode::GetVersion:
                if (packet.size() >= 2U && packet[1] == 0x01U) {
                    return SetPackedResponse();
                }
                if (packet.size() >= 2U && packet[1] == 0x02U) {
                    return GetPackedResponse();
                }
                return Err(ErrorCode::CmdUnknown);
            default:
                return Err(ErrorCode::CmdUnknown);
        }
    }

    [[nodiscard]] int Count(CommandCode cmd) const {
        const auto it = m_counts_.find(cmd);
        return it == m_counts_.end() ? 0 : it->second;
    }

    /// @brief 列表属性（bit4 = PACKED）
    std::uint8_t list_properties{0x10U};
    /// @brief 事件属性（bit4 = PACKED，bit0-2 含 DAQ/STIM）
    std::uint8_t event_properties{0x10U};
    /// @brief 事件周期（0 = 非周期，packed 不允许）
    std::uint8_t time_cycle{1U};
    /// @brief SET 请求是否被采纳（false 模拟 Slave 忽略或拒绝）
    bool accept_set{true};
    /// @brief GET 回读的 mode / ts / S
    std::uint8_t report_mode{0x01U};
    std::uint8_t report_ts{0x00U};
    std::uint16_t report_sample_count{4U};

 private:
    Bytes SetPackedResponse() {
        if (!accept_set) {
            return Err(ErrorCode::ModeNotValid);
        }
        return Res({});
    }

    Bytes GetPackedResponse() {
        Bytes body;
        body.push_back(0x00U);  // reserved
        body.push_back(report_mode);
        if (report_mode != 0x00U) {
            body.push_back(report_ts);
            body.push_back(static_cast<std::uint8_t>(report_sample_count &
                                                    0xFFU));
            body.push_back(
                static_cast<std::uint8_t>((report_sample_count >> 8U) & 0xFFU));
        }
        return Res(body);
    }

    std::map<CommandCode, int> m_counts_;
};

/// @brief Master 夹具
struct PackedMasterRig {
    std::unique_ptr<MockTransport> transport_owner;
    MockTransport* transport{nullptr};
    PackedMasterSlave slave;
    std::unique_ptr<XcpMaster> master;
    std::uint8_t max_cto{0x10U};

    PackedMasterRig() {
        transport_owner = std::make_unique<MockTransport>();
        transport = transport_owner.get();
        transport->SetResponse([this](BytesView p) { return slave(p); });
        XcpMasterOptions options;
        options.timeouts = CommandTimeouts{std::chrono::milliseconds(150),
                                          std::chrono::milliseconds(150), 2};
        master = std::make_unique<XcpMaster>(std::move(transport_owner),
                                             options);
        master->Connect();
    }
};

TEST(DaqPackedModeMaster, QueryReportsSlaveValues) {
    PackedMasterRig rig;
    rig.slave.report_mode = 0x02U;
    rig.slave.report_ts = 0x01U;
    rig.slave.report_sample_count = 9U;

    const auto mode = rig.master->QueryDaqPackedMode(0U);
    ASSERT_TRUE(mode.has_value());
    EXPECT_EQ(mode->mode, DaqPackedMode::EventGrouped);
    EXPECT_EQ(mode->timestamp_mode, DaqPackedTimestampMode::FirstSample);
    EXPECT_EQ(mode->sample_count, 9U);
}

TEST(DaqPackedModeMaster, QueryNulloptOnlyWhenCommandUnknown) {
    PackedMasterRig rig;
    rig.slave.report_mode = 0x00U;
    EXPECT_TRUE(rig.master->QueryDaqPackedMode(0U).has_value());

    // 命令未知 → nullopt（Slave 对所有 0xC0 子命令回 ERR_CMD_UNKNOWN）
    XcpMasterOptions options;
    options.timeouts = CommandTimeouts{std::chrono::milliseconds(150),
                                      std::chrono::milliseconds(150), 2};
    auto transport = std::make_unique<MockTransport>();
    transport->SetResponse([](BytesView packet) {
        if (packet.empty()) {
            return Bytes{};
        }
        const auto cmd = static_cast<CommandCode>(packet[0]);
        if (cmd == CommandCode::Connect) {
            return Res({0x15, 0x00, 0x10, 0x08, 0x00, 0x10, 0x10});
        }
        if (cmd == CommandCode::GetStatus) {
            return Res({0x00, 0x00, 0x01, 0x07, 0x00});
        }
        return Err(ErrorCode::CmdUnknown);
    });
    XcpMaster master(std::move(transport), options);
    master.Connect();
    EXPECT_FALSE(master.QueryDaqPackedMode(0U).has_value());
}

TEST(DaqPackedModeMaster, CanUseRequiresListEventAndPeriod) {
    PackedMasterRig rig;
    EXPECT_TRUE(rig.master->CanUseDaqPackedMode(0U, 1U));

    // 列表未声明 PACKED
    rig.slave.list_properties = 0x00U;
    EXPECT_FALSE(rig.master->CanUseDaqPackedMode(0U, 1U));

    // 恢复列表属性，但事件未声明 PACKED
    rig.slave.list_properties = 0x10U;
    rig.slave.event_properties = 0x00U;
    EXPECT_FALSE(rig.master->CanUseDaqPackedMode(0U, 1U));

    // 两者都声明但事件非周期（time_cycle == 0）
    rig.slave.event_properties = 0x10U;
    rig.slave.time_cycle = 0U;
    EXPECT_FALSE(rig.master->CanUseDaqPackedMode(0U, 1U));
}

TEST(DaqPackedModeMaster, SetRequiresMatchingReadback) {
    PackedMasterRig rig;
    rig.slave.report_mode = 0x01U;
    rig.slave.report_sample_count = 4U;
    const auto ok = rig.master->SetDaqPackedMode(
        0U, DaqPackedMode::ElementGrouped, DaqPackedTimestampMode::LastSample,
        4U);
    EXPECT_TRUE(ok.active);
    EXPECT_TRUE(ok.reason.empty());

    // Slave 采纳 SET 但 GET 回读仍是 non-packed → 不得报 active
    PackedMasterRig ignored;
    ignored.slave.report_mode = 0x00U;
    const auto rejected = ignored.master->SetDaqPackedMode(
        0U, DaqPackedMode::ElementGrouped, DaqPackedTimestampMode::LastSample,
        4U);
    EXPECT_FALSE(rejected.active);
    EXPECT_FALSE(rejected.reason.empty());
}

TEST(DaqPackedModeMaster, SetRejectsWhenSlaveRefusesMode) {
    PackedMasterRig rig;
    rig.slave.accept_set = false;
    const auto result = rig.master->SetDaqPackedMode(
        0U, DaqPackedMode::ElementGrouped, DaqPackedTimestampMode::LastSample,
        4U);
    EXPECT_FALSE(result.active);
    EXPECT_FALSE(result.reason.empty());
}

TEST(DaqPackedModeMaster, SetRejectsInvalidSampleCountLocally) {
    PackedMasterRig rig;
    const int before = rig.slave.Count(CommandCode::GetVersion);
    EXPECT_THROW(static_cast<void>(rig.master->SetDaqPackedMode(
                     0U, DaqPackedMode::ElementGrouped,
                     DaqPackedTimestampMode::LastSample, 1U)),
                 XcpException);
    // 本地拒绝：一个 0xC0 帧都不该发出
    EXPECT_EQ(rig.slave.Count(CommandCode::GetVersion), before);
}

TEST(DaqPackedModeMaster, NotPackedIsAcceptedAsReset) {
    PackedMasterRig rig;
    rig.slave.report_mode = 0x00U;
    const auto result = rig.master->SetDaqPackedMode(
        0U, DaqPackedMode::NotPacked, DaqPackedTimestampMode::LastSample, 0U);
    EXPECT_TRUE(result.active);
    EXPECT_EQ(result.mode.mode, DaqPackedMode::NotPacked);
}

// ---------------------------------------------------------------------------
// 规划层
// ---------------------------------------------------------------------------

/// @brief 规划用内存数据库
class PlannerDb final : public IMeasurementDatabase {
 public:
    PlannerDb() = default;

    MeasurementResult<MeasurementSymbolInfo> Find(
        std::string_view name) const override {
        const auto it = symbols.find(std::string(name));
        if (it == symbols.end()) {
            return detail::MakeMeasurementError<MeasurementSymbolInfo>(
                MeasurementErrorCode::NotFound,
                "未找到测量: " + std::string(name), std::string(name));
        }
        return detail::MakeMeasurementOk(it->second);
    }

    MeasurementResult<MeasurementValue> ToPhysical(
        std::string_view /*name*/, BytesView /*raw*/) const override {
        return detail::MakeMeasurementOk<MeasurementValue>(
            MeasurementValue{std::int64_t{0}});
    }

    std::map<std::string, MeasurementSymbolInfo> symbols;
};

TEST(DaqPackedModePlanner, CapacityIsMultipliedBySampleCount) {
    PlannerDb db;
    // 两个 4 字节测量：非 packed 一帧装下（8B）；S=2 时需要 16B
    db.symbols["a"] = MeasurementSymbolInfo{0x1000U, 0, 1, 4, 1, false};
    db.symbols["b"] = MeasurementSymbolInfo{0x1004U, 0, 1, 4, 1, false};

    MeasurementPlanner plain(db, 12, AddressGranularity::Byte, 0, 3);
    const auto plain_plan = plain.Build({"a", "b"});
    ASSERT_TRUE(plain_plan.HasValue());
    EXPECT_EQ(plain_plan.Value().daq_lists.front().odts.size(), 1U);
    EXPECT_EQ(plain_plan.Value().routes.front().packed_stride, 0U);

    // MAX_DTO=12、识别字段 3B → 9B 可用；S=2 时 8B×2=16B 超限 → 两个 ODT
    MeasurementPlanner packed(db, 12, AddressGranularity::Byte, 0, 3,
                              MeasurementDaqCapabilities{}, false, 0,
                              DaqPackedMode::ElementGrouped, 2U);
    const auto packed_plan = packed.Build({"a", "b"});
    ASSERT_TRUE(packed_plan.HasValue());
    EXPECT_EQ(packed_plan.Value().daq_lists.front().odts.size(), 2U);
    // element-grouped：stride = 该 Entry 自身宽度
    EXPECT_EQ(packed_plan.Value().routes.at(0).packed_stride, 4U);
    EXPECT_EQ(packed_plan.Value().routes.at(1).packed_stride, 4U);
}

TEST(DaqPackedModePlanner, EventGroupedStrideIsWholeOdtWidth) {
    PlannerDb db;
    db.symbols["a"] = MeasurementSymbolInfo{0x1000U, 0, 1, 2, 1, false};
    db.symbols["b"] = MeasurementSymbolInfo{0x1004U, 0, 1, 2, 1, false};

    MeasurementPlanner packed(db, 64, AddressGranularity::Byte, 0, 3,
                              MeasurementDaqCapabilities{}, false, 0,
                              DaqPackedMode::EventGrouped, 3U);
    const auto plan = packed.Build({"a", "b"});
    ASSERT_TRUE(plan.HasValue());
    EXPECT_EQ(plan.Value().daq_lists.front().odts.size(), 1U);
    // event-grouped：同 ODT 内所有测量的 stride 都是整个 ODT 单采样宽度
    EXPECT_EQ(plan.Value().routes.at(0).packed_stride, 4U);
    EXPECT_EQ(plan.Value().routes.at(1).packed_stride, 4U);
}

TEST(DaqPackedModePlanner, RejectsInvalidSampleCount) {
    PlannerDb db;
    db.symbols["a"] = MeasurementSymbolInfo{0x1000U, 0, 1, 4, 1, false};
    MeasurementPlanner planner(db, 64, AddressGranularity::Byte, 0, 3,
                               MeasurementDaqCapabilities{}, false, 0,
                               DaqPackedMode::ElementGrouped, 1U);
    const auto plan = planner.Build({"a"});
    ASSERT_FALSE(plan.HasValue());
    EXPECT_EQ(plan.ErrorInfo().code,
              MeasurementErrorCode::DaqConfigurationError);
}

// ---------------------------------------------------------------------------
// 会话层
// ---------------------------------------------------------------------------

/// @brief 会话语义用的内存数据库
class FakeDb final : public IMeasurementDatabase {
 public:
    FakeDb() = default;

    MeasurementResult<MeasurementSymbolInfo> Find(
        std::string_view name) const override {
        const auto it = symbols.find(std::string(name));
        if (it == symbols.end()) {
            return detail::MakeMeasurementError<MeasurementSymbolInfo>(
                MeasurementErrorCode::NotFound,
                "未找到测量: " + std::string(name), std::string(name));
        }
        return detail::MakeMeasurementOk(it->second);
    }

    MeasurementResult<MeasurementValue> ToPhysical(
        std::string_view /*name*/, BytesView raw) const override {
        const std::int64_t v =
            raw.empty() ? 0 : static_cast<std::int64_t>(raw[0]);
        return detail::MakeMeasurementOk(MeasurementValue{v});
    }

    std::map<std::string, MeasurementSymbolInfo> symbols;
};

/**
 * @brief 支持 Packed 的脚本化 Slave
 * @details 列表属性 bit4 PACKED、事件属性 bit4 PACKED 且周期 1（单位 1µs），
 *          SET_DAQ_PACKED_MODE 记录请求并回显为 GET 的回读值。
 */
class PackedSessionSlave {
 public:
    Bytes operator()(BytesView packet) {
        if (packet.empty()) {
            return {};
        }
        const auto cmd = static_cast<CommandCode>(packet[0]);
        ++m_counts_[cmd];
        switch (cmd) {
            case CommandCode::Connect:
                return Res({0x15, 0xC0, 0x10, 0x00, 0x01, 0x10, 0x10});
            case CommandCode::Disconnect:
                return Res({});
            case CommandCode::GetStatus:
                return Res({0x00, 0x00, 0x01, 0x07, 0x00});
            case CommandCode::Synch:
                return Err(ErrorCode::CmdSynch);
            case CommandCode::GetDaqProcessorInfo:
                return Res({0x11, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00});
            case CommandCode::GetDaqResolutionInfo:
                return Res({0x01, 0xF8, 0x01, 0x08, 0x0C, 0x01, 0x00});
            case CommandCode::GetDaqListInfo:
                return Res({0x10, 0x01, 0x04, 0x00, 0x00});
            case CommandCode::GetDaqEventInfo:
                // PACKED + 周期 1 × 1µs（单位码 3）
                return Res({0x10, 0xFF, 0x00, 0x01, 0x03, 0x00});
            case CommandCode::FreeDaq:
            case CommandCode::AllocDaq:
            case CommandCode::AllocOdt:
            case CommandCode::AllocOdtEntry:
            case CommandCode::SetDaqPtr:
            case CommandCode::WriteDaq:
            case CommandCode::ClearDaqList:
            case CommandCode::StartStopSynch:
                return Res({});
            case CommandCode::SetDaqListMode:
                m_last_list_mode = packet[1];
                return Res({});
            case CommandCode::StartStopDaqList:
                return Res({0x00});
            case CommandCode::GetDaqListMode:
                return Res({m_last_list_mode, 0x00, 0x00, 0x01, 0x00, 0x01,
                            0x00});
            case CommandCode::GetVersion:
                if (packet.size() >= 2U && packet[1] == 0x01U) {
                    // SET_DAQ_PACKED_MODE：记录并回显到 GET
                    m_packed_mode = packet[4];
                    m_packed_ts = packet[5];
                    m_packed_sample_count = static_cast<std::uint16_t>(
                        packet[6] | (static_cast<std::uint16_t>(packet[7])
                                     << 8U));
                    return Res({});
                }
                if (packet.size() >= 2U && packet[1] == 0x02U) {
                    Bytes body{0x00U, m_packed_mode};
                    if (m_packed_mode != 0x00U) {
                        body.push_back(m_packed_ts);
                        body.push_back(static_cast<std::uint8_t>(
                            m_packed_sample_count & 0xFFU));
                        body.push_back(static_cast<std::uint8_t>(
                            (m_packed_sample_count >> 8U) & 0xFFU));
                    }
                    return Res(body);
                }
                return Err(ErrorCode::CmdUnknown);
            default:
                return Err(ErrorCode::CmdUnknown);
        }
    }

    [[nodiscard]] int Count(CommandCode cmd) const {
        const auto it = m_counts_.find(cmd);
        return it == m_counts_.end() ? 0 : it->second;
    }

    /// @brief 让 GET_DAQ_EVENT_INFO 返回非周期事件
    bool periodic{true};
    /// @brief 让 GET_DAQ_EVENT_INFO 的 PACKED 位缺省
    bool event_packed{true};

    std::uint8_t m_packed_mode{0x00U};
    std::uint8_t m_packed_ts{0x00U};
    std::uint16_t m_packed_sample_count{0U};
    std::uint8_t m_last_list_mode{0x00U};

 private:
    std::map<CommandCode, int> m_counts_;
};

/// @brief 把完整 DTO 字节包装成 DtoPacket（pid 与 data[0] 恒等）
DtoPacket MakeDto(Bytes frame) {
    DtoPacket packet;
    packet.pid = frame.empty() ? 0U : frame[0];
    packet.data = std::move(frame);
    return packet;
}

/// @brief 构造 RelativeWord DTO 帧：[PID(relODT)][DAQ WORD][ts 4B][净荷]
Bytes MakeFrame(std::uint8_t daq, std::uint8_t rel_odt, std::uint32_t ts,
                BytesView payload) {
    Bytes frame;
    frame.push_back(rel_odt);
    frame.push_back(daq);
    frame.push_back(0x00U);
    for (std::size_t i = 0; i < 4U; ++i) {
        frame.push_back(static_cast<std::uint8_t>((ts >> (8U * i)) & 0xFFU));
    }
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

/// @brief 会话夹具
struct PackedSessionRig {
    FakeDb db;
    std::unique_ptr<MockTransport> transport_owner;
    MockTransport* transport{nullptr};
    PackedSessionSlave slave;
    std::unique_ptr<XcpMaster> master;
    MeasurementSession session{db};

    PackedSessionRig() {
        db.symbols["EngineSpeed"] = MeasurementSymbolInfo{0x1000U, 0, 1, 4, 1,
                                                          false};
        transport_owner = std::make_unique<MockTransport>();
        transport = transport_owner.get();
        transport->SetResponse([this](BytesView p) { return slave(p); });
        XcpMasterOptions options;
        options.timeouts = CommandTimeouts{std::chrono::milliseconds(150),
                                          std::chrono::milliseconds(150), 2};
        options.event_listener = &session;
        master = std::make_unique<XcpMaster>(std::move(transport_owner),
                                             options);
        session.Bind(*master);
        master->Connect();
        // LSB = 1ns，便于把手算的期望值写成整数纳秒
        session.SetTimestampUnit(1);
    }
};

/// @brief 回调帧收集器
class FrameCollector {
 public:
    void operator()(const MeasurementFrame& frame) {
        std::lock_guard<std::mutex> lock(m_mutex_);
        m_frames_.push_back(frame);
        m_cv_.notify_all();
    }

    [[nodiscard]] bool WaitFor(std::size_t n,
                              std::chrono::milliseconds timeout =
                                  std::chrono::milliseconds(2000)) {
        std::unique_lock<std::mutex> lock(m_mutex_);
        return m_cv_.wait_for(lock, timeout, [&] {
            return m_frames_.size() >= n;
        });
    }

    std::vector<MeasurementFrame> Frames() {
        std::lock_guard<std::mutex> lock(m_mutex_);
        return m_frames_;
    }

 private:
    std::mutex m_mutex_;
    std::condition_variable m_cv_;
    std::vector<MeasurementFrame> m_frames_;
};

TEST(DaqPackedModeSession, SplitsOneDtoIntoSampleFrames) {
    PackedSessionRig rig;
    rig.session.Add({"EngineSpeed"});
    rig.session.SetDaqPackedMode(DaqPackedMode::ElementGrouped,
                                 DaqPackedTimestampMode::LastSample, 2U);
    rig.session.Prepare();

    // SET 之后的回读必须已带上 packed 模式与采样数
    EXPECT_EQ(rig.slave.m_packed_mode, 0x01U);
    EXPECT_EQ(rig.slave.m_packed_sample_count, 2U);

    FrameCollector collector;
    rig.session.Start([&](const MeasurementFrame& f) { collector(f); });

    // 净荷 = 2 个采样 × 4 字节；时间戳 1000ns 是**末采样**锚点，
    // 事件周期 1µs → 首采样 = 1000 - 1000 = 0
    const Bytes payload{1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    rig.session.OnDto(MakeDto(MakeFrame(0U, 0U, 1000U, payload)));
    ASSERT_TRUE(collector.WaitFor(2U));

    const auto frames = collector.Frames();
    ASSERT_EQ(frames.size(), 2U);
    EXPECT_EQ(frames[0].sample_index, 0U);
    EXPECT_EQ(frames[0].sample_count, 2U);
    EXPECT_EQ(frames[0].timestamp_source, MeasurementTimestampSource::Reconstructed);
    EXPECT_EQ(frames[0].timestamp.count(), 0);
    EXPECT_EQ(frames[1].sample_index, 1U);
    EXPECT_EQ(frames[1].timestamp_source, MeasurementTimestampSource::Dto);
    EXPECT_EQ(frames[1].timestamp.count(), 1000);
    ASSERT_EQ(frames[0].samples.size(), 1U);
    ASSERT_EQ(frames[1].samples.size(), 1U);
}

TEST(DaqPackedModeSession, ReconstructsFromFirstSampleAnchor) {
    PackedSessionRig rig;
    rig.session.Add({"EngineSpeed"});
    rig.session.SetDaqPackedMode(DaqPackedMode::ElementGrouped,
                                 DaqPackedTimestampMode::FirstSample, 3U);
    rig.session.Prepare();

    FrameCollector collector;
    rig.session.Start([&](const MeasurementFrame& f) { collector(f); });

    const Bytes payload{1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U};
    rig.session.OnDto(MakeDto(MakeFrame(0U, 0U, 5000U, payload)));
    ASSERT_TRUE(collector.WaitFor(3U));

    const auto frames = collector.Frames();
    ASSERT_EQ(frames.size(), 3U);
    // 首采样锚点：0/1000/2000
    EXPECT_EQ(frames[0].timestamp_source, MeasurementTimestampSource::Dto);
    EXPECT_EQ(frames[0].timestamp.count(), 5000);
    EXPECT_EQ(frames[1].timestamp.count(), 6000);
    EXPECT_EQ(frames[2].timestamp.count(), 7000);
    EXPECT_EQ(frames[2].sample_index, 2U);
}

TEST(DaqPackedModeSession, RejectsWhenEventIsNotPeriodic) {
    PackedSessionRig rig;
    rig.session.Add({"EngineSpeed"});
    rig.session.SetDaqPackedMode(DaqPackedMode::ElementGrouped,
                                 DaqPackedTimestampMode::LastSample, 2U);

    PackedSessionSlave non_periodic;
    non_periodic.periodic = false;
    // 事件周期为 0 → 无法重建采样时间戳，必须在开始测量前拒绝
    rig.transport->SetResponse([non_periodic](BytesView p) mutable {
        if (!p.empty() &&
            static_cast<CommandCode>(p[0]) == CommandCode::GetDaqEventInfo) {
            return Res({0x10, 0xFF, 0x00, 0x00, 0x03, 0x00});
        }
        return non_periodic(p);
    });
    EXPECT_THROW(rig.session.Prepare(), XcpException);
}

TEST(DaqPackedModeSession, RejectsWhenEventLacksPackedProperty) {
    PackedSessionRig rig;
    rig.session.Add({"EngineSpeed"});
    rig.session.SetDaqPackedMode(DaqPackedMode::ElementGrouped,
                                 DaqPackedTimestampMode::LastSample, 2U);

    PackedSessionSlave no_packed;
    rig.transport->SetResponse([no_packed](BytesView p) mutable {
        if (!p.empty() &&
            static_cast<CommandCode>(p[0]) == CommandCode::GetDaqEventInfo) {
            return Res({0x00, 0xFF, 0x00, 0x01, 0x03, 0x00});
        }
        if (!p.empty() &&
            static_cast<CommandCode>(p[0]) == CommandCode::GetDaqListInfo) {
            return Res({0x00, 0x01, 0x04, 0x00, 0x00});
        }
        return no_packed(p);
    });
    EXPECT_THROW(rig.session.Prepare(), XcpException);
}

TEST(DaqPackedModeSession, RejectsInvalidConfigurationLocally) {
    PackedSessionRig rig;
    rig.session.Add({"EngineSpeed"});
    EXPECT_THROW(rig.session.SetDaqPackedMode(DaqPackedMode::ElementGrouped,
                                             DaqPackedTimestampMode::LastSample,
                                             1U),
                 XcpException);
    EXPECT_THROW(
        rig.session.SetDaqPackedMode(DaqPackedMode::ElementGrouped,
                                     DaqPackedTimestampMode::FirstSample, 0U),
        XcpException);
}

}  // namespace
}  // namespace calmcar::xcp
