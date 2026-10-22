/**
 * @file link_budget.hpp
 * @brief XCP on Ethernet 的链路带宽预算（MAX_BIT_RATE / MAX_BUS_LOAD）。
 *
 * @par 规范基线（TL V1.5 原始 PDF：
 *      `docs/ASAM-AE-MCD-1-XCP-AS-Ethernet-Transport-Layer-V1-5-0.pdf` §7）
 *   - §7.1（PDF 第 25-27 页）：MAX_BIT_RATE 与 MAX_BUS_LOAD 约束 DAQ/STIM
 *     的**净 XCP 数据**带宽。
 *   - 规范明确预算只覆盖 **XCP Header + Packet + Tail**，**不**计算
 *     Ethernet/IP/UDP 外层开销；Datagram 聚合不改变净 XCP 字节总数。
 *
 * @par 冻结设计（方案 §1.9A.3 第 6 项）
 *   本阶段采取「显式配置优先、确定性预算、A2L 自动提取 Unsupported」：
 *   不修改上游 `thirdparty/a2l-sdk/src`，由调用方提供 `LinkBudgetConfig`。
 *   只有未来确认上游公共 API 能可靠给出 MAX_BIT_RATE / MAX_BUS_LOAD 后，
 *   才另行启用 A2L 自动提取。
 */

#ifndef CALMCAR_XCP_LINK_BUDGET_HPP_
#define CALMCAR_XCP_LINK_BUDGET_HPP_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "libxcp/protocol_types.hpp"

namespace calmcar::xcp {

/**
 * @brief 未提供可用链路参数时的处置策略
 * @details 对应方案 §1.9A.3：未提供 `LinkBudgetConfig` 时固定采用
 *          `AllowUnverified` 语义，保持既有 DAQ 可用性；`Reject` 仅在调用方
 *          显式提供配置并选择它时生效。
 */
enum class UnknownLinkPolicy : std::uint8_t {
    /// @brief 允许运行但标记 Unverified 并发出诊断（默认）
    AllowUnverified = 0,

    /// @brief 拒绝启动 DAQ（要求调用方给出可验证的链路参数）
    Reject = 1,
};

/**
 * @brief 链路带宽预算配置（方案 §1.9A.3 冻结字段）
 *
 * @note 各字段单位严格对应规范：速率用 **bit/s**（不是 byte/s），
 *       负载用 **百分比**（不是比例），避免调用方按习惯误填。
 */
struct LinkBudgetConfig {
    /**
     * @brief 标称链路比特率（对应 MAX_BIT_RATE），单位 bit/s
     * @details 必须 > 0 才参与预算；为 0 表示未配置，按
     *          `unknown_link_policy` 处理。
     */
    std::uint64_t nominal_bit_rate_bps = 0;

    /**
     * @brief 允许的净 XCP 数据总线负载上限，单位百分比，取值区间 (0, 100]
     * @details 仅约束 DAQ/STIM 净数据。为 0 表示未配置。
     */
    double max_bus_load_percent = 0.0;

    /**
     * @brief 实际可用比特率（可选），单位 bit/s
     * @details 已知时必须 > 0。预算取
     *          `min(nominal, actual_available)`，用于表达"标称 1Gbit 但
     *          实测只有 12Mbit"这类降额场景。
     */
    std::optional<std::uint64_t> actual_available_bit_rate_bps;

    /// @brief 链路参数不可用时的处置策略
    UnknownLinkPolicy unknown_link_policy = UnknownLinkPolicy::AllowUnverified;

    /**
     * @brief 是否允许超预算继续（默认 false）
     * @details 置 true 时**只能**把结论降为 `Unverified` 并告警，
     *          **不得**标记为 `Verified`（方案 §1.9A.3 硬约束）。
     */
    bool allow_over_budget = false;
};

/// @brief 预算结论的验证等级
enum class BudgetVerdict : std::uint8_t {
    /// @brief 参数完整且未超预算，可证明安全
    Verified = 0,

    /// @brief 参数缺失或调用方显式允许超预算：可运行但不可证明
    Unverified = 1,

    /// @brief 已证超出允许带宽，须在 DAQ 启动前拒绝
    OverBudget = 2,
};

/**
 * @brief DAQ Event 的带宽预算输入（单个 Event 频道）
 *
 * @note `dto_packet_bytes` 是**各 ODT** 的 DTO 净长度（不含 4 字节 XCP
 *       Header 与 Tail）——Header/Tail 由预算器按 Alignment 补算，
 *       调用方不要预先加进去。
 */
struct EventBudgetInput {
    /// @brief Event 频道名（用于诊断输出）
    std::string event_name;

    /**
     * @brief Event 周期，单位秒
     * @details 必须 > 0。未知周期应通过 `std::nullopt` 表达，
     *          **不得**假造一个周期（方案 §1.9A.3）。
     */
    std::optional<double> cycle_seconds;

    /// @brief 该 Event 下各 ODT 的 DTO 净长度（每项对应一个 XCP Frame）
    std::vector<std::size_t> dto_packet_bytes;
};

/// @brief 单个 Event 的预算明细
struct EventBudgetResult {
    std::string event_name;                  ///< Event 频道名
    std::size_t frame_count = 0;             ///< ODT/Frame 数量
    std::size_t total_xcp_bytes = 0;         ///< 含 Header 与 Tail 的净 XCP 字节
    std::size_t total_tail_bytes = 0;        ///< 其中 Tail FILL 字节数
    double bytes_per_second = 0.0;           ///< 该 Event 每秒净 XCP 字节
    bool cycle_known = false;                ///< 周期是否已知
};

/// @brief 链路预算计算结果
struct LinkBudgetResult {
    /// @brief 总体结论
    BudgetVerdict verdict = BudgetVerdict::Unverified;

    /// @brief 是否可直接启动 DAQ（`OverBudget` 时为 false）
    bool allowed = true;

    /// @brief 各 Event 的明细
    std::vector<EventBudgetResult> events;

    /// @brief 所有 Event 合计的净 XCP 字节/秒
    double total_bytes_per_second = 0.0;

    /// @brief 允许的净 XCP 字节/秒（参数不足时为 0）
    double allowed_bytes_per_second = 0.0;

    /// @brief 生效比特率（bit/s，参数不足时为 0）
    std::uint64_t effective_bit_rate_bps = 0;

    /// @brief 人类可读的结论说明（含未验证/超预算原因）
    std::string message;
};

/**
 * @brief 单个 XCP Frame 的净字节数（Header + Packet + Tail）
 * @param dto_packet_bytes DTO 净长度（不含 Header 与 Tail）
 * @param alignment 会话协商到的 PACKET_ALIGNMENT
 * @return 含 4 字节 Header 与该 Alignment 所需 Tail 的总字节数
 *
 * @details 与 `TailBytesForAlignment()` 配套使用，保证预算与真实编码使用
 *          **同一个** Tail 公式，避免两处独立实现产生偏差。
 */
[[nodiscard]] std::size_t XcpFrameBytes(std::size_t dto_packet_bytes,
                                        PacketAlignment alignment) noexcept;

/**
 * @brief 计算链路带宽预算（不抛异常，结论通过返回值表达）
 * @param config 链路参数；未提供时按 `AllowUnverified` 语义处理
 * @param events 参与预算的 Event 列表
 * @param alignment 会话协商到的 PACKET_ALIGNMENT
 * @return 预算结论（含逐 Event 明细与人类可读说明）
 *
 * @details 预算公式（方案 §1.9A.3 冻结式）：
 * @code
 * alignment_bytes   = PACKET_ALIGNMENT / 8
 * xcp_frame_bytes   = 4 + dto_packet_bytes + tail_bytes
 * tail_bytes        = (alignment_bytes - (frame_without_tail mod
 * alignment_bytes)) mod alignment_bytes
 * event_bytes_per_second = (1 / cycle_seconds) * Σ(xcp_frame_bytes)
 * total_bytes_per_second = Σ(event_bytes_per_second)
 * effective_bit_rate_bps = min(nominal, actual_available if known)
 * allowed_bytes_per_second = effective_bit_rate_bps / 8
 *                            * max_bus_load_percent / 100
 * @endcode
 *
 * @par 判定规则
 *   - 任一 Event 周期未知，或链路参数缺失 → 至少 `Unverified`；
 *     若 `unknown_link_policy == Reject` 则 `allowed = false`；
 *   - `total > allowed` → 默认 `OverBudget` 且 `allowed = false`；
 *     调用方显式 `allow_over_budget = true` 时降为 `Unverified`（保持
 *     `allowed = true`）并给出告警文案，**不得**升为 `Verified`；
 *   - `alignment == Unknown` → 无法推算 Tail，结论至少 `Unverified`
 *     （方案 §1.9A.4：无证据不得假定对齐）。
 *
 * @note 所有比较使用 `double` 累加后再比较，不在中间步骤向下取整
 *       （方案 §1.9A.3 末条：不得通过取整掩盖超预算）。
 */
[[nodiscard]] LinkBudgetResult ComputeLinkBudget(
    const std::optional<LinkBudgetConfig>& config,
    const std::vector<EventBudgetInput>& events,
    PacketAlignment alignment) noexcept;

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_LINK_BUDGET_HPP_