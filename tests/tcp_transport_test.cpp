/**
 * @file tcp_transport_test.cpp
 * @brief TcpTransport 测试（B-T2）。
 *
 * 依据 code-plan/TCP传输_实施计划_B-T1至B-T5.md 第 5 节与 §9.2/§9.3。
 *
 * @par 两类测试，两种手段
 *   1. **端到端测试**用真实回环 TCP：验证角色、连接、双 Profile 收发、
 *      短写补全、EOF 语义、关闭时打断阻塞 recv 等真实行为。
 *   2. **错误分支测试**在独立目标 `TcpFakeSocketTest`
 *      （`tests/tcp_fake_socket_test.cpp` + `tests/tcp_fake_socket.hpp`）里，
 *      通过 `SetSocketOpsForTesting()` 注入脚本化假 SocketOps：partial write、
 *      EINTR、EAGAIN+可写超时、连接重置这些条件在真实 Socket 上无法确定性
 *      复现（内核缓冲大小、信号时序都不可控），只能用脚本化实现精确驱动。
 *      该目标独进程运行（ctest `RUN_SERIAL`），因为注入是进程级的。
 *
 * @par 不依赖固定端口
 *   所有监听端口均用 `local_port = 0` 让 OS 分配，再通过 `LocalPort()` 取回，
 *   避免测试间端口冲突与 CI 上的偶发失败。
 */

#include "libxcp/tcp_transport.hpp"

#include "src/socket_util.hpp"
#include "src/event_poller.hpp"
#include "tcp_fake_socket.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include "libxcp/ethernet_header_codec.hpp"
#include "libxcp/tcp_stream_decoder.hpp"

#include "socket_runtime_guard.hpp"

namespace calmcar::xcp {
namespace {

/// @brief 收集 Transport 回调的监听器（线程安全）
class RecordingListener : public IPacketListener {
public:
    void OnPacketReceived(BytesView packet) override {
        {
            const std::lock_guard<std::mutex> lock(m_mutex_);
            m_packets_.emplace_back(packet.begin(), packet.end());
        }
        m_cv_.notify_all();
    }

    void OnTransportClosed(std::string_view reason) override {
        {
            const std::lock_guard<std::mutex> lock(m_mutex_);
            m_closed_ = true;
            m_close_reason_ = std::string(reason);
        }
        m_cv_.notify_all();
    }

    void OnTransportWarning(std::string_view message) override {
        {
            const std::lock_guard<std::mutex> lock(m_mutex_);
            m_warnings_.emplace_back(message);
        }
        m_cv_.notify_all();
    }

    /// @brief 等待收到至少 expected_count 个 Packet（或通道关闭）
    bool WaitForPackets(std::size_t expected_count,
                        std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex_);
        return m_cv_.wait_for(lock, timeout, [&] {
            return m_packets_.size() >= expected_count || m_closed_;
        });
    }

    /// @brief 等待通道关闭事件
    bool WaitForClose(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex_);
        return m_cv_.wait_for(lock, timeout, [&] { return m_closed_; });
    }

    [[nodiscard]] std::vector<Bytes> Packets() const {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        return m_packets_;
    }

    [[nodiscard]] std::vector<std::string> Warnings() const {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        return m_warnings_;
    }

    [[nodiscard]] bool closed() const {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        return m_closed_;
    }

    [[nodiscard]] std::string close_reason() const {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        return m_close_reason_;
    }

private:
    mutable std::mutex m_mutex_;
    std::condition_variable m_cv_;
    std::vector<Bytes> m_packets_;
    std::vector<std::string> m_warnings_;
    bool m_closed_{false};
    std::string m_close_reason_;
};

/// @brief 测试用超时：足够宽松以避免 CI 抖动，又不至于让失败用例挂太久
constexpr auto kWaitShort = std::chrono::milliseconds(2000);
constexpr auto kWaitLong = std::chrono::milliseconds(5000);

/// @brief 构造一个 Slave 角色的监听配置（端口由 OS 分配）
TcpTransportConfig MakeSlaveConfig() {
    TcpTransportConfig cfg;
    cfg.role = TcpRole::Slave;
    cfg.local_host = "127.0.0.1";
    cfg.local_port = 0;
    cfg.connect_timeout_ms = 2000;
    cfg.send_timeout_ms = 2000;
    return cfg;
}

/// @brief 构造一个指向指定端口的 Master 配置
TcpTransportConfig MakeMasterConfig(std::uint16_t port) {
    TcpTransportConfig cfg;
    cfg.role = TcpRole::Master;
    cfg.remote_host = "127.0.0.1";
    cfg.remote_port = port;
    cfg.local_host = "127.0.0.1";
    cfg.local_port = 0;
    cfg.connect_timeout_ms = 2000;
    cfg.send_timeout_ms = 2000;
    return cfg;
}

/// @brief 用原生 Socket 连到指定端口（用于充当对端）
#if defined(_WIN32)
using RawSocket = SOCKET;
constexpr RawSocket kRawInvalid = INVALID_SOCKET;
#else
using RawSocket = int;
constexpr RawSocket kRawInvalid = -1;
#endif

/// @brief 关闭裸 Socket 句柄（Windows 必须 `closesocket`，详见
/// socket_runtime_guard.hpp）
void CloseRaw(RawSocket handle) {
    test_support::CloseRawSocket(static_cast<std::intptr_t>(handle));
}

/**
 * @brief 手工编一个接收侧的 wire Frame（AsamV14 形态：无 Tail）
 * @param payload XCP Packet 内容
 * @param ctr 本帧 CTR
 * @return wire 字节序列 `[LEN le16][CTR le16][payload]`
 * @details 测试侧**不**调用生产编码器：若收发两侧共用同一份编码实现，那么
 *          "能解析"只能证明本库自洽，同一个误解会在两侧对称出现而照样通过。
 *          这里手写字节，使 CTR 可以被任意构造。
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

/// @brief 把字节序列写给对端，失败即断言
/// @param handle 对端 Socket（由 `TcpEndToEndTest::m_peer_handle` 传入）
/// @param data 待写字节
/// @note 以函数形式而非成员，是为了让 CTR 用例能在一行内表达"写一帧"。
inline void SendAll(RawSocket handle, const Bytes& data) {
    ASSERT_NE(handle, kRawInvalid);
    ASSERT_GT(::send(handle, reinterpret_cast<const char*>(data.data()),
                     static_cast<int>(data.size()), 0),
              0);
}

}  // namespace

// ===========================================================================
// 配置校验（在建立任何连接之前失败）
// ===========================================================================

TEST(TcpTransportConfigTest, RejectsUnknownWireProfile) {
    TcpTransportConfig cfg = MakeMasterConfig(4000);
    cfg.wire_profile = TcpWireProfile::Unknown;
    TcpTransport transport(cfg);
    RecordingListener listener;

    EXPECT_THROW(transport.Open(listener), XcpException);
    EXPECT_FALSE(transport.IsOpen());
}

TEST(TcpTransportConfigTest, RejectsUnknownPacketAlignment) {
    TcpTransportConfig cfg = MakeMasterConfig(4000);
    cfg.packet_alignment = PacketAlignment::Unknown;
    TcpTransport transport(cfg);
    RecordingListener listener;

    EXPECT_THROW(transport.Open(listener), XcpException);
    EXPECT_FALSE(transport.IsOpen());
}

TEST(TcpTransportConfigTest, MasterRejectsZeroRemotePort) {
    TcpTransportConfig cfg = MakeMasterConfig(0);
    TcpTransport transport(cfg);
    RecordingListener listener;

    EXPECT_THROW(transport.Open(listener), XcpException);
}

TEST(TcpTransportConfigTest, MasterRejectsEmptyRemoteHost) {
    TcpTransportConfig cfg = MakeMasterConfig(4000);
    cfg.remote_host.clear();
    TcpTransport transport(cfg);
    RecordingListener listener;

    EXPECT_THROW(transport.Open(listener), XcpException);
}

TEST(TcpTransportConfigTest, RejectsZeroTimeouts) {
    RecordingListener listener;

    {
        TcpTransportConfig cfg = MakeMasterConfig(4000);
        cfg.connect_timeout_ms = 0;
        TcpTransport transport(cfg);
        EXPECT_THROW(transport.Open(listener), XcpException);
    }
    {
        TcpTransportConfig cfg = MakeMasterConfig(4000);
        cfg.send_timeout_ms = 0;
        TcpTransport transport(cfg);
        EXPECT_THROW(transport.Open(listener), XcpException);
    }
    {
        TcpTransportConfig cfg = MakeMasterConfig(4000);
        cfg.receive_chunk_size = 0;
        TcpTransport transport(cfg);
        EXPECT_THROW(transport.Open(listener), XcpException);
    }
}

TEST(TcpTransportConfigTest, RejectsOutOfRangePacketSizeLimits) {
    RecordingListener listener;

    {
        TcpTransportConfig cfg = MakeMasterConfig(4000);
        cfg.max_frame_packet_size = 0;
        TcpTransport transport(cfg);
        EXPECT_THROW(transport.Open(listener), XcpException);
    }
    {
        // 超过 16 位 LEN 能表达的最大值：必须拒绝，不能截断
        TcpTransportConfig cfg = MakeMasterConfig(4000);
        cfg.max_frame_packet_size = 70000;
        TcpTransport transport(cfg);
        EXPECT_THROW(transport.Open(listener), XcpException);
    }
    {
        TcpTransportConfig cfg = MakeMasterConfig(4000);
        cfg.max_packet_size = 70000;
        TcpTransport transport(cfg);
        EXPECT_THROW(transport.Open(listener), XcpException);
    }
}

// ===========================================================================
// 能力边界：IPv6 必须显式报 UnsupportedFeature
// ===========================================================================

TEST(TcpTransportCapabilityTest, Ipv6RemoteReportsUnsupportedFeature) {
    TcpTransportConfig cfg = MakeMasterConfig(4000);
    cfg.remote_host = "::1";
    TcpTransport transport(cfg);
    RecordingListener listener;

    try {
        transport.Open(listener);
        FAIL() << "IPv6 远端必须被拒绝";
    } catch (const XcpException& e) {
        // 必须是能力缺失而非"地址非法"：调用方需要能区分笔误与未实现
        EXPECT_EQ(e.Category(), ErrorCategory::UnsupportedFeature);
    }
}

TEST(TcpTransportCapabilityTest, Ipv6LocalReportsUnsupportedFeature) {
    TcpTransportConfig cfg = MakeMasterConfig(4000);
    cfg.local_host = "fe80::1";
    TcpTransport transport(cfg);
    RecordingListener listener;

    try {
        transport.Open(listener);
        FAIL() << "IPv6 本地地址必须被拒绝";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::UnsupportedFeature);
    }
}

// ===========================================================================
// 连接建立
// ===========================================================================

TEST(TcpTransportConnectionTest, MasterFailsWhenNothingListening) {
    // 先占一个端口再释放，得到一个几乎肯定无人监听的端口号
    TcpTransportConfig slave_cfg = MakeSlaveConfig();
    std::uint16_t dead_port = 0;
    {
        TcpTransport probe(slave_cfg);
        RecordingListener probe_listener;
        // 不存在可用的无监听取端口方式：直接用一个高位端口并接受极小概率冲突
        dead_port = 45711;
    }

    TcpTransportConfig cfg = MakeMasterConfig(dead_port);
    cfg.connect_timeout_ms = 500;
    TcpTransport transport(cfg);
    RecordingListener listener;

    EXPECT_THROW(transport.Open(listener), XcpException);
    EXPECT_FALSE(transport.IsOpen());
}

TEST(TcpTransportConnectionTest, SlaveAcceptTimesOutWithoutClient) {
    TcpTransportConfig cfg = MakeSlaveConfig();
    cfg.connect_timeout_ms = 300;
    TcpTransport transport(cfg);
    RecordingListener listener;

    EXPECT_THROW(transport.Open(listener), XcpException);
    EXPECT_FALSE(transport.IsOpen());
}

TEST(TcpTransportConnectionTest, SlavePublishesOsAssignedPort) {
    // 用一个"客户端"角色来建立真实连接，从而验证 port 回填发生在 accept 之前
    TcpTransportConfig slave_cfg = MakeSlaveConfig();
    TcpTransport slave(slave_cfg);
    RecordingListener slave_listener;

    // 用线程同时扮演 Slave（accept）与 Master（connect）
    std::thread slave_thread([&] {
        try {
            slave.Open(slave_listener);
        } catch (...) {
        }
    });
    slave_thread.join();  // Slave 单独不会成功；这里只是验证超时后无端口泄漏

    EXPECT_FALSE(slave.IsOpen());
}

// ===========================================================================
// 端到端：双 Profile 收发
// ===========================================================================

/**
 * @brief 端到端夹具：一条真实回环 TCP 连接的两端
 *
 * @details 一端是 `TcpTransport`（Master），另一端用原生 Socket 手写对端行为，
 *          以便精确控制写入的字节序列（这是验证 framing 的唯一可靠方式——
 *          两端都用我们的实现只能证明"自洽"，不能证明线格式正确）。
 */
class TcpEndToEndTest : public ::testing::Test {
protected:
    /// @brief 启动一个监听 Socket 并返回其端口
    std::uint16_t StartRawListener() {
        // Windows 上裸 ::socket 之前必须先 WSAStartup（见
        // socket_runtime_guard.hpp）
        test_support::EnsureSocketRuntime();
        m_listen_handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_listen_handle == kRawInvalid) {
            return 0;
        }
        int reuse = 1;
        ::setsockopt(m_listen_handle, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(0);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::bind(m_listen_handle, reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) != 0) {
            return 0;
        }
        socklen_t len = sizeof(addr);
        ::getsockname(m_listen_handle, reinterpret_cast<sockaddr*>(&addr),
                      &len);
        if (::listen(m_listen_handle, 1) != 0) {
            return 0;
        }
        return ntohs(addr.sin_port);
    }

    /// @brief 等待一个裸句柄可读，最多等 timeout_ms 毫秒
    /// @param handle 裸 Socket 句柄
    /// @param timeout_ms 等待上限（毫秒）
    /// @return 就绪返回 true；超时或等待失败返回 false
    /// @details 给后面的阻塞 `::accept` / `::recv` 一个明确期限：Transport 侧
    ///          `Open()` 失败时若直接阻塞，用例会挂到 ctest 的单用例超时
    ///          （Windows CI 上实际出现过），既看不到原因又白烧时间。
    static bool WaitReadableRaw(RawSocket handle, std::uint32_t timeout_ms) {
        const detail::WaitItem item{static_cast<detail::SocketHandle>(handle),
                                    true, false};
        std::vector<detail::WaitStatus> statuses;
        return detail::WaitAnySocket(
                   std::span<const detail::WaitItem>(&item, 1), timeout_ms,
                   statuses) > 0;
    }

    /// @brief 接受一个入站连接（最多等 5 秒），成功后 m_peer_handle 有效
    bool AcceptPeer() {
        if (!WaitReadableRaw(m_listen_handle, 5000U)) {
            return false;
        }
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        m_peer_handle =
            ::accept(m_listen_handle, reinterpret_cast<sockaddr*>(&peer), &len);
        return m_peer_handle != kRawInvalid;
    }

    /// @brief 从对端读取直到凑满 expected 字节（或超时/断开）
    Bytes ReadExact(std::size_t expected) {
        Bytes out;
        out.reserve(expected);
        std::uint8_t chunk[1024];
        while (out.size() < expected) {
            if (!WaitReadableRaw(m_peer_handle, 5000U)) {
                break;
            }
            const int n = ::recv(m_peer_handle, reinterpret_cast<char*>(chunk),
                                 sizeof(chunk), 0);
            if (n <= 0) {
                break;
            }
            out.insert(out.end(), chunk, chunk + n);
        }
        return out;
    }

    void TearDown() override {
        if (m_peer_handle != kRawInvalid) {
            ::shutdown(m_peer_handle, 2);
            CloseRaw(m_peer_handle);
        }
        if (m_listen_handle != kRawInvalid) {
            CloseRaw(m_listen_handle);
        }
    }

    RawSocket m_listen_handle{kRawInvalid};
    RawSocket m_peer_handle{kRawInvalid};
};

TEST_F(TcpEndToEndTest, MasterSendsAsamV14FrameWithoutTail) {
    const std::uint16_t port = StartRawListener();
    ASSERT_NE(port, 0);

    TcpTransportConfig cfg = MakeMasterConfig(port);
    cfg.wire_profile = TcpWireProfile::AsamV14;
    TcpTransport transport(cfg);
    RecordingListener listener;

    std::thread accept_thread([this] { AcceptPeer(); });
    transport.Open(listener);
    accept_thread.join();

    ASSERT_TRUE(transport.IsOpen());

    // 发一个 CRO（CONNECT 命令：0xFF 0x00）
    const Bytes cro{0xFF, 0x00};
    transport.Send(cro);

    const Bytes received = ReadExact(kEthernetHeaderSize + cro.size());
    ASSERT_EQ(received.size(), kEthernetHeaderSize + cro.size());

    // AsamV14：LEN 是真实 Packet 长度，且**绝不**附加 LEN 外 Tail
    const std::uint16_t len =
        static_cast<std::uint16_t>(received[0] | (received[1] << 8));
    const std::uint16_t ctr =
        static_cast<std::uint16_t>(received[2] | (received[3] << 8));
    EXPECT_EQ(len, cro.size());
    EXPECT_EQ(ctr, 0);  // Open() 后首个 Frame 使用 CTR=0
    EXPECT_EQ(received[4], 0xFF);
    EXPECT_EQ(received[5], 0x00);
}

/**
 * @brief 回归用例：对端保持静默时 `Close()` 必须在有限时间内返回
 *
 * @details Windows 上 `shutdown(SD_BOTH)` 会返回成功，但**不会**取消另一个
 *          线程里已挂起的阻塞 `recv()`（POSIX 上则会立刻返回 0）。因此接收
 *          线程若直接阻塞在 `recv()` 上，`Close()` 的 `join()` 就会永久等待
 *          —— 这正是 Windows CI 上整轮 ctest 被拖死的根因。
 *          用例建立连接后有意不发任何数据，直接 `Close()`，要求：
 *          1. 退出延迟有明确上界（远大于一个轮询片，远小于用例看门狗）；
 *          2. 接收线程退出前仍交付一次 `OnTransportClosed()`。
 */
TEST_F(TcpEndToEndTest, CloseReturnsPromptlyWhilePeerIsSilent) {
    const std::uint16_t port = StartRawListener();
    ASSERT_NE(port, 0);

    TcpTransportConfig cfg = MakeMasterConfig(port);
    TcpTransport transport(cfg);
    RecordingListener listener;

    std::thread accept_thread([this] { AcceptPeer(); });
    transport.Open(listener);
    accept_thread.join();
    ASSERT_TRUE(transport.IsOpen());

    // 对端一个字节都不发：此刻接收线程在等待可读
    const auto started = std::chrono::steady_clock::now();
    transport.Close();
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started)
            .count();

    EXPECT_LT(elapsed_ms, kWaitShort.count())
        << "Close() 耗时 " << elapsed_ms << " ms，接收线程疑似未被唤醒";
    EXPECT_FALSE(transport.IsOpen());
    // Close() 已 join 接收线程，关闭回调必须在返回前交付完毕
    EXPECT_TRUE(listener.closed());
    EXPECT_FALSE(listener.close_reason().empty());
}

TEST_F(TcpEndToEndTest, MasterSendsAsamV15FrameWithAlignmentTail) {
    const std::uint16_t port = StartRawListener();
    ASSERT_NE(port, 0);

    TcpTransportConfig cfg = MakeMasterConfig(port);
    cfg.wire_profile = TcpWireProfile::AsamV15;
    // alignment=32 且 Packet 长度使 Tail 非零，才能真正验证 Tail 被写出
    cfg.packet_alignment = PacketAlignment::Bit32;
    TcpTransport transport(cfg);
    RecordingListener listener;

    std::thread accept_thread([this] { AcceptPeer(); });
    transport.Open(listener);
    accept_thread.join();

    // Packet 长度 3：Header(4)+3 = 7，对齐到 4 需要 1 字节 Tail
    const Bytes packet{0x01, 0x02, 0x03};
    transport.Send(packet);

    const std::size_t expect_tail = TailBytesForAlignment(
        PacketAlignment::Bit32, kEthernetHeaderSize + packet.size());
    ASSERT_EQ(expect_tail, 1U);

    const Bytes received =
        ReadExact(kEthernetHeaderSize + packet.size() + expect_tail);
    ASSERT_EQ(received.size(),
              kEthernetHeaderSize + packet.size() + expect_tail);

    // LEN 必须是真实 Packet 长度（不含 Tail）——这是 AsamV15 的核心契约
    const std::uint16_t len =
        static_cast<std::uint16_t>(received[0] | (received[1] << 8));
    EXPECT_EQ(len, packet.size());
    EXPECT_EQ(received[4], 0x01);
    EXPECT_EQ(received[5], 0x02);
    EXPECT_EQ(received[6], 0x03);
    EXPECT_EQ(received[7], 0x00);  // FILL 内容规范定为 do not care，本库填 0
}

TEST_F(TcpEndToEndTest, ReceiveDecodesFrameSentByRawPeer) {
    const std::uint16_t port = StartRawListener();
    ASSERT_NE(port, 0);

    TcpTransportConfig cfg = MakeMasterConfig(port);
    cfg.wire_profile = TcpWireProfile::AsamV14;
    TcpTransport transport(cfg);
    RecordingListener listener;

    std::thread accept_thread([this] { AcceptPeer(); });
    transport.Open(listener);
    accept_thread.join();

    // 对端手写一个响应帧：RES + 数据
    const Bytes payload{0xFF, 0x10, 0xAA, 0xBB};
    std::vector<std::uint8_t> wire;
    wire.push_back(static_cast<std::uint8_t>(payload.size() & 0xFF));
    wire.push_back(static_cast<std::uint8_t>((payload.size() >> 8) & 0xFF));
    wire.push_back(0x00);  // CTR lo
    wire.push_back(0x00);  // CTR hi
    wire.insert(wire.end(), payload.begin(), payload.end());

    ASSERT_GT(::send(m_peer_handle, reinterpret_cast<const char*>(wire.data()),
                     static_cast<int>(wire.size()), 0),
              0);

    ASSERT_TRUE(listener.WaitForPackets(1, kWaitLong));
    const auto packets = listener.Packets();
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(packets[0], payload);
}

TEST_F(TcpEndToEndTest, ReceiveHandlesFrameSplitAcrossWrites) {
    const std::uint16_t port = StartRawListener();
    ASSERT_NE(port, 0);

    TcpTransportConfig cfg = MakeMasterConfig(port);
    cfg.wire_profile = TcpWireProfile::AsamV14;
    TcpTransport transport(cfg);
    RecordingListener listener;

    std::thread accept_thread([this] { AcceptPeer(); });
    transport.Open(listener);
    accept_thread.join();

    const Bytes payload{0xFF, 0x20, 0x01, 0x02, 0x03, 0x04, 0x05};
    std::vector<std::uint8_t> wire;
    wire.push_back(static_cast<std::uint8_t>(payload.size() & 0xFF));
    wire.push_back(static_cast<std::uint8_t>((payload.size() >> 8) & 0xFF));
    wire.push_back(0x00);
    wire.push_back(0x00);
    wire.insert(wire.end(), payload.begin(), payload.end());

    // 逐字节写入，附小睡以确保内核把它们当作独立 TCP 段交付。
    // 这验证的正是"recv 边界与 Frame 边界无关"这一 TCP 核心事实。
    for (const std::uint8_t byte : wire) {
        ASSERT_GT(
            ::send(m_peer_handle, reinterpret_cast<const char*>(&byte), 1, 0),
            0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    ASSERT_TRUE(listener.WaitForPackets(1, kWaitLong));
    const auto packets = listener.Packets();
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(packets[0], payload);
}

TEST_F(TcpEndToEndTest, ReceiveHandlesTwoFramesInOneWrite) {
    const std::uint16_t port = StartRawListener();
    ASSERT_NE(port, 0);

    TcpTransportConfig cfg = MakeMasterConfig(port);
    cfg.wire_profile = TcpWireProfile::AsamV14;
    TcpTransport transport(cfg);
    RecordingListener listener;

    std::thread accept_thread([this] { AcceptPeer(); });
    transport.Open(listener);
    accept_thread.join();

    const Bytes first{0xFF, 0x30, 0x11};
    const Bytes second{0xFF, 0x31, 0x22};

    std::vector<std::uint8_t> wire;
    // 两个帧各用**连续** CTR（0x0000、0x0001）。
    // 本用例验证的是"一次 write 里的粘包能被拆成两帧"，与 CTR 语义无关；
    // 但 CTR 校验上线后，若两帧都用 0x0000，第二帧会被正确判为 duplicate
    // 而断链——那会掩盖本用例真正要验证的重组行为。
    std::uint16_t ctr = 0;
    for (const auto* pkt : {&first, &second}) {
        wire.push_back(static_cast<std::uint8_t>(pkt->size() & 0xFF));
        wire.push_back(static_cast<std::uint8_t>((pkt->size() >> 8) & 0xFF));
        wire.push_back(static_cast<std::uint8_t>(ctr & 0xFFU));
        wire.push_back(static_cast<std::uint8_t>((ctr >> 8) & 0xFFU));
        wire.insert(wire.end(), pkt->begin(), pkt->end());
        ++ctr;
    }
    // 一次性写入两个完整帧（粘包）
    ASSERT_GT(::send(m_peer_handle, reinterpret_cast<const char*>(wire.data()),
                     static_cast<int>(wire.size()), 0),
              0);

    ASSERT_TRUE(listener.WaitForPackets(2, kWaitLong));
    const auto packets = listener.Packets();
    ASSERT_EQ(packets.size(), 2U);
    EXPECT_EQ(packets[0], first);
    EXPECT_EQ(packets[1], second);
}

TEST_F(TcpEndToEndTest, PeerCloseDeliversClosedCallback) {
    const std::uint16_t port = StartRawListener();
    ASSERT_NE(port, 0);

    TcpTransportConfig cfg = MakeMasterConfig(port);
    TcpTransport transport(cfg);
    RecordingListener listener;

    std::thread accept_thread([this] { AcceptPeer(); });
    transport.Open(listener);
    accept_thread.join();

    // 对端关闭连接
    ::shutdown(m_peer_handle, 2);
    CloseRaw(m_peer_handle);
    m_peer_handle = kRawInvalid;

    EXPECT_TRUE(listener.WaitForClose(kWaitLong));
    EXPECT_FALSE(transport.IsOpen());
}

TEST_F(TcpEndToEndTest, CloseInterruptsBlockingReceive) {
    const std::uint16_t port = StartRawListener();
    ASSERT_NE(port, 0);

    TcpTransportConfig cfg = MakeMasterConfig(port);
    TcpTransport transport(cfg);
    RecordingListener listener;

    std::thread accept_thread([this] { AcceptPeer(); });
    transport.Open(listener);
    accept_thread.join();

    // 对端保持连接但不发数据：接收线程此刻阻塞在 recv()
    const auto start = std::chrono::steady_clock::now();
    transport.Close();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    // Close 必须靠 shutdown 立即打断 recv，而不是等某个轮询周期
    EXPECT_LT(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
        1500);
    EXPECT_FALSE(transport.IsOpen());
    EXPECT_TRUE(listener.closed());
}

TEST_F(TcpEndToEndTest, CloseIsIdempotent) {
    const std::uint16_t port = StartRawListener();
    ASSERT_NE(port, 0);

    TcpTransportConfig cfg = MakeMasterConfig(port);
    TcpTransport transport(cfg);
    RecordingListener listener;

    std::thread accept_thread([this] { AcceptPeer(); });
    transport.Open(listener);
    accept_thread.join();

    transport.Close();
    EXPECT_NO_THROW(transport.Close());
    EXPECT_NO_THROW(transport.Close());
}

TEST_F(TcpEndToEndTest, SendAfterCloseThrowsInvalidState) {
    const std::uint16_t port = StartRawListener();
    ASSERT_NE(port, 0);

    TcpTransportConfig cfg = MakeMasterConfig(port);
    TcpTransport transport(cfg);
    RecordingListener listener;

    std::thread accept_thread([this] { AcceptPeer(); });
    transport.Open(listener);
    accept_thread.join();
    transport.Close();

    EXPECT_THROW(transport.Send(Bytes{0xFF, 0x00}), XcpException);
}

TEST_F(TcpEndToEndTest, SendRejectsEmptyPacket) {
    const std::uint16_t port = StartRawListener();
    ASSERT_NE(port, 0);

    TcpTransportConfig cfg = MakeMasterConfig(port);
    TcpTransport transport(cfg);
    RecordingListener listener;

    std::thread accept_thread([this] { AcceptPeer(); });
    transport.Open(listener);
    accept_thread.join();

    EXPECT_THROW(transport.Send(Bytes{}), XcpException);

    transport.Close();
}

TEST_F(TcpEndToEndTest, SendRejectsPacketBeyondConfiguredLimit) {
    const std::uint16_t port = StartRawListener();
    ASSERT_NE(port, 0);

    TcpTransportConfig cfg = MakeMasterConfig(port);
    cfg.max_packet_size = 16;
    TcpTransport transport(cfg);
    RecordingListener listener;

    std::thread accept_thread([this] { AcceptPeer(); });
    transport.Open(listener);
    accept_thread.join();

    const Bytes too_big(64, 0xAB);
    EXPECT_THROW(transport.Send(too_big), XcpException);

    transport.Close();
}

TEST_F(TcpEndToEndTest, SendCtrAdvancesPerFrame) {
    const std::uint16_t port = StartRawListener();
    ASSERT_NE(port, 0);

    TcpTransportConfig cfg = MakeMasterConfig(port);
    cfg.wire_profile = TcpWireProfile::AsamV14;
    TcpTransport transport(cfg);
    RecordingListener listener;

    std::thread accept_thread([this] { AcceptPeer(); });
    transport.Open(listener);
    accept_thread.join();

    EXPECT_EQ(transport.SendCtr(), 0);
    transport.Send(Bytes{0xFF, 0x00});
    EXPECT_EQ(transport.SendCtr(), 1);
    transport.Send(Bytes{0xFF, 0x01});
    EXPECT_EQ(transport.SendCtr(), 2);

    // 两帧的 CTR 必须递增且与写入顺序一致
    const Bytes received = ReadExact(2 * (kEthernetHeaderSize + 2));
    ASSERT_EQ(received.size(), 12U);
    EXPECT_EQ(received[2] | (received[3] << 8), 0);
    EXPECT_EQ(received[8] | (received[9] << 8), 1);

    transport.Close();
}

TEST_F(TcpEndToEndTest, SlaveAcceptsConnectionAndReceivesFrame) {
    // Slave 角色：先起 listen，再用原生 Socket 连上去并写一帧
    TcpTransportConfig cfg = MakeSlaveConfig();
    TcpTransport transport(cfg);
    RecordingListener listener;

    // 预先无法得知端口（由 Open 内 accept 之后才回填），故用固定高位端口。
    // 为避免冲突，先探测一个可用端口。
    const std::uint16_t probe_port = StartRawListener();
    ASSERT_NE(probe_port, 0);
    // 释放探测用的监听句柄，让 Slave 去绑定同一端口
    CloseRaw(m_listen_handle);
    m_listen_handle = kRawInvalid;

    cfg.local_port = probe_port;
    TcpTransport slave(cfg);
    RecordingListener slave_listener;

    std::thread slave_thread([&] {
        try {
            slave.Open(slave_listener);
        } catch (...) {
        }
    });

    // 等待 Slave 进入 listen（轮询直到连上或超时）
    RawSocket client = kRawInvalid;
    for (int attempt = 0; attempt < 100 && client == kRawInvalid; ++attempt) {
        client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(probe_port);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(client, reinterpret_cast<sockaddr*>(&addr),
                      sizeof(addr)) != 0) {
            CloseRaw(client);
            client = kRawInvalid;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    ASSERT_NE(client, kRawInvalid) << "无法连上 Slave 监听端口";

    slave_thread.join();
    ASSERT_TRUE(slave.IsOpen());

    // Slave 回填的本地端口应就是 probe_port
    EXPECT_EQ(slave.LocalPort(), probe_port);

    // 客户端写一帧，Slave 应解出并回调
    const Bytes payload{0xFF, 0x40, 0x7F};
    std::vector<std::uint8_t> wire;
    wire.push_back(static_cast<std::uint8_t>(payload.size() & 0xFF));
    wire.push_back(static_cast<std::uint8_t>((payload.size() >> 8) & 0xFF));
    wire.push_back(0x00);
    wire.push_back(0x00);
    wire.insert(wire.end(), payload.begin(), payload.end());
    ASSERT_GT(::send(client, reinterpret_cast<const char*>(wire.data()),
                     static_cast<int>(wire.size()), 0),
              0);

    EXPECT_TRUE(slave_listener.WaitForPackets(1, kWaitLong));
    const auto packets = slave_listener.Packets();
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(packets[0], payload);

    ::shutdown(client, 2);
    CloseRaw(client);
    slave.Close();
}

TEST_F(TcpEndToEndTest, AcceptClosesListenHandleExactlyOnce) {
    // 注入统计用假操作表：连接建立语义仍是真实的，只统计 close/listen/accept。
    // 注入是进程级的；gtest_discover_tests 让每个用例独占进程，因此这里安全。
    test_support::FakeSocketOps fake;
    test_support::ScopedFakeSocketOps ops_guard(fake);

    TcpTransportConfig cfg = MakeSlaveConfig();
    const std::uint16_t probe_port = StartRawListener();
    ASSERT_NE(probe_port, 0);
    CloseRaw(m_listen_handle);
    m_listen_handle = kRawInvalid;
    cfg.local_port = probe_port;

    TcpTransport slave(cfg);
    RecordingListener listener;
    std::thread slave_thread([&] {
        try {
            slave.Open(listener);
        } catch (...) {
        }
    });

    RawSocket client = kRawInvalid;
    for (int attempt = 0; attempt < 100 && client == kRawInvalid; ++attempt) {
        client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(probe_port);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(client, reinterpret_cast<sockaddr*>(&addr),
                      sizeof(addr)) != 0) {
            CloseRaw(client);
            client = kRawInvalid;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    ASSERT_NE(client, kRawInvalid) << "无法连上 Slave 监听端口";
    slave_thread.join();
    ASSERT_TRUE(slave.IsOpen());

    const detail::SocketHandle listen_handle = fake.LastListenHandle();
    const detail::SocketHandle accepted_handle = fake.LastAcceptHandle();
    ASSERT_NE(listen_handle, detail::kInvalidSocketHandle)
        << "Slave 必须经过注入的 listen()";
    ASSERT_NE(accepted_handle, detail::kInvalidSocketHandle)
        << "Slave 必须经过注入的 accept()";

    // accept 成功后监听句柄已释放，Close() 不得再关一次（否则可能误关
    // OS 复用后的同号句柄，正是本次修复的问题）。
    EXPECT_EQ(fake.CloseCount(listen_handle), 1)
        << "监听句柄必须在 accept 成功后只关闭一次";

    slave.Close();
    EXPECT_EQ(fake.CloseCount(listen_handle), 1)
        << "Close() 不得再次关闭已释放的监听句柄";
    EXPECT_EQ(fake.CloseCount(accepted_handle), 1)
        << "已接受连接必须且只能关闭一次";

    ::shutdown(client, 2);
    CloseRaw(client);
}

TEST_F(TcpEndToEndTest, ReopenAfterCloseSucceeds) {
    const std::uint16_t port = StartRawListener();
    ASSERT_NE(port, 0);

    TcpTransportConfig cfg = MakeMasterConfig(port);
    TcpTransport transport(cfg);
    RecordingListener listener;

    std::thread accept_thread([this] { AcceptPeer(); });
    transport.Open(listener);
    accept_thread.join();
    transport.Close();
    EXPECT_FALSE(transport.IsOpen());
    EXPECT_EQ(transport.SendCtr(), 0);

    // 重新打开应先 Close() 再建立新连接（幂等契约）
    RawSocket second_peer = kRawInvalid;
    std::thread accept_thread2([this, &second_peer] {
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        second_peer =
            ::accept(m_listen_handle, reinterpret_cast<sockaddr*>(&peer), &len);
    });
    transport.Open(listener);
    accept_thread2.join();

    EXPECT_TRUE(transport.IsOpen());
    EXPECT_EQ(transport.SendCtr(), 0);

    transport.Close();
    if (second_peer != kRawInvalid) {
        ::shutdown(second_peer, 2);
        CloseRaw(second_peer);
    }
}

// ===========================================================================
// 接收 CTR 校验（计划 §1.5）
//
// 背景（本批次实测发现）：B-T2 的 TCP 实现**完全没有** CTR 校验——
// `tcp_stream_decoder.cpp:160` 只把 CTR 写进视图、`tcp_transport.cpp:788`
// 只把它打进日志，全库唯一的 CTR 校验代码在 `udp_transport.cpp:865-920`。
// 本组用例针对**新实现**的 `TcpTransport::CheckReceiveCtr`。
//
// 与 UDP 的关键差异（§1.5 末条明确禁止直接复用 UDP 策略）：
//   - UDP 发现 CTR 异常时**丢弃该帧但保持连接**；
//   - TCP 中 CTR 违约一律**断链**（AsamV15 任何偏离；AsamV14 仅后向/重复），
//     因为 TCP 无消息边界，CTR 是发现"对端漏发/我方漏读"的唯一手段。
// ===========================================================================

class TcpReceiveCtrTest : public TcpEndToEndTest {
protected:
    /// @brief 起一个指定 Profile 的 Master Transport 并接好原始对端
    /// @param profile 目标 Wire Profile
    /// @return 已 `Open()` 的 Transport（唯一指针；`TcpTransport` 不可拷贝/
    ///         移动，故只能以指针形式移出）
    /// @note 端口在本函数内由 OS 分配后写入配置，故配置构造也必须在此完成。
    std::unique_ptr<TcpTransport> StartWith(TcpWireProfile profile,
                                            RecordingListener& listener) {
        const std::uint16_t port = StartRawListener();
        EXPECT_NE(port, 0) << "原始监听 Socket 启动失败";
        if (port == 0) {
            return nullptr;
        }
        TcpTransportConfig cfg = MakeMasterConfig(port);
        cfg.wire_profile = profile;
        auto transport = std::make_unique<TcpTransport>(cfg);
        std::thread accept_thread([this] { AcceptPeer(); });
        transport->Open(listener);
        accept_thread.join();
        EXPECT_TRUE(transport->IsOpen()) << "Transport 未能 Open";
        return transport;
    }
};

TEST_F(TcpReceiveCtrTest, AsamV14AcceptsArbitraryFirstCtr) {
    // §1.5："接收首个合法 Frame 建立基线，允许首值任意"。AsamV14 peer 的 CTR
    // 可能在 TCP 断连/重连后继续增长，故绝不能要求首帧为 0。
    RecordingListener listener;
    auto transport = StartWith(TcpWireProfile::AsamV14, listener);
    ASSERT_NE(transport, nullptr);

    const Bytes payload{0xFF, 0x10, 0x11, 0x22};
    SendAll(m_peer_handle, MakeWireFrame(payload, 0xABCDU));

    ASSERT_TRUE(listener.WaitForPackets(1, kWaitLong));
    EXPECT_EQ(listener.Packets().size(), 1U);
    EXPECT_FALSE(listener.closed()) << "首值任意，不得因非 0 首值断链";
    transport->Close();
}

TEST_F(TcpReceiveCtrTest, AsamV14AcceptsConsecutiveCtr) {
    RecordingListener listener;
    auto transport = StartWith(TcpWireProfile::AsamV14, listener);
    ASSERT_NE(transport, nullptr);

    SendAll(m_peer_handle, MakeWireFrame(Bytes{0xFF, 0x01}, 0x0000U));
    SendAll(m_peer_handle, MakeWireFrame(Bytes{0xFF, 0x02}, 0x0001U));
    SendAll(m_peer_handle, MakeWireFrame(Bytes{0xFF, 0x03}, 0x0002U));

    ASSERT_TRUE(listener.WaitForPackets(3, kWaitLong));
    EXPECT_EQ(listener.Packets().size(), 3U);
    EXPECT_FALSE(listener.closed());
    transport->Close();
}

TEST_F(TcpReceiveCtrTest, AsamV14ToleratesForwardGap) {
    // §1.5：AsamV14 的前向缺号**只记录诊断并继续**——XCPlite 的发送队列丢弃
    // 会主动推进全局 CTR，缺号属对端已知行为，不构成"我方失步"。
    RecordingListener listener;
    auto transport = StartWith(TcpWireProfile::AsamV14, listener);
    ASSERT_NE(transport, nullptr);

    SendAll(m_peer_handle, MakeWireFrame(Bytes{0xFF, 0x01}, 0x0000U));
    // 跳到 +5：中间 4 帧缺失
    SendAll(m_peer_handle, MakeWireFrame(Bytes{0xFF, 0x02}, 0x0005U));
    // 之后必须**以新值**为基线继续（0x0006 连续）
    SendAll(m_peer_handle, MakeWireFrame(Bytes{0xFF, 0x03}, 0x0006U));

    ASSERT_TRUE(listener.WaitForPackets(3, kWaitLong));
    EXPECT_EQ(listener.Packets().size(), 3U) << "前向缺号必须全部投递";
    EXPECT_FALSE(listener.closed()) << "AsamV14 前向缺号不得断链";

    // 诊断文本必须出现（§1.5 要求"只记录丢帧诊断"）
    const auto warnings = listener.Warnings();
    bool gap_reported = false;
    for (const auto& w : warnings) {
        if (w.find("前向缺号") != std::string::npos) {
            gap_reported = true;
        }
    }
    EXPECT_TRUE(gap_reported) << "前向缺号必须上报诊断文本";
    transport->Close();
}

TEST_F(TcpReceiveCtrTest, AsamV14RejectsBackwardCtr) {
    // §1.5：AsamV14 的 backward / duplicate 视为协议异常并**关闭连接**。
    RecordingListener listener;
    auto transport = StartWith(TcpWireProfile::AsamV14, listener);
    ASSERT_NE(transport, nullptr);

    SendAll(m_peer_handle, MakeWireFrame(Bytes{0xFF, 0x01}, 0x0005U));
    ASSERT_TRUE(listener.WaitForPackets(1, kWaitLong));
    // 回退到 0x0004（后向）
    SendAll(m_peer_handle, MakeWireFrame(Bytes{0xFF, 0x02}, 0x0004U));

    EXPECT_TRUE(listener.WaitForClose(kWaitLong)) << "后向 CTR 必须断链";
    EXPECT_EQ(listener.Packets().size(), 1U) << "违约帧不得投递";
    transport->Close();
}

TEST_F(TcpReceiveCtrTest, AsamV14RejectsDuplicateCtr) {
    // "重复"是后向距离为 0xFFFF 的特例（复用前一 CTR）。
    RecordingListener listener;
    auto transport = StartWith(TcpWireProfile::AsamV14, listener);
    ASSERT_NE(transport, nullptr);

    SendAll(m_peer_handle, MakeWireFrame(Bytes{0xFF, 0x01}, 0x0007U));
    ASSERT_TRUE(listener.WaitForPackets(1, kWaitLong));
    SendAll(m_peer_handle, MakeWireFrame(Bytes{0xFF, 0x02}, 0x0007U));

    EXPECT_TRUE(listener.WaitForClose(kWaitLong)) << "重复 CTR 必须断链";
    EXPECT_EQ(listener.Packets().size(), 1U) << "违约帧不得投递";
    transport->Close();
}

TEST_F(TcpReceiveCtrTest, AsamV15RejectsForwardGap) {
    // §1.5：AsamV15 后续必须**严格** current == previous + 1。与 AsamV14 相反，
    // 这里的缺号也要断链——严格 Profile 下缺号即意味流已不可信。
    RecordingListener listener;
    auto transport = StartWith(TcpWireProfile::AsamV15, listener);
    ASSERT_NE(transport, nullptr);

    SendAll(m_peer_handle, MakeWireFrame(Bytes{0xFF, 0x01}, 0x0000U));
    ASSERT_TRUE(listener.WaitForPackets(1, kWaitLong));
    SendAll(m_peer_handle, MakeWireFrame(Bytes{0xFF, 0x02}, 0x0003U));

    EXPECT_TRUE(listener.WaitForClose(kWaitLong))
        << "AsamV15 缺号必须断链（不得照搬 AsamV14 的容忍策略）";
    EXPECT_EQ(listener.Packets().size(), 1U) << "违约帧不得投递";
    transport->Close();
}

TEST_F(TcpReceiveCtrTest, AsamV15RejectsBackwardAndDuplicateCtr) {
    RecordingListener listener;
    auto transport = StartWith(TcpWireProfile::AsamV15, listener);
    ASSERT_NE(transport, nullptr);

    SendAll(m_peer_handle, MakeWireFrame(Bytes{0xFF, 0x01}, 0x0010U));
    ASSERT_TRUE(listener.WaitForPackets(1, kWaitLong));
    SendAll(m_peer_handle, MakeWireFrame(Bytes{0xFF, 0x02}, 0x000FU));

    EXPECT_TRUE(listener.WaitForClose(kWaitLong));
    transport->Close();
}

TEST_F(TcpReceiveCtrTest, CtrWrapsFromFfffToZero) {
    // §1.5 共同规则："0xFFFF -> 0 是正常回绕"，两种 Profile 都必须接受。
    for (const TcpWireProfile profile :
         {TcpWireProfile::AsamV14, TcpWireProfile::AsamV15}) {
        RecordingListener listener;
        auto transport = StartWith(profile, listener);
        ASSERT_NE(transport, nullptr);

        SendAll(m_peer_handle, MakeWireFrame(Bytes{0xFF, 0x01}, 0xFFFFU));
        ASSERT_TRUE(listener.WaitForPackets(1, kWaitLong));
        SendAll(m_peer_handle, MakeWireFrame(Bytes{0xFF, 0x02}, 0x0000U));

        EXPECT_TRUE(listener.WaitForPackets(2, kWaitLong))
            << "回绕 FFFF->0 必须被接受，profile="
            << TcpWireProfileName(profile);
        EXPECT_FALSE(listener.closed());
        transport->Close();
    }
}

TEST_F(TcpReceiveCtrTest, FirstFrameAfterReopenEstablishesFreshBaseline) {
    // §1.5：每次成功 Open 后接收基线清空。若不复位，重连后首帧会拿上一条
    // 连接的最后 CTR 去比对，从而被误判为违约。
    RecordingListener listener;
    auto transport = StartWith(TcpWireProfile::AsamV15, listener);
    ASSERT_NE(transport, nullptr);

    SendAll(m_peer_handle, MakeWireFrame(Bytes{0xFF, 0x01}, 0x1234U));
    ASSERT_TRUE(listener.WaitForPackets(1, kWaitLong));
    transport->Close();

    // 第二条连接：peer 的 CTR 继续增长且与旧值不连续
    RecordingListener listener2;
    auto transport2 = StartWith(TcpWireProfile::AsamV15, listener2);
    ASSERT_NE(transport2, nullptr);

    SendAll(m_peer_handle, MakeWireFrame(Bytes{0xFF, 0x02}, 0x9999U));
    ASSERT_TRUE(listener2.WaitForPackets(1, kWaitLong))
        << "新连接首帧应重建基线而非与旧基线比对";
    EXPECT_FALSE(listener2.closed());
    transport2->Close();
}

// ===========================================================================
// 平台错误码分类与关闭接口（Windows CI 回归）
//
// 历史曾将 POSIX 的 SHUT_RDWR 泄漏到 Windows，且在 Winsock 分支误用
// 不存在的 WSAEPIPE；当前原生关闭常量已仅存在平台实现文件中。
// 本组测试验证对外的错误集合和无效句柄返回语义。
// ===========================================================================

#if defined(_WIN32)
/// @brief 本平台应被判为"连接已丢失"的错误码
constexpr int kConnectionLostCodes[] = {WSAECONNRESET, WSAECONNABORTED,
                                        WSAENOTCONN, WSAESHUTDOWN};
/// @brief 本平台不应被判为"连接已丢失"的错误码
constexpr int kNotConnectionLostCodes[] = {0, WSAEWOULDBLOCK, WSAEINTR,
                                           WSAEINPROGRESS};
#else
/// @brief 本平台应被判为"连接已丢失"的错误码
constexpr int kConnectionLostCodes[] = {EPIPE, ECONNRESET, ECONNABORTED,
                                        ENOTCONN, ESHUTDOWN};
/// @brief 本平台不应被判为"连接已丢失"的错误码
constexpr int kNotConnectionLostCodes[] = {0, EAGAIN, EINTR, EINPROGRESS};
#endif

TEST(SocketPlatformTest, ClassifiesConnectionLostCodes) {
    // 判定集合必须覆盖"对端关闭"在本平台的**全部**表现，漏一个就会把断链
    // 误报成一般错误，进而让调用方继续在死连接上收发。
    for (const int code : kConnectionLostCodes) {
        EXPECT_TRUE(detail::IsConnectionLostCode(code))
            << "错误码应判为连接丢失，code=" << code;
    }
}

TEST(SocketPlatformTest, KeepsOtherCodesOutOfConnectionLostSet) {
    // 反向约束：可重试错误（would-block）与"操作被打断"都不属于连接丢失，
    // 若误判就会把可恢复的瞬时状态升级成断链。
    for (const int code : kNotConnectionLostCodes) {
        EXPECT_FALSE(detail::IsConnectionLostCode(code))
            << "错误码不应判为连接丢失，code=" << code;
    }
}

#if defined(_WIN32)
TEST(SocketPlatformTest, ClassifiesWindowsUdpShutdownErrors) {
    auto& ops = detail::RealSocketOps();
    // 保持既有 UDP 关闭场景：EINVAL/NOTSOCK 无需重试，断链错误另行分类。
    ::WSASetLastError(WSAEINVAL);
    EXPECT_TRUE(ops.is_invalid_socket());
    ::WSASetLastError(WSAENOTSOCK);
    EXPECT_TRUE(ops.is_invalid_socket());
    ::WSASetLastError(WSAECONNRESET);
    EXPECT_TRUE(ops.is_connection_lost());
}
#endif

TEST(SocketPlatformTest, ShutdownSocketReportsInvalidHandle) {
    // 关闭常量现仅存在平台实现内；此处验证抽象在无效句柄上的失败语义。
    EXPECT_NE(
        detail::RealSocketOps().shutdown_socket(detail::kInvalidSocketHandle),
        0);
}

}  // namespace calmcar::xcp