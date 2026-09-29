/**
 * @file daq_timestamp.hpp
 * @brief DAQ 时间戳换算：原始计数 → 单调纳秒，含回卷延展（v0.6）。
 *
 * 对应测量子系统代码增长计划 §5.4（v0.6 硬化）。信封解码器（v0.2）只给出
 * `raw_timestamp`（原始计数，不解释）；本文件把它换算成单调递增的
 * `std::chrono::nanoseconds`：
 *  - 位宽（1/2/4 字节）来自 GET_DAQ_RESOLUTION_INFO 取证（不猜，B-3）；
 *  - 时间单位（tick→ns）来自运行时取证；**码表本仓库无权威来源（R13）**，
 *    因此单位以 `unit_ns` 显式注入，0 = 未知 → valid=false 但保留 raw；
 *  - 回卷判定：raw 相对 prev_raw 递减（无符号差值越界）→ 按位宽模 2^bits
 *    连续延展，并计数 timestamp_wraps。
 */

#ifndef CALMCAR_XCP_DAQ_DAQ_TIMESTAMP_HPP_
#define CALMCAR_XCP_DAQ_DAQ_TIMESTAMP_HPP_

#include <chrono>
#include <cstdint>

namespace calmcar::xcp {

/**
 * @brief 换算后的时间戳
 */
struct DaqTimestamp {
    std::uint64_t raw = 0;              ///< 原始计数（取自信封 raw_timestamp）
    std::chrono::nanoseconds value{0};  ///< 换算后的单调纳秒（含回卷延展）
    bool valid = false;                 ///< 无时间戳位宽或单位未知时为 false
};

/**
 * @brief 原始计数 → 纳秒转换器（无状态换算 + 有状态回卷延展）
 *
 * @details 用法：每个 DAQ List（每条时间戳流）持有一个独立实例——
 *          不同事件通道的计数器互相独立，混用一个实例会把两路 raw 的差值
 *          误判为回卷。Convert 依调用次序推进内部基线，非线程安全，
 *          只在会话 worker 线程内使用。
 */
class DaqTimestampConverter {
public:
    /**
     * @brief 构造转换器
     * @param unit_ns 每个 tick 的纳秒数（来自运行时取证；0=未知，输出 valid=false）
     * @param bits 原始计数位宽（8/16/32；其它值视为未知 → valid=false）
     */
    DaqTimestampConverter(std::uint64_t unit_ns, std::uint8_t bits) noexcept;

    /**
     * @brief 换算一个原始计数为单调纳秒
     * @param raw 本帧原始计数
     * @return 换算结果；首次调用建立基线；raw < prev_raw 判为回卷一次，
     *         内部周期基线 += 2^bits 连续延展（不猜跳变帧数，单帧至多 +1 周期）。
     */
    [[nodiscard]] DaqTimestamp Convert(std::uint64_t raw) noexcept;

    /// @brief 复位基线（Stop→Start 重新计流时调用）
    void Reset() noexcept;

    /// @brief 累计回卷次数（供会话统计 timestamp_wraps）
    [[nodiscard]] std::uint64_t WrapCount() const noexcept;

private:
    std::uint64_t m_unit_ns_{0};    ///< tick→ns（0=未知）
    std::uint64_t m_modulo_{0};     ///< 2^bits（非法位宽=0 → valid=false）
    std::uint64_t m_cycles_{0};     ///< 已观察到的回卷周期数（本实例累计）
    std::uint64_t m_total_wraps_{0};///< 历史累计回卷次数（Reset 不清零）
    std::uint64_t m_prev_raw_{0};   ///< 上一帧原始计数
    bool m_has_prev_{false};        ///< 是否已建立基线
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_DAQ_DAQ_TIMESTAMP_HPP_
