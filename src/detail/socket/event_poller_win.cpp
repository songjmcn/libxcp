/**
 * @file event_poller_win.cpp
 * @brief Windows 的 WSAPoll 多句柄等待和独立 Winsock 生命周期。
 */

#include "../../event_poller.hpp"

#include <algorithm>
#include <vector>

// WSAPoll 自 Vista 起提供，工程其他文件未规定 SDK 的最低版本。
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

namespace calmcar::xcp::detail {
namespace {

/**
 * @brief 等待实现专用的 Winsock 生命周期守卫。
 * @details 与 SocketOps 的初始化独立计数，保证先使用 EventPoller 时也可等待。
 */
class WinsockGuard {
public:
    /// @brief 启动 Winsock，失败时保留 WSAStartup 返回的错误码。
    WinsockGuard() {
        const int rc = ::WSAStartup(MAKEWORD(2, 2), &m_data);
        m_ok = rc == 0;
        if (!m_ok) {
            m_error = rc;
        }
    }

    /// @brief 仅在启动成功时配平 Winsock 引用计数。
    ~WinsockGuard() {
        if (m_ok) {
            ::WSACleanup();
        }
    }

    WinsockGuard(const WinsockGuard&) = delete;
    WinsockGuard& operator=(const WinsockGuard&) = delete;

    /// @brief 返回本实例的 Winsock 启动状态。
    [[nodiscard]] bool ok() const noexcept { return m_ok; }

    /// @brief 返回启动失败时的错误码。
    [[nodiscard]] int error() const noexcept { return m_error; }

private:
    WSADATA m_data{};  ///< Winsock 版本信息。
    bool m_ok{false};  ///< 初始化是否成功。
    int m_error{0};    ///< 初始化失败时的错误码。
};

/**
 * @brief 将当前线程的 WSAPoll 错误格式化为可读文本。
 * @return 带 WSA 错误码的文本。
 */
std::string FormatPollError() {
    const int code = static_cast<int>(::WSAGetLastError());
    return "WSA error=" + std::to_string(code);
}

/// @brief 通过 Windows WSAPoll 等待多个 Socket 的真实实现。
class RealEventPollerImpl final : public EventPoller {
public:
    /**
     * @brief 等待关注的读写事件并按输入顺序填充结果。
     * @param items 等待项。
     * @param timeout_ms 最长等待毫秒数。
     * @param status_out 与输入一一对应的事件结果。
     * @param error_out 等待失败时接收错误文本，可为空。
     * @return 1 表示至少一项就绪，0 表示超时或空集合，-1 表示调用失败。
     */
    int Wait(std::span<const WaitItem> items, std::uint32_t timeout_ms,
             std::vector<WaitStatus>& status_out,
             std::string* error_out) override {
        status_out.assign(items.size(), WaitStatus{});
        if (items.empty()) {
            return 0;
        }
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

        // WSAPoll 的 timeout 是 INT；保持原有有符号上限钳制。
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
    }

private:
    WinsockGuard m_winsock;  ///< 保证 WSAPoll 前独立完成 Winsock 初始化。
};

}  // namespace

EventPoller& RealEventPoller() {
    static RealEventPollerImpl instance;
    return instance;
}

}  // namespace calmcar::xcp::detail
