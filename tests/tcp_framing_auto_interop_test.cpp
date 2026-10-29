/**
 * @file tcp_framing_auto_interop_test.cpp
 * @brief `--tcp-profile auto` 的**真实验证侧**跨进程验收（独立 V1.5 从站）。
 *
 * @par 为什么要跨进程
 *   候选验证组件（`tcp_framing_verification.hpp`）的全部价值在于"用线路字节
 *   判断布局"，如果从站在同一进程内、复用同一份 framing 实现，测试只能证明
 *   自洽。本文件让从站跑在独立进程里（`tests/xcp_v15_alignment_slave.cpp`，
 *   自带独立 framing 实现），Master 侧用生产代码去验证候选。
 *
 * @par 覆盖范围与边界
 *   - 8/16/32 三种 PACKET_ALIGNMENT 下 auto 都能收敛到真实布局；
 *   - A2L 证据（对齐声明）只影响**顺序**：证据正确时一次命中；
 *   - 没有监听者时全部候选必须判为"未决"，而不是"被拒"（退出码语义的依据）。
 *   不声称 V1.5 一致性认证，也不声称与第三方 V1.5 实现互操作。
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "libxcp/command_executor.hpp"
#include "libxcp/protocol_types.hpp"
#include "libxcp/tcp_framing_candidates.hpp"
#include "libxcp/tcp_framing_negotiation.hpp"
#include "libxcp/tcp_framing_verification.hpp"
#include "libxcp/tcp_framing_verdict.hpp"
#include "xcp_v15_slave_process.hpp"

namespace calmcar::xcp {
namespace {

using test::XcpV15SlaveProcess;

/// @brief 在给定从站进程上跑一次完整的 auto 协商
/// @param slave 已 Start() 的从站
/// @param evidence 候选生成用的证据（可为全空）
/// @return 协商结果（含全部尝试记录）
TcpFramingNegotiationResult NegotiateAgainst(XcpV15SlaveProcess& slave,
                                            const TcpFramingCandidateRequest&
                                                evidence) {
    TcpFramingProbeConfig probe;
    probe.host = "127.0.0.1";
    probe.port = slave.Port();
    probe.connect_timeout_ms = 3000U;
    probe.command_timeout_ms = 1000U;

    const auto candidates = BuildTcpFramingCandidates(evidence);
    const auto attempt =
        [&probe](const TcpFramingCandidate& candidate) {
            return VerifyTcpFramingCandidate(probe, candidate);
        };
    return NegotiateTcpFraming(candidates, attempt);
}

/// @brief 渲染一次协商的全部尝试，失败时便于定位
std::string Describe(const TcpFramingNegotiationResult& outcome) {
    std::string text = outcome.Summary();
    for (const auto& item : outcome.attempts) {
        text += "\n  ";
        text += FramingCandidateName(item.candidate);
        text += " → ";
        text += FramingVerdictName(item.verdict);
        if (!item.failure_reason.empty()) {
            text += "（";
            text += item.failure_reason;
            text += "）";
        }
    }
    return text;
}

/// @brief 8/16/32 三种对齐都必须由真实的线路证据确定下来
TEST(TcpFramingAutoInterop, ResolvesEveryAlignmentFromRealSlave) {
    for (const std::uint8_t alignment : {8U, 16U, 32U}) {
        XcpV15SlaveProcess slave{alignment};
        slave.Start();

        const auto outcome = NegotiateAgainst(slave, {});
        slave.Stop();

        ASSERT_TRUE(outcome.selected.has_value()) << Describe(outcome);
        EXPECT_EQ(static_cast<std::uint8_t>(outcome.selected->alignment),
                  alignment)
            << Describe(outcome);

        // 8 bit 下 V1.4 与 V1.5 字节布局相同，默认顺序把 V1.4 排在首位，
        // 因此首个候选就应命中；16/32 bit 只可能来自 V1.5 且需要跳过前两个
        // 8 bit 候选，这正说明结论来自线路而非猜测。
        if (alignment == 8U) {
            EXPECT_EQ(outcome.selected->profile, TcpWireProfile::AsamV14)
                << Describe(outcome);
            EXPECT_EQ(outcome.attempts.size(), 1U) << Describe(outcome);
        } else {
            EXPECT_EQ(outcome.selected->profile, TcpWireProfile::AsamV15)
                << Describe(outcome);
            EXPECT_GT(outcome.attempts.size(), 1U) << Describe(outcome);
        }
        // 被跳过（判失败）的候选不得被判成"接受"
        for (const auto& item : outcome.attempts) {
            if (item.candidate.alignment != outcome.selected->alignment) {
                EXPECT_NE(item.verdict, FramingVerdict::Accepted)
                    << Describe(outcome);
            }
        }
    }
}

/// @brief A2L 证据只改变顺序：证据正确时第一个候选即命中
TEST(TcpFramingAutoInterop, EvidenceOnlyReordersCandidates) {
    XcpV15SlaveProcess slave{16U};
    slave.Start();

    TcpFramingCandidateRequest evidence;
    evidence.transport_layer_version = static_cast<std::uint16_t>(0x0105);
    evidence.alignment = PacketAlignment::Bit16;

    const auto candidates = BuildTcpFramingCandidates(evidence);
    ASSERT_FALSE(candidates.empty());
    EXPECT_EQ(candidates.front().alignment, PacketAlignment::Bit16);

    const auto outcome = NegotiateAgainst(slave, evidence);
    slave.Stop();

    ASSERT_TRUE(outcome.selected.has_value()) << Describe(outcome);
    EXPECT_EQ(outcome.selected->alignment, PacketAlignment::Bit16)
        << Describe(outcome);
    EXPECT_EQ(outcome.attempts.size(), 1U) << Describe(outcome);
    EXPECT_EQ(outcome.attempts.front().verdict, FramingVerdict::Accepted);
}

/// @brief 证据写错（声明 32 bit 但实际 16 bit）仍须收敛到真实布局
TEST(TcpFramingAutoInterop, WrongEvidenceStillConverges) {
    XcpV15SlaveProcess slave{16U};
    slave.Start();

    TcpFramingCandidateRequest evidence;
    evidence.alignment = PacketAlignment::Bit32;

    const auto outcome = NegotiateAgainst(slave, evidence);
    slave.Stop();

    ASSERT_TRUE(outcome.selected.has_value()) << Describe(outcome);
    EXPECT_EQ(outcome.selected->alignment, PacketAlignment::Bit16)
        << Describe(outcome);
    EXPECT_GT(outcome.attempts.size(), 1U) << Describe(outcome);
}

/// @brief 没有监听者时全部候选都是"未决"，不得伪装成"被拒"
TEST(TcpFramingAutoInterop, NoListenerYieldsInconclusive) {
    // 先真实启动一次从站拿到端口，再立刻停掉它：该端口随后无人监听，
    // 连接会被系统立即拒绝（而不是靠"猜一个没人用的端口"）。
    std::uint16_t closed_port = 0;
    {
        XcpV15SlaveProcess slave{8U};
        slave.Start();
        closed_port = slave.Port();
        slave.Stop();
    }
    ASSERT_NE(closed_port, 0);

    TcpFramingProbeConfig probe;
    probe.host = "127.0.0.1";
    probe.port = closed_port;
    probe.connect_timeout_ms = 800U;
    probe.command_timeout_ms = 300U;

    const auto candidates = DefaultFramingCandidates();
    const auto attempt = [&probe](const TcpFramingCandidate& candidate) {
        return VerifyTcpFramingCandidate(probe, candidate);
    };
    const auto outcome = NegotiateTcpFraming(candidates, attempt);

    EXPECT_FALSE(outcome.selected.has_value());
    ASSERT_EQ(outcome.attempts.size(), candidates.size());
    for (const auto& item : outcome.attempts) {
        EXPECT_EQ(item.verdict, FramingVerdict::Inconclusive)
            << FramingCandidateName(item.candidate) << "："
            << item.failure_reason;
    }
    // 摘要必须显式区分"未决"，这是 CLI 退出码 4 的文本依据
    EXPECT_NE(outcome.Summary().find("未决"), std::string::npos)
        << outcome.Summary();
}

}  // namespace
}  // namespace calmcar::xcp