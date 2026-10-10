#ifndef LIBXCP_TOOLS_XCP_MASTER_SYMBOL_POLICY_HPP_
#define LIBXCP_TOOLS_XCP_MASTER_SYMBOL_POLICY_HPP_

#include <cstdint>
#include <limits>
#include <string>

#include "libxcp/a2l/a2l_types.hpp"

namespace calmcar::xcp::tools {

/// @brief 判断标量符号是否满足本工具的保守观测资格。
[[nodiscard]] inline std::string SymbolReadUnsupportedReason(
    const a2l::SymbolInfo& symbol) {
    if (symbol.xcp_address > std::numeric_limits<std::uint32_t>::max()) return "地址超过 32 位";
    if (!symbol.dimensions.empty()) return "数组/非标量暂不支持";
    if (symbol.element_size_bytes == 0) return "数据类型无固定宽度";
    if (symbol.daq_bit_offset.has_value() || symbol.bit_mask != 0) return "位字段暂不支持";
    if (symbol.kind == a2l::SymbolKind::Structure) return "结构体暂不支持";
    if (symbol.kind == a2l::SymbolKind::Characteristic &&
        symbol.characteristic_type != a2l::CharacteristicType::Value) {
        return "仅支持 MEASUREMENT 与 CHARACTERISTIC/VALUE 标量观测";
    }
    return {};
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

}  // namespace calmcar::xcp::tools

#endif  // LIBXCP_TOOLS_XCP_MASTER_SYMBOL_POLICY_HPP_
