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

    void Open(IPacketListener& listener) override {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_listener_ = &listener;
        m_is_open_ = true;
    }

    void Close() override {
        IPacketListener* listener = nullptr;
        {
            const std::lock_guard<std::mutex> lock(m_mutex_);
            if (!m_is_open_) {
                return;  // 幂等
            }
            m_is_open_ = false;
            listener = m_listener_;
            m_listener_ = nullptr;
        }
        if (listener != nullptr) {
            listener->OnTransportClosed("MockTransport 已关闭");
        }
    }

    /**
     * @brief 记录发送的 Packet 并同步投递脚本响应
     * @throws XcpException(TransportError) 未打开，或脚本显式要求模拟发送失败
     */
    void Send(BytesView packet) override {
        std::function<Bytes(BytesView)> responder;
        IPacketListener* listener = nullptr;
        {
            const std::lock_guard<std::mutex> lock(m_mutex_);
            if (!m_is_open_) {
                throw detail::MakeTransportError(
                    "MockTransport 未打开时调用 send()");
            }
            m_sent_packets_.emplace_back(packet.begin(), packet.end());
            if (m_fail_next_send_) {
                m_fail_next_send_ = false;
                throw detail::MakeTransportError(
                    "MockTransport 脚本注入的发送失败",
                    "injected send failure");
            }
            responder = m_response_func_;
            listener = m_listener_;
        }
        if (responder && listener != nullptr) {
            const Bytes response = responder(packet);
            if (!response.empty()) {
                // 非空返回值视为对本命令的最终 RES/ERR，同步投递给监听器
                listener->OnPacketReceived(BytesView{response});
            }
        }
    }

    [[nodiscard]] bool IsOpen() const noexcept override {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        return m_is_open_;
    }

    // ---- 测试控制接口 ----

    /**
     * @brief 设置 Send 时的响应生成器
     * @param response_func 接收发送的 Packet，返回要回调的响应 Packet；
     *                      返回空 Bytes 表示本次不产生响应（用于模拟丢包/超时）
     */
    void SetResponse(std::function<Bytes(BytesView)> response_func) {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_response_func_ = std::move(response_func);
    }

    /// @brief 让下一次 Send() 抛出 TransportError（模拟发送失败）
    void FailNextSend() {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_fail_next_send_ = true;
    }

    /// @brief 主动注入异步 Packet（模拟 EV/SERV/DTO）
    void InjectPacket(BytesView packet) {
        IPacketListener* listener = nullptr;
        {
            const std::lock_guard<std::mutex> lock(m_mutex_);
            listener = m_listener_;
        }
        if (listener != nullptr) {
            listener->OnPacketReceived(packet);
        }
    }

    /// @brief 主动注入 Transport 关闭事件
    void InjectClose(std::string_view reason) {
        IPacketListener* listener = nullptr;
        {
            const std::lock_guard<std::mutex> lock(m_mutex_);
            listener = m_listener_;
        }
        if (listener != nullptr) {
            listener->OnTransportClosed(reason);
        }
    }

    /// @brief 主动注入 Transport 警告
    void InjectWarning(std::string_view message) {
        IPacketListener* listener = nullptr;
        {
            const std::lock_guard<std::mutex> lock(m_mutex_);
            listener = m_listener_;
        }
        if (listener != nullptr) {
            listener->OnTransportWarning(message);
        }
    }

    /// @brief 获取已发送的 Packet 列表副本
    [[nodiscard]] std::vector<Bytes> SentPackets() const {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        return m_sent_packets_;
    }

    /// @brief 清空已发送记录与响应脚本
    void Reset() {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_sent_packets_.clear();
        m_response_func_ = nullptr;
        m_fail_next_send_ = false;
    }

private:
    IPacketListener* m_listener_{nullptr};
    bool m_is_open_{false};
    mutable std::mutex m_mutex_;
    std::function<Bytes(BytesView)> m_response_func_;
    bool m_fail_next_send_{false};
    std::vector<Bytes> m_sent_packets_;
};

}  // namespace calmcar::xcp::test

#endif  // CALMCAR_XCP_TEST_MOCK_TRANSPORT_HPP_
