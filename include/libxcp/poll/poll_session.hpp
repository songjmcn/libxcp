/**
 * @file poll_session.hpp
 * @brief 主站主动轮询（Poll）采集会话。
 *
 * 与 `MeasurementSession`（DAQ 路径）的分工：
 * - DAQ：从机按事件通道周期产出 DTO，主站被动解码；带宽高但需要从机侧配置。
 * - Poll：主站按本地定时器主动 UPLOAD，无需从机配置；周期精度受往返时间限制，
 *   适合低速、少量、不同周期的小集合（实施计划 §1.1/§1.3）。
 *
 * 本类只负责**调度与读取编排**，不做物理换算（交给
 * `IMeasurementDatabase::ToPhysical`），也不改变 `MemoryAccess` 的公开签名。
 */

#ifndef CALMCAR_XCP_POLL_POLL_SESSION_HPP_
#define CALMCAR_XCP_POLL_POLL_SESSION_HPP_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libxcp/detail/perf_window.hpp"
#include "libxcp/measurement/measurement_database.hpp"
#include "libxcp/measurement/measurement_result.hpp"
#include "libxcp/poll/poll_types.hpp"

namespace calmcar::xcp {

class XcpMaster;

/**
 * @brief poll 会话的运行期性能快照（P-B）。
 * @details 阶段耗时窗口 + slowest 榜单 + 合并读取的字节量。
 *          取快照即清零：与内部窗口日志共用同一个窗口，先取者得。
 * @note 字节量只统计成功的合并组读取；回退单读单独计入
 *       `single_read_bytes`，不与合并组混算（否则"合并是否值得"会被高估）。
 */
struct PollPerfSnapshot {
    PerfWindowSnapshot window;              ///< 阶段耗时窗口（含 slowest 榜单）
    std::uint64_t ticks{0};                 ///< 本窗口的 tick 数
    std::uint64_t overruns{0};              ///< 本窗口因迟到而整周期跳过的次数
    std::uint64_t variables{0};             ///< 本窗口读取的变量总次数
    std::uint64_t groups{0};                ///< 本窗口的合并读组总次数
    std::uint64_t group_bytes_total{0};     ///< 本窗口合并组读取的总字节
    std::uint64_t group_bytes_max{0};       ///< 单次合并组读取的最大字节
    std::uint64_t group_slices_total{0};    ///< 被合并进组的变量总次数
    std::uint64_t single_read_bytes{0};     ///< 回退单读累计字节

    /// @brief 是否为空窗口（没有采集到任何样本）
    [[nodiscard]] bool Empty() const noexcept { return window.Empty(); }
};

/**
 * @brief 多 event 的 poll 采集会话（每个 event 独立周期）。
 * @details 线程模型：`Start` 创建一个调度线程，所有 tick 的读取与回调都在该
 *          线程串行执行；`Stop` 请求停止并 join。因为
 *          `CommandExecutor::RunCommand` 拒绝并发命令，**不得**在多线程中同时
 *          调用 `XcpMaster` 的阻塞接口（实施计划 §2、§4.3）。
 *
 *          与 DAQ 互斥：`Start` 时若主站已配置任何 DAQ List 则拒绝启动，
 *          避免同一 transport 上两种采集语义交织（实施计划 §1.2-2）。
 *
 * @warning 回调在调度线程内执行，不得阻塞，也不得调用本会话的阻塞接口。
 */
class PollSession {
public:
    /// @brief 单个事件允许的最小周期（实施计划 §1.2-1）
    static constexpr std::chrono::milliseconds kMinPeriod{20};

    /**
     * @brief 构造会话。
     * @param master 已连接的主站（非拥有，生命周期须长于本对象）
     * @param database 变量数据库（非拥有，生命周期须长于本对象）
     * @throws XcpException(InvalidArgument) 依赖为空指针
     */
    PollSession(XcpMaster& master, const IMeasurementDatabase& database,
                PerfConfig perf_config = PerfConfigFromEnvironment());

    /// @brief 析构：若仍在运行则先 Stop（不抛异常）
    ~PollSession();

    PollSession(const PollSession&) = delete;
    PollSession& operator=(const PollSession&) = delete;
    PollSession(PollSession&&) = delete;
    PollSession& operator=(PollSession&&) = delete;

    /**
     * @brief 注册一个采集任务。
     * @throws XcpException(InvalidArgument) 名字为空/重复、周期小于 20 ms、
     *         符号列表为空
     * @throws XcpException(InvalidState) 会话正在运行
     */
    void AddEvent(PollEventSpec spec);

    /**
     * @brief 移除一个采集任务（按名字）。
     * @throws XcpException(InvalidArgument) 名字未注册
     * @throws XcpException(InvalidState) 会话正在运行
     */
    void RemoveEvent(std::string_view name);

    /**
     * @brief 预检：全部符号能否解析且布局可用于 poll。
     * @details 在 `Start` 之前显式调用可获得可操作的错误信息；`Start` 内部
     *          也会再校验一次。标量与数组都支持：数组按
     *          `元素数 × 单元素字节数` 一次读回，再逐元素经
     *          `IMeasurementDatabase::ToPhysicalElement` 换算。
     * @return 全部可用时成功；否则返回首个失败的符号与原因
     */
    [[nodiscard]] MeasurementResult<void> Validate() const;

    /**
     * @brief 启动调度线程。
     * @param callback 每个 tick 的结果回调（可为空：仅统计不投递）
     * @throws XcpException(InvalidState) 已在运行、未注册任何 event、
     *         主站未连接、主站已配置 DAQ List
     * @throws XcpException(InvalidArgument) 符号解析失败（含具体符号名）
     */
    void Start(PollCallback callback);

    /// @brief 请求停止并等待调度线程退出（可重复调用）
    void Stop();

    /// @brief 是否正在运行
    [[nodiscard]] bool IsRunning() const noexcept;

    /// @brief 统计快照（运行中也可安全读取）
    [[nodiscard]] PollStatistics Statistics() const noexcept;

    /// @brief 已注册的 event 数量
    [[nodiscard]] std::size_t EventCount() const noexcept;

    /**
     * @brief 运行期替换性能统计配置（下一个窗口边界生效）。
     * @param config 新配置（整体替换，不提供逐字段 setter）
     * @note 只影响窗口划分与 slowest 容量，不改变采集语义；`enabled=false`
     *       会丢弃尚未结束的窗口，不输出半截日志。
     */
    void SetPerfConfig(const PerfConfig& config) noexcept;

    /// @brief 当前生效的性能统计配置
    [[nodiscard]] PerfConfig PerfConfigValue() const noexcept;

    /**
     * @brief 取性能快照（**消耗**当前窗口：取后清零）。
     * @return 阶段耗时 + slowest + 合并字节量；未开始统计时返回空快照
     * @note 与内部窗口日志共用同一窗口；请在调度线程或 `Stop()` 之后调用。
     */
    [[nodiscard]] PollPerfSnapshot PerfSnapshot();

private:
    /// @brief 输出一次窗口性能日志（窗口到点时由调度线程调用）
    void FlushPerfWindow(std::string_view event_name);

    /// @brief 保护窗口计数（`PerfSnapshot` 可能来自其它线程）
    mutable std::mutex m_perf_mutex_;
    /// @brief 阶段耗时与 slowest 聚合（只由调度线程写入）
    WindowAggregator m_perf_;
    /// @brief 窗口内 tick 数
    std::uint64_t m_perf_ticks_{0};
    /// @brief 窗口内因迟到而整周期跳过的次数
    std::atomic<std::uint64_t> m_perf_overruns_{0};
    /// @brief 窗口内读取的变量总次数
    std::uint64_t m_perf_variables_{0};
    /// @brief 窗口内合并读组总次数
    std::uint64_t m_perf_groups_{0};
    /// @brief 窗口内合并组读取的总字节
    std::uint64_t m_perf_group_bytes_{0};
    /// @brief 单次合并组读取的最大字节
    std::uint64_t m_perf_group_bytes_max_{0};
    /// @brief 被合并进组的变量总次数
    std::uint64_t m_perf_group_slices_{0};
    /// @brief 回退单读累计字节
    std::uint64_t m_perf_single_read_bytes_{0};
    /// @brief 已解析的变量布局
    struct ResolvedVariable {
        std::string symbol;              ///< 变量名
        MeasurementSymbolInfo info{};    ///< 地址/扩展/宽度（不含 DAQ 通道语义）
        /// @brief 一次 tick 需要读取的总字节数
        /// @details 标量为单元素宽度；数组为 `element_count × element_size_bytes`。
        ///          在 `ResolveEvent` 里算好，读组归并与读取路径只依赖它。
        std::size_t byte_count{0};
    };

    /// @brief 合并读组中的一个变量切片。
    /// @details P3：相邻地址的多个变量由一次 UPLOAD 覆盖，切片记录各自偏移。
    struct PollSlice {
        std::size_t variable_index{0};  ///< 在 `EventState::variables` 中的下标
        std::size_t offset{0};          ///< 相对组起始地址的字节偏移
        std::size_t size{0};            ///< 字节数（= 该变量的 `byte_count`，数组为整组元素）
    };

    /// @brief 一次 UPLOAD 覆盖的连续地址区段。
    /// @details 只有**同一 address extension 且地址首尾相接**的变量才会被并入同组；
    ///          任何不连续处都另起一组，绝不跨越未声明的地址空隙读取。
    struct PollReadGroup {
        AddressExtension extension{0};   ///< 地址扩展
        Address address{0};              ///< 组起始地址
        std::size_t byte_count{0};       ///< 组总字节数
        std::vector<PollSlice> slices;   ///< 组内变量切片
    };

    /// @brief 一个 event 的运行时状态
    struct EventState {
        PollEventSpec spec;                                  ///< 原始定义
        std::vector<ResolvedVariable> variables;             ///< 解析结果
        std::vector<PollReadGroup> groups;                   ///< 地址合并后的读取组
        std::chrono::steady_clock::time_point next{};        ///< 下一次计划时刻
    };

    /// @brief 全量解析一个 event 的符号（失败抛 InvalidArgument）
    static void ResolveEvent(EventState& state,
                             const IMeasurementDatabase& database);

    /// @brief 调度线程主体
    void RunLoop();

    /// @brief 执行一次 tick（分组读取 + 换算 + 回调）
    void RunTick(EventState& state);

    /// @brief 读取并换算单个变量（合并组失效时的回退路径）
    void ReadSingle(const ResolvedVariable& variable, PollVariableSample& result);

    /// @brief 把一批已读字节换算为单个变量的物理值并填入结果
    void ConvertInto(const ResolvedVariable& variable, BytesView raw,
                     PollVariableSample& result);

    XcpMaster& m_master_;                          ///< 主站（非拥有）
    const IMeasurementDatabase& m_database_;       ///< 变量数据库（非拥有）

    mutable std::mutex m_mutex_;                   ///< 保护 m_events_/m_running_
    std::condition_variable m_cv_;                 ///< 通知调度线程停止
    std::vector<EventState> m_events_;             ///< 已注册事件（运行期不变）
    std::vector<EventState> m_running_events_;     ///< 调度线程独占的工作副本
    PollCallback m_callback_;                      ///< 结果回调
    std::thread m_thread_;                         ///< 调度线程
    bool m_running_{false};                        ///< 是否处于运行态
    std::atomic<bool> m_stop_requested_{false};    ///< 停止请求标志

    std::atomic<std::uint64_t> m_ticks_{0};             ///< tick 计数
    std::atomic<std::uint64_t> m_skipped_{0};           ///< 超期跳过计数
    std::atomic<std::uint64_t> m_read_errors_{0};       ///< 读取/换算失败计数
    std::atomic<std::uint64_t> m_callback_errors_{0};   ///< 回调异常计数
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_POLL_POLL_SESSION_HPP_