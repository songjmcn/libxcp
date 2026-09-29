/**
 * @file measurement_planner.cpp
 * @brief 测量规划器实现（v0.4）。
 *
 * 装箱策略（架构文档 §9，确定性、不最优）：按事件通道分组 → 保持用户顺序 →
 * 顺序填充当前 ODT → 超限开新 ODT → 单变量不跨 ODT → 一个 Entry = 一个
 * Measurement。MAX_DTO / AG / timestamp 头全部来自会话，不猜测。
 */

#include "libxcp/measurement/measurement_planner.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace calmcar::xcp {

MeasurementPlanner::MeasurementPlanner(
    const IMeasurementDatabase& database, std::uint16_t max_dto,
    AddressGranularity address_granularity, std::size_t timestamp_bytes)
    : m_database_(database),
      m_max_dto_(max_dto),
      m_ag_bytes_(AgToBytes(address_granularity)),
      m_timestamp_bytes_(timestamp_bytes) {}

MeasurementPlanner::~MeasurementPlanner() = default;

MeasurementResult<MeasurementPlan> MeasurementPlanner::Build(
    const std::vector<std::string>& names) const {
    // ---- 第一步：逐个查库，取得布局；任一失败即整体失败（不产出半成品） ----
    struct Row {
        std::string name;
        MeasurementSymbolInfo info;
        std::size_t byte_width;  // 实占字节 = 元素宽度 × 元素数（字节口径）
        std::size_t ag_units;   // 以 AG 为单位的元素数（DaqEntrySpec.size 口径）
    };
    std::vector<Row> rows;
    rows.reserve(names.size());
    for (const auto& name : names) {
        auto found = m_database_.Find(name);
        if (!found) {
            return detail::MakeMeasurementError<MeasurementPlan>(
                found.ErrorInfo().code,
                "规划失败：查库未命中测量 '" + name + "': " +
                    found.ErrorInfo().message,
                name);
        }
        const auto& info = found.Value();
        if (info.element_size_bytes == 0U || info.element_count == 0U) {
            return detail::MakeMeasurementError<MeasurementPlan>(
                MeasurementErrorCode::UnsupportedDataType,
                "测量 '" + name + "' 元素宽度/数量非法（element_size_bytes=" +
                    std::to_string(info.element_size_bytes) +
                    " element_count=" + std::to_string(info.element_count) +
                    "）",
                name);
        }
        const std::size_t byte_width =
            static_cast<std::size_t>(info.element_size_bytes) *
            static_cast<std::size_t>(info.element_count);
        // DaqEntrySpec.size 以 AG 为单位（xcp_master 逐 ODT 预检按
        // payload = Σ size*ag_bytes 计数）。字节→AG 换算必须在规划器内做
        // （计划 §5.3 硬核对项）；不可整除 → 非法布局。
        if (byte_width % m_ag_bytes_ != 0U) {
            return detail::MakeMeasurementError<MeasurementPlan>(
                MeasurementErrorCode::InvalidLayout,
                "测量 '" + name + "' 实占 " + std::to_string(byte_width) +
                    " 字节不能被 AG(" + std::to_string(m_ag_bytes_) +
                    "字节) 整除，无法映射为 DaqEntrySpec.size",
                name);
        }
        const std::size_t ag_units = byte_width / m_ag_bytes_;
        // DaqEntrySpec.size 是 uint8_t：AG 单位超 8 位上限的巨型变量无法装箱
        if (ag_units > 255U) {
            return detail::MakeMeasurementError<MeasurementPlan>(
                MeasurementErrorCode::DaqConfigurationError,
                "测量 '" + name + "' 需 " + std::to_string(ag_units) +
                    " 个 AG(" + std::to_string(m_ag_bytes_) + "字节) 元素，超过 "
                    "DaqEntrySpec.size 8 位上限(255)或单 ODT 覆盖能力",
                name);
        }
        rows.push_back(Row{name, info, byte_width, ag_units});
    }

    // ---- 第二步：确定可用的 ODT 净荷容量（不含信封头） ----
    const std::size_t max_dto_bytes = static_cast<std::size_t>(m_max_dto_);
    if (m_timestamp_bytes_ > max_dto_bytes) {
        // 会用掉的信封头都超 MAX_DTO：不可能装任何净荷
        return detail::MakeMeasurementError<MeasurementPlan>(
            MeasurementErrorCode::DaqConfigurationError,
            "信封头字节数 " + std::to_string(m_timestamp_bytes_) +
                " 已超 MAX_DTO " + std::to_string(m_max_dto_));
    }
    const std::size_t odt_id_bytes = 1;  // 识别字段（相对 ODT 模式至少 1 字节 vs 头）
    const std::size_t usable =
        max_dto_bytes - m_timestamp_bytes_ - odt_id_bytes;

    // ---- 单变量跨 ODT / 超容量检验 ----
    for (const auto& row : rows) {
        if (row.byte_width > usable) {
            return detail::MakeMeasurementError<MeasurementPlan>(
                MeasurementErrorCode::DaqConfigurationError,
                "测量 '" + row.name + "' 需 " + std::to_string(row.byte_width) +
                    " 字节净荷，超过单 ODT 可用容量 " +
                    std::to_string(usable) +
                    "（MAX_DTO=" + std::to_string(m_max_dto_) +
                    " timestamp_bytes=" + std::to_string(m_timestamp_bytes_) +
                    " 识别字段=" + std::to_string(odt_id_bytes) + "）",
                row.name);
        }
    }

    // ---- 第三步：按 event_channel 分组（0=不由通道触发 → 单独成组一），
    //     保持用户顺序。 ----
    // 用 稳定分区：先收集所有 event_channel（含 0，0 成组一），其余按首次出现
    // 顺序追加。简单实现：两遍扫描。
    std::vector<Row> grouped;
    grouped.reserve(rows.size());
    // 组一：event_channel==0（不含事件的测量）。先放所有 0。
    std::vector<std::uint16_t> seen_groups;  // 已建立的非零组，保持首次出现顺序
    for (const auto& row : rows) {
        if (row.info.event_channel == 0U) {
            grouped.push_back(row);
        } else {
            const auto it = std::find(seen_groups.begin(), seen_groups.end(),
                                      row.info.event_channel);
            if (it == seen_groups.end()) {
                seen_groups.push_back(row.info.event_channel);
            }
        }
    }
    // 非零组按 seen_groups 首次出现顺序逐组追加（组内保持用户顺序）
    for (const auto chan : seen_groups) {
        for (const auto& row : rows) {
            if (row.info.event_channel == chan) {
                grouped.push_back(row);
            }
        }
    }

    // ---- 第四步：装箱（每 List 起始 ODT 号 0；顺序填充） ----
    MeasurementPlan plan;
    // 当前正在填充的 DAQ List 上下文
    struct Active {
        DaqListSpec spec;
        std::uint16_t event_channel{0};
        std::size_t odt_used{0};  // 当前 ODT 已占净荷字节
    };
    Active cur;
    bool has_active = false;
    std::uint16_t daq_list_counter = 0;

    // 组内顺序即 grouped 顺序；同组连续，跨组即新 List。
    for (const auto& row : grouped) {
        const bool start_new_list =
            (!has_active) || (cur.event_channel != row.info.event_channel);

        if (start_new_list) {
            if (has_active) {
                plan.daq_lists.push_back(std::move(cur.spec));
            }
            cur = Active{};
            cur.event_channel = row.info.event_channel;
            // 序号从 0..N-1（ConfigureDaqListsDynamic 硬门槛）
            cur.spec.daq_list = daq_list_counter++;
            cur.spec.event_channel = row.info.event_channel;
            cur.spec.prescaler = 1;
            cur.spec.priority = 0;
            cur.spec.stim_direction = false;
            cur.spec.dto_counter = false;
            cur.spec.timestamp = (m_timestamp_bytes_ > 0);
            cur.spec.pid_off = false;
            // 首 ODT
            cur.spec.odts.emplace_back();
            cur.odt_used = 0;
            has_active = true;
        }

        // 当前 ODT 装不下 → 开新 ODT（同组内新 ODT 仍属当前 List）
        if (cur.odt_used + row.byte_width > usable) {
            cur.spec.odts.emplace_back();
            cur.odt_used = 0;
        }
        DaqOdtSpec& odt = cur.spec.odts.back();
        const std::size_t payload_offset = cur.odt_used;  // 记录偏移（此时尚未递增）
        // DaqEntrySpec.size 以 AG 为单位（xcp_master 预检按 Σ size*ag_bytes 计）
        odt.entries.emplace_back(DaqEntrySpec{
            row.info.address, row.info.extension,
            static_cast<std::uint8_t>(row.ag_units), kDaqBitOffsetNone});
        cur.odt_used += row.byte_width;
        const std::uint8_t odt_number =
            static_cast<std::uint8_t>(cur.spec.odts.size() - 1);
        plan.routes.push_back(MeasurementRoute{
            row.name, cur.spec.daq_list, odt_number, payload_offset,
            row.byte_width});
    }

    if (has_active) {
        plan.daq_lists.push_back(std::move(cur.spec));
    }

    // routes 与 daq_lists 的 (daq_list, odt, 净荷偏移) 一一对应（由装箱逻辑保证）
    return MeasurementResult<MeasurementPlan>(std::move(plan));
}

}  // namespace calmcar::xcp