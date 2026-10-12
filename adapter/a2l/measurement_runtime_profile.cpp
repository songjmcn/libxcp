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

MeasurementResult<GenericMeasurementRuntimeProfile>
BuildGenericMeasurementRuntimeProfile(
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
    if (processor->key_byte.identification_field_type != 0U) {
        return ProfileError("generic profile 仅支持经取证的 Absolute PID DTO (DAQ_KEY=0)");
    }
    if (processor->key_byte.address_extension_mode != 0U) {
        return ProfileError("generic profile 仅支持 FREE 地址扩展模式");
    }
    if (processor->min_daq != 0U || processor->max_daq == 0U) {
        return ProfileError("generic profile 要求动态 DAQ List 编号从 0 开始且容量非零");
    }
    const auto properties =
        static_cast<DaqProcessorPropertyBit>(processor->properties);
    if (!HasDaqProcessorProperty(properties,
                                 DaqProcessorPropertyBit::kConfigType)) {
        return ProfileError("Slave 未声明动态 DAQ 配置能力");
    }

    const auto resolution = master.QueryDaqResolutionInfo();
    if (!resolution.has_value()) {
        return ProfileError("Slave 不支持或未返回 GET_DAQ_RESOLUTION_INFO");
    }
    if (resolution->timestamp_mode.fixed) {
        return ProfileError("Slave 强制固定时间戳，generic 无时间戳布局不适用");
    }
    const auto session = master.GetSessionParameters();
    const std::size_t ag_bytes = AgToBytes(session.connect.address_granularity);
    if (resolution->granularity_daq == 0U ||
        resolution->granularity_daq != ag_bytes ||
        resolution->max_odt_entry_size_daq == 0U) {
        return ProfileError("DAQ Entry granularity/max-size 与 AG 不兼容");
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

    GenericMeasurementRuntimeProfile profile;
    profile.daq_entry_granularity_bytes = resolution->granularity_daq;
    profile.max_daq_entry_size_bytes = resolution->max_odt_entry_size_daq;
    return profile;
}

MeasurementResult<XcpliteMeasurementRuntimeProfile>
BuildXcpliteMeasurementRuntimeProfile(
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
    // XCPlite 以 wire code 3 声明 DAQ_HDR_ODT_FIL_DAQW，对应
    // [ODTrel][0xAA][DAQ16 LE] 信封；解码配置使用 RelativeByte + 4 字节头，
    // 仅是厂商配置映射，不代表通用 XCP 映射。
    if (processor->key_byte.identification_field_type != 3U) {
        return ProfileError("当前运行时识别字段不是 XCPlite RelativeByte 方言");
    }
    // XCPlite 的 MAX_DAQ 当前返回已分配的 DAQ List 数，而非总容量；
    // 尚未配置时为 0 是正常状态。这里只能用 MIN_DAQ 验证编号从 0 开始。
    if (processor->min_daq != 0U) {
        return ProfileError("XCPlite profile 要求动态 DAQ List 从 0 开始");
    }
    const auto daq_properties =
        static_cast<DaqProcessorPropertyBit>(processor->properties);
    if (!HasDaqProcessorProperty(daq_properties,
                                 DaqProcessorPropertyBit::kConfigType) ||
        !HasDaqProcessorProperty(daq_properties,
                                 DaqProcessorPropertyBit::kTimestamp)) {
        return ProfileError("Slave 未声明动态 DAQ 与时间戳能力");
    }

    const auto resolution = master.QueryDaqResolutionInfo();
    if (!resolution.has_value()) {
        return ProfileError("Slave 不支持或未返回 GET_DAQ_RESOLUTION_INFO");
    }
    if (!resolution->timestamp_mode.fixed ||
        resolution->timestamp_mode.size_code != 4U) {
        return ProfileError("XCPlite profile 要求固定 32-bit 时间戳");
    }
    if (resolution->timestamp_ticks == 0U) {
        return ProfileError("DAQ 时间戳 ticks 必须大于 0");
    }
    const auto session = master.GetSessionParameters();
    if (session.connect.byte_order != ByteOrder::Intel) {
        return ProfileError("当前 XCPlite 时间戳解码仅验证 Intel 字节序");
    }

    // XCPlite 的时间戳单位码 0..6 对应 1ns..1ms；该换算仅限本命名配置，
    // 不用于解释通用 XCP 单位码。
    constexpr std::array<std::uint64_t, 7> kXcpliteUnitNanoseconds{
        1ULL, 10ULL, 100ULL, 1'000ULL, 10'000ULL, 100'000ULL, 1'000'000ULL};
    const std::uint8_t unit_code = resolution->timestamp_mode.unit_code;
    if (unit_code >= kXcpliteUnitNanoseconds.size()) {
        return ProfileError("XCPlite profile 遇到未知时间戳单位码");
    }
    const std::uint64_t unit_ns = kXcpliteUnitNanoseconds[unit_code];
    if (resolution->timestamp_ticks >
        std::numeric_limits<std::uint64_t>::max() / unit_ns) {
        return ProfileError("时间戳单位换算溢出");
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
            return ProfileError("Slave 未返回有效的 A2L EVENT 运行时信息或 DAQ 列表容量",
                                event.name);
        }
        if (runtime->time_cycle != event.time_cycle ||
            runtime->time_unit != event.time_unit) {
            return ProfileError(
                "A2L EVENT 周期/单位与 GET_DAQ_EVENT_INFO 不一致", event.name);
        }
    }

    XcpliteMeasurementRuntimeProfile profile;
    profile.timestamp_unit_ns =
        unit_ns * static_cast<std::uint64_t>(resolution->timestamp_ticks);
    return profile;
}

void ApplyMeasurementRuntimeProfile(
    MeasurementSession& session,
    const XcpliteMeasurementRuntimeProfile& profile) {
    session.SetEnvelopeMode(profile.identification_field_type,
                            profile.identification_header_bytes);
    session.SetTimestampFirstOdtOnly(profile.timestamp_first_odt_only);
    session.SetTimestampUnit(profile.timestamp_unit_ns);
}

}  // namespace calmcar::xcp
