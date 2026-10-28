/**
 * @file event_poller_posix.cpp
 * @brief Linux/macOS 的 poll 多句柄等待实现。
 */

#include "../../event_poller.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <vector>

namespace calmcar::xcp::detail {
namespace {

/**
 * @brief 将当前线程的 poll 错误格式化为可读文本。
 * @return 保存错误码和系统描述的文本。
 */
std::string FormatPollError() {
    const int code = errno;
    return "errno=" + std::to_string(code) + " " + std::strerror(code);
}

/// @brief 通过 POSIX poll 等待多个 Socket 的真实实现。
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

        std::vector<pollfd> fds(items.size());
        for (std::size_t i = 0; i < items.size(); ++i) {
            fds[i].fd = static_cast<int>(items[i].handle);
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

        // poll 的超时参数是 int；维持既有的有符号上限钳制。
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
    }
};

}  // namespace

EventPoller& RealEventPoller() {
    static RealEventPollerImpl instance;
    return instance;
}

}  // namespace calmcar::xcp::detail
