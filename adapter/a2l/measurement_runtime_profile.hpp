#ifndef LIBXCP_MEASUREMENT_ADAPTER_MEASUREMENT_RUNTIME_PROFILE_HPP_
#define LIBXCP_MEASUREMENT_ADAPTER_MEASUREMENT_RUNTIME_PROFILE_HPP_

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "libxcp/daq/dto_envelope_types.hpp"
#include "libxcp/measurement/measurement_database.hpp"
#include "libxcp/measurement/measurement_result.hpp"

namespace calmcar::xcp {

class MeasurementSession;
class XcpMaster;

/// Candidate event IDs collected from an explicit A2L/application binding
/// source. uint64_t is intentional: out-of-range values must be rejected, not
/// truncated.
using MeasurementEventBindingCandidates =
    std::map<std::string, std::vector<std::uint64_t>>;

/// Decorates a measurement database with explicit, validated event bindings.
/// Missing or conflicting bindings fail Find(); event channel 0 is valid.
class EventBoundMeasurementDatabase final : public IMeasurementDatabase {
public:
    EventBoundMeasurementDatabase(IMeasurementDatabase& inner,
                                  MeasurementEventBindingCandidates bindings);

    MeasurementResult<MeasurementSymbolInfo> Find(
        std::string_view name) const override;
    MeasurementResult<MeasurementValue> ToPhysical(
        std::string_view name, BytesView raw) const override;

private:
    IMeasurementDatabase& m_inner_;
    MeasurementEventBindingCandidates m_bindings_;
};

/// A2L EVENT facts used to cross-check runtime GET_DAQ_EVENT_INFO.
struct A2lEventMetadata {
    std::string name;
    std::string short_name;
    std::uint16_t channel{0};
    std::uint8_t time_cycle{0};
    std::uint8_t time_unit{0};
};

/// Narrow XCPlite wire profile derived from processor/resolution queries.
struct XcpliteMeasurementRuntimeProfile {
    IdentificationFieldType identification_field_type{
        IdentificationFieldType::RelativeByte};
    std::uint8_t identification_header_bytes{4};
    std::uint64_t timestamp_unit_ns{0};
    bool timestamp_first_odt_only{true};
};

/// Query and validate the XCPlite-specific DTO/timestamp profile and each
/// supplied A2L event against the live Slave. This is deliberately not a
/// generic XCP unit-code interpretation.
[[nodiscard]] MeasurementResult<XcpliteMeasurementRuntimeProfile>
BuildXcpliteMeasurementRuntimeProfile(
    XcpMaster& master, std::span<const A2lEventMetadata> a2l_events);

/// Apply a previously validated runtime profile to a stopped/new session.
void ApplyMeasurementRuntimeProfile(
    MeasurementSession& session,
    const XcpliteMeasurementRuntimeProfile& profile);

}  // namespace calmcar::xcp

#endif  // LIBXCP_MEASUREMENT_ADAPTER_MEASUREMENT_RUNTIME_PROFILE_HPP_
