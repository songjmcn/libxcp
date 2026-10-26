/**
 * @file tcp_fake_socket.hpp
 * @brief 脚本化假 Socket 操作表：让 partial write / EINTR / EAGAIN / 连接重置
 *        这些"真实 Socket 上不可确定性复现"的错误分支变成可复现的用例。
 *
 * 依据 `code-plan/历史遗漏项_详细方案.md` §1.3（L-A2）实现。
 *
 * @par 为什么不直接用真实 Socket
 *   - partial write：真实 TCP 的内核发送缓冲通常一次吞下小报文，无法稳定制造
 *     短写；
 *   - EINTR：依赖信号到达时机；
 *   - EAGAIN：依赖对端停止读取与缓冲水位，且真等下去会拖慢测试；
 *   - ECONNRESET：需要精确控制对端 RST 时机。
 *
 * @par 设计取舍：默认转发，只脚本化必要的方法
 *   本假实现**不**重写整套 Socket 生命周期（那会让测试失去"真实连接"这一
 *   前提），而是默认把每次调用转发给 `RealSocketOps()`，仅对测试显式脚本化
 *   的方法（`send` / `recv` / `wait_writable`）按脚本行事。
 *   于是连接建立、端口分配、关闭语义仍然是真实的，被替换的只有错误注入点。
 *
 * @warning 注入是**进程级**的（见 `SetSocketOpsForTesting()`）：使用本头文件的
 *          测试必须独占进程，对应 ctest 用例已设置 `RUN_SERIAL`。
 */

#ifndef CALMCAR_XCP_TESTS_TCP_FAKE_SOCKET_HPP_
#define CALMCAR_XCP_TESTS_TCP_FAKE_SOCKET_HPP_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "src/socket_util.hpp"

namespace calmcar::xcp::test_support {

/**
 * @brief 脚本化的 `SocketOps` 实现
 * @details 未脚本化的方法一律转发给 `detail::RealSocketOps()`。
 */
class FakeSocketOps final : public detail::SocketOps {
public:
    /// @brief `send` 的一次脚本动作
    enum class SendAction : std::uint8_t {
        PassThrough,         ///< 原样转发给真实实现
        TruncateTo,          ///< 只发出前 N 字节（真实短写），N 由 `bytes` 给出
        FailInterrupted,     ///< 返回 -1 且判定为 EINTR
        FailWouldBlock,      ///< 返回 -1 且判定为 EAGAIN
        FailConnectionLost,  ///< 返回 -1 且判定为连接已断
    };

    /// @brief `recv` 的一次脚本动作
    enum class ReceiveAction : std::uint8_t {
        PassThrough,         ///< 原样转发给真实实现
        FailInterrupted,     ///< 返回 -1 且判定为 EINTR
        FailConnectionLost,  ///< 返回 -1 且判定为连接已断
    };

    /// @brief 一条 `send` 脚本
    struct SendStep {
        SendAction action{SendAction::PassThrough};  ///< 动作
        int bytes{0};                                ///< TruncateTo 的字节数
    };

    FakeSocketOps() = default;

    /// @brief 依次压入若干 `send` 脚本（脚本耗尽后自动转回转发模式）
    void ScriptSend(std::initializer_list<SendStep> steps) {
        for (const SendStep& step : steps) {
            m_send_script_.push_back(step);
        }
    }

    /// @brief 压入一条 `recv` 脚本
    void ScriptReceive(ReceiveAction action) {
        m_recv_script_.push_back(action);
    }

    /// @brief 覆盖 `wait_writable` 的返回值（1 就绪 / 0 超时 / -1 错误）
    void SetWaitWritableResult(int result) noexcept {
        m_wait_writable_override_ = result;
    }

    /// @brief 已发生的 `send` 调用次数（含被脚本消费的与转发的）
    [[nodiscard]] std::size_t SendCallCount() const noexcept {
        return m_send_calls_;
    }

    /// @brief 剩余未消费的 `send` 脚本条数
    [[nodiscard]] std::size_t PendingSendSteps() const noexcept {
        return m_send_script_.size();
    }

    /// @brief 已发生的 `recv` 调用次数
    [[nodiscard]] std::size_t ReceiveCallCount() const noexcept {
        return m_recv_calls_;
    }

    // ---- SocketOps 覆盖：仅 send / recv / wait_writable / 错误分类 ----

    /// @brief 按脚本行事；脚本耗尽后转发真实实现
    int send(detail::SocketHandle handle, const std::uint8_t* data,
             std::size_t size, bool no_signal) override {
        ++m_send_calls_;
        if (m_send_script_.empty()) {
            return detail::RealSocketOps().send(handle, data, size, no_signal);
        }
        const SendStep step = m_send_script_.front();
        m_send_script_.pop_front();
        switch (step.action) {
            case SendAction::PassThrough:
                ClearError();
                return detail::RealSocketOps().send(handle, data, size,
                                                    no_signal);
            case SendAction::TruncateTo: {
                ClearError();
                const std::size_t limit =
                    static_cast<std::size_t>(std::max(step.bytes, 0));
                const std::size_t cut = (std::min)(size, limit);
                return detail::RealSocketOps().send(handle, data, cut,
                                                    no_signal);
            }
            case SendAction::FailInterrupted:
                return FailWith("测试脚本：发送被信号打断",
                                /*interrupted=*/true,
                                /*would_block=*/false, /*lost=*/false);
            case SendAction::FailWouldBlock:
                return FailWith("测试脚本：发送缓冲已满", /*interrupted=*/false,
                                /*would_block=*/true, /*lost=*/false);
            case SendAction::FailConnectionLost:
                return FailWith("测试脚本：连接已被重置", /*interrupted=*/false,
                                /*would_block=*/false, /*lost=*/true);
        }
        return -1;
    }

    /// @brief 按脚本行事；脚本耗尽后转发真实实现
    int recv(detail::SocketHandle handle, std::uint8_t* data,
             std::size_t size) override {
        ++m_recv_calls_;
        if (m_recv_script_.empty()) {
            return detail::RealSocketOps().recv(handle, data, size);
        }
        const ReceiveAction action = m_recv_script_.front();
        m_recv_script_.pop_front();
        switch (action) {
            case ReceiveAction::PassThrough:
                ClearError();
                return detail::RealSocketOps().recv(handle, data, size);
            case ReceiveAction::FailInterrupted:
                return FailWith("测试脚本：接收被信号打断",
                                /*interrupted=*/true,
                                /*would_block=*/false, /*lost=*/false);
            case ReceiveAction::FailConnectionLost:
                return FailWith("测试脚本：连接已被重置", /*interrupted=*/false,
                                /*would_block=*/false, /*lost=*/true);
        }
        return -1;
    }

    /// @brief 若设置了覆盖值则返回它，否则转发真实实现
    int wait_writable(detail::SocketHandle handle,
                      std::uint32_t timeout_ms) override {
        if (m_wait_writable_override_.has_value()) {
            return *m_wait_writable_override_;
        }
        return detail::RealSocketOps().wait_writable(handle, timeout_ms);
    }

    /// @brief 最近一次失败是否为"被信号打断"
    [[nodiscard]] bool is_interrupted() const override {
        return m_interrupted_ ? true : detail::RealSocketOps().is_interrupted();
    }

    /// @brief 最近一次失败是否为"稍后重试"
    [[nodiscard]] bool is_would_block() const override {
        return m_would_block_ ? true : detail::RealSocketOps().is_would_block();
    }

    /// @brief 最近一次失败是否为"连接已断"
    [[nodiscard]] bool is_connection_lost() const override {
        return m_connection_lost_
                   ? true
                   : detail::RealSocketOps().is_connection_lost();
    }

    /// @brief 最近一次失败的文本（脚本注入优先）
    [[nodiscard]] std::string last_error() const override {
        if (!m_last_error_.empty()) {
            return m_last_error_;
        }
        return detail::RealSocketOps().last_error();
    }

    // ---- 其余方法一律转发真实实现 ----

    /// @brief 创建 Socket（转发）
    detail::SocketHandle create(int domain, int type, int protocol) override {
        return detail::RealSocketOps().create(domain, type, protocol);
    }

    /// @brief 关闭 Socket（转发）
    void close(detail::SocketHandle handle) override {
        detail::RealSocketOps().close(handle);
    }

    /// @brief 绑定（转发）
    int bind(detail::SocketHandle handle, const sockaddr* addr,
             socklen_t addr_len) override {
        return detail::RealSocketOps().bind(handle, addr, addr_len);
    }

    /// @brief 查询本地地址（转发）
    int getsockname(detail::SocketHandle handle, sockaddr* addr,
                    socklen_t* addr_len) override {
        return detail::RealSocketOps().getsockname(handle, addr, addr_len);
    }

    /// @brief 名称解析（转发）
    int getaddrinfo(const char* node, const char* service,
                    const addrinfo* hints, addrinfo** result) override {
        return detail::RealSocketOps().getaddrinfo(node, service, hints,
                                                   result);
    }

    /// @brief 释放解析结果（转发）
    void freeaddrinfo(addrinfo* result) override {
        detail::RealSocketOps().freeaddrinfo(result);
    }

    /// @brief 设为非阻塞（转发）
    int set_non_blocking(detail::SocketHandle handle) override {
        return detail::RealSocketOps().set_non_blocking(handle);
    }

    /// @brief 设为阻塞（转发）
    int set_blocking(detail::SocketHandle handle) override {
        return detail::RealSocketOps().set_blocking(handle);
    }

    /// @brief 监听（转发）
    int listen(detail::SocketHandle handle, int backlog) override {
        return detail::RealSocketOps().listen(handle, backlog);
    }

    /// @brief 接受连接（转发）
    detail::SocketHandle accept(detail::SocketHandle handle, sockaddr* addr,
                                socklen_t* addr_len) override {
        return detail::RealSocketOps().accept(handle, addr, addr_len);
    }

    /// @brief 发起连接（转发）
    int connect(detail::SocketHandle handle, const sockaddr* addr,
                socklen_t addr_len) override {
        return detail::RealSocketOps().connect(handle, addr, addr_len);
    }

    /// @brief 读取 SO_ERROR（转发）
    int get_so_error(detail::SocketHandle handle, int* error_out) override {
        return detail::RealSocketOps().get_so_error(handle, error_out);
    }

    /// @brief 设置整数选项（转发）
    int set_int_option(detail::SocketHandle handle, int level, int option,
                       int value) override {
        return detail::RealSocketOps().set_int_option(handle, level, option,
                                                      value);
    }

    /// @brief 半关闭/关闭方向（转发）
    int shutdown_socket(detail::SocketHandle handle) override {
        return detail::RealSocketOps().shutdown_socket(handle);
    }

    /// @brief 等待可读（转发）
    int wait_readable(detail::SocketHandle handle,
                      std::uint32_t timeout_ms) override {
        return detail::RealSocketOps().wait_readable(handle, timeout_ms);
    }

    /// @brief 是否处于"连接正在建立"（转发）
    [[nodiscard]] bool is_in_progress() const override {
        return detail::RealSocketOps().is_in_progress();
    }

private:
    /// @brief 记录一次注入的失败并返回 -1
    int FailWith(std::string error, bool interrupted, bool would_block,
                 bool lost) {
        m_last_error_ = std::move(error);
        m_interrupted_ = interrupted;
        m_would_block_ = would_block;
        m_connection_lost_ = lost;
        return -1;
    }

    /// @brief 清除注入的失败状态（转发前调用，避免污染真实错误判定）
    void ClearError() noexcept {
        m_last_error_.clear();
        m_interrupted_ = false;
        m_would_block_ = false;
        m_connection_lost_ = false;
    }

    std::deque<SendStep> m_send_script_;           ///< 待消费的 send 脚本
    std::deque<ReceiveAction> m_recv_script_;      ///< 待消费的 recv 脚本
    std::optional<int> m_wait_writable_override_;  ///< 可写等待结果覆盖
    std::size_t m_send_calls_{0};                  ///< send 调用次数
    std::size_t m_recv_calls_{0};                  ///< recv 调用次数
    std::string m_last_error_;                     ///< 注入的错误文本
    bool m_interrupted_{false};                    ///< 注入的 EINTR 判定
    bool m_would_block_{false};                    ///< 注入的 EAGAIN 判定
    bool m_connection_lost_{false};                ///< 注入的连接断开判定
};

/**
 * @brief 把 `FakeSocketOps` 安装到进程内的 RAII 守卫
 * @details 构造时安装、析构时恢复真实实现。**必须**在传输对象之前声明，
 *          以保证传输先析构、注入后卸载。
 */
class ScopedFakeSocketOps {
public:
    /// @brief 安装假实现
    explicit ScopedFakeSocketOps(FakeSocketOps& fake) noexcept {
        detail::SetSocketOpsForTesting(&fake);
    }

    /// @brief 卸载假实现（恢复真实 Socket）
    ~ScopedFakeSocketOps() { detail::SetSocketOpsForTesting(nullptr); }

    ScopedFakeSocketOps(const ScopedFakeSocketOps&) = delete;
    ScopedFakeSocketOps& operator=(const ScopedFakeSocketOps&) = delete;
    ScopedFakeSocketOps(ScopedFakeSocketOps&&) = delete;
    ScopedFakeSocketOps& operator=(ScopedFakeSocketOps&&) = delete;
};

}  // namespace calmcar::xcp::test_support

#endif  // CALMCAR_XCP_TESTS_TCP_FAKE_SOCKET_HPP_