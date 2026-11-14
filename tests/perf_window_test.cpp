/**
 * @file perf_window_test.cpp
 * @brief 滑动窗口性能聚合器的单元测试。
 *
 * 全部用注入的假时钟，断言与机器速度无关；不做真实计时。
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>

#include "libxcp/detail/perf_window.hpp"

namespace {

using calmcar::xcp::kPerfBucketCount;
using calmcar::xcp::kPerfStageCount;
using calmcar::xcp::PerfConfig;
using calmcar::xcp::PerfConfigFromEnvironment;
using calmcar::xcp::PerfStage;
using calmcar::xcp::StageWindow;
using calmcar::xcp::WindowAggregator;

/**
 * @brief 可手动推进的假时钟。
 */
class FakeClock {
public:
    /// @brief 当前时刻。
    std::chrono::steady_clock::time_point now{};

    /// @brief 取给聚合器用的回调。
    WindowAggregator::NowFn Fn() {
        return [this] { return now; };
    }

    /// @brief 前进指定毫秒。
    void Advance(std::chrono::milliseconds delta) { now += delta; }
};

/**
 * @brief 环境变量的作用域守卫，析构时恢复原值。
 */
class ScopedEnv {
public:
    /// @brief 设置变量。
    ScopedEnv(const char* name, const char* value) : name_(name) {
        const std::string previous = calmcar::xcp::detail::PerfEnvironmentValue(name);
        if (!previous.empty()) {
            had_value_ = true;
            previous_ = previous;
        }
        Assign(value);
    }

    /// @brief 恢复原值。
    ~ScopedEnv() { Assign(had_value_ ? previous_.c_str() : nullptr); }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    /// @brief 写入或清除变量。
    void Assign(const char* value) {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value == nullptr ? "" : value);
#else
        if (value == nullptr) {
            unsetenv(name_.c_str());
        } else {
            setenv(name_.c_str(), value, 1);
        }
#endif
    }

    /// @brief 变量名。
    std::string name_;

    /// @brief 原值是否存在。
    bool had_value_{false};

    /// @brief 原值。
    std::string previous_;
};

}  // namespace

/**
 * @brief 空聚合器：未开始、不 Due、快照为空。
 */
TEST(PerfWindow, EmptyAggregatorIsNotDue) {
    FakeClock clock;
    WindowAggregator aggregator(PerfConfig{}, clock.Fn());

    EXPECT_FALSE(aggregator.Started());
    EXPECT_FALSE(aggregator.Due(clock.now));
    EXPECT_EQ(aggregator.Events(), 0U);

    const auto snapshot = aggregator.TakeSnapshot(clock.now);
    EXPECT_TRUE(snapshot.Empty());
    EXPECT_TRUE(snapshot.slowest.empty());
}

/**
 * @brief 阶段累计：count/sum/max 正确，桶计数之和等于样本数。
 */
TEST(PerfWindow, AccumulatesStageTotalsAndBuckets) {
    FakeClock clock;
    WindowAggregator aggregator(PerfConfig{}, clock.Fn());

    aggregator.Record(PerfStage::Convert, 1.0);
    aggregator.Record(PerfStage::Convert, 2.0);
    aggregator.Record(PerfStage::Convert, 0.5);

    const auto snapshot = aggregator.TakeSnapshot(clock.now);
    const auto& stage = snapshot.Stage(PerfStage::Convert);
    EXPECT_EQ(stage.count, 3U);
    EXPECT_DOUBLE_EQ(stage.sum_ms, 3.5);
    EXPECT_DOUBLE_EQ(stage.max_ms, 2.0);
    EXPECT_NEAR(stage.MeanMs(), 3.5 / 3.0, 1e-9);

    std::uint32_t bucket_total = 0;
    for (const auto value : stage.buckets) {
        bucket_total += value;
    }
    EXPECT_EQ(bucket_total, 3U);
}

/**
 * @brief 负耗时按 0 处理，不产生负累计。
 */
TEST(PerfWindow, NegativeDurationIsClampedToZero) {
    FakeClock clock;
    WindowAggregator aggregator(PerfConfig{}, clock.Fn());

    aggregator.Record(PerfStage::Callback, -5.0);

    const auto snapshot = aggregator.TakeSnapshot(clock.now);
    const auto& stage = snapshot.Stage(PerfStage::Callback);
    EXPECT_EQ(stage.count, 1U);
    EXPECT_DOUBLE_EQ(stage.sum_ms, 0.0);
    EXPECT_DOUBLE_EQ(stage.max_ms, 0.0);
}

/**
 * @brief 窗口到点判定：差 1 ms 不到点，正好到点即为 true。
 */
TEST(PerfWindow, DueBecomesTrueAtWindowBoundary) {
    FakeClock clock;
    PerfConfig config;
    config.window = std::chrono::milliseconds(5000);
    WindowAggregator aggregator(config, clock.Fn());

    aggregator.Record(PerfStage::TickTotal, 1.0);
    EXPECT_FALSE(aggregator.Due(clock.now + std::chrono::milliseconds(4999)));
    EXPECT_TRUE(aggregator.Due(clock.now + std::chrono::milliseconds(5000)));
}

/**
 * @brief 取快照后归零，第二份快照为空。
 */
TEST(PerfWindow, SnapshotResetsCounters) {
    FakeClock clock;
    WindowAggregator aggregator(PerfConfig{}, clock.Fn());

    aggregator.Record(PerfStage::Convert, 1.0);
    aggregator.Record(PerfStage::Assemble, 0.2);

    const auto first = aggregator.TakeSnapshot(clock.now);
    EXPECT_EQ(first.samples, 2U);
    EXPECT_FALSE(first.Empty());

    EXPECT_FALSE(aggregator.Started());
    EXPECT_EQ(aggregator.Events(), 0U);

    clock.Advance(std::chrono::milliseconds(10));
    aggregator.Record(PerfStage::Convert, 3.0);
    const auto second = aggregator.TakeSnapshot(clock.now);
    EXPECT_EQ(second.Stage(PerfStage::Convert).count, 1U);
    EXPECT_EQ(second.Stage(PerfStage::Assemble).count, 0U);
}

/**
 * @brief 采样率：事件全计数，样本按 N 抽。
 */
TEST(PerfWindow, SampleEveryThinsSamplesButCountsEvents) {
    FakeClock clock;
    PerfConfig config;
    config.sample_every = 4;
    WindowAggregator aggregator(config, clock.Fn());

    for (int i = 0; i < 8; ++i) {
        aggregator.Record(PerfStage::Convert, 1.0);
    }

    const auto snapshot = aggregator.TakeSnapshot(clock.now);
    EXPECT_EQ(snapshot.events, 8U);
    EXPECT_EQ(snapshot.samples, 2U);
    EXPECT_EQ(snapshot.Stage(PerfStage::Convert).count, 2U);
}

/**
 * @brief 关闭时不采样、不开始窗口。
 */
TEST(PerfWindow, DisabledConfigRecordsNothing) {
    FakeClock clock;
    PerfConfig config;
    config.enabled = false;
    WindowAggregator aggregator(config, clock.Fn());

    aggregator.Record(PerfStage::Convert, 1.0);

    EXPECT_FALSE(aggregator.Started());
    EXPECT_EQ(aggregator.Events(), 0U);
    EXPECT_TRUE(aggregator.TakeSnapshot(clock.now).Empty());
}

/**
 * @brief 运行期换配置：在下一次采样时才生效。
 */
TEST(PerfWindow, PendingConfigAppliesOnNextRecord) {
    FakeClock clock;
    PerfConfig config;
    config.window = std::chrono::milliseconds(5000);
    WindowAggregator aggregator(config, clock.Fn());

    aggregator.Record(PerfStage::Convert, 1.0);

    PerfConfig next;
    next.window = std::chrono::milliseconds(1000);
    aggregator.SetConfig(next);

    // 旧配置仍生效：5 s 窗口还没到。
    EXPECT_FALSE(aggregator.Due(clock.now + std::chrono::milliseconds(2000)));

    // 下一次采样应用新配置，于是 2 s 已超过 1 s 窗口。
    aggregator.Record(PerfStage::Convert, 1.0);
    EXPECT_EQ(aggregator.ActiveConfig().window, std::chrono::milliseconds(1000));
    EXPECT_TRUE(aggregator.Due(clock.now + std::chrono::milliseconds(2000)));
}

/**
 * @brief slowest 按窗口累计耗时降序，容量满后替换最弱者。
 */
TEST(PerfWindow, SlowestKeepsTopBySum) {
    FakeClock clock;
    PerfConfig config;
    config.slowest_capacity = 2;
    WindowAggregator aggregator(config, clock.Fn());

    aggregator.Record(PerfStage::Convert, 10.0, "a");
    aggregator.Record(PerfStage::Convert, 20.0, "b");
    aggregator.Record(PerfStage::Convert, 30.0, "c");

    const auto snapshot = aggregator.TakeSnapshot(clock.now);
    ASSERT_EQ(snapshot.slowest.size(), 2U);
    EXPECT_EQ(snapshot.slowest[0].Symbol(), "c");
    EXPECT_EQ(snapshot.slowest[1].Symbol(), "b");
}

/**
 * @brief 同一符号多次采样累加到同一个槽。
 */
TEST(PerfWindow, SlowestAccumulatesRepeatedSymbol) {
    FakeClock clock;
    WindowAggregator aggregator(PerfConfig{}, clock.Fn());

    aggregator.Record(PerfStage::Convert, 2.0, "sig");
    aggregator.Record(PerfStage::Convert, 2.0, "sig");
    aggregator.Record(PerfStage::Convert, 2.0, "sig");

    const auto snapshot = aggregator.TakeSnapshot(clock.now);
    ASSERT_EQ(snapshot.slowest.size(), 1U);
    EXPECT_EQ(snapshot.slowest[0].count, 3U);
    EXPECT_DOUBLE_EQ(snapshot.slowest[0].sum_ms, 6.0);
    EXPECT_DOUBLE_EQ(snapshot.slowest[0].max_ms, 2.0);
}

/**
 * @brief 不同阶段同名符号占不同槽。
 */
TEST(PerfWindow, SlowestSeparatesStages) {
    FakeClock clock;
    WindowAggregator aggregator(PerfConfig{}, clock.Fn());

    aggregator.Record(PerfStage::Convert, 5.0, "sig");
    aggregator.Record(PerfStage::GroupRead, 3.0, "sig");

    const auto snapshot = aggregator.TakeSnapshot(clock.now);
    EXPECT_EQ(snapshot.slowest.size(), 2U);
}

/**
 * @brief 容量为 0 时不收集慢点。
 */
TEST(PerfWindow, ZeroSlowestCapacityCollectsNothing) {
    FakeClock clock;
    PerfConfig config;
    config.slowest_capacity = 0;
    WindowAggregator aggregator(config, clock.Fn());

    aggregator.Record(PerfStage::Convert, 5.0, "sig");

    const auto snapshot = aggregator.TakeSnapshot(clock.now);
    EXPECT_TRUE(snapshot.slowest.empty());
    EXPECT_EQ(snapshot.Stage(PerfStage::Convert).count, 1U);
}

/**
 * @brief 合并读组槽保留组内变量数与字节数。
 */
TEST(PerfWindow, GroupReadSpotKeepsGroupShape) {
    FakeClock clock;
    WindowAggregator aggregator(PerfConfig{}, clock.Fn());

    aggregator.Record(PerfStage::GroupRead, 1.5, "grp_head", 4U, 56U);

    const auto snapshot = aggregator.TakeSnapshot(clock.now);
    ASSERT_EQ(snapshot.slowest.size(), 1U);
    EXPECT_EQ(snapshot.slowest[0].stage, PerfStage::GroupRead);
    EXPECT_EQ(snapshot.slowest[0].group_size, 4U);
    EXPECT_EQ(snapshot.slowest[0].group_bytes, 56U);
}

/**
 * @brief 分位数：多数 1 ms + 少数 10 ms，p50 落在低段、p95 落在高段。
 */
TEST(StageWindow, PercentileFollowsDistribution) {
    StageWindow window;
    for (int i = 0; i < 90; ++i) {
        window.Add(1.0);
    }
    for (int i = 0; i < 10; ++i) {
        window.Add(10.0);
    }

    const double p50 = window.PercentileMs(0.50);
    const double p95 = window.PercentileMs(0.95);
    EXPECT_GE(p50, 1.0);
    EXPECT_LE(p50, 2.0);
    EXPECT_GE(p95, 9.9);
    EXPECT_LE(p95, 20.0);
}

/**
 * @brief 空阶段的均值与分位数都是 0，不产生 NaN。
 */
TEST(StageWindow, EmptyWindowReturnsZero) {
    const StageWindow window;
    EXPECT_TRUE(window.Empty());
    EXPECT_DOUBLE_EQ(window.MeanMs(), 0.0);
    EXPECT_DOUBLE_EQ(window.PercentileMs(0.95), 0.0);
}

/**
 * @brief 桶边界与桶下标的基本约定。
 */
TEST(PerfWindow, BucketIndexIsMonotonic) {
    std::size_t previous = 0U;
    for (const double value : {0.0, 0.05, 0.1, 0.3, 1.0, 5.0, 900.0, 100000.0}) {
        const auto index = calmcar::xcp::PerfBucketIndex(value);
        ASSERT_LT(index, kPerfBucketCount);
        EXPECT_GE(index, previous);
        previous = index;
    }
}

/**
 * @brief 阶段名非空且可区分。
 */
TEST(PerfWindow, StageNamesAreStable) {
    EXPECT_STREQ(calmcar::xcp::PerfStageName(PerfStage::Convert), "convert");
    EXPECT_STREQ(calmcar::xcp::PerfStageName(PerfStage::E2e), "e2e");
    EXPECT_STREQ(calmcar::xcp::PerfStageName(PerfStage::Count), "unknown");
    EXPECT_LT(static_cast<std::size_t>(PerfStage::E2e), kPerfStageCount);
}

/**
 * @brief 环境变量覆盖缺省配置。
 */
TEST(PerfConfig, EnvironmentOverridesDefaults) {
    ScopedEnv enabled("LIBXCP_PERF_ENABLED", "1");
    ScopedEnv window("LIBXCP_PERF_WINDOW_MS", "1000");
    ScopedEnv every("LIBXCP_PERF_SAMPLE_EVERY", "4");
    ScopedEnv slowest("LIBXCP_PERF_SLOWEST", "3");
    ScopedEnv verbose("LIBXCP_PERF_VERBOSE_STAGES", "0");

    const auto config = PerfConfigFromEnvironment();
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.window, std::chrono::milliseconds(1000));
    EXPECT_EQ(config.sample_every, 4U);
    EXPECT_EQ(config.slowest_capacity, 3U);
    EXPECT_FALSE(config.verbose_stages);
}

/**
 * @brief 非法环境变量不关闭统计，回落到缺省值。
 */
TEST(PerfConfig, InvalidEnvironmentKeepsDefaults) {
    ScopedEnv window("LIBXCP_PERF_WINDOW_MS", "not-a-number");

    const auto config = PerfConfigFromEnvironment();
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.window, std::chrono::milliseconds(5000));
}

/**
 * @brief 配置相等比较覆盖所有字段。
 */
TEST(PerfConfig, EqualityComparesEveryField) {
    PerfConfig lhs;
    PerfConfig rhs;
    EXPECT_EQ(lhs, rhs);

    rhs.slowest_capacity = 7;
    EXPECT_NE(lhs, rhs);

    rhs = lhs;
    rhs.verbose_stages = false;
    EXPECT_NE(lhs, rhs);
}
