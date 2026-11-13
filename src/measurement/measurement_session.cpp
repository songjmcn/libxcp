/**
 * @file measurement_session.cpp
 * @brief 实现 MeasurementSession：把 DTO 流转成物理值帧（v0.5）。
 *
 * 数据通路见头文件：RX 线程 OnDto 只入队（有界，溢出计数）→ worker 线程
 * 取帧 → DtoEnvelopeDecoder 切信封 → 按路由切片（DtoPayloadDecoder）→
 * IMeasurementDatabase::ToPhysical 换算 → MeasurementFrame 回调。
 *
 * 兼容性说明：本实现的构造形态（MeasurementSession(IMeasurementDatabase&)
 * + Bind(XcpMaster&)）是计划 §5.5 字面签名 `(XcpMaster&,
 * IMeasurementDatabase&)` 的偏差——因为 XcpMaster
 * 的事件监听器仅在构造时固定，session 必须先构造并以 &session 作为 master
 * 监听器，master 引用随后 Bind。详见 v0.5 记录"偏差说明"。
 */

#include "libxcp/measurement/measurement_session.hpp"

#include <algorithm>  // std::find / std::find_if / std::prev
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "libxcp/daq/daq_timestamp.hpp"
#include "libxcp/log.hpp"
#include "libxcp/daq/dto_payload_decoder.hpp"
#include "libxcp/daq/dto_envelope_decoder.hpp"
#include "libxcp/measurement/measurement_planner.hpp"
#include "libxcp/measurement/measurement_result.hpp"
#include "libxcp/protocol_types.hpp"
#include "libxcp/xcp_error.hpp"
#include "libxcp/xcp_master.hpp"

namespace calmcar::xcp {

namespace {
/// @brief 以 InvalidArgument 类别抛出测量会话前置错误
[[noreturn]] void ThrowInvalid(const char* msg) {
    throw XcpException(ErrorCategory::InvalidArgument, msg);
}

/// @brief 以 InvalidState 类别抛出会话状态类错误（v0.6：未绑定/运行中/未
///        Prepare 等"时机不对"的错误归状态类，与参数非法区分，计划 §5.6）
[[noreturn]] void ThrowState(const char* msg) {
    throw XcpException(ErrorCategory::InvalidState, msg);
}

/// @brief 测量规划错误码 → 异常类别（v0.6 细化；v0.5 曾一律 InvalidArgument）
[[nodiscard]] ErrorCategory CategoryOf(MeasurementErrorCode code) noexcept {
    switch (code) {
        case MeasurementErrorCode::UnsupportedDataType:
        case MeasurementErrorCode::UnsupportedConversion:
            return ErrorCategory::UnsupportedFeature;
        case MeasurementErrorCode::Busy:
        case MeasurementErrorCode::NotRunning:
            return ErrorCategory::InvalidState;
        case MeasurementErrorCode::Fatal:
        case MeasurementErrorCode::DtoDecodeError:
        case MeasurementErrorCode::DaqConfigurationError:
            return ErrorCategory::ProtocolError;
        case MeasurementErrorCode::NotFound:
        case MeasurementErrorCode::AmbiguousName:
        case MeasurementErrorCode::RawSizeMismatch:
        case MeasurementErrorCode::InvalidLayout:
        default:
            return ErrorCategory::InvalidArgument;
    }
}
}  // namespace

MeasurementSession::MeasurementSession(IMeasurementDatabase& database)
    : m_database_(database) {}

MeasurementSession::~MeasurementSession() {
    if (m_worker_ && m_worker_->joinable()) {
        Stop();
    }
}

void MeasurementSession::SetEnvelopeMode(IdentificationFieldType type,
                                         std::size_t id_field_bytes) {
    // 取证入口：识别字段模式与头长只能来自会话取证，绝不猜测（B-16）。
    // 必须在 Prepare 之前调用（Prepare 起路由与解码布局冻结）。
    if (!m_routes_.empty()) {
        ThrowState("MeasurementSession::SetEnvelopeMode 须在 Prepare 前调用");
    }
    // 识别字段最小长度按规范 Figure 51/52/53 逐模式校验（N-02）：
    //   Absolute=1、RelativeByte=2、RelativeWord=3、RelativeWordAligned=4。
    // 长度统一取自 IdentificationFieldMinimumBytes，避免各处硬编码漂移。
    // 未知枚举值先拒（解码器同样会拒，此处提前给出更明确的失败点）。
    switch (type) {
        case IdentificationFieldType::Absolute:
        case IdentificationFieldType::RelativeByte:
        case IdentificationFieldType::RelativeWord:
        case IdentificationFieldType::RelativeWordAligned:
            break;
        default:
            ThrowInvalid("MeasurementSession::SetEnvelopeMode 未知识别字段模式");
    }
    const std::size_t minimum_bytes =
        IdentificationFieldMinimumBytes(type);
    if (id_field_bytes < minimum_bytes) {
        ThrowInvalid("MeasurementSession::SetEnvelopeMode 识别字段长度不足");
    }
    m_id_field_type_ = type;
    m_id_field_bytes_ = id_field_bytes;
}

void MeasurementSession::SetTimestampUnit(std::uint64_t unit_ns) {
    // 单位码表无权威来源（R13）：只能由上层把取证到的 tick→ns 显式注入；
    // 0=未知 → 帧时间戳 valid=false、仅保留 raw（不猜，B-3）。
    if (Running()) {
        ThrowState("MeasurementSession::SetTimestampUnit 运行中禁止");
    }
    m_timestamp_unit_ns_ = unit_ns;
}

void MeasurementSession::SetTimestampEnabled(bool enabled) {
    if (!m_routes_.empty()) {
        ThrowState("MeasurementSession::SetTimestampEnabled 须在 Prepare 前调用");
    }
    if (Running()) {
        ThrowState("MeasurementSession::SetTimestampEnabled 运行中禁止");
    }
    m_timestamp_enabled_ = enabled;
}

void MeasurementSession::SetTimestampFirstOdtOnly(bool first_only) {
    // v0.9：部分 Slave（XCPlite D13）只在事件首 ODT 帧附时间戳段。
    // 开关式取证注入（默认关=每帧带），运行中禁改（与 SetTimestampUnit 同律）。
    if (Running()) {
        ThrowState("MeasurementSession::SetTimestampFirstOdtOnly 运行中禁止");
    }
    m_timestamp_first_odt_only_ = first_only;
}

void MeasurementSession::SetDaqEntryCapabilities(
    MeasurementDaqCapabilities capabilities) {
    if (m_prepared_ || Running()) {
        ThrowState("MeasurementSession::SetDaqEntryCapabilities 须在 Prepare 前调用");
    }
    if (capabilities.supports_contiguous_1d_arrays &&
        (capabilities.entry_granularity_bytes == 0U ||
         capabilities.max_entry_size_elements == 0U)) {
        ThrowInvalid("数组准入缺少已验证的 DAQ Entry 粒度或最大长度");
    }
    m_daq_entry_capabilities_ = capabilities;
}

void MeasurementSession::SetDtoCounterEnabled(bool enabled) {
    if (m_prepared_ || Running()) {
        ThrowState("MeasurementSession::SetDtoCounterEnabled 须在 Prepare 前调用");
    }
    m_dto_counter_enabled_ = enabled;
}

void MeasurementSession::SetDaqPackedMode(DaqPackedMode mode,
                                          DaqPackedTimestampMode timestamp_mode,
                                          std::uint16_t sample_count) {
    if (m_prepared_ || Running()) {
        ThrowState("MeasurementSession::SetDaqPackedMode 须在 Prepare 前调用");
    }
    // 规范明确 DPM_SAMPLE_COUNT 的 0 与 1 非法：本地先拒绝，不发帧。
    if (mode != DaqPackedMode::NotPacked &&
        (sample_count < kMinDaqPackedSampleCount ||
         sample_count > kMaxDaqPackedSampleCount)) {
        ThrowInvalid(
            "MeasurementSession::SetDaqPackedMode 的采样数必须在 [2, 0xFFFF] 内");
    }
    m_packed_mode_ = mode;
    m_packed_timestamp_mode_ = timestamp_mode;
    m_packed_sample_count_ =
        (mode == DaqPackedMode::NotPacked) ? 1U : sample_count;
}

void MeasurementSession::Bind(XcpMaster& master) {
    if (m_bound_) {
        ThrowInvalid("MeasurementSession::Bind 重复绑定");
    }
    m_master_ = &master;
    m_bound_ = true;
}

void MeasurementSession::Add(std::string_view name) {
    std::lock_guard<std::mutex> lock(m_names_mutex_);
    if (m_running_) {
        ThrowInvalid("MeasurementSession::Add 运行中禁止");
    }
    m_names_.emplace_back(name);
}

void MeasurementSession::Remove(std::string_view name) {
    std::lock_guard<std::mutex> lock(m_names_mutex_);
    if (m_running_) {
        ThrowInvalid("MeasurementSession::Remove 运行中禁止");
    }
    const auto it =
        std::find(m_names_.begin(), m_names_.end(), std::string(name));
    if (it != m_names_.end()) {
        m_names_.erase(it);
    }
}

void MeasurementSession::Clear() {
    std::lock_guard<std::mutex> lock(m_names_mutex_);
    m_names_.clear();
}

void MeasurementSession::Prepare() {
    if (!m_bound_) {
        ThrowInvalid("MeasurementSession::Prepare 未 Bind master");
    }
    XcpMaster& master = *m_master_;
    if (master.IsConnected() == false) {
        ThrowInvalid("MeasurementSession::Prepare master 未连接");
    }
    const SessionParameters params = master.GetSessionParameters();
    m_max_dto_ = params.connect.max_dto;
    m_ag_bytes_ = static_cast<std::size_t>(params.connect.address_granularity);
    m_byte_order_ = params.connect.byte_order;
    m_timestamp_bytes_ = m_timestamp_enabled_
                             ? master.DaqTimestampBytesCached()
                             : 0U;

    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(m_names_mutex_);
        names = m_names_;
    }

    // 规划 + 下发（一个整体：失败不留半份账，ConfigureDaqListsDynamic 自回滚）
    // 计数器段是否额外占用 1 字节：RelativeWordAligned 模式下 CTR 取代
    // FILL 字节（Figure 56），不增加帧长；其余识别模式各占 1 字节。
    const std::size_t counter_header_bytes =
        (m_dto_counter_enabled_ &&
         !IdentificationFieldMergesCounter(m_id_field_type_))
            ? 1U
            : 0U;
    MeasurementPlanner planner(m_database_, m_max_dto_,
                               params.connect.address_granularity,
                               m_timestamp_bytes_, m_id_field_bytes_,
                               m_daq_entry_capabilities_,
                               m_dto_counter_enabled_, counter_header_bytes,
                               m_packed_mode_, m_packed_sample_count_);
    const MeasurementResult<MeasurementPlan> plan = planner.Build(names);
    if (!plan.HasValue()) {
        // v0.6 硬化：按错误码映射异常类别上抛（不再统一 InvalidArgument）
        const MeasurementError err = plan.ErrorInfo();
        throw XcpException(CategoryOf(err.code), err.message);
    }

    if (m_id_field_type_ == IdentificationFieldType::Absolute &&
        (plan.Value().daq_lists.size() != 1U ||
         plan.Value().daq_lists.front().daq_list != 0U)) {
        ThrowInvalid("Absolute PID DTO 当前仅支持单个 DAQ List（DAQ_LIST 0）");
    }
    master.ConfigureDaqListsDynamic(plan.Value().daq_lists, m_id_field_bytes_);

    // B2 Counter 贯通：解码布局的计数器位**只认回读事实**，不认本地期望。
    // GET_DAQ_LIST_MODE 是 Optional 命令：启用了计数器却无法回读时，无从
    // 确定 DTO 是否真的带计数器段——此时若按“无计数器”解析，净荷会整体
    // 前移 1 字节，全部样本错位。故明确拒绝，不静默降级（计划 §3 B2-2）。
    m_counter_active_ = false;
    if (m_dto_counter_enabled_) {
        for (const DaqListSpec& spec : plan.Value().daq_lists) {
            const std::optional<DaqListModeSnapshot> mode =
                master.QueryDaqListMode(spec.daq_list);
            if (!mode.has_value()) {
                throw XcpException(
                    ErrorCategory::UnsupportedFeature,
                    "已要求 DTO 计数器，但 Slave 不支持 GET_DAQ_LIST_MODE，"
                    "无法确认计数器布局（DAQ List " +
                        std::to_string(spec.daq_list) + "）");
            }
            const bool actual =
                HasDaqStatus(static_cast<DaqListStatusBit>(mode->mode),
                             DaqListStatusBit::kDtoCounter);
            if (!actual) {
                throw XcpException(
                    ErrorCategory::ProtocolError,
                    "SET_DAQ_LIST_MODE 已请求 DTO 计数器，但回读未置该位"
                    "（DAQ List " +
                        std::to_string(spec.daq_list) + "）");
            }
        }
        m_counter_active_ = true;
    }

    // ---- B4：DAQ Packed Mode 协商（逐个 List 设置 + 回读确认）----
    // 只有回读确认的列表才允许按 packed 布局解码；任一处失败即整体拒绝，
    // 绝不出现"按 packed 配置、按普通 DTO 解码"的错位（方案 §6/§7）。
    m_packed_active_ = false;
    m_packed_lists_.clear();
    if (m_packed_mode_ != DaqPackedMode::NotPacked) {
        for (const DaqListSpec& spec : plan.Value().daq_lists) {
            // 采样间隔 = 事件周期 × 降频因子；事件周期不可得则不猜，拒绝启用
            const std::optional<GetDaqEventInfoResponse> event_info =
                master.QueryDaqEventInfo(spec.event_channel);
            if (!event_info.has_value()) {
                throw XcpException(
                    ErrorCategory::UnsupportedFeature,
                    "已请求 DAQ Packed Mode，但无法取得事件通道 " +
                        std::to_string(spec.event_channel) +
                        " 的周期信息（GET_DAQ_EVENT_INFO 不可用），"
                        "无法重建采样时间戳");
            }
            const std::optional<std::uint64_t> unit_ns =
                EventChannelTimeUnitNanoseconds(event_info->time_unit);
            if (event_info->time_cycle == 0U || !unit_ns.has_value()) {
                throw XcpException(
                    ErrorCategory::UnsupportedFeature,
                    "DAQ Packed Mode 要求周期事件且单位可解析（Event Channel " +
                        std::to_string(spec.event_channel) + "，time_cycle=" +
                        std::to_string(event_info->time_cycle) + "，time_unit=" +
                        std::to_string(event_info->time_unit) + "）");
            }
            const DaqPackedModeNegotiation negotiation =
                master.SetDaqPackedMode(spec.daq_list, m_packed_mode_,
                                        spec.event_channel,
                                        m_packed_timestamp_mode_,
                                        m_packed_sample_count_);
            if (!negotiation.active) {
                throw XcpException(
                    ErrorCategory::UnsupportedFeature,
                    "DAQ Packed Mode 未生效（DAQ List " +
                        std::to_string(spec.daq_list) + "）：" +
                        negotiation.reason);
            }
            // 降频因子必须来自回读：SET_DAQ_LIST_MODE 的请求值允许被 Slave
            // 改写，用请求值会让非锚点采样时间戳按错误间隔重建。
            const XcpMaster::DaqPackedRuntimeInfo runtime =
                master.ReadDaqPackedRuntimeInfo(spec.daq_list);
            if (!runtime.prescaler_verified) {
                throw XcpException(
                    ErrorCategory::UnsupportedFeature,
                    "已启用 DAQ Packed Mode，但无法回读该列表的实际降频因子"
                    "（GET_DAQ_LIST_MODE 不可用或 prescaler=0，DAQ List " +
                        std::to_string(spec.daq_list) +
                        "），不能按请求值假设采样间隔");
            }
            PackedListInfo info;
            // 采样数保留完整 16 位：DPM_SAMPLE_COUNT 合法范围是 [2, 0xFFFF]，
            // 截断会把 S=256 变成 0，进而只解出一个采样而静默丢弃其余采样。
            info.sample_count = m_packed_sample_count_;
            info.sample_interval_ns =
                static_cast<std::uint64_t>(event_info->time_cycle) *
                (*unit_ns) * static_cast<std::uint64_t>(runtime.prescaler);
            info.first_sample_timestamp =
                (m_packed_timestamp_mode_ ==
                 DaqPackedTimestampMode::FirstSample);
            m_packed_lists_[spec.daq_list] = info;
        }
        m_packed_active_ = true;
    }

    // 建成路由视图：按 (daq_list, odt) 分组的切片规格
    m_routes_.clear();
    for (const MeasurementRoute& r : plan.Value().routes) {
        // 找到（或新建）该 daq_list 的 ODT 视图组
        auto list_it =
            std::find_if(m_routes_.begin(), m_routes_.end(),
                         [&](const auto& l) { return l.first == r.daq_list; });
        if (list_it == m_routes_.end()) {
            m_routes_.emplace_back(r.daq_list, std::vector<OdtView>{});
            list_it = std::prev(m_routes_.end());
        }
        auto& odts = list_it->second;
        // 找到（或新建）该 ODT 的切片视图
        auto it = std::find_if(
            odts.begin(), odts.end(),
            [&](const OdtView& v) { return v.odt_number == r.odt; });
        if (it == odts.end()) {
            odts.push_back(OdtView{r.odt, {}});
            it = std::prev(odts.end());
        }
        it->slices.push_back(OdtSlice{r.name,
                                      r.payload_offset,
                                      r.size,
                                      false,
                                      r.is_array,
                                      r.element_count,
                                      r.element_size_bytes,
                                      r.packed_stride});
    }

    m_plan_generation_ = master.DaqConfigGeneration();
    m_prepared_ = true;
}

void MeasurementSession::Start(MeasurementCallback callback) {
    if (!m_bound_) {
        ThrowInvalid("MeasurementSession::Start 未 Bind master");
    }
    if (m_routes_.empty()) {
        ThrowInvalid("MeasurementSession::Start 未 Prepare");
    }
    if (m_plan_generation_ != m_master_->DaqConfigGeneration()) {
        ThrowInvalid("MeasurementSession::Start DAQ 配置已在 Prepare 后变化");
    }
    {
        std::lock_guard<std::mutex> lock(m_run_mutex_);
        if (m_running_) {
            ThrowInvalid("MeasurementSession::Start 已在运行");
        }
        m_callback_ = std::move(callback);
        m_queue_.clear();
        m_stop_requested_ = false;
        m_running_ = true;
    }

    try {
        if (m_id_field_type_ == IdentificationFieldType::Absolute) {
            m_first_pid_ = m_master_->StartDaqList(0U);
            if (static_cast<std::size_t>(m_first_pid_) +
                    m_routes_.front().second.size() > 256U) {
                throw XcpException(
                    ErrorCategory::UnsupportedFeature,
                    "FIRST_PID 加 ODT 范围超出 PID 字节，拒绝解码");
            }
        } else {
            m_master_->StartDaqSync();
        }

        m_worker_ = std::make_unique<std::thread>(
            &MeasurementSession::WorkerLoop, this);
    } catch (...) {
        try {
            m_master_->StopDaq();
        } catch (...) {
            // 保留首个启动异常；断开连接仍会收尾残余 Slave 状态。
        }
        {
            std::lock_guard<std::mutex> lock(m_run_mutex_);
            m_running_ = false;
            m_stop_requested_ = false;
            m_queue_.clear();
            m_callback_ = nullptr;
        }
        throw;
    }
}

void MeasurementSession::Stop() {
    {
        std::lock_guard<std::mutex> lock(m_run_mutex_);
        if (!m_running_) {
            return;
        }
        m_stop_requested_ = true;
        m_running_ = false;
    }
    m_run_cv_.notify_all();
    if (m_worker_ && m_worker_->joinable()) {
        m_worker_->join();
        m_worker_.reset();
    }
    if (m_bound_) {
        m_master_->StopDaq();
    }
    m_callback_ = nullptr;
}

bool MeasurementSession::Running() const noexcept {
    std::lock_guard<std::mutex> lock(m_run_mutex_);
    return m_running_;
}

MeasurementStatistics MeasurementSession::Statistics() const {
    MeasurementStatistics s;
    s.dto_received = m_dto_received_.load();
    s.dto_dropped = m_dto_dropped_.load();
    s.decode_errors = m_decode_errors_.load();
    s.timestamp_wraps = m_timestamp_wraps_.load();
    s.counter_gaps = m_counter_gaps_.load();
    s.counter_lost = m_counter_lost_.load();
    return s;
}

void MeasurementSession::OnDto(const DtoPacket& dto) {
    // RX 线程：只入队，绝不阻塞。有界队列满 → 丢弃计数。
    std::lock_guard<std::mutex> lock(m_run_mutex_);
    if (!m_running_) {
        return;
    }
    m_dto_received_.fetch_add(1, std::memory_order_relaxed);
    if (m_queue_.size() >= kMaxQueue) {
        m_queue_.pop_front();
        m_dto_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    m_queue_.emplace_back(dto.data.begin(), dto.data.end());
    // 入队必须唤醒 worker（v0.9 L3 暴露）：worker 阻塞在 m_run_cv_.wait 上，
    // 没有 notify 就要等到 Stop 才一次性排空队列——实时流形同虚设。
    // 队列仍持锁，唤醒后 worker 重新判定谓词，无丢帧竞态。
    m_run_cv_.notify_all();
}

void MeasurementSession::NotifyWorker() {
    m_run_cv_.notify_all();
}

void MeasurementSession::WorkerLoop() {
    // 每帧解码用的信封布局：识别字段模式/头长来自 SetEnvelopeMode 取证
    // （默认 RelativeWord，2 字节），时间戳段由解码器按 timestamp_size_bits
    // 在识别字段**之后**单独消费——因此 header_bytes 只计识别字段，
    // 绝不能把时间戳再计入（否则重复扣字节，帧被误判过短）。
    DtoFrameLayout layout;
    layout.identification_field_type = m_id_field_type_;
    layout.first_odt = m_id_field_type_ == IdentificationFieldType::Absolute
                           ? m_first_pid_
                           : 0U;
    // B2：计数器位只来自 Prepare 的回读确认（m_counter_active_），
    // 绝不按“配置请求了”就假定生效——错 1 字节即整帧净荷错位。
    layout.counter_enabled = m_counter_active_;
    layout.timestamp_enabled = m_timestamp_bytes_ > 0;
    layout.timestamp_size_bits =
        static_cast<std::uint8_t>(m_timestamp_bytes_ * 8U);
    layout.overflow_indicator = false;
    layout.pid_off = false;
    layout.header_bytes = m_id_field_bytes_;
    // v0.9：XCPlite 类 Slave 只在事件首 ODT 帧带时间戳（D13）。开关默认
    // false，关闭时与 v0.2/v0.6 口径逐字节一致（既有测试语义不变）。
    layout.timestamp_relative_first_only = m_timestamp_first_odt_only_;

    // (daq_list, odt) → 切片视图 快速下标（Prepare 时 route 结构）
    std::vector<std::pair<std::uint16_t, std::vector<OdtView>>> routes =
        m_routes_;

    DtoEnvelopeDecoder decoder(m_byte_order_);

    // v0.6 时间戳硬化：每个 DAQ List（事件通道）一条独立换算流——不同
    // 通道的原始计数互不相干，混用一个实例会把两路 raw 的差值误判为回卷
    // （架构文档 §12）。位宽 = Prepare 取证字节数 ×8；单位 = SetTimestampUnit
    // 注入（0=未知 → valid=false 仅保留 raw，不猜，B-3/R13）。转换器是
    // worker 线程局部状态：Stop→Start 重建线程即自然复位基线（不跨流猜
    // 续接），历史回卷数则累进会话统计（跨 Stop→Start 连续计数）。
    const std::uint8_t ts_bits =
        static_cast<std::uint8_t>(m_timestamp_bytes_ * 8U);
    std::map<std::uint16_t, DaqTimestampConverter> converters;

    // B2 Counter 贯通：逐 DAQ List 记住“上一次观测到的计数”（模 256）。
    // 计数器由 Slave 逐帧自增，缺口即可观测的丢帧；这里只上报，不重排、
    // 不补偿——丢帧成因（传输错误/被覆盖/迟到）无法仅凭计数器判定。
    std::map<std::uint16_t, std::uint8_t> last_counters;

    for (;;) {
        Bytes frame;
        {
            std::unique_lock<std::mutex> lock(m_run_mutex_);
            m_run_cv_.wait(lock, [&] {
                return (!m_queue_.empty()) || m_stop_requested_;
            });
            if (m_stop_requested_ && m_queue_.empty()) {
                break;
            }
            frame = std::move(m_queue_.front());
            m_queue_.pop_front();
        }

        // 解码信封（失败该帧 → decode_errors++，drop 计数）
        DtoEnvelope envelope;
        try {
            envelope = decoder.Decode(BytesView{frame}, layout);
        } catch (const XcpException&) {
            LIBXCP_LOG_WARNING("DTO envelope decode failed; frame dropped");
            m_decode_errors_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // 定位路由：找 routes 中 (daq_list) → odt 匹配切片组
        const std::uint16_t dq = envelope.identity.daq_list;
        const std::uint8_t odt = envelope.identity.odt;
        const OdtView* view = nullptr;
        for (const auto& [list_entry, odt_views] : routes) {
            if (list_entry == dq) {
                for (const OdtView& v : odt_views) {
                    if (v.odt_number == odt) {
                        view = &v;
                        break;
                    }
                }
            }
            if (view != nullptr) {
                break;
            }
        }
        if (view == nullptr) {
            // 未匹配到已规划 ODT：视为无样本帧，不计数错误
            continue;
        }

        // B2 Counter 贯通：按 DAQ List 校验计数连续性（逐帧 +1，模 256）。
        // 只有真正参与测量的帧才推进期望值，避免无样本帧污染序列。
        std::uint8_t counter_lost = 0;
        if (envelope.counter.has_value()) {
            const std::uint8_t current = *envelope.counter;
            const auto last_it = last_counters.find(dq);
            if (last_it != last_counters.end()) {
                // 差值为模 256 的当前值减上一观测值：0=重复帧，1=连续，
                // 其余 delta-1 即观测到的丢帧数。
                const std::uint8_t delta =
                    static_cast<std::uint8_t>(current - last_it->second);
                if (delta == 0U) {
                    // 与上一帧同值：重发/迟到帧，记一次不连续但无丢帧
                    m_counter_gaps_.fetch_add(1, std::memory_order_relaxed);
                } else if (delta != 1U) {
                    counter_lost = static_cast<std::uint8_t>(delta - 1U);
                    m_counter_gaps_.fetch_add(1, std::memory_order_relaxed);
                    m_counter_lost_.fetch_add(counter_lost,
                                              std::memory_order_relaxed);
                }
            }
            last_counters[dq] = current;
        }

        // ---- B4：本 DTO 内打包了多少个采样 ----
        // 非 packed 恒为 1；packed 时由 Prepare 回读确认后的列表参数给出。
        std::uint16_t sample_total = 1;
        std::uint64_t sample_interval_ns = 0;
        bool first_sample_timestamp = false;
        if (m_packed_active_) {
            const auto packed_it = m_packed_lists_.find(dq);
            if (packed_it != m_packed_lists_.end()) {
                sample_total = packed_it->second.sample_count;
                sample_interval_ns = packed_it->second.sample_interval_ns;
                first_sample_timestamp =
                    packed_it->second.first_sample_timestamp;
            }
        }
        if (sample_total == 0U) {
            sample_total = 1U;
        }

        // ---- 时间戳锚点 ----
        // v0.6：raw 计数经每流独立转换器做回卷延展；位宽或单位未知时不猜
        // ——timestamp_valid=false，仅保留 timestamp_raw。packed 下一帧只有
        // 一个时间戳（锚点采样），其余采样按事件周期 × 降频因子重建。
        bool base_valid = false;
        std::chrono::nanoseconds base_ts{};
        if (envelope.raw_timestamp.has_value()) {
            auto conv_it = converters.find(dq);
            if (conv_it == converters.end()) {
                conv_it =
                    converters
                        .emplace(
                            dq, DaqTimestampConverter(m_timestamp_unit_ns_,
                                                      ts_bits))
                        .first;
            }
            const std::uint64_t wraps_before = conv_it->second.WrapCount();
            const DaqTimestamp ts =
                conv_it->second.Convert(*envelope.raw_timestamp);
            const std::uint64_t wraps_added =
                conv_it->second.WrapCount() - wraps_before;
            if (wraps_added != 0U) {
                m_timestamp_wraps_.fetch_add(wraps_added,
                                             std::memory_order_relaxed);
            }
            if (ts.valid) {
                base_valid = true;
                base_ts = ts.value;
            }
        }

        for (std::uint16_t sample_index = 0; sample_index < sample_total;
             ++sample_index) {
            // 按切片取出原始字节：packed 时第 k 个采样位于
            // offset + k * packed_stride（stride 由规划器按分组方式给出）。
            std::vector<PayloadSlice> specs;
            specs.reserve(view->slices.size());
            for (const OdtSlice& s : view->slices) {
                const std::size_t offset =
                    s.offset +
                    static_cast<std::size_t>(sample_index) * s.packed_stride;
                specs.push_back(PayloadSlice{s.name, offset, s.size});
            }
            const std::vector<DecodedSlice> slices =
                DecodePayload(envelope.payload, specs);

            // 换算 + 构造帧
            MeasurementFrame frame_out;
            frame_out.daq_list = dq;
            frame_out.odt = odt;
            frame_out.counter = envelope.counter;
            frame_out.counter_lost_before = counter_lost;
            frame_out.sample_index = sample_index;
            frame_out.sample_count = sample_total;
            if (envelope.raw_timestamp.has_value()) {
                frame_out.timestamp_raw = *envelope.raw_timestamp;
            }
            if (base_valid) {
                // 相对锚点的采样步数：ts_mode=0 时锚点是末采样，
                // ts_mode=1 时锚点是首采样
                const std::uint64_t steps =
                    first_sample_timestamp
                        ? static_cast<std::uint64_t>(sample_index)
                        : static_cast<std::uint64_t>(sample_total - 1U -
                                                     sample_index);
                if (steps == 0U) {
                    frame_out.timestamp_valid = true;
                    frame_out.timestamp = base_ts;
                    frame_out.timestamp_source =
                        MeasurementTimestampSource::Dto;
                } else {
                    const std::uint64_t offset_ns = steps * sample_interval_ns;
                    if (first_sample_timestamp) {
                        frame_out.timestamp_valid = true;
                        frame_out.timestamp =
                            base_ts + std::chrono::nanoseconds(offset_ns);
                        frame_out.timestamp_source =
                            MeasurementTimestampSource::Reconstructed;
                    } else if (base_ts.count() >= 0 &&
                               static_cast<std::uint64_t>(base_ts.count()) >=
                                   offset_ns) {
                        frame_out.timestamp_valid = true;
                        frame_out.timestamp =
                            base_ts - std::chrono::nanoseconds(offset_ns);
                        frame_out.timestamp_source =
                            MeasurementTimestampSource::Reconstructed;
                    } else {
                        // 锚点是末采样但纳秒值不足以回退：不猜，标记无效
                        frame_out.timestamp_valid = false;
                        frame_out.timestamp_source =
                            MeasurementTimestampSource::None;
                    }
                }
            }
            frame_out.samples.reserve(slices.size());
            for (const DecodedSlice& ds : slices) {
                MeasurementSample sample;
                sample.name = ds.name;
                sample.raw = ds.raw;
                sample.valid = ds.valid;
                sample.timestamp = frame_out.timestamp;
                const auto route_it = std::find_if(
                    view->slices.begin(), view->slices.end(),
                    [&](const OdtSlice& slice) { return slice.name == ds.name; });
                if (route_it == view->slices.end()) {
                    sample.valid = false;
                    sample.error = "DTO 切片缺少已规划路由";
                } else {
                    sample.is_array = route_it->is_array;
                    if (!ds.valid) {
                        sample.error = "DTO 原始切片解码失败";
                    } else if (!route_it->is_array) {
                        const MeasurementResult<MeasurementValue> conv =
                            m_database_.ToPhysical(ds.name, BytesView{ds.raw});
                        if (conv.HasValue()) {
                            sample.value = conv.Value();
                        } else {
                            sample.valid = false;
                            sample.error = conv.ErrorInfo().message;
                        }
                    } else if (route_it->element_count == 0U ||
                               route_it->element_size_bytes == 0U ||
                               route_it->element_count >
                                   std::numeric_limits<std::size_t>::max() /
                                       route_it->element_size_bytes ||
                               ds.raw.size() !=
                                   static_cast<std::size_t>(route_it->element_count) *
                                       route_it->element_size_bytes) {
                        sample.valid = false;
                        sample.error = "DTO 数组字节数与已规划元素布局不匹配";
                    } else {
                        std::vector<MeasurementValue> elements;
                        elements.reserve(route_it->element_count);
                        for (std::uint64_t index = 0;
                             index < route_it->element_count; ++index) {
                            const std::size_t byte_offset =
                                static_cast<std::size_t>(index) *
                                route_it->element_size_bytes;
                            const BytesView element_raw{
                                ds.raw.data() + byte_offset,
                                route_it->element_size_bytes};
                            const MeasurementResult<MeasurementValue> converted =
                                m_database_.ToPhysicalElement(ds.name, index,
                                                              element_raw);
                            if (!converted.HasValue()) {
                                sample.valid = false;
                                sample.error = converted.ErrorInfo().message;
                                elements.clear();
                                break;
                            }
                            elements.push_back(converted.Value());
                        }
                        if (sample.valid &&
                            elements.size() == route_it->element_count) {
                            sample.elements = std::move(elements);
                        } else {
                            sample.valid = false;
                            if (sample.error.empty()) {
                                sample.error = "数组元素换算未生成完整样本";
                            }
                        }
                    }
                }
                // 截断 DTO 切片维持原有降级语义：仍回调无效样本，但只对完整切片的
                // 换算/数组布局失败递增解码错误计数（畸形整帧在上层单独计数）。
                if (!sample.valid && ds.valid) {
                    m_decode_errors_.fetch_add(1, std::memory_order_relaxed);
                }
                frame_out.samples.push_back(std::move(sample));
            }

            MeasurementCallback cb;
            {
                std::lock_guard<std::mutex> lock(m_run_mutex_);
                cb = m_callback_;
            }
            if (cb) {
                cb(frame_out);
            }
        }

    }
}

}  // namespace calmcar::xcp