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

MeasurementResult<XcpliteMeasurementRuntimeProfile>
BuildXcpliteMeasurementRuntimeProfile(
    XcpMaster& master, std::span<const A2lEventMetadata> a2l_events) {
    if (a2l_events.empty()) {
        return ProfileError("A2L 未提供可验证的 EVENT 元数据");
    }

    const auto processor = master.QueryDaqProcessorInfo();
    if (!processor.has_value()) {
        return ProfileError("Slave 不支持或未返回 GET_DAQ_PROCESSOR_INFO");
    }
    // XCPlite advertises DAQ_HDR_ODT_FIL_DAQW (wire code 3) for its
    // [ODTrel][0xAA][DAQ16 LE] envelope. The decoder's XCPlite profile is
    // expressed as RelativeByte + 4-byte header; this is not generic XCP
    // mapping.
    if (processor->key_byte.identification_field_type != 3U) {
        return ProfileError("当前运行时识别字段不是 XCPlite RelativeByte 方言");
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

    // XCPlite's timestamp unit codes: 0..6 => 1ns..1ms. This table is
    // intentionally scoped to this named profile, not generic XCP semantics.
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
        if (!runtime.has_value()) {
            return ProfileError("Slave 未返回 A2L EVENT 对应的运行时事件信息",
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
