/**
 * @file poll_session.cpp
 * @brief `PollSession` 的实现：本地周期调度 + 逐变量读取 + 物理换算。
 */

#include "libxcp/poll/poll_session.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <numeric>
#include <cstdio>
#include <string>
#include <utility>

#include "libxcp/log.hpp"
#include "libxcp/protocol_types.hpp"
#include "libxcp/xcp_error.hpp"
#include "libxcp/xcp_master.hpp"

namespace calmcar::xcp {
namespace {

/**
 * @brief 计算一个符号一次 tick 需要读取的总字节数。
 * @details 标量（`element_count == 1`）等于单元素宽度；数组为
 *          `元素数 × 单元素字节数`。适配器已保证数组元素连续
 *          （`byte_stride == element_size_bytes`），因此可以直接相乘。
 * @param info 符号布局
 * @param[out] byte_count 总字节数
 * @return 布局合法（宽度/元素数非 0 且乘积不溢出 `size_t`）时返回 true
 */
bool ComputePollByteCount(const MeasurementSymbolInfo& info,
                          std::size_t& byte_count) {
    if (info.element_size_bytes == 0U || info.element_count == 0U) {
        return false;
    }
    const std::size_t width = static_cast<std::size_t>(info.element_size_bytes);
    const std::size_t count = static_cast<std::size_t>(info.element_count);
    if (count > std::numeric_limits<std::size_t>::max() / width) {
        return false;
    }
    byte_count = width * count;
    return true;
}

}  // namespace

PollSession::PollSession(XcpMaster& master, const IMeasurementDatabase& database,
                         PerfConfig perf_config)
    : m_master_(master), m_database_(database), m_perf_(perf_config) {}

PollSession::~PollSession() {
    // 析构不得抛异常：Stop 内部已吞掉 join 失败以外的一切
    Stop();
}

void PollSession::AddEvent(PollEventSpec spec) {
    if (spec.name.empty()) {
        throw detail::MakeInvalidArgument("poll event 名称不能为空");
    }
    if (spec.period < kMinPeriod) {
        throw detail::MakeInvalidArgument(
            "poll event 周期小于下限 20 ms（当前 " +
            std::to_string(spec.period.count()) + " ms）");
    }
    if (spec.symbols.empty()) {
        throw detail::MakeInvalidArgument("poll event \"" + spec.name +
                                          "\" 的符号列表为空");
    }

    std::lock_guard<std::mutex> lock(m_mutex_);
    if (m_running_) {
        throw detail::MakeInvalidState("poll 会话运行中不允许修改 event 表");
    }
    const bool duplicated =
        std::any_of(m_events_.begin(), m_events_.end(),
                    [&spec](const EventState& e) { return e.spec.name == spec.name; });
    if (duplicated) {
        throw detail::MakeInvalidArgument("poll event 名称重复：" + spec.name);
    }
    EventState state;
    state.spec = std::move(spec);
    m_events_.push_back(std::move(state));
}

void PollSession::RemoveEvent(std::string_view name) {
    std::lock_guard<std::mutex> lock(m_mutex_);
    if (m_running_) {
        throw detail::MakeInvalidState("poll 会话运行中不允许修改 event 表");
    }
    const auto it = std::find_if(
        m_events_.begin(), m_events_.end(),
        [name](const EventState& e) { return e.spec.name == name; });
    if (it == m_events_.end()) {
        throw detail::MakeInvalidArgument("poll event 未注册：" + std::string{name});
    }
    m_events_.erase(it);
}

void PollSession::ResolveEvent(EventState& state,
                               const IMeasurementDatabase& database) {
    state.variables.clear();
    state.variables.reserve(state.spec.symbols.size());
    for (const auto& symbol : state.spec.symbols) {
        auto found = database.Find(symbol);
        if (!found) {
            const auto& error = found.ErrorInfo();
            throw detail::MakeInvalidArgument(
                "poll event \"" + state.spec.name + "\" 的符号 \"" + symbol +
                "\" 无法解析：" + error.message);
        }
        const MeasurementSymbolInfo info = found.Value();
        // 标量与数组统一按"一次 tick 读多少字节"建模：标量是 1 个元素，
        // 数组是 element_count 个元素。布局合法性在这里一次判掉，后续的
        // 读组归并与读取都只依赖 byte_count。
        std::size_t byte_count = 0;
        if (!ComputePollByteCount(info, byte_count)) {
            throw detail::MakeInvalidArgument(
                "poll event \"" + state.spec.name + "\" 的符号 \"" + symbol +
                "\" 布局非法（元素宽度 " + std::to_string(info.element_size_bytes) +
                "，元素数 " + std::to_string(info.element_count) +
                "），无法确定读取字节数");
        }
        ResolvedVariable variable;
        variable.symbol = symbol;
        variable.info = info;
        variable.byte_count = byte_count;
        state.variables.push_back(std::move(variable));
    }

    // P3 地址合并：按 (extension, address) 排序后贪心归并"首尾相接"的变量，
    // 让一次 UPLOAD 覆盖多个相邻变量。任何不连续处都另起一组——绝不跨越
    // 未声明的地址空隙读取，因为那会读到别的信号甚至非法地址。
    state.groups.clear();
    std::vector<std::size_t> order(state.variables.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&state](std::size_t lhs, std::size_t rhs) {
        const auto& a = state.variables[lhs].info;
        const auto& b = state.variables[rhs].info;
        if (a.extension != b.extension) {
            return a.extension < b.extension;
        }
        return a.address < b.address;
    });

    for (const std::size_t index : order) {
        const auto& info = state.variables[index].info;
        // 数组变量占用整段字节（元素数 × 单元素宽度），归并时按它推进地址
        const auto size = state.variables[index].byte_count;
        if (!state.groups.empty()) {
            auto& last = state.groups.back();
            const auto last_end = static_cast<std::uint64_t>(last.address) +
                                  static_cast<std::uint64_t>(last.byte_count);
            if (last.extension == info.extension && last_end <= 0xFFFFFFFFULL &&
                static_cast<Address>(last_end) == info.address) {
                last.slices.push_back(PollSlice{index, last.byte_count, size});
                last.byte_count += size;
                continue;
            }
        }
        PollReadGroup group;
        group.extension = info.extension;
        group.address = info.address;
        group.byte_count = size;
        group.slices.push_back(PollSlice{index, 0, size});
        state.groups.push_back(std::move(group));
    }
}

MeasurementResult<void> PollSession::Validate() const {
    std::lock_guard<std::mutex> lock(m_mutex_);
    for (const auto& event : m_events_) {
        for (const auto& symbol : event.spec.symbols) {
            auto found = m_database_.Find(symbol);
            if (!found) {
                return MeasurementResult<void>(
                    MeasurementError{found.ErrorInfo().code,
                                     "poll event \"" + event.spec.name +
                                         "\" 的符号无法解析：" +
                                         found.ErrorInfo().message,
                                     symbol});
            }
            const MeasurementSymbolInfo info = found.Value();
            std::size_t byte_count = 0;
            if (!ComputePollByteCount(info, byte_count)) {
                return MeasurementResult<void>(MeasurementError{
                    MeasurementErrorCode::UnsupportedDataType,
                    "poll 符号布局非法（元素宽度或元素数为 0，或字节数溢出），"
                    "无法读取",
                    symbol});
            }
        }
    }
    return MeasurementResult<void>{};
}

void PollSession::Start(PollCallback callback) {
    std::vector<EventState> working;
    {
        std::lock_guard<std::mutex> lock(m_mutex_);
        if (m_running_) {
            throw detail::MakeInvalidState("poll 会话已在运行");
        }
        if (m_events_.empty()) {
            throw detail::MakeInvalidState("未注册任何 poll event，无法启动");
        }
        if (!m_master_.IsConnected()) {
            throw detail::MakeInvalidState("poll 会话要求主站已连接");
        }
        if (!m_master_.DaqLedger().empty()) {
            // 二者语义互斥：DAQ 由从机驱动 DTO 流，poll 由主站主动 UPLOAD；
            // 同一 transport 上混用会让周期统计与错误归因都失去意义。
            throw detail::MakeInvalidState(
                "主站已配置 DAQ List，poll 会话与 DAQ 不允许并存");
        }
        working = m_events_;
    }

    // 约束 2 的第二半：账本为空不代表从机没有在播 DTO（DAQ 可能由别的会话
    // 配好并已启动），因此再用 GET_STATUS 的 DAQ_RUNNING 位确认一次
    // （实施计划 §3.5）。该查询只在 Start 发生一次。
    if (m_master_.QueryStatus().daq_running) {
        throw detail::MakeInvalidState(
            "从机报告 DAQ 正在运行，poll 会话与 DAQ 不允许并存");
    }

    // 解析放在锁外：Find 可能触碰 A2L 结构的内部锁
    for (auto& event : working) {
        ResolveEvent(event, m_database_);
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex_);
        m_stop_requested_.store(false, std::memory_order_relaxed);
        m_running_events_ = std::move(working);
        m_callback_ = std::move(callback);
        m_running_ = true;
        m_thread_ = std::thread([this] { RunLoop(); });
    }
}

void PollSession::Stop() {
    {
        std::lock_guard<std::mutex> lock(m_mutex_);
        if (!m_running_) {
            return;
        }
        m_stop_requested_.store(true, std::memory_order_relaxed);
    }
    m_cv_.notify_all();
    if (m_thread_.joinable()) {
        m_thread_.join();
    }
    std::lock_guard<std::mutex> lock(m_mutex_);
    m_running_ = false;
    m_running_events_.clear();
    m_callback_ = nullptr;
}

bool PollSession::IsRunning() const noexcept {
    std::lock_guard<std::mutex> lock(m_mutex_);
    return m_running_;
}

PollStatistics PollSession::Statistics() const noexcept {
    PollStatistics statistics;
    statistics.ticks = m_ticks_.load(std::memory_order_relaxed);
    statistics.skipped_overruns = m_skipped_.load(std::memory_order_relaxed);
    statistics.read_errors = m_read_errors_.load(std::memory_order_relaxed);
    statistics.callback_errors = m_callback_errors_.load(std::memory_order_relaxed);
    return statistics;
}

std::size_t PollSession::EventCount() const noexcept {
    std::lock_guard<std::mutex> lock(m_mutex_);
    return m_events_.size();
}

void PollSession::RunLoop() {
    auto& events = m_running_events_;
    if (events.empty()) {
        return;
    }
    const auto begin = std::chrono::steady_clock::now();
    for (auto& event : events) {
        event.next = begin;
    }

    while (!m_stop_requested_.load(std::memory_order_relaxed)) {
        // 选最早到期的 event（P2 会改用最小堆；当前数量下线性扫描足够）
        auto earliest = std::min_element(
            events.begin(), events.end(),
            [](const EventState& lhs, const EventState& rhs) {
                return lhs.next < rhs.next;
            });
        {
            std::unique_lock<std::mutex> lock(m_mutex_);
            // 绝对时刻等待：避免 sleep_for 累积漂移
            if (m_cv_.wait_until(lock, earliest->next, [this] {
                    return m_stop_requested_.load(std::memory_order_relaxed);
                })) {
                return;
            }
        }
        if (m_stop_requested_.load(std::memory_order_relaxed)) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto period = earliest->spec.period;
        if (now >= earliest->next + period) {
            // 已迟到至少一个完整周期：丢弃过期 tick，不补跑（实施计划 §3/§4.4）
            std::uint64_t skipped = 0;
            while (earliest->next + period <= now) {
                earliest->next += period;
                ++skipped;
            }
            m_skipped_.fetch_add(skipped, std::memory_order_relaxed);
            m_perf_overruns_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        RunTick(*earliest);
        earliest->next += period;
    }
}

void PollSession::ConvertInto(const ResolvedVariable& variable, BytesView raw,
                              PollVariableSample& result) {
    if (!variable.info.is_array && variable.info.element_count <= 1U) {
        auto converted = m_database_.ToPhysical(variable.symbol, raw);
        if (!converted) {
            result.valid = false;
            result.error = converted.ErrorInfo().message;
            m_read_errors_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        result.value = converted.Value();
        result.valid = true;
        return;
    }

    // 数组：逐元素换算。任一元素失败则整条样本无效且不保留部分结果，
    // 与 DAQ 路径（MeasurementSession）的数组语义保持一致。
    result.is_array = true;
    result.elements.clear();
    const std::size_t width =
        static_cast<std::size_t>(variable.info.element_size_bytes);
    const std::size_t count =
        static_cast<std::size_t>(variable.info.element_count);
    if (width == 0U || count == 0U || raw.size() != width * count) {
        result.valid = false;
        result.error = "数组字节数与元素布局不匹配（期望 " +
                       std::to_string(width * count) + " 字节，实际 " +
                       std::to_string(raw.size()) + " 字节）";
        m_read_errors_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::vector<MeasurementValue> elements;
    elements.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const BytesView element_raw{raw.data() + index * width, width};
        auto converted =
            m_database_.ToPhysicalElement(variable.symbol, index, element_raw);
        if (!converted) {
            result.valid = false;
            result.error = converted.ErrorInfo().message;
            result.elements.clear();
            m_read_errors_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        elements.push_back(converted.Value());
    }
    result.elements = std::move(elements);
    result.valid = true;
}

void PollSession::ReadSingle(const ResolvedVariable& variable,
                             PollVariableSample& result) {
    try {
        // 标量取单元素宽度，数组取整数组字节数（见 ComputePollByteCount）
        if (variable.byte_count >
            static_cast<std::size_t>(std::numeric_limits<ByteCount>::max())) {
            result.valid = false;
            result.error = "数组总字节数超出单次读取上限";
            m_read_errors_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto byte_count = static_cast<ByteCount>(variable.byte_count);
        const Bytes raw = m_master_.ReadMemoryBytes(
            variable.info.address, variable.info.extension, byte_count);
        ConvertInto(variable, BytesView{raw.data(), raw.size()}, result);
    } catch (const std::exception& error) {
        // 单变量失败不影响同 tick 的其他变量（错误隔离，实施计划 §4.2）
        result.valid = false;
        result.error = error.what();
        m_read_errors_.fetch_add(1, std::memory_order_relaxed);
    }
}

void PollSession::SetPerfConfig(const PerfConfig& config) noexcept {
    m_perf_.SetConfig(config);
}

PerfConfig PollSession::PerfConfigValue() const noexcept { return m_perf_.Config(); }

PollPerfSnapshot PollSession::PerfSnapshot() {
    std::lock_guard<std::mutex> lock(m_perf_mutex_);
    PollPerfSnapshot snapshot;
    snapshot.window = m_perf_.TakeSnapshot(std::chrono::steady_clock::now());
    snapshot.ticks = m_perf_ticks_;
    snapshot.overruns = m_perf_overruns_.exchange(0, std::memory_order_relaxed);
    snapshot.variables = m_perf_variables_;
    snapshot.groups = m_perf_groups_;
    snapshot.group_bytes_total = m_perf_group_bytes_;
    snapshot.group_bytes_max = m_perf_group_bytes_max_;
    snapshot.group_slices_total = m_perf_group_slices_;
    snapshot.single_read_bytes = m_perf_single_read_bytes_;
    m_perf_ticks_ = 0;
    m_perf_variables_ = 0;
    m_perf_groups_ = 0;
    m_perf_group_bytes_ = 0;
    m_perf_group_bytes_max_ = 0;
    m_perf_group_slices_ = 0;
    m_perf_single_read_bytes_ = 0;
    return snapshot;
}

void PollSession::FlushPerfWindow(std::string_view event_name) {
    const PerfConfig config = m_perf_.ActiveConfig();
    if (!config.log_to_logger) {
        // 关掉日志也要清空窗口，否则下次打开会带上很久以前的样本
        (void)PerfSnapshot();
        return;
    }
    const PollPerfSnapshot snapshot = PerfSnapshot();
    if (snapshot.window.Empty()) {
        return;
    }
    const auto& stages = snapshot.window.stages;
    const double total_mean = stages[static_cast<std::size_t>(PerfStage::TickTotal)].MeanMs();
    const double elapsed_s = static_cast<double>(snapshot.window.elapsed.count()) / 1000.0;
    const double merged_pct =
        snapshot.variables == 0U
            ? 0.0
            : 100.0 * (1.0 - static_cast<double>(snapshot.groups) /
                                 static_cast<double>(snapshot.variables));
    const double bytes_per_tick =
        snapshot.ticks == 0U
            ? 0.0
            : static_cast<double>(snapshot.group_bytes_total) / static_cast<double>(snapshot.ticks);
    const double group_mean =
        snapshot.groups == 0U
            ? 0.0
            : static_cast<double>(snapshot.group_bytes_total) / static_cast<double>(snapshot.groups);
    const double overrun_pct =
        snapshot.ticks == 0U
            ? 0.0
            : 100.0 * static_cast<double>(snapshot.overruns) / static_cast<double>(snapshot.ticks);

    char line[512];
    std::string text;
    text.reserve(1024);
    std::snprintf(line, sizeof(line),
                  "[perf][poll event=%.*s] window=%.3fs ticks=%llu overrun=%llu (%.2f%%) "
                  "vars=%llu groups=%llu merged=%.1f%% bytes/tick=%.0fB "
                  "group_mean=%.0fB group_max=%lluB vars/group=%.1f\n",
                  static_cast<int>(event_name.size()), event_name.data(), elapsed_s,
                  static_cast<unsigned long long>(snapshot.ticks),
                  static_cast<unsigned long long>(snapshot.overruns), overrun_pct,
                  static_cast<unsigned long long>(snapshot.variables),
                  static_cast<unsigned long long>(snapshot.groups), merged_pct, bytes_per_tick,
                  group_mean, static_cast<unsigned long long>(snapshot.group_bytes_max),
                  snapshot.groups == 0U
                      ? 0.0
                      : static_cast<double>(snapshot.group_slices_total) /
                            static_cast<double>(snapshot.groups));
    text += line;

    struct StageRow {
        PerfStage stage;
        const char* label;
    };
    const StageRow rows[] = {
        {PerfStage::TickTotal, "total"},
        {PerfStage::GroupRead, "group_read"},
        {PerfStage::SingleRead, "single_read"},
        {PerfStage::Convert, "convert"},
        {PerfStage::Assemble, "assemble"},
        {PerfStage::Callback, "callback"},
        {PerfStage::ScheduleGap, "sched_gap"},
    };
    for (const auto& row : rows) {
        const auto& window = stages[static_cast<std::size_t>(row.stage)];
        if (window.count == 0U) {
            continue;
        }
        if (!config.verbose_stages && row.stage != PerfStage::TickTotal) {
            continue;
        }
        const double mean = window.MeanMs();
        const double share = total_mean > 0.0 ? 100.0 * mean / total_mean : 0.0;
        std::snprintf(line, sizeof(line),
                      "  %-10s mean=%.3fms p95=%.3fms max=%.3fms (%.1f%%)\n", row.label, mean,
                      window.PercentileMs(0.95), window.max_ms, share);
        text += line;
    }

    for (const auto& spot : snapshot.window.slowest) {
        const double mean = spot.MeanMs();
        const double share = total_mean > 0.0 ? 100.0 * mean / total_mean : 0.0;
        // SlowSpot 里的符号是定长缓冲，取 string_view 后再交给 printf
        const std::string symbol_name(spot.Symbol());
        if (spot.stage == PerfStage::GroupRead) {
            std::snprintf(line, sizeof(line),
                          "  slowest: %s[%s] n=%llu sum=%.2fms mean=%.3fms max=%.3fms "
                          "组%u变量/%uB (%.1f%%)\n",
                          symbol_name.c_str(), PerfStageName(spot.stage),
                          static_cast<unsigned long long>(spot.count), spot.sum_ms, mean, spot.max_ms,
                          spot.group_size, spot.group_bytes, share);
        } else {
            std::snprintf(line, sizeof(line),
                          "  slowest: %s[%s] n=%llu sum=%.2fms mean=%.3fms max=%.3fms (%.1f%%)\n",
                          symbol_name.c_str(), PerfStageName(spot.stage),
                          static_cast<unsigned long long>(spot.count), spot.sum_ms, mean, spot.max_ms,
                          share);
        }
        text += line;
    }

    LIBXCP_LOG_INFO("%s", text.c_str());
}

void PollSession::RunTick(EventState& state) {
    const auto tick_start = std::chrono::steady_clock::now();

    // 阶段耗时：本 tick 内逐段累加，最后各记一次，
    // 保证"各阶段之和 ≈ total"（按每变量记账会把阶段均值拉成单变量耗时）。
    double schedule_gap_ms = 0.0;
    double group_read_ms = 0.0;
    double single_read_ms = 0.0;
    double convert_ms = 0.0;
    double assemble_ms = 0.0;
    double callback_ms = 0.0;
    std::uint64_t tick_variables = 0;
    std::uint64_t tick_groups = 0;
    std::uint64_t tick_group_bytes = 0;
    std::uint64_t tick_group_bytes_max = 0;
    std::uint64_t tick_group_slices = 0;
    std::uint64_t tick_single_bytes = 0;

    // 调度抖动：计划时刻 → 真正进入 tick 的时刻（迟到整周期已由 RunLoop 记 skipped）
    const auto gap_ms =
        std::chrono::duration<double, std::milli>(tick_start - state.next).count();
    schedule_gap_ms = gap_ms > 0.0 ? gap_ms : 0.0;

    PollSample sample;
    sample.event_name = state.spec.name;
    sample.steady_time = tick_start;
    sample.system_time = std::chrono::system_clock::now();

    // 结果先按变量下标落位，最后再按用户给定的符号顺序输出
    const auto assemble_start = std::chrono::steady_clock::now();
    std::vector<PollVariableSample> results(state.variables.size());
    for (std::size_t i = 0; i < state.variables.size(); ++i) {
        results[i].symbol = state.variables[i].symbol;
        // 数组标记取自布局而不是换算结果：换算失败时消费方仍需知道形状
        results[i].is_array = state.variables[i].info.is_array ||
                              state.variables[i].info.element_count > 1U;
    }
    assemble_ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - assemble_start)
                      .count();
    tick_variables = state.variables.size();
    tick_groups = state.groups.size();

    for (const auto& group : state.groups) {
        Bytes buffer;
        bool group_ok = true;
        const auto read_start = std::chrono::steady_clock::now();
        try {
            buffer = m_master_.ReadMemoryBytes(
                group.address, group.extension,
                static_cast<ByteCount>(group.byte_count));
        } catch (const std::exception&) {
            // 合并读失败（例如总长度不满足 AG 对齐）：整组回退为逐变量读，
            // 把影响面重新收敛到单变量，避免一个对齐问题拖垮整组。
            group_ok = false;
        }
        const double group_ms = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - read_start)
                                    .count();
        group_read_ms += group_ms;

        if (group_ok && !group.slices.empty()) {
            tick_group_bytes += group.byte_count;
            tick_group_slices += group.slices.size();
            if (group.byte_count > tick_group_bytes_max) {
                tick_group_bytes_max = group.byte_count;
            }
            // 合并组耗时整组共担：slowest 记在组内首变量名下，并带上组形状
            m_perf_.RecordSlowSpot(
                PerfStage::GroupRead, group_ms,
                state.variables[group.slices.front().variable_index].symbol,
                static_cast<std::uint32_t>(group.slices.size()),
                static_cast<std::uint32_t>(group.byte_count));
        }

        for (const auto& slice : group.slices) {
            auto& result = results[slice.variable_index];
            if (!group_ok) {
                const auto single_start = std::chrono::steady_clock::now();
                ReadSingle(state.variables[slice.variable_index], result);
                // 回退单读的耗时（含换算）只能整体计入 single_read，
                // 否则同一段时间会在 convert 里被记第二遍（破坏可加和）
                single_read_ms += std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - single_start)
                                      .count();
                tick_single_bytes += state.variables[slice.variable_index].byte_count;
                continue;
            }
            const auto convert_start = std::chrono::steady_clock::now();
            ConvertInto(state.variables[slice.variable_index],
                        BytesView{buffer.data() + slice.offset, slice.size},
                        result);
            const double convert_sample = std::chrono::duration<double, std::milli>(
                                              std::chrono::steady_clock::now() - convert_start)
                                              .count();
            convert_ms += convert_sample;
            m_perf_.RecordSlowSpot(PerfStage::Convert, convert_sample,
                                   state.variables[slice.variable_index].symbol);
        }
    }

    sample.samples = std::move(results);
    // 单次 tick 的真实采样时刻落在 [steady_time, steady_time_end] 区间内，
    // 只保留起点是保守但不精确的做法，故两者都记录（实施计划 §3.4）。
    sample.steady_time_end = std::chrono::steady_clock::now();

    if (m_callback_) {
        const auto callback_start = std::chrono::steady_clock::now();
        try {
            m_callback_(sample);
        } catch (...) {
            // 回调异常只计数：调度线程必须继续，否则一次用户代码异常会终止采集
            m_callback_errors_.fetch_add(1, std::memory_order_relaxed);
        }
        callback_ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - callback_start)
                          .count();
    }

    // total 取 [steady_time, steady_time_end]：不含回调，也不含调度等待
    const double tick_total_ms =
        std::chrono::duration<double, std::milli>(sample.steady_time_end - tick_start).count();

    m_perf_.Record(PerfStage::ScheduleGap, schedule_gap_ms);
    m_perf_.Record(PerfStage::GroupRead, group_read_ms);
    if (single_read_ms > 0.0) {
        m_perf_.Record(PerfStage::SingleRead, single_read_ms);
    }
    m_perf_.Record(PerfStage::Convert, convert_ms);
    m_perf_.Record(PerfStage::Assemble, assemble_ms);
    m_perf_.Record(PerfStage::Callback, callback_ms);
    m_perf_.Record(PerfStage::TickTotal, tick_total_ms);

    {
        std::lock_guard<std::mutex> lock(m_perf_mutex_);
        ++m_perf_ticks_;
        m_perf_variables_ += tick_variables;
        m_perf_groups_ += tick_groups;
        m_perf_group_bytes_ += tick_group_bytes;
        m_perf_group_slices_ += tick_group_slices;
        m_perf_single_read_bytes_ += tick_single_bytes;
        if (tick_group_bytes_max > m_perf_group_bytes_max_) {
            m_perf_group_bytes_max_ = tick_group_bytes_max;
        }
    }

    // 窗口到点才输出一次日志：日志频率与采样频率解耦（实施计划 §2）
    if (m_perf_.Due(sample.steady_time_end)) {
        FlushPerfWindow(sample.event_name);
    }

    m_ticks_.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace calmcar::xcp