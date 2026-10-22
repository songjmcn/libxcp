/**
 * @file test_main.cpp
 * @brief GoogleTest 自有进程入口及可选文件日志生命周期管理。
 */

#include <cstdio>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#if LIBXCP_ENABLE_LOG
#include "program_logging.hpp"
#endif

namespace {

/// @brief 检查参数是否为 CMake GoogleTest discovery 的测试枚举调用。
bool IsTestDiscoveryInvocation(int argc, char** argv) noexcept {
    constexpr std::string_view kListTests = "--gtest_list_tests";
    constexpr std::string_view kListTestsEquals = "--gtest_list_tests=";
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == kListTests || argument.starts_with(kListTestsEquals)) {
            return true;
        }
    }
    return false;
}

}  // namespace

/**
 * @brief 初始化 GoogleTest，在测试枚举之外可选初始化文件日志并执行用例。
 * @param argc 命令行参数数量。
 * @param argv 命令行参数数组。
 * @return GoogleTest 的用例执行结果或日志启动失败码。
 */
int main(int argc, char** argv) {
    const bool discovery = IsTestDiscoveryInvocation(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);

#if LIBXCP_ENABLE_LOG
    calmcar::xcp::internal::ProgramLogSession logging;
    if (!discovery) {
        std::string error_message;
        if (!logging.Initialize(argv[0], "tests", error_message)) {
            std::fprintf(stderr, "日志初始化失败: %s\n", error_message.c_str());
            return 2;
        }
    }
#endif

    const int result = RUN_ALL_TESTS();
#if LIBXCP_ENABLE_LOG
    logging.Shutdown();
#endif
    return result;
}
