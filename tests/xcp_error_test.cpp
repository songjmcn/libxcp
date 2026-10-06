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

    EXPECT_EQ(ex.Category(), ErrorCategory::ProtocolError);
    EXPECT_EQ(ex.GetCommandCode(),
              std::optional<CommandCode>(CommandCode::Upload));
    EXPECT_EQ(ex.GetErrorCode(),
              std::optional<ErrorCode>(ErrorCode::AccessLocked));
    EXPECT_EQ(ex.RetryCount(), 2);
    EXPECT_EQ(ex.TransportError(), "socket closed");
    EXPECT_STREQ(ex.what(), "Slave 拒绝访问");
}

TEST(XcpException, DefaultsAreEmptyOptionals) {
    const XcpException ex(ErrorCategory::InvalidArgument, "参数非法");
    EXPECT_FALSE(ex.GetCommandCode().has_value());
    EXPECT_FALSE(ex.GetErrorCode().has_value());
    EXPECT_EQ(ex.RetryCount(), 0);
    EXPECT_TRUE(ex.TransportError().empty());
}

TEST(XcpException, IsCatchableAsRuntimeError) {
    bool caught = false;
    try {
        throw detail::MakeTimeout("等待 UPLOAD 响应超时", CommandCode::Upload,
                                  1);
    } catch (const std::runtime_error& e) {
        caught = true;
        const auto* xcp_ex = dynamic_cast<const XcpException*>(&e);
        ASSERT_NE(xcp_ex, nullptr);
        EXPECT_EQ(xcp_ex->Category(), ErrorCategory::Timeout);
        EXPECT_EQ(xcp_ex->GetCommandCode(),
                  std::optional<CommandCode>(CommandCode::Upload));
        EXPECT_EQ(xcp_ex->RetryCount(), 1);
    }
    EXPECT_TRUE(caught);
}

TEST(ErrorCategoryName, CoversAllCategories) {
    EXPECT_EQ(ErrorCategoryName(ErrorCategory::InvalidArgument),
              "InvalidArgument");
    EXPECT_EQ(ErrorCategoryName(ErrorCategory::InvalidState), "InvalidState");
    EXPECT_EQ(ErrorCategoryName(ErrorCategory::TransportError),
              "TransportError");
    EXPECT_EQ(ErrorCategoryName(ErrorCategory::Timeout), "Timeout");
    EXPECT_EQ(ErrorCategoryName(ErrorCategory::MalformedPacket),
              "MalformedPacket");
    EXPECT_EQ(ErrorCategoryName(ErrorCategory::ProtocolError), "ProtocolError");
    EXPECT_EQ(ErrorCategoryName(ErrorCategory::UnsupportedFeature),
              "UnsupportedFeature");
    EXPECT_EQ(ErrorCategoryName(ErrorCategory::RecoveryFailed),
              "RecoveryFailed");
    EXPECT_EQ(ErrorCategoryName(ErrorCategory::OperationOutcomeUnknown),
              "OperationOutcomeUnknown");
}

TEST(XcpException, KeepsOperationOutcomeUnknownCategory) {
    const XcpException ex(ErrorCategory::OperationOutcomeUnknown,
                          "MODIFY_BITS outcome unknown",
                          CommandCode::ModifyBits);
    EXPECT_EQ(ex.Category(), ErrorCategory::OperationOutcomeUnknown);
    EXPECT_EQ(ex.GetCommandCode(),
              std::optional<CommandCode>(CommandCode::ModifyBits));
    EXPECT_FALSE(ex.GetErrorCode().has_value());
}

TEST(DetailFactories, SetCategoryAndPayload) {
    EXPECT_EQ(detail::MakeInvalidArgument("a").Category(),
              ErrorCategory::InvalidArgument);
    EXPECT_EQ(detail::MakeInvalidState("b").Category(),
              ErrorCategory::InvalidState);

    const auto transport_err = detail::MakeTransportError("c", "WSAEACCES");
    EXPECT_EQ(transport_err.Category(), ErrorCategory::TransportError);
    EXPECT_EQ(transport_err.TransportError(), "WSAEACCES");

    const auto malformed = detail::MakeMalformedPacket("d");
    EXPECT_EQ(malformed.Category(), ErrorCategory::MalformedPacket);

    const auto protocol_err = detail::MakeProtocolError(
        "e", CommandCode::Connect, ErrorCode::CmdBusy);
    EXPECT_EQ(protocol_err.Category(), ErrorCategory::ProtocolError);
    EXPECT_EQ(protocol_err.GetCommandCode(),
              std::optional<CommandCode>(CommandCode::Connect));
    EXPECT_EQ(protocol_err.GetErrorCode(),
              std::optional<ErrorCode>(ErrorCode::CmdBusy));

    EXPECT_EQ(detail::MakeUnsupportedFeature("f").Category(),
              ErrorCategory::UnsupportedFeature);

    const auto recovery =
        detail::MakeRecoveryFailed("g", CommandCode::Synch, 3);
    EXPECT_EQ(recovery.Category(), ErrorCategory::RecoveryFailed);
    EXPECT_EQ(recovery.RetryCount(), 3);
}

}  // namespace
}  // namespace calmcar::xcp
