/**
 * @file measurement_session.hpp
 * @brief 测量会话：把 DTO 流转成物理值帧的高级用户 API（v0.5）。
 *
 * 对应测量子系统代码增长计划 §5.5/§5.6。`MeasurementSession` 实现
 * `IEventListener` 以接收 DTO，使用时**先构造本对象、再以 &session 作为
 * XcpMaster 构造时的监听器**（用户已确认 DTO 接线方案：构造时传监听器 +
 * session 先构造）。由于 XcpMaster 的事件监听器仅在构造时固定且无 setter，
 * 本类**不**在构造函数接收 `XcpMaster&`，而是提供 `Bind` 把 master 引用
 * 后置绑定（v0.5 相对计划 §5.5 字面签名的偏差，详见 v0.5 记录）。
 *
 * 数据通路：RX(Transport) 线程的 OnDto 只把完整 DTO 帧入有界队列 →
 * 内部 worker 线程取出 → 以账本快照解码（DtoEnvelopeDecoder →
 * DtoPayloadDecoder）→ 经 IMeasurementDatabase::ToPhysical 换算 → 构造
 * MeasurementFrame 回调用户（回调在 worker 线程，非 RX 线程，架构文档 §17）。
 */

#ifndef LIBXCP_MEASUREMENT_SESSION_HPP
#define LIBXCP_MEASUREMENT_SESSION_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "libxcp/command_executor.hpp"  // IEventListener / DtoPacket / EventPacket / ServicePacket
#include "libxcp/daq/dto_envelope_types.hpp"
#include "libxcp/detail/perf_window.hpp"
#include "libxcp/measurement/measurement_database.hpp"
#include "libxcp/measurement/measurement_sample.hpp"
#include "libxcp/protocol_types.hpp"

namespace calmcar::xcp {

/// @brief 前置声明：本头以引用/指针持有 master，定义见 xcp_master.hpp
class XcpMaster;

/**
 * @brief 测量会话统计（§5.6，v0.6 硬化回卷等语义）
 */
struct MeasurementStatistics {
    std::uint64_t dto_received{0};    ///< 收到的 DTO 帧总数
    std::uint64_t dto_dropped{0};     ///< 有界队列溢出丢弃数
    std::uint64_t decode_errors{0};   ///< 单帧解码失败（drop 计数）
    std::uint64_t timestamp_wraps{0}; ///< 时间戳回卷次数
    std::uint64_t counter_gaps{0};    ///< DTO 计数器不连续的次数（含重复值）
    std::uint64_t counter_lost{0};    ///< 由计数器缺口推算出的丢帧总数
};

/**
 * @brief 测量会话性能快照（P-C：DAQ 六阶段滑动窗口）
 *
 * @details 与 `MeasurementStatistics` 分开：本结构只覆盖"自上次取快照
 *          以来"的一个统计窗口，取走即清零；累计计数仍在
 *          `MeasurementStatistics` 里，两者不互相影响。
 */
struct MeasurementPerfSnapshot {
    PerfWindowSnapshot window;      ///< 六阶段耗时窗口（未到窗口边界时可为空）
    std::uint64_t frames{0};        ///< 窗口内完成回调的 DTO 帧数
    std::uint64_t payload_slices{0};///< 窗口内解码的净荷切片总数

    /// @brief 窗口内没有任何样本
    [[nodiscard]] bool Empty() const noexcept {
        return window.samples == 0U && frames == 0U;
    }
};

/**
 * @brief 测量会话（最终用户 API）
 *
 * @details 使用方法（最终应用码不变量，见计划 §5.5）：
 * @code
 * A2lMeasurementDatabase adapter(db);      // 或任意 IMeasurementDatabase
 * MeasurementSession session(adapter);     // 1) 先构造 session
 * XcpMaster master(transport, {}, &session); // 2) master 以 &session 作监听器
 * session.Bind(master);                    // 3) 绑定 master
 * session.Add("EngineSpeed");              // 4) 追加待测变量
 * session.Prepare();                       // 5) 规划 + ConfigureDaqListsDynamic
 * session.Start([](const MeasurementFrame& f){ Consume(f); }); // 6) 启动
 * @endcode
 */
class MeasurementSession : public IEventListener {
public:
    /**
     * @brief 构造会话（此时 master 尚未绑定）
     * @param database IMeasurementDatabase 视图（适配器提供；生命周期须长于本对象）
     */
    explicit MeasurementSession(
        IMeasurementDatabase& database,
        PerfConfig perf_config = PerfConfigFromEnvironment());
    ~MeasurementSession() override;  // 尽力 Stop + join + 撤销配置

    MeasurementSession(const MeasurementSession&) = delete;
    MeasurementSession& operator=(const MeasurementSession&) = delete;

    /**
     * @brief 绑定 XcpMaster（其构造已把 &this 作为事件监听器）
     * @param master 已 Connect 的 XcpMaster；生命周期须长于本对象
     * @throws XcpException(InvalidArgument) 重复绑定
     */
    void Bind(XcpMaster& master);

    /// @brief 追加待测变量（Prepare 前可改）
    void Add(std::string_view name);
    /// @brief 移除指定待测变量（Prepare 前可改）
    void Remove(std::string_view name);
    /// @brief 清空待测变量集合
    void Clear();

    /**
     * @brief 规划 + ConfigureDaqListsDynamic（配置期记录 DaqConfigGeneration）
     * @throws XcpException master 未绑定，或规划/下发失败
     */
    void Prepare();

    /**
     * @brief 启动 DAQ 并注册回调；DTO 在 worker 线程解码后回调（非 RX 线程）
     * @param callback 消费 MeasurementFrame 的回调（worker 线程执行）
     * @throws XcpException 未 Prepare、未绑定或已运行
     */
    void Start(MeasurementCallback callback);

    /// @brief 停止 DAQ、join worker、复位运行态
    void Stop();

    /// @brief 是否正在运行
    [[nodiscard]] bool Running() const noexcept;

    /// @brief 统计快照副本
    [[nodiscard]] MeasurementStatistics Statistics() const;

    /**
     * @brief 运行期整体替换性能统计配置（下一个窗口边界生效）
     * @param config 新配置；`enabled=false` 时丢弃未完成的窗口
     * @note 与 poll 侧同签名同语义，便于工具统一配置两条采集路径
     */
    void SetPerfConfig(const PerfConfig& config);

    /// @brief 当前生效的性能统计配置
    [[nodiscard]] PerfConfig PerfConfigValue() const;

    /**
     * @brief 取走当前窗口的性能快照（取走即清零窗口计数）
     */
    [[nodiscard]] MeasurementPerfSnapshot PerfSnapshot();

    /**
     * @brief 设定 DTO 识别字段模式（运行时取证用；须在 Prepare 前调用）
     * @param type 识别字段模式（默认 RelativeWord）
     * @param id_field_bytes 识别字段头长（默认 3；按规范 Figure 51/52/53：
     *        Absolute=1，RelativeByte=2，RelativeWord=3，RelativeWordAligned=4）
     * @details 信封布局只能来自会话取证，绝不猜测（B-16）。worker 解码时
     *          按此模式 + 取证到的时间戳宽度切分识别字段（时间戳段不计入
     *          id_field_bytes，由解码器按 timestamp_size_bits 单独消费）。
     */
    void SetEnvelopeMode(IdentificationFieldType type,
                         std::size_t id_field_bytes);

    /**
     * @brief 设定时间戳单位（tick→纳秒），v0.6 时间戳硬化用
     * @param unit_ns 每个原始计数 tick 对应的纳秒数；0=未知
     * @details 单位码表本仓库无权威来源（R13），只能来自运行时取证
     *          （A2L EVENT CONFIGURATION / ECU 文档），绝不猜测（B-3）。
     *          未设定（0）时帧时间戳 `timestamp_valid=false`、仅保留
     *          `timestamp_raw`；位宽（1/2/4B）始终取 Prepare 取证值。
     *          运行中调用抛 InvalidState。
     */
    void SetTimestampUnit(std::uint64_t unit_ns);

    /**
     * @brief 配置 DTO 是否携带时间戳（须在 Prepare 前调用）
     * @param enabled false 时不请求时间戳并按无时间戳布局规划/解码
     */
    void SetTimestampEnabled(bool enabled);

    /**
     * @brief 时间戳是否只随**本事件首个 ODT 帧**出现（v0.9，XCPlite 实然 D13）
     * @param first_only true=相对模式下只有相对 ODT 为 0 的帧带时间戳段；
     *                   false（默认）=每个 DTO 帧都带（v0.2 通用口径）
     * @details 部分 Slave（如 XCPlite）只在事件的第一个 ODT 包上附时间戳，
     *          其余 ODT 包没有该段。若不区分，后续帧会把时间戳字节误当净荷
     *          前缀（或反之把净荷当时间戳）。默认关闭保持既有语义不变；
     *          Absolute 识别模式不受该开关影响。运行中调用抛 InvalidState。
     */
    void SetTimestampFirstOdtOnly(bool first_only);

    /**
     * @brief 注入已验证的 DAQ Entry 限制（须在 Prepare 前调用）
     * @param capabilities 仅来自运行时 profile；默认能力不开放数组
     */
    void SetDaqEntryCapabilities(MeasurementDaqCapabilities capabilities);

    /**
     * @brief 配置是否要求 DTO 携带计数器（须在 Prepare 前调用）
     * @param enabled true=规划时置 `DaqListModeBit::kDtoCounter`，并在下发后
     *                用 `GET_DAQ_LIST_MODE` 回读确认实际生效
     * @throws XcpException(UnsupportedFeature) 已启用但 Slave 无法回读
     *         `GET_DAQ_LIST_MODE`（无法确认布局，不静默假设）
     * @throws XcpException(ProtocolError) 回读的 `DTO_CTR` 位与请求不一致
     * @details 默认 false，与被本批次修改前的既有行为逐字节一致。启用后
     *          解码布局的计数器位**只来自回读事实**，不来自本地期望；
     *          计数器缺口经 `MeasurementFrame::counter_lost_before` 与
     *          `MeasurementStatistics::counter_gaps/counter_lost` 上报。
     *          运行中调用抛 InvalidState。
     */
    void SetDtoCounterEnabled(bool enabled);

    /**
     * @brief 请求会话使用 DAQ Packed Mode（须在 Prepare 前调用）
     * @param mode 打包方式；`NotPacked` 表示常规单采样传输（默认）
     * @param timestamp_mode 帧时间戳语义（仅 packed 有意义）
     * @param sample_count 一帧打包的采样数（合法范围 [2, 0xFFFF]）
     * @throws XcpException(InvalidArgument) 采样数越界或 packed 下给出了保留的
     *         时间戳语义（本地拒绝，不发帧）
     * @throws XcpException(InvalidState) 运行中调用
     * @note 真正生效须在 `Prepare()` 中逐个列表协商并**回读确认**；对端不支持
     *       或回读不一致时 `Prepare()` 报错（不会"一边按 packed 配置、一边按
     *       普通 DTO 解码"）。切换 packed 后旧代际 DTO 必须丢弃：本类通过
     *       `Stop()`→重新 `Prepare()` 建立新布局，不跨代际复用解码路由。
     */
    void SetDaqPackedMode(DaqPackedMode mode,
                          DaqPackedTimestampMode timestamp_mode,
                          std::uint16_t sample_count);

    // ---- IEventListener（供 XcpMaster 回调；RX 线程只入队，不阻塞）----
    void OnEvent(const EventPacket& /*event*/) override {}   ///< EV 不处理
    void OnService(const ServicePacket& /*service*/) override {}  ///< SERV 不处理
    /**
     * @brief 收到完整 DTO 帧：仅入有界队列（溢出计数 dto_dropped）
     */
    void OnDto(const DtoPacket& dto) override;

private:
    /// @brief worker 线程主循环：取帧→解码→换算→回调
    void WorkerLoop();
    /// @brief worker 线程停止通知
    void NotifyWorker();

    /// @brief 一帧 DAQ 处理的六阶段耗时（毫秒；负值表示该阶段未发生）
    struct DaqFrameTiming {
        double rx_enqueue_ms{0.0};       ///< RX 线程入队耗时
        double queue_wait_ms{0.0};       ///< 入队 → worker 取帧的排队时间
        double envelope_decode_ms{0.0};  ///< 信封解码耗时
        double payload_decode_ms{-1.0};  ///< 净荷解码 + 换算耗时
        double callback_ms{-1.0};        ///< 用户回调耗时（无回调时为负）
        double e2e_ms{-1.0};             ///< 入队 → 回调开始的端到端延迟
        std::size_t slices{0};           ///< 本帧解码的切片数
    };
    /// @brief 记录一帧的六阶段耗时，到窗口边界时输出日志
    void RecordFramePerf(const DaqFrameTiming& timing);
    /// @brief 输出一个性能窗口（调用方须持有 m_perf_mutex_）
    void FlushPerfWindow();

    IMeasurementDatabase& m_database_;
    XcpMaster* m_master_{nullptr};         ///< 经 Bind 绑定（可为空）
    bool m_bound_{false};
    bool m_prepared_{false};

    std::vector<std::string> m_names_;     ///< 待测变量（用户线程持有锁访问）
    mutable std::mutex m_names_mutex_;

    std::uint16_t m_max_dto_{0};           ///< Prepare 时缓存
    std::size_t m_ag_bytes_{1};            ///< Prepare 时缓存（AG→字节）
    std::size_t m_timestamp_bytes_{0};     ///< Prepare 时缓存
    ByteOrder m_byte_order_{ByteOrder::Intel};
    std::uint32_t m_plan_generation_{0};   ///< Prepare 时记录的代际
    IdentificationFieldType m_id_field_type_{
        IdentificationFieldType::RelativeWord};  ///< 识别字段模式（取证可改）
    std::size_t m_id_field_bytes_{3};  ///< 识别字段头长（不含时间戳）；
                                       ///< RelativeWord=3（Figure 52）
    std::uint64_t m_timestamp_unit_ns_{
        0};  ///< tick→纳秒（0=未知不猜，R13；SetTimestampUnit 取证注入）
    bool m_timestamp_first_odt_only_{
        false};  ///< 时间戳只随首 ODT 帧（v0.9 D13；SetTimestampFirstOdtOnly）
    bool m_timestamp_enabled_{true};  ///< Prepare 前配置；默认保持原有取证行为
    bool m_dto_counter_enabled_{false};  ///< Prepare 前配置：是否要求 DTO 计数器
    bool m_counter_active_{false};       ///< Prepare 回读确认后的实际布局位
    DaqPackedMode m_packed_mode_{DaqPackedMode::NotPacked};  ///< 请求的打包方式
    DaqPackedTimestampMode m_packed_timestamp_mode_{
        DaqPackedTimestampMode::LastSample};  ///< 帧时间戳语义
    std::uint16_t m_packed_sample_count_{1};  ///< 请求的一帧采样数
    bool m_packed_active_{false};  ///< Prepare 回读确认后的实际 packed 状态
    /// @brief 每个 DAQ List 的 packed 运行参数（仅 m_packed_active_ 时填充）
    struct PackedListInfo {
        std::uint16_t sample_count{1};    ///< 一帧采样数（DTO 内，[1, 0xFFFF]）
        std::uint64_t sample_interval_ns{0};  ///< 相邻采样间隔（纳秒）
        bool first_sample_timestamp{false};  ///< 帧时间戳是否指向首采样
    };
    std::map<std::uint16_t, PackedListInfo> m_packed_lists_;
    MeasurementDaqCapabilities m_daq_entry_capabilities_{};  ///< 已验证的 Entry 限制
    std::uint8_t m_first_pid_{0};     ///< Absolute 模式 START 返回的 FIRST_PID

    // 运行态 + 队列 + worker（Start/Stop/OnDto/回调共享锁）
    mutable std::mutex m_run_mutex_;
    std::condition_variable m_run_cv_;
    bool m_running_{false};
    bool m_stop_requested_{false};
    MeasurementCallback m_callback_;
    /// @brief 队列元素：DTO 净荷 + 到达时刻（端到端延迟与排队等待需要）
    struct QueuedDto {
        Bytes bytes;                                      ///< DTO 净荷
        std::chrono::steady_clock::time_point arrival{};  ///< RX 线程入队时刻
        double enqueue_ms{0.0};                           ///< 入队本身耗时（毫秒）
    };
    std::deque<QueuedDto> m_queue_;        ///< 有界 DTO 队列（带到达时刻）
    std::unique_ptr<std::thread> m_worker_;
    static constexpr std::size_t kMaxQueue = 256;  ///< 有界队列容量

    // 统计（RX 线程与 worker 线程写，Statistics() 读）
    std::atomic<std::uint64_t> m_dto_received_{0};
    std::atomic<std::uint64_t> m_dto_dropped_{0};
    std::atomic<std::uint64_t> m_decode_errors_{0};
    std::atomic<std::uint64_t> m_timestamp_wraps_{0};
    std::atomic<std::uint64_t> m_counter_gaps_{0};
    std::atomic<std::uint64_t> m_counter_lost_{0};

    // 性能统计（worker 线程写、用户线程读，均经 m_perf_mutex_）
    mutable std::mutex m_perf_mutex_;  ///< 保护 m_perf_ 与窗口计数
    WindowAggregator m_perf_;          ///< 六阶段耗时滑动窗口
    std::uint64_t m_perf_frames_{0};   ///< 窗口内已回调的帧数
    std::uint64_t m_perf_slices_{0};   ///< 窗口内已解码的切片数

    /// @brief 已规划的路由视图（Prepare 时建立；(daq_list,odt) → 有序切片规格）
    struct OdtSlice {
        std::string name;          ///< 变量名
        std::size_t offset{0};     ///< 净荷内偏移（相对于 envelope.payload@0）
        std::size_t size{0};       ///< 实占字节数
        bool needs_timestamp{false};
        bool is_array{false};      ///< 本 Entry 是完整的一维数组
        std::uint8_t element_count{1};
        std::uint8_t element_size_bytes{0};
        /// packed 下同测量相邻采样的字节间隔（0=非 packed）
        std::size_t packed_stride{0};
    };
    struct OdtView {
        std::uint8_t odt_number{0};
        std::vector<OdtSlice> slices;
    };
    std::vector<std::pair<std::uint16_t, std::vector<OdtView>>>
        m_routes_;  ///< 每个 daq_list 的有序 ODT 视图
};

}  // namespace calmcar::xcp

#endif  // LIBXCP_MEASUREMENT_SESSION_HPP