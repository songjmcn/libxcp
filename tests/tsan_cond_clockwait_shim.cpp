// GCC 11 的 ThreadSanitizer 误报规避：把 pthread_cond_clockwait 转发到
// pthread_cond_timedwait。
//
// 背景
// ----
// glibc 2.30 起提供 pthread_cond_clockwait()，libstdc++ 11 在可用时会用它实现
// std::condition_variable::wait_for()/wait_until()。GCC 11 自带的 libtsan
// **没有拦截这个函数**，因此「等待期间释放互斥量、醒来后重新获取」没有被建模，
// 任何「mutex + condition_variable」的正确代码都会被报成
// `double lock of a mutex` / `data race`（最小复现见
// code-plan/TSan报错排查_GCClibtsan误报结论.md）。
//
// 处置
// ----
// 在 TSan + GCC(<12) 的测试可执行文件里定义同名符号，先算出相对剩余时长，
// 再用 **CLOCK_REALTIME** 的 now + 剩余时长构造死线，转调 pthread_cond_timedwait()；
// libtsan 对该函数有完整拦截，于是互斥量释放/重取被正确建模，误报消失。
//
// 为什么是 CLOCK_REALTIME 而不是 clock_id
// ----
// pthread_cond_timedwait **不接收 clock 参数**，它解释 abstime 时只用条件变量
// 创建时绑定的那个时钟。而调用方传来的 clock_id 与 cond 的绑定时钟**可以不同**：
//
//   场景                                传入 clock_id    cond 绑定时钟
//   libstdc++ wait_until(steady_clock)    REALTIME         MONOTONIC
//   原生 pthread / Boost.Asio（若出现）   MONOTONIC        MONOTONIC
//
// libstdc++ 在 condition_variable:200 已经把 steady_clock 死线换算成
// **CLOCK_REALTIME 绝对时刻**再调用 clockwait，所以按 REALTIME 取 now 造死线
// 与它传入的 abstime 基准一致，是正确路径；反过来按 clock_id 取 now 会得到
// ~0 的剩余时长，让 wait_until 立即返回（测试变绿但语义已坏，即假绿）。
//
// 约束（必须读，改动本文件前先看这一节）
// ----
// - 仅在 Linux/glibc 上生效：其它平台编译为空翻译单元。
// - 仅由 tests/CMakeLists.txt 在 `LIBXCP_SANITIZER=thread` 且 GCC < 12 时加入
//   libxcp_tests，不进入库本体，也不影响普通构建。
// - **已知边界**：若出现「MONOTONIC 绑定的 condvar + MONOTONIC 死线」，
//   本 shim 会把 REALTIME 死线当成"MONOTONIC 轴上十几亿秒之后"而永久挂起
//   （实测 REALTIME 与 MONOTONIC 相差约 17.95 亿秒，见
//   docs/review/TSAN-RECHECK-PR25.md）。当前仓库 4 处 wait_until 全部经
//   libstdc++（传 REALTIME），触发不到；一旦命中，CI 会以超时方式显式失败，
//   不会静默通过。彻底根治需要 hook pthread_cond_init 维护 cond→clock 映射，
//   glibc 不导出可转调的 __pthread_cond_init，本轮不做。
// - 见 tests/tsan_cond_clockwait_shim_test.cpp 的 `SteadyClockWaitActuallyWaits`：
//   它守住"不得假绿"这条线。

// __GLIBC__ 由 <features.h> 定义，必须在条件编译之前先包含它，
// 否则下面的 #if 永远为假（本文件曾因此被编译成空翻译单元，shim 静默失效）。
#if defined(__linux__)
#include <features.h>
#endif

#if defined(__linux__) && defined(__GLIBC__) && !defined(__ANDROID__)

#include <pthread.h>
#include <time.h>

// EINVAL 来自 <errno.h>；<pthread.h> 不保证间接引入它。
#include <errno.h>

namespace {

/// @brief 取指定时钟的当前时刻；失败时返回 false。
bool ReadClock(clockid_t clock_id, struct timespec& out) noexcept {
    return ::clock_gettime(clock_id, &out) == 0;
}

}  // namespace

extern "C" {

/// @brief 用 pthread_cond_timedwait 实现 pthread_cond_clockwait 的语义。
/// @param cond 条件变量。
/// @param mutex 调用方已持有的互斥量；本函数在内部等待期间会释放并重新获取它。
/// @param clock_id 计算 @p abstime 所用的时钟。
/// @param abstime 以 @p clock_id 为基准的绝对超时时刻。
/// @return 与 pthread_cond_timedwait 相同的返回码（0 / ETIMEDOUT / EINVAL 等）。
int pthread_cond_clockwait(pthread_cond_t* cond, pthread_mutex_t* mutex,
                           clockid_t clock_id,
                           const struct timespec* abstime) noexcept {
    struct timespec base{};
    if (abstime == nullptr || !ReadClock(clock_id, base)) {
        // 拿不到基准时钟：此时 abstime 的相对时长无法计算。
        // 直接返回 EINVAL，让错误在测试里立刻暴露，而不是静默变成死等。
        return EINVAL;
    }

    // 先算出「以 clock_id 为基准」的相对剩余时长（相对时长跨时钟可移植）。
    long long delta_ns =
        static_cast<long long>(abstime->tv_sec - base.tv_sec) * 1000000000LL +
        (static_cast<long long>(abstime->tv_nsec) - base.tv_nsec);
    if (delta_ns < 0) {
        delta_ns = 0;  // 已过期：立即超时，与 clockwait 语义一致
    }

    // 死线用 CLOCK_REALTIME 构造：libstdc++ 传入的 abstime 已经是 REALTIME
    // 绝对时刻（condition_variable:200），二者基准一致。
    // 用 clock_id 取 now 会让 steady_clock 路径的 delta≈0，造成假绿。
    struct timespec realtime_now{};
    if (!ReadClock(CLOCK_REALTIME, realtime_now)) {
        return EINVAL;
    }

    struct timespec deadline = realtime_now;
    deadline.tv_sec += static_cast<time_t>(delta_ns / 1000000000LL);
    deadline.tv_nsec += static_cast<long>(delta_ns % 1000000000LL);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    return ::pthread_cond_timedwait(cond, mutex, &deadline);
}

}  // extern "C"

#else

// 非 Linux/glibc 平台：libtsan 的该缺陷不适用，保留空翻译单元以便统一编排。
namespace {
[[maybe_unused]] constexpr int kTsanCondClockwaitShimUnused = 0;
}  // namespace

#endif