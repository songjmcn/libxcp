#pragma once

#include <cstddef>
#include <string>

#include "libxcp/a2l/a2l_types.hpp"
#include "libxcp/protocol_types.hpp"
#include "symbol_policy.hpp"

namespace calmcar::xcp::tools {

/**
 * @brief 校验标量访问所需的 A2L/XCP 字节布局条件。
 * @param symbol 待访问的 A2L 标量符号。
 * @param address_granularity 已协商的 XCP 地址粒度。
 * @param ecu_byte_order 已协商的 ECU 字节序。
 * @return 不满足时返回拒绝原因，满足时返回空字符串。
 */
inline std::string ScalarLayoutUnsupportedReason(
    const a2l::SymbolInfo& symbol, std::size_t address_granularity,
    ByteOrder ecu_byte_order) {
    const auto policy_reason = SymbolReadUnsupportedReason(symbol);
    if (!policy_reason.empty()) return policy_reason;
    if (address_granularity == 0U ||
        symbol.element_size_bytes % address_granularity != 0U) {
        return "地址粒度与符号宽度不兼容";
    }
    const auto expected_order = ecu_byte_order == ByteOrder::Intel
                                    ? a2l::ByteOrder::MsbLast
                                    : a2l::ByteOrder::MsbFirst;
    if (symbol.byte_order != expected_order) {
        return "A2L 与 Slave 字节序不一致";
    }
    return {};
}

/**
 * @brief 校验标定编码结果与符号宽度、地址粒度完全匹配。
 * @param symbol 待写入的 A2L 标量符号。
 * @param encoded_size A2L 编码产生的字节数。
 * @param address_granularity 已协商的 XCP 地址粒度。
 * @return 不满足时返回拒绝原因，满足时返回空字符串。
 */
inline std::string EncodedScalarUnsupportedReason(
    const a2l::SymbolInfo& symbol, std::size_t encoded_size,
    std::size_t address_granularity) {
    if (address_granularity == 0U ||
        encoded_size != symbol.element_size_bytes ||
        encoded_size % address_granularity != 0U) {
        return "编码宽度或地址粒度不兼容";
    }
    return {};
}

}  // namespace calmcar::xcp::tools
