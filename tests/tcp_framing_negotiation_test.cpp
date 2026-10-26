/**
 * @file tcp_framing_negotiation_test.cpp
 * @brief TCP 帧布局候选试连机制（P3）的单元测试。
 *
 * 依据 `code-plan/传输帧布局自动协商_实施计划.md` §6 与 §9 编写。
 *
 * @par 测试策略
 *   机制本身只负责"顺序、预算、记录"，判定语义由注入的谓词决定，因此这里
 *   全部用**假谓词**钉住契约，不建立任何真实 socket：真实连通性由
 *   `xcplite_tcp_interop_test.cpp` 覆盖。
 */

#include "libxcp/tcp_framing_negotiation.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace {

using calmcar::xcp::DefaultFramingCandidates;
using calmcar::xcp::FramingAttemptOutcome;
using calmcar::xcp::FramingCandidateName;
using calmcar::xcp::NegotiateTcpFraming;
using calmcar::xcp::PacketAlignment;
using calmcar::xcp::TcpFramingCandidate;
using calmcar::xcp::TcpFramingNegotiationResult;
using calmcar::xcp::TcpWireProfile;

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
        [&tried](const TcpFramingCandidate& candidate, std::string*) {
            tried.push_back(FramingCandidateName(candidate));
            return true;
        });

    ASSERT_TRUE(result.selected.has_value());
    EXPECT_EQ(result.selected->profile, TcpWireProfile::AsamV14);
    EXPECT_EQ(result.selected->alignment, PacketAlignment::Bit8);
    ASSERT_EQ(tried.size(), 1U);
    ASSERT_EQ(result.attempts.size(), 1U);
    EXPECT_TRUE(result.attempts.front().accepted);
}

/// @brief 被拒候选逐个记录原因后继续，胜出者可以是后面的候选
TEST(TcpFramingNegotiation, SkipsRejectedCandidatesAndRecordsReasons) {
    const auto result = NegotiateTcpFraming(
        DefaultFramingCandidates(),
        [](const TcpFramingCandidate& candidate, std::string* reason) {
            if (candidate.alignment == PacketAlignment::Bit16) {
                return true;
            }
            *reason = "长度不自洽";
            return false;
        });

    ASSERT_TRUE(result.selected.has_value());
    EXPECT_EQ(result.selected->alignment, PacketAlignment::Bit16);
    ASSERT_EQ(result.attempts.size(), 3U);
    EXPECT_FALSE(result.attempts[0].accepted);
    EXPECT_EQ(result.attempts[0].failure_reason, "长度不自洽");
    EXPECT_FALSE(result.attempts[1].accepted);
    EXPECT_TRUE(result.attempts[2].accepted);
}

/// @brief 全部候选失败时必须没有胜出者，且每个候选都要有原因
TEST(TcpFramingNegotiation, AllCandidatesRejectedYieldsNoSelection) {
    const auto result = NegotiateTcpFraming(
        DefaultFramingCandidates(),
        [](const TcpFramingCandidate&, std::string* reason) {
            *reason = "CONNECT 无响应";
            return false;
        });

    EXPECT_FALSE(result.selected.has_value());
    EXPECT_EQ(result.attempts.size(), 4U);
    for (const auto& attempt : result.attempts) {
        EXPECT_FALSE(attempt.accepted);
        EXPECT_EQ(attempt.failure_reason, "CONNECT 无响应");
    }
    // 失败摘要必须逐条列出候选与原因，否则现场无法判断"是全部不支持还是
    // 只有某一个配错"。
    const std::string summary = result.Summary();
    EXPECT_NE(summary.find("profile=AsamV14 alignment=8bit"), std::string::npos)
        << summary;
    EXPECT_NE(summary.find("CONNECT 无响应"), std::string::npos) << summary;
}

/// @brief 未指定 Profile/对齐的候选属于配置错误，不得调用谓词
TEST(TcpFramingNegotiation, InvalidCandidateIsNotAttempted) {
    int calls = 0;
    const std::vector<TcpFramingCandidate> candidates{
        TcpFramingCandidate{TcpWireProfile::Unknown, PacketAlignment::Bit8},
        TcpFramingCandidate{TcpWireProfile::AsamV15, PacketAlignment::Unknown},
    };
    const auto result = NegotiateTcpFraming(
        candidates, [&calls](const TcpFramingCandidate&, std::string*) {
            ++calls;
            return true;
        });

    EXPECT_EQ(calls, 0);
    EXPECT_FALSE(result.selected.has_value());
    ASSERT_EQ(result.attempts.size(), 2U);
    EXPECT_NE(result.attempts[0].failure_reason.find("候选非法"),
              std::string::npos);
}

/// @brief 谓词抛异常要转成该候选的失败原因，绝不外泄给调用方
TEST(TcpFramingNegotiation, PredicateExceptionBecomesFailureReason) {
    const auto result = NegotiateTcpFraming(
        DefaultFramingCandidates(),
        [](const TcpFramingCandidate& candidate, std::string*) -> bool {
            if (candidate.alignment == PacketAlignment::Bit8) {
                throw std::runtime_error{"连接被拒绝"};
            }
            return true;
        });

    ASSERT_TRUE(result.selected.has_value());
    EXPECT_EQ(result.selected->alignment, PacketAlignment::Bit16);
    ASSERT_GE(result.attempts.size(), 3U);
    EXPECT_FALSE(result.attempts[0].accepted);
    EXPECT_NE(result.attempts[0].failure_reason.find("连接被拒绝"),
              std::string::npos);
}

/// @brief 每次尝试必须记录耗时（用于把"最坏 4 次连接"变成可观测量）
TEST(TcpFramingNegotiation, RecordsElapsedMillisecondsPerAttempt) {
    const auto result = NegotiateTcpFraming(
        DefaultFramingCandidates(),
        [](const TcpFramingCandidate&, std::string*) { return false; });

    ASSERT_EQ(result.attempts.size(), 4U);
    for (const auto& attempt : result.attempts) {
        EXPECT_GE(attempt.elapsed_ms, 0);
    }
}

}  // namespace