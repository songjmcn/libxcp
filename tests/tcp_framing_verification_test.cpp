/**
 * @file tcp_framing_verification_test.cpp
 * @brief 候选验证的失败分类单元测试（设计文档 C2）。
 *
 * @par 为什么不覆盖真实连接
 *   `VerifyTcpFramingCandidate()` 需要真实 socket 与真实从站，属于交叉进程
 *   验收范围（由 V1.2/V2.2 从站夹具与 auto 端到端用例覆盖）。这里只钉住
 *   `ClassifyFramingFailure()` 这张"错误分类 → 三态"的映射表——它决定了错误
 *   文本与退出码是否诚实，一旦把超时误判成"被拒"，用户会被引导去改配置。
 */

#include "libxcp/tcp_framing_verification.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using calmcar::xcp::ClassifyFramingFailure;
using calmcar::xcp::ErrorCategory;
using calmcar::xcp::FramingVerdict;
using calmcar::xcp::PacketAlignment;
using calmcar::xcp::TcpFramingCandidate;
using calmcar::xcp::TcpFramingProbeConfig;
using calmcar::xcp::TcpWireProfile;
using calmcar::xcp::VerifyTcpFramingCandidate;

/// @brief 超时无论连接是否建立，都是"未决"：没有协议层证据
TEST(TcpFramingVerification, TimeoutIsAlwaysInconclusive) {
    EXPECT_EQ(ClassifyFramingFailure(ErrorCategory::Timeout, false),
              FramingVerdict::Inconclusive);
    EXPECT_EQ(ClassifyFramingFailure(ErrorCategory::Timeout, true),
              FramingVerdict::Inconclusive);
}

/// @brief 连接阶段传输失败：分不清"不支持"与"没在监听"，只能算未决
TEST(TcpFramingVerification, TransportErrorBeforeConnectIsInconclusive) {
    EXPECT_EQ(ClassifyFramingFailure(ErrorCategory::TransportError, false),
              FramingVerdict::Inconclusive);
}

/// @brief 连接已建立后传输失败：对端主动断开，是明确的拒绝证据
TEST(TcpFramingVerification, TransportErrorAfterConnectIsRejected) {
    EXPECT_EQ(ClassifyFramingFailure(ErrorCategory::TransportError, true),
              FramingVerdict::Rejected);
}

/// @brief 协议层违规一律视为拒绝，与连接状态无关
TEST(TcpFramingVerification, ProtocolViolationsAreRejected) {
    for (const auto category : {ErrorCategory::MalformedPacket,
                                ErrorCategory::ProtocolError,
                                ErrorCategory::InvalidState,
                                ErrorCategory::InvalidArgument,
                                ErrorCategory::RecoveryFailed,
                                ErrorCategory::UnsupportedFeature,
                                ErrorCategory::Cancelled}) {
        EXPECT_EQ(ClassifyFramingFailure(category, false),
                  FramingVerdict::Rejected);
        EXPECT_EQ(ClassifyFramingFailure(category, true),
                  FramingVerdict::Rejected);
    }
}

/// @brief 结果未知（响应丢失）证据强度等同超时，不得算成"被拒"
TEST(TcpFramingVerification, OutcomeUnknownIsInconclusive) {
    EXPECT_EQ(
        ClassifyFramingFailure(ErrorCategory::OperationOutcomeUnknown, true),
        FramingVerdict::Inconclusive);
}

/// @brief 非法候选不建连接，直接给出"被拒 + 配置错误"的原因
TEST(TcpFramingVerification, InvalidCandidateIsRejectedWithoutConnecting) {
    TcpFramingProbeConfig config;
    config.host = "127.0.0.1";
    config.port = 5555;

    const TcpFramingCandidate candidate{TcpWireProfile::AsamV15,
                                        PacketAlignment::Unknown};
    const auto result = VerifyTcpFramingCandidate(config, candidate);

    EXPECT_EQ(result.verdict, FramingVerdict::Rejected);
    EXPECT_NE(result.reason.find("候选非法"), std::string::npos) << result.reason;
    EXPECT_EQ(result.elapsed_ms, 0);
}

/// @brief 端口为 0 之类的参数错误同样不得建连接
TEST(TcpFramingVerification, InvalidProbeConfigIsRejectedWithoutConnecting) {
    TcpFramingProbeConfig config;
    config.host = "127.0.0.1";
    config.port = 0;

    const TcpFramingCandidate candidate{TcpWireProfile::AsamV14,
                                        PacketAlignment::Bit8};
    const auto result = VerifyTcpFramingCandidate(config, candidate);

    EXPECT_EQ(result.verdict, FramingVerdict::Rejected);
    EXPECT_NE(result.reason.find("验证参数非法"), std::string::npos)
        << result.reason;
}

}  // namespace