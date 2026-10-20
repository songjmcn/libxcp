/**
 * @file logging_test.cpp
 * @brief 验证日志宏的格式调用与关闭态参数求值约束。
 */

#include <stdexcept>

#include <gtest/gtest.h>

#include "libxcp/log.hpp"

TEST(LoggingMacrosTest, LoggingCallsDoNotEscapeExceptions) {
    int evaluated = 0;
    const auto throwing_value = [&evaluated]() -> int {
        ++evaluated;
        throw std::runtime_error("测试参数异常");
    };
    (void)throwing_value;

    LIBXCP_LOG_INFO("info value={}", throwing_value());
    LIBXCP_LOG_WARNING("warning value={}", throwing_value());
    LIBXCP_LOG_ERROR("error value={}", throwing_value());

#if LIBXCP_ENABLE_LOG
    EXPECT_EQ(evaluated, 3);
#else
    EXPECT_EQ(evaluated, 0);
#endif
}
