/**
 * @file tcp_framing_negotiation_test.cpp
 * @brief TCP 帧布局候选试连机制（P3）的单元测试。
 *
 * 依据 `code-plan/传输帧布局自动协商_实施计划.md` §6 与 §9、
 * `code-plan/TCP_auto证据收敛与候选验证_设计.md` C3 编写。
 *
 * @par 测试策略
 *   机制本身只负责"顺序、预算、记录"，判定语义由注入的谓词决定，因此这里
 *   全部用**假谓词**钉住契约，不建立任何真实 socket：真实连通性由
 *   `xcplite_tcp_interop_test.cpp` 与 auto 端到端用例覆盖。
 *
 * @par 三态语义是重点
 *   "被拒"与"未决"必须能被调用方区分：前者是"对端明确不要这个布局"，后者是
 *   "对端根本没回话"。混在一起会输出误导性的诊断，因此下面的用例逐条钉住
 *   扫描是否继续、摘要如何措辞。
 */

#include "libxcp/tcp_framing_negotiation.hpp"
#include "libxcp/tcp_framing_verdict.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace {

using calmcar::xcp::DefaultFramingCandidates;
using calmcar::xcp::FramingAttemptOutcome;
using calmcar::xcp::FramingCandidateName;
using calmcar::xcp::FramingVerdict;
using calmcar::xcp::FramingVerificationResult;
using calmcar::xcp::NegotiateTcpFraming;
using calmcar::xcp::PacketAlignment;
using calmcar::xcp::TcpFramingCandidate;
using calmcar::xcp::TcpFramingNegotiationResult;
using calmcar::xcp::TcpWireProfile;

/// @brief 构造一个带结论与原因的谓词返回值
FramingVerificationResult Verdict(FramingVerdict verdict,
                                  std::string reason = {}) {
    FramingVerificationResult result;
    result.verdict = verdict;
    result.reason = std::move(reason);
    return result;
}

/// @brief 候选默认顺序：8 bit 优先、V1.4 先于 V1.5、非 8 bit 只配 V1.5
TEST(TcpFramingNegotiation, DefaultCandidatesKeepFixedOrder) {
    const auto candidates = DefaultFramingCandidates();
    ASSERT_EQ(candidates.size(), 4U);

    EXPECT_EQ(candidates[0].profile, TcpWireProfile::AsamV14);
    EXPECT_EQ(candidates[0].alignment, PacketAlignment::Bit8);
    EXPECT_EQ(candidates[1].profile, TcpWireProfile::AsamV15);
    EXPECT_EQ(candidates[1].alignment, PacketAlignment::Bit8);
    EXPECT_EQ(candidates[2].alignment, PacketAlignment::Bit16);
    EXPECT_EQ(candidates[3].alignment, PacketAlignment::Bit32);
    // 非 8 bit 只出现在 V1.5 上：V1.4 没有 LEN 外 Tail，16/32 无意义。
    EXPECT_EQ(candidates[2].profile, TcpWireProfile::AsamV15);
    EXPECT_EQ(candidates[3].profile, TcpWireProfile::AsamV15);
}

/// @brief 首个被接受者胜出，且不再尝试后续候选
TEST(TcpFramingNegotiation, StopsAtFirstAcceptedCandidate) {
    std::vector<std::string> tried;
    const auto result = NegotiateTcpFraming(
        DefaultFramingCandidates(),
        [&tried](const TcpFramingCandidate& candidate) {
            tried.push_back(FramingCandidateName(candidate));
            return Verdict(FramingVerdict::Accepted);
        });

    ASSERT_TRUE(result.selected.has_value());
    EXPECT_EQ(result.selected->profile, TcpWireProfile::AsamV14);
    EXPECT_EQ(result.selected->alignment, PacketAlignment::Bit8);
    ASSERT_EQ(tried.size(), 1U);
    ASSERT_EQ(result.attempts.size(), 1U);
    EXPECT_EQ(result.attempts.front().verdict, FramingVerdict::Accepted);
    // 被接受时不得残留原因，否则日志会自相矛盾
    EXPECT_TRUE(result.attempts.front().failure_reason.empty());
}

/// @brief 被拒候选逐个记录原因后继续，胜出者可以是后面的候选
TEST(TcpFramingNegotiation, SkipsRejectedCandidatesAndRecordsReasons) {
    const auto result = NegotiateTcpFraming(
        DefaultFramingCandidates(),
        [](const TcpFramingCandidate& candidate) {
            if (candidate.alignment == PacketAlignment::Bit16) {
                return Verdict(FramingVerdict::Accepted);
            }
            return Verdict(FramingVerdict::Rejected, "长度不自洽");
        });

    ASSERT_TRUE(result.selected.has_value());
    EXPECT_EQ(result.selected->alignment, PacketAlignment::Bit16);
    ASSERT_EQ(result.attempts.size(), 3U);
    EXPECT_EQ(result.attempts[0].verdict, FramingVerdict::Rejected);
    EXPECT_EQ(result.attempts[0].failure_reason, "长度不自洽");
    EXPECT_EQ(result.attempts[1].verdict, FramingVerdict::Rejected);
    EXPECT_EQ(result.attempts[2].verdict, FramingVerdict::Accepted);
}

/// @brief "未决"既不终止扫描，也不得被当成"被拒"记录下来
TEST(TcpFramingNegotiation, InconclusiveDoesNotStopScanning) {
    const auto result = NegotiateTcpFraming(
        DefaultFramingCandidates(),
        [](const TcpFramingCandidate& candidate) {
            if (candidate.alignment == PacketAlignment::Bit8) {
                return Verdict(FramingVerdict::Inconclusive, "等待响应超时");
            }
            return Verdict(FramingVerdict::Accepted);
        });

    ASSERT_TRUE(result.selected.has_value());
    // 前两个都是 8 bit：都应当是"未决"，而不是被记成被拒
    EXPECT_EQ(result.selected->alignment, PacketAlignment::Bit16);
    ASSERT_EQ(result.attempts.size(), 3U);
    EXPECT_EQ(result.attempts[0].verdict, FramingVerdict::Inconclusive);
    EXPECT_EQ(result.attempts[1].verdict, FramingVerdict::Inconclusive);
    EXPECT_EQ(result.attempts[2].verdict, FramingVerdict::Accepted);
}

/// @brief 全部被拒：摘要必须说"被对端拒绝"，不能暗示"没有响应"
TEST(TcpFramingNegotiation, AllRejectedSummarySaysRejected) {
    const auto result = NegotiateTcpFraming(
        DefaultFramingCandidates(),
        [](const TcpFramingCandidate&) {
            return Verdict(FramingVerdict::Rejected, "对端关闭连接");
        });

    EXPECT_FALSE(result.selected.has_value());
    EXPECT_EQ(result.attempts.size(), 4U);
    const std::string summary = result.Summary();
    EXPECT_NE(summary.find("全部候选均被对端拒绝"), std::string::npos) << summary;
    EXPECT_EQ(summary.find("协商未决"), std::string::npos) << summary;
    EXPECT_NE(summary.find("profile=AsamV14 alignment=8bit"), std::string::npos)
        << summary;
    EXPECT_NE(summary.find("对端关闭连接"), std::string::npos) << summary;
}

/// @brief 存在未决且无胜者：摘要必须说"未决"，并给出"查网络/端口"的方向
TEST(TcpFramingNegotiation, InconclusiveSummarySaysInconclusive) {
    const auto result = NegotiateTcpFraming(
        DefaultFramingCandidates(),
        [](const TcpFramingCandidate& candidate) {
            if (candidate.alignment == PacketAlignment::Bit16) {
                return Verdict(FramingVerdict::Rejected, "长度非法");
            }
            return Verdict(FramingVerdict::Inconclusive, "等待响应超时");
        });

    EXPECT_FALSE(result.selected.has_value());
    const std::string summary = result.Summary();
    EXPECT_NE(summary.find("协商未决"), std::string::npos) << summary;
    // 逐项结论必须保留：只写"未决"而不说明哪几个候选未决，现场无法定位
    EXPECT_NE(summary.find("未决（等待响应超时）"), std::string::npos) << summary;
    EXPECT_NE(summary.find("被拒（长度非法）"), std::string::npos) << summary;
}

/// @brief 未指定 Profile/对齐的候选属于配置错误，不得调用谓词
TEST(TcpFramingNegotiation, InvalidCandidateIsNotAttempted) {
    int calls = 0;
    const std::vector<TcpFramingCandidate> candidates{
        TcpFramingCandidate{TcpWireProfile::Unknown, PacketAlignment::Bit8},
        TcpFramingCandidate{TcpWireProfile::AsamV15, PacketAlignment::Unknown},
    };
    const auto result = NegotiateTcpFraming(
        candidates, [&calls](const TcpFramingCandidate&) {
            ++calls;
            return Verdict(FramingVerdict::Accepted);
        });

    EXPECT_EQ(calls, 0);
    EXPECT_FALSE(result.selected.has_value());
    ASSERT_EQ(result.attempts.size(), 2U);
    EXPECT_EQ(result.attempts[0].verdict, FramingVerdict::Rejected);
    EXPECT_NE(result.attempts[0].failure_reason.find("候选非法"),
              std::string::npos);
}

/// @brief 谓词抛异常要转成该候选的结论，绝不外泄给调用方
TEST(TcpFramingNegotiation, PredicateExceptionBecomesRejected) {
    const auto result = NegotiateTcpFraming(
        DefaultFramingCandidates(), [](const TcpFramingCandidate& candidate) {
            if (candidate.alignment == PacketAlignment::Bit8) {
                throw std::runtime_error{"连接被拒绝"};
            }
            return Verdict(FramingVerdict::Accepted);
        });

    ASSERT_TRUE(result.selected.has_value());
    EXPECT_EQ(result.selected->alignment, PacketAlignment::Bit16);
    ASSERT_GE(result.attempts.size(), 3U);
    EXPECT_EQ(result.attempts[0].verdict, FramingVerdict::Rejected);
    EXPECT_NE(result.attempts[0].failure_reason.find("连接被拒绝"),
              std::string::npos);
}

/// @brief 每次尝试必须记录耗时（用于把"最坏 4 次连接"变成可观测量）
TEST(TcpFramingNegotiation, RecordsElapsedMillisecondsPerAttempt) {
    const auto result = NegotiateTcpFraming(
        DefaultFramingCandidates(), [](const TcpFramingCandidate&) {
            return Verdict(FramingVerdict::Inconclusive, "超时");
        });

    ASSERT_EQ(result.attempts.size(), 4U);
    for (const auto& attempt : result.attempts) {
        EXPECT_GE(attempt.elapsed_ms, 0);
        EXPECT_EQ(attempt.verdict, FramingVerdict::Inconclusive);
    }
}

}  // namespace