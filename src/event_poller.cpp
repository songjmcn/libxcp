/**
 * @file event_poller.cpp
 * @brief EventPoller 的平台无关测试注入与等待转发。
 */

#include "event_poller.hpp"

#include <atomic>

namespace calmcar::xcp::detail {
namespace {

/// @brief 当前测试替代实现；空指针表示使用真实平台实现。
std::atomic<EventPoller*> g_test_poller{nullptr};

}  // namespace

EventPoller& CurrentEventPoller() noexcept {
    EventPoller* const override_poller =
        g_test_poller.load(std::memory_order_acquire);
    if (override_poller != nullptr) {
        return *override_poller;
    }
    return RealEventPoller();
}

void SetEventPollerForTesting(EventPoller* poller) noexcept {
    g_test_poller.store(poller, std::memory_order_release);
}

int WaitAnySocket(std::span<const WaitItem> items, std::uint32_t timeout_ms,
                  std::vector<WaitStatus>& status_out) {
    return CurrentEventPoller().Wait(items, timeout_ms, status_out, nullptr);
}

}  // namespace calmcar::xcp::detail
