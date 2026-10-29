/**
 * @file xcp_v15_alignment_interop_test.cpp
 * @brief 用独立进程从站验收 Master 在 PACKET_ALIGNMENT = 8/16/32 下的真实线路行为。
 *
 * @par 证据边界（必须与结论一起陈述）
 *   对端是 `tests/xcp_v15_alignment_slave.cpp` 包起来的测试从站，其 V1.5 framing
 *   在 `tests/tcp_test_slave.cpp` 中**独立实现**（刻意不复用生产 decoder）。
 *   因此正向用例能证明"两侧对 LEN 与 LEN 外 Tail 的理解一致"，负向用例能证明
 *   "Master 确实在按 LEN/CTR 校验而非读到什么算什么"。
 *   但 V1.5 的 Tail 语义在仓库内没有第三方实现可对照，故**不得**把本文件的
 *   结果表述为"已与第三方 V1.5 实现互操作"或"V1.5 一致性已认证"。
 */
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "libxcp/protocol_types.hpp"
#include "libxcp/tcp_transport.hpp"
#include "libxcp/tcp_transport_config.hpp"
#include "libxcp/xcp_error.hpp"
#include "libxcp/xcp_master.hpp"

#include "xcp_v15_slave_process.hpp"

namespace calmcar::xcp::test {
namespace {

/// @brief 与从站约定的模拟内存基地址。
constexpr Address kMemoryBase = 0x1000;

/// @brief 从站模拟内存中固定的填充字节。
constexpr std::uint8_t kContentByte = 0x5A;

/// @brief 单次读取的字节数。
constexpr std::size_t kReadSize = 16;

/**
 * @brief 把位宽转换为库内对齐枚举。
 * @param bits 位宽（8/16/32）
 * @return 对应枚举
 */
PacketAlignment AlignmentOf(std::uint8_t bits) {
    switch (bits) {
        case 8:
            return PacketAlignment::Bit8;
        case 16:
            return PacketAlignment::Bit16;
        case 32:
            return PacketAlignment::Bit32;
        default:
            throw std::invalid_argument("用例参数只允许 8/16/32");
    }
}

/**
 * @brief 构造指向回环从站的 V1.5 Master Transport。
 * @param port 从站端口
 * @param alignment Master 侧显式配置的对齐
 * @return Master Transport
 */
std::unique_ptr<IXcpTransport> MakeMasterTransport(std::uint16_t port,
                                                   PacketAlignment alignment) {
    TcpTransportConfig config;
    config.role = TcpRole::Master;
    config.remote_host = "127.0.0.1";
    config.remote_port = port;
    config.local_host = "127.0.0.1";
    config.local_port = 0;
    config.packet_alignment = alignment;
    config.wire_profile = TcpWireProfile::AsamV15;
    config.connect_timeout_ms = 2000;
    return std::make_unique<TcpTransport>(config);
}

/// @brief 超时配置：负向用例需要短超时，避免整套用例被拖长。
XcpMasterOptions MakeMasterOptions(std::chrono::milliseconds timeout) {
    XcpMasterOptions options;
    options.timeouts = CommandTimeouts{timeout, timeout, 0};
    return options;
}

/**
 * @brief 断言读到的字节全部等于从站填充值。
 * @param data 读到的数据
 */
void ExpectContentBytes(const Bytes& data) {
    ASSERT_EQ(data.size(), kReadSize);
    for (std::size_t index = 0; index < data.size(); ++index) {
        EXPECT_EQ(data[index], kContentByte) << "第 " << index << " 字节不符";
    }
}

}  // namespace

/**
 * @brief 正向矩阵：从站与 Master 使用同一 alignment 时必须正常互通。
 * @details 除读取外还做一次 DISCONNECT → 重连，验证对齐参数不会在第二个
 *          连接上"碰巧"失效（从站只维护一条活动连接，重连路径独立）。
 */
class XcpV15AlignmentInteropTest : public ::testing::TestWithParam<std::uint8_t> {
protected:
    /// @brief 启动指定 alignment 的从站进程。
    void SetUp() override {
        slave_ = std::make_unique<XcpV15SlaveProcess>(GetParam());
        ASSERT_NO_THROW(slave_->Start());
    }

    /// @brief 回收从站进程。
    void TearDown() override {
        if (slave_) {
            slave_->Stop();
        }
    }

    std::unique_ptr<XcpV15SlaveProcess> slave_;  ///< 独立进程从站
};

TEST_P(XcpV15AlignmentInteropTest, ReadsMemoryWithMatchingAlignment) {
    const std::uint8_t bits = GetParam();
    SCOPED_TRACE("PACKET_ALIGNMENT=" + std::to_string(bits));

    auto master = std::make_unique<XcpMaster>(
        MakeMasterTransport(slave_->Port(), AlignmentOf(bits)),
        MakeMasterOptions(std::chrono::milliseconds(2000)));
    ASSERT_NO_THROW(master->Connect());
    ASSERT_TRUE(master->IsConnected());

    const Bytes data = master->ReadMemoryBytes(kMemoryBase, 0, kReadSize);
    ExpectContentBytes(data);

    ASSERT_NO_THROW(master->Disconnect());
    EXPECT_FALSE(master->IsConnected());
}

TEST_P(XcpV15AlignmentInteropTest, ReconnectsAndReadsAgain) {
    const std::uint8_t bits = GetParam();
    SCOPED_TRACE("PACKET_ALIGNMENT=" + std::to_string(bits));

    {
        auto first = std::make_unique<XcpMaster>(
            MakeMasterTransport(slave_->Port(), AlignmentOf(bits)),
            MakeMasterOptions(std::chrono::milliseconds(2000)));
        ASSERT_NO_THROW(first->Connect());
        ExpectContentBytes(first->ReadMemoryBytes(kMemoryBase, 0, kReadSize));
        ASSERT_NO_THROW(first->Disconnect());
    }

    // 第二次连接：从站需重新 accept，framing 状态（CTR 等）也必须重新对齐。
    auto second = std::make_unique<XcpMaster>(
        MakeMasterTransport(slave_->Port(), AlignmentOf(bits)),
        MakeMasterOptions(std::chrono::milliseconds(2000)));
    ASSERT_NO_THROW(second->Connect());
    ExpectContentBytes(second->ReadMemoryBytes(kMemoryBase, 0, kReadSize));
    ASSERT_NO_THROW(second->Disconnect());
}

INSTANTIATE_TEST_SUITE_P(
    Alignment8_16_32, XcpV15AlignmentInteropTest,
    ::testing::Values(std::uint8_t{8}, std::uint8_t{16}, std::uint8_t{32}),
    // 默认参数名会把 8/16/32 渲染成 '\b' / '\x10' / ' '，可读性差且不便筛选，
    // 故显式给出 Alignment8 / Alignment16 / Alignment32。
    [](const ::testing::TestParamInfo<std::uint8_t>& info) {
        return "Alignment" + std::to_string(info.param);
    });

/**
 * @brief 负向用例：Master 的 alignment 与从站不一致时绝不返回"成功读取"。
 * @details 这是本文件里最有价值的一条：正向互通只能说明两侧理解一致，
 *          只有错配失败才能证明 Master 真的按 LEN 校验帧边界。
 */
TEST(XcpV15AlignmentNegativeTest, MismatchedAlignmentNeverSucceeds) {
    XcpV15SlaveProcess slave(16);  // 从站按 16 位对齐填充 Tail
    ASSERT_NO_THROW(slave.Start());

    // 先证明该从站在匹配对齐下确实可用，否则"错配失败"可能只是因为从站根本
    // 没在服务（那将是假证据）。
    {
        auto baseline = std::make_unique<XcpMaster>(
            MakeMasterTransport(slave.Port(), PacketAlignment::Bit16),
            MakeMasterOptions(std::chrono::milliseconds(2000)));
        ASSERT_NO_THROW(baseline->Connect());
        ExpectContentBytes(baseline->ReadMemoryBytes(kMemoryBase, 0, kReadSize));
        ASSERT_NO_THROW(baseline->Disconnect());
    }

    auto master = std::make_unique<XcpMaster>(
        MakeMasterTransport(slave.Port(), PacketAlignment::Bit8),  // 故意错配
        MakeMasterOptions(std::chrono::milliseconds(1200)));

    bool read_succeeded = false;
    bool completed_without_error = false;
    try {
        master->Connect();
        completed_without_error = true;
        const Bytes data = master->ReadMemoryBytes(kMemoryBase, 0, kReadSize);
        read_succeeded = (data.size() == kReadSize);
    } catch (const XcpException&) {
        // 预期路径：超时/协议错误/流不可恢复
    } catch (const std::exception&) {
        // 其它异常同样视为"没有假成功"
    }

    // 关键断言：绝不能读到"看似正确"的数据
    EXPECT_FALSE(read_succeeded)
        << "alignment 错配却读到了完整数据，说明 Master 未按 LEN 校验帧边界"
        << "（连接是否建立=" << completed_without_error << "）";
}

/**
 * @brief 负向用例：V1.5 未显式给出 alignment 时配置层必须拒绝。
 */
TEST(XcpV15AlignmentNegativeTest, UnknownAlignmentIsRejectedByConfig) {
    auto transport = MakeMasterTransport(1, PacketAlignment::Unknown);
    XcpMaster master(std::move(transport),
                     MakeMasterOptions(std::chrono::milliseconds(500)));
    EXPECT_THROW(master.Connect(), XcpException);
    EXPECT_FALSE(master.IsConnected());
}

/**
 * @brief 负向用例：从站把响应 LEN 故意写大时，Master 必须失败。
 * @details 复用 `TcpFaultInjection::corrupt_len_n`（LEN = 实际字节 + 64），
 *          作用于 CONNECT 响应。Master 若严格按 LEN 读取，就必须等不到那 64
 *          字节而以超时/协议错误结束；若它"读到什么算什么"，CONNECT 仍会
 *          成功——本用例专门证伪后者。
 *
 * @note 首次实现曾用"连接建立后另发一段畸形帧"的方式，实测**不可靠**：畸形
 *       帧可能与正常响应交叉到达，用例时过时不过。改用从站自身确定性的
 *       响应级注入后才具备可重复性。
 */
TEST(XcpV15AlignmentNegativeTest, CorruptedLengthFailsInsteadOfSucceeding) {
    // 第 1 个响应即 CONNECT 响应
    XcpV15SlaveProcess slave(8, /*corrupt_len_at=*/1);
    ASSERT_NO_THROW(slave.Start());

    auto master = std::make_unique<XcpMaster>(
        MakeMasterTransport(slave.Port(), PacketAlignment::Bit8),
        MakeMasterOptions(std::chrono::milliseconds(1200)));

    bool connected = false;
    bool read_succeeded = false;
    try {
        master->Connect();
        connected = true;
        const Bytes data = master->ReadMemoryBytes(kMemoryBase, 0, kReadSize);
        read_succeeded = (data.size() == kReadSize);
    } catch (const XcpException&) {
        // 预期路径：LEN 与实际字节不符 → 超时或协议错误
    } catch (const std::exception&) {
    }

    EXPECT_FALSE(read_succeeded)
        << "LEN 与实际字节不符时仍读到了完整数据，说明流校验失效"
        << "（CONNECT 是否成功=" << connected << "）";
    EXPECT_FALSE(master->IsConnected());
}

}  // namespace calmcar::xcp::test