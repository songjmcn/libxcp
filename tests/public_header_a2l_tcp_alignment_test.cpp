/**
 * @file public_header_a2l_tcp_alignment_test.cpp
 * @brief 头自足性检查：`libxcp/a2l_tcp_alignment.hpp` 必须能被单独包含。
 *
 * 本 TU **首先**包含待检头，之前不引入任何头（含 gtest）。若该头漏掉自身所需的
 * 标准库或库内依赖，编译期即失败——这类缺陷在"恰好先包含了别的头"的翻译单元里
 * 会被掩盖，只有单独包含才能暴露。
 */

#include "libxcp/a2l_tcp_alignment.hpp"

#include <gtest/gtest.h>

namespace {

/// @brief 默认构造的候选结构必须是"全部字段未声明"的确定状态。
TEST(PublicHeaderA2lTcpAlignmentTest, DefaultConstructedCandidateIsEmpty) {
    const calmcar::xcp::A2lTcpCandidate candidate;
    EXPECT_EQ(candidate.version, 0U);
    EXPECT_EQ(candidate.port, 0U);
    EXPECT_FALSE(candidate.alignment.has_value());
    EXPECT_FALSE(candidate.host.has_value());
    EXPECT_FALSE(candidate.address.has_value());
}

}  // namespace