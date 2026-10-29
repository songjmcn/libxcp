/**
 * @file tcp_framing_candidates.cpp
 * @brief 候选顺序生成实现（设计文档 C1）。
 */

#include "libxcp/tcp_framing_candidates.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace calmcar::xcp {
namespace {

/// @brief V1.5 传输层版本的起始值（`0x0105`）
constexpr std::uint16_t kTransportLayerV15 = 0x0105;

/// @brief 两个候选是否相同（用于去重）
[[nodiscard]] bool SameCandidate(const TcpFramingCandidate& lhs,
                                 const TcpFramingCandidate& rhs) {
    return lhs.profile == rhs.profile && lhs.alignment == rhs.alignment;
}

/// @brief 把候选追加到序列尾部（已存在则跳过，保持首次出现的位置）
void AppendUnique(std::vector<TcpFramingCandidate>& candidates,
                  const TcpFramingCandidate& candidate) {
    const bool exists =
        std::any_of(candidates.begin(), candidates.end(),
                    [&candidate](const TcpFramingCandidate& existing) {
                        return SameCandidate(existing, candidate);
                    });
    if (!exists) {
        candidates.push_back(candidate);
    }
}

/// @brief 证据是否指向 V1.5（严格 CTR）语义
[[nodiscard]] bool EvidenceSuggestsV15(
    const std::optional<std::uint16_t>& version) {
    return version.has_value() && *version >= kTransportLayerV15;
}

}  // namespace

std::vector<TcpFramingCandidate> BuildTcpFramingCandidates(
    const TcpFramingCandidateRequest& request) {
    std::vector<TcpFramingCandidate> candidates;
    const auto defaults = DefaultFramingCandidates();
    candidates.reserve(defaults.size() + 1);

    switch (request.alignment) {
        case PacketAlignment::Bit8: {
            // 8 bit 下两种 Profile 字节完全一致，只有 CTR 严格性不同：
            // 证据说是 V1.5 就先试 V1.5，否则先试更宽松的 V1.4。
            const TcpFramingCandidate v14{TcpWireProfile::AsamV14,
                                          PacketAlignment::Bit8};
            const TcpFramingCandidate v15{TcpWireProfile::AsamV15,
                                          PacketAlignment::Bit8};
            if (EvidenceSuggestsV15(request.transport_layer_version)) {
                AppendUnique(candidates, v15);
                AppendUnique(candidates, v14);
            } else {
                AppendUnique(candidates, v14);
                AppendUnique(candidates, v15);
            }
            break;
        }
        case PacketAlignment::Bit16:
        case PacketAlignment::Bit32: {
            // V1.4 的帧长度没有填充语义，非 8 bit 只可能是 V1.5
            AppendUnique(candidates,
                         TcpFramingCandidate{TcpWireProfile::AsamV15,
                                             request.alignment});
            break;
        }
        case PacketAlignment::Unknown:
        default:
            // 没有任何对齐证据：连第一个候选都不该猜，直接用默认顺序
            break;
    }

    // 证据命中的候选排最前，其余按默认顺序补齐，避免"A2L 写错就彻底连不上"
    for (const auto& candidate : defaults) {
        AppendUnique(candidates, candidate);
    }
    return candidates;
}

}  // namespace calmcar::xcp