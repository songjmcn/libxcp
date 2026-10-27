/**
 * @file test_main.cpp
 * @brief GoogleTest 自有进程入口及可选文件日志生命周期管理。
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

#if LIBXCP_ENABLE_LOG
#include "program_logging.hpp"
#endif

namespace {

/// @brief 单个用例的看门狗上限：超过即打印用例名并中止进程（默认 240 s）。
constexpr std::chrono::seconds kDefaultWatchdogLimit{240};

/**
 * @brief 读取看门狗上限（秒），支持 `LIBXCP_TEST_WATCHDOG_SECONDS` 覆盖。
 *
 * @return 上限时长；环境变量缺失、非数字或非正数时返回默认值。
 *
 * @details 挂死用例在 CI 上只会表现为“作业卡住”，日志里没有任何用例名，
 *          因此必须由测试进程自己点名；上限取小于 ctest `TIMEOUT` 的值，
 *          这样先由看门狗 abort（带用例名），再被 ctest 记为失败。
 */
std::chrono::seconds ReadWatchdogLimit() noexcept {
    const char* raw = std::getenv("LIBXCP_TEST_WATCHDOG_SECONDS");
    if (raw == nullptr) {
        return kDefaultWatchdogLimit;
    }
    char* end = nullptr;
    const long parsed = std::strtol(raw, &end, 10);
    if (end == raw || parsed <= 0) {
        return kDefaultWatchdogLimit;
    }
    return std::chrono::seconds{parsed};
}

/**
 * @brief 用例级看门狗：单用例超时则打印用例名并中止进程。
 *
 * @details 只做两件事：记录当前用例名与开始时刻（gtest 回调线程），
 *          由后台线程按固定节拍检查是否超时。超时后 `std::abort()`，
 *          让 ctest 立即拿到失败而不是把整份 CI 作业挂到超时。
 */
class WatchdogListener : public ::testing::EmptyTestEventListener {
public:
    /// @param limit 单用例允许的最长运行时间
    explicit WatchdogListener(std::chrono::seconds limit) : limit_(limit) {}

    /// @brief 启动后台检查线程
    void Start() {
        thread_ = std::thread([this] { Run(); });
    }

    /// @brief 停止后台检查线程并等待其退出
    void Stop() {
        stop_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void OnTestStart(const ::testing::TestInfo& info) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        current_ = std::string(info.test_suite_name()) + "." + info.name();
        started_ = std::chrono::steady_clock::now();
    }

    void OnTestEnd(const ::testing::TestInfo& info) override {
        static_cast<void>(info);
        const std::lock_guard<std::mutex> lock(mutex_);
        current_.clear();
    }

private:
    /// @brief 后台循环：每 200 ms 检查一次当前用例是否超时
    void Run() {
        while (!stop_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
            std::string name;
            std::chrono::steady_clock::duration elapsed{};
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                if (current_.empty()) {
                    continue;
                }
                name = current_;
                elapsed = std::chrono::steady_clock::now() - started_;
            }
            if (elapsed < limit_) {
                continue;
            }
            const auto seconds =
                std::chrono::duration_cast<std::chrono::seconds>(elapsed)
                    .count();
            std::fprintf(
                stderr, "\n[ WATCHDOG ] 用例超过 %lld s 仍未结束，已中止: %s\n",
                static_cast<long long>(limit_.count()), name.c_str());
            std::fflush(stderr);
            std::abort();
        }
    }

    std::chrono::seconds limit_;  ///< 单用例上限
    std::mutex mutex_;            ///< 保护 current_/started_
    std::string current_;         ///< 当前正在运行的用例名（空表示不在用例内）
    std::chrono::steady_clock::time_point started_{};  ///< 当前用例开始时刻
    std::atomic<bool> stop_{false};                    ///< 后台线程退出标志
    std::thread thread_;                               ///< 后台检查线程
};

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

    ::testing::TestEventListeners& listeners =
        ::testing::UnitTest::GetInstance()->listeners();
    WatchdogListener* watchdog = nullptr;
    if (!discovery) {
        // gtest 的监听器容器在进程退出时会 delete 所有已 Append 的监听器，
        // 因此这里交出所有权；结束时用 Release 取回再自行释放，避免二次释放。
        watchdog = new WatchdogListener(ReadWatchdogLimit());
        listeners.Append(watchdog);
        watchdog->Start();
    }

    const int result = RUN_ALL_TESTS();

    if (watchdog != nullptr) {
        watchdog->Stop();
        delete listeners.Release(watchdog);
    }
#if LIBXCP_ENABLE_LOG
    logging.Shutdown();
#endif
    return result;
}
