#ifndef LIBXCP_TOOLS_XCP_MASTER_SYMBOL_POLICY_HPP_
#define LIBXCP_TOOLS_XCP_MASTER_SYMBOL_POLICY_HPP_

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "libxcp/a2l/a2l_types.hpp"
#include "libxcp/protocol_types.hpp"

namespace calmcar::xcp::tools {

/// @brief 判断标量符号是否满足本工具的保守观测资格。
[[nodiscard]] inline std::string SymbolReadUnsupportedReason(
    const a2l::SymbolInfo& symbol) {
    if (symbol.xcp_address > std::numeric_limits<std::uint32_t>::max())
        return "地址超过 32 位";
    if (!symbol.dimensions.empty()) return "数组/非标量暂不支持";
    if (symbol.element_size_bytes == 0) return "数据类型无固定宽度";
    if (symbol.daq_bit_offset.has_value() || symbol.bit_mask != 0)
        return "位字段暂不支持";
    if (symbol.kind == a2l::SymbolKind::Structure) return "结构体暂不支持";
    if (symbol.kind == a2l::SymbolKind::Characteristic &&
        symbol.characteristic_type != a2l::CharacteristicType::Value) {
        return "仅支持 MEASUREMENT 与 CHARACTERISTIC/VALUE 标量观测";
    }
    return {};
}

[[nodiscard]] inline std::string SymbolDetailReadUnsupportedReason(
    const a2l::SymbolInfo& symbol);

/// @brief 根据A2L静态声明判断变量是否能加入DAQ观测集合。
/// @param symbol 待观测符号。
/// @param available_events A2L声明的EVENT_CHANNEL编号集合。
/// @param allow_contiguous_1d_array 是否允许定长连续一维测量数组。
/// @return 空串表示静态合格；否则为明确的中文拒绝原因。
/// @details 数组准入已不再按厂商 profile 门控（原 `--daq-profile` 已移除）：
///          本函数只做保守的静态上限检查，真实的 Entry 容量核对
///          （MAX_ODT_ENTRY_SIZE_DAQ 与元素长度）由 watch 运行期用取证到的
///          从站能力完成，故此处无需也无法得知从站布局。
[[nodiscard]] inline std::string SymbolDaqObservationUnsupportedReason(
    const a2l::SymbolInfo& symbol,
    std::span<const std::uint16_t> available_events,
    bool allow_contiguous_1d_array = false) {
    if (symbol.kind != a2l::SymbolKind::Measurement)
        return "DAQ 仅支持 MEASUREMENT";
    if (!symbol.dimensions.empty()) {
        if (!allow_contiguous_1d_array) return "暂不支持 DAQ 数组";
        const auto array_reason =
            SymbolDetailReadUnsupportedReason(symbol);
        if (!array_reason.empty()) return array_reason;
        // 静态限制须与核心 uint8 元素计数及单 Entry 字节字段一致；
        // 从站实际粒度、最大 Entry 和 DTO 容量留待 watch 运行期再核实。
        const auto& dimension = symbol.dimensions.front();
        if (dimension.extent > 255U ||
            dimension.extent > 255U / symbol.element_size_bytes)
            return "DAQ 数组元素数或总字节数超过单 Entry 静态上限";
    } else {
        const auto read_reason = SymbolReadUnsupportedReason(symbol);
        if (!read_reason.empty()) return read_reason;
    }

    // 与启动DAQ时一致：同一固定事件号重复声明只计作一个事件。
    auto fixed = symbol.daq_event_fixed;
    std::sort(fixed.begin(), fixed.end());
    fixed.erase(std::unique(fixed.begin(), fixed.end()), fixed.end());
    if (fixed.size() != 1U)
        return "需要唯一 FIXED_EVENT_LIST；不回退到 DEFAULT/AVAILABLE。";
    if (std::find(available_events.begin(), available_events.end(),
                  fixed.front()) == available_events.end())
        return "A2L 缺少对应 EVENT_CHANNEL 元数据。";
    return {};
}

/// @brief 判断详情弹窗是否能安全读取标量、单维测量数组或固定轴曲线。
[[nodiscard]] inline std::string SymbolDetailReadUnsupportedReason(
    const a2l::SymbolInfo& symbol) {
    if (symbol.xcp_address > std::numeric_limits<std::uint32_t>::max())
        return "地址超过 32 位";
    if (symbol.element_size_bytes == 0) return "数据类型无固定宽度";
    if (symbol.daq_bit_offset.has_value() || symbol.bit_mask != 0)
        return "位字段暂不支持";
    if (symbol.kind == a2l::SymbolKind::Structure) return "结构体暂不支持";

    if (symbol.kind == a2l::SymbolKind::Measurement) {
        if (symbol.dimensions.empty())
            return SymbolReadUnsupportedReason(symbol);
        if (symbol.dimensions.size() != 1U) return "仅支持单维测量数组分页读取";
        const auto& dimension = symbol.dimensions.front();
        if (dimension.extent == 0U || dimension.extent > 4096U ||
            dimension.byte_stride != symbol.element_size_bytes ||
            dimension.byte_stride == 0U ||
            dimension.extent > (static_cast<std::uint64_t>(
                                    std::numeric_limits<std::uint32_t>::max()) -
                                symbol.xcp_address + 1U) /
                                   dimension.byte_stride)
            return "数组维度、地址范围或元素步长未通过边界校验";
        return {};
    }

    if (symbol.kind == a2l::SymbolKind::Characteristic &&
        symbol.characteristic_type == a2l::CharacteristicType::Value)
        return SymbolReadUnsupportedReason(symbol);

    if (symbol.kind == a2l::SymbolKind::Characteristic &&
        symbol.characteristic_type == a2l::CharacteristicType::Curve &&
        symbol.fixed_curve_axis.has_value() && symbol.dimensions.size() == 1U) {
        const auto& dimension = symbol.dimensions.front();
        const auto& axis = *symbol.fixed_curve_axis;
        if (dimension.extent == 0U || dimension.extent > 4096U ||
            dimension.byte_stride != symbol.element_size_bytes ||
            dimension.byte_stride == 0U ||
            dimension.extent > (static_cast<std::uint64_t>(
                                    std::numeric_limits<std::uint32_t>::max()) -
                                symbol.xcp_address + 1U) /
                                   dimension.byte_stride ||
            axis.point_count != dimension.extent)
            return "固定轴曲线的点数或元素步长不一致";
        return {};
    }
    return "仅支持 MEASUREMENT、CHARACTERISTIC/VALUE 标量及已验证固定轴 CURVE";
}

/// @brief 判断标量标定符号是否满足本工具的保守写入资格。
[[nodiscard]] inline std::string SymbolWriteUnsupportedReason(
    const a2l::SymbolInfo& symbol) {
    const auto read_reason = SymbolReadUnsupportedReason(symbol);
    if (!read_reason.empty()) return read_reason;
    if (symbol.kind != a2l::SymbolKind::Characteristic ||
        symbol.characteristic_type != a2l::CharacteristicType::Value) {
        return "仅支持 CHARACTERISTIC/VALUE 写入";
    }
    if (!symbol.read_write) return "A2L 未标记为可写";
    return {};
}

/// @brief 判断固定轴曲线的单点写入资格；整条曲线始终不可整体写入。
[[nodiscard]] inline std::string CurvePointWriteUnsupportedReason(
    const a2l::SymbolInfo& symbol, std::uint64_t element_index) {
    const auto read_reason = SymbolDetailReadUnsupportedReason(symbol);
    if (!read_reason.empty()) return read_reason;
    if (symbol.kind != a2l::SymbolKind::Characteristic ||
        symbol.characteristic_type != a2l::CharacteristicType::Curve ||
        !symbol.fixed_curve_axis.has_value())
        return "仅支持已验证固定轴 CURVE 的单点写入";
    if (!symbol.read_write) return "A2L INSTANCE 未标记为可写";
    if (element_index >= symbol.fixed_curve_axis->point_count)
        return "曲线点索引超出固定轴范围";
    return {};
}

/// @brief 只根据A2L静态信息检查标定写入候选，不读取ECU状态。
[[nodiscard]] inline std::string SymbolStaticWriteUnsupportedReason(
    const a2l::SymbolInfo& symbol) {
    std::string reason;
    if (symbol.kind == a2l::SymbolKind::Characteristic &&
        symbol.characteristic_type == a2l::CharacteristicType::Curve) {
        reason = SymbolDetailReadUnsupportedReason(symbol);
        if (!reason.empty()) return reason;
        if (!symbol.read_write) return "A2L INSTANCE 未标记为可写";
        if (symbol.fixed_curve_axis->point_count == 0U)
            return "固定轴曲线没有可写点";
    } else {
        reason = SymbolWriteUnsupportedReason(symbol);
        if (!reason.empty()) return reason;
    }

    if (!symbol.memory_segment.has_value() ||
        !symbol.memory_segment->has_xcp_segment)
        return "无法唯一关联有效的XCP MEMORY_SEGMENT/PAG";
    const auto& pages = symbol.memory_segment->pages;
    if (std::none_of(pages.begin(), pages.end(), [](const auto& page) {
            return page.xcp_write_access == a2l::XcpPageWriteAccess::DontCare;
        }))
        return "A2L未声明允许无条件XCP写入的PAGE";
    return {};
}

/// @brief 检查A2L静态页声明与XCP实时页属性是否都允许写入。
[[nodiscard]] inline std::string CalibrationPageWriteUnsupportedReason(
    const a2l::SymbolInfo& symbol, std::uint8_t active_page,
    PageAccessType runtime_write_access) {
    if (!symbol.memory_segment.has_value())
        return "符号未能唯一映射到 A2L MEMORY_SEGMENT";
    const auto& segment = *symbol.memory_segment;
    if (!segment.has_xcp_segment)
        return "MEMORY_SEGMENT 未声明有效 XCP SEGMENT";
    const auto page =
        std::find_if(segment.pages.begin(), segment.pages.end(),
                     [active_page](const a2l::MemoryPageInfo& item) {
                         return item.number == active_page;
                     });
    if (page == segment.pages.end()) return "活动页未在 A2L 中声明";
    if (page->xcp_write_access != a2l::XcpPageWriteAccess::DontCare)
        return "A2L PAGE 未声明无条件 XCP 写访问";
    if (runtime_write_access != PageAccessType::DontCare)
        return "Slave 当前 PAGE_PROPERTIES 不允许无条件 XCP 写访问";
    return {};
}

}  // namespace calmcar::xcp::tools

#endif  // LIBXCP_TOOLS_XCP_MASTER_SYMBOL_POLICY_HPP_
