/**
 * @file link_budget.cpp
 * @brief 链路带宽预算实现。公式与判定规则见 `include/libxcp/link_budget.hpp`。
 */

#include "libxcp/link_budget.hpp"

#include <cmath>
#include <sstream>

#include "libxcp/udp_header_codec.hpp"

namespace calmcar::xcp {

std::size_t XcpFrameBytes(std::size_t dto_packet_bytes,
                          PacketAlignment alignment) noexcept {
    // 与编解码共用同一个 Tail 公式，避免预算与真实编码产生偏差
    const std::size_t without_tail = kUdpHeaderSize + dto_packet_bytes;
    return without_tail +
           TailBytesForAlignment(alignment, without_tail);
}

LinkBudgetResult ComputeLinkBudget(
    const std::optional<LinkBudgetConfig>& config,
    const std::vector<EventBudgetInput>& events,
    PacketAlignment alignment) noexcept {
    LinkBudgetResult result;

    // ---- 1. 逐 Event 累计净 XCP 字节 ----
    bool any_cycle_unknown = false;
    for (const auto& event : events) {
        EventBudgetResult detail;
        detail.event_name = event.event_name;
        detail.frame_count = event.dto_packet_bytes.size();
        detail.cycle_known = event.cycle_seconds.has_value() &&
                             *event.cycle_seconds > 0.0;
        if (!detail.cycle_known) {
            any_cycle_unknown = true;
        }

        // 每个 ODT 是一个独立 XCP Frame：4 字节 Header + DTO + Tail
        std::size_t total = 0;
        std::size_t tails = 0;
        for (const std::size_t dto : event.dto_packet_bytes) {
            const std::size_t frame = XcpFrameBytes(dto, alignment);
            const std::size_t without_tail = kUdpHeaderSize + dto;
            tails += frame - without_tail;
            total += frame;
        }
        detail.total_xcp_bytes = total;
        detail.total_tail_bytes = tails;
        // 周期未知时不计算速率（不得假造周期），保持 0 并由 verdict 表达
        detail.bytes_per_second =
            detail.cycle_known ? static_cast<double>(total) / *event.cycle_seconds
                               : 0.0;

        result.total_bytes_per_second += detail.bytes_per_second;
        result.events.push_back(std::move(detail));
    }

    // ---- 2. 判定链路参数是否可用 ----
    const bool params_present =
        config.has_value() && config->nominal_bit_rate_bps > 0U &&
        config->max_bus_load_percent > 0.0;
    const UnknownLinkPolicy policy =
        config.has_value() ? config->unknown_link_policy
                           : UnknownLinkPolicy::AllowUnverified;
    const bool over_budget_allowed =
        config.has_value() && config->allow_over_budget;

    // alignment 未知则无法推算 Tail，预算不可证明
    const bool alignment_known = alignment != PacketAlignment::Unknown;

    std::ostringstream msg;

    if (!params_present) {
        // 未提供配置：按策略处理；默认保持 DAQ 可用但标记 Unverified
        result.verdict = BudgetVerdict::Unverified;
        result.allowed = (policy != UnknownLinkPolicy::Reject);
        result.allowed_bytes_per_second = 0.0;
        result.effective_bit_rate_bps = 0;
        msg << (result.allowed
                    ? "链路参数未配置：带宽能力 Unverified"
                    : "链路参数未配置且策略为 Reject：拒绝启动 DAQ");
        if (any_cycle_unknown) {
            msg << "；另有 Event 周期未知";
        }
        result.message = msg.str();
        return result;
    }

    // ---- 3. 生效比特率与允许字节率 ----
    std::uint64_t effective = config->nominal_bit_rate_bps;
    if (config->actual_available_bit_rate_bps.has_value()) {
        const std::uint64_t actual = *config->actual_available_bit_rate_bps;
        if (actual > 0U && actual < effective) {
            effective = actual;
        }
    }
    result.effective_bit_rate_bps = effective;
    // 允许字节率 = 比特率 / 8 * 负载百分比 / 100（不提前取整）
    result.allowed_bytes_per_second =
        static_cast<double>(effective) / 8.0 *
        (config->max_bus_load_percent / 100.0);

    // ---- 4. 结论 ----
    const bool exceeds =
        result.total_bytes_per_second > result.allowed_bytes_per_second;

    if (exceeds) {
        msg << "超出链路预算：需要 " << result.total_bytes_per_second
            << " byte/s，允许 " << result.allowed_bytes_per_second << " byte/s";
        if (over_budget_allowed) {
            // 显式放行只能降为 Unverified，不得标记 Verified
            result.verdict = BudgetVerdict::Unverified;
            result.allowed = true;
            msg << "；调用方已显式 allow_over_budget，降为 Unverified";
        } else {
            result.verdict = BudgetVerdict::OverBudget;
            result.allowed = false;
            msg << "；拒绝在 DAQ 启动前继续";
        }
    } else if (!alignment_known || any_cycle_unknown) {
        // 参数齐但证据不全：不得标记 Verified
        result.verdict = BudgetVerdict::Unverified;
        result.allowed = (policy != UnknownLinkPolicy::Reject);
        msg << "带宽未超限但证据不完整：";
        if (!alignment_known) {
            msg << "PACKET_ALIGNMENT 未知（Tail 长度按 0 计）";
        }
        if (any_cycle_unknown) {
            msg << (alignment_known ? "" : "；") << "存在周期未知的 Event";
        }
        if (!result.allowed) {
            msg << "；策略为 Reject，拒绝启动 DAQ";
        }
    } else {
        result.verdict = BudgetVerdict::Verified;
        result.allowed = true;
        msg << "带宽预算通过：需要 " << result.total_bytes_per_second
            << " byte/s，允许 " << result.allowed_bytes_per_second << " byte/s";
    }

    result.message = msg.str();
    return result;
}

}  // namespace calmcar::xcp