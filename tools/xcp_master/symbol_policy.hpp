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

// ---------------------------------------------------------------------------
// DAQ 事件通道来源判定（规则顺序固定，纯函数、无 IO）
// ---------------------------------------------------------------------------

/// @brief DAQ 事件来源，顺序即规则优先级。
/// @details 规范依据（XCP 协议层 §4.1.5 EVENT CHANNELS）：
///          - `FIXED_EVENT_LIST`：工具**不得**改动分配；
///          - `DEFAULT_EVENT_LIST`：工具**可以**改成其它分配（本工具不改，但
///            按计划 §7.2 在界面上标注"未改动"）；
///          - `AVAILABLE_EVENT_LIST`：若该表不存在，则 ECU 提供的全部事件
///            通道都可被工具分配；
///          - `CONSISTENCY_EVENT_LIST`：MEASUREMENT 未声明 DEFAULT 时，
///            规范要求（shall）以它作为默认事件通道分配。
enum class DaqEventSource : std::uint8_t {
    None = 0,         ///< 未能确定（error 非空时恒为 None）
    Fixed = 1,        ///< 来自 FIXED_EVENT_LIST
    Default = 2,      ///< 来自 DEFAULT_EVENT_LIST
    Consistency = 3,  ///< 来自 CONSISTENCY_EVENT_LIST（无 DEFAULT 时的规范性兜底）
};

/// @brief DAQ 事件来源的稳定短名，用于界面与日志（不要本地化）。
[[nodiscard]] inline const char* DaqEventSourceName(DaqEventSource source) {
    switch (source) {
        case DaqEventSource::Fixed:
            return "fixed";
        case DaqEventSource::Default:
            return "default";
        case DaqEventSource::Consistency:
            return "consistency";
        case DaqEventSource::None:
            break;
    }
    return "none";
}

/// @brief 事件通道解析结果。
struct DaqEventCandidate {
    /// 采用的来源；`error` 非空时恒为 `None`。
    DaqEventSource source = DaqEventSource::None;
    /// 去重升序后的事件通道号；合格时恰 1 个，否则为空。
    std::vector<std::uint16_t> channels;
    /// 非空表示不可用，内容即面向用户的中文原因。
    std::string error;
};

/// @brief 事件号去重并升序，保持"重复声明只计一个"的既有口径。
[[nodiscard]] inline std::vector<std::uint16_t> NormalizeDaqEventNumbers(
    const std::vector<std::uint16_t>& events) {
    auto normalized = events;
    std::sort(normalized.begin(), normalized.end());
    normalized.erase(std::unique(normalized.begin(), normalized.end()),
                     normalized.end());
    return normalized;
}

/// @brief 把事件号列表渲染成 `0x1/0x2` 形式，用于错误原因。
[[nodiscard]] inline std::string FormatDaqEventNumbers(
    const std::vector<std::uint16_t>& events) {
    std::string text;
    for (const auto event : events) {
        if (!text.empty()) text += "/";
        text += "0x";
        constexpr char kHex[] = "0123456789ABCDEF";
        bool started = false;
        for (int shift = 12; shift >= 0; shift -= 4) {
            const auto digit = (event >> shift) & 0xFU;
            if (digit != 0U || started || shift == 0) {
                text += kHex[digit];
                started = true;
            }
        }
    }
    return text;
}

/// @brief 判定某个符号应采用哪个事件通道，以及依据来自哪张表。
/// @param symbol 已解析的 A2L 符号事实。
/// @return 合格时 `error` 为空且 `channels` 恰 1 个；否则 `error` 为拒绝原因。
/// @details 规则依次为：
///          0. **自洽性（无条件先校验）**：`AVAILABLE_EVENT_LIST` 非空时，
///             `DEFAULT`/`CONSISTENCY` 的每个事件号都必须出现在其中，否则拒绝
///             —— 依据规范 §4.1.5 的 must 级约束；基于自相矛盾的 A2L 做出的
///             任何事件选择都不可信。
///          1. `FIXED` 去重后恰 1 个 → 采用；
///          2. `FIXED` 多于 1 个 → 拒绝（规范禁止工具改动固定分配，工具无权
///             在多个固定事件里挑一个）；
///          3. `FIXED` 为空且 `DEFAULT` 恰 1 个 → 采用（来源 Default）；
///          4. `FIXED` 为空且 `DEFAULT` 多于 1 个 → 拒绝（多义，不猜）；
///          5. `FIXED`/`DEFAULT` 皆空且 `CONSISTENCY` 恰 1 个 → 采用（规范兜底）；
///          5b. `CONSISTENCY` 多于 1 个 → 拒绝；
///          5c. 三者皆空 → 拒绝（该 A2L 未提供任何可用的事件分配）。
///          注意规则 2 优先于规则 3：`FIXED` 多值时**不得**退到 `DEFAULT`。
[[nodiscard]] inline DaqEventCandidate ResolveDaqEventCandidate(
    const a2l::SymbolInfo& symbol) {
    const auto fixed = NormalizeDaqEventNumbers(symbol.daq_event_fixed);
    const auto defaults = NormalizeDaqEventNumbers(symbol.daq_event_default);
    const auto consistency =
        NormalizeDaqEventNumbers(symbol.daq_consistency_event);
    const auto available =
        NormalizeDaqEventNumbers(symbol.daq_event_available);

    DaqEventCandidate candidate;

    // 规则 0：AVAILABLE 非空时，DEFAULT/CONSISTENCY 必须是它的子集。
    if (!available.empty()) {
        for (const auto& [name, events] :
             {std::pair{"DEFAULT_EVENT_LIST", defaults},
              std::pair{"CONSISTENCY_EVENT_LIST", consistency}}) {
            for (const auto event : events) {
                if (std::find(available.begin(), available.end(), event) ==
                    available.end()) {
                    candidate.error =
                        std::string("A2L 事件声明自相矛盾：") + name + " 声明了 " +
                        FormatDaqEventNumbers({event}) +
                        "，但 AVAILABLE_EVENT_LIST 只列出 " +
                        FormatDaqEventNumbers(available) +
                        "；请修正 A2L 或改用 POLL 模式。";
                    return candidate;
                }
            }
        }
    }

    // 规则 1/2：FIXED 唯一则采用；多值拒绝（不得退到 DEFAULT）。
    if (!fixed.empty()) {
        if (fixed.size() != 1U) {
            candidate.error =
                "A2L 声明了多个 FIXED_EVENT_LIST 事件号（" +
                FormatDaqEventNumbers(fixed) +
                "）；规范禁止工具改动固定事件分配，工具不代为选择；"
                "请修正 A2L 或改用 POLL 模式。";
            return candidate;
        }
        candidate.source = DaqEventSource::Fixed;
        candidate.channels = fixed;
        return candidate;
    }

    // 规则 3/4：FIXED 为空时用 DEFAULT。
    if (!defaults.empty()) {
        if (defaults.size() != 1U) {
            candidate.error = "A2L 声明了多个 DEFAULT_EVENT_LIST 事件号（" +
                              FormatDaqEventNumbers(defaults) +
                              "），无法唯一确定应使用哪一个；"
                              "请修正 A2L 或改用 POLL 模式。";
            return candidate;
        }
        candidate.source = DaqEventSource::Default;
        candidate.channels = defaults;
        return candidate;
    }

    // 规则 5/5b：无 DEFAULT 时按规范用 CONSISTENCY 兜底。
    if (!consistency.empty()) {
        if (consistency.size() != 1U) {
            candidate.error = "A2L 声明了多个 CONSISTENCY_EVENT_LIST 事件号（" +
                              FormatDaqEventNumbers(consistency) +
                              "），无法唯一确定应使用哪一个；"
                              "请修正 A2L 或改用 POLL 模式。";
            return candidate;
        }
        candidate.source = DaqEventSource::Consistency;
        candidate.channels = consistency;
        return candidate;
    }

    // 规则 5c：三者皆空。
    candidate.error =
        "A2L 未声明 FIXED_EVENT_LIST、DEFAULT_EVENT_LIST 或 "
        "CONSISTENCY_EVENT_LIST（没有任何可用的 DAQ 事件分配）；"
        "请改用 POLL 模式。";
    return candidate;
}

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

    // 事件来源统一由 ResolveDaqEventCandidate 判定（fixed → default →
    // consistency，且先做 AVAILABLE 子集自洽性校验），避免两处规则漂移。
    const auto candidate = ResolveDaqEventCandidate(symbol);
    if (!candidate.error.empty()) return candidate.error;
    if (std::find(available_events.begin(), available_events.end(),
                  candidate.channels.front()) == available_events.end())
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

/// @brief A2L 声明的标定页命令能力（源自 PROTOCOL_LAYER 的 OPTIONAL_CMD）。
struct CalibrationPageFacts {
    bool declares_get_cal_page{false};   ///< A2L 是否声明 GET_CAL_PAGE
    bool declares_set_cal_page{false};   ///< A2L 是否声明 SET_CAL_PAGE
    bool declares_get_page_info{false};  ///< A2L 是否声明 GET_PAGE_INFO
};

/// @brief 判断命令码是否出现在 A2L OPTIONAL_CMD 声明中。
[[nodiscard]] inline bool A2lDeclaresCommand(
    const std::vector<std::uint8_t>& optional_commands, CommandCode code) {
    const auto value = static_cast<std::uint8_t>(code);
    return std::find(optional_commands.begin(), optional_commands.end(), value) !=
           optional_commands.end();
}

/// @brief 从 A2L OPTIONAL_CMD 命令码构造标定页能力事实。
[[nodiscard]] inline CalibrationPageFacts MakeCalibrationPageFacts(
    const std::vector<std::uint8_t>& optional_commands) {
    CalibrationPageFacts facts;
    facts.declares_get_cal_page =
        A2lDeclaresCommand(optional_commands, CommandCode::GetCalPage);
    facts.declares_set_cal_page =
        A2lDeclaresCommand(optional_commands, CommandCode::SetCalPage);
    facts.declares_get_page_info =
        A2lDeclaresCommand(optional_commands, CommandCode::GetPageInfo);
    return facts;
}

/// @brief 符号是否带可用的段页事实（MEMORY_SEGMENT 且声明了有效 XCP SEGMENT）。
[[nodiscard]] inline bool HasCalibrationPageFacts(const a2l::SymbolInfo& symbol) {
    return symbol.memory_segment.has_value() &&
           symbol.memory_segment->has_xcp_segment;
}

/// @brief 写入资格的界面附注：说明本轮判定依据（无附注时返回空串）。
[[nodiscard]] inline std::string CalibrationPageNote(
    const a2l::SymbolInfo& symbol, const CalibrationPageFacts& facts) {
    if (HasCalibrationPageFacts(symbol)) {
        if (facts.declares_get_page_info) return {};
        return "A2L 未声明 GET_PAGE_INFO：仅按活动页判定，页写权限未取证";
    }
    if (!facts.declares_get_cal_page)
        return "A2L 未声明 MEMORY_SEGMENT 与 GET_CAL_PAGE：按单页直写，未做页校验";
    return "A2L 未声明 MEMORY_SEGMENT：按 0 号段查询活动页，活动页 0 视为工作页";
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

    // P2：段页是可选能力。A2L 没有 MEMORY_SEGMENT 时不再拒绝写入，改为在运行时
    // 按活动页判定（见 CliApp::CheckCalibrationPageWriteLocked）；只有确实声明了
    // 段页事实时才要求存在"允许无条件 XCP 写"的 PAGE。
    if (!HasCalibrationPageFacts(symbol)) return {};
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
