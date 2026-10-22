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

/// @brief 由取证事实决定的 DTO 布局与运行时能力配置。
/// @details **不再区分厂商 profile**：识别字段模式由从站上报的
///          `DAQ_KEY_BYTE.identification_field_type` 自动决定（规范原始 PDF
///          第 95-96 页 Figure 50-53 定义了 0..3 四种模式），而非由调用方按
///          厂商名选择。这使任何符合规范的从站都能零配置对接。
///
///          **单位纪律（批次 E2/F-12 修正）**：`daq_entry_granularity_bytes`
///          是**字节数**（规范 GRANULARITY_ODT_ENTRY_SIZE_x 以 BYTE 表示）；
///          `max_daq_entry_size_elements` 是 **AG 单位**的元素数（规范
///          MAX_ODT_ENTRY_SIZE_x[AG]，原始 PDF 第 15 页 §4.1.2）。
///          旧字段名 `max_daq_entry_size_bytes` 的单位口径是错的。
struct MeasurementRuntimeProfile {
    /// @brief 识别字段模式（由 DAQ_KEY_BYTE 高 2 位取证得到）。
    IdentificationFieldType identification_field_type{
        IdentificationFieldType::Absolute};
    /// @brief 识别字段头长（字节），按 Figure 51/52/53 为 1/2/3/4。
    std::uint8_t identification_header_bytes{1};
    /// @brief DAQ Entry 粒度（字节），对应规范 GRANULARITY_ODT_ENTRY_SIZE_DAQ。
    std::uint8_t daq_entry_granularity_bytes{0};
    /// @brief 单 Entry 最大元素数（AG 单位），对应规范 MAX_ODT_ENTRY_SIZE_DAQ。
    std::uint8_t max_daq_entry_size_elements{0};
    /// @brief 时间戳单位换算后的纳秒值；0 表示未知（不猜测，帧时间戳标 invalid）。
    std::uint64_t timestamp_unit_ns{0};
    /// @brief 是否仅在首个 ODT 携带时间戳。
    bool timestamp_first_odt_only{true};
    /// @brief DAQ Entry 能力（供会话运行期核实真实容量）。
    MeasurementDaqCapabilities daq_entry_capabilities{};
    /// @brief 是否采用了 XCPlite 的时间戳单位码换算约定（取证记录，供上层知情）。
    /// @details 该换算表（单位码 0..6 → 1ns..1ms）是厂商约定，不是通用 XCP
    ///          码表。当且仅当从站特征与 XCPlite 相符时置真；否则
    ///          `timestamp_unit_ns` 保持 0，交由上层显式注入。
    bool used_xcplite_timestamp_unit_convention{false};
};

/// @brief 查询并校验从站 DTO 布局与运行时能力，并核对全部 A2L 事件。
/// @details 布局按取证到的识别字段模式自动选择：任何规范定义的模式
///          （0..3）都会被接受并按对应头长应用，不再要求调用方预先声明
///          厂商 profile。时间戳单位仅在从站特征与 XCPlite 相符时按厂商
///          约定换算，否则保持未知。
[[nodiscard]] MeasurementResult<MeasurementRuntimeProfile>
BuildMeasurementRuntimeProfile(
    XcpMaster& master, std::span<const A2lEventMetadata> a2l_events);

/// @brief 将已验证的运行时配置应用到新建或已停止的会话。
void ApplyMeasurementRuntimeProfile(
    MeasurementSession& session,
    const MeasurementRuntimeProfile& profile);

}  // namespace calmcar::xcp

#endif  // LIBXCP_MEASUREMENT_ADAPTER_MEASUREMENT_RUNTIME_PROFILE_HPP_
