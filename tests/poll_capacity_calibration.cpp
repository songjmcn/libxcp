/**
 * @file poll_capacity_calibration.cpp
 * @brief P4：20 ms 周期下的 poll 容量标定（真实 UDP Slave）。
 *
 * 依据 `code-plan/Poll模式多Event架构实施计划.md` §9 批次 P4 与 §11.1 验收口径：
 *   - 只测**单 event**、周期 20 ms、运行窗口 ≥ 10 s；
 *   - 变量地址**两两不相邻**，使 P3 的地址合并必然失效，从而每个变量占一次往返
 *     （这是最坏情况，也是"20 ms 内最多能轮询多少变量"的直接度量）；
 *   - 判据：超期比例 ≤ 1%（见 @ref kMaxOverrunRatio 对计划口径的修正说明）、
 *     `read_errors == 0`、`callback_errors == 0`、平均周期偏差 ≤ 5%（平均间隔 ≤ 21 ms）；
 *   - 倍增找出上界后在区间内二分，输出完整测量表与最终容量。
 *
 * 为什么用 `g_blob`：XCPlite Slave 的 `g_blob` 是 600 字节的可读 U8 区，
 * 取其中每隔 @ref kProbeStride 字节的一个位置作为探针地址，即可在不新增
 * Slave 变量的前提下得到 150 个**真实存在且两两不相邻**的单字节地址。
 *
 * 运行方式（手动，不进入 `libxcp_tests` 常规套件）：
 * @code
 *   ./cmake-build-release/tests/XcpliteIntegration.exe \
 *       --gtest_filter='*PollCapacity*'
 * @endcode
 * 可用环境变量 `LIBXCP_POLL_CALIBRATION_WINDOW_MS` 缩短窗口以便快速自检，
 * 但正式标定必须保持默认 10000 ms。
 */

#if defined(_MSC_VER)
// 本文件仅读取自有的只读环境变量，MSVC 的 getenv 不安全告警不适用
#pragma warning(disable : 4996)
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "libxcp/detail/perf_window.hpp"
#include "libxcp/measurement/measurement_database.hpp"
#include "libxcp/poll/poll_session.hpp"
#include "libxcp/tcp_transport.hpp"
#include "libxcp/tcp_transport_config.hpp"
#include "libxcp/xcp_master.hpp"
#include "xcplite_slave_fixture.hpp"

namespace calmcar::xcp::test {
namespace {

using namespace std::chrono_literals;

/// @brief 探针之间的地址间隔（> 单元素宽度，保证互不相邻、合并必然失效）
constexpr std::uint32_t kProbeStride = 4U;

/// @brief 探针容量上限（= g_blob 的 600 字节 / 4）
constexpr std::uint32_t kProbeCapacity = 150U;

/// @brief 平均周期允许的最大偏差（5%，实施计划 §11.1）
constexpr double kMaxPeriodDeviation = 0.05;

/// @brief 允许的超期比例上限（1%）。
/// @details 实施计划 §11.1 原写"`skipped_overruns == 0`"，但 Windows 不是实时 OS：
///          实测即使在 N=1 时相邻 tick 间隔也会出现约 32 ms 的抖动（比 20 ms 周期
///          多出约 12 ms），10 s 窗口内偶尔必然触发一次超期跳过。因此判据改为
///          **超期比例**而非绝对次数——这仍然是"能否稳定按 20 ms 采样"的有效度量，
///          且对 N 有单调区分力（实测 N=99 时超期率约 2.6%）。
constexpr double kMaxOverrunRatio = 0.01;

/**
 * @brief 标定窗口时长。
 * @details 默认 10 s（实施计划 §11.1 要求 ≥ 10 s）；环境变量仅用于快速自检。
 */
std::chrono::milliseconds CalibrationWindow() {
    if (const char* env = std::getenv("LIBXCP_POLL_CALIBRATION_WINDOW_MS")) {
        const long long value = std::atoll(env);
        if (value > 0) {
            return std::chrono::milliseconds{value};
        }
    }
    return std::chrono::milliseconds{10000};
}

/**
 * @brief 在 A2L 文本中按符号名定位 32 位地址。
 * @details XCPlite 生成的 A2L 每行一个对象，地址是该行第一个 `0x` 字面量
 *          （`MEASUREMENT` 为 `ECU_ADDRESS 0x...`，`CHARACTERISTIC` 为
 *          `VAL_BLK 0x...`）。只用于测试夹具，不是通用 A2L 解析器。
 * @param a2l_path A2L 文件路径
 * @param symbol 符号名
 * @return 解析到的地址；未找到时 nullopt
 */
std::optional<std::uint32_t> FindAddressByName(const std::filesystem::path& a2l_path,
                                               std::string_view symbol) {
    std::ifstream stream(a2l_path);
    if (!stream) {
        return std::nullopt;
    }
    std::string line;
    while (std::getline(stream, line)) {
        if (line.find(symbol) == std::string::npos) {
            continue;
        }
        const std::size_t marker = line.find("0x");
        if (marker == std::string::npos) {
            continue;
        }
        const char* begin = line.c_str() + marker + 2;
        char* end = nullptr;
        const unsigned long value = std::strtoul(begin, &end, 16);
        if (end == begin) {
            continue;
        }
        return static_cast<std::uint32_t>(value);
    }
    return std::nullopt;
}

/**
 * @brief 标定用最小数据库：把 `probe_<i>` 映射到 blob 内第 i 个探针地址。
 * @details 每个探针都是 1 字节标量，地址间隔 @ref kProbeStride，
 *          因此任意两个探针都不会被 P3 的地址合并归入同一读取组。
 */
class BlobProbeDatabase final : public IMeasurementDatabase {
public:
    /**
     * @brief 构造
     * @param base blob 起始地址（来自 A2L，不硬编码）
     * @param extension 地址扩展（XCPlite 固定为 1）
     */
    BlobProbeDatabase(Address base, AddressExtension extension)
        : base_(base), extension_(extension) {}

    [[nodiscard]] MeasurementResult<MeasurementSymbolInfo> Find(
        std::string_view name) const override {
        std::size_t index = 0;
        if (!ParseProbeName(name, index)) {
            return MeasurementError{MeasurementErrorCode::NotFound,
                                    "不是本标定用例的探针符号", std::string{name}};
        }
        MeasurementSymbolInfo info;
        info.address = base_ + static_cast<Address>(index * kProbeStride);
        info.extension = extension_;
        info.element_size_bytes = 1;
        info.element_count = 1;
        info.is_array = false;
        return info;
    }

    [[nodiscard]] MeasurementResult<MeasurementValue> ToPhysical(
        std::string_view name, BytesView raw) const override {
        if (raw.size() != 1U) {
            return MeasurementError{MeasurementErrorCode::RawSizeMismatch,
                                    "探针为单字节无符号量", std::string{name}};
        }
        return MeasurementValue{static_cast<std::uint64_t>(raw[0])};
    }

private:
    /**
     * @brief 解析 `probe_<十进制下标>`。
     * @param name 符号名
     * @param[out] index 解析出的下标
     * @return 是否匹配
     */
    static bool ParseProbeName(std::string_view name, std::size_t& index) {
        constexpr std::string_view kPrefix = "probe_";
        if (name.size() <= kPrefix.size() ||
            name.substr(0, kPrefix.size()) != kPrefix) {
            return false;
        }
        std::size_t value = 0;
        for (const char ch : name.substr(kPrefix.size())) {
            if (ch < '0' || ch > '9') {
                return false;
            }
            value = value * 10U + static_cast<std::size_t>(ch - '0');
        }
        if (value >= kProbeCapacity) {
            return false;
        }
        index = value;
        return true;
    }

    Address base_{0};              ///< blob 起始地址
    AddressExtension extension_{0};  ///< 地址扩展
};

/// @brief 一次标定测量的结果
struct CapacityRun {
    std::size_t variables{0};            ///< 本次轮询的变量数
    PollStatistics stats{};              ///< 会话统计
    PollPerfSnapshot perf{};             ///< 本轮的性能窗口快照（阶段分解）
    double mean_interval_ms{0.0};        ///< 相邻 tick 的平均间隔
    double worst_interval_ms{0.0};       ///< 相邻 tick 的最大间隔
    double overrun_ratio{0.0};           ///< 超期比例（skipped / 计划 tick 总数）
    bool passed{false};                  ///< 是否满足全部判据
};

/**
 * @brief 用指定变量数跑一轮标定。
 * @param master 已连接主站
 * @param database 探针数据库
 * @param variables 变量数
 * @param window 运行窗口
 * @return 本轮测量结果
 */
CapacityRun RunOne(XcpMaster& master, const IMeasurementDatabase& database,
                   std::size_t variables, std::chrono::milliseconds window) {
    CapacityRun run;
    run.variables = variables;

    PollSession session(master, database);

    // 窗口设成比运行窗口更长：标定期间不输出任何窗口日志，
    // 跑完取一次快照即可得到整轮的阶段分解（避免日志噪声干扰测量）。
    calmcar::xcp::PerfConfig perf_config;
    perf_config.enabled = true;
    perf_config.window = window + std::chrono::milliseconds{60000};
    perf_config.log_to_logger = false;
    session.SetPerfConfig(perf_config);

    PollEventSpec spec;
    spec.name = "capacity";
    spec.period = PollSession::kMinPeriod;
    spec.symbols.reserve(variables);
    for (std::size_t i = 0; i < variables; ++i) {
        spec.symbols.push_back("probe_" + std::to_string(i));
    }
    session.AddEvent(std::move(spec));

    const MeasurementResult<void> validation = session.Validate();
    if (!validation) {
        std::printf("  N=%-4zu Validate 失败：%s\n", variables,
                    validation.ErrorInfo().message.c_str());
        return run;
    }

    std::mutex mutex;
    std::vector<std::chrono::steady_clock::time_point> stamps;
    session.Start([&mutex, &stamps](const PollSample& sample) {
        std::lock_guard<std::mutex> lock(mutex);
        stamps.push_back(sample.steady_time);
    });
    std::this_thread::sleep_for(window);
    session.Stop();
    run.stats = session.Statistics();
    run.perf = session.PerfSnapshot();

    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stamps.size() >= 2U) {
            const auto span = stamps.back() - stamps.front();
            const double span_ms =
                std::chrono::duration<double, std::milli>(span).count();
            run.mean_interval_ms = span_ms / static_cast<double>(stamps.size() - 1U);
            double worst_ms = 0.0;
            for (std::size_t i = 1; i < stamps.size(); ++i) {
                const double delta_ms = std::chrono::duration<double, std::milli>(
                                            stamps[i] - stamps[i - 1])
                                            .count();
                worst_ms = std::max(worst_ms, delta_ms);
            }
            run.worst_interval_ms = worst_ms;
        }
    }

    const double expected_ms =
        static_cast<double>(PollSession::kMinPeriod.count());
    const double deviation = run.mean_interval_ms > 0.0
                                 ? std::abs(run.mean_interval_ms - expected_ms) / expected_ms
                                 : 1.0;
    const double scheduled = static_cast<double>(run.stats.ticks + run.stats.skipped_overruns);
    run.overrun_ratio = scheduled > 0.0
                            ? static_cast<double>(run.stats.skipped_overruns) / scheduled
                            : 1.0;
    run.passed = run.stats.ticks >= 10U && run.overrun_ratio <= kMaxOverrunRatio &&
                 run.stats.read_errors == 0U && run.stats.callback_errors == 0U &&
                 deviation <= kMaxPeriodDeviation;
    return run;
}

/// @brief 打印一轮标定的阶段分解（P-E：把 poll 性能窗口接进容量报告）
/// @param run 本轮测量结果
/// @details 只打印一次读完的汇总，不含每次 tick 的明细；阶段耗时以
///          `TickTotal` 为分母算占比，`merged` 用 1 - groups/variables。
void PrintRunPerf(const CapacityRun& run) {
    const auto& window = run.perf.window;
    const auto stage = [&window](calmcar::xcp::PerfStage index) -> const StageWindow& {
        return window.stages[static_cast<std::size_t>(index)];
    };
    const StageWindow& total = stage(calmcar::xcp::PerfStage::TickTotal);
    if (window.samples == 0U || total.count == 0U) {
        std::printf("       perf: 无样本（窗口未启动或统计被关闭）\n");
        return;
    }

    const double total_sum = total.sum_ms > 0.0 ? total.sum_ms : 0.0;
    const auto share = [total_sum](const StageWindow& item) {
        return total_sum > 0.0 ? item.sum_ms * 100.0 / total_sum : 0.0;
    };

    const double groups_per_tick =
        run.perf.ticks > 0U
            ? static_cast<double>(run.perf.groups) / static_cast<double>(run.perf.ticks)
            : 0.0;
    const double merged = run.perf.variables > 0U
                              ? 100.0 *
                                    (1.0 - static_cast<double>(run.perf.groups) /
                                               static_cast<double>(run.perf.variables))
                              : 0.0;
    const double bytes_per_tick =
        run.perf.ticks > 0U
            ? static_cast<double>(run.perf.group_bytes_total) /
                  static_cast<double>(run.perf.ticks)
            : 0.0;

    const StageWindow& group = stage(calmcar::xcp::PerfStage::GroupRead);
    const StageWindow& single = stage(calmcar::xcp::PerfStage::SingleRead);
    const StageWindow& convert = stage(calmcar::xcp::PerfStage::Convert);
    const StageWindow& assemble = stage(calmcar::xcp::PerfStage::Assemble);

    std::printf(
        "       perf: tick mean=%.3f p95=%.3f max=%.3f ms | "
        "group=%.3f(%.0f%%) single=%.3f(%.0f%%) convert=%.3f(%.0f%%) "
        "assemble=%.3f(%.0f%%) | groups/tick=%.2f merged=%.1f%% bytes/tick=%.0fB\n",
        total.MeanMs(), total.PercentileMs(0.95), total.max_ms, group.MeanMs(),
        share(group), single.MeanMs(), share(single), convert.MeanMs(),
        share(convert), assemble.MeanMs(), share(assemble), groups_per_tick,
        merged, bytes_per_tick);
}

/// @brief 打印一行标定结果
void PrintRun(const CapacityRun& run) {
    std::printf(
        "  N=%-4zu ticks=%-5llu skip=%-4llu(%5.2f%%) read_err=%-4llu cb_err=%-3llu "
        "mean=%7.3f ms worst=%7.3f ms  %s\n",
        run.variables, static_cast<unsigned long long>(run.stats.ticks),
        static_cast<unsigned long long>(run.stats.skipped_overruns),
        run.overrun_ratio * 100.0,
        static_cast<unsigned long long>(run.stats.read_errors),
        static_cast<unsigned long long>(run.stats.callback_errors),
        run.mean_interval_ms, run.worst_interval_ms,
        run.passed ? "PASS" : "FAIL");
    PrintRunPerf(run);
}

/// @brief 一轮标定的结论
struct CalibrationResult {
    std::size_t max_variables{0};  ///< 20 ms 周期下可稳定轮询的变量数
    bool saturated{false};         ///< true = 已达探针上限，非真实上限
};

/**
 * @brief 倍增找上界后在区间内二分收敛（UDP / TCP 共用同一流程）
 * @param master 已建链的主站
 * @param database 探针数据库
 * @param window 每轮运行窗口
 * @return 结论；baseline 未达标时返回 `{0, false}`
 */
CalibrationResult CalibrateCapacity(XcpMaster& master,
                                    const IMeasurementDatabase& database,
                                    std::chrono::milliseconds window) {
    // 先确认 1 个变量能跑通，否则后续数字都没有意义
    const CapacityRun baseline = RunOne(master, database, 1, window);
    PrintRun(baseline);
    if (!baseline.passed) {
        return CalibrationResult{};
    }

    std::size_t last_pass = 1;
    std::size_t first_fail = 0;

    // 倍增找上界
    for (std::size_t n = 2; n <= kProbeCapacity; n *= 2) {
        const CapacityRun run = RunOne(master, database, n, window);
        PrintRun(run);
        if (!run.passed) {
            first_fail = n;
            break;
        }
        last_pass = n;
    }

    // 区间内二分（first_fail == 0 表示探针上限仍未失败）
    if (first_fail > 0) {
        while (first_fail > last_pass + 1U) {
            const std::size_t mid = last_pass + (first_fail - last_pass) / 2U;
            const CapacityRun run = RunOne(master, database, mid, window);
            PrintRun(run);
            if (run.passed) {
                last_pass = mid;
            } else {
                first_fail = mid;
            }
        }
    }

    return CalibrationResult{last_pass, first_fail == 0};
}

}  // namespace

/**
 * @brief P4 容量标定：20 ms 周期下单 event 能稳定轮询多少变量。
 * @details 倍增找上界后二分收敛；判据见文件头。本用例是**手动**运行的长耗时
 *          标定（默认 10 s × 约 9 轮），故意不挂到 `libxcp_tests`。
 */
TEST_F(XcpliteSlaveTest, PollCapacityAtTwentyMillisecondPeriod) {
    auto master = MakeConnectedMaster();
    ASSERT_NE(master, nullptr);

    const auto base = FindAddressByName(slave_.A2lPath(), "g_blob");
    ASSERT_TRUE(base.has_value()) << "A2L 中未找到 g_blob 地址，无法构造探针区";
    BlobProbeDatabase database{*base, 1};

    const auto window = CalibrationWindow();
    std::printf("\n[poll 容量标定] blob 基址=0x%X 步长=%u 上限=%u 窗口=%lld ms\n",
                static_cast<unsigned>(*base), kProbeStride, kProbeCapacity,
                static_cast<long long>(window.count()));

    const CalibrationResult result = CalibrateCapacity(*master, database, window);
    ASSERT_GT(result.max_variables, 1U) << "单变量 baseline 未达标或容量不足 2";
    std::printf(
        "[poll 容量标定] 结论：20 ms 周期下最多 %zu 个互不相邻变量%s\n\n",
        result.max_variables,
        result.saturated ? "（已达探针上限 150，非真实上限）"
                         : "（下一档即超期或周期偏差超标）");

    RecordProperty("max_variables_at_20ms", static_cast<int>(result.max_variables));
    RecordProperty("probe_capacity_saturated", result.saturated ? "true" : "false");
    EXPECT_GT(result.max_variables, 1U) << "20 ms 周期下连 2 个变量都无法稳定轮询";
}

/**
 * @brief TCP 传输下的 poll 容量标定夹具（真实 TCP Slave 进程）
 * @details 实施计划 §8.3 要求端到端用例同时覆盖 UDP 与 TCP；夹具独立于
 *          `XcpliteSlaveTest`，因为后者的 `MakeMaster()` 固定使用 UDP。
 */
class PollCapacityTcpTest : public ::testing::Test {
protected:
    void SetUp() override {
        try {
            slave_.Start();
            const auto a2l = slave_.A2lPath();
            ASSERT_TRUE(std::filesystem::exists(a2l)) << "A2L 不存在: " << a2l.string();
        } catch (const std::exception& e) {
            FAIL() << "XCPlite TCP Slave 启动失败: " << e.what();
        }
    }

    void TearDown() override { slave_.Stop(); }

    /**
     * @brief 构造指向 TCP Slave 且已 CONNECT 的主站
     * @return 持有独立 TcpTransport 的 XcpMaster
     * @details XCPlite 的 TCP 线路布局是 `[LEN][CTR][Packet]`，对应
     *          `TcpWireProfile::AsamV14`（与既有 TCP 互操作用例一致）。
     */
    [[nodiscard]] std::unique_ptr<XcpMaster> MakeConnectedTcpMaster() const {
        TcpTransportConfig cfg;
        cfg.role = TcpRole::Master;
        cfg.remote_host = "127.0.0.1";
        cfg.remote_port = slave_.Port();
        cfg.local_host = "127.0.0.1";
        cfg.local_port = 0;
        cfg.packet_alignment = PacketAlignment::Bit8;
        cfg.wire_profile = TcpWireProfile::AsamV14;
        cfg.connect_timeout_ms = 3000;
        cfg.send_timeout_ms = 3000;
        XcpMasterOptions options;
        options.timeouts = CommandTimeouts{std::chrono::milliseconds(2000),
                                           std::chrono::milliseconds(2000), 1};
        auto master = std::make_unique<XcpMaster>(
            std::make_unique<TcpTransport>(cfg), options);
        master->Connect();
        return master;
    }

    XcpliteSlaveProcess slave_{std::nullopt, 0, /*use_tcp=*/true};
};

/**
 * @brief P4 容量标定（TCP）：与 UDP 版本同法同判据，仅传输层不同。
 * @details 手动运行；判据与 UDP 版本完全一致（超期率 ≤ 1%、无读写错误、
 *          平均周期偏差 ≤ 5%），以便直接对比两种 transport 的容量差异。
 */
TEST_F(PollCapacityTcpTest, PollCapacityAtTwentyMillisecondPeriodOverTcp) {
    auto master = MakeConnectedTcpMaster();
    ASSERT_NE(master, nullptr);

    const auto base = FindAddressByName(slave_.A2lPath(), "g_blob");
    ASSERT_TRUE(base.has_value()) << "A2L 中未找到 g_blob 地址，无法构造探针区";
    BlobProbeDatabase database{*base, 1};

    const auto window = CalibrationWindow();
    std::printf("\n[poll 容量标定/TCP] blob 基址=0x%X 步长=%u 上限=%u 窗口=%lld ms\n",
                static_cast<unsigned>(*base), kProbeStride, kProbeCapacity,
                static_cast<long long>(window.count()));

    const CalibrationResult result = CalibrateCapacity(*master, database, window);
    ASSERT_GT(result.max_variables, 0U) << "TCP 下单变量 baseline 未达标";
    std::printf(
        "[poll 容量标定/TCP] 结论：20 ms 周期下最多 %zu 个互不相邻变量%s\n\n",
        result.max_variables,
        result.saturated ? "（已达探针上限 150，非真实上限）"
                         : "（下一档即超期或周期偏差超标）");

    RecordProperty("max_variables_at_20ms_tcp",
                   static_cast<int>(result.max_variables));
    RecordProperty("probe_capacity_saturated_tcp",
                   result.saturated ? "true" : "false");
    EXPECT_GT(result.max_variables, 1U) << "TCP 下 20 ms 周期连 2 个变量都无法稳定轮询";
}

}  // namespace calmcar::xcp::test