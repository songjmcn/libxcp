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
// 在 TSan + GCC(<12) 的测试可执行文件里定义同名符号，把按指定时钟的超时换算成
// CLOCK_REALTIME 的绝对时刻后转调 pthread_cond_timedwait()；
// libtsan 对该函数有完整拦截，于是互斥量释放/重取被正确建模，误报消失。
//
// 约束
// ----
// - 仅在 Linux/glibc 上生效：其它平台编译为空翻译单元。
// - 仅由 tests/CMakeLists.txt 在 `LIBXCP_SANITIZER=thread` 且 GCC < 12 时加入
//   libxcp_tests，不进入库本体，也不影响普通构建。
// - 语义与 clockwait 一致：超时按调用时给出的时钟计算，已过期则立即返回。

// __GLIBC__ 由 <features.h> 定义，必须在条件编译之前先包含它，
// 否则下面的 #if 永远为假（本文件曾因此被编译成空翻译单元，shim 静默失效）。
#if defined(__linux__)
#include <features.h>
#endif

#if defined(__linux__) && defined(__GLIBC__) && !defined(__ANDROID__)

#include <pthread.h>
#include <time.h>

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
        // 拿不到基准时钟：退回原语，行为由 libc 决定，不吞掉错误。
        return ::pthread_cond_timedwait(cond, mutex, abstime);
    }

    // 先算出相对剩余时间，再换算到 CLOCK_REALTIME，避免两种时钟基准混用。
    long long delta_ns =
        static_cast<long long>(abstime->tv_sec - base.tv_sec) * 1000000000LL +
        (static_cast<long long>(abstime->tv_nsec) - base.tv_nsec);
    if (delta_ns < 0) {
        delta_ns = 0;  // 已过期：立即超时，与 clockwait 语义一致
    }

    struct timespec realtime_now{};
    if (!ReadClock(CLOCK_REALTIME, realtime_now)) {
        return ::pthread_cond_timedwait(cond, mutex, abstime);
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
