/**
 * @file measurement_counter_test.cpp
 * @brief B2 批次：DTO 计数器（DTO_CTR）贯通测试。
 *
 * 覆盖计划 §3 B2-2 后半段"打通 MeasurementSession 的 counter_enabled
 * 硬编码"的验收项：
 *   - 规划层：请求计数器时 `DaqListSpec.dto_counter` 置位，且计数器额外
 *     占用的信封字节（0 或 1）必须从 ODT 可用容量里扣除；
 *   - 会话层：启用计数器后必须用 GET_DAQ_LIST_MODE 回读确认，
 *     Slave 不支持该命令 → UnsupportedFeature（不静默按无计数器解析）；
 *     回读缺 DTO_CTR 位 → ProtocolError；两者都发生在 Start 之前；
 *   - 解码层：计数器段被正确切分（RelativeWord 下位于识别字段之后、
 *     时间戳之前），原始值转交 MeasurementFrame::counter；
 *   - 断续计数：缺口 → counter_lost_before + 统计计数；重复值 →
 *     记一次不连续但无丢帧；
 *   - 默认路径：未启用时不发 GET_DAQ_LIST_MODE，布局与既有行为一致。
 *
 * 帧布局按会话默认取证：RelativeWord（PID(relODT) 1B + DAQ(WORD) 2B）
 * + 计数器 1B（仅启用时）+ 时间戳 4B + 净荷。
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "libxcp/daq/dto_envelope_types.hpp"
#include "libxcp/measurement/measurement_database.hpp"
#include "libxcp/measurement/measurement_planner.hpp"
#include "libxcp/measurement/measurement_result.hpp"
#include "libxcp/measurement/measurement_session.hpp"
#include "libxcp/protocol_types.hpp"
#include "libxcp/xcp_error.hpp"
#include "libxcp/xcp_master.hpp"
#include "mock_transport.hpp"

namespace calmcar::xcp {
namespace {

using test::MockTransport;

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

TEST(MeasurementPlannerCounter, SetsCounterBitAndReservesOneByte) {
    PlannerDb db;
    // 8 字节净荷：MAX_DTO=12、识别字段 3B、无时间戳 → 无计数器时可用 9B，
    // 有计数器时可用 8B。两者都能装下 8 字节，但再多 1 字节就装不下。
    db.symbols["a"] = MeasurementSymbolInfo{0x1000U, 0, 0, 8, 1, false};
    db.symbols["b"] = MeasurementSymbolInfo{0x1008U, 0, 0, 1, 1, false};

    MeasurementPlanner without(db, 12, AddressGranularity::Byte, 0, 3);
    const auto plan_no_counter = without.Build({"a", "b"});
    ASSERT_TRUE(plan_no_counter.HasValue());
    // 无计数器：9 字节可用，8+1=9 恰好一帧装下
    ASSERT_EQ(plan_no_counter.Value().daq_lists.size(), 1U);
    EXPECT_EQ(plan_no_counter.Value().daq_lists.front().odts.size(), 1U);
    EXPECT_FALSE(plan_no_counter.Value().daq_lists.front().dto_counter);

    MeasurementPlanner with(db, 12, AddressGranularity::Byte, 0, 3,
                            MeasurementDaqCapabilities{}, true, 1);
    const auto plan_counter = with.Build({"a", "b"});
    ASSERT_TRUE(plan_counter.HasValue());
    // 有计数器：8 字节可用，8+1=9 超限 → 开第二个 ODT
    ASSERT_EQ(plan_counter.Value().daq_lists.size(), 1U);
    EXPECT_EQ(plan_counter.Value().daq_lists.front().odts.size(), 2U);
    EXPECT_TRUE(plan_counter.Value().daq_lists.front().dto_counter);
}

TEST(MeasurementPlannerCounter, MergedCounterTakesNoExtraByte) {
    PlannerDb db;
    db.symbols["a"] = MeasurementSymbolInfo{0x1000U, 0, 0, 6, 1, false};
    db.symbols["b"] = MeasurementSymbolInfo{0x1008U, 0, 0, 2, 1, false};

    // RelativeWordAligned 下 CTR 取代 FILL，不额外占字节：0 字节开销。
    // MAX_DTO=12、识别字段 4B → 可用 8B，6+2 恰满一帧；若误扣 1B
    // 就会开第二个 ODT——这正是本用例要防的回归。
    MeasurementPlanner planner(db, 12, AddressGranularity::Byte, 0, 4,
                               MeasurementDaqCapabilities{}, true, 0);
    const auto plan = planner.Build({"a", "b"});
    ASSERT_TRUE(plan.HasValue());
    EXPECT_TRUE(plan.Value().daq_lists.front().dto_counter);
    EXPECT_EQ(plan.Value().daq_lists.front().odts.size(), 1U);
}

TEST(MeasurementPlannerCounter, CounterByteAloneCannotExceedMaxDto) {
    PlannerDb db;
    db.symbols["a"] = MeasurementSymbolInfo{0x1000U, 0, 0, 1, 1, false};
    // MAX_DTO=4、识别字段 3B → 剩 1B；计数器再要 1B 就超出
    MeasurementPlanner planner(db, 4, AddressGranularity::Byte, 0, 3,
                               MeasurementDaqCapabilities{}, true, 1);
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
        const std::int64_t v = static_cast<std::int64_t>(raw.size()) +
                               (raw.empty() ? 0
                                            : static_cast<std::int64_t>(raw[0]));
        return detail::MakeMeasurementOk(MeasurementValue{v});
    }

    std::map<std::string, MeasurementSymbolInfo> symbols;
};

/**
 * @brief 脚本化 Slave：动态整表通路 + 可配置的 GET_DAQ_LIST_MODE 行为
 * @details `list_mode_supported=false` 时回 ERR_CMD_UNKNOWN（模拟不支持
 *          Optional 命令的从站）；`list_mode` 是回读的 Current Mode 字节。
 */
class CounterSlave {
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
            case CommandCode::GetCommModeInfo:
                return Res({0x00, 0x0E, 0x00, 0x04, 0x02, 0x08, 0x13});
            case CommandCode::Synch:
                return Err(ErrorCode::CmdSynch);
            case CommandCode::GetDaqProcessorInfo:
                return Res({0x11, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00});
            case CommandCode::GetDaqResolutionInfo:
                return Res({0x01, 0xF8, 0x01, 0x08, 0x0C, 0x01, 0x00});
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
                // 记录最近一次设置的模式字节，便于断言 DTO_CTR 位是否请求
                m_last_set_mode = packet[1];
                return Res({packet[1], packet[2], packet[3], packet[4],
                            packet[5]});
            case CommandCode::StartStopDaqList:
                return Res({0x00});
            case CommandCode::GetDaqListMode: {
                if (!list_mode_supported) {
                    return Err(ErrorCode::CmdUnknown);
                }
                // RES=[mode][WORD reserved][WORD event][prescaler][priority]
                return Res({list_mode, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00});
            }
            default:
                return Err(ErrorCode::CmdUnknown);
        }
    }

    [[nodiscard]] int Count(CommandCode cmd) const {
        const auto it = m_counts_.find(cmd);
        return it == m_counts_.end() ? 0 : it->second;
    }

    bool list_mode_supported{true};
    /// @brief GET_DAQ_LIST_MODE 回报的 Current Mode 字节
    std::uint8_t list_mode{0x00};
    /// @brief 最近一次 SET_DAQ_LIST_MODE 的 Mode 字节
    std::uint8_t m_last_set_mode{0x00};

 private:
    static Bytes Res(const std::vector<std::uint8_t>& body) {
        Bytes out;
        out.reserve(1 + body.size());
        out.push_back(static_cast<std::uint8_t>(PacketType::Res));
        out.insert(out.end(), body.begin(), body.end());
        return out;
    }
    static Bytes Err(ErrorCode code) {
        return Bytes{static_cast<std::uint8_t>(PacketType::Err),
                     static_cast<std::uint8_t>(code)};
    }

    std::map<CommandCode, int> m_counts_;
};

/// @brief 把完整 DTO 字节包装成 DtoPacket（pid 与 data[0] 恒等）
DtoPacket MakeDto(Bytes frame) {
    DtoPacket packet;
    packet.pid = frame.empty() ? 0U : frame[0];
    packet.data = std::move(frame);
    return packet;
}

/// @brief 构造带计数器的 RelativeWord DTO 帧
/// @details [PID(relODT)][DAQ(WORD)][CTR 1B][ts 4B][净荷]
Bytes MakeCounterFrame(std::uint8_t daq, std::uint8_t rel_odt,
                       std::uint8_t counter, std::uint32_t ts,
                       BytesView payload) {
    Bytes frame;
    frame.push_back(rel_odt);
    frame.push_back(daq);
    frame.push_back(0x00U);
    frame.push_back(counter);
    for (std::size_t i = 0; i < 4U; ++i) {
        frame.push_back(static_cast<std::uint8_t>((ts >> (8U * i)) & 0xFFU));
    }
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

/// @brief 构造不带计数器的 RelativeWord DTO 帧（既有布局）
Bytes MakePlainFrame(std::uint8_t daq, std::uint8_t rel_odt, std::uint32_t ts,
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

/// @brief 会话夹具：db → session → master（&session 作监听器）→ Bind → Connect
struct CounterRig {
    FakeDb db;
    std::unique_ptr<MockTransport> transport_owner;
    MockTransport* transport{nullptr};
    CounterSlave slave;
    std::unique_ptr<XcpMaster> master;
    MeasurementSession session{db};

    CounterRig() {
        db.symbols["EngineSpeed"] = MeasurementSymbolInfo{0x1000U, 0, 0, 4, 1,
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
        return m_cv_.wait_for(lock, timeout,
                             [&] { return m_frames_.size() >= n; });
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

TEST(MeasurementSessionCounter, RejectedWhenSlaveCannotReadListMode) {
    CounterRig rig;
    rig.slave.list_mode_supported = false;
    rig.session.Add("EngineSpeed");
    rig.session.SetDtoCounterEnabled(true);

    // 启用计数器却无法回读布局 → 明确不支持，绝不按“无计数器”静默解析
    EXPECT_THROW(
        {
            try {
                rig.session.Prepare();
            } catch (const XcpException& e) {
                EXPECT_EQ(e.Category(), ErrorCategory::UnsupportedFeature);
                throw;
            }
        },
        XcpException);
}

TEST(MeasurementSessionCounter, RejectedWhenReadbackLacksCounterBit) {
    CounterRig rig;
    rig.slave.list_mode_supported = true;
    // 回读模式缺 DTO_CTR 位（bit3=0x08）
    rig.slave.list_mode = 0x00U;
    rig.session.Add("EngineSpeed");
    rig.session.SetDtoCounterEnabled(true);

    EXPECT_THROW(
        {
            try {
                rig.session.Prepare();
            } catch (const XcpException& e) {
                EXPECT_EQ(e.Category(), ErrorCategory::ProtocolError);
                throw;
            }
        },
        XcpException);
}

TEST(MeasurementSessionCounter, PrepareRequestsCounterAndConfirmsByReadback) {
    CounterRig rig;
    rig.slave.list_mode_supported = true;
    rig.slave.list_mode = 0x08U;  // DTO_CTR 位
    rig.session.Add("EngineSpeed");
    rig.session.SetDtoCounterEnabled(true);

    ASSERT_NO_THROW(rig.session.Prepare());
    // 下发时确实请求了 kDtoCounter（bit3=0x08）
    EXPECT_EQ(rig.slave.m_last_set_mode & 0x08U, 0x08U);
    // 且确实回读确认过
    EXPECT_EQ(rig.slave.Count(CommandCode::GetDaqListMode), 1);
}

TEST(MeasurementSessionCounter, DefaultPathDoesNotQueryListMode) {
    CounterRig rig;
    rig.session.Add("EngineSpeed");
    ASSERT_NO_THROW(rig.session.Prepare());
    // 未启用计数器 → 不得为了“顺带确认”多发可选命令
    EXPECT_EQ(rig.slave.Count(CommandCode::GetDaqListMode), 0);
    EXPECT_EQ(rig.slave.m_last_set_mode & 0x08U, 0x00U);
}

TEST(MeasurementSessionCounter, FramesCarryCounterAndNoGap) {
    CounterRig rig;
    rig.slave.list_mode = 0x08U;
    rig.session.Add("EngineSpeed");
    rig.session.SetDtoCounterEnabled(true);
    ASSERT_NO_THROW(rig.session.Prepare());

    FrameCollector collector;
    rig.session.Start([&](const MeasurementFrame& f) { collector(f); });

    const Bytes p1{0x11U, 0x22U, 0x33U, 0x44U};
    rig.session.OnDto(MakeDto(MakeCounterFrame(0, 0, 0x10U, 100U, p1)));
    rig.session.OnDto(MakeDto(MakeCounterFrame(0, 0, 0x11U, 200U, p1)));
    rig.session.OnDto(MakeDto(MakeCounterFrame(0, 0, 0x12U, 300U, p1)));
    ASSERT_TRUE(collector.WaitFor(3));
    rig.session.Stop();

    const auto frames = collector.Frames();
    ASSERT_EQ(frames.size(), 3U);
    EXPECT_EQ(frames[0].counter.value_or(0xFF), 0x10U);
    EXPECT_EQ(frames[1].counter.value_or(0xFF), 0x11U);
    EXPECT_EQ(frames[2].counter.value_or(0xFF), 0x12U);
    for (const auto& f : frames) {
        EXPECT_EQ(f.counter_lost_before, 0U);
    }
    const MeasurementStatistics stats = rig.session.Statistics();
    EXPECT_EQ(stats.counter_gaps, 0U);
    EXPECT_EQ(stats.counter_lost, 0U);
    // 计数器段被正确消费，净荷未错位（首字节仍是 0x11）
    EXPECT_EQ(frames[0].samples.at(0).raw.at(0), 0x11U);
}

TEST(MeasurementSessionCounter, GapIsReportedAsLostFrames) {
    CounterRig rig;
    rig.slave.list_mode = 0x08U;
    rig.session.Add("EngineSpeed");
    rig.session.SetDtoCounterEnabled(true);
    ASSERT_NO_THROW(rig.session.Prepare());

    FrameCollector collector;
    rig.session.Start([&](const MeasurementFrame& f) { collector(f); });

    const Bytes p1{0x01U, 0x02U, 0x03U, 0x04U};
    rig.session.OnDto(MakeDto(MakeCounterFrame(0, 0, 0x20U, 100U, p1)));
    rig.session.OnDto(MakeDto(MakeCounterFrame(0, 0, 0x25U, 200U, p1)));
    ASSERT_TRUE(collector.WaitFor(2));
    rig.session.Stop();

    const auto frames = collector.Frames();
    ASSERT_EQ(frames.size(), 2U);
    EXPECT_EQ(frames[0].counter_lost_before, 0U);
    // 0x20 → 0x25：delta=5，丢了 4 帧
    EXPECT_EQ(frames[1].counter_lost_before, 4U);
    const MeasurementStatistics stats = rig.session.Statistics();
    EXPECT_EQ(stats.counter_gaps, 1U);
    EXPECT_EQ(stats.counter_lost, 4U);
}

TEST(MeasurementSessionCounter, DuplicateCounterCountsGapWithoutLoss) {
    CounterRig rig;
    rig.slave.list_mode = 0x08U;
    rig.session.Add("EngineSpeed");
    rig.session.SetDtoCounterEnabled(true);
    ASSERT_NO_THROW(rig.session.Prepare());

    FrameCollector collector;
    rig.session.Start([&](const MeasurementFrame& f) { collector(f); });

    const Bytes p1{0x01U, 0x02U, 0x03U, 0x04U};
    rig.session.OnDto(MakeDto(MakeCounterFrame(0, 0, 0x30U, 100U, p1)));
    rig.session.OnDto(MakeDto(MakeCounterFrame(0, 0, 0x30U, 200U, p1)));
    ASSERT_TRUE(collector.WaitFor(2));
    rig.session.Stop();

    const auto frames = collector.Frames();
    ASSERT_EQ(frames.size(), 2U);
    // 重复值：记一次不连续，但不推断丢帧数（delta=0 无法区分重发与丢 255）
    EXPECT_EQ(frames[1].counter_lost_before, 0U);
    const MeasurementStatistics stats = rig.session.Statistics();
    EXPECT_EQ(stats.counter_gaps, 1U);
    EXPECT_EQ(stats.counter_lost, 0U);
}

TEST(MeasurementSessionCounter, SetterRejectedWhileRunning) {
    CounterRig rig;
    rig.slave.list_mode = 0x08U;
    rig.session.Add("EngineSpeed");
    ASSERT_NO_THROW(rig.session.Prepare());
    FrameCollector collector;
    rig.session.Start([&](const MeasurementFrame& f) { collector(f); });

    EXPECT_THROW(rig.session.SetDtoCounterEnabled(true), XcpException);
    rig.session.Stop();
}

TEST(MeasurementSessionCounter, LegacyLayoutStillDecodesWithoutCounter) {
    CounterRig rig;
    rig.session.Add("EngineSpeed");
    ASSERT_NO_THROW(rig.session.Prepare());

    FrameCollector collector;
    rig.session.Start([&](const MeasurementFrame& f) { collector(f); });

    const Bytes p1{0x01U, 0x02U, 0x03U, 0x04U};
    rig.session.OnDto(MakeDto(MakePlainFrame(0, 0, 100U, p1)));
    ASSERT_TRUE(collector.WaitFor(1));
    rig.session.Stop();

    const auto frames = collector.Frames();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_FALSE(frames[0].counter.has_value());
    EXPECT_EQ(frames[0].samples.at(0).raw.at(0), 0x01U);
}

}  // namespace
}  // namespace calmcar::xcp