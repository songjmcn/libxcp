// =============================================================================
// a2l_types.cpp —— B-1 地址计算与元素计数（设计文档 §6.1-A）
// =============================================================================

#include "libxcp/a2l/a2l_types.hpp"

namespace calmcar::xcp::a2l {

namespace {

/// @brief 构造地址阶段的结构化错误
Error AddressError(ErrorCode code, const SymbolInfo& symbol,
                   std::string message) {
    Error e;
    e.code = code;
    e.phase = Phase::Address;
    e.severity = Severity::Error;
    e.module = symbol.module_name;
    e.symbol = symbol.name;
    e.message = std::move(message);
    return e;
}

}  // namespace

Result<std::uint64_t> ComputeElementAddress(const SymbolInfo& symbol,
                                            std::uint64_t element_index,
                                            AddressGranularity ag) noexcept {
    const std::uint64_t ag_bytes = AgToBytes(ag);
    if (ag_bytes == 0) {
        return AddressError(ErrorCode::BadArgument, symbol,
                            "地址粒度为 0，非法 AG 枚举值");
    }
    if (symbol.element_size_bytes == 0) {
        // B-3：未知/无固定宽度类型禁止参与地址步进（不得猜默认宽度）
        return AddressError(ErrorCode::UnsupportedDataType, symbol,
                            "符号元素宽度未知，无法按索引推进地址");
    }

    // byte_offset = element_index * element_size_bytes（乘法溢出检测）
    const std::uint64_t elem = symbol.element_size_bytes;
    if (element_index > UINT64_MAX / elem) {
        return AddressError(ErrorCode::AddressOverflow, symbol,
                            "元素偏移量乘法溢出（禁止发包）");
    }
    const std::uint64_t byte_offset = element_index * elem;

    // 前置条件：byte_offset % AG == 0（B-1）
    if (byte_offset % ag_bytes != 0) {
        return AddressError(ErrorCode::AddressOverflow, symbol,
                            "元素偏移未按 ADDRESS_GRANULARITY 对齐");
    }

    // element_address = xcp_address + byte_offset / AG（加法溢出检测；
    // AG 绝不乘入基地址，extension 独立编码不参与进位）
    const std::uint64_t addr_step = byte_offset / ag_bytes;
    if (addr_step > UINT64_MAX - symbol.xcp_address) {
        return AddressError(ErrorCode::AddressOverflow, symbol,
                            "元素地址加法溢出（禁止发包）");
    }
    return symbol.xcp_address + addr_step;
}

Result<std::uint64_t> CountElements(const SymbolInfo& symbol) noexcept {
    std::uint64_t total = 1;
    std::uint64_t low_dim_product = 1;  // 已累乘的低维元素数
    for (const Dimension& dim : symbol.dimensions) {
        if (dim.extent == 0) {
            Error e = detail::MakeError(ErrorCode::InvalidLayout, Phase::Query,
                                        "维度 extent 为 0，布局不可证明");
            e.module = symbol.module_name;
            e.symbol = symbol.name;
            return e;
        }
        if (total > UINT64_MAX / dim.extent) {
            Error e = detail::MakeError(ErrorCode::InvalidLayout, Phase::Query,
                                        "维度乘积溢出，拒绝扁平化猜测（B-2）");
            e.module = symbol.module_name;
            e.symbol = symbol.name;
            return e;
        }
        total *= dim.extent;

        // 规则性检查（B-2/B-8）：stride 必须等于低维乘积 × 元素宽；
        // 首维的期望 stride 即元素宽本身。SDK 侧同样执行该检查，这里是
        // bridge 的第二道防线（禁止不规则布局被静默展平）。
        if (symbol.element_size_bytes != 0 && dim.byte_stride != 0) {
            const std::uint64_t expected =
                low_dim_product * symbol.element_size_bytes;
            if (dim.byte_stride != expected) {
                Error e = detail::MakeError(
                    ErrorCode::InvalidLayout, Phase::Query,
                    "维度字节跨度与规则连续布局不符，拒绝猜测");
                e.module = symbol.module_name;
                e.symbol = symbol.name;
                return e;
            }
        }
        low_dim_product *= dim.extent;
    }
    return total;
}

}  // namespace calmcar::xcp::a2l
