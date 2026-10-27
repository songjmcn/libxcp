/**
 * @file event_poller.hpp
 * @brief internal 跨平台**多句柄等待**抽象（POSIX `poll` / Windows
 * `WSAPoll`）。
 *
 * 依据 code-plan/Platform接口分层_实施计划_EventPoller与SocketHandle.md §2
 * 实现。
 *
 * @par 为什么需要它
 *   改造前 `socket_util.cpp` 里有两份同名 `waitFor()`：Windows 用 `select`、
 *   POSIX 用 `poll`。`select` 受 `FD_SETSIZE`（默认 64）限制，且 UDP 接收循环
 *   （`src/udp_transport.cpp`）要同时等"数据句柄 + 唤醒句柄"，POSIX 侧还被写成
 *   `select(FD_SETSIZE, ...)`——首参本应是 `maxfd+1`。把等待收敛到一个抽象后：
 *   - 平台差异分别封装在 `detail/socket/event_poller_win.cpp` 与
 *     `detail/socket/event_poller_posix.cpp`；
 *   - 多句柄等待成为**一等能力**，不再借用 `fd_set`；
 *   - 等待原语可被测试注入（`SetEventPollerForTesting`）。
 *
 * @note 本文件位于 `src/`，属于**内部实现**头，不对外安装。
 */

#ifndef CALMCAR_XCP_EVENT_POLLER_HPP_
#define CALMCAR_XCP_EVENT_POLLER_HPP_

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "detail/socket/socket_ops.hpp"

namespace calmcar::xcp::detail {

/// @brief 单个等待项：目标句柄与关注的事件。
struct WaitItem {
    SocketHandle handle{kInvalidSocketHandle};  ///< 目标句柄
    bool readable{false};                       ///< 关注可读
    bool writable{false};                       ///< 关注可写
};

/// @brief 单个等待项的结果。
struct WaitStatus {
    bool readable{false};  ///< 该句柄当前可读
    bool writable{false};  ///< 该句柄当前可写
    bool failed{false};    ///< 该句柄出现错误/挂断（POLLERR/POLLHUP/POLLNVAL）

    /// @brief 是否"就绪"（可读、可写或出错都算，调用方随后用真实 syscall 定性）
    [[nodiscard]] bool Ready() const noexcept {
        return readable || writable || failed;
    }
};

/**
 * @brief 多句柄等待抽象。
 *
 * @details 实现必须满足与既有 `SocketOps::wait_readable/wait_writable`
 * 相同的三态约定： `1` 至少一个句柄就绪、`0` 超时、`-1` 失败（错误文本写入
 * `error_out`）。
 */
class EventPoller {
public:
    /// @brief 虚析构，允许测试派生
    virtual ~EventPoller() = default;

    /**
     * @brief 等待任意句柄进入关注状态
     * @param items 等待项列表（可为空；空集合立即返回 0）
     * @param timeout_ms 超时（毫秒）
     * @param status_out 输出：与 `items` 等长的结果数组（本函数会先 `assign`）
     * @param error_out 失败时的错误文本（可为 `nullptr`）
     * @return 1 就绪、0 超时、-1 失败
     */
    virtual int Wait(std::span<const WaitItem> items, std::uint32_t timeout_ms,
                     std::vector<WaitStatus>& status_out,
                     std::string* error_out) = 0;
};

/**
 * @brief 取得生产环境的等待实现（进程内单例）
 * @return 真实平台实现的引用，永不为空
 */
[[nodiscard]] EventPoller& RealEventPoller();

/**
 * @brief 取当前生效的等待实现
 * @return 测试注入的实现（若已注入），否则与 `RealEventPoller()` 相同
 */
[[nodiscard]] EventPoller& CurrentEventPoller() noexcept;

/**
 * @brief 仅供测试：替换进程内等待实现
 * @param poller 假实现指针；传 `nullptr` 恢复真实实现
 *
 * @warning 只能由测试代码调用。本模块只借用 `poller`（不拥有、不删除），
 *          调用方必须保证其生命周期长于注入期间的所有传输对象。
 */
void SetEventPollerForTesting(EventPoller* poller) noexcept;

/**
 * @brief 等待任意句柄就绪（走当前生效的 `EventPoller`）
 * @param items 等待项列表
 * @param timeout_ms 超时（毫秒）
 * @param status_out 输出：与 `items` 等长的结果数组
 * @return 1 就绪、0 超时、-1 失败
 */
int WaitAnySocket(std::span<const WaitItem> items, std::uint32_t timeout_ms,
                  std::vector<WaitStatus>& status_out);

}  // namespace calmcar::xcp::detail

#endif  // CALMCAR_XCP_EVENT_POLLER_HPP_
