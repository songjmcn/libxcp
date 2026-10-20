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

/// @brief 显式收集的 A2L/应用事件通道候选值。
/// @details 使用 uint64_t 保留原值；超范围候选必须拒绝，不得截断。
using MeasurementEventBindingCandidates =
    std::map<std::string, std::vector<std::uint64_t>>;

/// @brief 为测量数据库附加显式校验的事件绑定。
/// @details 缺少或冲突的绑定会使 Find() 失败；事件通道 0 是有效值。
class EventBoundMeasurementDatabase final : public IMeasurementDatabase {
public:
    EventBoundMeasurementDatabase(IMeasurementDatabase& inner,
                                  MeasurementEventBindingCandidates bindings);

    MeasurementResult<MeasurementSymbolInfo> Find(
        std::string_view name) const override;
    MeasurementResult<MeasurementValue> ToPhysical(
        std::string_view name, BytesView raw) const override;
    MeasurementResult<MeasurementValue> ToPhysicalElement(
        std::string_view name, std::uint64_t element_index,
        BytesView raw) const override;

private:
    IMeasurementDatabase& m_inner_;
    MeasurementEventBindingCandidates m_bindings_;
};

/// @brief 用于与运行时 GET_DAQ_EVENT_INFO 交叉校验的 A2L EVENT 信息。
struct A2lEventMetadata {
    std::string name;
    std::string short_name;
    std::uint16_t channel{0};
    std::uint8_t time_cycle{0};
    std::uint8_t time_unit{0};
    std::uint8_t type{1};
    std::uint8_t max_daq_list{0};
};

/// @brief 最小化的标准 Absolute-PID DTO 配置。
/// @details 不携带时间戳，并暂限于 FREE 地址扩展模式。
struct GenericMeasurementRuntimeProfile {
    IdentificationFieldType identification_field_type{
        IdentificationFieldType::Absolute};
    std::uint8_t identification_header_bytes{1};
    std::uint8_t daq_entry_granularity_bytes{0};
    std::uint8_t max_daq_entry_size_bytes{0};
};

/// @brief 根据处理器/分辨率查询得到的受限 XCPlite DTO 配置。
struct XcpliteMeasurementRuntimeProfile {
    IdentificationFieldType identification_field_type{
        IdentificationFieldType::RelativeByte};
    std::uint8_t identification_header_bytes{4};
    std::uint64_t timestamp_unit_ns{0};
    bool timestamp_first_odt_only{true};
    MeasurementDaqCapabilities daq_entry_capabilities{};
};

/// @brief 查询并校验保守的通用 Absolute-PID 无时间戳配置及 A2L 事件。
[[nodiscard]] MeasurementResult<GenericMeasurementRuntimeProfile>
BuildGenericMeasurementRuntimeProfile(
    XcpMaster& master, std::span<const A2lEventMetadata> a2l_events);

/// @brief 查询并校验 XCPlite 专用 DTO/时间戳配置及所有 A2L 事件。
/// @details 时间单位解释仅适用于 XCPlite，不代表通用 XCP 码表。
[[nodiscard]] MeasurementResult<XcpliteMeasurementRuntimeProfile>
BuildXcpliteMeasurementRuntimeProfile(
    XcpMaster& master, std::span<const A2lEventMetadata> a2l_events);

/// @brief 将已验证的运行时配置应用到新建或已停止的会话。
void ApplyMeasurementRuntimeProfile(
    MeasurementSession& session,
    const XcpliteMeasurementRuntimeProfile& profile);

}  // namespace calmcar::xcp

#endif  // LIBXCP_MEASUREMENT_ADAPTER_MEASUREMENT_RUNTIME_PROFILE_HPP_
