/**
 * @file tcp_fake_socket_test.cpp
 * @brief 用脚本化假 Socket 驱动 TcpTransport 的错误分支（L-A2）。
 *
 * 依据 `code-plan/历史遗漏项_详细方案.md` §1.3。
 *
 * @par 覆盖范围
 *   1. 短写（partial write）后必须继续发送剩余字节；
 *   2. EINTR 后必须立即重试，不得当成失败；
 *   3. EAGAIN + 可写等待超时 → 明确的发送超时错误；
 *   4. 接收侧 ECONNRESET → 通过 `OnTransportClosed` 上报"被对端重置"。
 *
 * @note 注入是进程级的：本可执行文件在 ctest 中设置了 `RUN_SERIAL`，
 *       不与其它测试并行，避免污染其它用例的 Socket 行为。
 */

#include "tcp_fake_socket.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include "libxcp/ethernet_header_codec.hpp"
#include "libxcp/tcp_transport.hpp"
#include "libxcp/xcp_error.hpp"

#include "socket_runtime_guard.hpp"

namespace calmcar::xcp {
namespace {

using test_support::FakeSocketOps;
using test_support::ScopedFakeSocketOps;

/// @brief 原生 Socket 句柄类型别名
#if defined(_WIN32)
using RawSocket = SOCKET;
constexpr RawSocket kRawInvalid = INVALID_SOCKET;
#else
using RawSocket = int;
constexpr RawSocket kRawInvalid = -1;
#endif

/// @brief 统一等待上限：足够覆盖 CI 抖动，又不至于让失败用例挂死
constexpr auto kWait = std::chrono::milliseconds(3000);

/**
 * @brief 记录 Transport 回调的监听器（线程安全）
 * @details 只需"是否收到关闭事件 + 原因"，故比端到端测试的版本更薄。
 */
class CloseRecordingListener : public IPacketListener {
public:
    /// @brief 收到 Packet（本文件不关心内容，仅计数）
    void OnPacketReceived(BytesView packet) override {
        {
            const std::lock_guard<std::mutex> lock(m_mutex_);
            ++m_packet_count_;
            static_cast<void>(packet);
        }
        m_cv_.notify_all();
    }

    /// @brief 通道关闭：记录原因并唤醒等待者
    void OnTransportClosed(std::string_view reason) override {
        {
            const std::lock_guard<std::mutex> lock(m_mutex_);
            m_closed_ = true;
            m_reason_ = std::string(reason);
        }
        m_cv_.notify_all();
    }

    /// @brief 传输告警（本文件不断言）
    void OnTransportWarning(std::string_view message) override {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_warnings_.emplace_back(message);
    }

    /// @brief 等待通道关闭事件
    bool WaitForClose(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex_);
        return m_cv_.wait_for(lock, timeout, [&] { return m_closed_; });
    }

    /// @brief 关闭原因文本
    [[nodiscard]] std::string reason() const {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        return m_reason_;
    }

private:
    mutable std::mutex m_mutex_;           ///< 保护下列字段
    std::condition_variable m_cv_;         ///< 关闭事件通知
    std::size_t m_packet_count_{0};        ///< 收到的 Packet 数
    std::vector<std::string> m_warnings_;  ///< 告警文本
    bool m_closed_{false};                 ///< 是否已关闭
    std::string m_reason_;                 ///< 关闭原因
};

/**
 * @brief 测试基座：起一个原生监听 Socket，供 Master 角色连接
 * @details 与 `tests/tcp_transport_test.cpp` 的 `TcpEndToEndTest` 同构。
 *          这里独立实现而非共享头文件，是为了不改动已完成测试的既有结构。
 */
class TcpFakeSocketTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Windows 上裸 ::socket 之前必须先 WSAStartup（见 socket_runtime_guard.hpp）
        test_support::EnsureSocketRuntime();
        m_listen_handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ASSERT_NE(m_listen_handle, kRawInvalid);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(0);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        ASSERT_EQ(::bind(m_listen_handle, reinterpret_cast<sockaddr*>(&addr),
                         sizeof(addr)),
                  0);
        socklen_t len = sizeof(addr);
        ASSERT_EQ(::getsockname(m_listen_handle,
                                reinterpret_cast<sockaddr*>(&addr), &len),
                  0);
        ASSERT_EQ(::listen(m_listen_handle, 1), 0);
        m_port = ntohs(addr.sin_port);
        ASSERT_NE(m_port, 0);
    }

    void TearDown() override {
        if (m_peer_handle != kRawInvalid) {
            ::shutdown(m_peer_handle, 2);
            ::close(m_peer_handle);
            m_peer_handle = kRawInvalid;
        }
        if (m_listen_handle != kRawInvalid) {
            ::shutdown(m_listen_handle, 2);
            ::close(m_listen_handle);
            m_listen_handle = kRawInvalid;
        }
    }

    /// @brief 接受 Master 发起的连接（连接已被内核排队，accept 立即返回）
    void AcceptPeer() {
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        m_peer_handle =
            ::accept(m_listen_handle, reinterpret_cast<sockaddr*>(&peer), &len);
    }

    /// @brief 从对端读取恰好 expected 字节（超时/断开则返回已读部分）
    Bytes ReadExact(std::size_t expected) {
        Bytes out;
        out.reserve(expected);
        std::vector<std::uint8_t> chunk(1024);
        while (out.size() < expected) {
            const int n =
                ::recv(m_peer_handle, reinterpret_cast<char*>(chunk.data()),
                       static_cast<int>(chunk.size()), 0);
            if (n <= 0) {
                break;
            }
            out.insert(out.end(), chunk.begin(), chunk.begin() + n);
        }
        return out;
    }

    /// @brief 构造指向本测试监听端口的 Master 配置
    TcpTransportConfig MakeMasterConfig() const {
        TcpTransportConfig cfg;
        cfg.role = TcpRole::Master;
        cfg.remote_host = "127.0.0.1";
        cfg.remote_port = m_port;
        cfg.local_host = "127.0.0.1";
        cfg.local_port = 0;
        cfg.connect_timeout_ms = 2000;
        cfg.send_timeout_ms = 2000;
        return cfg;
    }

    RawSocket m_listen_handle{kRawInvalid};  ///< 监听句柄
    RawSocket m_peer_handle{kRawInvalid};    ///< 已接受的对端句柄
    std::uint16_t m_port{0};                 ///< 监听端口（OS 分配）
};

/**
 * @brief 按 AsamV14 手工编一个 Frame
 * @param payload Packet 内容
 * @param ctr 帧序号
 * @return `[LEN le16][CTR le16][payload]`
 */
Bytes MakeWireFrame(const Bytes& payload, std::uint16_t ctr) {
    Bytes wire;
    wire.reserve(kEthernetHeaderSize + payload.size());
    wire.push_back(static_cast<std::uint8_t>(payload.size() & 0xFFU));
    wire.push_back(static_cast<std::uint8_t>((payload.size() >> 8) & 0xFFU));
    wire.push_back(static_cast<std::uint8_t>(ctr & 0xFFU));
    wire.push_back(static_cast<std::uint8_t>((ctr >> 8) & 0xFFU));
    wire.insert(wire.end(), payload.begin(), payload.end());
    return wire;
}

// ===========================================================================
// 发送侧：短写 / EINTR / EAGAIN 超时
// ===========================================================================

TEST_F(TcpFakeSocketTest, SendCompletesAfterPartialWrite) {
    // 首帧只放行 3 字节，其余交给真实 send：验证 WriteAll 的补全循环。
    FakeSocketOps fake;
    ScopedFakeSocketOps guard(fake);
    fake.ScriptSend({{FakeSocketOps::SendAction::TruncateTo, 3}});

    CloseRecordingListener listener;
    TcpTransport transport(MakeMasterConfig());
    transport.Open(listener);
    AcceptPeer();

    const Bytes payload{0xFF, 0x00};
    transport.Send(payload);

    // 至少两次 send：第一次被截断，第二次（转发）发完剩余字节
    EXPECT_GE(fake.SendCallCount(), 2U);
    EXPECT_EQ(fake.PendingSendSteps(), 0U);

    const Bytes wire = ReadExact(kEthernetHeaderSize + payload.size());
    EXPECT_EQ(wire, MakeWireFrame(payload, /*ctr=*/0));

    transport.Close();
}

TEST_F(TcpFakeSocketTest, SendRetriesAfterInterrupt) {
    // 一次 EINTR：必须立即重试，既不丢弃也不上报失败。
    FakeSocketOps fake;
    ScopedFakeSocketOps guard(fake);
    fake.ScriptSend({{FakeSocketOps::SendAction::FailInterrupted, 0}});

    CloseRecordingListener listener;
    TcpTransport transport(MakeMasterConfig());
    transport.Open(listener);
    AcceptPeer();

    const Bytes payload{0xFF, 0x00, 0x11};
    transport.Send(payload);

    EXPECT_EQ(fake.SendCallCount(), 2U);  // 被中断一次 + 成功一次
    const Bytes wire = ReadExact(kEthernetHeaderSize + payload.size());
    EXPECT_EQ(wire, MakeWireFrame(payload, /*ctr=*/0));

    transport.Close();
}

TEST_F(TcpFakeSocketTest, SendFailsWhenWritableWaitTimesOut) {
    // EAGAIN + 可写等待超时 → 发送超时错误（TransportError），不得静默丢弃。
    FakeSocketOps fake;
    ScopedFakeSocketOps guard(fake);
    fake.ScriptSend({{FakeSocketOps::SendAction::FailWouldBlock, 0}});

    CloseRecordingListener listener;
    TcpTransport transport(MakeMasterConfig());
    transport.Open(listener);
    AcceptPeer();
    // 连接建立之后才覆盖：非阻塞 connect 同样走 wait_writable，
    // 提前覆盖会把连接建立本身判成超时。
    fake.SetWaitWritableResult(0);  // 0 = 等待超时

    const Bytes payload{0xFF, 0x00};
    try {
        transport.Send(payload);
        FAIL() << "等待可写超时后 Send 必须抛错";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::TransportError);
        EXPECT_NE(std::string(e.what()).find("发送超时"), std::string::npos)
            << e.what();
    }

    transport.Close();
}

TEST_F(TcpFakeSocketTest, SendReportsFailureWhenWritableWaitErrors) {
    // 可写等待本身返回 -1（例如 Socket 已被关闭）→ 上报"等待可写失败"。
    FakeSocketOps fake;
    ScopedFakeSocketOps guard(fake);
    fake.ScriptSend({{FakeSocketOps::SendAction::FailWouldBlock, 0}});

    CloseRecordingListener listener;
    TcpTransport transport(MakeMasterConfig());
    transport.Open(listener);
    AcceptPeer();
    fake.SetWaitWritableResult(-1);

    const Bytes payload{0xFF, 0x00};
    try {
        transport.Send(payload);
        FAIL() << "可写等待失败后 Send 必须抛错";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::TransportError);
        EXPECT_NE(std::string(e.what()).find("等待 TCP 可写失败"),
                  std::string::npos)
            << e.what();
    }

    transport.Close();
}

// ===========================================================================
// 接收侧：连接重置
// ===========================================================================

TEST_F(TcpFakeSocketTest, ReceiveReportsConnectionReset) {
    // 接收返回 -1 且判定为连接已断 → 必须走 OnTransportClosed，原因含"重置"。
    FakeSocketOps fake;
    ScopedFakeSocketOps guard(fake);
    // 脚本必须在 Open 之前装好：接收线程一启动就会阻塞在 recv 上，
    // 之后再注入则它已经消费了真实 recv，脚本永远不会被执行。
    fake.ScriptReceive(FakeSocketOps::ReceiveAction::FailConnectionLost);

    CloseRecordingListener listener;
    TcpTransport transport(MakeMasterConfig());
    transport.Open(listener);
    AcceptPeer();

    EXPECT_TRUE(listener.WaitForClose(kWait));
    EXPECT_NE(listener.reason().find("重置"), std::string::npos)
        << "关闭原因应说明是被重置，实际：" << listener.reason();

    transport.Close();
}

}  // namespace
}  // namespace calmcar::xcp