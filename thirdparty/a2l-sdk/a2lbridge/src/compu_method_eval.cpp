// =============================================================================
// compu_method_eval.cpp —— COMPU_METHOD 求值器（B-8/B-14/B-15）
//
// 纯函数实现，无 SDK/上游依赖：
//   raw bytes --(类型+字节序解码)--> 原始数值 --(bit_mask)--> i --(换算)--> p
//   p --(逆换算，唯一可逆且在范围内才放行)--> i --(编码)--> raw bytes
// 明确不允许的静默降级：未知类型猜宽度、FORM 猜公式、表外取默认、
// 逆解多值任取其一、越界截断。
// =============================================================================

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include "libxcp/a2l/compu_method.hpp"

namespace calmcar::xcp::a2l {
namespace {

/// @brief 构造换算阶段错误
Error ConvError(ErrorCode code, const SymbolInfo& symbol, std::string message) {
    Error e;
    e.code = code;
    e.phase = Phase::Conversion;
    e.severity = Severity::Error;
    e.module = symbol.module_name;
    e.symbol = symbol.name;
    e.message = std::move(message);
    return e;
}

/**
 * @brief 判断 BIT_MASK 是否为连续位掩码（首版约束，§4.1）
 * @param mask 待检掩码
 * @param shift 输出最低置位位偏移
 * @param width 输出连续位数
 * @return false 表示存在空洞（非连续），调用方必须拒绝而非猜测
 */
bool AnalyzeContiguousMask(std::uint64_t mask, unsigned* shift,
                           unsigned* width) noexcept {
    if (mask == 0) {
        *shift = 0;
        *width = 64;  // 全 0 视为未标注：按整字处理
        return true;
    }
    const unsigned low = static_cast<unsigned>(std::countr_zero(mask));
    const std::uint64_t shrunk = mask >> low;
    if ((shrunk & (shrunk + 1)) != 0) {
        return false;  // 存在空洞 → 非连续
    }
    *shift = low;
    *width = static_cast<unsigned>(64 - std::countl_zero(shrunk));
    return true;
}

/// @brief 按符号声明的字节序把 raw 读成小端 64-bit 字（仅取 element_size 内）
std::uint64_t ReadRawLittleEndian(BytesView raw, ByteOrder order) noexcept {
    std::uint8_t tmp[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    if (order == ByteOrder::MsbLast) {
        // INTEL：低字节在前
        for (std::size_t i = 0; i < raw.size(); ++i) {
            tmp[i] = raw[i];
        }
    } else {
        // MOTOROLA：高字节在前，镜像到小端缓冲
        for (std::size_t i = 0; i < raw.size(); ++i) {
            tmp[raw.size() - 1 - i] = raw[i];
        }
    }
    std::uint64_t out = 0;
    std::memcpy(&out, tmp, sizeof(out));
    return out;
}

/// @brief 把小端 64-bit 字按符号字节序写回 raw
Bytes WriteRawLittleEndian(std::uint64_t value, std::uint8_t size_bytes,
                           ByteOrder order) {
    std::uint8_t tmp[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(tmp, &value, sizeof(value));
    Bytes out(size_bytes);
    if (order == ByteOrder::MsbLast) {
        for (std::uint8_t i = 0; i < size_bytes; ++i) {
            out[i] = tmp[i];
        }
    } else {
        for (std::uint8_t i = 0; i < size_bytes; ++i) {
            out[i] = tmp[size_bytes - 1 - i];
        }
    }
    return out;
}

/// @brief 从 PhysicalValue 提取 double（整数分支无损提升；文本/布尔为 false）
bool AsDouble(const PhysicalValue& v, double* out) noexcept {
    if (const auto* p = std::get_if<double>(&v)) {
        *out = *p;
        return true;
    }
    if (const auto* p = std::get_if<std::int64_t>(&v)) {
        *out = static_cast<double>(*p);
        return true;
    }
    if (const auto* p = std::get_if<std::uint64_t>(&v)) {
        *out = static_cast<double>(*p);
        return true;
    }
    return false;
}

/**
 * @brief 原始位模式 → 数值 i（含符号扩展与浮点重组）
 * @param bits 已按小端归一化的位模式（高位补 0）
 * @param size_bytes 元素宽
 * @param type 显式 ASAM 类型（B-3：调用方保证有固定宽度）
 */
Result<double> BitsToNumeric(std::uint64_t bits, std::uint8_t size_bytes,
                             AsamDataType type, const SymbolInfo& symbol) {
    using E = AsamDataType;
    // 符号扩展辅助：按元素宽把无符号位模式转成有符号值
    auto sign_extend = [size_bytes](std::uint64_t v) -> std::int64_t {
        const unsigned total_bits = static_cast<unsigned>(size_bytes) * 8u;
        const unsigned sign_bit = total_bits - 1u;
        const std::uint64_t sign = std::uint64_t{1} << sign_bit;
        if (v & sign) {
            // 负数：高位填充 1 后按 int64 解释
            const std::uint64_t extended = v | ~(sign - 1u);
            return static_cast<std::int64_t>(extended);
        }
        return static_cast<std::int64_t>(v);
    };

    switch (type) {
        case E::UByte:
        case E::UWord:
        case E::ULong:
        case E::ULong64:
            return static_cast<double>(bits);
        case E::SByte:
        case E::SWord:
        case E::SLong:
        case E::SLong64:
            return static_cast<double>(sign_extend(bits));
        case E::Float16: {
            // IEEE754 half：查表法展开为 float（避免平台依赖）
            const std::uint16_t h = static_cast<std::uint16_t>(bits);
            const std::uint32_t sign = static_cast<std::uint32_t>(h >> 15)
                                       << 31;
            const std::uint32_t exp = (h >> 10) & 0x1Fu;
            const std::uint32_t frac = h & 0x3FFu;
            std::uint32_t f_bits = 0;
            if (exp == 0) {
                if (frac == 0) {
                    f_bits = sign;  // ±0
                } else {
                    // 次正规：规格化
                    std::uint32_t e2 = 0;
                    std::uint32_t mantissa = frac;
                    while ((mantissa & 0x400u) == 0) {
                        mantissa <<= 1;
                        ++e2;
                    }
                    mantissa &= 0x3FFu;
                    const std::uint32_t exponent = 127u - 15u - e2;
                    f_bits = sign | (exponent << 23) | (mantissa << 13);
                }
            } else if (exp == 0x1Fu) {
                f_bits = sign | 0x7F800000u | (frac << 13);  // Inf/NaN
            } else {
                const std::uint32_t exponent = exp - 15u + 127u;
                f_bits = sign | (exponent << 23) | (frac << 13);
            }
            float f = 0.0f;
            std::memcpy(&f, &f_bits, sizeof(f));
            return static_cast<double>(f);
        }
        case E::Float32: {
            std::uint32_t u32 = static_cast<std::uint32_t>(bits);
            float f = 0.0f;
            std::memcpy(&f, &u32, sizeof(f));
            return static_cast<double>(f);
        }
        case E::Float64: {
            double d = 0.0;
            std::memcpy(&d, &bits, sizeof(d));
            return d;
        }
        default:
            return ConvError(ErrorCode::UnsupportedDataType, symbol,
                             "该 ASAM 类型无固定宽度，禁止数值读写（B-3）");
    }
}

/**
 * @brief 数值 → 原始位模式（FromPhysical 用；越界即拒，B-15）
 */
Result<std::uint64_t> NumericToBits(double value, std::uint8_t size_bytes,
                                    AsamDataType type,
                                    const SymbolInfo& symbol) {
    using E = AsamDataType;
    if (!std::isfinite(value)) {
        return ConvError(ErrorCode::BadArgument, symbol,
                         "物理值为 NaN/Inf，拒绝编码（B-15）");
    }
    const unsigned total_bits = static_cast<unsigned>(size_bytes) * 8u;
    const double max_unsigned =
        total_bits >= 64 ? std::ldexp(1.0, 64) : std::ldexp(1.0, total_bits);

    switch (type) {
        case E::UByte:
        case E::UWord:
        case E::ULong:
        case E::ULong64: {
            if (value < 0.0 || value >= max_unsigned ||
                value != std::trunc(value)) {
                return ConvError(ErrorCode::ConversionOutOfRange, symbol,
                                 "逆算结果不在无符号类型范围或非整数（B-15）");
            }
            return static_cast<std::uint64_t>(value);
        }
        case E::SByte:
        case E::SWord:
        case E::SLong:
        case E::SLong64: {
            const double lo =
                -std::ldexp(1.0, static_cast<int>(total_bits - 1));
            const double hi = std::ldexp(1.0, static_cast<int>(total_bits - 1));
            if (value < lo || value >= hi || value != std::trunc(value)) {
                return ConvError(ErrorCode::ConversionOutOfRange, symbol,
                                 "逆算结果不在有符号类型范围或非整数（B-15）");
            }
            const std::int64_t sv = static_cast<std::int64_t>(value);
            return static_cast<std::uint64_t>(sv) &
                   (total_bits >= 64 ? UINT64_MAX
                                     : (std::uint64_t{1} << total_bits) - 1);
        }
        case E::Float16: {
            // 就近舍入转 half（RTNE，手动实现，避免平台依赖）
            float f = static_cast<float>(value);
            std::uint32_t b = 0;
            std::memcpy(&b, &f, sizeof(b));
            const std::uint32_t sign = (b >> 16) & 0x8000u;
            std::int32_t exp =
                static_cast<std::int32_t>((b >> 23) & 0xFFu) - 127 + 15;
            std::uint32_t frac = b & 0x7FFFFFu;
            if (exp <= 0) {
                if (exp < -10) {
                    return std::uint64_t{sign};  // 下溢为 ±0
                }
                // 次正规 half
                frac |= 0x800000u;
                const std::uint32_t shift =
                    static_cast<std::uint32_t>(14 - exp);  // exp<=0 → shift>=14
                const std::uint32_t rounded =
                    (frac + (std::uint32_t{1} << (shift - 1))) >> shift;
                return std::uint64_t{sign | rounded};
            }
            if (exp >= 31) {
                return std::uint64_t{sign | 0x7C00u |
                                     (frac ? 0x200u : 0u)};  // Inf / NaN 保留
            }
            const std::uint32_t rounded_frac =
                (frac + 0x1000u) >> 13;  // RTNE 进位可能溢出到指数
            std::uint32_t h_exp = static_cast<std::uint32_t>(exp);
            std::uint32_t h_frac = rounded_frac & 0x3FFu;
            if (rounded_frac > 0x3FFu) {
                ++h_exp;
                if (h_exp >= 31) {
                    return std::uint64_t{sign | 0x7C00u};
                }
            }
            return std::uint64_t{sign | (h_exp << 10) | h_frac};
        }
        case E::Float32: {
            const float f = static_cast<float>(value);
            std::uint32_t u32 = 0;
            std::memcpy(&u32, &f, sizeof(u32));
            return std::uint64_t{u32};
        }
        case E::Float64: {
            std::uint64_t u64 = 0;
            std::memcpy(&u64, &value, sizeof(u64));
            return u64;
        }
        default:
            return ConvError(ErrorCode::UnsupportedDataType, symbol,
                             "该 ASAM 类型无固定宽度，禁止数值写入（B-3）");
    }
}

/// @brief 查表正算：TAB_INTP 插值 / TAB_NOINTP 阶梯（表外拒绝，B-15）
Result<PhysicalValue> TableForward(const SymbolInfo& symbol, double i,
                                   bool interpolate) {
    const auto* table = std::get_if<std::vector<ConversionTableEntry>>(
        &symbol.conversion.payload);
    if (table == nullptr || table->empty()) {
        return ConvError(ErrorCode::InvalidLayout, symbol,
                         "转换表为空，无法执行查表换算");
    }
    if (i < table->front().input_min || i > table->back().input_max) {
        return ConvError(ErrorCode::ConversionOutOfRange, symbol,
                         "原始值在表范围之外，无默认值可用（B-15）");
    }
    for (std::size_t k = 0; k + 1 < table->size(); ++k) {
        const ConversionTableEntry& lo = (*table)[k];
        const ConversionTableEntry& hi = (*table)[k + 1];
        const bool in_span = interpolate
                                 ? (i >= lo.input_min && i <= hi.input_max)
                                 : (i >= lo.input_min && i < hi.input_min);
        if (!in_span) {
            continue;
        }
        if (!lo.numeric_output) {
            // TAB_VERB 文本输出：直接取区间起点的文本
            return lo.output;
        }
        const double p_lo = std::get<double>(lo.output);
        if (!interpolate) {
            return PhysicalValue{p_lo};
        }
        const double span = hi.input_min - lo.input_min;
        if (span <= 0.0) {
            return PhysicalValue{p_lo};  // 退化区间（单点）：不插值
        }
        const double p_hi = std::get<double>(hi.output);
        return PhysicalValue{p_lo +
                             (p_hi - p_lo) * ((i - lo.input_min) / span)};
    }
    // 命中最后一个端点
    const ConversionTableEntry& last = table->back();
    if (!last.numeric_output) {
        return last.output;
    }
    return last.output;
}

}  // namespace

Result<PhysicalValue> ConvertForward(const SymbolInfo& symbol,
                                     double raw_value) {
    const ConversionInfo& conv = symbol.conversion;
    switch (conv.kind) {
        case ConversionKind::None:
        case ConversionKind::Identical:
            // IDENTICAL：保持整数精度语义（B-14）
            if (raw_value == std::trunc(raw_value) &&
                raw_value >= -9.2233720368547758e18 &&
                raw_value <= 9.2233720368547758e18) {
                return PhysicalValue{static_cast<std::int64_t>(raw_value)};
            }
            return PhysicalValue{raw_value};
        case ConversionKind::Linear: {
            const auto& c = std::get<LinearCoefficients>(conv.payload);
            return PhysicalValue{c.f + raw_value * c.c + raw_value * c.o};
        }
        case ConversionKind::RatFunc: {
            const auto& r = std::get<RatFuncCoefficients>(conv.payload);
            const double denom =
                r.d1 * raw_value * raw_value + r.d2 * raw_value + r.d3;
            if (denom == 0.0) {
                return ConvError(ErrorCode::ConversionNotInvertible, symbol,
                                 "RAT_FUNC 分母为零（B-8）");
            }
            const double numer =
                r.n1 * raw_value * raw_value + r.n2 * raw_value + r.n3;
            return PhysicalValue{numer / denom};
        }
        case ConversionKind::TabIntp:
            return TableForward(symbol, raw_value, /*interpolate=*/true);
        case ConversionKind::TabNoIntp:
        case ConversionKind::TabVerb:
            return TableForward(symbol, raw_value, /*interpolate=*/false);
        case ConversionKind::FormulaUnsupported:
            return ConvError(ErrorCode::UnsupportedConversion, symbol,
                             "FORM 公式仅保留原文，不做数值换算（B-8）");
    }
    return ConvError(ErrorCode::Internal, symbol, "未知的 ConversionKind");
}

Result<double> ConvertInverse(const SymbolInfo& symbol,
                              const PhysicalValue& phys_value) {
    const ConversionInfo& conv = symbol.conversion;
    double p = 0.0;
    if (!AsDouble(phys_value, &p)) {
        return ConvError(ErrorCode::ConversionNotInvertible, symbol,
                         "文本/布尔物理值没有数值逆映射（B-14）");
    }
    if (!std::isfinite(p)) {
        return ConvError(ErrorCode::BadArgument, symbol,
                         "物理值为 NaN/Inf（B-15）");
    }
    switch (conv.kind) {
        case ConversionKind::None:
        case ConversionKind::Identical:
            return p;
        case ConversionKind::Linear: {
            const auto& c = std::get<LinearCoefficients>(conv.payload);
            const double slope = c.c + c.o;  // p = f + i*(C+O)
            if (slope == 0.0) {
                return ConvError(ErrorCode::ConversionNotInvertible, symbol,
                                 "LINEAR 斜率为零，逆解不唯一（B-15）");
            }
            return (p - c.f) / slope;
        }
        case ConversionKind::RatFunc: {
            const auto& r = std::get<RatFuncCoefficients>(conv.payload);
            // p*(D1 i² + D2 i + D3) = N1 i² + N2 i + N3
            // → (p*D1 - N1) i² + (p*D2 - N2) i + (p*D3 - N3) = 0
            const double a = p * r.d1 - r.n1;
            const double b = p * r.d2 - r.n2;
            const double cc = p * r.d3 - r.n3;
            if (a == 0.0) {
                if (b == 0.0) {
                    return ConvError(
                        ErrorCode::ConversionNotInvertible, symbol,
                        "RAT_FUNC 退化为常数方程，逆解不存在或不唯一");
                }
                return -cc / b;  // 一次方程唯一解
            }
            const double disc = b * b - 4.0 * a * cc;
            if (disc < 0.0) {
                return ConvError(ErrorCode::ConversionNotInvertible, symbol,
                                 "RAT_FUNC 逆解为复数（无实数原像）");
            }
            const double root = (-b + std::sqrt(disc)) / (2.0 * a);
            const double other = (-b - std::sqrt(disc)) / (2.0 * a);
            if (root != other) {
                // 二次两实根：默认严格策略拒绝任取其一（B-15），
                // 除非两根重合（重根时唯一）。
                return ConvError(ErrorCode::ConversionNotInvertible, symbol,
                                 "RAT_FUNC 逆解多值，拒绝猜测（B-15）");
            }
            return root;
        }
        case ConversionKind::TabIntp: {
            const auto* table =
                std::get_if<std::vector<ConversionTableEntry>>(&conv.payload);
            if (table == nullptr || table->empty()) {
                return ConvError(ErrorCode::InvalidLayout, symbol,
                                 "转换表为空");
            }
            // 单调性检查：非单调表逆解不唯一 → 拒绝（B-15）
            for (std::size_t k = 1; k < table->size(); ++k) {
                const double y0 = std::get<double>((*table)[k - 1].output);
                const double y1 = std::get<double>((*table)[k].output);
                if (y1 == y0) {
                    return ConvError(ErrorCode::ConversionNotInvertible, symbol,
                                     "TAB_INTP 相邻输出相等，逆解不唯一");
                }
            }
            const bool ascending = std::get<double>(table->front().output) <
                                   std::get<double>(table->back().output);
            for (std::size_t k = 0; k + 1 < table->size(); ++k) {
                const ConversionTableEntry& lo = (*table)[k];
                const ConversionTableEntry& hi = (*table)[k + 1];
                const double y0 = std::get<double>(lo.output);
                const double y1 = std::get<double>(hi.output);
                const bool inside =
                    ascending ? (p >= y0 && p <= y1) : (p <= y0 && p >= y1);
                if (!inside) {
                    continue;
                }
                if (y1 == y0) {
                    return lo.input_min;
                }
                return lo.input_min +
                       (hi.input_min - lo.input_min) * ((p - y0) / (y1 - y0));
            }
            return ConvError(ErrorCode::ConversionOutOfRange, symbol,
                             "物理值在表范围之外（B-15）");
        }
        case ConversionKind::TabNoIntp:
        case ConversionKind::TabVerb: {
            const auto* table =
                std::get_if<std::vector<ConversionTableEntry>>(&conv.payload);
            if (table == nullptr || table->empty()) {
                return ConvError(ErrorCode::InvalidLayout, symbol,
                                 "转换表为空");
            }
            int hits = 0;
            double hit_input = 0.0;
            for (const ConversionTableEntry& e : *table) {
                const bool match = e.numeric_output
                                       ? (std::get<double>(e.output) == p)
                                       : false;
                if (match) {
                    ++hits;
                    hit_input = e.input_min;
                }
            }
            if (hits == 0) {
                return ConvError(ErrorCode::ConversionOutOfRange, symbol,
                                 "物理值不在任何表项中（B-15）");
            }
            if (hits > 1) {
                return ConvError(ErrorCode::ConversionNotInvertible, symbol,
                                 "多个表项输出相同，逆解不唯一（B-15）");
            }
            return hit_input;
        }
        case ConversionKind::FormulaUnsupported:
            return ConvError(ErrorCode::UnsupportedConversion, symbol,
                             "FORM 公式不可执行，逆换算同样拒绝（B-8）");
    }
    return ConvError(ErrorCode::Internal, symbol, "未知的 ConversionKind");
}

Result<PhysicalValue> EvaluateToPhysical(const SymbolInfo& symbol,
                                         BytesView raw) {
    if (symbol.element_size_bytes == 0) {
        return ConvError(ErrorCode::UnsupportedDataType, symbol,
                         "符号无固定元素宽度，禁止数值读取（B-3）");
    }
    if (raw.size() != symbol.element_size_bytes) {
        return ConvError(ErrorCode::RawSizeMismatch, symbol,
                         "raw 字节长度与符号元素宽度不符");
    }
    const std::uint64_t bits = ReadRawLittleEndian(raw, symbol.byte_order);

    // BIT_MASK 仅接受连续掩码（§4.1）。位域提取后的解释规则：
    //   - 全字掩码（或未标注）：按符号声明类型完整解码（含符号扩展/浮点重组）；
    //   - 真子集掩码：位域本身按无符号整数解释——A2L 未定义子字段的
    //     有符号性，禁止猜测（B-3），换算输入取该无符号值。
    unsigned mask_shift = 0;
    unsigned mask_width = 0;
    if (symbol.bit_mask != 0) {
        if (!AnalyzeContiguousMask(symbol.bit_mask, &mask_shift, &mask_width)) {
            return ConvError(ErrorCode::InvalidLayout, symbol,
                             "BIT_MASK 非连续，首版不支持（§4.1）");
        }
    }
    const bool full_word_mask =
        symbol.bit_mask == 0 || mask_width >= symbol.element_size_bytes * 8u;

    Result<double> numeric = [&]() -> Result<double> {
        if (full_word_mask) {
            return BitsToNumeric(bits, symbol.element_size_bytes,
                                 symbol.data_type, symbol);
        }
        // 子字段按无符号解释（禁止猜测子字段有符号性，B-3）
        const std::uint64_t field = (bits & symbol.bit_mask) >> mask_shift;
        return static_cast<double>(field);
    }();
    if (!numeric.HasValue()) {
        return numeric.TakeError();
    }
    return ConvertForward(symbol, numeric.Value());
}

Result<Bytes> EvaluateFromPhysical(const SymbolInfo& symbol,
                                   const PhysicalValue& value) {
    if (symbol.element_size_bytes == 0) {
        return ConvError(ErrorCode::UnsupportedDataType, symbol,
                         "符号无固定元素宽度，禁止数值写入（B-3）");
    }
    if (!symbol.read_write) {
        return ConvError(ErrorCode::UnsupportedOperation, symbol,
                         "A2L 声明该符号只读");
    }
    // 位掩码须先校验连续性（正逆共用同一约束，B-9/§4.1）
    unsigned mask_shift = 0;
    unsigned mask_width = 0;
    if (symbol.bit_mask != 0) {
        if (!AnalyzeContiguousMask(symbol.bit_mask, &mask_shift, &mask_width)) {
            return ConvError(ErrorCode::InvalidLayout, symbol,
                             "BIT_MASK 非连续，首版不支持（§4.1）");
        }
    }

    // TAB_VERB 文本逆映射：ConvertInverse 对文本输入直接报错（B-14），
    // 但 TAB_VERB 的字符串输出命中表项时可按阶梯表匹配——先尝试通用逆算。
    Result<double> inverse = ConvertInverse(symbol, value);
    if (!inverse.HasValue()) {
        return inverse.TakeError();
    }
    const double i = inverse.Value();

    // 原始值必须是整数；负值仅允许出现在有符号类型
    if (i != std::trunc(i)) {
        return ConvError(ErrorCode::ConversionOutOfRange, symbol,
                         "逆算原始值非整数（B-15）");
    }
    const bool signed_type = symbol.data_type == AsamDataType::SByte ||
                             symbol.data_type == AsamDataType::SWord ||
                             symbol.data_type == AsamDataType::SLong ||
                             symbol.data_type == AsamDataType::SLong64;
    if (i < 0.0 && !signed_type) {
        return ConvError(ErrorCode::ConversionOutOfRange, symbol,
                         "逆算原始值为负数，超出无符号域（B-15）");
    }

    // 符号级限值检查（have_limit 时执行；COMPU_METHOD 限值由换算层负责）
    if (symbol.have_limit) {
        double p = 0.0;
        if (AsDouble(value, &p)) {
            if (p < symbol.lower_limit || p > symbol.upper_limit) {
                return ConvError(ErrorCode::ConversionOutOfRange, symbol,
                                 "物理值超出 A2L 符号限值（B-15）");
            }
        }
    }

    std::uint64_t masked_bits = 0;
    if (symbol.bit_mask != 0 && mask_width < symbol.element_size_bytes * 8u) {
        // 子字段：按无符号窄类型编码后移位回填到掩码位置（与正向解释对称）
        const AsamDataType narrow = mask_width <= 8    ? AsamDataType::UByte
                                    : mask_width <= 16 ? AsamDataType::UWord
                                    : mask_width <= 32 ? AsamDataType::ULong
                                                       : AsamDataType::ULong64;
        Result<std::uint64_t> encoded = NumericToBits(
            i, static_cast<std::uint8_t>((mask_width + 7) / 8), narrow, symbol);
        if (!encoded.HasValue()) {
            return encoded.TakeError();
        }
        if (mask_width < 64 &&
            encoded.Value() > ((std::uint64_t{1} << mask_width) - 1)) {
            return ConvError(ErrorCode::ConversionOutOfRange, symbol,
                             "逆算结果超出 BIT_MASK 位域容量（B-15）");
        }
        masked_bits = encoded.Value() << mask_shift;
    } else {
        Result<std::uint64_t> encoded = NumericToBits(
            i, symbol.element_size_bytes, symbol.data_type, symbol);
        if (!encoded.HasValue()) {
            return encoded.TakeError();
        }
        masked_bits = encoded.Value();
    }
    return WriteRawLittleEndian(masked_bits, symbol.element_size_bytes,
                                symbol.byte_order);
}

}  // namespace calmcar::xcp::a2l
