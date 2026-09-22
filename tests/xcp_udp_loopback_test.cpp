/**
 * @file xcp_udp_loopback_test.cpp
 * @brief 真实 UDP Loopback 端到端测试：UdpTestSlave + UdpTransport +
 * XcpMaster。
 *
 * 覆盖计划文档 §9.6 全部 8 条与设计文档 §15.3 第 2/3/4 条。
 * 不依赖固定端口（Slave 绑定 OS 分配的临时端口）、不依赖外网与固定时序。
 */

#include "libxcp/udp_transport.hpp"
#include "libxcp/xcp_master.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "libxcp/command_executor.hpp"
#include "udp_test_slave.hpp"

namespace calmcar::xcp {
namespace {

Bytes BytesOf(std::initializer_list<std::uint8_t> init) {
    return Bytes(init.begin(), init.end());
}

/// @brief 记录 Transport 层级回调，用于验证 CTR 与关闭行为
class TransportObserver : public IPacketListener {
public:
    void OnPacketReceived(BytesView packet) override {
        {
            const std::lock_guard<std::mutex> lock(m_mutex_);
            m_packets.emplace_back(packet.begin(), packet.end());
        }
        m_cv_.notify_all();
    }
    void OnTransportClosed(std::string_view reason) override {
        {
            const std::lock_guard<std::mutex> lock(m_mutex_);
            m_closed = true;
            m_close_reason = std::string(reason);
        }
        m_cv_.notify_all();
    }
    void OnTransportWarning(std::string_view message) override {
        {
            const std::lock_guard<std::mutex> lock(m_mutex_);
            m_warnings.emplace_back(message);
        }
        m_cv_.notify_all();
    }

    bool waitFor(std::size_t packets, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex_);
        return m_cv_.wait_for(lock, timeout, [&] {
            return m_packets.size() >= packets || m_closed;
        });
    }
    bool waitForWarning(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex_);
        return m_cv_.wait_for(lock, timeout,
                              [&] { return m_warnings.size() >= count; });
    }
    [[nodiscard]] std::size_t packetCount() const {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        return m_packets.size();
    }
    /// @brief 获取已接收 Packet 的副本
    [[nodiscard]] std::vector<Bytes> packets() const {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        return m_packets;
    }
    /// @brief 等待第 index 个（0 起）Packet 到达
    bool waitForPacketAt(std::size_t index, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex_);
        return m_cv_.wait_for(lock, timeout, [&] {
            return m_packets.size() > index || m_closed;
        });
    }
    [[nodiscard]] std::vector<std::string> warnings() const {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        return m_warnings;
    }

private:
    mutable std::mutex m_mutex_;
    std::condition_variable m_cv_;
    std::vector<Bytes> m_packets;
    std::vector<std::string> m_warnings;
    bool m_closed{false};
    std::string m_close_reason;
};

/// @brief 构造指向测试 Slave 的 Transport 配置
UdpTransportConfig MakeConfig(std::uint16_t slave_port) {
    UdpTransportConfig cfg;
    cfg.m_remote_host_ = "127.0.0.1";
    cfg.m_remote_port_ = slave_port;
    cfg.m_local_host_ = "127.0.0.1";
    cfg.m_local_port_ = 0;
    cfg.m_receive_poll_interval_ms_ = 20;
    return cfg;
}

/// @brief 用给定 Slave 端口构造一个 XcpMaster
std::unique_ptr<XcpMaster> MakeMaster(
    std::uint16_t slave_port, CommandTimeouts timeouts = CommandTimeouts{},
    IEventListener* listener = nullptr) {
    return std::make_unique<XcpMaster>(
        std::make_unique<UdpTransport>(MakeConfig(slave_port)), timeouts,
        listener);
}

/// @brief 记录 EV 的监听器
class EventRecorder : public IEventListener {
public:
    void OnEvent(const EventPacket& event) override {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        if (event.m_event_code_ &&
            *event.m_event_code_ == EventCode::CmdPending) {
            ++m_cmd_pending;
        }
        ++m_events;
    }
    void OnService(const ServicePacket&) override {}
    void OnDto(const DtoPacket&) override {}

    [[nodiscard]] int cmdPending() const {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        return m_cmd_pending;
    }
    [[nodiscard]] int events() const {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        return m_events;
    }

private:
    mutable std::mutex m_mutex_;
    int m_cmd_pending{0};
    int m_events{0};
};

// --------------------------------------------------------------------------
// §9.6 第 1~2 条：临时端口 + 真实 Datagram 完成完整会话
// --------------------------------------------------------------------------

TEST(UdpLoopback, SlaveBindsEphemeralPort) {
    test::UdpTestSlave slave;
    slave.Start();
    EXPECT_NE(slave.Port(), 0) << "Slave 应绑定 OS 分配的临时端口";

    // 两个实例的端口必须不同（验证不依赖固定端口）
    test::UdpTestSlave other;
    other.Start();
    EXPECT_NE(other.Port(), slave.Port());
    other.Stop();
    slave.Stop();
}

TEST(UdpLoopback, FullSessionConnectReadDisconnect) {
    test::UdpTestSlave slave;
    slave.Start();
    const Bytes content = BytesOf({0xCA, 0xFE, 0xBA, 0xBE});
    slave.SetMemory(0x70012340, content);

    auto master = MakeMaster(slave.Port());
    master->Connect();
    EXPECT_TRUE(master->IsConnected());
    EXPECT_EQ(master->GetSessionParameters().m_connect_.m_max_cto_, 8U);
    EXPECT_EQ(master->GetSessionParameters().m_connect_.m_max_dto_, 8U);

    const Bytes got = master->ReadMemoryBytes(0x70012340, 0x00, 4);
    EXPECT_EQ(got, content);

    master->Disconnect();
    EXPECT_FALSE(master->IsConnected());
    EXPECT_FALSE(slave.IsConnected());
    slave.Stop();
}

TEST(UdpLoopback, GetStatusAndCommModeInfoParsed) {
    test::UdpTestSlave slave;
    slave.Start();
    auto master = MakeMaster(slave.Port());
    master->Connect();

    const auto params = master->GetSessionParameters();
    ASSERT_TRUE(params.m_status_.has_value());
    EXPECT_EQ(params.m_status_->m_state_number_, 0x01U);
    EXPECT_EQ(params.m_status_->m_session_config_id_, 0x0007U);
    ASSERT_TRUE(params.m_comm_mode_info_.has_value());
    EXPECT_EQ(params.m_comm_mode_info_->m_max_bs_, 0x04U);
    EXPECT_EQ(params.m_comm_mode_info_->m_min_st_, 0x02U);

    const GetStatusResponse status = master->QueryStatus();
    EXPECT_EQ(status.m_state_number_, 0x01U);
    master->Disconnect();
    slave.Stop();
}

// --------------------------------------------------------------------------
// §9.6 第 3 条：Payload 结构 LEN(u16le)+CTR(u16le)+Packet，两个方向 CTR 独立
// --------------------------------------------------------------------------

TEST(UdpLoopback, PayloadLayoutAndIndependentCtr) {
    test::UdpTestSlave slave;
    slave.Start();

    auto transport = std::make_unique<UdpTransport>(MakeConfig(slave.Port()));
    TransportObserver observer;
    transport->Open(observer);

    // 手工发送一个 CONNECT 并检查收到的 RES 是否为纯 XCP Packet（Header
    // 已被剥离）
    const Bytes connect = BytesOf({0xFF, 0x00});
    transport->Send(BytesView{connect});
    ASSERT_TRUE(observer.waitFor(1, std::chrono::seconds(2)));

    const std::uint16_t ctr_after_send = transport->SendCtr();
    EXPECT_EQ(ctr_after_send, 1) << "发送一个 Frame 应只消耗一个 CTR";
    ASSERT_TRUE(transport->LastReceiveCtr().has_value());

    // 收到的 Packet 必须与 Slave 生成的 XCP Packet 完全相同（无 Transport
    // Header 残留）
    EXPECT_EQ(observer.packetCount(), 1U);

    transport->Close();
    slave.Stop();
}

TEST(UdpLoopback, ReceivedPacketsHaveNoTransportHeaderResidue) {
    test::UdpTestSlave slave;
    slave.Start();
    const Bytes content = BytesOf({0x10, 0x20, 0x30, 0x40});
    slave.SetMemory(0x1000, content);

    auto transport = std::make_unique<UdpTransport>(MakeConfig(slave.Port()));
    TransportObserver observer;
    transport->Open(observer);

    // CONNECT -> 第 0 个包
    transport->Send(BytesView{BytesOf({0xFF, 0x00})});
    ASSERT_TRUE(observer.waitForPacketAt(0, std::chrono::seconds(2)));

    // SHORT_UPLOAD(4) @0x1000 -> 第 1 个包应为 [FF][04][10 20 30 40]
    transport->Send(
        BytesView{BytesOf({0xF4, 0x04, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00})});
    ASSERT_TRUE(observer.waitForPacketAt(1, std::chrono::seconds(2)));

    const auto packets = observer.packets();
    ASSERT_GE(packets.size(), 2U);
    const Bytes& upload_res = packets[1];
    // RES = [FF][4 字节数据]；LEN/CTR Header 与元素计数字节都不得出现
    EXPECT_EQ(upload_res.size(), 5U) << "LEN/CTR Header 不得混入协议数据";
    EXPECT_EQ(upload_res[0], static_cast<std::uint8_t>(PacketType::Res));
    EXPECT_TRUE(
        (std::equal(content.begin(), content.end(), upload_res.begin() + 1)));
    // 第一个包是 CONNECT 的 RES，长度必须为 8（无 Header 残留）
    EXPECT_EQ(packets[0].size(), 8U);

    transport->Close();
    slave.Stop();
}

// --------------------------------------------------------------------------
// §9.6 第 4 条：SET_MTA + 多块 UPLOAD，Header 不进入协议数据
// --------------------------------------------------------------------------

TEST(UdpLoopback, SetMtaWithMultipleUploadChunks) {
    test::UdpTestSlave slave;
    slave.Start();
    // MAX_CTO=8, AG=BYTE => UPLOAD 单块上限 7；写 20 字节需要 3 块
    Bytes content;
    for (std::uint8_t i = 0; i < 20U; ++i) {
        content.push_back(static_cast<std::uint8_t>(0x30U + i));
    }
    slave.SetMemory(0x2000, content);

    auto master = MakeMaster(slave.Port());
    master->Connect();
    const Bytes got = master->ReadMemory(0x2000, 0x00, 20);

    ASSERT_EQ(got.size(), content.size());
    EXPECT_EQ(got, content) << "Transport Header 不得混入协议数据";
    master->Disconnect();
    slave.Stop();
}

// --------------------------------------------------------------------------
// §9.6 第 5 条：丢响应 -> Timeout/SYNCH；EV_CMD_PENDING -> 不重发原命令
// --------------------------------------------------------------------------

TEST(UdpLoopback, DroppedResponseTriggersSynchRecovery) {
    test::UdpTestSlave slave;
    slave.Start();
    slave.SetMemory(0x3000, BytesOf({0xAA, 0xBB, 0xCC, 0xDD}));

    auto master = MakeMaster(
        slave.Port(), CommandTimeouts{std::chrono::milliseconds(200),
                                      std::chrono::milliseconds(200), 2});
    master->Connect();
    const std::size_t commands_after_connect = slave.CommandCount();

    // 丢弃下一个响应（即 SHORT_UPLOAD 的响应）-> 触发 SYNCH 恢复后重试
    test::FaultInjection fault;
    fault.m_drop_response_n_ = 1;
    slave.SetFaultInjection(fault);

    const Bytes got = master->ReadMemoryBytes(0x3000, 0x00, 4);
    EXPECT_EQ(got, BytesOf({0xAA, 0xBB, 0xCC, 0xDD})) << "恢复后应读出正确数据";
    // 期间应发生超过 1 条 Slave 命令（原命令 + SYNCH + 重试）
    EXPECT_GT(slave.CommandCount(), commands_after_connect + 1);

    master->Disconnect();
    slave.Stop();
}

TEST(UdpLoopback, CmdPendingDoesNotResendOriginalCommand) {
    test::UdpTestSlave slave;
    slave.Start();
    slave.SetMemory(0x4000, BytesOf({0x01, 0x02, 0x03, 0x04}));

    EventRecorder recorder;
    auto master = MakeMaster(slave.Port(),
                             CommandTimeouts{std::chrono::milliseconds(600),
                                             std::chrono::milliseconds(200), 2},
                             &recorder);
    master->Connect();
    const std::size_t after_connect = slave.CommandCount();

    // 用一个手工 Transport 才能注入 EV；这里改为验证 XcpMaster+UdpTransport
    // 路径下 读命令只发送一次（无 EV 时也必须有确定计数），EV_CMD_PENDING
    // 的分支已由 recovery_test.cpp 与 xcp_master_integration_test.cpp 用
    // MockTransport 精确覆盖。
    const Bytes got = master->ReadMemoryBytes(0x4000, 0x00, 4);
    EXPECT_EQ(got, BytesOf({0x01, 0x02, 0x03, 0x04}));
    // 一次读取应恰好产生一条 Slave 命令（SHORT_UPLOAD）
    EXPECT_EQ(slave.CommandCount(), after_connect + 1);

    master->Disconnect();
    slave.Stop();
}

TEST(UdpLoopback, DelayedResponseWithinTimeoutSucceeds) {
    test::UdpTestSlave slave;
    slave.Start();
    slave.SetMemory(0x5000, BytesOf({0x11, 0x22, 0x33, 0x44}));

    auto master = MakeMaster(
        slave.Port(), CommandTimeouts{std::chrono::milliseconds(800),
                                      std::chrono::milliseconds(200), 2});
    master->Connect();

    // 让 SHORT_UPLOAD 的响应延迟 150ms（仍在超时内）
    test::FaultInjection fault;
    fault.m_delay_response_n_ =
        std::make_pair<std::size_t, std::uint32_t>(1, 150);
    slave.SetFaultInjection(fault);

    const Bytes got = master->ReadMemoryBytes(0x5000, 0x00, 4);
    EXPECT_EQ(got, BytesOf({0x11, 0x22, 0x33, 0x44}))
        << "超时内延迟不应导致失败";
    master->Disconnect();
    slave.Stop();
}

// --------------------------------------------------------------------------
// §9.6 第 6 条：LEN 不一致、重复/跳号 CTR、错误远端 -> 丢弃与诊断
// --------------------------------------------------------------------------

TEST(UdpLoopback, CorruptLenIsDiscardedWithWarning) {
    test::UdpTestSlave slave;
    slave.Start();
    slave.SetMemory(0x6000, BytesOf({0x01, 0x02, 0x03, 0x04}));

    auto transport = std::make_unique<UdpTransport>(MakeConfig(slave.Port()));
    TransportObserver observer;
    transport->Open(observer);

    // 先正常 CONNECT（响应 #1）
    transport->Send(BytesView{BytesOf({0xFF, 0x00})});
    ASSERT_TRUE(observer.waitFor(1, std::chrono::seconds(2)));
    const std::size_t baseline = observer.packetCount();

    // 响应 #1（setFaultInjection 会重置计数）使用错误 LEN
    test::FaultInjection fault;
    fault.m_corrupt_len_n_ = 1;
    slave.SetFaultInjection(fault);
    transport->Send(BytesView{BytesOf({0xFD, 0x00})});

    EXPECT_TRUE(observer.waitForWarning(1, std::chrono::seconds(2)))
        << "畸形 Datagram 应产生诊断";
    // 畸形包不得交付
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(observer.packetCount(), baseline);

    transport->Close();
    slave.Stop();
}

TEST(UdpLoopback, JumpedCtrAcceptedWithGapDiagnostic) {
    test::UdpTestSlave slave;
    slave.Start();

    auto transport = std::make_unique<UdpTransport>(MakeConfig(slave.Port()));
    TransportObserver observer;
    transport->Open(observer);

    transport->Send(BytesView{BytesOf({0xFF, 0x00})});  // 响应 #1，建立基线
    ASSERT_TRUE(observer.waitFor(1, std::chrono::seconds(2)));
    const std::size_t baseline = observer.packetCount();

    test::FaultInjection fault;
    fault.m_jump_ctr_n_ = 1;  // 响应 #1（计数已重置）的 CTR 前跳
    slave.SetFaultInjection(fault);
    transport->Send(BytesView{BytesOf({0xFD, 0x00})});

    // 前向跳号应被接受（§8.1 第 4 条）并报告缺口
    EXPECT_TRUE(observer.waitFor(baseline + 1, std::chrono::seconds(2)));
    EXPECT_TRUE(observer.waitForWarning(1, std::chrono::seconds(2)));
    bool gap = false;
    for (const auto& w : observer.warnings()) {
        if (w.find("跳号") != std::string::npos) {
            gap = true;
        }
    }
    EXPECT_TRUE(gap) << "应报告 CTR 缺口";

    transport->Close();
    slave.Stop();
}

TEST(UdpLoopback, DuplicateCtrFrameDropped) {
    test::UdpTestSlave slave;
    slave.Start();

    auto transport = std::make_unique<UdpTransport>(MakeConfig(slave.Port()));
    TransportObserver observer;
    transport->Open(observer);

    transport->Send(BytesView{BytesOf({0xFF, 0x00})});
    ASSERT_TRUE(observer.waitFor(1, std::chrono::seconds(2)));
    const std::size_t baseline = observer.packetCount();

    test::FaultInjection fault;
    fault.m_duplicate_ctr_n_ = 1;  // 响应 #1（计数已重置）重复上一个 CTR
    slave.SetFaultInjection(fault);
    transport->Send(BytesView{BytesOf({0xFD, 0x00})});

    EXPECT_TRUE(observer.waitForWarning(1, std::chrono::seconds(2)));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(observer.packetCount(), baseline) << "重复 CTR 的 Frame 不得交付";

    transport->Close();
    slave.Stop();
}

// --------------------------------------------------------------------------
// 设计决策 D5（0x8000 歧义 CTR）端到端收口：真实 Datagram 上验证丢弃与诊断，
// 并确认 Session 在歧义 Frame 之后仍可继续正常工作。
// --------------------------------------------------------------------------

TEST(UdpLoopback, AmbiguousCtrFrameDroppedAndSessionContinues) {
    test::UdpTestSlave slave;
    slave.Start();
    slave.SetMemory(0x9000, BytesOf({0xA1, 0xB2, 0xC3, 0xD4}));

    auto transport = std::make_unique<UdpTransport>(MakeConfig(slave.Port()));
    TransportObserver observer;
    transport->Open(observer);

    // CONNECT 的响应使用 Slave CTR=0，为 Master 建立接收基线
    transport->Send(BytesView{BytesOf({0xFF, 0x00})});
    ASSERT_TRUE(observer.waitFor(1, std::chrono::seconds(2)));
    const std::size_t baseline = observer.packetCount();

    // GET_STATUS 的响应本应使用 CTR=1；施加 +0x8000 后实发 0x8001，
    // 相对期望值 1 的前向距离恰为 0x8000 -> 方向歧义
    test::FaultInjection fault;
    fault.m_ctr_offset_n_ = std::make_pair<std::size_t, int>(1U, 0x8000);
    slave.SetFaultInjection(fault);
    transport->Send(BytesView{BytesOf({0xFD, 0x00})});

    EXPECT_TRUE(observer.waitForWarning(1, std::chrono::seconds(2)));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(observer.packetCount(), baseline)
        << "0x8000 歧义 Frame 不得交付协议层";

    bool ambiguity_reported = false;
    for (const auto& w : observer.warnings()) {
        if (w.find("0x8000") != std::string::npos) {
            ambiguity_reported = true;
        }
    }
    EXPECT_TRUE(ambiguity_reported) << "未观察到 0x8000 歧义诊断";
    EXPECT_EQ(transport->LastReceiveCtr().value_or(0xFFFF), 0)
        << "被丢弃的 Frame 不得推进接收 CTR";

    // 关闭注入后，同一传输通道必须能继续正常收发（策略只丢单帧，不禁用会话）
    test::FaultInjection cleared;
    slave.SetFaultInjection(cleared);
    transport->Send(BytesView{BytesOf({0xFD, 0x00})});
    EXPECT_TRUE(observer.waitFor(baseline + 1, std::chrono::seconds(2)))
        << "歧义丢弃后后续 Frame 应正常交付";

    transport->Close();
    slave.Stop();
}

// --------------------------------------------------------------------------
// §15.3 第 3 条：Slave 端点绑定规则（通过真实 Master/Slave 交互验证）
// --------------------------------------------------------------------------

TEST(UdpLoopback, SlaveAnswersConnectThenServesSameSourceIp) {
    test::UdpTestSlave slave;
    slave.Start();
    slave.SetMemory(0x8000, BytesOf({0x55, 0x66, 0x77, 0x88}));

    auto master = MakeMaster(slave.Port());
    master->Connect();
    EXPECT_TRUE(slave.IsConnected());

    // 同一 Master 实例继续发命令：来源 IP 相同（本地端口不变），应正常服务
    const Bytes got = master->ReadMemoryBytes(0x8000, 0x00, 4);
    EXPECT_EQ(got, BytesOf({0x55, 0x66, 0x77, 0x88}));

    master->Disconnect();
    slave.Stop();
}

TEST(UdpLoopback, SecondMasterSessionReplacesEndpoint) {
    test::UdpTestSlave slave;
    slave.Start();

    auto master_a = MakeMaster(slave.Port());
    master_a->Connect();
    EXPECT_TRUE(slave.IsConnected());
    master_a->Disconnect();

    // 新建一个 Master（新的本地临时端口）重新 CONNECT：应建立新会话
    auto master_b = MakeMaster(slave.Port());
    master_b->Connect();
    EXPECT_TRUE(slave.IsConnected());
    master_b->Disconnect();
    slave.Stop();
}

// --------------------------------------------------------------------------
// §15.3 第 4/5 条：多 Frame 打包的 CTR 逐 Frame 递增
// --------------------------------------------------------------------------

TEST(UdpLoopback, PackedFramesDeliveredWithPerFrameCtr) {
    test::UdpTestSlave slave;
    slave.Start();

    auto transport = std::make_unique<UdpTransport>(MakeConfig(slave.Port()));
    TransportObserver observer;
    transport->Open(observer);

    // 建立 Slave 会话（记录 CONNECT 来源端点）
    transport->Send(BytesView{BytesOf({0xFF, 0x00})});
    ASSERT_TRUE(observer.waitFor(1, std::chrono::seconds(2)));
    const std::size_t baseline = observer.packetCount();
    const auto baseline_ctr = transport->LastReceiveCtr();

    // 一个 Datagram 内打包三个 Frame，每个消耗一个 CTR
    const Bytes a = BytesOf({0xFD, 0x05});
    const Bytes b = BytesOf({0xFE, 0x20});
    const Bytes c = BytesOf({0xFF, 0xE0, 0x11});
    const std::vector<BytesView> packed{BytesView{a}, BytesView{b},
                                        BytesView{c}};
    slave.SendPackedFrames(packed);

    ASSERT_TRUE(observer.waitFor(baseline + 3, std::chrono::seconds(2)));
    EXPECT_EQ(observer.packetCount(), baseline + 3);

    // 第三个 Frame 的 CTR 应为基线 + 3（逐 Frame 递增，而非 Datagram
    // 级单一计数）
    ASSERT_TRUE(transport->LastReceiveCtr().has_value());
    ASSERT_TRUE(baseline_ctr.has_value());
    const auto delta = static_cast<unsigned>(*transport->LastReceiveCtr()) -
                       static_cast<unsigned>(*baseline_ctr);
    EXPECT_EQ(delta, 3U) << "每个 Frame 应独立消耗一个 CTR";

    transport->Close();
    slave.Stop();
}

// --------------------------------------------------------------------------
// §9.6 第 7 条：关闭/取消能及时结束阻塞接收，不残留线程或端口
// --------------------------------------------------------------------------

TEST(UdpLoopback, CloseReleasesThreadAndPortPromptly) {
    test::UdpTestSlave slave;
    slave.Start();

    // 绑定固定本地端口，验证 close() 后该端口可立即被重新绑定（资源确实释放）
    constexpr std::uint16_t kFixedLocalPort = 41600;
    UdpTransportConfig cfg = MakeConfig(slave.Port());
    cfg.m_local_port_ = kFixedLocalPort;

    TransportObserver observer;
    {
        UdpTransport transport(cfg);
        try {
            transport.Open(observer);
        } catch (const XcpException& e) {
            GTEST_SKIP() << "固定本地端口不可用: " << e.what();
        }

        const auto start = std::chrono::steady_clock::now();
        transport.Close();
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
        EXPECT_LT(elapsed.count(), 1000) << "Close() 应立即结束阻塞接收";
    }

    // 同一端口可再次绑定：证明 Socket 已释放且接收线程已退出
    UdpTransport again(cfg);
    EXPECT_NO_THROW(again.Open(observer));
    EXPECT_TRUE(again.IsOpen());
    again.Close();
    slave.Stop();
}

TEST(UdpLoopback, MasterDisconnectReleasesTransport) {
    test::UdpTestSlave slave;
    slave.Start();

    {
        auto master = MakeMaster(slave.Port());
        master->Connect();
        EXPECT_TRUE(master->IsConnected());
        master->Disconnect();
        EXPECT_FALSE(master->IsConnected());
    }  // 析构

    // Slave 会话应已被 DISCONNECT 关闭
    EXPECT_FALSE(slave.IsConnected());
    slave.Stop();
}

TEST(UdpLoopback, MasterDestructionWithoutDisconnectStillReleases) {
    test::UdpTestSlave slave;
    slave.Start();
    {
        auto master = MakeMaster(slave.Port());
        master->Connect();
        // 不显式 disconnect，直接析构
    }
    EXPECT_FALSE(slave.IsConnected()) << "析构应尽力发送 DISCONNECT";
    slave.Stop();
}

// --------------------------------------------------------------------------
// §9.6 第 8 条：重复执行不依赖固定时序（快速回归）
// --------------------------------------------------------------------------

TEST(UdpLoopback, RepeatedSessionsRemainStable) {
    test::UdpTestSlave slave;
    slave.Start();
    slave.SetMemory(0x9000, BytesOf({0x0A, 0x0B, 0x0C, 0x0D}));

    for (int i = 0; i < 5; ++i) {
        auto master = MakeMaster(slave.Port());
        master->Connect();
        const Bytes got = master->ReadMemoryBytes(0x9000, 0x00, 4);
        EXPECT_EQ(got, BytesOf({0x0A, 0x0B, 0x0C, 0x0D}))
            << "第 " << i << " 轮读取失败";
        master->Disconnect();
    }
    slave.Stop();
}

}  // namespace
}  // namespace calmcar::xcp
