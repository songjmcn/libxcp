/**
 * @file poll_session_test.cpp
 * @brief P1：`PollSession` 骨架与单/多 event 周期调度的单元测试。
 */

#include "libxcp/poll/poll_session.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "libxcp/measurement/measurement_database.hpp"
#include "libxcp/protocol_types.hpp"
#include "libxcp/xcp_error.hpp"
#include "libxcp/xcp_master.hpp"

#include "mock_transport.hpp"

namespace calmcar::xcp {
namespace {

using ::testing::Test;
using test::MockTransport;

Bytes Res(std::initializer_list<std::uint8_t> bytes) {
    Bytes packet;
    packet.push_back(0xFF);
    packet.insert(packet.end(), bytes.begin(), bytes.end());
    return packet;
}

Bytes Err(ErrorCode code) {
    Bytes packet;
    packet.push_back(0xFE);
    packet.push_back(static_cast<std::uint8_t>(code));
    return packet;
}

/// @brief 同步投递器：把响应报文交给监听器
struct Yield {
    IPacketListener* listener{nullptr};
};

/// @brief 内存与符号数据库的轻量假实现
class FakeDb : public IMeasurementDatabase {
public:
    struct Entry {
        MeasurementSymbolInfo info;
    };
    std::map<std::string, Entry> symbols;

    MeasurementResult<MeasurementSymbolInfo> Find(
        std::string_view name) const override {
        const auto it = symbols.find(std::string{name});
        if (it == symbols.end()) {
            return MeasurementResult<MeasurementSymbolInfo>(MeasurementError{
                MeasurementErrorCode::NotFound, "未找到该测量", std::string{name}});
        }
        return MeasurementResult<MeasurementSymbolInfo>(it->second.info);
    }

    MeasurementResult<MeasurementValue> ToPhysical(
        std::string_view name, BytesView raw) const override {
        if (raw.empty()) {
            return MeasurementResult<MeasurementValue>(MeasurementError{
                MeasurementErrorCode::RawSizeMismatch, "raw 为空", std::string{name}});
        }
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < raw.size() && i < 8; ++i) {
            value |= static_cast<std::uint64_t>(raw[i]) << (8U * i);
        }
        return MeasurementResult<MeasurementValue>(
            MeasurementValue{value});
    }

    /// @brief 逐元素换算；`fail_element` 命中该下标时返回失败，用于验证
    ///        数组任一元素失败即整条样本无效（不暴露部分结果）
    MeasurementResult<MeasurementValue> ToPhysicalElement(
        std::string_view name, std::uint64_t element_index,
        BytesView raw) const override {
        if (static_cast<std::int64_t>(element_index) == fail_element.load()) {
            return MeasurementResult<MeasurementValue>(MeasurementError{
                MeasurementErrorCode::UnsupportedConversion,
                "元素换算失败（测试注入）", std::string{name}});
        }
        return ToPhysical(name, raw);
    }

    std::atomic<int> fail_element{-1};  ///< 命中该元素下标时令换算失败
};

/// @brief 支持 CONNECT / GET_STATUS / SHORT_UPLOAD 的最小模拟从机
class PollSlave {
public:
    std::map<Address, std::uint8_t> memory;
    std::atomic<bool> fail_short_upload{false};
    std::atomic<int> fail_next_short_uploads{0};   ///< 仅让接下来 N 次 SHORT_UPLOAD 失败
    std::atomic<int> short_upload_count{0};        ///< SHORT_UPLOAD 到达次数
    std::atomic<std::uint8_t> session_status{0x00};  ///< GET_STATUS 的 Current Session Status
    std::atomic<Address> fail_address{0xFFFFFFFFU};  ///< 读取范围含该地址时回 ERR_OUT_OF_RANGE

    Bytes operator()(BytesView packet) {
        if (packet.empty()) {
            return {};
        }
        const auto code = static_cast<CommandCode>(packet[0]);
        switch (code) {
            case CommandCode::Connect:
                return Res({0x15, 0x00, 0x10, 0x08, 0x00, 0x10, 0x10});
            case CommandCode::GetStatus:
                return Res({session_status.load(), 0x00, 0x01, 0x07, 0x00});
            case CommandCode::ShortUpload:
                return HandleShortUpload(packet);
            default:
                return Err(ErrorCode::CmdUnknown);
        }
    }

private:
    Bytes HandleShortUpload(BytesView packet) {
        short_upload_count.fetch_add(1);
        if (fail_short_upload.load()) {
            return Err(ErrorCode::OutOfRange);
        }
        if (fail_next_short_uploads.load() > 0) {
            fail_next_short_uploads.fetch_sub(1);
            return Err(ErrorCode::OutOfRange);
        }
        // CTO = [ED][n][reserved][ext][addr DWORD]
        if (packet.size() < 8) {
            return Err(ErrorCode::CmdSyntax);
        }
        const std::uint8_t elements = packet[1];
        const auto address = static_cast<Address>(
            static_cast<std::uint32_t>(packet[4]) |
            (static_cast<std::uint32_t>(packet[5]) << 8U) |
            (static_cast<std::uint32_t>(packet[6]) << 16U) |
            (static_cast<std::uint32_t>(packet[7]) << 24U));
        // 按地址注入失败：只让覆盖该地址的那次读失败，用于验证"单变量失败
        // 不拖垮同 tick 其余变量"（实施计划 §4.2）
        const Address failing = fail_address.load();
        if (failing != 0xFFFFFFFFU) {
            for (std::uint8_t i = 0; i < elements; ++i) {
                if (static_cast<Address>(address + i) == failing) {
                    return Err(ErrorCode::OutOfRange);
                }
            }
        }
        Bytes response;
        response.push_back(0xFF);
        for (std::uint8_t i = 0; i < elements; ++i) {
            const auto it = memory.find(static_cast<Address>(address + i));
            response.push_back(it == memory.end() ? 0x00 : it->second);
        }
        return response;
    }
};

/// @brief 采集帧收集器
class Collector {
public:
    void operator()(const PollSample& sample) {
        {
            std::lock_guard<std::mutex> lock(m_mutex_);
            m_samples_.push_back(sample);
        }
        m_cv_.notify_all();
    }

    bool WaitFor(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex_);
        return m_cv_.wait_for(lock, timeout,
                              [this, count] { return m_samples_.size() >= count; });
    }

    std::vector<PollSample> Samples() const {
        std::lock_guard<std::mutex> lock(m_mutex_);
        return m_samples_;
    }

private:
    mutable std::mutex m_mutex_;
    std::condition_variable m_cv_;
    std::vector<PollSample> m_samples_;
};

/// @brief 组装：模拟从机 + 主站 + 数据库 + 会话
struct Rig {
    explicit Rig(std::uint8_t scale = 1) {
        for (std::uint8_t i = 0; i < 4; ++i) {
            auto& entry = database.symbols["sig" + std::to_string(i)];
            entry.info.address = 0x1000U + static_cast<Address>(i) * scale;
            entry.info.extension = 0;
            entry.info.element_size_bytes = scale;
            entry.info.element_count = 1;
            entry.info.is_array = false;
            for (std::uint8_t b = 0; b < scale; ++b) {
                slave.memory[static_cast<Address>(0x1000U + i * scale + b)] =
                    static_cast<std::uint8_t>(0x10U * (i + 1) + b);
            }
        }
        transport = std::make_unique<MockTransport>();
        transport->SetResponse([this](BytesView packet) { return slave(packet); });
        XcpMasterOptions options;
        options.timeouts.command_timeout = std::chrono::milliseconds(200);
        master = std::make_unique<XcpMaster>(std::move(transport), options);
        master->Connect();
        session = std::make_unique<PollSession>(*master, database);
    }

    FakeDb database;
    PollSlave slave;
    std::unique_ptr<MockTransport> transport;
    std::unique_ptr<XcpMaster> master;
    std::unique_ptr<PollSession> session;
};

TEST(PollSessionSkeleton, AddEventRejectsDuplicateName) {
    Rig rig;
    rig.session->AddEvent(PollEventSpec{"fast", std::chrono::milliseconds(20), {"sig0"}});
    EXPECT_THROW(rig.session->AddEvent(
                     PollEventSpec{"fast", std::chrono::milliseconds(50), {"sig1"}}),
                 XcpException);
    EXPECT_EQ(rig.session->EventCount(), 1U);
}

TEST(PollSessionSkeleton, AddEventRejectsPeriodBelowMinimum) {
    Rig rig;
    EXPECT_THROW(rig.session->AddEvent(
                     PollEventSpec{"fast", std::chrono::milliseconds(19), {"sig0"}}),
                 XcpException);
    EXPECT_EQ(rig.session->EventCount(), 0U);
}

TEST(PollSessionSkeleton, AddEventRejectsEmptySymbolList) {
    Rig rig;
    EXPECT_THROW(rig.session->AddEvent(
                     PollEventSpec{"fast", std::chrono::milliseconds(20), {}}),
                 XcpException);
}

TEST(PollSessionSkeleton, RemoveEventRejectsUnknownName) {
    Rig rig;
    rig.session->AddEvent(PollEventSpec{"fast", std::chrono::milliseconds(20), {"sig0"}});
    rig.session->RemoveEvent("fast");
    EXPECT_EQ(rig.session->EventCount(), 0U);
    EXPECT_THROW(rig.session->RemoveEvent("fast"), XcpException);
}

TEST(PollSessionSkeleton, ValidateReportsUnresolvedSymbol) {
    Rig rig;
    rig.session->AddEvent(
        PollEventSpec{"fast", std::chrono::milliseconds(20), {"sig0", "missing"}});
    const auto result = rig.session->Validate();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.ErrorInfo().symbol, "missing");
    EXPECT_EQ(result.ErrorInfo().code, MeasurementErrorCode::NotFound);
}

TEST(PollSessionSkeleton, ValidateAcceptsArraySymbol) {
    Rig rig;
    rig.database.symbols["arr"].info.address = 0x2000U;
    rig.database.symbols["arr"].info.element_size_bytes = 1;
    rig.database.symbols["arr"].info.element_count = 4;
    rig.database.symbols["arr"].info.is_array = true;
    rig.session->AddEvent(
        PollEventSpec{"fast", std::chrono::milliseconds(20), {"arr"}});
    // 数组不再被拒绝：布局合法（宽度/元素数非 0）即可通过预检
    EXPECT_TRUE(rig.session->Validate());
}

TEST(PollSessionSkeleton, ArraySymbolProducesElementSamples) {
    Rig rig;
    rig.database.symbols["arr"].info.address = 0x2000U;
    rig.database.symbols["arr"].info.element_size_bytes = 1;
    rig.database.symbols["arr"].info.element_count = 4;
    rig.database.symbols["arr"].info.is_array = true;
    for (std::uint8_t i = 0; i < 4; ++i) {
        rig.slave.memory[static_cast<Address>(0x2000U + i)] =
            static_cast<std::uint8_t>(i + 1);
    }
    rig.session->AddEvent(
        PollEventSpec{"fast", std::chrono::milliseconds(20), {"arr"}});
    Collector collector;
    rig.session->Start(
        [&collector](const PollSample& sample) { collector(sample); });
    ASSERT_TRUE(collector.WaitFor(1, std::chrono::milliseconds(2000)));
    rig.session->Stop();

    const auto samples = collector.Samples();
    ASSERT_FALSE(samples.empty());
    ASSERT_EQ(samples.front().samples.size(), 1U);
    const auto& variable = samples.front().samples.front();
    ASSERT_TRUE(variable.valid) << variable.error;
    EXPECT_TRUE(variable.is_array);
    ASSERT_EQ(variable.elements.size(), 4U);
    for (std::size_t i = 0; i < variable.elements.size(); ++i) {
        ASSERT_TRUE(std::holds_alternative<std::uint64_t>(variable.elements[i]));
        EXPECT_EQ(std::get<std::uint64_t>(variable.elements[i]), i + 1);
    }
}

TEST(PollSessionSkeleton, ArrayElementFailureDoesNotLeakPartialResult) {
    Rig rig;
    rig.database.symbols["arr"].info.address = 0x2000U;
    rig.database.symbols["arr"].info.element_size_bytes = 1;
    rig.database.symbols["arr"].info.element_count = 4;
    rig.database.symbols["arr"].info.is_array = true;
    for (std::uint8_t i = 0; i < 4; ++i) {
        rig.slave.memory[static_cast<Address>(0x2000U + i)] =
            static_cast<std::uint8_t>(i + 1);
    }
    rig.database.fail_element.store(2);
    rig.session->AddEvent(PollEventSpec{
        "fast", std::chrono::milliseconds(20), {"arr", "sig0"}});
    Collector collector;
    rig.session->Start(
        [&collector](const PollSample& sample) { collector(sample); });
    ASSERT_TRUE(collector.WaitFor(1, std::chrono::milliseconds(2000)));
    rig.session->Stop();

    const auto samples = collector.Samples();
    ASSERT_FALSE(samples.empty());
    const auto& results = samples.front().samples;
    ASSERT_EQ(results.size(), 2U);
    // 数组任一元素失败：整条无效且不保留部分结果
    EXPECT_FALSE(results[0].valid);
    EXPECT_TRUE(results[0].is_array);
    EXPECT_TRUE(results[0].elements.empty());
    EXPECT_FALSE(results[0].error.empty());
    // 同 tick 的标量变量不受影响（错误隔离）
    EXPECT_TRUE(results[1].valid) << results[1].error;
    EXPECT_FALSE(results[1].is_array);
}

TEST(PollSessionSkeleton, ValidateSucceedsForScalarSymbols) {
    Rig rig;
    rig.session->AddEvent(
        PollEventSpec{"fast", std::chrono::milliseconds(20), {"sig0", "sig1"}});
    EXPECT_TRUE(rig.session->Validate());
}

TEST(PollSessionSkeleton, StartRejectsWhenNotConnected) {
    FakeDb database;
    database.symbols["sig0"].info.address = 0x1000U;
    database.symbols["sig0"].info.element_size_bytes = 1;
    database.symbols["sig0"].info.element_count = 1;
    auto transport = std::make_unique<MockTransport>();
    XcpMaster master(std::move(transport));
    PollSession session(master, database);
    session.AddEvent(PollEventSpec{"fast", std::chrono::milliseconds(20), {"sig0"}});
    EXPECT_THROW(session.Start(PollCallback{}), XcpException);
}

TEST(PollSessionSkeleton, StartRejectsWithoutEvents) {
    Rig rig;
    EXPECT_THROW(rig.session->Start(PollCallback{}), XcpException);
}

TEST(PollSessionSkeleton, StartRejectsUnresolvedSymbol) {
    Rig rig;
    rig.session->AddEvent(
        PollEventSpec{"fast", std::chrono::milliseconds(20), {"missing"}});
    EXPECT_THROW(rig.session->Start(PollCallback{}), XcpException);
    EXPECT_FALSE(rig.session->IsRunning());
}

TEST(PollSessionSkeleton, SingleEventProducesPeriodicSamples) {
    Rig rig;
    rig.session->AddEvent(
        PollEventSpec{"fast", std::chrono::milliseconds(20), {"sig0", "sig1"}});
    Collector collector;
    rig.session->Start([&collector](const PollSample& sample) { collector(sample); });
    ASSERT_TRUE(collector.WaitFor(3, std::chrono::milliseconds(2000)));
    rig.session->Stop();

    const auto samples = collector.Samples();
    ASSERT_GE(samples.size(), 3U);
    EXPECT_EQ(samples.front().event_name, "fast");
    ASSERT_EQ(samples.front().samples.size(), 2U);
    EXPECT_TRUE(samples.front().samples[0].valid);
    EXPECT_EQ(std::get<std::uint64_t>(samples.front().samples[0].value), 0x10U);
    EXPECT_EQ(std::get<std::uint64_t>(samples.front().samples[1].value), 0x20U);
    EXPECT_GE(rig.session->Statistics().ticks, 3U);
}

TEST(PollSessionSkeleton, SingleVariableFailureDoesNotAbortTick) {
    Rig rig;
    // sig0 与 sig1 地址相邻会被合并成一次读；只让覆盖 0x1000 的那次读失败
    rig.slave.fail_address.store(0x1000U);
    rig.session->AddEvent(
        PollEventSpec{"fast", std::chrono::milliseconds(20), {"sig0", "sig1"}});
    Collector collector;
    rig.session->Start([&collector](const PollSample& sample) { collector(sample); });
    ASSERT_TRUE(collector.WaitFor(1, std::chrono::milliseconds(2000)));
    rig.session->Stop();

    const auto samples = collector.Samples();
    ASSERT_FALSE(samples.empty());
    ASSERT_EQ(samples.front().samples.size(), 2U);
    EXPECT_EQ(samples.front().samples[0].symbol, "sig0");
    EXPECT_FALSE(samples.front().samples[0].valid);
    EXPECT_FALSE(samples.front().samples[0].error.empty());
    // 同一 tick 的其余变量必须照常有样本（错误隔离，实施计划 §8.2）
    EXPECT_EQ(samples.front().samples[1].symbol, "sig1");
    EXPECT_TRUE(samples.front().samples[1].valid);
    const auto* value = std::get_if<std::uint64_t>(&samples.front().samples[1].value);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, 0x20U);
    EXPECT_GE(rig.session->Statistics().read_errors, 1U);
}

TEST(PollSessionSkeleton, CallbackExceptionDoesNotStopScheduling) {
    Rig rig;
    rig.session->AddEvent(
        PollEventSpec{"fast", std::chrono::milliseconds(20), {"sig0"}});
    std::atomic<int> calls{0};
    Collector collector;
    rig.session->Start([&](const PollSample& sample) {
        if (calls.fetch_add(1) == 0) {
            throw std::runtime_error("callback failure");
        }
        collector(sample);
    });
    ASSERT_TRUE(collector.WaitFor(2, std::chrono::milliseconds(2000)));
    rig.session->Stop();
    EXPECT_GE(rig.session->Statistics().callback_errors, 1U);
}

TEST(PollSessionSkeleton, StopFlipsRunningAndIsIdempotent) {
    Rig rig;
    rig.session->AddEvent(
        PollEventSpec{"fast", std::chrono::milliseconds(20), {"sig0"}});
    EXPECT_FALSE(rig.session->IsRunning());
    rig.session->Start(PollCallback{});
    EXPECT_TRUE(rig.session->IsRunning());
    rig.session->Stop();
    EXPECT_FALSE(rig.session->IsRunning());
    rig.session->Stop();  // 幂等
}

TEST(PollSessionSkeleton, AddEventRejectedWhileRunning) {
    Rig rig;
    rig.session->AddEvent(
        PollEventSpec{"fast", std::chrono::milliseconds(20), {"sig0"}});
    rig.session->Start(PollCallback{});
    EXPECT_THROW(rig.session->AddEvent(
                     PollEventSpec{"slow", std::chrono::milliseconds(50), {"sig1"}}),
                 XcpException);
    EXPECT_THROW(rig.session->RemoveEvent("fast"), XcpException);
    rig.session->Stop();
}

TEST(PollSessionSkeleton, MultipleEventsKeepIndependentPeriods) {
    Rig rig;
    rig.session->AddEvent(
        PollEventSpec{"fast", std::chrono::milliseconds(20), {"sig0"}});
    rig.session->AddEvent(
        PollEventSpec{"slow", std::chrono::milliseconds(50), {"sig1"}});
    Collector collector;
    rig.session->Start([&collector](const PollSample& sample) { collector(sample); });
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    rig.session->Stop();

    std::size_t fast = 0;
    std::size_t slow = 0;
    std::vector<std::chrono::steady_clock::time_point> slow_times;
    for (const auto& sample : collector.Samples()) {
        if (sample.event_name == "fast") {
            ++fast;
            ASSERT_EQ(sample.samples.size(), 1U);
            EXPECT_EQ(sample.samples[0].symbol, "sig0");
        } else if (sample.event_name == "slow") {
            ++slow;
            ASSERT_EQ(sample.samples.size(), 1U);
            EXPECT_EQ(sample.samples[0].symbol, "sig1");
            slow_times.push_back(sample.steady_time);
        }
    }
    // 20 ms : 50 ms = 2.5，留足非实时调度抖动余量
    ASSERT_GE(fast, 10U);
    ASSERT_GE(slow, 4U);
    EXPECT_GE(fast, slow * 2);
    ASSERT_GE(slow_times.size(), 2U);
    for (std::size_t i = 1; i < slow_times.size(); ++i) {
        const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(
            slow_times[i] - slow_times[i - 1]);
        EXPECT_GE(delta.count(), 40);
        // 上界放到 75 ms：非实时 Windows 上单次 tick 抖动可达 +12 ms（P4 标定
        // 里 20 ms 周期的最坏间隔也有 32 ms），50 ms 周期偶发 61~62 ms 属正常
        EXPECT_LE(delta.count(), 75);
    }
}

TEST(PollSessionSkeleton, OverrunIsSkippedNotCatchUp) {
    Rig rig;
    rig.session->AddEvent(
        PollEventSpec{"fast", std::chrono::milliseconds(20), {"sig0"}});
    Collector collector;
    // 回调内阻塞 60 ms（> 周期）：调度线程必然超期，验证"跳过不补读"
    rig.session->Start([&collector](const PollSample& sample) {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        collector(sample);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    rig.session->Stop();

    const auto statistics = rig.session->Statistics();
    EXPECT_GT(statistics.skipped_overruns, 0U);
    // 400 ms / 60 ms ≈ 6~7 次；若"补读"则会逼近 20 次
    EXPECT_GE(statistics.ticks, 3U);
    EXPECT_LE(statistics.ticks, 10U);
}

TEST(PollSessionSkeleton, StartRejectedWhenDaqRunning) {
    Rig rig;
    rig.slave.session_status.store(0x40U);  // Current Session Status bit6 = DAQ_RUNNING
    rig.session->AddEvent(
        PollEventSpec{"fast", std::chrono::milliseconds(20), {"sig0"}});
    EXPECT_THROW(rig.session->Start([](const PollSample&) {}), XcpException);
    EXPECT_FALSE(rig.session->IsRunning());
}

TEST(PollSessionSkeleton, TimestampIsMonotonicPerEvent) {
    Rig rig;
    rig.session->AddEvent(
        PollEventSpec{"fast", std::chrono::milliseconds(20), {"sig0"}});
    Collector collector;
    rig.session->Start([&collector](const PollSample& sample) { collector(sample); });
    ASSERT_TRUE(collector.WaitFor(5, std::chrono::milliseconds(2000)));
    rig.session->Stop();

    const auto samples = collector.Samples();
    ASSERT_GE(samples.size(), 5U);
    for (std::size_t i = 1; i < samples.size(); ++i) {
        EXPECT_LT(samples[i - 1].steady_time, samples[i].steady_time);
    }
    // §3.4：t_begin / t_end 都保留，区间覆盖真实采样时刻
    for (const auto& sample : samples) {
        EXPECT_GE(sample.steady_time_end, sample.steady_time);
    }
}

TEST(PollSessionAddressMerge, AdjacentVariablesShareOneRead) {
    Rig rig;
    rig.session->AddEvent(PollEventSpec{"fast", std::chrono::milliseconds(20),
                                        {"sig0", "sig1", "sig2", "sig3"}});
    Collector collector;
    rig.session->Start([&collector](const PollSample& sample) { collector(sample); });
    ASSERT_TRUE(collector.WaitFor(1, std::chrono::milliseconds(2000)));
    rig.session->Stop();

    const auto samples = collector.Samples();
    ASSERT_FALSE(samples.empty());
    ASSERT_EQ(samples.front().samples.size(), 4U);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_TRUE(samples.front().samples[i].valid);
        EXPECT_EQ(std::get<std::uint64_t>(samples.front().samples[i].value),
                  static_cast<std::uint64_t>(0x10U * (i + 1)));
    }
    // 四个变量地址首尾相接，应合并成一次读取
    EXPECT_EQ(rig.slave.short_upload_count.load(), 1);
}

TEST(PollSessionAddressMerge, NonAdjacentVariablesAreReadSeparately) {
    Rig rig;
    // 只取 0x1000 与 0x1002：中间空出 0x1001，不得跨越空隙合并
    rig.database.symbols.erase("sig1");
    rig.session->AddEvent(PollEventSpec{"fast", std::chrono::milliseconds(20),
                                        {"sig0", "sig2"}});
    Collector collector;
    rig.session->Start([&collector](const PollSample& sample) { collector(sample); });
    ASSERT_TRUE(collector.WaitFor(1, std::chrono::milliseconds(2000)));
    rig.session->Stop();
    EXPECT_EQ(rig.slave.short_upload_count.load(), 2);
}

TEST(PollSessionAddressMerge, DifferentExtensionsAreNotMerged) {
    Rig rig;
    // 地址相接但 address extension 不同：不同页/段的内存，绝不能合并
    rig.database.symbols["sig1"].info.extension = 1;
    rig.session->AddEvent(PollEventSpec{"fast", std::chrono::milliseconds(20),
                                        {"sig0", "sig1"}});
    Collector collector;
    rig.session->Start([&collector](const PollSample& sample) { collector(sample); });
    ASSERT_TRUE(collector.WaitFor(1, std::chrono::milliseconds(2000)));
    rig.session->Stop();
    EXPECT_EQ(rig.slave.short_upload_count.load(), 2);
}

TEST(PollSessionAddressMerge, MergedReadFailureFallsBackPerVariable) {
    Rig rig;
    rig.slave.fail_next_short_uploads.store(1);  // 只让首次（合并）读失败
    rig.session->AddEvent(PollEventSpec{"fast", std::chrono::milliseconds(20),
                                        {"sig0", "sig1", "sig2", "sig3"}});
    Collector collector;
    rig.session->Start([&collector](const PollSample& sample) { collector(sample); });
    ASSERT_TRUE(collector.WaitFor(1, std::chrono::milliseconds(2000)));
    rig.session->Stop();

    const auto samples = collector.Samples();
    ASSERT_FALSE(samples.empty());
    // 合并读失败后逐变量回退，四个变量仍应各自读成功
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_TRUE(samples.front().samples[i].valid);
    }
    EXPECT_EQ(rig.slave.short_upload_count.load(), 5);  // 1 次合并 + 4 次回退
}

}  // namespace
// ---- 性能窗口（P-B）：poll 阶段埋点 ----

namespace {

/// @brief 从快照里取指定阶段的窗口累计值
/// @details 按值返回：窗口结构很小（16 个桶 + 三个标量），复制比持有引用更安全。
StageWindow StageOf(const PollPerfSnapshot& snapshot, PerfStage stage) {
    return snapshot.window.Stage(stage);
}

/// @brief 在慢变量榜单里查找「阶段 + 符号」条目，找不到返回 nullptr
const SlowSpot* FindSpot(const PollPerfSnapshot& snapshot, PerfStage stage,
                         const std::string& symbol) {
    for (const auto& spot : snapshot.window.slowest) {
        if (spot.stage == stage && spot.Symbol() == symbol) {
            return &spot;
        }
    }
    return nullptr;
}

}  // namespace

TEST(PollSessionPerf, SnapshotReportsTickAndStageTotals) {
    Rig rig;
    PerfConfig config;  // 默认窗口 5 s，用例执行期间不会触发窗口日志
    config.log_to_logger = false;
    rig.session->SetPerfConfig(config);
    rig.session->AddEvent(
        PollEventSpec{"fast", std::chrono::milliseconds(20), {"sig0", "sig1"}});

    Collector collector;
    rig.session->Start([&collector](const PollSample& sample) { collector(sample); });
    ASSERT_TRUE(collector.WaitFor(4, std::chrono::milliseconds(3000)));
    rig.session->Stop();

    const auto snapshot = rig.session->PerfSnapshot();
    ASSERT_GE(snapshot.ticks, 4U);
    // 变量数与合并组数按 tick 累加，所以窗口内等于 ticks 乘以每 tick 的个数
    EXPECT_EQ(snapshot.variables, snapshot.ticks * 2U);
    EXPECT_EQ(snapshot.groups, snapshot.ticks);
    // 阶段按每 tick 记一次，计数应与 ticks 一一对应
    EXPECT_EQ(StageOf(snapshot, PerfStage::TickTotal).count, snapshot.ticks);
    EXPECT_EQ(StageOf(snapshot, PerfStage::GroupRead).count, snapshot.ticks);
    EXPECT_EQ(StageOf(snapshot, PerfStage::Convert).count, snapshot.ticks);
    EXPECT_EQ(StageOf(snapshot, PerfStage::ScheduleGap).count, snapshot.ticks);
    // 可加和：读组 + 换算 + 装配不超过 tick 总耗时（schedule_gap 在总耗时之外）
    const double sum_ms = StageOf(snapshot, PerfStage::GroupRead).sum_ms +
                          StageOf(snapshot, PerfStage::SingleRead).sum_ms +
                          StageOf(snapshot, PerfStage::Convert).sum_ms +
                          StageOf(snapshot, PerfStage::Assemble).sum_ms;
    EXPECT_LE(sum_ms, StageOf(snapshot, PerfStage::TickTotal).sum_ms + 1e-6);
}

TEST(PollSessionPerf, MergedGroupBytesAreReported) {
    Rig rig;
    PerfConfig config;
    config.log_to_logger = false;
    rig.session->SetPerfConfig(config);
    // sig0..sig3 在 0x1000..0x1003 首尾相接（各 1 字节）→ 每 tick 一次 4 字节合并读
    rig.session->AddEvent(PollEventSpec{"fast", std::chrono::milliseconds(20),
                                        {"sig0", "sig1", "sig2", "sig3"}});

    Collector collector;
    rig.session->Start([&collector](const PollSample& sample) { collector(sample); });
    ASSERT_TRUE(collector.WaitFor(3, std::chrono::milliseconds(3000)));
    rig.session->Stop();

    const auto snapshot = rig.session->PerfSnapshot();
    ASSERT_GE(snapshot.ticks, 3U);
    EXPECT_EQ(snapshot.variables, snapshot.ticks * 4U);
    EXPECT_EQ(snapshot.groups, snapshot.ticks);  // 四个变量合成一组
    EXPECT_EQ(snapshot.group_slices_total, snapshot.ticks * 4U);
    EXPECT_EQ(snapshot.group_bytes_total, snapshot.ticks * 4U);
    EXPECT_EQ(snapshot.group_bytes_max, 4U);
    EXPECT_EQ(snapshot.single_read_bytes, 0U);

    const auto* spot = FindSpot(snapshot, PerfStage::GroupRead, "sig0");
    ASSERT_NE(spot, nullptr);
    EXPECT_EQ(spot->group_size, 4U);
    EXPECT_EQ(spot->group_bytes, 4U);
}

TEST(PollSessionPerf, ConvertSpotsAreRecordedPerSymbol) {
    Rig rig;
    PerfConfig config;
    config.log_to_logger = false;
    rig.session->SetPerfConfig(config);
    rig.session->AddEvent(
        PollEventSpec{"fast", std::chrono::milliseconds(20), {"sig0", "sig1"}});

    Collector collector;
    rig.session->Start([&collector](const PollSample& sample) { collector(sample); });
    ASSERT_TRUE(collector.WaitFor(2, std::chrono::milliseconds(3000)));
    rig.session->Stop();

    const auto snapshot = rig.session->PerfSnapshot();
    ASSERT_FALSE(snapshot.window.slowest.empty());
    // 每个变量的换算耗时各自成槽，才能回答"慢在哪个变量"
    EXPECT_NE(FindSpot(snapshot, PerfStage::Convert, "sig0"), nullptr);
    EXPECT_NE(FindSpot(snapshot, PerfStage::Convert, "sig1"), nullptr);
}

TEST(PollSessionPerf, DisabledConfigCollectsNoStages) {
    Rig rig;
    PerfConfig config;
    config.enabled = false;
    config.log_to_logger = false;
    rig.session->SetPerfConfig(config);
    rig.session->AddEvent(
        PollEventSpec{"fast", std::chrono::milliseconds(20), {"sig0"}});

    Collector collector;
    rig.session->Start([&collector](const PollSample& sample) { collector(sample); });
    ASSERT_TRUE(collector.WaitFor(2, std::chrono::milliseconds(3000)));
    rig.session->Stop();

    const auto snapshot = rig.session->PerfSnapshot();
    // tick 计数与采样开关无关（关掉统计仍要知道跑了多少 tick）
    EXPECT_GE(snapshot.ticks, 2U);
    EXPECT_EQ(StageOf(snapshot, PerfStage::TickTotal).count, 0U);
    EXPECT_TRUE(snapshot.window.slowest.empty());
}

TEST(PollSessionPerf, SnapshotResetsWindowCounters) {
    Rig rig;
    PerfConfig config;
    config.log_to_logger = false;
    rig.session->SetPerfConfig(config);
    rig.session->AddEvent(
        PollEventSpec{"fast", std::chrono::milliseconds(20), {"sig0"}});

    Collector collector;
    rig.session->Start([&collector](const PollSample& sample) { collector(sample); });
    ASSERT_TRUE(collector.WaitFor(2, std::chrono::milliseconds(3000)));
    rig.session->Stop();

    const auto first = rig.session->PerfSnapshot();
    ASSERT_GT(first.ticks, 0U);
    // 取快照即归零：第二次拿到的是空窗口
    const auto second = rig.session->PerfSnapshot();
    EXPECT_EQ(second.ticks, 0U);
    EXPECT_EQ(second.group_bytes_total, 0U);
    EXPECT_TRUE(second.window.slowest.empty());
}

}  // namespace calmcar::xcp