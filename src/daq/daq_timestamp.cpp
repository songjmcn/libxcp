/**
 * @file daq_timestamp.cpp
 * @brief 实现 DaqTimestampConverter：原始计数 → 单调纳秒 + 回卷延展（v0.6）。
 *
 * 语义（计划 §5.4 / v0.6 实现硬核对项）：
 *  - 位宽 8/16/32 合法（对应 1/2/4 字节时间戳，GET_DAQ_RESOLUTION_INFO 取证），
 *    其它值 = 未知 → 不猜（B-3），valid=false 且保留 raw；
 *  - 单位 unit_ns=0 = 未知（R13：单位码表本仓库无权威来源）→ valid=false；
 *  - 回卷判定：raw < prev_raw 判为跨过一次 2^bits 边界，周期基线 +1 连续延展，
 *    单帧至多 +1 周期（不猜丢失的帧数），同时累计 WrapCount()。
 */

#include "libxcp/daq/daq_timestamp.hpp"

#include <cstdint>
#include <limits>

namespace calmcar::xcp {

namespace {

/// @brief 位宽 → 模 2^bits；非 8/16/32 返回 0 表示未知（不猜）
[[nodiscard]] constexpr std::uint64_t ModuloOf(std::uint8_t bits) noexcept {
    switch (bits) {
        case 8U:
            return 1ULL << 8U;
        case 16U:
            return 1ULL << 16U;
        case 32U:
            return 1ULL << 32U;
        default:
            return 0ULL;
    }
}

/// @brief extended×unit 的饱和乘法：结果越出 int64 范围时钳到最大值（防 UB）
[[nodiscard]] std::int64_t SaturateNs(std::uint64_t extended,
                                      std::uint64_t unit_ns) noexcept {
    constexpr std::uint64_t kInt64Max =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (unit_ns != 0ULL && extended > kInt64Max / unit_ns) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(extended * unit_ns);
}

}  // namespace

DaqTimestampConverter::DaqTimestampConverter(std::uint64_t unit_ns,
                                             std::uint8_t bits) noexcept
    : m_unit_ns_(unit_ns), m_modulo_(ModuloOf(bits)) {}

DaqTimestamp DaqTimestampConverter::Convert(std::uint64_t raw) noexcept {
    DaqTimestamp out;
    out.raw = raw;
    if (m_modulo_ == 0ULL) {
        // 位宽未知：不解释、不猜测（B-3），保留 raw，valid=false
        return out;
    }
    // 回卷判定：相对上帧倒退即判为跨过一次 2^bits 边界（单帧至多 +1 周期）
    if (m_has_prev_ && raw < m_prev_raw_) {
        ++m_cycles_;
        ++m_total_wraps_;
    }
    m_prev_raw_ = raw;
    m_has_prev_ = true;

    const std::uint64_t extended = m_cycles_ * m_modulo_ + raw;
    if (m_unit_ns_ == 0ULL) {
        // 单位未知：不猜（R13 无权威码表），保留 raw，valid=false
        return out;
    }
    out.value = std::chrono::nanoseconds(SaturateNs(extended, m_unit_ns_));
    out.valid = true;
    return out;
}

void DaqTimestampConverter::Reset() noexcept {
    // 复位基线与周期，但历史累计回卷数不清零（统计语义跨 Stop→Start 连续）
    m_cycles_ = 0ULL;
    m_prev_raw_ = 0ULL;
    m_has_prev_ = false;
}

std::uint64_t DaqTimestampConverter::WrapCount() const noexcept {
    return m_total_wraps_;
}

}  // namespace calmcar::xcp
