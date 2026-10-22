#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "libxcp/a2l/a2l_types.hpp"
#include "libxcp/protocol_types.hpp"
#include "symbol_policy.hpp"

namespace calmcar::xcp::tools {

/** @brief 严格解析非负十进制0基元素索引，不接受空白、符号或溢出。 */
inline std::optional<std::uint64_t> ParseElementIndex(std::string_view text) {
    if (text.empty()) return std::nullopt;
    std::uint64_t index = 0;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), index);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        return std::nullopt;
    return index;
}

/** @brief 连续数组分页的0基起点与元素数量。 */
struct ElementPageRange {
    std::uint64_t first_index{0}; /**< 本页第一个元素的0基索引。 */
    std::uint64_t count{0};       /**< 本页有效元素数。 */
};

/**
 * @brief 验证分页起点并计算末页截断后的元素数量。
 * @param extent 数组元素总数。
 * @param page_start 请求页的0基起点，必须对齐page_size。
 * @param page_size 每页元素数。
 * @return 有效页范围；起点未对齐、越界或参数为零时返回空。
 */
inline std::optional<ElementPageRange> MakeElementPageRange(
    std::uint64_t extent, std::uint64_t page_start, std::uint64_t page_size) {
    if (extent == 0U || page_size == 0U || page_start >= extent ||
        page_start % page_size != 0U)
        return std::nullopt;
    return ElementPageRange{page_start,
                            std::min(page_size, extent - page_start)};
}

/**
 * @brief 计算下一页起点并钳位到最后一页，避免无符号加法溢出。
 * @param extent 数组元素总数。
 * @param page_start 当前页起点；必须按page_size对齐。
 * @param page_size 每页元素数。
 * @return 下一页起点；无效参数时返回空。
 */
inline std::optional<std::uint64_t> NextElementPageStart(
    std::uint64_t extent, std::uint64_t page_start, std::uint64_t page_size) {
    if (extent == 0U || page_size == 0U || page_start % page_size != 0U)
        return std::nullopt;
    const auto last_start = ((extent - 1U) / page_size) * page_size;
    if (page_start >= last_start) return last_start;
    return page_start + std::min(page_size, last_start - page_start);
}

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
 * @brief 校验数组/曲线元素范围、地址粒度和字节序。
 * @param symbol 已通过A2L详情能力检查的符号。
 * @param first_index 起始的0基元素索引。
 * @param count 连续读取/写入的元素数；曲线写入必须为1。
 * @param address_granularity 已协商的 XCP 地址粒度。
 * @param ecu_byte_order 已协商的 ECU 字节序。
 * @return 不满足时返回拒绝原因，满足时返回空字符串。
 */
inline std::string ElementRangeLayoutUnsupportedReason(
    const a2l::SymbolInfo& symbol, std::uint64_t first_index,
    std::uint64_t count, std::size_t address_granularity,
    ByteOrder ecu_byte_order) {
    const auto policy_reason = SymbolDetailReadUnsupportedReason(symbol);
    if (!policy_reason.empty()) return policy_reason;
    const std::uint64_t element_count =
        symbol.dimensions.empty() ? 1U : symbol.dimensions.front().extent;
    const std::uint64_t stride = symbol.dimensions.empty()
                                     ? symbol.element_size_bytes
                                     : symbol.dimensions.front().byte_stride;
    if (count == 0U || first_index >= element_count ||
        count > element_count - first_index)
        return "元素读取范围超出已证明维度";
    if (address_granularity == 0U || stride % address_granularity != 0U ||
        symbol.element_size_bytes % address_granularity != 0U)
        return "地址粒度与元素宽度不兼容";
    if (count > std::numeric_limits<std::uint32_t>::max() / stride)
        return "元素读取长度超出 XCP 字节计数";
    const std::uint64_t byte_offset = first_index * stride;
    const std::uint64_t byte_count = count * stride;
    const std::uint64_t max_address = std::numeric_limits<std::uint32_t>::max();
    if (byte_offset > max_address - symbol.xcp_address ||
        byte_count - 1U > max_address - symbol.xcp_address - byte_offset)
        return "元素地址范围超过 32 位";
    const auto expected_order = ecu_byte_order == ByteOrder::Intel
                                    ? a2l::ByteOrder::MsbLast
                                    : a2l::ByteOrder::MsbFirst;
    if (symbol.byte_order != expected_order) return "A2L 与 Slave 字节序不一致";
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
