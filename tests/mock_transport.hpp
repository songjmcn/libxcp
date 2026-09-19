/**
 * @file mock_transport.hpp
 * @brief 脚本化 Mock Transport：不使用真实网络栈的确定性测试替身。
 *
 * 依据 code-plan/XCP_1.3.0_详细设计_类接口与头文件.md 第 15.1 节实现。
 * send() 在调用者线程同步回调 listener，因此测试无需处理线程时序。
 */

#ifndef CALMCAR_XCP_TEST_MOCK_TRANSPORT_HPP_
#define CALMCAR_XCP_TEST_MOCK_TRANSPORT_HPP_

#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "libxcp/ixcp_transport.hpp"
#include "libxcp/xcp_error.hpp"

namespace calmcar::xcp::test {

/**
 * @brief 脚本化 Mock Transport
 *
 * 测试用例预先设置期望的发送内容和对应的响应；也可主动注入 EV/SERV/DTO 报文
 * 与关闭、警告事件，用于确定性地驱动 CommandExecutor 的分支。
 */
class MockTransport : public IXcpTransport {
public:
    MockTransport() = default;
    ~MockTransport() override = default;

    // 禁止拷贝（持有监听器指针与脚本状态）
    MockTransport(const MockTransport&) = delete;
    MockTransport& operator=(const MockTransport&) = delete;

    void open(IPacketListener& listener) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        listener_ = &listener;
        is_open_ = true;
    }

    void close() override {
        IPacketListener* listener = nullptr;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (!is_open_) {
                return;  // 幂等
            }
            is_open_ = false;
            listener = listener_;
            listener_ = nullptr;
        }
        if (listener != nullptr) {
            listener->onTransportClosed("MockTransport 已关闭");
        }
    }

    /**
     * @brief 记录发送的 Packet 并同步投递脚本响应
     * @throws XcpException(TransportError) 未打开，或脚本显式要求模拟发送失败
     */
    void send(BytesView packet) override {
        std::function<Bytes(BytesView)> responder;
        IPacketListener* listener = nullptr;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (!is_open_) {
                throw detail::makeTransportError(
                    "MockTransport 未打开时调用 send()");
            }
            sent_packets_.emplace_back(packet.begin(), packet.end());
            if (fail_next_send_) {
                fail_next_send_ = false;
                throw detail::makeTransportError(
                    "MockTransport 脚本注入的发送失败",
                    "injected send failure");
            }
            responder = response_func_;
            listener = listener_;
        }
        if (responder && listener != nullptr) {
            const Bytes response = responder(packet);
            if (!response.empty()) {
                // 非空返回值视为对本命令的最终 RES/ERR，同步投递给监听器
                listener->onPacketReceived(BytesView{response});
            }
        }
    }

    [[nodiscard]] bool isOpen() const noexcept override {
        const std::lock_guard<std::mutex> lock(mutex_);
        return is_open_;
    }

    // ---- 测试控制接口 ----

    /**
     * @brief 设置 send 时的响应生成器
     * @param response_func 接收发送的 Packet，返回要回调的响应 Packet；
     *                      返回空 Bytes 表示本次不产生响应（用于模拟丢包/超时）
     */
    void setResponse(std::function<Bytes(BytesView)> response_func) {
        const std::lock_guard<std::mutex> lock(mutex_);
        response_func_ = std::move(response_func);
    }

    /// @brief 让下一次 send() 抛出 TransportError（模拟发送失败）
    void failNextSend() {
        const std::lock_guard<std::mutex> lock(mutex_);
        fail_next_send_ = true;
    }

    /// @brief 主动注入异步 Packet（模拟 EV/SERV/DTO）
    void injectPacket(BytesView packet) {
        IPacketListener* listener = nullptr;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            listener = listener_;
        }
        if (listener != nullptr) {
            listener->onPacketReceived(packet);
        }
    }

    /// @brief 主动注入 Transport 关闭事件
    void injectClose(std::string_view reason) {
        IPacketListener* listener = nullptr;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            listener = listener_;
        }
        if (listener != nullptr) {
            listener->onTransportClosed(reason);
        }
    }

    /// @brief 主动注入 Transport 警告
    void injectWarning(std::string_view message) {
        IPacketListener* listener = nullptr;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            listener = listener_;
        }
        if (listener != nullptr) {
            listener->onTransportWarning(message);
        }
    }

    /// @brief 获取已发送的 Packet 列表副本
    [[nodiscard]] std::vector<Bytes> sentPackets() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return sent_packets_;
    }

    /// @brief 清空已发送记录与响应脚本
    void reset() {
        const std::lock_guard<std::mutex> lock(mutex_);
        sent_packets_.clear();
        response_func_ = nullptr;
        fail_next_send_ = false;
    }

private:
    IPacketListener* listener_{nullptr};             ///< 非拥有监听器指针
    bool is_open_{false};                            ///< 是否已打开
    mutable std::mutex mutex_;                       ///< 保护以下全部字段
    std::function<Bytes(BytesView)> response_func_;  ///< 响应脚本
    bool fail_next_send_{false};                     ///< 下次发送失败标记
    std::vector<Bytes> sent_packets_;                ///< 已发送 Packet 记录
};

}  // namespace calmcar::xcp::test

#endif  // CALMCAR_XCP_TEST_MOCK_TRANSPORT_HPP_
