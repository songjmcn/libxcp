/**
 * @file public_header_transport_selection_test.cpp
 * @brief 头自足性检查：`libxcp/transport_selection.hpp` 必须能被单独包含。
 *
 * 本 TU **首先**包含待检头，之前不引入任何头（含 gtest）；该头若依赖别的头
 * 先行提供声明，编译会失败。
 */

#include "libxcp/transport_selection.hpp"

#include <gtest/gtest.h>

namespace {

/// @brief 默认请求必须是 UDP（保持既有行为，§8.1 规则 9 的编译期体现）。
TEST(PublicHeaderTransportSelectionTest, DefaultRequestIsUdp) {
    const calmcar::xcp::TransportSelectionRequest request;
    EXPECT_EQ(request.transport, calmcar::xcp::TransportKind::Udp);
    EXPECT_FALSE(request.tcp_profile.has_value());
    EXPECT_FALSE(request.tcp_profile_auto);
    EXPECT_FALSE(request.tcp_profile_auto_legacy);
}

}  // namespace