/**
 * @file tsan_cond_clockwait_shim_test.cpp
 * @brief 验证 TSan shim（tests/tsan_cond_clockwait_shim.cpp）不改变等待语义。
 *
 * 被测对象是**工程补丁**而不是 libxcp 功能：GCC 11 的 libtsan 未拦截
 * `pthread_cond_clockwait`，本仓库用同名 shim 转发到 `pthread_cond_timedwait`
 * 以消除误报。shim 一旦行为不端，TSan 门禁的结论就不可信。
 *
 * 断言只覆盖两条对用户可见的承诺：
 * 1. **不挂起**：死线到期必须返回，实测耗时有上限；
 * 2. **不提前返回**：死线未到且无人通知时必须真的阻塞——否则所有"等超时"
 *    的测试都会瞬间通过且不报错，这是最坏的失效方式（假绿）。
 *
 * 用例使用 std::condition_variable，因为它才是 shim 要适配的真实调用方：
 * libstdc++ 11 构造 condvar 时绑定 CLOCK_MONOTONIC，而
 * `wait_until(steady_clock)` 传给 clockwait 的 abstime 是 CLOCK_REALTIME
 * 绝对时刻（/usr/include/c++/11/condition_variable:72-75 与 :200 可证）。
 *
 * 完整说明见 docs/tsan_cond_clockwait_shim_测试说明.md。
 * 仅当 shim 真正参与编译（Linux/glibc）时才注册用例，其它平台为空翻译单元。
 */

#include <gtest/gtest.h>
#include <pthread.h>
#include <time.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#if defined(__linux__)
#include <features.h>
#endif

#if defined(__linux__) && defined(__GLIBC__)

namespace calmcar::xcp {
namespace {

/// @brief 等待宽限上限（毫秒）：超过即判定为挂起。
constexpr long long kHangLimitMs = 500;

/// @brief 目标等待时长（毫秒）。
constexpr int kWaitMs = 80;

/// @brief 真实经过的毫秒数。
long long ElapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}

/**
 * @brief steady_clock 死线必须真的等满，不得立即返回。
 *
 * 这是"假绿"回归点：若 shim 用 clock_id(=REALTIME) 取 now 构造死线，
 * delta 会恒为 ~0，wait_until 立刻返回，本用例的上限断言会失败。
 */
TEST(TsanCondClockwaitShim, SteadyClockWaitActuallyWaits) {
    std::mutex mutex;
    std::condition_variable cv;
    std::unique_lock<std::mutex> lock(mutex);

    const auto start = std::chrono::steady_clock::now();
    const auto status =
        cv.wait_until(lock, std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs));
    const long long elapsed = ElapsedMs(start);

    EXPECT_EQ(status, std::cv_status::timeout);
    EXPECT_GE(elapsed, kWaitMs / 2);  // 真的等待过
    EXPECT_LT(elapsed, kHangLimitMs);  // 没有挂起
}

/**
 * @brief 有信号时必须被唤醒，不得等满。
 */
TEST(TsanCondClockwaitShim, SteadyClockWaitWakesOnSignal) {
    std::mutex mutex;
    std::condition_variable cv;
    std::unique_lock<std::mutex> lock(mutex);

    std::thread notifier([&cv] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        cv.notify_one();
    });

    const auto start = std::chrono::steady_clock::now();
    const auto status = cv.wait_until(
        lock, std::chrono::steady_clock::now() + std::chrono::milliseconds(kHangLimitMs));
    const long long elapsed = ElapsedMs(start);
    lock.unlock();

    EXPECT_EQ(status, std::cv_status::no_timeout);
    EXPECT_LT(elapsed, kWaitMs);  // 被唤醒而不是等满
    notifier.join();
}

/**
 * @brief system_clock 死线（libstdc++ 传 CLOCK_REALTIME）也必须真的等满。
 */
TEST(TsanCondClockwaitShim, SystemClockWaitActuallyWaits) {
    std::mutex mutex;
    std::condition_variable cv;
    std::unique_lock<std::mutex> lock(mutex);

    const auto start = std::chrono::steady_clock::now();
    const auto status =
        cv.wait_until(lock, std::chrono::system_clock::now() + std::chrono::milliseconds(kWaitMs));
    const long long elapsed = ElapsedMs(start);

    EXPECT_EQ(status, std::cv_status::timeout);
    EXPECT_GE(elapsed, kWaitMs / 2);
    EXPECT_LT(elapsed, kHangLimitMs);
}

/**
 * @brief 已过期的死线必须立即超时，不得挂起。
 */
TEST(TsanCondClockwaitShim, ExpiredDeadlineReturnsImmediately) {
    std::mutex mutex;
    std::condition_variable cv;
    std::unique_lock<std::mutex> lock(mutex);

    const auto start = std::chrono::steady_clock::now();
    const auto status = cv.wait_until(lock, std::chrono::steady_clock::now() - std::chrono::seconds(5));
    const long long elapsed = ElapsedMs(start);

    EXPECT_EQ(status, std::cv_status::timeout);
    EXPECT_LT(elapsed, kHangLimitMs);
}

/**
 * @brief wait_for 走同一条 clockwait 路径，也必须真的等满。
 */
TEST(TsanCondClockwaitShim, WaitForActuallyWaits) {
    std::mutex mutex;
    std::condition_variable cv;
    std::unique_lock<std::mutex> lock(mutex);

    const auto start = std::chrono::steady_clock::now();
    const auto status = cv.wait_for(lock, std::chrono::milliseconds(kWaitMs));
    const long long elapsed = ElapsedMs(start);

    EXPECT_EQ(status, std::cv_status::timeout);
    EXPECT_GE(elapsed, kWaitMs / 2);
    EXPECT_LT(elapsed, kHangLimitMs);
}

}  // namespace
}  // namespace calmcar::xcp

#endif  // defined(__linux__) && defined(__GLIBC__)
