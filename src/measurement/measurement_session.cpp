/**
 * @file measurement_session.cpp
 * @brief 实现 MeasurementSession：把 DTO 流转成物理值帧（v0.5）。
 *
 * 数据通路见头文件：RX 线程 OnDto 只入队（有界，溢出计数）→ worker 线程
 * 取帧 → DtoEnvelopeDecoder 切信封 → 按路由切片（DtoPayloadDecoder）→
 * IMeasurementDatabase::ToPhysical 换算 → MeasurementFrame 回调。
 *
 * 兼容性说明：本实现的构造形态（MeasurementSession(IMeasurementDatabase&)
 * + Bind(XcpMaster&)）是计划 §5.5 字面签名 `(XcpMaster&, IMeasurementDatabase&)`
 * 的偏差——因为 XcpMaster 的事件监听器仅在构造时固定，session 必须先构造并以
 * &session 作为 master 监听器，master 引用随后 Bind。详见 v0.5 记录"偏差说明"。
 */

#include "libxcp/measurement/measurement_session.hpp"

#include <algorithm>  // std::find / std::find_if / std::prev
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
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

void MeasurementSession::SetTimestampFirstOdtOnly(bool first_only) {
    // v0.9：部分 Slave（XCPlite D13）只在事件首 ODT 帧附时间戳段。
    // 开关式取证注入（默认关=每帧带），运行中禁改（与 SetTimestampUnit 同律）。
    if (Running()) {
        ThrowState("MeasurementSession::SetTimestampFirstOdtOnly 运行中禁止");
    }
    m_timestamp_first_odt_only_ = first_only;
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
    m_timestamp_bytes_ = master.DaqTimestampBytesCached();

    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(m_names_mutex_);
        names = m_names_;
    }

    // 规划 + 下发（一个整体：失败不留半份账，ConfigureDaqListsDynamic 自回滚）
    MeasurementPlanner planner(m_database_, m_max_dto_,
                               params.connect.address_granularity,
                               m_timestamp_bytes_);
    const MeasurementResult<MeasurementPlan> plan = planner.Build(names);
    if (!plan.HasValue()) {
        // v0.6 硬化：按错误码映射异常类别上抛（不再统一 InvalidArgument）
        const MeasurementError err = plan.ErrorInfo();
        throw XcpException(CategoryOf(err.code), err.message);
    }

    master.ConfigureDaqListsDynamic(plan.Value().daq_lists);

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
        it->slices.push_back(OdtSlice{r.name, r.payload_offset, r.size, false});
    }

    m_plan_generation_ = master.DaqConfigGeneration();
}

void MeasurementSession::Start(MeasurementCallback callback) {
    if (!m_bound_) {
        ThrowInvalid("MeasurementSession::Start 未 Bind master");
    }
    if (m_routes_.empty()) {
        ThrowInvalid("MeasurementSession::Start 未 Prepare");
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

    m_master_->StartDaqSync();

    m_worker_ = std::make_unique<std::thread>(&MeasurementSession::WorkerLoop,
                                               this);
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
    layout.first_odt = 0;
    layout.counter_enabled = false;
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

        // 按切片取出原始字节
        std::vector<PayloadSlice> specs;
        specs.reserve(view->slices.size());
        for (const OdtSlice& s : view->slices) {
            specs.push_back(PayloadSlice{s.name, s.offset, s.size});
        }
        const std::vector<DecodedSlice> slices =
            DecodePayload(envelope.payload, specs);

        // 换算 + 构造帧
        MeasurementFrame frame_out;
        frame_out.daq_list = dq;
        frame_out.odt = odt;
        if (envelope.raw_timestamp.has_value()) {
            // v0.6：raw 计数经每流独立转换器做回卷延展；位宽或单位未知
            // 时不猜——timestamp_valid=false，仅保留 timestamp_raw。
            frame_out.timestamp_raw = *envelope.raw_timestamp;
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
                frame_out.timestamp_valid = true;
                frame_out.timestamp = ts.value;
            }
        }
        frame_out.samples.reserve(slices.size());
        for (const DecodedSlice& ds : slices) {
            MeasurementSample sample;
            sample.name = ds.name;
            sample.raw = ds.raw;
            sample.valid = ds.valid;
            sample.timestamp = frame_out.timestamp;
            if (ds.valid) {
                const MeasurementResult<MeasurementValue> conv =
                    m_database_.ToPhysical(ds.name, BytesView{ds.raw});
                if (conv.HasValue()) {
                    sample.value = conv.Value();
                } else {
                    sample.valid = false;
                    m_decode_errors_.fetch_add(1, std::memory_order_relaxed);
                }
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

}  // namespace calmcar::xcp