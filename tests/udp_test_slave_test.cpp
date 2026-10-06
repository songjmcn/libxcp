/**
 * @file udp_test_slave_test.cpp
 * @brief UdpTestSlave 自身行为测试：XCP 1.1 UDP/IP 端点绑定规则（设计 §15.3 第
 * 3 条）。
 */

#include "udp_test_slave.hpp"

#include <chrono>
#include <cstring>
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

#include "libxcp/protocol_types.hpp"
#include "libxcp/udp_header_codec.hpp"

namespace calmcar::xcp::test {
namespace {

/// @brief 无效 Socket 句柄的跨平台表示
#if defined(_WIN32)
constexpr auto kInvalidHandle = INVALID_SOCKET;
#else
constexpr auto kInvalidHandle = -1;
#endif

/// @brief 简易 Loopback UDP 端点：绑定指定源端口，可收发 Frame
class RawEndpoint {
public:
    explicit RawEndpoint(std::uint16_t src_port) {
#if defined(_WIN32)
        WSADATA data;
        m_wsa_ok_ = (::WSAStartup(MAKEWORD(2, 2), &data) == 0);
#endif
        m_handle_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        m_valid_ = (m_handle_ != kInvalidHandle);
        if (!m_valid_) {
            return;
        }
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(src_port);
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        // 构造函数不能用 ASSERT_*（宏要求 void 返回），失败用标志位表达
        m_valid_ = (::bind(m_handle_, reinterpret_cast<sockaddr*>(&local),
                           sizeof(local)) == 0);

        // 接收超时，避免测试挂死
#if defined(_WIN32)
        DWORD tv = 100;
        ::setsockopt(m_handle_, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
        timeval tv{};
        tv.tv_usec = 100 * 1000;
        ::setsockopt(m_handle_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }

    /// @brief 端点是否就绪
    [[nodiscard]] bool valid() const noexcept { return m_valid_; }

    ~RawEndpoint() {
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

    /// @brief 发送一个 XCP Packet（自动加 Header），目标为 dst_port
    void sendPacket(BytesView xcp_packet, std::uint16_t dst_port,
                    DatagramCtr ctr) {
        const auto frame = EncodeUdpFrame(xcp_packet, ctr);
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(dst_port);
        dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::sendto(m_handle_, reinterpret_cast<const char*>(frame.data.data()),
                 static_cast<int>(frame.data.size()), 0,
                 reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    }

    /// @brief 读取一个完整 Frame 的 XCP Packet（同时记录其 Header 中的 CTR）
    bool recvPacketWithCtr(Bytes& out, DatagramCtr& out_ctr) {
        std::uint8_t buffer[kUdpMaxDatagramSize];
        sockaddr_in src{};
        socklen_t src_len = sizeof(src);
        const int received = ::recvfrom(
            m_handle_, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
            reinterpret_cast<sockaddr*>(&src), &src_len);
        if (received <= 0) {
            return false;
        }
        const auto frames = DecodeUdpDatagram(
            BytesView{buffer, static_cast<std::size_t>(received)});
        if (!frames || frames->empty()) {
            return false;
        }
        out_ctr = (*frames)[0].header.ctr;
        out.assign((*frames)[0].xcp_packet.begin(),
                   (*frames)[0].xcp_packet.end());
        return true;
    }

    bool recvPacket(Bytes& out, std::string& out_from_ip,
                    std::uint16_t& out_from_port) {
        std::uint8_t buffer[kUdpMaxDatagramSize];
        sockaddr_in src{};
        socklen_t src_len = sizeof(src);
        const int received = ::recvfrom(
            m_handle_, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
            reinterpret_cast<sockaddr*>(&src), &src_len);
        if (received <= 0) {
            return false;
        }
        const auto frames = DecodeUdpDatagram(
            BytesView{buffer, static_cast<std::size_t>(received)});
        if (!frames || frames->empty()) {
            return false;
        }
        out.assign((*frames)[0].xcp_packet.begin(),
                   (*frames)[0].xcp_packet.end());
        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &src.sin_addr, ip, INET_ADDRSTRLEN);
        out_from_ip = ip;
        out_from_port = ntohs(src.sin_port);
        return true;
    }

private:
#if defined(_WIN32)
    bool m_wsa_ok_{false};
#endif
    bool m_valid_{false};
    decltype(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) m_handle_{};
};

Bytes BytesOf(std::initializer_list<std::uint8_t> init) {
    return Bytes(init.begin(), init.end());
}

constexpr std::chrono::milliseconds kWait{2000};

/// @brief 轮询等待直到条件成立或超时
template <typename Pred>
bool WaitUntil(Pred pred, std::chrono::milliseconds timeout = kWait) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

// --------------------------------------------------------------------------
// 未连接时只对 CONNECT 的来源端点应答
// --------------------------------------------------------------------------

TEST(UdpTestSlaveSession, IgnoresCommandsBeforeConnect) {
    UdpTestSlave slave;
    slave.Start();

    RawEndpoint ep(43100);
    // 未连接时发送 GET_STATUS：应被忽略，无任何响应
    ep.sendPacket(BytesView{BytesOf({0xFD, 0x00})}, slave.Port(), 0);
    Bytes resp;
    std::string ip;
    std::uint16_t port = 0;
    EXPECT_FALSE(ep.recvPacket(resp, ip, port))
        << "未连接时 Slave 不应对非 CONNECT 命令应答";
    EXPECT_EQ(slave.CommandCount(), 1U);
    EXPECT_FALSE(slave.IsConnected());
    slave.Stop();
}

TEST(UdpTestSlaveSession, ConnectIsAnsweredAtSourceEndpoint) {
    UdpTestSlave slave;
    slave.Start();

    RawEndpoint ep(43110);
    ep.sendPacket(BytesView{BytesOf({0xFF, 0x00})}, slave.Port(), 0);

    Bytes resp;
    std::string ip;
    std::uint16_t port = 0;
    ASSERT_TRUE(ep.recvPacket(resp, ip, port));
    ASSERT_GE(resp.size(), 8U);
    EXPECT_EQ(resp[0], static_cast<std::uint8_t>(PacketType::Res));
    // 默认关闭 DAQ 模拟：RESOURCE=0x11（CAL/PAG+PGM），不声明未实现的 DAQ
    EXPECT_EQ(resp[1], 0x11U);
    EXPECT_EQ(resp[2], 0xC0U);
    EXPECT_EQ(resp[3], 8U);
    EXPECT_EQ(ip, "127.0.0.1");
    EXPECT_EQ(port, slave.Port());
    EXPECT_TRUE(slave.IsConnected());
    slave.Stop();
}

TEST(UdpTestSlaveSession, ConnectResourceAdvertisesDaqWhenSimulationEnabled) {
    UdpTestSlave slave;
    slave.SetDaqSimulationEnabled(true);
    slave.Start();

    RawEndpoint ep(43111);
    ep.sendPacket(BytesView{BytesOf({0xFF, 0x00})}, slave.Port(), 0);

    Bytes resp;
    std::string ip;
    std::uint16_t port = 0;
    ASSERT_TRUE(ep.recvPacket(resp, ip, port));
    ASSERT_GE(resp.size(), 2U);
    EXPECT_EQ(resp[1], 0x15U);
    slave.Stop();
}

// --------------------------------------------------------------------------
// 连接后：同一 IP 的不同源端口仍处理，但响应固定回到 CONNECT 来源端点
// --------------------------------------------------------------------------

TEST(UdpTestSlaveSession, SameIpDifferentPortStillServed) {
    UdpTestSlave slave;
    slave.Start();

    RawEndpoint connect_ep(43120);
    connect_ep.sendPacket(BytesView{BytesOf({0xFF, 0x00})}, slave.Port(), 0);
    Bytes first;
    std::string ip;
    std::uint16_t port = 0;
    ASSERT_TRUE(connect_ep.recvPacket(first, ip, port));
    ASSERT_TRUE(slave.IsConnected());

    // 换一个源端口发 GET_STATUS：命令应被处理，且响应必须回到原 CONNECT 端点
    RawEndpoint other_ep(43121);
    other_ep.sendPacket(BytesView{BytesOf({0xFD, 0x00})}, slave.Port(), 1);

    Bytes via_connect;
    const bool answered_at_connect_endpoint =
        WaitUntil([&] { return connect_ep.recvPacket(via_connect, ip, port); });
    ASSERT_TRUE(answered_at_connect_endpoint)
        << "响应应发往原 CONNECT 来源 IP:port";
    EXPECT_EQ(via_connect[0], static_cast<std::uint8_t>(PacketType::Res));
    EXPECT_EQ(port, slave.Port());

    // 新端口不应收到任何响应
    Bytes stray;
    EXPECT_FALSE(other_ep.recvPacket(stray, ip, port))
        << "响应不得发往变更后的源端口";
    slave.Stop();
}

TEST(UdpTestSlaveSession, OtherIpCommandsIgnoredAfterConnect) {
    UdpTestSlave slave;
    slave.Start();

    RawEndpoint connect_ep(43130);
    connect_ep.sendPacket(BytesView{BytesOf({0xFF, 0x00})}, slave.Port(), 0);
    Bytes first;
    std::string ip;
    std::uint16_t port = 0;
    ASSERT_TRUE(connect_ep.recvPacket(first, ip, port));

    // 从另一本机回环地址视角无法直接伪造源 IP；此处用第二个 Slave
    // 实例做端口隔离验证： 未与其建立会话前，它的命令同样被忽略，等价覆盖“非
    // CONNECT 来源 IP 忽略”分支。
    RawEndpoint stranger(43131);
    stranger.sendPacket(BytesView{BytesOf({0xFD, 0x00})}, slave.Port(), 9);
    Bytes stray;
    EXPECT_FALSE(stranger.recvPacket(stray, ip, port))
        << "非会话来源不得收到响应";
    slave.Stop();
}

// --------------------------------------------------------------------------
// 最小命令语义
// --------------------------------------------------------------------------

class UdpTestSlaveCommands : public ::testing::Test {
protected:
    void SetUp() override {
        slave_.Start();
        ep_ = std::make_unique<RawEndpoint>(43140);
        ctr_ = 0;
        // 建立会话
        ep_->sendPacket(BytesView{BytesOf({0xFF, 0x00})}, slave_.Port(),
                        ctr_++);
        std::string ip;
        std::uint16_t port = 0;
        ASSERT_TRUE(ep_->recvPacket(res_, ip, port));
        ASSERT_EQ(res_[0], static_cast<std::uint8_t>(PacketType::Res));
    }

    void TearDown() override {
        ep_.reset();
        slave_.Stop();
    }

    /// @brief 发送命令并收取一个响应
    bool Request(const Bytes& cmd, Bytes& out) {
        ep_->sendPacket(BytesView{cmd}, slave_.Port(), ctr_++);
        std::string ip;
        std::uint16_t port = 0;
        return WaitUntil([&] { return ep_->recvPacket(out, ip, port); });
    }

    UdpTestSlave slave_;
    std::unique_ptr<RawEndpoint> ep_;
    DatagramCtr ctr_{0};
    Bytes res_;
};

TEST_F(UdpTestSlaveCommands, GetStatusReturnsSixBytePacket) {
    ASSERT_TRUE(Request(BytesOf({0xFD, 0x00}), res_));
    ASSERT_EQ(res_.size(), 6U);
    EXPECT_EQ(res_[0], static_cast<std::uint8_t>(PacketType::Res));
}

TEST_F(UdpTestSlaveCommands, GetCommModeInfoReturnsEightBytePacket) {
    ASSERT_TRUE(Request(BytesOf({0xFB, 0x00}), res_));
    ASSERT_EQ(res_.size(), 8U);
    EXPECT_EQ(res_[0], static_cast<std::uint8_t>(PacketType::Res));
    // 布局：[FF][reserved][COMM_MODE_OPTIONAL][reserved][MAX_BS][MIN_ST][QUEUE_SIZE][DRIVER]
    EXPECT_EQ(res_[2], 0x0EU);  // COMM_MODE_OPTIONAL
    EXPECT_EQ(res_[4], 0x04U);  // MAX_BS
    EXPECT_EQ(res_[5], 0x02U);  // MIN_ST
    EXPECT_EQ(res_[6], 0x08U);  // QUEUE_SIZE
    EXPECT_EQ(res_[7], 0x13U);  // Driver Version 1.3
}

TEST_F(UdpTestSlaveCommands, SynchAlwaysAnswersErrCmdSynch) {
    ASSERT_TRUE(Request(BytesOf({0xFC, 0x00}), res_));
    ASSERT_EQ(res_.size(), 2U);
    EXPECT_EQ(res_[0], static_cast<std::uint8_t>(PacketType::Err));
    EXPECT_EQ(res_[1], static_cast<std::uint8_t>(ErrorCode::CmdSynch));
}

TEST_F(UdpTestSlaveCommands, UnknownCommandReturnsErrCmdUnknown) {
    // 批次14 起 DOWNLOAD(0xF0) 已由 Slave 模拟实现（虽默认关闭，另有专门
    // 用例锁定其默认拒绝），故本用例改用**始终未实现**的命令码 0xD2，
    // 保持"未实现 → ERR_CMD_UNKNOWN"这条语义继续被覆盖。
    ASSERT_TRUE(Request(BytesOf({0xD2, 0x00}), res_));
    ASSERT_EQ(res_.size(), 2U);
    EXPECT_EQ(res_[0], static_cast<std::uint8_t>(PacketType::Err));
    EXPECT_EQ(res_[1], static_cast<std::uint8_t>(ErrorCode::CmdUnknown));
}

TEST_F(UdpTestSlaveCommands, ShortUploadReadsConfiguredMemory) {
    const Bytes content = BytesOf({0xDE, 0xAD, 0xBE, 0xEF});
    slave_.SetMemory(0x1000, BytesView{content});

    // SHORT_UPLOAD: [F4][n=4][00][ext=00][addr 小端 00 10 00 00]
    ASSERT_TRUE(Request(
        BytesOf({0xF4, 0x04, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00}), res_));
    // RES = [FF][data...]，长度 == n*AG（docs/XCP_1.3.0_document.md §12.5）
    ASSERT_EQ(res_.size(), 5U);
    EXPECT_EQ(res_[0], static_cast<std::uint8_t>(PacketType::Res));
    EXPECT_TRUE((std::equal(content.begin(), content.end(), res_.begin() + 1)));
}

TEST_F(UdpTestSlaveCommands, SetMtaThenUploadReadsSameMemory) {
    const Bytes content = BytesOf({0x11, 0x22, 0x33, 0x44, 0x55});
    slave_.SetMemory(0x2000, BytesView{content});

    ASSERT_TRUE(Request(
        BytesOf({0xF6, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00}),
        res_));  // SET_MTA addr=0x2000（[F6][MODE][rsv][EXT][ADDR]）
    EXPECT_EQ(res_[0], static_cast<std::uint8_t>(PacketType::Res));

    // UPLOAD(3) => [FF][3 字节数据]（无计数字节）
    ASSERT_TRUE(Request(BytesOf({0xF5, 0x03}), res_));
    ASSERT_EQ(res_.size(), 4U);
    EXPECT_TRUE(
        (std::equal(content.begin(), content.begin() + 3, res_.begin() + 1)));

    // MTA 已前进：再 UPLOAD(2) => [FF][2 字节数据]
    ASSERT_TRUE(Request(BytesOf({0xF5, 0x02}), res_));
    ASSERT_EQ(res_.size(), 3U);
    EXPECT_TRUE(
        (std::equal(content.begin() + 3, content.end(), res_.begin() + 1)));
}

TEST_F(UdpTestSlaveCommands, UploadBeyondMemoryReturnsAccessDenied) {
    const Bytes content = BytesOf({0x01, 0x02});
    slave_.SetMemory(0x3000, BytesView{content});
    ASSERT_TRUE(Request(
        BytesOf({0xF6, 0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00}),
        res_));
    ASSERT_TRUE(Request(BytesOf({0xF5, 0x06}), res_));  // 超出内存块范围
    ASSERT_EQ(res_.size(), 2U);
    EXPECT_EQ(res_[0], static_cast<std::uint8_t>(PacketType::Err));
    EXPECT_EQ(res_[1], static_cast<std::uint8_t>(ErrorCode::AccessDenied));
}

TEST_F(UdpTestSlaveCommands, OutOfRangeElementCountRejected) {
    // MAX_CTO=8, AG=1 => UPLOAD 上限 7
    ASSERT_TRUE(Request(BytesOf({0xF5, 0x08}), res_));
    ASSERT_EQ(res_[0], static_cast<std::uint8_t>(PacketType::Err));
    EXPECT_EQ(res_[1], static_cast<std::uint8_t>(ErrorCode::OutOfRange));

    // SHORT_UPLOAD 上限 8，9 越界
    ASSERT_TRUE(Request(
        BytesOf({0xF4, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}), res_));
    ASSERT_EQ(res_[0], static_cast<std::uint8_t>(PacketType::Err));
    EXPECT_EQ(res_[1], static_cast<std::uint8_t>(ErrorCode::OutOfRange));
}

TEST_F(UdpTestSlaveCommands, TruncatedSetMtaReturnsCmdSyntax) {
    ASSERT_TRUE(Request(BytesOf({0xF6, 0x00, 0x00}), res_));  // 缺 Address
    ASSERT_EQ(res_[0], static_cast<std::uint8_t>(PacketType::Err));
    EXPECT_EQ(res_[1], static_cast<std::uint8_t>(ErrorCode::CmdSyntax));
}

TEST_F(UdpTestSlaveCommands, DisconnectClearsSession) {
    ASSERT_TRUE(Request(BytesOf({0xFE, 0x00}), res_));
    EXPECT_EQ(res_[0], static_cast<std::uint8_t>(PacketType::Res));
    EXPECT_FALSE(slave_.IsConnected());
}

// --------------------------------------------------------------------------
// 故障注入计数与多 Frame 打包
// --------------------------------------------------------------------------

TEST(UdpTestSlaveFault, ResponseCounterStartsFreshAfterEachInjection) {
    UdpTestSlave slave;
    slave.Start();
    RawEndpoint ep(43150);
    DatagramCtr ctr = 0;
    std::string ip;
    std::uint16_t port = 0;
    Bytes resp;

    ep.sendPacket(BytesView{BytesOf({0xFF, 0x00})}, slave.Port(), ctr++);
    ASSERT_TRUE(ep.recvPacket(resp, ip, port));  // 响应 #1

    FaultInjection fault;
    fault.drop_response_n =
        1;  // setFaultInjection 会重置计数，故此次丢弃的是新的 #1
    slave.SetFaultInjection(fault);
    ep.sendPacket(BytesView{BytesOf({0xFD, 0x00})}, slave.Port(), ctr++);
    EXPECT_FALSE(ep.recvPacket(resp, ip, port)) << "被标记丢弃的响应不应发出";

    slave.SetFaultInjection(FaultInjection{});  // 清空并重置计数
    ep.sendPacket(BytesView{BytesOf({0xFD, 0x00})}, slave.Port(), ctr++);
    EXPECT_TRUE(WaitUntil([&] { return ep.recvPacket(resp, ip, port); }))
        << "取消注入后应恢复应答";
    slave.Stop();
}

TEST(UdpTestSlaveFault, CtrOffsetProducesExactHeaderCounter) {
    // 锁定 ctr_offset_n 的语义：只改写实际发出的 Header CTR，
    // 不改变 Slave 自身计数器的推进规律（Master 侧 D5 用例依赖该前提）。
    UdpTestSlave slave;
    slave.Start();
    RawEndpoint ep(43151);
    DatagramCtr ctr = 0;
    Bytes resp;

    ep.sendPacket(BytesView{BytesOf({0xFF, 0x00})}, slave.Port(), ctr++);
    ASSERT_TRUE(ep.recvPacketWithCtr(resp, ctr));
    EXPECT_EQ(ctr, 0) << "CONNECT 响应应使用 Slave 的首个 CTR";

    FaultInjection fault;
    // 对响应 #1 施加 +0x8000：本应发 1，实发 (1+32768) mod 65536 = 0x8001
    fault.ctr_offset_n = std::make_pair<std::size_t, int>(1U, 0x8000);
    slave.SetFaultInjection(fault);
    ASSERT_EQ(slave.NextSendCtr(), 1) << "注入不应改变待发送的 CTR";

    ep.sendPacket(BytesView{BytesOf({0xFD, 0x00})}, slave.Port(), ctr++);
    DatagramCtr received_ctr = 0;
    ASSERT_TRUE(ep.recvPacketWithCtr(resp, received_ctr));
    EXPECT_EQ(received_ctr, 0x8001) << "偏移后的 CTR 必须精确等于 0x8001";

    // 计数器仍按正常序列推进：下一个响应使用 2
    EXPECT_EQ(slave.NextSendCtr(), 2) << "精确偏移不得影响后续 CTR 推进";
    slave.Stop();
}

TEST(UdpTestSlavePacking, SendPackedFramesRequiresConnection) {
    UdpTestSlave slave;
    slave.Start();
    const Bytes a = BytesOf({0xFD, 0x05});
    const std::vector<BytesView> packets{BytesView{a}};
    EXPECT_THROW(slave.SendPackedFrames(packets), XcpException);
    try {
        slave.SendPackedFrames(packets);
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidState);
    }
    slave.Stop();
}

// ---------------------------------------------------------------------------
// 批次14（T14-09）：DAQ 命令组与 DOWNLOAD 写回模拟
//
// 默认关闭（既有 253 项用例语义不变）→ 用例必须显式 SetDaqSimulationEnabled。
// ---------------------------------------------------------------------------

class UdpTestSlaveDaq : public UdpTestSlaveCommands {
protected:
    void SetUp() override {
        UdpTestSlaveCommands::SetUp();
        slave_.SetDaqSimulationEnabled(true);
    }
};

TEST_F(UdpTestSlaveCommands, DaqCommandsRejectedWhileSimulationDisabled) {
    // 默认关闭 → DAQ 组与 DOWNLOAD 一律 ERR_CMD_UNKNOWN（既有语义锁定）
    ASSERT_TRUE(Request(BytesOf({0xE2, 0x00, 0x00, 0x00, 0x00, 0x00}), res_));
    ASSERT_EQ(res_.size(), 2U);
    EXPECT_EQ(res_[0], static_cast<std::uint8_t>(PacketType::Err));
    EXPECT_EQ(res_[1], static_cast<std::uint8_t>(ErrorCode::CmdUnknown));
}

TEST_F(UdpTestSlaveDaq, ProcessorAndResolutionInfoReportActualKeyByte) {
    // GET_DAQ_PROCESSOR_INFO: RES [FF][PROPS][MAX_DAQ(2)][MAX_EVENT(2)]
    //                           [MIN_DAQ][KEY_BYTE]
    ASSERT_TRUE(Request(BytesOf({0xDA}), res_));
    ASSERT_EQ(res_.size(), 8U);
    EXPECT_EQ(res_[0], static_cast<std::uint8_t>(PacketType::Res));
    EXPECT_EQ(res_[1] & 0x01U, 0U) << "DAQ_CONFIG_TYPE 必须为 Static（B-5）";
    EXPECT_EQ(res_[6], 0x00U);
    EXPECT_EQ(res_[7], 0x30U) << "DAQ_KEY_BYTE: ADDRESS_EXTENSION=DAQ, "
                                 "IDENTIFICATION=ABSOLUTE（B-16 比对真值）";

    // GET_DAQ_RESOLUTION_INFO: RES
    // [FF][GRAN][MAX][GRAN][MAX][TS_MODE][TICKS(2)]
    ASSERT_TRUE(Request(BytesOf({0xD9}), res_));
    ASSERT_EQ(res_.size(), 8U);
    EXPECT_EQ(res_[1], 1U);     // 粒度 1 字节
    EXPECT_EQ(res_[2], 8U);     // Entry 上限 8 字节
    EXPECT_EQ(res_[5], 0x31U);  // TIMESTAMP_MODE（非 FIXED）
}

TEST_F(UdpTestSlaveDaq, WriteDaqThenReadDaqRoundTripsEntry) {
    // SET_DAQ_PTR(daq=1, odt=0, entry=0)
    ASSERT_TRUE(Request(BytesOf({0xE2, 0x00, 0x01, 0x00, 0x00, 0x00}), res_));
    ASSERT_EQ(res_.size(), 1U);
    // WRITE_DAQ: [E1][0xFF 无位偏][size=2][ext=0][addr=0x00003000 小端]
    ASSERT_TRUE(Request(
        BytesOf({0xE1, 0xFF, 0x02, 0x00, 0x00, 0x30, 0x00, 0x00}), res_));
    ASSERT_EQ(res_.size(), 1U);
    // READ_DAQ 读回的正是刚写入的 Entry（指针自增后被重新定位）
    ASSERT_TRUE(Request(BytesOf({0xE2, 0x00, 0x01, 0x00, 0x00, 0x00}), res_));
    ASSERT_TRUE(Request(BytesOf({0xDB}), res_));
    ASSERT_EQ(res_.size(), 8U);
    EXPECT_EQ(res_[0], static_cast<std::uint8_t>(PacketType::Res));
    EXPECT_EQ(res_[1], 0xFFU);  // BIT_OFFSET=0xFF（无位偏）
    EXPECT_EQ(res_[2], 2U);     // SIZE
    EXPECT_EQ(res_[3], 0U);     // EXT
    EXPECT_EQ((res_[4] | (res_[5] << 8)), 0x3000U);  // ADDR
}

TEST_F(UdpTestSlaveDaq, WriteDaqOnPredefinedListIsWriteProtected) {
    slave_.SetDaqListPredefined(3, true);  // 3 号列表标记为 PREDEFINED
    // 先定位指针（这一步本身合法，RES 1 字节）
    ASSERT_TRUE(Request(BytesOf({0xE2, 0x00, 0x03, 0x00, 0x00, 0x00}), res_));
    ASSERT_EQ(res_.size(), 1U);
    // 再写 Entry：docs L2168 → ERR_WRITE_PROTECTED
    ASSERT_TRUE(Request(
        BytesOf({0xE1, 0xFF, 0x01, 0x00, 0x00, 0x50, 0x00, 0x00}), res_));
    ASSERT_EQ(res_.size(), 2U);
    EXPECT_EQ(res_[0], static_cast<std::uint8_t>(PacketType::Err));
    EXPECT_EQ(res_[1], static_cast<std::uint8_t>(ErrorCode::WriteProtected))
        << "docs L2168：WRITE_DAQ 指向 PREDEFINED 列表必须 ERR_WRITE_PROTECTED";
}

TEST_F(UdpTestSlaveDaq, OutOfRangeDaqObjectRejected) {
    // docs L2153：指定对象不存在 → ERR_OUT_OF_RANGE
    ASSERT_TRUE(Request(BytesOf({0xE2, 0x00, 0x63, 0x00, 0x00, 0x00}), res_));
    ASSERT_EQ(res_.size(), 2U);
    EXPECT_EQ(res_[0], static_cast<std::uint8_t>(PacketType::Err));
    EXPECT_EQ(res_[1], static_cast<std::uint8_t>(ErrorCode::OutOfRange));
}

TEST_F(UdpTestSlaveDaq,
       StartStopDaqListReturnsFirstPidAndGetStatusReflectsRun) {
    ASSERT_TRUE(Request(BytesOf({0xE2, 0x00, 0x02, 0x00, 0x00, 0x00}), res_));
    ASSERT_TRUE(Request(
        BytesOf({0xE1, 0xFF, 0x01, 0x00, 0x00, 0x20, 0x30, 0x00}), res_));
    // daq=2 的 FIRST_PID = 0x10 + 2*4 = 0x18（预留 4 个连续 PID）
    ASSERT_TRUE(Request(BytesOf({0xDE, 0x01, 0x02, 0x00}), res_));
    ASSERT_EQ(res_.size(), 2U);
    EXPECT_EQ(res_[0], static_cast<std::uint8_t>(PacketType::Res));
    EXPECT_EQ(res_[1], 0x18U);
    // GET_STATUS 的 DAQ_RUNNING（bit6）应随之置位（docs L2220）
    ASSERT_TRUE(Request(BytesOf({0xFD, 0x00}), res_));
    ASSERT_GE(res_.size(), 2U);
    EXPECT_NE(res_[1] & 0x40U, 0U);
    // STOP ALL 后清除
    ASSERT_TRUE(Request(BytesOf({0xDD, 0x00}), res_));
    ASSERT_TRUE(Request(BytesOf({0xFD, 0x00}), res_));
    EXPECT_EQ(res_[1] & 0x40U, 0U);
}

TEST_F(UdpTestSlaveDaq, GetDaqListInfoReportsCapacityAndNoFirstPid) {
    // docs L2452-2457：本命令响应只有 PROPERTIES/MAX_ODT/MAX_ODT_ENTRIES/
    // FIXED_EVENT 四项 —— **不含 FIRST_PID**（那是 START_STOP_DAQ_LIST 的）
    ASSERT_TRUE(Request(BytesOf({0xD8, 0x00, 0x02, 0x00}), res_));
    ASSERT_EQ(res_.size(), 6U);
    EXPECT_EQ(res_[0], static_cast<std::uint8_t>(PacketType::Res));
    EXPECT_EQ(res_[1] & 0x01U, 0U) << "未标记 PREDEFINED";
    EXPECT_EQ(res_[2], 4U);                     // MAX_ODT
    EXPECT_EQ(res_[3], 8U);                     // MAX_ODT_ENTRIES
    EXPECT_EQ((res_[4] | (res_[5] << 8)), 0U);  // FIXED_EVENT
}

TEST_F(UdpTestSlaveDaq, DownloadWritesAtMtaAndAdvancesIt) {
    slave_.SetMemory(0x4000, BytesOf({0x00, 0x00, 0x00, 0x00}));
    // SET_MTA(0x4000) → DOWNLOAD 2 元素 → SHORT_UPLOAD 读回
    ASSERT_TRUE(Request(
        BytesOf({0xF6, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00}), res_));
    ASSERT_TRUE(Request(BytesOf({0xF0, 0x02, 0xAA, 0xBB}), res_));
    ASSERT_EQ(res_.size(), 1U);
    ASSERT_TRUE(Request(
        BytesOf({0xF4, 0x02, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00}), res_));
    ASSERT_EQ(res_.size(), 3U);
    EXPECT_EQ(res_[1], 0xAAU);
    EXPECT_EQ(res_[2], 0xBBU);
}

TEST_F(UdpTestSlaveDaq, ShortDownloadWritesGivenAddressAndDownloadNextUnknown) {
    slave_.SetMemory(0x4100, BytesOf({0x00, 0x00}));
    // SHORT_DOWNLOAD: [ED][n=2][00][ext=00][addr 小端 00 41 00 00][data...]
    ASSERT_TRUE(Request(
        BytesOf({0xED, 0x02, 0x00, 0x00, 0x00, 0x41, 0x00, 0x00, 0x11, 0x22}),
        res_));
    ASSERT_EQ(res_.size(), 1U);
    ASSERT_TRUE(Request(
        BytesOf({0xF4, 0x02, 0x00, 0x00, 0x00, 0x41, 0x00, 0x00}), res_));
    ASSERT_EQ(res_.size(), 3U);
    EXPECT_EQ(res_[1], 0x11U);
    EXPECT_EQ(res_[2], 0x22U);
    // DOWNLOAD_NEXT 属 Block Mode，未实现 → 仍须按 docs L1631 回 CMD_UNKNOWN
    ASSERT_TRUE(Request(BytesOf({0xEF, 0x00}), res_));
    ASSERT_EQ(res_.size(), 2U);
    EXPECT_EQ(res_[1], static_cast<std::uint8_t>(ErrorCode::CmdUnknown));
}

TEST_F(UdpTestSlaveDaq, ClearDaqListResetsEntries) {
    ASSERT_TRUE(Request(BytesOf({0xE2, 0x00, 0x01, 0x00, 0x00, 0x00}), res_));
    ASSERT_TRUE(Request(
        BytesOf({0xE1, 0xFF, 0x01, 0x00, 0x00, 0x50, 0x00, 0x00}), res_));
    ASSERT_TRUE(Request(BytesOf({0xE3, 0x00, 0x01, 0x00}), res_));  // CLEAR
    ASSERT_EQ(res_.size(), 1U);
    // 清除后再 READ_DAQ：Entry 已不存在 → 不猜，回 ERR_OUT_OF_RANGE
    ASSERT_TRUE(Request(BytesOf({0xE2, 0x00, 0x01, 0x00, 0x00, 0x00}), res_));
    ASSERT_TRUE(Request(BytesOf({0xDB}), res_));
    ASSERT_EQ(res_.size(), 2U);
    EXPECT_EQ(res_[0], static_cast<std::uint8_t>(PacketType::Err));
    EXPECT_EQ(res_[1], static_cast<std::uint8_t>(ErrorCode::OutOfRange));
}

}  // namespace
}  // namespace calmcar::xcp::test
