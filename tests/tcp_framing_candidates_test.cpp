/**
 * @file tcp_framing_candidates_test.cpp
 * @brief `BuildTcpFramingCandidates()` 的单元测试（设计文档 C1）。
 *
 * @par 钉住的契约
 *   证据只影响**排序**，不删除任何默认候选；证据与真实布局不符时仍必须能
 *   收敛。反过来，证据缺失时顺序必须与默认序列完全一致——"没有证据就不猜"。
 */

#include "libxcp/tcp_framing_candidates.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace {

using calmcar::xcp::BuildTcpFramingCandidates;
using calmcar::xcp::DefaultFramingCandidates;
using calmcar::xcp::PacketAlignment;
using calmcar::xcp::TcpFramingCandidate;
using calmcar::xcp::TcpFramingCandidateRequest;
using calmcar::xcp::TcpWireProfile;

/// @brief 两个候选序列是否逐项相同
bool SameSequence(const std::vector<TcpFramingCandidate>& lhs,
                  const std::vector<TcpFramingCandidate>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].profile != rhs[i].profile ||
            lhs[i].alignment != rhs[i].alignment) {
            return false;
        }
    }
    return true;
}

/// @brief 没有证据时退化为默认顺序（"没有证据就不猜"）
TEST(TcpFramingCandidates, NoEvidenceFallsBackToDefaultOrder) {
    const TcpFramingCandidateRequest request;
    const auto candidates = BuildTcpFramingCandidates(request);
    EXPECT_TRUE(SameSequence(candidates, DefaultFramingCandidates()));
}

/// @brief 对齐未知时，即便给了 TL 版本也不得用来推断对齐
TEST(TcpFramingCandidates, VersionAloneCannotInferAlignment) {
    TcpFramingCandidateRequest request;
    request.transport_layer_version = 0x0105;
    const auto candidates = BuildTcpFramingCandidates(request);
    EXPECT_TRUE(SameSequence(candidates, DefaultFramingCandidates()));
}

/// @brief 8 bit + V1.5 证据：先试 V1.5/8，再试 V1.4/8
TEST(TcpFramingCandidates, EightBitWithV15EvidencePrefersV15) {
    TcpFramingCandidateRequest request;
    request.alignment = PacketAlignment::Bit8;
    request.transport_layer_version = 0x0105;
    const auto candidates = BuildTcpFramingCandidates(request);

    ASSERT_GE(candidates.size(), 2U);
    EXPECT_EQ(candidates[0].profile, TcpWireProfile::AsamV15);
    EXPECT_EQ(candidates[0].alignment, PacketAlignment::Bit8);
    EXPECT_EQ(candidates[1].profile, TcpWireProfile::AsamV14);
    EXPECT_EQ(candidates[1].alignment, PacketAlignment::Bit8);
    // 其余候选仍必须保留：证据可能过时，不能因此拒绝真实布局
    EXPECT_EQ(candidates.size(), DefaultFramingCandidates().size());
}

/// @brief 8 bit + V1.4 证据（或没有版本证据）：先试 V1.4/8
TEST(TcpFramingCandidates, EightBitWithV14EvidencePrefersV14) {
    TcpFramingCandidateRequest request;
    request.alignment = PacketAlignment::Bit8;
    request.transport_layer_version = 0x0104;
    const auto candidates = BuildTcpFramingCandidates(request);

    ASSERT_GE(candidates.size(), 2U);
    EXPECT_EQ(candidates[0].profile, TcpWireProfile::AsamV14);
    EXPECT_EQ(candidates[1].profile, TcpWireProfile::AsamV15);
}

/// @brief 8 bit 但没有 TL 版本：按"更宽松优先"仍先试 V1.4
TEST(TcpFramingCandidates, EightBitWithoutVersionPrefersV14) {
    TcpFramingCandidateRequest request;
    request.alignment = PacketAlignment::Bit8;
    const auto candidates = BuildTcpFramingCandidates(request);

    ASSERT_FALSE(candidates.empty());
    EXPECT_EQ(candidates[0].profile, TcpWireProfile::AsamV14);
    EXPECT_EQ(candidates[0].alignment, PacketAlignment::Bit8);
}

/// @brief 16 bit 证据：只可能是 V1.5，且必须排在最前
TEST(TcpFramingCandidates, SixteenBitEvidencePrefersV15Only) {
    TcpFramingCandidateRequest request;
    request.alignment = PacketAlignment::Bit16;
    request.transport_layer_version = 0x0104;  // 版本与对齐矛盾时以对齐为准
    const auto candidates = BuildTcpFramingCandidates(request);

    ASSERT_FALSE(candidates.empty());
    EXPECT_EQ(candidates[0].profile, TcpWireProfile::AsamV15);
    EXPECT_EQ(candidates[0].alignment, PacketAlignment::Bit16);
    // V1.4 没有 LEN 外 Tail，不会出现 "AsamV14 + 16bit" 这种组合
    for (const auto& candidate : candidates) {
        if (candidate.profile == TcpWireProfile::AsamV14) {
            EXPECT_EQ(candidate.alignment, PacketAlignment::Bit8);
        }
    }
    EXPECT_EQ(candidates.size(), DefaultFramingCandidates().size());
}

/// @brief 32 bit 证据：同样只可能是 V1.5，且其余候选保留
TEST(TcpFramingCandidates, ThirtyTwoBitEvidenceKeepsFallbacks) {
    TcpFramingCandidateRequest request;
    request.alignment = PacketAlignment::Bit32;
    const auto candidates = BuildTcpFramingCandidates(request);

    ASSERT_FALSE(candidates.empty());
    EXPECT_EQ(candidates[0].profile, TcpWireProfile::AsamV15);
    EXPECT_EQ(candidates[0].alignment, PacketAlignment::Bit32);
    EXPECT_EQ(candidates.size(), DefaultFramingCandidates().size());
}

/// @brief 任何输入都不得产生重复候选（重复会让同一布局被连两次）
TEST(TcpFramingCandidates, NoDuplicateCandidates) {
    for (const auto alignment : {PacketAlignment::Unknown,
                                 PacketAlignment::Bit8, PacketAlignment::Bit16,
                                 PacketAlignment::Bit32}) {
        TcpFramingCandidateRequest request;
        request.alignment = alignment;
        request.transport_layer_version = 0x0105;
        const auto candidates = BuildTcpFramingCandidates(request);
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            for (std::size_t j = i + 1; j < candidates.size(); ++j) {
                const bool same =
                    candidates[i].profile == candidates[j].profile &&
                    candidates[i].alignment == candidates[j].alignment;
                EXPECT_FALSE(same) << "重复候选：i=" << i << " j=" << j;
            }
        }
    }
}

}  // namespace