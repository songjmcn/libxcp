/**
 * @file tcp_framing_candidates.hpp
 * @brief 依据已有证据生成 `--tcp-profile auto` 的候选顺序（设计文档 C1）。
 *
 * @par 证据只是"优先",不是"命令"
 *   A2L 里声明的 TL 版本与 PACKET_ALIGNMENT 可能过时、抄错甚至属于别的
 *   ECU 配置。因此本组件把它们当作**排序偏好**：按证据最可能的候选先试，
 *   其余候选仍按默认顺序跟在其后。这样"证据正确"时第一次就命中，"证据错误"
 *   时仍能收敛到真实布局——两种情况下都不需要用户手工改命令行。
 *
 *   但**根数**是硬约束：证据指向 8 bit 时，V1.5 与 V1.4 的字节布局完全相同，
 *   差别只在 CTR 严格性，因此仍保留两者；证据指向 16/32 bit 时只可能是
 *   V1.5（V1.4 没有填充语义），此时不浪费一次连接去试 V1.4 + 填充。
 */

#ifndef CALMCAR_XCP_TCP_FRAMING_CANDIDATES_HPP_
#define CALMCAR_XCP_TCP_FRAMING_CANDIDATES_HPP_

#include <cstdint>
#include <optional>
#include <vector>

#include "libxcp/protocol_types.hpp"
#include "libxcp/tcp_framing_negotiation.hpp"

namespace calmcar::xcp {

/**
 * @brief 生成候选顺序时可用的证据
 *
 * @details 两个字段都可能是"未知"：未知时退化为默认顺序，绝不猜测。
 */
struct TcpFramingCandidateRequest {
    /**
     * @brief A2L IF_DATA 声明的传输层版本（如 `0x0104`）
     * @details 仅用于区分 8 bit 下先试 V1.4 还是 V1.5：
     *          `>= 0x0105` 视为 V1.5 语义（严格 CTR）。
     */
    std::optional<std::uint16_t> transport_layer_version;

    /// @brief 证据指向的对齐（`Unknown` 表示没有证据）
    PacketAlignment alignment{PacketAlignment::Unknown};
};

/**
 * @brief 生成候选序列（顺序即契约，调用方不得重排）
 *
 * @param request 证据；字段全为空时等价于 `DefaultFramingCandidates()`
 * @return 去重后的候选序列，至少包含默认 4 个候选中的一个
 *
 * @details 规则：
 *          - 对齐未知 → 直接返回默认顺序（无法从版本推断对齐，见设计 C1）；
 *          - 对齐 8 bit → 先按 TL 版本在 `{V14,8}`/`{V15,8}` 中择优，再补另一
 *            个，最后补 `{V15,16}`/`{V15,32}`；
 *          - 对齐 16/32 bit → 先 `{V15,该对齐}`，再 `{V14,8}`、`{V15,8}`、
 *            其余对齐，保证 A2L 写错时仍能收敛。
 */
[[nodiscard]] std::vector<TcpFramingCandidate> BuildTcpFramingCandidates(
    const TcpFramingCandidateRequest& request);

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_TCP_FRAMING_CANDIDATES_HPP_