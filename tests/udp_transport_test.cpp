/**
 * @file udp_transport_test.cpp
 * @brief UdpTransport 端到端测试：真实 Loopback Socket、CTR
 * 策略、来源过滤与关闭语义。
 *
 * 覆盖计划文档 §9.3 与设计文档 §15.3 第 2/4/5 条。测试不依赖固定端口。
 */

#include "libxcp/udp_transport.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
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

#include "libxcp/udp_header_codec.hpp"
#include "udp_test_slave.hpp"

namespace calmcar::xcp {
namespace {

/// @brief 无效 Socket 句柄的跨平台表示
#if defined(_WIN32)
constexpr auto kInvalidHandle = INVALID_SOCKET;
#else
constexpr auto kInvalidHandle = -1;
#endif

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

    /// @brief 等待收到第 expected_count 个 Packet
    bool WaitForPackets(std::size_t expected_count,
                        std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex_);
        return m_cv_.wait_for(lock, timeout, [&] {
            return m_packets_.size() >= expected_count || m_closed_;
        });
    }

    bool WaitForClose(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex_);
        return m_cv_.wait_for(lock, timeout, [&] { return m_closed_; });
    }

    bool WaitForWarnings(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex_);
        return m_cv_.wait_for(lock, timeout,
                              [&] { return m_warnings_.size() >= count; });
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

private:
    mutable std::mutex m_mutex_;
    std::condition_variable m_cv_;
    std::vector<Bytes> m_packets_;
    std::vector<std::string> m_warnings_;
    bool m_closed_{false};
    std::string m_close_reason_;
};

/// @brief 构造指向测试 Slave 的配置（Loopback + OS 分配本地端口）
UdpTransportConfig MakeConfig(std::uint16_t slave_port) {
    UdpTransportConfig cfg;
    cfg.m_remote_host_ = "127.0.0.1";
    cfg.m_remote_port_ = slave_port;
    cfg.m_local_host_ = "127.0.0.1";
    cfg.m_local_port_ = 0;
    cfg.m_receive_poll_interval_ms_ = 20;
    return cfg;
}

/// @brief 原始 UDP Sender：向指定目的端口注入任意 Datagram / 伪造来源端点
class RawSender {
public:
    explicit RawSender(std::uint16_t dst_port) : m_dst_port_(dst_port) {
#if defined(_WIN32)
        WSADATA data;
        m_wsa_ok_ = (::WSAStartup(MAKEWORD(2, 2), &data) == 0);
#endif
        m_handle_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        // 构造函数不能用 ASSERT_*（宏要求 void 返回），用标志位表达失败
        m_valid_ = (m_handle_ != kInvalidHandle);
    }

    /// @brief Socket 是否创建成功
    [[nodiscard]] bool valid() const noexcept { return m_valid_; }

    ~RawSender() {
        if (m_valid_) {
#if defined(_WIN32)
            ::closesocket(m_handle_);
#else
            ::close(m_handle_);
#endif
        }
#if defined(_WIN32)
        if (m_wsa_ok_) {
            ::WSACleanup();
        }
#endif
    }

    /// @brief 绑定到指定源 IP/端口后发送（模拟特定来源端点）
    void sendFrom(BytesView datagram, std::uint16_t src_port,
                  const char* src_ip = "127.0.0.1") {
        if (!m_valid_) {
            ADD_FAILURE() << "RawSender Socket 不可用";
            return;
        }
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(src_port);
        ::inet_pton(AF_INET, src_ip, &local.sin_addr);
        if (::bind(m_handle_, reinterpret_cast<sockaddr*>(&local),
                   sizeof(local)) != 0) {
            ADD_FAILURE() << "RawSender 绑定源端点失败";
            return;
        }
        send(datagram);
    }

    void send(BytesView datagram) {
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(m_dst_port_);
        dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::sendto(m_handle_, reinterpret_cast<const char*>(datagram.data()),
                 static_cast<int>(datagram.size()), 0,
                 reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    }

private:
    std::uint16_t m_dst_port_;
#if defined(_WIN32)
    bool m_wsa_ok_{false};
#endif
    bool m_valid_{false};
    decltype(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) m_handle_{};
};

Bytes BytesOf(std::initializer_list<std::uint8_t> init) {
    return Bytes(init.begin(), init.end());
}

// --------------------------------------------------------------------------
// 生命周期与配置校验
// --------------------------------------------------------------------------

TEST(UdpTransportLifecycle, OpenThenCloseTransitionsAndNotifies) {
    test::UdpTestSlave slave;
    slave.Start();
    UdpTransport transport(MakeConfig(slave.Port()));
    RecordingListener listener;

    EXPECT_FALSE(transport.IsOpen());
    transport.Open(listener);
    EXPECT_TRUE(transport.IsOpen());

    transport.Close();
    EXPECT_FALSE(transport.IsOpen());
    EXPECT_TRUE(listener.WaitForClose(std::chrono::seconds(2)));
    // close 幂等：二次调用不得再次回调或抛异常
    EXPECT_NO_THROW(transport.Close());
    slave.Stop();
}

TEST(UdpTransportLifecycle, DestructorClosesWithoutThrowing) {
    test::UdpTestSlave slave;
    slave.Start();
    {
        UdpTransport transport(MakeConfig(slave.Port()));
        RecordingListener listener;
        transport.Open(listener);
    }  // 析构自动 close
    slave.Stop();
}

TEST(UdpTransportConfigValidation, RejectsBadConfigs) {
    RecordingListener listener;

    {
        auto cfg = MakeConfig(0);  // remotePort == 0
        UdpTransport t(cfg);
        EXPECT_THROW(t.Open(listener), XcpException);
    }
    {
        auto cfg = MakeConfig(40000);
        cfg.m_max_frame_packet_size_ = 0;
        UdpTransport t(cfg);
        EXPECT_THROW(t.Open(listener), XcpException);
    }
    {
        auto cfg = MakeConfig(40000);
        cfg.m_max_datagram_size_ = 3;  // 小于 Header + 1
        UdpTransport t(cfg);
        EXPECT_THROW(t.Open(listener), XcpException);
    }
    {
        auto cfg = MakeConfig(40000);
        cfg.m_remote_host_ = "not-an-ip";
        UdpTransport t(cfg);
        EXPECT_THROW(t.Open(listener), XcpException);
    }
    {
        auto cfg = MakeConfig(40000);
        cfg.m_local_host_ = "999.1.1.1";
        UdpTransport t(cfg);
        EXPECT_THROW(t.Open(listener), XcpException);
    }
}

TEST(UdpTransportConfigValidation, SendBeforeOpenIsInvalidState) {
    UdpTransport transport(MakeConfig(40001));
    const Bytes cmd = BytesOf({0xFF, 0x00});
    EXPECT_THROW(transport.Send(BytesView{cmd}), XcpException);
    try {
        transport.Send(BytesView{cmd});
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidState);
    }
}

// --------------------------------------------------------------------------
// 发送方向：单 Frame / 单 Datagram，CTR 每 Frame 递增
// --------------------------------------------------------------------------

TEST(UdpTransportSend, EachSendProducesOneFrameWithIncrementingCtr) {
    test::UdpTestSlave slave;
    slave.Start();
    UdpTransport transport(MakeConfig(slave.Port()));
    RecordingListener listener;
    transport.Open(listener);

    EXPECT_EQ(transport.SendCtr(), 0);
    const Bytes cmd = BytesOf({0xFF, 0x00});
    transport.Send(BytesView{cmd});
    transport.Send(BytesView{cmd});
    EXPECT_EQ(transport.SendCtr(), 2);  // 两个 Frame 消耗两个 CTR

    ASSERT_TRUE(listener.WaitForPackets(2, std::chrono::seconds(2)));
    // Slave 对 CONNECT 回 RES；这里只验证 Master 侧收到的都是完整 XCP
    // Packet（无 Header 残留）
    for (const auto& pkt : listener.Packets()) {
        EXPECT_GE(pkt.size(), 2U);
        EXPECT_EQ(pkt[0], static_cast<std::uint8_t>(PacketType::Res));
    }
    transport.Close();
    slave.Stop();
}

TEST(UdpTransportSend, EnforcesMaxFramePacketSize) {
    test::UdpTestSlave slave;
    slave.Start();
    auto cfg = MakeConfig(slave.Port());
    cfg.m_max_frame_packet_size_ = 4;
    UdpTransport transport(cfg);
    RecordingListener listener;
    transport.Open(listener);

    const Bytes too_long(5, 0x00);
    EXPECT_THROW(transport.Send(BytesView{too_long}), XcpException);
    const Bytes fits = BytesOf({0xFD, 0x00, 0x00, 0x00});
    EXPECT_NO_THROW(transport.Send(BytesView{fits}));
    EXPECT_THROW(transport.Send(BytesView{}), XcpException);  // 空 Packet 非法
    transport.Close();
    slave.Stop();
}

// --------------------------------------------------------------------------
// 接收方向：多 Frame Datagram、CTR 策略、来源过滤
// --------------------------------------------------------------------------

TEST(UdpTransportReceive, MultipleFramesInOneDatagramDeliveredInOrder) {
    test::UdpTestSlave slave;
    slave.Start();
    UdpTransport transport(MakeConfig(slave.Port()));
    RecordingListener listener;
    transport.Open(listener);

    // 先 CONNECT 建立 Slave 会话（同时让 Slave 记录来源端点）
    const Bytes connect = BytesOf({0xFF, 0x00});
    transport.Send(BytesView{connect});
    ASSERT_TRUE(listener.WaitForPackets(1, std::chrono::seconds(2)));
    const std::size_t baseline = listener.Packets().size();

    // 设计 §15.3 第 4 条：一个 Datagram 内 EV + RES，以及两个连续 RES/ERR Frame
    const Bytes ev = BytesOf({0xFD, 0x05});
    const Bytes res = BytesOf({0xFF, 0xE0, 0x11});
    const std::vector<BytesView> packed{BytesView{ev}, BytesView{res}};
    slave.SendPackedFrames(packed);

    ASSERT_TRUE(listener.WaitForPackets(baseline + 2, std::chrono::seconds(2)));
    const auto got = listener.Packets();
    ASSERT_GE(got.size(), baseline + 2);
    EXPECT_EQ(got[baseline], ev);  // 顺序必须保持
    EXPECT_EQ(got[baseline + 1], res);
    transport.Close();
    slave.Stop();
}

TEST(UdpTransportReceive, CorruptSecondFrameDropsWholeDatagram) {
    test::UdpTestSlave slave;
    slave.Start();

    UdpTransport transport(MakeConfig(slave.Port()));
    RecordingListener listener;
    transport.Open(listener);

    // 先 CONNECT，让 Slave 记录 Master 的端点（只有 Slave
    // 自身能从"配置的远端端口"发包）
    transport.Send(BytesView{BytesOf({0xFF, 0x00})});
    ASSERT_TRUE(listener.WaitForPackets(1, std::chrono::seconds(2)));
    const std::size_t baseline = listener.Packets().size();

    // 第一个 Frame 合法（EV），第二个 Frame LEN 被篡改越界 -> 整个 Datagram
    // 必须不交付
    const Bytes good = BytesOf({0xFD, 0x05});
    const Bytes bad_second = BytesOf({0xFF, 0xE0});
    auto f1 = EncodeUdpFrame(BytesView{good}, 300).m_data_;
    auto f2 = EncodeUdpFrame(BytesView{bad_second}, 301).m_data_;
    f2[0] = 0xFF;  // LEN := 255，明显超出剩余字节
    Bytes datagram = f1;
    datagram.insert(datagram.end(), f2.begin(), f2.end());

    slave.SendRawPayload(BytesView{datagram});

    EXPECT_TRUE(listener.WaitForWarnings(1, std::chrono::seconds(2)));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(listener.Packets().size(), baseline)
        << "畸形 Datagram 的部分内容不得交付";
    bool atomic_drop = false;
    for (const auto& w : listener.Warnings()) {
        if (w.find("整体丢弃") != std::string::npos) {
            atomic_drop = true;
        }
    }
    EXPECT_TRUE(atomic_drop) << "未观察到整包丢弃诊断";
    transport.Close();
    slave.Stop();
}

TEST(UdpTransportReceive, LenCorruptionDiscardsDatagram) {
    test::UdpTestSlave slave;
    slave.Start();
    UdpTransport transport(MakeConfig(slave.Port()));
    RecordingListener listener;
    transport.Open(listener);

    const Bytes connect = BytesOf({0xFF, 0x00});
    transport.Send(BytesView{connect});
    ASSERT_TRUE(listener.WaitForPackets(1, std::chrono::seconds(2)));

    // 第 1 个响应（setFaultInjection 会重置计数）使用错误
    // LEN：该响应不得交付上层，并产生警告
    test::FaultInjection fault;
    fault.m_corrupt_len_n_ = 1;
    slave.SetFaultInjection(fault);

    const std::size_t baseline = listener.Packets().size();
    const Bytes get_status = BytesOf({0xFD, 0x00});
    transport.Send(BytesView{get_status});

    EXPECT_TRUE(listener.WaitForWarnings(1, std::chrono::seconds(2)));
    // 稍等确认没有把畸形包交付上去
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(listener.Packets().size(), baseline);
    transport.Close();
    slave.Stop();
}

TEST(UdpTransportReceive, ForwardJumpIsAcceptedWithGapWarning) {
    test::UdpTestSlave slave;
    slave.Start();
    UdpTransport transport(MakeConfig(slave.Port()));
    RecordingListener listener;
    transport.Open(listener);

    const Bytes connect = BytesOf({0xFF, 0x00});
    transport.Send(BytesView{connect});
    ASSERT_TRUE(listener.WaitForPackets(1, std::chrono::seconds(2)));

    test::FaultInjection fault;
    fault.m_jump_ctr_n_ = 1;  // 第 1 个响应（计数已重置）的 CTR 前跳
    slave.SetFaultInjection(fault);

    const std::size_t baseline = listener.Packets().size();
    const Bytes get_status = BytesOf({0xFD, 0x00});
    transport.Send(BytesView{get_status});

    // 前向跳号：仍应交付（§8.1 第 4 条），同时报告缺口
    EXPECT_TRUE(listener.WaitForPackets(baseline + 1, std::chrono::seconds(2)));
    EXPECT_TRUE(listener.WaitForWarnings(1, std::chrono::seconds(2)));
    bool gap_reported = false;
    for (const auto& w : listener.Warnings()) {
        if (w.find("跳号") != std::string::npos) {
            gap_reported = true;
        }
    }
    EXPECT_TRUE(gap_reported) << "未观察到缺口诊断";
    EXPECT_EQ(transport.LastReceiveCtr().value_or(0),
              6);  // 基线 1 -> 前跳 5 后为 6
    transport.Close();
    slave.Stop();
}

TEST(UdpTransportReceive, DuplicateCtrIsDropped) {
    test::UdpTestSlave slave;
    slave.Start();
    UdpTransport transport(MakeConfig(slave.Port()));
    RecordingListener listener;
    transport.Open(listener);

    const Bytes connect = BytesOf({0xFF, 0x00});
    transport.Send(BytesView{connect});
    ASSERT_TRUE(listener.WaitForPackets(1, std::chrono::seconds(2)));

    test::FaultInjection fault;
    fault.m_duplicate_ctr_n_ = 1;  // 第 1 个响应（计数已重置）重复上一个 CTR
    slave.SetFaultInjection(fault);

    const std::size_t baseline = listener.Packets().size();
    const Bytes get_status = BytesOf({0xFD, 0x00});
    transport.Send(BytesView{get_status});

    EXPECT_TRUE(listener.WaitForWarnings(1, std::chrono::seconds(2)));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(listener.Packets().size(), baseline)
        << "重复 CTR 的 Frame 不应交付";
    transport.Close();
    slave.Stop();
}

TEST(UdpTransportReceive, IgnoresForeignSourceIp) {
    // Master 期望远端为不可达 IP，Loopback 上来的任何 Datagram
    // 都必须被丢弃并诊断。
    constexpr std::uint16_t kFixedLocalPort = 41520;
    auto cfg = MakeConfig(41999);
    cfg.m_remote_host_ = "10.255.255.255";  // 非 Loopback，永不匹配
    cfg.m_local_port_ = kFixedLocalPort;
    UdpTransport transport(cfg);
    RecordingListener listener;
    try {
        transport.Open(listener);
    } catch (const XcpException& e) {
        GTEST_SKIP() << "固定本地端口不可用: " << e.what();
    }

    const Bytes res = BytesOf({0xFF, 0x00});
    const auto frame = EncodeUdpFrame(BytesView{res}, 0);
    RawSender sender(kFixedLocalPort);
    sender.send(
        BytesView{frame.m_data_});  // 来源为 127.0.0.1，与配置的远端 IP 不符

    EXPECT_TRUE(listener.WaitForWarnings(1, std::chrono::seconds(2)));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_TRUE(listener.Packets().empty())
        << "非配置远端 IP 的 Datagram 不得交付";
    transport.Close();
}

TEST(UdpTransportReceive, ForeignPortRejectedWhenStrict) {
    // Master 绑定固定本地端口，使 RawSender 可用错误的源端口发包
    constexpr std::uint16_t kFixedLocalPort = 41500;
    constexpr std::uint16_t kForeignPort = 41999;
    test::UdpTestSlave slave;
    slave.Start();

    auto cfg = MakeConfig(slave.Port());
    cfg.m_local_port_ = kFixedLocalPort;
    cfg.m_strict_remote_port_ = true;

    const Bytes res = BytesOf({0xFF, 0x00});
    const auto frame = EncodeUdpFrame(BytesView{res}, 0);

    {
        UdpTransport transport(cfg);
        RecordingListener listener;
        try {
            transport.Open(listener);
        } catch (const XcpException& e) {
            GTEST_SKIP() << "固定本地端口不可用: " << e.what();
        }
        ASSERT_TRUE(transport.IsOpen());

        // 从非 remotePort 的来源发出 -> 严格模式应丢弃
        RawSender sender(kFixedLocalPort);
        ASSERT_TRUE(sender.valid());
        sender.sendFrom(BytesView{frame.m_data_}, kForeignPort);

        EXPECT_TRUE(listener.WaitForWarnings(1, std::chrono::seconds(2)));
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        EXPECT_TRUE(listener.Packets().empty());
        transport.Close();
    }

    // 放宽为仅匹配 IP 后，同一来源端口的包应被接受
    cfg.m_strict_remote_port_ = false;
    UdpTransport lenient(cfg);
    RecordingListener lenient_listener;
    try {
        lenient.Open(lenient_listener);
    } catch (const XcpException& e) {
        GTEST_SKIP() << "固定本地端口不可用: " << e.what();
    }
    RawSender sender2(kFixedLocalPort);
    ASSERT_TRUE(sender2.valid());
    sender2.sendFrom(BytesView{frame.m_data_}, kForeignPort);
    EXPECT_TRUE(lenient_listener.WaitForPackets(1, std::chrono::seconds(2)));
    EXPECT_EQ(lenient_listener.Packets().size(), 1U);
    lenient.Close();
    slave.Stop();
}

TEST(UdpTransportReceive, DatagramOverMaxSizeDiscarded) {
    // Master 绑定固定本地端口；放宽端口匹配以便 RawSender 能通过来源过滤
    constexpr std::uint16_t kFixedLocalPort = 41530;
    auto cfg = MakeConfig(41999);
    cfg.m_remote_host_ = "127.0.0.1";
    cfg.m_local_port_ = kFixedLocalPort;
    cfg.m_strict_remote_port_ =
        false;  // 仅匹配 IP，便于用任意源端口的原始 Socket 注入
    cfg.m_max_datagram_size_ = 8;  // 只允许极小的 Datagram
    UdpTransport transport(cfg);
    RecordingListener listener;
    try {
        transport.Open(listener);
    } catch (const XcpException& e) {
        GTEST_SKIP() << "固定本地端口不可用: " << e.what();
    }

    const Bytes res = BytesOf({0xFF, 0x00});
    const auto frame = EncodeUdpFrame(BytesView{res}, 0);  // 6 字节：在限制内
    const auto big =
        EncodeUdpFrame(BytesView{Bytes(20, 0x11)}, 1);  // 24 字节：超限

    RawSender sender(kFixedLocalPort);
    ASSERT_TRUE(sender.valid());
    sender.send(BytesView{big.m_data_});
    EXPECT_TRUE(listener.WaitForWarnings(1, std::chrono::seconds(2)));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(listener.Packets().empty()) << "超限 Datagram 不得交付";

    sender.send(BytesView{frame.m_data_});
    EXPECT_TRUE(listener.WaitForPackets(1, std::chrono::seconds(2)));
    EXPECT_EQ(listener.Packets().size(), 1U);
    transport.Close();
}

// --------------------------------------------------------------------------
// CTR 语义细项：回绕、后向乱序、截断 Datagram、绑定失败（计划 §9.3）
// --------------------------------------------------------------------------

TEST(UdpTransportCtrSemantics, ReceiveCtrWrapsFromFfffToZero) {
    constexpr std::uint16_t kMasterPort = 41620;
    test::UdpTestSlave slave;
    slave.Start();

    auto cfg = MakeConfig(slave.Port());
    cfg.m_local_port_ = kMasterPort;
    UdpTransport transport(cfg);
    RecordingListener listener;
    try {
        transport.Open(listener);
    } catch (const XcpException& e) {
        GTEST_SKIP() << "固定本地端口不可用: " << e.what();
    }

    const Bytes a = BytesOf({0xFD, 0x01});
    const Bytes b = BytesOf({0xFD, 0x02});

    // 首个合法 Frame 建立基线 = 0xFFFF
    slave.SendRawPayloadTo(
        BytesView{EncodeUdpFrame(BytesView{a}, 0xFFFF).m_data_}, "127.0.0.1",
        kMasterPort);
    ASSERT_TRUE(listener.WaitForPackets(1, std::chrono::seconds(2)));
    EXPECT_EQ(transport.LastReceiveCtr().value_or(0), 0xFFFF);

    // 下一个 Frame 的 CTR 回绕到 0x0000，恰为期望值 -> 应被接受
    slave.SendRawPayloadTo(
        BytesView{EncodeUdpFrame(BytesView{b}, 0x0000).m_data_}, "127.0.0.1",
        kMasterPort);
    ASSERT_TRUE(listener.WaitForPackets(2, std::chrono::seconds(2)));
    EXPECT_EQ(transport.LastReceiveCtr().value_or(0xFFFF), 0x0000);
    EXPECT_EQ(listener.Packets().size(), 2U)
        << "0xFFFF -> 0x0000 回绕必须被接受";
    transport.Close();
    slave.Stop();
}

TEST(UdpTransportCtrSemantics, BackwardCtrFrameDropped) {
    constexpr std::uint16_t kMasterPort = 41621;
    test::UdpTestSlave slave;
    slave.Start();

    auto cfg = MakeConfig(slave.Port());
    cfg.m_local_port_ = kMasterPort;
    UdpTransport transport(cfg);
    RecordingListener listener;
    try {
        transport.Open(listener);
    } catch (const XcpException& e) {
        GTEST_SKIP() << "固定本地端口不可用: " << e.what();
    }

    const Bytes pkt = BytesOf({0xFD, 0x01});
    slave.SendRawPayloadTo(
        BytesView{EncodeUdpFrame(BytesView{pkt}, 10).m_data_}, "127.0.0.1",
        kMasterPort);
    ASSERT_TRUE(listener.WaitForPackets(1, std::chrono::seconds(2)));

    // 后向乱序（10 -> 5）：相对期望值 11 的前向距离极大 -> 判为迟到/乱序，丢弃
    slave.SendRawPayloadTo(BytesView{EncodeUdpFrame(BytesView{pkt}, 5).m_data_},
                           "127.0.0.1", kMasterPort);
    EXPECT_TRUE(listener.WaitForWarnings(1, std::chrono::seconds(2)));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(listener.Packets().size(), 1U) << "后向乱序 Frame 不得交付";
    EXPECT_EQ(transport.LastReceiveCtr().value_or(0), 10)
        << "被丢弃的 Frame 不得推进接收 CTR";
    transport.Close();
    slave.Stop();
}

TEST(UdpTransportCtrSemantics, TruncatedDatagramDiscarded) {
    constexpr std::uint16_t kMasterPort = 41622;
    test::UdpTestSlave slave;
    slave.Start();

    auto cfg = MakeConfig(slave.Port());
    cfg.m_local_port_ = kMasterPort;
    UdpTransport transport(cfg);
    RecordingListener listener;
    try {
        transport.Open(listener);
    } catch (const XcpException& e) {
        GTEST_SKIP() << "固定本地端口不可用: " << e.what();
    }

    // 0~3 字节的截断 Header：一律整体丢弃
    for (std::size_t n = 0; n < kUdpHeaderSize; ++n) {
        const Bytes truncated(kUdpHeaderSize, 0x00);
        slave.SendRawPayloadTo(BytesView{truncated}.first(n), "127.0.0.1",
                               kMasterPort);
    }
    EXPECT_TRUE(listener.WaitForWarnings(1, std::chrono::seconds(2)));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_TRUE(listener.Packets().empty());
    EXPECT_FALSE(transport.LastReceiveCtr().has_value())
        << "截断包不得建立接收基线";
    transport.Close();
    slave.Stop();
}

TEST(UdpTransportCtrSemantics, AmbiguousCtrOffsetIsDropped) {
    // 设计决策 D5 / §8.1 第 6 条：CTR 与期望值恰好相差 0x8000 时方向歧义，
    // 必须丢弃且不得推进接收基线。
    //
    // 精确构造方法（批次 2 记录 §9 限制 3）：
    //   Slave 正常响应 CONNECT 时使用其自身 CTR=0，Master 以此建立基线；
    //   随后 Master 发送 GET_STATUS，Slave 的下一个响应本应使用 CTR=1，
    //   施加 +0x8000 偏移后实发 (1+32768) mod 65536 = 0x8001，
    //   而 Master 的期望值为 1，前向距离恰为 0x8000。
    test::UdpTestSlave slave;
    slave.Start();
    UdpTransport transport(MakeConfig(slave.Port()));
    RecordingListener listener;
    transport.Open(listener);

    const Bytes connect = BytesOf({0xFF, 0x00});
    transport.Send(BytesView{connect});
    ASSERT_TRUE(listener.WaitForPackets(1, std::chrono::seconds(2)));
    EXPECT_EQ(transport.LastReceiveCtr().value_or(0xFFFF), 0);
    const std::size_t baseline = listener.Packets().size();

    test::FaultInjection fault;
    // 施加 +0x8000：本应发 CTR=1，实发 (1+32768) mod 65536 = 32769 = 0x8001，
    // 相对期望值 1 的前向距离恰为 0x8000（歧义点）。
    fault.m_ctr_offset_n_ = std::make_pair<std::size_t, int>(1U, 0x8000);
    slave.SetFaultInjection(fault);

    const Bytes get_status = BytesOf({0xFD, 0x00});
    transport.Send(BytesView{get_status});

    EXPECT_TRUE(listener.WaitForWarnings(1, std::chrono::seconds(2)));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(listener.Packets().size(), baseline)
        << "0x8000 歧义 Frame 不得交付上层";
    EXPECT_EQ(transport.LastReceiveCtr().value_or(0xFFFF), 0)
        << "被丢弃的 Frame 不得推进接收 CTR";

    bool ambiguity_reported = false;
    for (const auto& w : listener.Warnings()) {
        if (w.find("0x8000") != std::string::npos) {
            ambiguity_reported = true;
        }
    }
    EXPECT_TRUE(ambiguity_reported) << "未观察到 0x8000 歧义诊断";
    transport.Close();
    slave.Stop();
}

TEST(UdpTransportCtrSemantics, JustBelowAmbiguousBoundaryIsAccepted) {
    // 相邻区间对照：差值 0x7FFF 仍属前向跳号（§8.1 第 4
    // 条），必须接收并报告缺口。 与 AmbiguousCtrOffsetIsDropped 一起锁定 0x7FFF
    // / 0x8000 的边界归属。
    test::UdpTestSlave slave;
    slave.Start();
    UdpTransport transport(MakeConfig(slave.Port()));
    RecordingListener listener;
    transport.Open(listener);

    const Bytes connect = BytesOf({0xFF, 0x00});
    transport.Send(BytesView{connect});
    ASSERT_TRUE(listener.WaitForPackets(1, std::chrono::seconds(2)));
    const std::size_t baseline = listener.Packets().size();

    test::FaultInjection fault;
    fault.m_ctr_offset_n_ = std::make_pair<std::size_t, int>(1U, 0x7FFE);
    slave.SetFaultInjection(fault);

    const Bytes get_status = BytesOf({0xFD, 0x00});
    transport.Send(BytesView{get_status});

    EXPECT_TRUE(listener.WaitForPackets(baseline + 1, std::chrono::seconds(2)));
    bool gap_reported = false;
    for (const auto& w : listener.Warnings()) {
        if (w.find("跳号") != std::string::npos) {
            gap_reported = true;
        }
    }
    EXPECT_TRUE(gap_reported) << "0x7FFF 差值应按前向跳号处理";
    EXPECT_EQ(transport.LastReceiveCtr().value_or(0),
              static_cast<DatagramCtr>(32767))
        << "跳号 Frame 应推进接收 CTR";
    transport.Close();
    slave.Stop();
}

TEST(UdpTransportCtrSemantics, JustAboveAmbiguousBoundaryIsDropped) {
    // 相邻区间对照：差值 0x8001 落在后向侧（§8.1 第 5 条），按迟到乱序丢弃。
    test::UdpTestSlave slave;
    slave.Start();
    UdpTransport transport(MakeConfig(slave.Port()));
    RecordingListener listener;
    transport.Open(listener);

    const Bytes connect = BytesOf({0xFF, 0x00});
    transport.Send(BytesView{connect});
    ASSERT_TRUE(listener.WaitForPackets(1, std::chrono::seconds(2)));
    const std::size_t baseline = listener.Packets().size();

    test::FaultInjection fault;
    fault.m_ctr_offset_n_ = std::make_pair<std::size_t, int>(1U, -0x7FFE);
    slave.SetFaultInjection(fault);

    const Bytes get_status = BytesOf({0xFD, 0x00});
    transport.Send(BytesView{get_status});

    EXPECT_TRUE(listener.WaitForWarnings(1, std::chrono::seconds(2)));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(listener.Packets().size(), baseline)
        << "差值 0x8001 属后向乱序，不得交付";
    EXPECT_EQ(transport.LastReceiveCtr().value_or(0xFFFF), 0);
    transport.Close();
    slave.Stop();
}

TEST(UdpTransportConfigValidation, BindFailureReportedAsTransportError) {
    test::UdpTestSlave slave;
    slave.Start();

    // 绑定到已被 Slave 占用的端口：应报 TransportError
    auto cfg = MakeConfig(slave.Port());
    cfg.m_local_port_ = slave.Port();
    UdpTransport transport(cfg);
    RecordingListener listener;
    try {
        transport.Open(listener);
        FAIL() << "绑定到已占用端口应失败";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::TransportError);
        EXPECT_FALSE(e.TransportError().empty()) << "底层错误描述应被保留";
    }
    slave.Stop();
}

// --------------------------------------------------------------------------
// 关闭 / 取消语义
// --------------------------------------------------------------------------

TEST(UdpTransportClose, CloseWhileBlockedReturnsPromptly) {
    test::UdpTestSlave slave;
    slave.Start();
    auto cfg = MakeConfig(slave.Port());
    cfg.m_receive_poll_interval_ms_ =
        1000;  // 故意放大轮询周期，验证不依赖轮询也能及时退出
    UdpTransport transport(cfg);
    RecordingListener listener;
    transport.Open(listener);

    const auto start = std::chrono::steady_clock::now();
    transport.Close();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    EXPECT_LT(elapsed.count(), 1000)
        << "close() 未能通过关闭 Socket 唤醒阻塞接收";
    EXPECT_TRUE(listener.WaitForClose(std::chrono::seconds(2)));
    slave.Stop();
}

TEST(UdpTransportClose, ReopenResetsSendCtrAndReceiveBaseline) {
    test::UdpTestSlave slave;
    slave.Start();
    UdpTransport transport(MakeConfig(slave.Port()));
    RecordingListener listener;

    transport.Open(listener);
    const Bytes connect = BytesOf({0xFF, 0x00});
    transport.Send(BytesView{connect});
    ASSERT_TRUE(listener.WaitForPackets(1, std::chrono::seconds(2)));
    EXPECT_GT(transport.SendCtr(), 0);
    EXPECT_TRUE(transport.LastReceiveCtr().has_value());
    transport.Close();

    // 设计决策 D1：open() 复位发送 CTR 并清空接收基线
    RecordingListener listener2;
    transport.Open(listener2);
    EXPECT_EQ(transport.SendCtr(), 0);
    EXPECT_FALSE(transport.LastReceiveCtr().has_value());
    transport.Close();
    slave.Stop();
}

TEST(UdpTransportClose, NoCallbacksAfterCloseReturns) {
    test::UdpTestSlave slave;
    slave.Start();
    UdpTransport transport(MakeConfig(slave.Port()));
    RecordingListener listener;
    transport.Open(listener);
    transport.Close();
    ASSERT_TRUE(listener.WaitForClose(std::chrono::seconds(2)));

    const std::size_t after_close = listener.Packets().size();
    // 关闭后再发命令给 Slave，其响应不应回到 Master
    slave.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(listener.Packets().size(), after_close);
    slave.Stop();
}

// --------------------------------------------------------------------------
// CTR 不是事务 ID：响应 CTR 与请求 CTR 无关仍能正常处理
// --------------------------------------------------------------------------

TEST(UdpTransportCtrSemantics, ResponseMatchingDoesNotDependOnRequestCtr) {
    test::UdpTestSlave slave;
    slave.Start();
    UdpTransport transport(MakeConfig(slave.Port()));
    RecordingListener listener;
    transport.Open(listener);

    // Master 发送 CTR=0，Slave 响应 CTR 从自身序列独立取值；两者不相等
    const Bytes connect = BytesOf({0xFF, 0x00});
    transport.Send(BytesView{connect});
    ASSERT_TRUE(listener.WaitForPackets(1, std::chrono::seconds(2)));
    EXPECT_NE(transport.SendCtr(), transport.LastReceiveCtr().value_or(0xFFFF));
    EXPECT_TRUE(transport.LastReceiveCtr().has_value());
    transport.Close();
    slave.Stop();
}

}  // namespace
}  // namespace calmcar::xcp
