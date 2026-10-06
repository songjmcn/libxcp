// =============================================================================
// compu_method.hpp —— COMPU_METHOD 求值接口（设计文档 §4 / B-8/B-14/B-15）
//
// 求值器实现在 src/compu_method_eval.cpp：纯函数、无 SDK/上游依赖，
// 便于单测与复用（IA2lDatabase::ToPhysical 与 IDaqLayout::Decode 共用）。
// =============================================================================

#ifndef LIBXCP_A2L_COMPU_METHOD_HPP_
#define LIBXCP_A2L_COMPU_METHOD_HPP_

#include "libxcp/a2l/a2l_result.hpp"
#include "libxcp/a2l/a2l_types.hpp"

namespace calmcar::xcp::a2l {

/**
 * @brief 原始字节 → tagged 物理值（B-8/B-14）
 *
 * 步骤：按 symbol.data_type + symbol.byte_order 解码整数/浮点原始值 →
 * 应用 bit_mask（仅连续掩码；非连续返回 InvalidLayout）→ 套用 conversion。
 *
 * @param symbol 符号快照（提供类型、字节序、掩码与换算）
 * @param raw 实际字节；长度必须等于 element_size_bytes（RawSizeMismatch）
 * @return 物理值；FORM 公式返回 UnsupportedConversion（B-8 不猜公式）
 */
[[nodiscard]] Result<PhysicalValue> EvaluateToPhysical(const SymbolInfo& symbol,
                                                       BytesView raw);

/**
 * @brief tagged 物理值 → 原始字节（逆换算，B-15 严格模式）
 *
 * 仅当映射唯一可逆且结果在范围内时成功；分母为零、表外无默认、逆解多值、
 * NaN/Inf、越界一律结构化错误，禁止静默截断。
 *
 * @param symbol 符号快照
 * @param value 物理值
 * @return element_size_bytes 长度的原始字节（按 symbol.byte_order 编码）
 */
[[nodiscard]] Result<Bytes> EvaluateFromPhysical(const SymbolInfo& symbol,
                                                 const PhysicalValue& value);

/**
 * @brief 数值域正算（供数组元素级解码复用）
 * @param symbol 符号快照
 * @param raw_value 已解码的原始数值（整数或浮点统一为 double/int64 语义）
 * @return 物理值
 */
[[nodiscard]] Result<PhysicalValue> ConvertForward(const SymbolInfo& symbol,
                                                   double raw_value);

/**
 * @brief 数值域逆算（B-15：越界/不可逆即错）
 * @param symbol 符号快照
 * @param phys_value 物理值（须为数值分支）
 * @return 原始数值
 */
[[nodiscard]] Result<double> ConvertInverse(const SymbolInfo& symbol,
                                            const PhysicalValue& phys_value);

}  // namespace calmcar::xcp::a2l

#endif  // LIBXCP_A2L_COMPU_METHOD_HPP_
