/**
 * @file event_poller.cpp
 * @brief `event_poller.hpp` 的平台实现（POSIX `poll` / Windows `WSAPoll`）。
 *
 * @par 平台差异
 *   - POSIX：`::poll()`，`timeout` 为 `int`（钳到 24 天上限）；
 *     可读事件 `POLLIN`、可写 `POLLOUT`、错误 `POLLERR/POLLHUP/POLLNVAL`。
 *   - Windows：`::WSAPoll()`，可读事件 `POLLRDNORM`、可写 `POLLWRNORM`，
 *     错误位同名前缀。相对 `select` 的好处是不受 `FD_SETSIZE` 限制，
 *     且一次可等多个句柄、各自关注不同事件。
 */

#include "event_poller.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <poll.h>
#endif

namespace calmcar::xcp::detail {
namespace {

#if defined(_WIN32)

/**
 * @brief 等待实现专用的 Winsock 生命周期守卫
 *
 * @details `WSAPoll` 要求 Winsock 已初始化。与 `socket_util.cpp` 的
 *          `WinsockSession` 独立计数（`WSAStartup` 本身可重入计数，
 *          只要 Startup/Cleanup 配平即可），这样即使调用方先创建
 *          `EventPoller` 再创建 Transport，也不会出现"未初始化"错误。
 */
class WinsockGuard {
public:
    /// @brief 启动 Winsock（失败时记录错误码）
    WinsockGuard() {
        const int rc = ::WSAStartup(MAKEWORD(2, 2), &m_data);
        m_ok = rc == 0;
        if (!m_ok) {
            m_error = rc;
        }
    }

    /// @brief 清理 Winsock
    ~WinsockGuard() {
        if (m_ok) {
            ::WSACleanup();
        }
    }

    WinsockGuard(const WinsockGuard&) = delete;
    WinsockGuard& operator=(const WinsockGuard&) = delete;

    /// @brief 初始化是否成功
    [[nodiscard]] bool ok() const noexcept { return m_ok; }

    /// @brief 初始化失败时的错误码
    [[nodiscard]] int error() const noexcept { return m_error; }

private:
    WSADATA m_data{};  ///< Winsock 版本信息
    bool m_ok{false};  ///< 初始化结果
    int m_error{0};    ///< 失败时的错误码
};

#endif

/// @brief 把平台错误码格式化为可读文本
std::string FormatPollError() {
#if defined(_WIN32)
    const int code = static_cast<int>(::WSAGetLastError());
    return "WSA error=" + std::to_string(code);
#else
    const int code = errno;
    return "errno=" + std::to_string(code) + " " + std::strerror(code);
#endif
}

/**
 * @brief 真实等待实现
 */
class RealEventPollerImpl final : public EventPoller {
public:
    RealEventPollerImpl() = default;

    int Wait(std::span<const WaitItem> items, std::uint32_t timeout_ms,
             std::vector<WaitStatus>& status_out,
             std::string* error_out) override {
        status_out.assign(items.size(), WaitStatus{});
        if (items.empty()) {
            return 0;
        }
#if defined(_WIN32)
        if (!m_winsock.ok()) {
            if (error_out != nullptr) {
                *error_out = "WSAStartup failed, code=" +
                             std::to_string(m_winsock.error());
            }
            return -1;
        }
        std::vector<WSAPOLLFD> fds(items.size());
        for (std::size_t i = 0; i < items.size(); ++i) {
            fds[i].fd = static_cast<SOCKET>(items[i].handle);
            short events = 0;
            if (items[i].readable) {
                events = static_cast<short>(events | POLLRDNORM);
            }
            if (items[i].writable) {
                events = static_cast<short>(events | POLLWRNORM);
            }
            fds[i].events = events;
            fds[i].revents = 0;
        }
        const auto clamped = std::min<std::uint32_t>(timeout_ms, 0x7FFFFFFFU);
        const int ready = ::WSAPoll(fds.data(), static_cast<ULONG>(fds.size()),
                                    static_cast<INT>(clamped));
        if (ready < 0) {
            if (error_out != nullptr) {
                *error_out = FormatPollError();
            }
            return -1;
        }
        if (ready == 0) {
            return 0;
        }
        for (std::size_t i = 0; i < items.size(); ++i) {
            const short re = fds[i].revents;
            status_out[i].readable = (re & POLLRDNORM) != 0;
            status_out[i].writable = (re & POLLWRNORM) != 0;
            status_out[i].failed = (re & (POLLERR | POLLHUP | POLLNVAL)) != 0;
        }
        return 1;
#else
        std::vector<pollfd> fds(items.size());
        for (std::size_t i = 0; i < items.size(); ++i) {
            fds[i].fd = items[i].handle;
            short events = 0;
            if (items[i].readable) {
                events = static_cast<short>(events | POLLIN);
            }
            if (items[i].writable) {
                events = static_cast<short>(events | POLLOUT);
            }
            fds[i].events = events;
            fds[i].revents = 0;
        }
        // poll 的 timeout 是 int；上限钳制到约 24 天，足以覆盖任何合理配置
        const auto clamped = std::min<std::uint32_t>(timeout_ms, 0x7FFFFFFFU);
        const int ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()),
                                 static_cast<int>(clamped));
        if (ready < 0) {
            if (error_out != nullptr) {
                *error_out = FormatPollError();
            }
            return -1;
        }
        if (ready == 0) {
            return 0;
        }
        for (std::size_t i = 0; i < items.size(); ++i) {
            const short re = fds[i].revents;
            status_out[i].readable = (re & POLLIN) != 0;
            status_out[i].writable = (re & POLLOUT) != 0;
            status_out[i].failed = (re & (POLLERR | POLLHUP | POLLNVAL)) != 0;
        }
        return 1;
#endif
    }

private:
#if defined(_WIN32)
    WinsockGuard m_winsock;  ///< 保证 WSAPoll 前 Winsock 已初始化
#endif
};

/// @brief 测试注入的替代实现；`nullptr` 表示"未注入，使用真实实现"。
std::atomic<EventPoller*> g_test_poller{nullptr};

}  // namespace

EventPoller& RealEventPoller() {
    static RealEventPollerImpl instance;
    return instance;
}

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
