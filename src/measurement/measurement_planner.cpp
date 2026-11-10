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
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace calmcar::xcp {

MeasurementPlanner::MeasurementPlanner(
    const IMeasurementDatabase& database, std::uint16_t max_dto,
    AddressGranularity address_granularity, std::size_t timestamp_bytes,
    std::size_t identification_bytes,
    MeasurementDaqCapabilities daq_capabilities, bool dto_counter,
    std::size_t counter_header_bytes, DaqPackedMode packed_mode,
    std::uint16_t packed_sample_count)
    : m_database_(database),
      m_max_dto_(max_dto),
      m_ag_bytes_(AgToBytes(address_granularity)),
      m_timestamp_bytes_(timestamp_bytes),
      m_identification_bytes_(identification_bytes),
      m_daq_capabilities_(daq_capabilities),
      m_dto_counter_(dto_counter),
      m_counter_header_bytes_(counter_header_bytes),
      m_packed_mode_(packed_mode),
      m_packed_sample_count_(packed_sample_count) {}

MeasurementPlanner::~MeasurementPlanner() = default;

MeasurementResult<MeasurementPlan> MeasurementPlanner::Build(
    const std::vector<std::string>& names) const {
    // ---- B4：Packed Mode 的采样倍率 ----
    // 非 packed 时倍率恒为 1（与既有行为逐字节一致）；packed 时一个 DTO
    // 承载 S 个连续采样，净荷容量按 S 倍计算。规范明确 DPM_SAMPLE_COUNT 的
    // 0 与 1 非法，这里在装箱前就拒绝，避免产出必然被对端拒绝的规划。
    const bool packed = (m_packed_mode_ != DaqPackedMode::NotPacked);
    if (packed && (m_packed_sample_count_ < kMinDaqPackedSampleCount ||
                   m_packed_sample_count_ > kMaxDaqPackedSampleCount)) {
        return detail::MakeMeasurementError<MeasurementPlan>(
            MeasurementErrorCode::DaqConfigurationError,
            "DAQ Packed Mode 的 DPM_SAMPLE_COUNT 必须在 [2, 0xFFFF] 内，实际 " +
                std::to_string(m_packed_sample_count_));
    }
    const std::size_t sample_multiplier =
        packed ? static_cast<std::size_t>(m_packed_sample_count_) : 1U;
    // ---- 第一步：逐个查库，取得布局；任一失败即整体失败（不产出半成品） ----
    struct Row {
        std::string name;
        MeasurementSymbolInfo info;
        std::size_t byte_width;  // 实占字节 = 元素宽度 × 元素数（字节口径）
        std::size_t ag_units;   // 以 AG 为单位的 Entry.size
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
        const MeasurementSymbolInfo& info = found.Value();
        if (info.element_size_bytes == 0U || info.element_count == 0U ||
            (!info.is_array && info.element_count != 1U)) {
            return detail::MakeMeasurementError<MeasurementPlan>(
                MeasurementErrorCode::UnsupportedDataType,
                "测量 '" + name + "' 元素宽度/形状非法（element_size_bytes=" +
                    std::to_string(info.element_size_bytes) +
                    " element_count=" + std::to_string(info.element_count) +
                    " is_array=" + (info.is_array ? "true" : "false") + "）",
                name);
        }
        if (info.is_array &&
            !m_daq_capabilities_.supports_contiguous_1d_arrays) {
            return detail::MakeMeasurementError<MeasurementPlan>(
                MeasurementErrorCode::DaqConfigurationError,
                "当前 DAQ runtime profile 不允许数组 Entry",
                name);
        }
        if (info.is_array &&
            (m_daq_capabilities_.entry_granularity_bytes == 0U ||
             m_daq_capabilities_.max_entry_size_elements == 0U)) {
            return detail::MakeMeasurementError<MeasurementPlan>(
                MeasurementErrorCode::DaqConfigurationError,
                "数组 Entry 缺少已验证的粒度或最大长度",
                name);
        }
        const std::size_t element_size = info.element_size_bytes;
        const std::size_t element_count = info.element_count;
        if (element_count >
            std::numeric_limits<std::size_t>::max() / element_size) {
            return detail::MakeMeasurementError<MeasurementPlan>(
                MeasurementErrorCode::InvalidLayout,
                "测量 '" + name + "' 数组总字节数乘法溢出",
                name);
        }
        const std::size_t byte_width = element_size * element_count;
        // E2/F-12 单位修正：MAX_ODT_ENTRY_SIZE_x 的单位是 **AG**，不是字节。
        // 规范（原始 PDF 第 15 页 §4.1.2）：
        //   "The MAX_ODT_ENTRY_SIZE_x parameters indicate the upper limits for
        //    the size of the element described by an ODT entry in
        //    ADDRESS_GRANULARITY."
        //   SizeOf(element described by ODT entry)[AG] <= MAX_ODT_ENTRY_SIZE_x[AG]
        // 因此此处必须先把字节宽度换算为 AG 元素数再比对。AG=1（BYTE）时
        // 换算为恒等，与修正前行为逐位一致。
        if (byte_width % m_ag_bytes_ != 0U) {
            return detail::MakeMeasurementError<MeasurementPlan>(
                MeasurementErrorCode::InvalidLayout,
                "测量 '" + name + "' 实占 " + std::to_string(byte_width) +
                    " 字节无法按 AG(" + std::to_string(m_ag_bytes_) +
                    " 字节) 换算为整数元素数，不能与 MAX_ODT_ENTRY_SIZE 比较",
                name);
        }
        const std::size_t element_width_ag = byte_width / m_ag_bytes_;
        if (m_daq_capabilities_.max_entry_size_elements != 0U &&
            element_width_ag > m_daq_capabilities_.max_entry_size_elements) {
            return detail::MakeMeasurementError<MeasurementPlan>(
                MeasurementErrorCode::DaqConfigurationError,
                "测量 '" + name + "' 单 Entry 占 " +
                    std::to_string(element_width_ag) + " 个 AG 元素，超过 Slave " +
                    "声明的 MAX_ODT_ENTRY_SIZE " +
                    std::to_string(m_daq_capabilities_.max_entry_size_elements),
                name);
        }
        // 粒度判据按规范同样是 AG 单位换算后再取模：
        //   SizeOf(...)[AG] mod (GRANULARITY_x[BYTE] / AG[BYTE]) = 0
        if (m_daq_capabilities_.entry_granularity_bytes != 0U &&
            byte_width % m_daq_capabilities_.entry_granularity_bytes != 0U) {
            return detail::MakeMeasurementError<MeasurementPlan>(
                MeasurementErrorCode::InvalidLayout,
                "测量 '" + name + "' 单 Entry 字节数不能被 DAQ 粒度整除",
                name);
        }
        // DaqEntrySpec.size 以当前 XCP 地址粒度为单位。
        if (byte_width % m_ag_bytes_ != 0U) {
            return detail::MakeMeasurementError<MeasurementPlan>(
                MeasurementErrorCode::InvalidLayout,
                "测量 '" + name + "' 实占 " + std::to_string(byte_width) +
                    " 字节不能被 AG(" + std::to_string(m_ag_bytes_) +
                    "字节) 整除，无法映射为 DaqEntrySpec.size",
                name);
        }
        const std::size_t ag_units = byte_width / m_ag_bytes_;
        if (ag_units > 255U) {
            return detail::MakeMeasurementError<MeasurementPlan>(
                MeasurementErrorCode::DaqConfigurationError,
                "测量 '" + name + "' 的 Entry 超过 DaqEntrySpec.size 8 位上限",
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
    const std::size_t odt_id_bytes = m_identification_bytes_;
    if (odt_id_bytes == 0U || odt_id_bytes > max_dto_bytes) {
        return detail::MakeMeasurementError<MeasurementPlan>(
            MeasurementErrorCode::DaqConfigurationError,
            "识别字段头宽非法或超过 MAX_DTO: " +
                std::to_string(odt_id_bytes));
    }
    if (m_timestamp_bytes_ > max_dto_bytes - odt_id_bytes) {
        return detail::MakeMeasurementError<MeasurementPlan>(
            MeasurementErrorCode::DaqConfigurationError,
            "识别字段与时间戳头超出 MAX_DTO");
    }
    // 计数器段：只有识别字段模式不把 CTR 并入 FILL 时才额外占 1 字节；
    // 漏算这一字节会让规划刚好撑满 MAX_DTO，实际 DTO 超长被 Slave 拒绝（B2）。
    const std::size_t envelope_fixed = m_timestamp_bytes_ + odt_id_bytes;
    if (m_counter_header_bytes_ > max_dto_bytes - envelope_fixed) {
        return detail::MakeMeasurementError<MeasurementPlan>(
            MeasurementErrorCode::DaqConfigurationError,
            "识别字段、时间戳与计数器头超出 MAX_DTO");
    }
    const std::size_t usable =
        max_dto_bytes - envelope_fixed - m_counter_header_bytes_;

    // ---- 单变量跨 ODT / 超容量检验 ----
    for (const auto& row : rows) {
        if (row.byte_width * sample_multiplier > usable) {
            return detail::MakeMeasurementError<MeasurementPlan>(
                MeasurementErrorCode::DaqConfigurationError,
                "测量 '" + row.name + "' 需 " + std::to_string(row.byte_width) +
                    " 字节净荷（packed 倍率 " +
                    std::to_string(sample_multiplier) + "），超过单 ODT 可用容量 " +
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
        std::size_t odt_used{0};        // 当前 ODT 已占净荷字节（含 S 倍）
        std::size_t odt_single_used{0}; // 当前 ODT 单采样累计宽度
        std::vector<std::size_t> odt_route_indices;  // 当前 ODT 的路由下标
    };
    Active cur;
    bool has_active = false;
    std::uint16_t daq_list_counter = 0;

    // 关闭当前 ODT：把 packed 采样步长回填到本 ODT 的所有路由。
    // element-grouped 的步长是各 Entry 自身宽度；event-grouped 的步长是
    // 整个 ODT 的单采样总宽度（同 ODT 内按采样组连续排列）。非 packed 一律 0。
    //
    // element-grouped 还必须修正**基址**：装箱阶段 payload_offset 记录的是
    // 单采样内偏移，而 element-grouped 的物理布局是「Entry e 的全部 S 个采样
    // 连续排布，再排 Entry e+1」，故 Entry e 的背包基址应为
    // `Σ_{i<e} w_i × S`，即单采样偏移乘以 S。漏乘会让第 2 个及以后的 Entry
    // 从**前一个 Entry 的后续采样**中取数，产生静默错位且不报错。
    // event-grouped 的布局是「采样 k 的全部 Entry 连续」，单采样偏移配
    // `k × ODT 单采样宽`的步长已正确，基址不得再乘 S。
    const auto close_odt = [&](Active& active) {
        if (active.odt_route_indices.empty()) {
            return;
        }
        for (const std::size_t index : active.odt_route_indices) {
            MeasurementRoute& route = plan.routes[index];
            if (!packed) {
                route.packed_stride = 0;
            } else if (m_packed_mode_ == DaqPackedMode::ElementGrouped) {
                route.packed_stride = route.size;
                route.payload_offset *= sample_multiplier;
            } else {
                route.packed_stride = active.odt_single_used;
            }
        }
        active.odt_route_indices.clear();
    };

    // 组内顺序即 grouped 顺序；同组连续，跨组即新 List。
    for (const auto& row : grouped) {
        const bool start_new_list =
            (!has_active) || (cur.event_channel != row.info.event_channel);

        if (start_new_list) {
            if (has_active) {
                close_odt(cur);
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
            cur.spec.dto_counter = m_dto_counter_;
            cur.spec.timestamp = (m_timestamp_bytes_ > 0);
            cur.spec.pid_off = false;
            // 首 ODT
            cur.spec.odts.emplace_back();
            cur.odt_used = 0;
            cur.odt_single_used = 0;
            cur.odt_route_indices.clear();
            has_active = true;
        }

        // 当前 ODT 装不下 → 开新 ODT（同组内新 ODT 仍属当前 List）。
        // packed 时一帧要放下 S 个采样，故按 S 倍宽度判定。
        if (row.byte_width * sample_multiplier > usable - cur.odt_used) {
            if (cur.spec.odts.size() >= 256U) {
                return detail::MakeMeasurementError<MeasurementPlan>(
                    MeasurementErrorCode::DaqConfigurationError,
                    "DAQ List ODT 数量超过 8 位 ODT 编号范围(0..255)",
                    row.name);
            }
            close_odt(cur);
            cur.spec.odts.emplace_back();
            cur.odt_used = 0;
            cur.odt_single_used = 0;
        }
        DaqOdtSpec& odt = cur.spec.odts.back();
        // 偏移是**单采样内**的偏移：packed 时第 k 个采样的同一测量位于
        // payload_offset + k * packed_stride。
        const std::size_t payload_offset = cur.odt_single_used;
        // DaqEntrySpec.size 以 AG 为单位（xcp_master 预检按 Σ size*ag_bytes 计）
        odt.entries.emplace_back(DaqEntrySpec{
            row.info.address, row.info.extension,
            static_cast<std::uint8_t>(row.ag_units), kDaqBitOffsetNone});
        cur.odt_used += row.byte_width * sample_multiplier;
        cur.odt_single_used += row.byte_width;
        const std::uint8_t odt_number =
            static_cast<std::uint8_t>(cur.spec.odts.size() - 1);
        plan.routes.push_back(MeasurementRoute{
            row.name, cur.spec.daq_list, odt_number, payload_offset,
            row.byte_width, row.info.is_array, row.info.element_count,
            row.info.element_size_bytes, 0});
        cur.odt_route_indices.push_back(plan.routes.size() - 1);
    }

    if (has_active) {
        close_odt(cur);
        plan.daq_lists.push_back(std::move(cur.spec));
    }

    // routes 与 daq_lists 的 (daq_list, odt, 净荷偏移) 一一对应（由装箱逻辑保证）
    return MeasurementResult<MeasurementPlan>(std::move(plan));
}

}  // namespace calmcar::xcp