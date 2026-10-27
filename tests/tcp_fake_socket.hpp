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
 *   的方法（`send` / `recv` / `send_to` / `recv_from` / `wait_writable` /
 *   `wait_readable`）按脚本行事。
 *   于是连接建立、端口分配、关闭语义仍然是真实的，被替换的只有错误注入点。
 *   除错误注入外，本实现还统计每个句柄的 `close()` 次数与最近一次
 *   `listen()`/`accept()` 的句柄，用于验证"监听句柄只关闭一次"。
 *
 * @warning 注入是**进程级**的（见 `SetSocketOpsForTesting()`）：使用本头文件的
 *          测试必须独占进程，对应 ctest 用例已设置 `RUN_SERIAL`。
 */

#ifndef CALMCAR_XCP_TESTS_TCP_FAKE_SOCKET_HPP_
#define CALMCAR_XCP_TESTS_TCP_FAKE_SOCKET_HPP_

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
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

    /// @brief `recv_from` 的一次脚本动作
    /// @details UDP 接收分支按"超时 / 中断 / 失效 / 其它"分类，其中前两类与
    ///          "其它"的判定来自平台错误码。真实 Socket 上无法稳定制造这些
    ///          错误码，因此脚本动作同时决定返回值与分类判定。
    enum class RecvFromAction : std::uint8_t {
        PassThrough,         ///< 原样转发给真实实现
        DeliverZeroLength,   ///< 返回 0（空 Datagram）
        FailTimeout,         ///< 返回 -1 且判定为接收超时
        FailInterrupted,     ///< 返回 -1 且判定为 EINTR
        FailInvalidSocket,   ///< 返回 -1 且判定为句柄失效
        FailConnectionLost,  ///< 返回 -1 且判定为连接已断
        FailOther,           ///< 返回 -1 且不属于上述任何分类
    };

    /// @brief `send_to` 的一次脚本动作
    enum class SendToAction : std::uint8_t {
        PassThrough,  ///< 原样转发给真实实现
        FailOther,    ///< 返回 -1 且错误文本可观测
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

    /// @brief 依次压入若干 `recv_from` 脚本（脚本耗尽后自动转回转发模式）
    /// @param actions 脚本动作序列
    /// @details 接收线程会并发调用 `recv_from`，因此脚本队列用互斥量保护。
    void ScriptRecvFrom(std::initializer_list<RecvFromAction> actions) {
        ScriptRecvFrom(std::vector<RecvFromAction>(actions));
    }

    /// @brief 依次压入若干 `recv_from` 脚本（容器重载）
    /// @param actions 脚本动作序列
    void ScriptRecvFrom(const std::vector<RecvFromAction>& actions) {
        const std::lock_guard<std::mutex> lock(m_script_mutex_);
        m_recv_from_script_.insert(m_recv_from_script_.end(), actions.begin(),
                                   actions.end());
    }

    /// @brief 依次压入若干 `send_to` 脚本（脚本耗尽后自动转回转发模式）
    /// @param actions 脚本动作序列
    void ScriptSendTo(std::initializer_list<SendToAction> actions) {
        const std::lock_guard<std::mutex> lock(m_script_mutex_);
        for (const SendToAction action : actions) {
            m_send_to_script_.push_back(action);
        }
    }

    /// @brief 已发生的 `recv_from` 调用次数
    [[nodiscard]] std::size_t RecvFromCallCount() const noexcept {
        return m_recv_from_calls_.load(std::memory_order_relaxed);
    }

    /// @brief 剩余未消费的 `recv_from` 脚本条数
    [[nodiscard]] std::size_t PendingRecvFromSteps() const {
        const std::lock_guard<std::mutex> lock(m_script_mutex_);
        return m_recv_from_script_.size();
    }

    /// @brief 指定句柄被 `close()` 的次数
    /// @param handle 目标句柄
    /// @return 关闭次数（未关闭过则返回 0）
    /// @details 用于验证"监听句柄在 accept 成功后只关闭一次"。
    [[nodiscard]] int CloseCount(detail::SocketHandle handle) const {
        const std::lock_guard<std::mutex> lock(m_script_mutex_);
        const auto it = m_close_counts_.find(handle);
        return it == m_close_counts_.end() ? 0 : it->second;
    }

    /// @brief 最近一次传给 `listen()` 的句柄（无则返回无效句柄）
    [[nodiscard]] detail::SocketHandle LastListenHandle() const {
        const std::lock_guard<std::mutex> lock(m_script_mutex_);
        return m_last_listen_handle_;
    }

    /// @brief 最近一次 `accept()` 返回的句柄（无则返回无效句柄）
    [[nodiscard]] detail::SocketHandle LastAcceptHandle() const {
        const std::lock_guard<std::mutex> lock(m_script_mutex_);
        return m_last_accept_handle_;
    }

    // ---- SocketOps 覆盖：send / recv / wait_writable / wait_readable /
    // 错误分类 ----

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
    int wait_writable(detail::SocketHandle handle, std::uint32_t timeout_ms,
                      std::string* error_out) override {
        if (m_wait_writable_override_.has_value()) {
            if (error_out != nullptr && *m_wait_writable_override_ < 0) {
                *error_out = "测试脚本：等待可写失败";
            }
            return *m_wait_writable_override_;
        }
        return detail::RealSocketOps().wait_writable(handle, timeout_ms,
                                                     error_out);
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
    detail::SocketHandle create(detail::SocketKind kind) override {
        return detail::RealSocketOps().create(kind);
    }

    /// @brief 关闭 Socket（统计每个句柄的关闭次数后转发）
    void close(detail::SocketHandle handle) override {
        {
            const std::lock_guard<std::mutex> lock(m_script_mutex_);
            ++m_close_counts_[handle];
        }
        detail::RealSocketOps().close(handle);
    }

    /// @brief 绑定（转发）
    int bind(detail::SocketHandle handle,
             const detail::Ipv4Endpoint& endpoint) override {
        return detail::RealSocketOps().bind(handle, endpoint);
    }

    /// @brief 查询本地地址（转发）
    int getsockname(detail::SocketHandle handle,
                    detail::Ipv4Endpoint* endpoint) override {
        return detail::RealSocketOps().getsockname(handle, endpoint);
    }

    /// @brief POD IPv4 名称解析（转发）
    int resolve_ipv4(const char* node, const char* service,
                     detail::SocketKind kind,
                     std::vector<detail::Ipv4Endpoint>* results,
                     std::string* error_out) override {
        return detail::RealSocketOps().resolve_ipv4(node, service, kind,
                                                    results, error_out);
    }

    /// @brief 设为非阻塞（转发）
    int set_non_blocking(detail::SocketHandle handle) override {
        return detail::RealSocketOps().set_non_blocking(handle);
    }

    /// @brief 设为阻塞（转发）
    int set_blocking(detail::SocketHandle handle) override {
        return detail::RealSocketOps().set_blocking(handle);
    }

    /// @brief 监听（记录句柄后转发）
    int listen(detail::SocketHandle handle, int backlog) override {
        {
            const std::lock_guard<std::mutex> lock(m_script_mutex_);
            m_last_listen_handle_ = handle;
        }
        return detail::RealSocketOps().listen(handle, backlog);
    }

    /// @brief 接受连接（记录返回句柄后转发）
    detail::SocketHandle accept(detail::SocketHandle handle,
                                detail::Ipv4Endpoint* endpoint) override {
        const detail::SocketHandle accepted =
            detail::RealSocketOps().accept(handle, endpoint);
        {
            const std::lock_guard<std::mutex> lock(m_script_mutex_);
            m_last_accept_handle_ = accepted;
        }
        return accepted;
    }

    /// @brief 发起连接（转发）
    int connect(detail::SocketHandle handle,
                const detail::Ipv4Endpoint& endpoint) override {
        return detail::RealSocketOps().connect(handle, endpoint);
    }

    /// @brief 读取 SO_ERROR（转发）
    int get_so_error(detail::SocketHandle handle, int* error_out) override {
        return detail::RealSocketOps().get_so_error(handle, error_out);
    }

    /// @brief 设置语义选项（转发）
    int set_option(detail::SocketHandle handle, detail::SocketOption option,
                   bool enabled) override {
        return detail::RealSocketOps().set_option(handle, option, enabled);
    }

    /// @brief 设置 UDP 接收轮询超时（转发）
    int set_receive_timeout(detail::SocketHandle handle,
                            std::uint32_t timeout_ms) override {
        return detail::RealSocketOps().set_receive_timeout(handle, timeout_ms);
    }

    /// @brief 发送 UDP 数据报：按脚本行事，脚本耗尽后转发真实实现
    int send_to(detail::SocketHandle handle, const std::uint8_t* data,
                std::size_t size, const detail::Ipv4Endpoint& peer) override {
        SendToAction action = SendToAction::PassThrough;
        {
            const std::lock_guard<std::mutex> lock(m_script_mutex_);
            if (!m_send_to_script_.empty()) {
                action = m_send_to_script_.front();
                m_send_to_script_.pop_front();
            }
        }
        switch (action) {
            case SendToAction::PassThrough:
                ClearError();
                return detail::RealSocketOps().send_to(handle, data, size,
                                                       peer);
            case SendToAction::FailOther:
                m_last_error_ = "测试脚本：UDP 发送失败";
                return -1;
        }
        return -1;
    }

    /// @brief 接收 UDP 数据报：按脚本行事，脚本耗尽后转发真实实现
    /// @details 返回值与错误分类同时由脚本决定，因为分类判定读取的是平台
    ///          最近错误码，注入返回 -1 不足以让上层走进对应分支。
    int recv_from(detail::SocketHandle handle, std::uint8_t* data,
                  std::size_t size, detail::Ipv4Endpoint* peer) override {
        m_recv_from_calls_.fetch_add(1, std::memory_order_relaxed);
        RecvFromAction action = RecvFromAction::PassThrough;
        {
            const std::lock_guard<std::mutex> lock(m_script_mutex_);
            if (!m_recv_from_script_.empty()) {
                action = m_recv_from_script_.front();
                m_recv_from_script_.pop_front();
            }
        }
        switch (action) {
            case RecvFromAction::PassThrough:
                ClearError();
                return detail::RealSocketOps().recv_from(handle, data, size,
                                                         peer);
            case RecvFromAction::DeliverZeroLength:
                ClearError();
                if (peer != nullptr) {
                    *peer = detail::Ipv4Endpoint{};
                }
                return 0;
            case RecvFromAction::FailTimeout:
                return FailTyped("测试脚本：接收超时",
                                 RecvFromAction::FailTimeout);
            case RecvFromAction::FailInterrupted:
                return FailTyped("测试脚本：接收被信号打断",
                                 RecvFromAction::FailInterrupted);
            case RecvFromAction::FailInvalidSocket:
                return FailTyped("测试脚本：句柄已失效",
                                 RecvFromAction::FailInvalidSocket);
            case RecvFromAction::FailConnectionLost:
                return FailTyped("测试脚本：连接已被重置",
                                 RecvFromAction::FailConnectionLost);
            case RecvFromAction::FailOther:
                return FailTyped("测试脚本：其它接收错误",
                                 RecvFromAction::FailOther);
        }
        return -1;
    }

    /// @brief 判断最近错误是否为 UDP 超时（脚本注入优先）
    [[nodiscard]] bool is_timeout() const override {
        return m_timeout_ ? true : detail::RealSocketOps().is_timeout();
    }

    /// @brief 判断最近错误是否为失效 Socket（脚本注入优先）
    [[nodiscard]] bool is_invalid_socket() const override {
        return m_invalid_ ? true : detail::RealSocketOps().is_invalid_socket();
    }

    /// @brief 半关闭/关闭方向（转发）
    int shutdown_socket(detail::SocketHandle handle) override {
        return detail::RealSocketOps().shutdown_socket(handle);
    }

    /// @brief 等待可读：有待消费的 recv 脚本时视为"立即可读"，否则转发真实实现
    /// @details 生产代码的接收循环按"先等可读、再 recv"推进（Windows 上
    ///          `shutdown()` 无法取消阻塞 `recv()`，见
    ///          `kReceivePollSliceMs`）。
    ///          若这里一并转发真实实现，脚本注入的错误条件会因为真实 Socket
    ///          上没有数据而永远等不到 recv 被调用；把"有待消费脚本"当作
    ///          立即可读，才能让脚本继续驱动接收分支。
    int wait_readable(detail::SocketHandle handle, std::uint32_t timeout_ms,
                      std::string* error_out) override {
        bool has_script = !m_recv_script_.empty();
        if (!has_script) {
            const std::lock_guard<std::mutex> lock(m_script_mutex_);
            has_script = !m_recv_from_script_.empty();
        }
        if (has_script) {
            ClearError();
            return 1;
        }
        return detail::RealSocketOps().wait_readable(handle, timeout_ms,
                                                     error_out);
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

    /// @brief 按 `recv_from` 分类记录一次注入的失败并返回 -1
    /// @param error 注入的错误文本
    /// @param kind 失败分类（决定 `is_timeout`/`is_interrupted`/... 的判定）
    int FailTyped(std::string error, RecvFromAction kind) {
        m_last_error_ = std::move(error);
        m_interrupted_ = (kind == RecvFromAction::FailInterrupted);
        m_connection_lost_ = (kind == RecvFromAction::FailConnectionLost);
        m_timeout_ = (kind == RecvFromAction::FailTimeout);
        m_invalid_ = (kind == RecvFromAction::FailInvalidSocket);
        return -1;
    }

    /// @brief 清除注入的失败状态（转发前调用，避免污染真实错误判定）
    void ClearError() noexcept {
        m_last_error_.clear();
        m_interrupted_ = false;
        m_would_block_ = false;
        m_connection_lost_ = false;
        m_timeout_ = false;
        m_invalid_ = false;
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
    bool m_timeout_{false};                        ///< 注入的接收超时判定
    bool m_invalid_{false};                        ///< 注入的句柄失效判定

    mutable std::mutex m_script_mutex_;  ///< 保护跨线程访问的脚本与计数
    std::deque<RecvFromAction>
        m_recv_from_script_;                     ///< 待消费的 recv_from 脚本
    std::deque<SendToAction> m_send_to_script_;  ///< 待消费的 send_to 脚本
    std::map<detail::SocketHandle, int> m_close_counts_;  ///< 各句柄关闭次数
    std::atomic<std::size_t> m_recv_from_calls_{0};  ///< recv_from 调用次数
    detail::SocketHandle m_last_listen_handle_{
        detail::kInvalidSocketHandle};  ///< 最近一次 listen 的句柄
    detail::SocketHandle m_last_accept_handle_{
        detail::kInvalidSocketHandle};  ///< 最近一次 accept 返回的句柄
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