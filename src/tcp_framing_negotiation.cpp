/**
 * @file tcp_framing_negotiation.cpp
 * @brief TCP 帧布局候选试连机制的实现（P3）。
 *
 * 本文件只做**顺序、预算、记录**：判定语义全部由调用方注入的谓词负责，
 * 传输层与协议层都不在这里参与决策。
 */

#include "libxcp/tcp_framing_negotiation.hpp"

#include <chrono>
#include <exception>
#include <utility>

namespace calmcar::xcp {
namespace {

/// @brief 对齐位宽的渲染文本（诊断用）
[[nodiscard]] std::string_view AlignmentName(PacketAlignment alignment) {
    switch (alignment) {
        case PacketAlignment::Bit8:
            return "8bit";
        case PacketAlignment::Bit16:
            return "16bit";
        case PacketAlignment::Bit32:
            return "32bit";
        case PacketAlignment::Unknown:
        default:
            return "unknown";
    }
}

/// @brief 候选是否可进入判定（Profile 与对齐均已显式选定）
[[nodiscard]] bool IsAttemptable(const TcpFramingCandidate& candidate) {
    return IsTcpWireProfileResolved(candidate.profile) &&
           candidate.alignment != PacketAlignment::Unknown;
}

}  // namespace

std::vector<TcpFramingCandidate> DefaultFramingCandidates() {
    // 顺序是契约的一部分：8 bit 优先（兼容面最大）、V1.4 先于 V1.5
    // （8 bit 下字节相同，取更宽松的 CTR 语义），非 8 bit 只可能是 V1.5。
    return {
        TcpFramingCandidate{TcpWireProfile::AsamV14, PacketAlignment::Bit8},
        TcpFramingCandidate{TcpWireProfile::AsamV15, PacketAlignment::Bit8},
        TcpFramingCandidate{TcpWireProfile::AsamV15, PacketAlignment::Bit16},
        TcpFramingCandidate{TcpWireProfile::AsamV15, PacketAlignment::Bit32},
    };
}

std::string FramingCandidateName(const TcpFramingCandidate& candidate) {
    std::string name(TcpWireProfileName(candidate.profile));
    name += '/';
    name += AlignmentName(candidate.alignment);
    return name;
}

std::string TcpFramingNegotiationResult::Summary() const {
    std::string text;
    if (selected.has_value()) {
        long long total_ms = 0;
        for (const auto& attempt : attempts) {
            total_ms += attempt.elapsed_ms;
        }
        // 字段名与计划 §6.4 对齐（profile=/alignment=），便于日志检索与脚本化。
        text = "TCP 帧布局协商成功：profile=";
        text += TcpWireProfileName(selected->profile);
        text += " alignment=";
        text += AlignmentName(selected->alignment);
        text += "（第 ";
        text += std::to_string(attempts.size());
        text += " 次尝试，累计 ";
        text += std::to_string(total_ms);
        text += " ms）";
        return text;
    }
    // 定性结论必须区分"对端明确拒绝"与"对端根本没回话"：后者通常是端口写错、
    // 从站卡死或网络阻断，报"四种布局都不支持"会误导用户去改配置。
    bool any_inconclusive = false;
    for (const auto& attempt : attempts) {
        if (attempt.verdict == FramingVerdict::Inconclusive) {
            any_inconclusive = true;
            break;
        }
    }
    text = any_inconclusive
               ? "TCP 帧布局协商未决：部分候选没有任何协议层证据"
               : "TCP 帧布局协商失败：全部候选均被对端拒绝";
    text += "（共 ";
    text += std::to_string(attempts.size());
    text += " 个候选）";
    for (const auto& attempt : attempts) {
        text += "\n  · profile=";
        text += TcpWireProfileName(attempt.candidate.profile);
        text += " alignment=";
        text += AlignmentName(attempt.candidate.alignment);
        text += "：";
        text += FramingVerdictName(attempt.verdict);
        if (!attempt.failure_reason.empty()) {
            text += "（";
            text += attempt.failure_reason;
            text += "）";
        }
    }
    return text;
}

TcpFramingNegotiationResult NegotiateTcpFraming(
    const std::vector<TcpFramingCandidate>& candidates,
    const FramingAttemptFn& attempt) {
    TcpFramingNegotiationResult result;
    result.attempts.reserve(candidates.size());

    for (const auto& candidate : candidates) {
        FramingAttemptOutcome outcome;
        outcome.candidate = candidate;

        if (!IsAttemptable(candidate)) {
            // 配置错误必须与"对端不支持"区分开：不调用谓词、直接记拒绝
            outcome.failure_reason =
                "候选非法：Profile 或 PACKET_ALIGNMENT 未指定";
            result.attempts.push_back(std::move(outcome));
            continue;
        }
        if (!attempt) {
            outcome.failure_reason = "未提供判定谓词";
            result.attempts.push_back(std::move(outcome));
            continue;
        }

        const auto started = std::chrono::steady_clock::now();
        FramingVerificationResult verdict;
        try {
            verdict = attempt(candidate);
        } catch (const std::exception& error) {
            // 谓词内部通常直接构造 Master 并 Connect()，异常就是"这个候选
            // 解析不出合法响应"的主要信号，必须转成结论而不是外泄。
            verdict = FramingVerificationResult{};
            verdict.verdict = FramingVerdict::Rejected;
            verdict.reason = error.what();
        } catch (...) {
            verdict = FramingVerificationResult{};
            verdict.verdict = FramingVerdict::Rejected;
            verdict.reason = "尝试过程中抛出未知异常";
        }
        const auto finished = std::chrono::steady_clock::now();
        outcome.elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(finished -
                                                                  started)
                .count();
        outcome.verdict = verdict.verdict;
        const bool accepted = verdict.verdict == FramingVerdict::Accepted;
        if (!accepted) {
            outcome.failure_reason =
                verdict.reason.empty() ? "判定谓词未给出原因" : verdict.reason;
        }
        result.attempts.push_back(std::move(outcome));

        if (accepted) {
            result.selected = candidate;
            return result;
        }
    }
    return result;
}

}  // namespace calmcar::xcp