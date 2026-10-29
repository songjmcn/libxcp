/**
 * @file public_header_trusted_snapshot_test.cpp
 * @brief 头自足性检查：`libxcp/trusted_a2l_snapshot.hpp` 必须能被单独包含。
 *
 * 本 TU **首先**包含待检头，之前不引入任何头（含 gtest）；该头若漏掉
 * `<filesystem>`/`<memory>` 等自身依赖，编译会失败。
 */

#include "libxcp/trusted_a2l_snapshot.hpp"

#include <gtest/gtest.h>

namespace {

/// @brief 默认结果的语义必须是"不可信"，避免调用方误用未填充的路径。
TEST(PublicHeaderTrustedSnapshotTest, DefaultResultIsUntrusted) {
    const calmcar::xcp::TrustedA2lSnapshot snapshot;
    EXPECT_FALSE(snapshot.trusted);
    EXPECT_TRUE(snapshot.path.empty());
    EXPECT_TRUE(snapshot.bytes.empty());
    EXPECT_EQ(snapshot.guard, nullptr);
}

}  // namespace