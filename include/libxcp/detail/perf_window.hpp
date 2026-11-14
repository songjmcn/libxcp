/**
 * @file perf_window.hpp
 * @brief 滑动窗口性能聚合器（header-only，无锁、无堆分配）。
 *
 * 设计要点见 code-plan/性能统计_滑动窗口与分阶段耗时_实施计划.md：
 * - 采样按事件（poll tick / DAQ 帧）累加，日志按墙钟窗口输出，两者解耦；
 * - 每个阶段独立累计 count / sum / max，并用 16 个固定桶估算 p50/p95/p99；
 * - slowest 槽记录「阶段 × 变量」的累计耗时，用来回答"慢在哪个变量"；
 * - 时钟可注入，便于单测做确定性断言。
 *
 * 本文件只做聚合，不做格式化与输出；日志格式由各 Session 负责。
 */

#ifndef CALMCAR_XCP_DETAIL_PERF_WINDOW_HPP_
#define CALMCAR_XCP_DETAIL_PERF_WINDOW_HPP_

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libxcp/log.hpp"

namespace calmcar::xcp {

/**
 * @brief 性能统计的阶段编号。
 *
 * 前 7 个属于 poll 路径，后 5 个属于 DAQ 路径；两个 Session 各自只用到自己
 * 那一段，共用同一个枚举是为了让聚合器与格式化代码只有一份。
 */
enum class PerfStage : std::uint8_t {
    ScheduleGap,     ///< poll：计划时刻到实际开始，调度抖动
    GroupRead,       ///< poll：合并读组（整组共担，不归属单个变量）
    SingleRead,      ///< poll：合并组失败后的单变量回退读
    Convert,         ///< poll：原始字节到物理量的换算
    Assemble,        ///< poll：结果装配与样本构造
    Callback,        ///< poll：用户回调
    TickTotal,       ///< poll：单 tick 总耗时（不含回调）
    RxEnqueue,       ///< DAQ：接收线程入队
    QueueWait,       ///< DAQ：入队到工作线程取帧
    EnvelopeDecode,  ///< DAQ：DTO 信封解码
    PayloadDecode,   ///< DAQ：切片解码与换算
    E2e,             ///< DAQ：入队时刻到回调开始的端到端延迟
    CycleActual,     ///< 工具侧：相邻两次 poll 周期的实际间隔
    Count            ///< 哨兵，必须位于末尾
};

/// @brief 阶段总数（含哨兵）。
inline constexpr std::size_t kPerfStageCount = static_cast<std::size_t>(PerfStage::Count);

/// @brief 直方图桶数；改桶边界会破坏跨版本可比性，故与桶数一起写死。
inline constexpr std::size_t kPerfBucketCount = 16U;

/// @brief slowest 槽的最大数量（配置值会被夹到这个上限）。
inline constexpr std::size_t kPerfMaxSlowSpots = 16U;

/// @brief slowest 槽里符号名的最大长度（超出截断，保证无堆分配）。
inline constexpr std::size_t kPerfSymbolCapacity = 64U;

/**
 * @brief 直方图桶的上边界（毫秒），升序；最后一个为无穷大。
 *
 * 桶 i 覆盖 `[edges[i-1], edges[i])`，第 0 个桶是 `[0, edges[0])`。
 */
inline constexpr std::array<double, kPerfBucketCount> kPerfBucketEdgesMs{
    0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0,
    50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0,
    std::numeric_limits<double>::infinity()};

/**
 * @brief 取阶段名（日志用小写短名）。
 * @param stage 阶段
 * @return 静态字符串，永不返回空指针
 */
[[nodiscard]] inline const char* PerfStageName(PerfStage stage) noexcept {
    switch (stage) {
        case PerfStage::ScheduleGap: return "schedule_gap";
        case PerfStage::GroupRead: return "group_read";
        case PerfStage::SingleRead: return "single_read";
        case PerfStage::Convert: return "convert";
        case PerfStage::Assemble: return "assemble";
        case PerfStage::Callback: return "callback";
        case PerfStage::TickTotal: return "tick_total";
        case PerfStage::RxEnqueue: return "rx_enqueue";
        case PerfStage::QueueWait: return "queue_wait";
        case PerfStage::EnvelopeDecode: return "envelope_decode";
        case PerfStage::PayloadDecode: return "payload_decode";
        case PerfStage::E2e: return "e2e";
        case PerfStage::CycleActual: return "cycle_actual";
        case PerfStage::Count: break;
    }
    return "unknown";
}

/**
 * @brief 求某个耗时落在哪个桶。
 * @param duration_ms 耗时（毫秒，负值按 0 处理）
 * @return 桶下标 `[0, kPerfBucketCount)`
 */
[[nodiscard]] inline std::size_t PerfBucketIndex(double duration_ms) noexcept {
    const double value = duration_ms > 0.0 ? duration_ms : 0.0;
    for (std::size_t i = 0; i < kPerfBucketCount; ++i) {
        if (value < kPerfBucketEdgesMs[i]) {
            return i;
        }
    }
    return kPerfBucketCount - 1U;
}

/**
 * @brief 单阶段在窗口内的累计。
 */
struct StageWindow {
    /// @brief 样本数。
    std::uint64_t count{0};

    /// @brief 耗时总和（毫秒）。
    double sum_ms{0.0};

    /// @brief 单次最大耗时（毫秒）。
    double max_ms{0.0};

    /// @brief 16 个桶的样本计数。
    std::array<std::uint32_t, kPerfBucketCount> buckets{};

    /// @brief 是否有样本。
    [[nodiscard]] bool Empty() const noexcept { return count == 0U; }

    /// @brief 平均耗时（毫秒），无样本返回 0。
    [[nodiscard]] double MeanMs() const noexcept {
        return count == 0U ? 0.0 : sum_ms / static_cast<double>(count);
    }

    /**
     * @brief 用桶内线性插值估算分位数。
     * @param percentile 分位（0~1，例如 0.95）
     * @return 估算的耗时（毫秒）；无样本返回 0
     */
    [[nodiscard]] double PercentileMs(double percentile) const noexcept {
        if (count == 0U) {
            return 0.0;
        }
        const double clamped = std::clamp(percentile, 0.0, 1.0);
        const double target = clamped * static_cast<double>(count);
        double cumulative = 0.0;
        for (std::size_t i = 0; i < kPerfBucketCount; ++i) {
            const double before = cumulative;
            cumulative += static_cast<double>(buckets[i]);
            if (cumulative < target) {
                continue;
            }
            const double lower = (i == 0U) ? 0.0 : kPerfBucketEdgesMs[i - 1U];
            double upper = kPerfBucketEdgesMs[i];
            if (!std::isfinite(upper)) {
                upper = std::max(max_ms, lower);
            }
            const double in_bucket = static_cast<double>(buckets[i]);
            const double fraction =
                in_bucket > 0.0 ? std::clamp((target - before) / in_bucket, 0.0, 1.0) : 0.0;
            return lower + (upper - lower) * fraction;
        }
        return max_ms;
    }

    /**
     * @brief 记录一个样本。
     * @param duration_ms 耗时（毫秒，负值按 0 处理）
     */
    void Add(double duration_ms) noexcept {
        const double value = duration_ms > 0.0 ? duration_ms : 0.0;
        ++count;
        sum_ms += value;
        if (value > max_ms) {
            max_ms = value;
        }
        ++buckets[PerfBucketIndex(value)];
    }

    /// @brief 清零。
    void Reset() noexcept {
        count = 0U;
        sum_ms = 0.0;
        max_ms = 0.0;
        buckets.fill(0U);
    }
};

/**
 * @brief 性能统计的运行期配置。
 * @details 整体替换、整体生效：调用 SetPerfConfig() 传一份新配置，统计在下一个
 *          窗口边界读取它；不提供逐字段 setter，避免半更新状态。
 */
struct PerfConfig {
    /// @brief 总开关；false 时不采样、不聚合、不写日志。
    bool enabled{true};

    /// @brief 日志窗口长度（墙钟）。默认 5 s。
    /// @note 窗口过短会让 p95 失去统计意义（样本 < 20），下限建议 500 ms。
    std::chrono::milliseconds window{5000};

    /// @brief 采样率：每 N 次事件测一次；0 等价于 1。
    std::uint32_t sample_every{1};

    /// @brief slowest 记录的槽数（阶段×变量）。0 表示不统计慢点。
    std::size_t slowest_capacity{5};

    /// @brief 是否把窗口摘要写进日志。
    bool log_to_logger{true};

    /// @brief 是否输出各阶段明细行；false 时只输出 total 一行。
    bool verbose_stages{true};
};

/// @brief 配置相等比较（便于单测断言）。
[[nodiscard]] inline bool operator==(const PerfConfig& lhs, const PerfConfig& rhs) noexcept {
    return lhs.enabled == rhs.enabled && lhs.window == rhs.window &&
           lhs.sample_every == rhs.sample_every &&
           lhs.slowest_capacity == rhs.slowest_capacity &&
           lhs.log_to_logger == rhs.log_to_logger &&
           lhs.verbose_stages == rhs.verbose_stages;
}

/// @brief 配置不等比较。
[[nodiscard]] inline bool operator!=(const PerfConfig& lhs, const PerfConfig& rhs) noexcept {
    return !(lhs == rhs);
}

namespace detail {

/**
 * @brief 读取环境变量，未设置时返回空串。
 * @param name 变量名
 * @return 变量值；变量缺失时为空串
 * @note MSVC 把 std::getenv 标为不安全（C4996），这里改用 _dupenv_s，
 *       读的是同一份 CRT 环境块，语义与 POSIX 分支保持一致。
 */
[[nodiscard]] inline std::string PerfEnvironmentValue(const char* name) {
#if defined(_MSC_VER)
    char* raw = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&raw, &length, name) != 0 || raw == nullptr) {
        return {};
    }
    std::string value(raw);
    std::free(raw);
    return value;
#else
    const char* raw = std::getenv(name);
    return raw == nullptr ? std::string{} : std::string(raw);
#endif
}

/**
 * @brief 解析环境变量里的整数。
 * @param name 变量名
 * @param fallback 解析失败时的回退值
 * @return 解析结果；变量缺失、格式非法或越界时返回 fallback
 * @note 非法值会记一条 WARNING，但绝不因此关闭统计或抛异常。
 */
[[nodiscard]] inline long long ParsePerfEnvInteger(const char* name, long long fallback) noexcept {
    const std::string text = PerfEnvironmentValue(name);
    if (text.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const long long parsed = std::strtoll(text.c_str(), &end, 10);
    const bool valid =
        end != nullptr && end != text.c_str() && *end == '\0';
    if (!valid) {
        LIBXCP_LOG_WARNING("性能统计：环境变量 %s=\"%s\" 不是整数，按缺省值 %lld 处理", name,
                           text.c_str(), fallback);
        return fallback;
    }
    return parsed;
}

}  // namespace detail

/**
 * @brief 从环境变量构造配置，非法值回落到缺省并记告警。
 *
 * 支持的变量：`LIBXCP_PERF_ENABLED`、`LIBXCP_PERF_WINDOW_MS`、
 * `LIBXCP_PERF_SAMPLE_EVERY`、`LIBXCP_PERF_SLOWEST`、`LIBXCP_PERF_VERBOSE_STAGES`。
 */
[[nodiscard]] inline PerfConfig PerfConfigFromEnvironment() {
    PerfConfig config{};

    const long long enabled = detail::ParsePerfEnvInteger("LIBXCP_PERF_ENABLED", -1);
    if (enabled >= 0) {
        config.enabled = enabled != 0;
    }

    const long long window_ms =
        detail::ParsePerfEnvInteger("LIBXCP_PERF_WINDOW_MS", -1);
    if (window_ms >= 0) {
        // 0 表示关闭统计，语义与 enabled=false 一致。
        config.enabled = config.enabled && window_ms > 0;
        if (window_ms > 0) {
            config.window = std::chrono::milliseconds(window_ms);
        }
    }

    const long long sample_every =
        detail::ParsePerfEnvInteger("LIBXCP_PERF_SAMPLE_EVERY", -1);
    if (sample_every > 0) {
        config.sample_every = static_cast<std::uint32_t>(sample_every);
    }

    const long long slowest = detail::ParsePerfEnvInteger("LIBXCP_PERF_SLOWEST", -1);
    if (slowest >= 0) {
        config.slowest_capacity = static_cast<std::size_t>(slowest);
    }

    const long long verbose = detail::ParsePerfEnvInteger("LIBXCP_PERF_VERBOSE_STAGES", -1);
    if (verbose >= 0) {
        config.verbose_stages = verbose != 0;
    }

    return config;
}

/**
 * @brief 一个「阶段 × 变量」的慢点累计。
 *
 * 槽身份是 `(stage, symbol)`：同名变量出现在不同阶段会各占一槽。
 */
struct SlowSpot {
    /// @brief 符号名（固定长度缓冲，超出截断，避免每次采样分配内存）。
    std::array<char, kPerfSymbolCapacity> symbol{};

    /// @brief 该槽所属阶段。
    PerfStage stage{PerfStage::Convert};

    /// @brief 样本数。
    std::uint64_t count{0};

    /// @brief 累计耗时（毫秒），排序依据。
    double sum_ms{0.0};

    /// @brief 单次最大耗时（毫秒）。
    double max_ms{0.0};

    /// @brief 合并读组包含的变量数（仅 GroupRead 有意义，其余为 1）。
    std::uint32_t group_size{1};

    /// @brief 合并读组读取的字节数（仅 GroupRead 有意义，其余为 0）。
    std::uint32_t group_bytes{0};

    /// @brief 取符号名视图（永不返回空指针）。
    [[nodiscard]] std::string_view Symbol() const noexcept {
        const auto end = std::find(symbol.begin(), symbol.end(), '\0');
        return std::string_view(symbol.data(),
                                static_cast<std::size_t>(end - symbol.begin()));
    }

    /// @brief 平均耗时（毫秒），无样本返回 0。
    [[nodiscard]] double MeanMs() const noexcept {
        return count == 0U ? 0.0 : sum_ms / static_cast<double>(count);
    }

    /// @brief 是否有样本。
    [[nodiscard]] bool Empty() const noexcept { return count == 0U; }

    /**
     * @brief 写入符号名，超长截断。
     * @param name 符号名
     */
    void SetSymbol(std::string_view name) noexcept {
        symbol.fill('\0');
        const std::size_t size = std::min(name.size(), kPerfSymbolCapacity - 1U);
        std::copy_n(name.data(), size, symbol.data());
    }

    /// @brief 清零（保留缓冲，等价于空槽）。
    void Reset() noexcept {
        symbol.fill('\0');
        stage = PerfStage::Convert;
        count = 0U;
        sum_ms = 0.0;
        max_ms = 0.0;
        group_size = 1U;
        group_bytes = 0U;
    }
};

/**
 * @brief 一个窗口的聚合结果（TakeSnapshot 的返回值）。
 */
struct PerfWindowSnapshot {
    /// @brief 窗口起点（第一个采样事件时刻）。
    std::chrono::steady_clock::time_point begin{};

    /// @brief 窗口终点（取快照时刻）。
    std::chrono::steady_clock::time_point end{};

    /// @brief 窗口实际长度。
    std::chrono::milliseconds elapsed{0};

    /// @brief 窗口内看到的事件数（含被采样率跳过的）。
    std::uint64_t events{0};

    /// @brief 实际采样的次数。
    std::uint64_t samples{0};

    /// @brief 各阶段累计。
    std::array<StageWindow, kPerfStageCount> stages{};

    /// @brief 慢点列表，按累计耗时降序。
    std::vector<SlowSpot> slowest;

    /// @brief 是否有任何样本。
    [[nodiscard]] bool Empty() const noexcept { return samples == 0U; }

    /**
     * @brief 取某阶段的累计。
     * @param stage 阶段
     * @return 该阶段累计；越界（传入 Count）返回空累计
     */
    [[nodiscard]] const StageWindow& Stage(PerfStage stage) const noexcept {
        static const StageWindow kEmpty{};
        const auto index = static_cast<std::size_t>(stage);
        return index < kPerfStageCount ? stages[index] : kEmpty;
    }
};

/**
 * @brief 滑动窗口聚合器。
 *
 * 线程模型：**单线程使用**——只由所属 Session 的调度线程写入，取快照也在同一线程。
 * 因此内部无锁；跨线程改配置由 Session 自己的 mutex 负责，不在这里做。
 */
class WindowAggregator {
public:
    /// @brief 可注入的时钟。
    using NowFn = std::function<std::chrono::steady_clock::time_point()>;

    /**
     * @brief 构造。
     * @param config 初始配置
     * @param now_fn 时钟；为空时用 `std::chrono::steady_clock::now`
     */
    explicit WindowAggregator(PerfConfig config = {},
                              NowFn now_fn = {}) noexcept
        : config_(config), now_fn_(std::move(now_fn)) {
        slowest_.fill(SlowSpot{});
    }

    /**
     * @brief 替换配置，在下一个窗口边界生效。
     * @param config 新配置
     */
    void SetConfig(const PerfConfig& config) noexcept {
        pending_ = config;
        has_pending_ = true;
    }

    /// @brief 当前生效配置。
    [[nodiscard]] const PerfConfig& ActiveConfig() const noexcept { return config_; }

    /// @brief 返回下一次采样将使用的配置（有待生效配置时返回它）
    /// @details 配置在下一个采样点才真正生效，但调用方读取"我刚设的值"
    ///          时应当看到待生效的那一份，否则 set 之后立刻读会拿到旧值。
    [[nodiscard]] const PerfConfig& Config() const noexcept {
        return has_pending_ ? pending_ : config_;
    }

    /// @brief 是否已经开始统计（窗口内有第一个采样事件）。
    [[nodiscard]] bool Started() const noexcept { return started_; }

    /// @brief 当前窗口起点；未开始时返回 `time_point{}`。
    [[nodiscard]] std::chrono::steady_clock::time_point WindowBegin() const noexcept {
        return begin_;
    }

    /// @brief 窗口内看到的事件数。
    [[nodiscard]] std::uint64_t Events() const noexcept { return events_; }

    /**
     * @brief 记录一个样本。
     * @param stage 阶段
     * @param duration_ms 耗时（毫秒，负值按 0 处理）
     * @param symbol 变量名；GroupRead 传组内首变量名，其它阶段可省略
     * @param group_size 合并组包含的变量数（仅 GroupRead 使用，其余传 1）
     * @param group_bytes 合并组读取的字节数（仅 GroupRead 使用，其余传 0）
     * @note `noexcept`、无锁、无堆分配；被采样率跳过时只累加事件计数。
     */
    void Record(PerfStage stage, double duration_ms, std::string_view symbol = {},
                std::uint32_t group_size = 1, std::uint32_t group_bytes = 0) noexcept {
        ApplyPendingConfig();
        if (!config_.enabled) {
            return;
        }
        const auto stage_index = static_cast<std::size_t>(stage);
        if (stage_index >= kPerfStageCount) {
            return;
        }
        ++events_;
        const std::uint32_t sample_every = config_.sample_every == 0U ? 1U : config_.sample_every;
        if (sample_every > 1U && (events_ % sample_every) != 0U) {
            return;
        }
        if (!started_) {
            started_ = true;
            begin_ = Now();
        }
        ++samples_;
        stages_[stage_index].Add(duration_ms);
        if (config_.slowest_capacity > 0U && !symbol.empty()) {
            AddSlowSpot(stage, symbol, duration_ms, group_size, group_bytes);
        }
    }

    /**
     * @brief 只记录 slowest 榜单，不进入阶段耗时统计。
     * @param stage 阶段
     * @param duration_ms 耗时（毫秒，负值按 0 处理）
     * @param symbol 变量名（空则忽略）
     * @param group_size 合并组包含的变量数（仅 GroupRead 使用，其余传 1）
     * @param group_bytes 合并组读取的字节数（仅 GroupRead 使用，其余传 0）
     * @note 供"阶段按每 tick 记总量、slowest 按每变量记明细"的调用方使用：
     *       `Record` 一次会把样本同时计入阶段窗口，分变量记账会污染阶段均值。
     */
    void RecordSlowSpot(PerfStage stage, double duration_ms, std::string_view symbol,
                        std::uint32_t group_size = 1,
                        std::uint32_t group_bytes = 0) noexcept {
        ApplyPendingConfig();
        if (!config_.enabled || symbol.empty() || config_.slowest_capacity == 0U) {
            return;
        }
        const auto stage_index = static_cast<std::size_t>(stage);
        if (stage_index >= kPerfStageCount) {
            return;
        }
        AddSlowSpot(stage, symbol, duration_ms, group_size, group_bytes);
    }

    /**
     * @brief 判断窗口是否到点。
     * @param now 当前时刻
     * @return 已开始统计、有采样、且距窗口起点已超过配置长度时返回 true
     */
    [[nodiscard]] bool Due(std::chrono::steady_clock::time_point now) const noexcept {
        if (!config_.enabled || !started_ || samples_ == 0U) {
            return false;
        }
        return now - begin_ >= config_.window;
    }

    /**
     * @brief 取快照并清零。
     * @param now 当前时刻
     * @return 窗口快照；未开始统计时返回空快照（不改变状态）
     * @note 取快照时才分配 `slowest` 向量，采样路径不受影响。
     */
    [[nodiscard]] PerfWindowSnapshot TakeSnapshot(std::chrono::steady_clock::time_point now) {
        PerfWindowSnapshot snapshot;
        if (!started_) {
            return snapshot;
        }
        snapshot.begin = begin_;
        snapshot.end = now;
        snapshot.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - begin_);
        snapshot.events = events_;
        snapshot.samples = samples_;
        snapshot.stages = stages_;
        snapshot.slowest.reserve(slowest_count_);
        for (std::size_t i = 0; i < slowest_count_; ++i) {
            snapshot.slowest.push_back(slowest_[i]);
        }
        std::sort(snapshot.slowest.begin(), snapshot.slowest.end(),
                  [](const SlowSpot& lhs, const SlowSpot& rhs) {
                      if (lhs.sum_ms != rhs.sum_ms) {
                          return lhs.sum_ms > rhs.sum_ms;
                      }
                      return lhs.max_ms > rhs.max_ms;
                  });
        Reset();
        return snapshot;
    }

    /**
     * @brief 清零所有累计，回到"未开始"状态。
     * @note 不清配置；未生效的配置仍然保留，等下一个事件时应用。
     */
    void Reset() noexcept {
        stages_.fill(StageWindow{});
        for (auto& spot : slowest_) {
            spot.Reset();
        }
        slowest_count_ = 0U;
        events_ = 0U;
        samples_ = 0U;
        started_ = false;
        begin_ = std::chrono::steady_clock::time_point{};
    }

private:
    /// @brief 应用待生效配置（只在事件到来时调用，保证"下个窗口生效"）。
    void ApplyPendingConfig() noexcept {
        if (!has_pending_) {
            return;
        }
        has_pending_ = false;
        const bool was_enabled = config_.enabled;
        config_ = pending_;
        // 从开启切到关闭：丢弃未完成窗口，不写半截日志。
        if (was_enabled && !config_.enabled) {
            Reset();
        }
    }

    /// @brief 取当前时刻。
    [[nodiscard]] std::chrono::steady_clock::time_point Now() const {
        return now_fn_ ? now_fn_() : std::chrono::steady_clock::now();
    }

    /**
     * @brief 维护 slowest 槽（固定容量，线性替换，无分配）。
     * @param stage 阶段
     * @param symbol 符号名
     * @param duration_ms 本次耗时
     * @param group_size 合并组变量数（仅 GroupRead 有意义）
     * @param group_bytes 合并组字节数（仅 GroupRead 有意义）
     */
    void AddSlowSpot(PerfStage stage, std::string_view symbol, double duration_ms,
                     std::uint32_t group_size, std::uint32_t group_bytes) noexcept {
        const double value = duration_ms > 0.0 ? duration_ms : 0.0;
        const std::size_t capacity =
            std::min(config_.slowest_capacity, kPerfMaxSlowSpots);
        if (capacity == 0U) {
            return;
        }
        // 已存在的槽：直接累加。组信息取窗口内最大值，便于看出最大那组有多大。
        for (std::size_t i = 0; i < slowest_count_; ++i) {
            if (slowest_[i].stage == stage && slowest_[i].Symbol() == symbol) {
                ++slowest_[i].count;
                slowest_[i].sum_ms += value;
                if (value > slowest_[i].max_ms) {
                    slowest_[i].max_ms = value;
                }
                if (group_size > slowest_[i].group_size) {
                    slowest_[i].group_size = group_size;
                }
                if (group_bytes > slowest_[i].group_bytes) {
                    slowest_[i].group_bytes = group_bytes;
                }
                return;
            }
        }
        // 还有空槽：占一个。
        if (slowest_count_ < capacity) {
            SlowSpot& slot = slowest_[slowest_count_];
            slot.Reset();
            slot.stage = stage;
            slot.SetSymbol(symbol);
            slot.count = 1U;
            slot.sum_ms = value;
            slot.max_ms = value;
            slot.group_size = group_size;
            slot.group_bytes = group_bytes;
            ++slowest_count_;
            return;
        }
        // 满了：与累计最小的槽比较，只有更值得关注才替换。
        std::size_t weakest = 0U;
        for (std::size_t i = 1; i < slowest_count_; ++i) {
            if (slowest_[i].sum_ms < slowest_[weakest].sum_ms) {
                weakest = i;
            }
        }
        if (slowest_[weakest].sum_ms >= value) {
            return;
        }
        SlowSpot& slot = slowest_[weakest];
        slot.Reset();
        slot.stage = stage;
        slot.SetSymbol(symbol);
        slot.count = 1U;
        slot.sum_ms = value;
        slot.max_ms = value;
        slot.group_size = group_size;
        slot.group_bytes = group_bytes;
    }

    /// @brief 生效配置。
    PerfConfig config_{};

    /// @brief 待生效配置。
    PerfConfig pending_{};

    /// @brief 是否存在待生效配置。
    bool has_pending_{false};

    /// @brief 注入的时钟；为空时用 steady_clock。
    NowFn now_fn_{};

    /// @brief 各阶段累计。
    std::array<StageWindow, kPerfStageCount> stages_{};

    /// @brief slowest 槽。
    std::array<SlowSpot, kPerfMaxSlowSpots> slowest_{};

    /// @brief 已占用的 slowest 槽数。
    std::size_t slowest_count_{0U};

    /// @brief 窗口内事件数（含被采样率跳过的）。
    std::uint64_t events_{0U};

    /// @brief 窗口内采样次数。
    std::uint64_t samples_{0U};

    /// @brief 是否已经开始统计。
    bool started_{false};

    /// @brief 窗口起点。
    std::chrono::steady_clock::time_point begin_{};
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_DETAIL_PERF_WINDOW_HPP_
