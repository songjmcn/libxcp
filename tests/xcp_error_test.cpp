/**
 * @file xcp_error_test.cpp
 * @brief XcpException 与错误分类的单元测试。
 */

#include "libxcp/xcp_error.hpp"

#include <exception>
#include <optional>
#include <string>

#include <gtest/gtest.h>

namespace calmcar::xcp {
namespace {

TEST(XcpException, KeepsStructuredFields) {
    const XcpException ex(ErrorCategory::ProtocolError, "Slave 拒绝访问",
                          CommandCode::Upload, ErrorCode::AccessLocked, 2,
                          "socket closed");

    EXPECT_EQ(ex.category(), ErrorCategory::ProtocolError);
    EXPECT_EQ(ex.commandCode(),
              std::optional<CommandCode>(CommandCode::Upload));
    EXPECT_EQ(ex.errorCode(),
              std::optional<ErrorCode>(ErrorCode::AccessLocked));
    EXPECT_EQ(ex.retryCount(), 2);
    EXPECT_EQ(ex.transportError(), "socket closed");
    EXPECT_STREQ(ex.what(), "Slave 拒绝访问");
}

TEST(XcpException, DefaultsAreEmptyOptionals) {
    const XcpException ex(ErrorCategory::InvalidArgument, "参数非法");
    EXPECT_FALSE(ex.commandCode().has_value());
    EXPECT_FALSE(ex.errorCode().has_value());
    EXPECT_EQ(ex.retryCount(), 0);
    EXPECT_TRUE(ex.transportError().empty());
}

TEST(XcpException, IsCatchableAsRuntimeError) {
    bool caught = false;
    try {
        throw detail::makeTimeout("等待 UPLOAD 响应超时", CommandCode::Upload,
                                  1);
    } catch (const std::runtime_error& e) {
        caught = true;
        const auto* xcp_ex = dynamic_cast<const XcpException*>(&e);
        ASSERT_NE(xcp_ex, nullptr);
        EXPECT_EQ(xcp_ex->category(), ErrorCategory::Timeout);
        EXPECT_EQ(xcp_ex->commandCode(),
                  std::optional<CommandCode>(CommandCode::Upload));
        EXPECT_EQ(xcp_ex->retryCount(), 1);
    }
    EXPECT_TRUE(caught);
}

TEST(ErrorCategoryName, CoversAllCategories) {
    EXPECT_EQ(errorCategoryName(ErrorCategory::InvalidArgument),
              "InvalidArgument");
    EXPECT_EQ(errorCategoryName(ErrorCategory::InvalidState), "InvalidState");
    EXPECT_EQ(errorCategoryName(ErrorCategory::TransportError),
              "TransportError");
    EXPECT_EQ(errorCategoryName(ErrorCategory::Timeout), "Timeout");
    EXPECT_EQ(errorCategoryName(ErrorCategory::MalformedPacket),
              "MalformedPacket");
    EXPECT_EQ(errorCategoryName(ErrorCategory::ProtocolError), "ProtocolError");
    EXPECT_EQ(errorCategoryName(ErrorCategory::UnsupportedFeature),
              "UnsupportedFeature");
    EXPECT_EQ(errorCategoryName(ErrorCategory::RecoveryFailed),
              "RecoveryFailed");
}

TEST(DetailFactories, SetCategoryAndPayload) {
    EXPECT_EQ(detail::makeInvalidArgument("a").category(),
              ErrorCategory::InvalidArgument);
    EXPECT_EQ(detail::makeInvalidState("b").category(),
              ErrorCategory::InvalidState);

    const auto transport_err = detail::makeTransportError("c", "WSAEACCES");
    EXPECT_EQ(transport_err.category(), ErrorCategory::TransportError);
    EXPECT_EQ(transport_err.transportError(), "WSAEACCES");

    const auto malformed = detail::makeMalformedPacket("d");
    EXPECT_EQ(malformed.category(), ErrorCategory::MalformedPacket);

    const auto protocol_err = detail::makeProtocolError(
        "e", CommandCode::Connect, ErrorCode::CmdBusy);
    EXPECT_EQ(protocol_err.category(), ErrorCategory::ProtocolError);
    EXPECT_EQ(protocol_err.commandCode(),
              std::optional<CommandCode>(CommandCode::Connect));
    EXPECT_EQ(protocol_err.errorCode(),
              std::optional<ErrorCode>(ErrorCode::CmdBusy));

    EXPECT_EQ(detail::makeUnsupportedFeature("f").category(),
              ErrorCategory::UnsupportedFeature);

    const auto recovery =
        detail::makeRecoveryFailed("g", CommandCode::Synch, 3);
    EXPECT_EQ(recovery.category(), ErrorCategory::RecoveryFailed);
    EXPECT_EQ(recovery.retryCount(), 3);
}

}  // namespace
}  // namespace calmcar::xcp
