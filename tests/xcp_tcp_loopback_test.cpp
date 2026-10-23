/**
 * @file xcp_tcp_loopback_test.cpp
 * @brief 真实 TCP Loopback 端到端测试：TcpTestSlave + TcpTransport + XcpMaster。
 *
 * 依据 code-plan/TCP传输_实施计划_B-T1至B-T5.md 第 6.3 节 Gate。
 * 不依赖固定端口（Slave 绑定 OS 分配的临时端口）、不依赖外网与固定时序。
 *
 * @par 验证性质的分层声明（计划 §6.3 明确要求）
 *   - **AsamV14 用例**：`TcpTestSlave` 的 framing 独立实现且逐字节复刻
 *     `thirdparty/XCPliteV1.2/src/xcpEthTl.c` 的收发路径，故此层可声称
 *     "与 XCPlite 语义一致"。
 *   - **AsamV15 用例**：LEN 外 Tail 行为**没有第三方实现可对照**，
 *     属**自有闭环验证**，不得宣称"已与独立实现互操作"。
 */

#include "libxcp/tcp_transport.hpp"
#include "libxcp/xcp_master.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "libxcp/tcp_wire_profile.hpp"
#include "tcp_test_slave.hpp"

namespace calmcar::xcp {
namespace {

Bytes BytesOf(std::initializer_list<std::uint8_t> init) {
    return Bytes(init.begin(), init.end());
}

/// @brief 构造指向测试 Slave 的 Master 侧 TCP Transport 配置
/// @param slave_port Slave 监听端口
/// @param profile 线格式档（显式指定，绝不自动推断）
/// @param alignment 对齐位宽（仅 AsamV15 影响 Tail）
TcpTransportConfig MakeConfig(std::uint16_t slave_port, TcpWireProfile profile,
                              PacketAlignment alignment = PacketAlignment::Bit8) {
    TcpTransportConfig cfg;
    cfg.role = TcpRole::Master;
    cfg.remote_host = "127.0.0.1";
    cfg.remote_port = slave_port;
    cfg.local_host = "127.0.0.1";
    cfg.local_port = 0;
    cfg.connect_timeout_ms = 2000;
    cfg.send_timeout_ms = 2000;
    cfg.wire_profile = profile;
    cfg.packet_alignment = alignment;
    return cfg;
}

/// @brief 用给定 Slave 端口与 Profile 构造一个 XcpMaster
std::unique_ptr<XcpMaster> MakeMaster(
    std::uint16_t slave_port, TcpWireProfile profile,
    PacketAlignment alignment = PacketAlignment::Bit8,
    CommandTimeouts timeouts = CommandTimeouts{}) {
    XcpMasterOptions options;
    options.timeouts = timeouts;
    options.autodetect_version = false;  // Slave 不实现 GET_VERSION
    return std::make_unique<XcpMaster>(
        std::make_unique<TcpTransport>(MakeConfig(slave_port, profile, alignment)),
        options);
}

/// @brief 双 Profile 参数化名称（供 TEST_P 复用）
struct ProfileCase {
    /// @brief 用例后缀名
    std::string name;
    /// @brief 线格式档
    TcpWireProfile profile;
    /// @brief 对齐位宽
    PacketAlignment alignment;
};

/**
 * @brief 取双 Profile 测试矩阵
 * @return 覆盖 AsamV14（无 Tail）与 AsamV15（Bit8/Bit16/Bit32）的用例集
 * @details AsamV15 覆盖三种对齐：Bit8 时 Tail 恒 0（两种 Profile 数值等价），
 *          Bit16/Bit32 才真正产生非零 Tail，是 Tail 逻辑的有效验证点。
 */
std::vector<ProfileCase> ProfileMatrix() {
    return {
        {"AsamV14Bit8", TcpWireProfile::AsamV14, PacketAlignment::Bit8},
        {"AsamV15Bit8", TcpWireProfile::AsamV15, PacketAlignment::Bit8},
        {"AsamV15Bit16", TcpWireProfile::AsamV15, PacketAlignment::Bit16},
        {"AsamV15Bit32", TcpWireProfile::AsamV15, PacketAlignment::Bit32},
    };
}

// --------------------------------------------------------------------------
// §6.3 Gate：双 Profile 完整会话
// --------------------------------------------------------------------------

class TcpLoopbackProfileTest : public ::testing::TestWithParam<ProfileCase> {};

TEST_P(TcpLoopbackProfileTest, FullSessionConnectReadDisconnect) {
    const ProfileCase pc = GetParam();

    test::TcpTestSlave slave(pc.profile);
    slave.SetPacketAlignment(pc.alignment);
    slave.Start();
    ASSERT_NE(slave.Port(), 0) << "Slave 应绑定 OS 分配的临时端口";

    const Bytes content = BytesOf({0xCA, 0xFE, 0xBA, 0xBE});
    slave.SetMemory(0x70012340, content);

    auto master = MakeMaster(slave.Port(), pc.profile, pc.alignment);
    master->Connect();
    ASSERT_TRUE(master->IsConnected());

    // CONNECT 响应应被正确解析（MAX_CTO / MAX_DTO 来自 Slave 宣告）
    EXPECT_EQ(master->GetSessionParameters().connect.max_cto, 8U);
    EXPECT_EQ(master->GetSessionParameters().connect.max_dto, 0xFFFFU);

    // SET_MTA + UPLOAD 真实往返（经 TCP 字节流）
    const Bytes got = master->ReadMemoryBytes(0x70012340, 0x00, 4);
    EXPECT_EQ(got, content) << "Profile=" << pc.name << " 下 UPLOAD 数据应一致";

    master->Disconnect();
    EXPECT_FALSE(master->IsConnected());
    slave.Stop();
}

TEST_P(TcpLoopbackProfileTest, ShortUploadMatchesUpload) {
    const ProfileCase pc = GetParam();

    test::TcpTestSlave slave(pc.profile);
    slave.SetPacketAlignment(pc.alignment);
    slave.Start();

    const Bytes content = BytesOf({0x11, 0x22, 0x33, 0x44});
    slave.SetMemory(0x4000, content);

    auto master = MakeMaster(slave.Port(), pc.profile, pc.alignment);
    master->Connect();

    // SHORT_UPLOAD 不依赖 MTA，应返回同一内容
    const Bytes got = master->ReadMemoryBytes(0x4000, 0x00, 4);
    EXPECT_EQ(got, content) << "Profile=" << pc.name << " 下 SHORT_UPLOAD 应一致";

    master->Disconnect();
    slave.Stop();
}

TEST_P(TcpLoopbackProfileTest, CtrAdvancesMonotonically) {
    const ProfileCase pc = GetParam();

    test::TcpTestSlave slave(pc.profile);
    slave.SetPacketAlignment(pc.alignment);
    slave.Start();

    auto master = MakeMaster(slave.Port(), pc.profile, pc.alignment);
    master->Connect();

    // 连续多次读取：Master 侧 RX CTR 期望值须随每帧推进。
    // Slave 的 CTR 从 0 起自增（XCPlite `gXcpTl.Ctr` 语义）。
    for (int i = 0; i < 5; ++i) {
        slave.SetMemory(static_cast<Address>(0x1000 + i), Bytes{0xAB});
        const Bytes got = master->ReadMemoryBytes(
            static_cast<Address>(0x1000 + i), 0x00, 1);
        ASSERT_EQ(got.size(), 1U) << "第 " << i << " 次读取";
        EXPECT_EQ(got[0], 0xAB) << "Profile=" << pc.name << " 第 " << i << " 次";
    }

    master->Disconnect();
    slave.Stop();
}

INSTANTIATE_TEST_SUITE_P(Profiles, TcpLoopbackProfileTest,
                         ::testing::ValuesIn(ProfileMatrix()),
                         [](const ::testing::TestParamInfo<ProfileCase>& info) {
                             return info.param.name;
                         });

// --------------------------------------------------------------------------
// §6.3 Gate：TCP 特有场景
// --------------------------------------------------------------------------

class TcpLoopbackTest : public ::testing::Test {};

TEST_F(TcpLoopbackTest, SlaveRejectsSecondConnection) {
    test::TcpTestSlave slave(TcpWireProfile::AsamV14);
    slave.Start();

    // 第一条连接被正常服务
    auto first = MakeMaster(slave.Port(), TcpWireProfile::AsamV14);
    first->Connect();
    ASSERT_TRUE(first->IsConnected());
    EXPECT_TRUE(slave.WaitForConnection(std::chrono::milliseconds(500)));

    // 第二条连接应被 Slave accept 后立即关闭：Master 的 Open 会失败或
    // 连接后立即断开。无论哪种，都不得出现"两条会话同时活动"。
    auto second = MakeMaster(slave.Port(), TcpWireProfile::AsamV14);
    bool second_connected = false;
    try {
        second->Connect();
        second_connected = second->IsConnected();
    } catch (const XcpException&) {
        second_connected = false;  // 被拒是预期结果之一
    }
    EXPECT_FALSE(second_connected)
        << "Slave 只服务一条会话，第二条连接必须被拒绝";

    // 第一条会话应仍然可用（拒绝第二连接不得影响已建立会话）
    slave.SetMemory(0x2000, Bytes{0x5A});
    const Bytes got = first->ReadMemoryBytes(0x2000, 0x00, 1);
    EXPECT_EQ(got, BytesOf({0x5A}));

    first->Disconnect();
    slave.Stop();
}

TEST_F(TcpLoopbackTest, ByteWiseSplitFramesAreReassembled) {
    test::TcpTestSlave slave(TcpWireProfile::AsamV14);
    slave.Start();

    auto master = MakeMaster(slave.Port(), TcpWireProfile::AsamV14);
    master->Connect();

    // 让 Slave 把第 1 个响应（CONNECT 之后的首个命令响应）逐字节写出。
    // 验证 Master 侧"recv 边界与 Frame 边界无关"。
    test::TcpFaultInjection fault;
    fault.split_bytes_n = 1;
    slave.SetFaultInjection(fault);

    slave.SetMemory(0x3000, Bytes{0x77, 0x88});
    const Bytes got = master->ReadMemoryBytes(0x3000, 0x00, 2);
    EXPECT_EQ(got, BytesOf({0x77, 0x88}))
        << "逐字节分片写出后，Master 必须能重组出正确 Frame";

    master->Disconnect();
    slave.Stop();
}

TEST_F(TcpLoopbackTest, FrameHeaderSplitAcrossWrites) {
    test::TcpTestSlave slave(TcpWireProfile::AsamV14);
    slave.Start();

    auto master = MakeMaster(slave.Port(), TcpWireProfile::AsamV14);
    master->Connect();

    // 第 1 个响应切成 3 字节块：4 字节 Header 会跨两次 write。
    // 这是真实 TCP 最常见的分段位置，必须被正确处理。
    test::TcpFaultInjection fault;
    fault.chunk_size_n = std::make_pair<std::size_t, std::size_t>(1, 3);
    slave.SetFaultInjection(fault);

    slave.SetMemory(0x3100, Bytes{0x99, 0xAA, 0xBB});
    const Bytes got = master->ReadMemoryBytes(0x3100, 0x00, 3);
    EXPECT_EQ(got, BytesOf({0x99, 0xAA, 0xBB}))
        << "帧头跨 write 边界时 Master 必须正确缓冲";

    master->Disconnect();
    slave.Stop();
}

// --------------------------------------------------------------------------
// §6.3 Gate：断链行为
// --------------------------------------------------------------------------

TEST_F(TcpLoopbackTest, PeerCloseAfterResponseIsObserved) {
    test::TcpTestSlave slave(TcpWireProfile::AsamV14);
    slave.Start();

    // 故障必须在 Connect **之前**装好：Connect 自身会消耗 4 个响应
    // （CONNECT、GET_VERSION、GET_STATUS、PAG 探测），响应序号是全局递增的。
    // 若 Connect 之后才注入 #1，序号早已越过，注入永远不会触发。
    test::TcpFaultInjection fault;
    fault.close_after_response_n = 5;  // Connect 用掉 1..4，第 5 个才是本次命令
    slave.SetFaultInjection(fault);

    auto master = MakeMaster(slave.Port(), TcpWireProfile::AsamV14);
    master->Connect();
    ASSERT_TRUE(master->IsConnected());

    // 该命令的响应应被收到（Slave 先写响应再关闭），随后通道关闭。
    // 本次调用可能抛出 Cancelled（F-11：Transport Close 属"取消类"唤醒，
    // 见 src/command_executor.cpp:445-449）——这是**预期**行为，不算失败。
    try {
        (void)master->ReadMemoryBytes(0x3200, 0x00, 1);
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::Cancelled)
            << "对端断链应报 Cancelled 而非 Timeout，避免误触发恢复";
    }

    // 等 Slave 侧观测到断链
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(500);
    while (slave.HasActiveConnection() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_FALSE(slave.HasActiveConnection()) << "Slave 侧连接应已关闭";

    // 核心断言：**下一条命令必须失败**——这是"通道已关闭"对调用方可观测的
    // 契约。注意不要求 `master->IsConnected()` 立刻变 false：按现有设计
    // （src/command_executor.cpp:439-451），Transport 关闭只是把执行器置为
    // cancelled/closed 并唤醒等待者，Session 状态要等到**下一次操作失败**
    // 才迁移（UDP 侧同一契约见 xcp_udp_loopback_test.cpp:871 用
    // GetSessionState()==Failed 断言）。因此这里断言"命令失败"而非"标志位翻转"。
    bool failed = false;
    try {
        (void)master->ReadMemoryBytes(0x3210, 0x00, 1);
    } catch (const XcpException&) {
        failed = true;
    }
    EXPECT_TRUE(failed) << "对端关闭后，后续命令必须失败而不是静默成功";

    slave.Stop();
}

TEST_F(TcpLoopbackTest, NoAutomaticReconnectAfterPeerClose) {
    test::TcpTestSlave slave(TcpWireProfile::AsamV14);
    slave.Start();

    auto master = MakeMaster(slave.Port(), TcpWireProfile::AsamV14);
    master->Connect();

    // 主动断开连接（模拟链路丢失）
    slave.CloseActiveConnection();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 断言"绝不静默重连"：Slave 侧不得再出现活动连接。
    // 若库自动重连，以下断言会失败——这正是本用例要守住的不变量。
    EXPECT_FALSE(slave.HasActiveConnection())
        << "对端关闭后 Slave 不得收到新的入站连接（库绝不静默重连）";

    bool failed = false;
    try {
        (void)master->ReadMemoryBytes(0x3400, 0x00, 1);
    } catch (const XcpException&) {
        failed = true;
    }
    EXPECT_TRUE(failed) << "断链后命令必须失败，不得被静默重连掩盖";

    EXPECT_FALSE(slave.HasActiveConnection())
        << "命令失败之后仍然不得发生自动重连";

    slave.Stop();
}

// --------------------------------------------------------------------------
// §6.3 Gate：Profile 不匹配必须显式失败（不得自动探测）
// --------------------------------------------------------------------------

TEST_F(TcpLoopbackTest, ProfileMismatchIsNotAutodetected) {
    // Slave 用 AsamV14（无 Tail），Master 却配 AsamV15 + Bit16。
    // Master 会按 Bit16 去读 Tail，而 Slave 不发 Tail —— 双方失步。
    // 要求：**不得**自动切档救场，必须显式失败或以错误数据暴露问题。
    test::TcpTestSlave slave(TcpWireProfile::AsamV14);
    slave.SetPacketAlignment(PacketAlignment::Bit8);
    slave.Start();

    auto master = MakeMaster(slave.Port(), TcpWireProfile::AsamV15,
                             PacketAlignment::Bit16);

    bool failed = false;
    try {
        master->Connect();
        // 若连接本身侥幸成功，首个数据命令必须暴露失步
        slave.SetMemory(0x3300, Bytes{0x01, 0x02});
        const Bytes got = master->ReadMemoryBytes(0x3300, 0x00, 2);
        failed = (got != BytesOf({0x01, 0x02}));
    } catch (const XcpException&) {
        failed = true;
    }
    EXPECT_TRUE(failed)
        << "Profile 不匹配时必须显式失败，不得自动探测后静默成功";

    slave.Stop();
}

// --------------------------------------------------------------------------
// §6.3 Gate：Dynamic DAQ 验收已显式删除（计划 §6.1）
// --------------------------------------------------------------------------

TEST_F(TcpLoopbackTest, DynamicDaqIsExplicitlyOutOfScope) {
    // 计划 §6.1 写明"否则删除 Dynamic DAQ 验收"。本批次据此**显式删除**。
    // 该用例以可执行的方式固定这一范围决定：Slave 不声明 DAQ 资源位
    // （RESOURCE = 0x05），因此 Master 侧不得声称 DAQ 可用。
    test::TcpTestSlave slave(TcpWireProfile::AsamV14);
    slave.Start();

    auto master = MakeMaster(slave.Port(), TcpWireProfile::AsamV14);
    master->Connect();

    const auto params = master->GetSessionParameters();
    // Resource 位定义（include/libxcp/protocol_types.hpp:538-543）：
    //   CalPag = 0x01(bit0)、Daq = 0x04(bit2)、Stim = 0x08(bit3)、Pgm = 0x10
    // 本 Slave 宣告 0x11 = CalPag | Pgm，DAQ 位必须为 0。
    EXPECT_EQ(params.connect.resource_mask & 0x04U, 0U)
        << "Slave 未声明 DAQ 资源位（bit2 = 0x04），Master 不得认为 DAQ 可用";
    // 同时确认 CAL/PAG 位确实置上了，避免"用了错的掩码所以恒过"
    EXPECT_EQ(params.connect.resource_mask & 0x01U, 0x01U)
        << "Slave 应声明 CAL/PAG 资源位";

    master->Disconnect();
    slave.Stop();
}

}  // namespace
}  // namespace calmcar::xcp