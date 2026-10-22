#include "measurement_runtime_profile.hpp"

#include <array>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "libxcp/measurement/measurement_session.hpp"
#include "libxcp/xcp_master.hpp"

namespace calmcar::xcp {
namespace {

MeasurementError ProfileError(std::string message, std::string symbol = {}) {
    return MeasurementError{MeasurementErrorCode::DaqConfigurationError,
                            std::move(message), std::move(symbol)};
}

/// @brief XCPlite 时间戳单位码到纳秒的换算表（厂商约定，非通用 XCP 码表）。
/// @details 单位码 0..6 依次对应 1ns、10ns、100ns、1us、10us、100us、1ms。
constexpr std::array<std::uint64_t, 7> kXcpliteUnitNanoseconds{
    1ULL, 10ULL, 100ULL, 1'000ULL, 10'000ULL, 100'000ULL, 1'000'000ULL};

}  // namespace

EventBoundMeasurementDatabase::EventBoundMeasurementDatabase(
    IMeasurementDatabase& inner, MeasurementEventBindingCandidates bindings)
    : m_inner_(inner), m_bindings_(std::move(bindings)) {}

MeasurementResult<MeasurementSymbolInfo> EventBoundMeasurementDatabase::Find(
    std::string_view name) const {
    MeasurementResult<MeasurementSymbolInfo> result = m_inner_.Find(name);
    if (!result.HasValue()) {
        return result;
    }

    const std::string key(name);
    const auto binding = m_bindings_.find(key);
    if (binding == m_bindings_.end() || binding->second.empty()) {
        return MeasurementError{MeasurementErrorCode::InvalidLayout,
                                "测量缺少显式 DAQ 事件通道绑定", key};
    }

    std::set<std::uint64_t> distinct_channels(binding->second.begin(),
                                              binding->second.end());
    if (distinct_channels.size() != 1U) {
        return MeasurementError{MeasurementErrorCode::InvalidLayout,
                                "测量事件通道绑定缺失或存在歧义", key};
    }
    const std::uint64_t channel = *distinct_channels.begin();
    if (channel > std::numeric_limits<std::uint16_t>::max()) {
        return MeasurementError{MeasurementErrorCode::InvalidLayout,
                                "测量事件通道超出 uint16_t 范围", key};
    }

    MeasurementSymbolInfo info = result.Value();
    info.event_channel = static_cast<std::uint16_t>(channel);
    return info;
}

MeasurementResult<MeasurementValue> EventBoundMeasurementDatabase::ToPhysical(
    std::string_view name, BytesView raw) const {
    return m_inner_.ToPhysical(name, raw);
}

MeasurementResult<MeasurementValue>
EventBoundMeasurementDatabase::ToPhysicalElement(
    std::string_view name, std::uint64_t element_index, BytesView raw) const {
    const MeasurementResult<MeasurementSymbolInfo> info = Find(name);
    if (!info.HasValue()) return info.ErrorInfo();
    if (!info.Value().is_array || element_index >= info.Value().element_count) {
        return MeasurementError{MeasurementErrorCode::InvalidLayout,
                                "数组元素索引超出已绑定 A2L 布局",
                                std::string{name}};
    }
    if (raw.size() != info.Value().element_size_bytes) {
        return MeasurementError{MeasurementErrorCode::RawSizeMismatch,
                                "数组元素原始长度与元素宽度不匹配",
                                std::string{name}};
    }
    return m_inner_.ToPhysicalElement(name, element_index, raw);
}

MeasurementResult<MeasurementRuntimeProfile> BuildMeasurementRuntimeProfile(
    XcpMaster& master, std::span<const A2lEventMetadata> a2l_events) {
    if (a2l_events.empty()) {
        return ProfileError("A2L 未提供可验证的 EVENT 元数据");
    }

    const GetStatusResponse status = master.QueryStatus();
    if (HasResource(status.resource_protection, Resource::Daq)) {
        return ProfileError("Slave DAQ 资源受保护；未配置 Seed&Key，不发送 FREE_DAQ");
    }

    const auto processor = master.QueryDaqProcessorInfo();
    if (!processor.has_value()) {
        return ProfileError("Slave 不支持或未返回 GET_DAQ_PROCESSOR_INFO");
    }
    // 识别字段模式由取证事实决定，不再由调用方按厂商选择。规范原始 PDF
    // 第 95-96 页 Figure 50-53 定义了 wire code 0..3 四种模式；本库解码器
    // 已全部支持。取值超出 0..3 才是真正的非法。
    const auto id_field_type = IdentificationFieldTypeFromKeyByte(
        processor->key_byte.identification_field_type);
    if (!id_field_type.has_value()) {
        return ProfileError(
            "DAQ_KEY_BYTE 的 identification 位域取值非法（超出 0..3）");
    }
    if (processor->key_byte.address_extension_mode != 0U) {
        return ProfileError("仅支持 FREE 地址扩展模式（0）");
    }
    // 规范原始 PDF 第 179 页 Table 136 与第 576-581 行：MAX_DAQ 是「可用的
    // DAQ List 总数」，且在 DAQ_CONFIG_TYPE = dynamic 时等于
    // MIN_DAQ + DAQ_COUNT，其中 DAQ_COUNT 是「已动态分配的列表数」。
    // 因此动态配置的从站在 ALLOC_DAQ 之前 MAX_DAQ 天生为 0，这是正常态，
    // **不构成「无 DAQ 容量」**——容量由 ALLOC_DAQ 的返回值裁决。
    // 旧实现把 `max_daq == 0` 当作无容量，会误拒所有动态配置从站
    // （含 XCPlite），这是本次修正的核心缺陷。
    const auto properties =
        static_cast<DaqProcessorPropertyBit>(processor->properties);
    const bool dynamic_config =
        HasDaqProcessorProperty(properties, DaqProcessorPropertyBit::kConfigType);
    if (processor->min_daq != 0U) {
        return ProfileError("要求 DAQ List 编号从 0 开始（MIN_DAQ 须为 0）");
    }
    // 本工具只会走动态配置路径（会发送 ALLOC_DAQ/FREE_DAQ），故必须声明
    // DYNAMIC 能力。静态配置的从站不适用本库的 DAQ 观测流程。
    if (!dynamic_config) {
        return ProfileError("Slave 未声明动态 DAQ 配置能力");
    }

    const auto resolution = master.QueryDaqResolutionInfo();
    if (!resolution.has_value()) {
        return ProfileError("Slave 不支持或未返回 GET_DAQ_RESOLUTION_INFO");
    }
    const auto session = master.GetSessionParameters();
    const std::size_t ag_bytes = AgToBytes(session.connect.address_granularity);
    if (resolution->granularity_daq == 0U ||
        resolution->granularity_daq != ag_bytes ||
        resolution->max_odt_entry_size_daq == 0U) {
        return ProfileError("DAQ Entry granularity/max-size 与 AG 不兼容");
    }

    // 时间戳单位换算：XCPlite 约定为「固定 32-bit 时间戳 + 单位码 0..6 对应
    // 1ns..1ms」。该换算表是厂商约定而非通用 XCP 码表，故仅当从站特征与之
    // 相符时采用；否则保持未知（0），由上层显式注入，绝不猜测。
    std::uint64_t timestamp_unit_ns = 0;
    bool used_xcplite_convention = false;
    if (resolution->timestamp_mode.fixed) {
        if (resolution->timestamp_mode.size_code != 4U) {
            return ProfileError("固定时间戳模式仅支持 32-bit（size code 4）");
        }
        if (resolution->timestamp_ticks == 0U) {
            return ProfileError("DAQ 时间戳 ticks 必须大于 0");
        }
        if (session.connect.byte_order != ByteOrder::Intel) {
            return ProfileError("固定时间戳解码仅验证 Intel 字节序");
        }
        const std::uint8_t unit_code = resolution->timestamp_mode.unit_code;
        if (unit_code >= kXcpliteUnitNanoseconds.size()) {
            return ProfileError("固定时间戳遇到未知单位码");
        }
        const std::uint64_t unit_ns = kXcpliteUnitNanoseconds[unit_code];
        if (resolution->timestamp_ticks >
            std::numeric_limits<std::uint64_t>::max() / unit_ns) {
            return ProfileError("时间戳单位换算溢出");
        }
        timestamp_unit_ns =
            unit_ns * static_cast<std::uint64_t>(resolution->timestamp_ticks);
        used_xcplite_convention = true;
    }

    std::map<std::uint16_t, std::pair<std::uint8_t, std::uint8_t>>
        checked_channels;
    for (const A2lEventMetadata& event : a2l_events) {
        if ((event.type != 1U && event.type != 3U) || event.max_daq_list == 0U) {
            return ProfileError("A2L EVENT 未声明 DAQ 类型或动态列表容量", event.name);
        }
        if (event.channel > processor->max_event_channel) {
            return ProfileError("A2L EVENT 通道超出 Slave 声明范围", event.name);
        }
        const auto [channel_it, inserted] = checked_channels.emplace(
            event.channel, std::pair{event.time_cycle, event.time_unit});
        if (!inserted) {
            if (channel_it->second.first != event.time_cycle ||
                channel_it->second.second != event.time_unit) {
                return ProfileError("A2L 同一事件通道含冲突的周期/单位",
                                    event.name);
            }
            continue;
        }
        const auto runtime = master.QueryDaqEventInfo(event.channel);
        if (!runtime.has_value() || runtime->max_daq_lists == 0U) {
            return ProfileError("Slave 未提供可用的 A2L EVENT 运行时信息",
                                event.name);
        }
        if (runtime->time_cycle != event.time_cycle ||
            runtime->time_unit != event.time_unit) {
            return ProfileError(
                "A2L EVENT 周期/单位与 GET_DAQ_EVENT_INFO 不一致", event.name);
        }
    }

    MeasurementRuntimeProfile profile;
    profile.identification_field_type = *id_field_type;
    // 头长按规范 Figure 51/52/53 由模式唯一决定，禁止各处硬编码漂移。
    profile.identification_header_bytes = static_cast<std::uint8_t>(
        IdentificationFieldMinimumBytes(*id_field_type));
    profile.daq_entry_granularity_bytes = resolution->granularity_daq;
    // E2/F-12：MAX_ODT_ENTRY_SIZE 的单位是 AG（不是字节），直接透传原值
    profile.max_daq_entry_size_elements = resolution->max_odt_entry_size_daq;
    profile.timestamp_unit_ns = timestamp_unit_ns;
    profile.timestamp_first_odt_only = true;
    profile.daq_entry_capabilities = MeasurementDaqCapabilities{
        resolution->timestamp_mode.fixed, resolution->granularity_daq,
        resolution->max_odt_entry_size_daq};
    profile.used_xcplite_timestamp_unit_convention = used_xcplite_convention;
    return profile;
}

void ApplyMeasurementRuntimeProfile(
    MeasurementSession& session,
    const MeasurementRuntimeProfile& profile) {
    session.SetEnvelopeMode(profile.identification_field_type,
                            profile.identification_header_bytes);
    session.SetTimestampFirstOdtOnly(profile.timestamp_first_odt_only);
    session.SetTimestampUnit(profile.timestamp_unit_ns);
    session.SetDaqEntryCapabilities(profile.daq_entry_capabilities);
}

}  // namespace calmcar::xcp
