/**
 * @file xcp_v12_interop_test.cpp
 * @brief 用 libxcp Master 验收 XCPlite V1.2 测量读取与标定写回。
 */
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include "libxcp/a2l/a2l_bridge.hpp"
#include "libxcp/tcp_transport.hpp"
#include "libxcp/tcp_transport_config.hpp"
#include "libxcp/udp_transport.hpp"
#include "libxcp/xcp_master.hpp"
#include "src/detail/socket/socket_ops.hpp"

#include "xcp_v12_slave_process.hpp"
#include "../examples/xcp_slave_demo_v12/slave_model.hpp"

namespace calmcar::xcp::test {
namespace {

namespace a2l = calmcar::xcp::a2l;
using xcp_slave_demo_v12::CalibrationParameters;
using xcp_slave_demo_v12::FastMeasurements;

/** @brief 按协议类型构造指向独立 V1.2 从站的 Master Transport。 */
std::unique_ptr<IXcpTransport> MakeTransport(const std::string& type,
                                             std::uint16_t port) {
    if (type == "tcp") {
        TcpTransportConfig config;
        config.role = TcpRole::Master;
        config.remote_host = "127.0.0.1";
        config.remote_port = port;
        config.local_host = "127.0.0.1";
        config.local_port = 0;
        config.packet_alignment = PacketAlignment::Bit8;
        config.wire_profile = TcpWireProfile::AsamV14;
        config.connect_timeout_ms = 1500;
        return std::make_unique<TcpTransport>(config);
    }
    UdpTransportConfig config;
    config.remote_host = "127.0.0.1";
    config.remote_port = port;
    config.local_host = "127.0.0.1";
    config.local_port = 0;
    config.receive_poll_interval_ms = 10;
    return std::make_unique<UdpTransport>(config);
}

/** @brief 检查并转换 A2L 地址为 libxcp 的 XCP 地址类型。 */
Address ToAddress(const a2l::SymbolInfo& symbol) {
    if (symbol.xcp_address > std::numeric_limits<Address>::max()) {
        throw std::overflow_error("A2L XCP 地址超出 Master 地址宽度");
    }
    return static_cast<Address>(symbol.xcp_address);
}

TEST(XcpliteV12A2lGenerationTest, RewritesAndParsesA2lForTcpAndUdp) {
    for (const std::string transport : {"udp", "tcp"}) {
        SCOPED_TRACE("transport=" + transport);
        XcpliteV12SlaveProcess generator(transport);
        ASSERT_NO_THROW(generator.VerifyA2lOverwrite());

        std::ifstream input(generator.A2lPath(), std::ios::binary);
        ASSERT_TRUE(input.is_open());
        const std::string content{std::istreambuf_iterator<char>(input),
                                  std::istreambuf_iterator<char>()};
        EXPECT_NE(content.find(transport == "tcp" ? "XCP_ON_TCP_IP"
                                                   : "XCP_ON_UDP_IP"),
                  std::string::npos);
        EXPECT_NE(content.find("XCP_104.aml"), std::string::npos);
        EXPECT_NE(content.find("READ_WRITE"), std::string::npos);

        auto loaded = a2l::A2lBridge::Load(generator.A2lPath().string());
        ASSERT_TRUE(loaded.HasValue())
            << "A2L 二次覆盖后无法解析: " << loaded.ErrorInfo().message;
        auto fast = loaded.Value()->Database()->Find(
            "xcp_slave_demo_v12::fast");
        ASSERT_TRUE(fast.HasValue()) << fast.ErrorInfo().message;
        auto cal = loaded.Value()->Database()->Find("xcp_slave_demo_v12::cal");
        ASSERT_TRUE(cal.HasValue()) << cal.ErrorInfo().message;
        EXPECT_EQ(fast.Value().address_extension, 3U);
        EXPECT_EQ(cal.Value().xcp_address, 0x80000000U);
    }
}

class XcpliteV12InteropTest : public ::testing::TestWithParam<const char*> {
protected:
    /** @brief 启动 V1.2 对端、加载 A2L 并完成 Master CONNECT。 */
    void SetUp() override {
        transport_name_ = GetParam();
        slave_ = std::make_unique<XcpliteV12SlaveProcess>(transport_name_);
        ASSERT_NO_THROW(slave_->Start());

        auto loaded = a2l::A2lBridge::Load(slave_->A2lPath().string());
        ASSERT_TRUE(loaded.HasValue())
            << "V1.2 A2L 加载失败: " << loaded.ErrorInfo().message;
        bridge_ = std::move(loaded.Value());
        ASSERT_NE(bridge_->Database(), nullptr);

        XcpMasterOptions options;
        options.timeouts = CommandTimeouts{std::chrono::milliseconds(1800),
                                           std::chrono::milliseconds(1800), 1};
        master_ = std::make_unique<XcpMaster>(
            MakeTransport(transport_name_, slave_->Port()), options);
        ASSERT_NO_THROW(master_->Connect()) << "Master CONNECT 失败";
    }

    /** @brief 断开 Master 并回收 V1.2 子进程及其临时文件。 */
    void TearDown() override {
        if (master_ && master_->IsConnected()) {
            try {
                master_->Disconnect();
            } catch (const std::exception&) {
                // 清理阶段继续回收从站；用例中的协议错误已由主断言报告。
            }
        }
        master_.reset();
        if (slave_) {
            slave_->Stop();
        }
    }

    /** @brief 通过 MODULE::symbol 从已加载 A2L 数据库查询实例。 */
    [[nodiscard]] a2l::SymbolInfo FindSymbol(const std::string& name) {
        auto result = bridge_->Database()->Find("xcp_slave_demo_v12::" + name);
        EXPECT_TRUE(result.HasValue()) << "A2L 未找到符号 " << name << ": "
                                       << result.ErrorInfo().message;
        return result.HasValue() ? result.Value() : a2l::SymbolInfo{};
    }

    std::string transport_name_;  ///< 当前验收使用的传输协议。
    std::unique_ptr<XcpliteV12SlaveProcess> slave_;  ///< 独立 V1.2 从站进程。
    std::unique_ptr<a2l::A2lBridge> bridge_;  ///< 从站生成的 A2L 桥接视图。
    std::unique_ptr<XcpMaster> master_;  ///< 驱动 V1.2 对端的 libxcp 主站。
};

TEST_P(XcpliteV12InteropTest, ReadsMeasurementThroughA2lAddress) {
    const auto fast = FindSymbol("fast");
    const Address fast_address = ToAddress(fast);
    const auto block = master_->ReadMemoryBytes(
        fast_address, fast.address_extension,
        static_cast<ByteCount>(sizeof(FastMeasurements)));
    ASSERT_EQ(block.size(), sizeof(FastMeasurements));

    FastMeasurements measurements{};
    std::memcpy(&measurements, block.data(), sizeof(measurements));
    EXPECT_GT(measurements.counter, 0U);
    EXPECT_TRUE(std::isfinite(measurements.sine));
    EXPECT_LE(std::fabs(measurements.sine), 100.0F);
}

TEST_P(XcpliteV12InteropTest,
       CalibrationWriteIsReadBackAndChangesMeasurementArray) {
    const auto cal = FindSymbol("cal");
    const Address cal_address = ToAddress(cal);

    // V1.2 的参考页为 1、工作页为 0；每个传输用例先复位工作页。
    master_->CopyCalPage(CopyCalPageRequest{0, 1, 0, 0});
    master_->SetCalPage(CalPageModeBit::kXcp, 0, 0);
    const auto active_page = master_->GetCalPage(CalPageAccessMode::Xcp, 0);
    ASSERT_TRUE(active_page.has_value());
    ASSERT_EQ(active_page->page, 0U);

    constexpr float kCorrection = 0.75F;
    const Address correction_address =
        cal_address + static_cast<Address>(
                          offsetof(CalibrationParameters, corrections));
    Bytes expected(sizeof(kCorrection));
    std::memcpy(expected.data(), &kCorrection, sizeof(kCorrection));
    master_->WriteMemoryBytes(correction_address, cal.address_extension, expected);
    const Bytes readback = master_->ReadMemoryBytes(
        correction_address, cal.address_extension, sizeof(kCorrection));
    ASSERT_EQ(readback, expected);

    const auto fast = FindSymbol("fast");
    const Address fast_address = ToAddress(fast);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    bool correction_observed = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto block = master_->ReadMemoryBytes(
            fast_address, fast.address_extension,
            static_cast<ByteCount>(sizeof(FastMeasurements)));
        if (block.size() != sizeof(FastMeasurements)) {
            break;
        }
        FastMeasurements measurements{};
        std::memcpy(&measurements, block.data(), sizeof(measurements));
        const float delta = measurements.samples[0] - measurements.sine;
        if (std::fabs(delta - kCorrection) < 0.05F) {
            correction_observed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(correction_observed)
        << "从站测量输出未观测到已写入的 corrections[0]=" << kCorrection;
}

INSTANTIATE_TEST_SUITE_P(TcpAndUdp, XcpliteV12InteropTest,
                         ::testing::Values("udp", "tcp"),
                         [](const auto& info) {
                             return std::string(info.param) == "tcp" ? "Tcp"
                                                                     : "Udp";
                         });

/// @brief 平台套接字操作别名，供端口占用辅助类使用。
namespace socket_ops = calmcar::xcp::detail;

/** @brief 获取当前测试进程 ID，复现夹具旧的端口基址公式。 */
std::uint32_t CurrentProcessId() noexcept {
#ifdef _WIN32
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}

/**
 * @brief 持有若干已绑定的回环 UDP 端口，模拟"候选端口整段不可用"。
 *
 * @details 用例用它在启动从站前占住旧策略的候选端口；析构时统一释放，保证
 *          断言失败也不会泄漏句柄。
 */
class PortHolder {
public:
    PortHolder() = default;
    ~PortHolder() { Release(); }

    PortHolder(const PortHolder&) = delete;
    PortHolder& operator=(const PortHolder&) = delete;

    /**
     * @brief 尝试绑定 127.0.0.1 上的指定 UDP 端口。
     * @param port 待占用的端口号。
     * @return 绑定成功返回 true；端口已被占用或位于系统保留段时返回 false。
     */
    bool TryBind(std::uint16_t port) {
        auto& ops = socket_ops::CurrentSocketOps();
        const auto handle = ops.create(socket_ops::SocketKind::Udp);
        if (handle == socket_ops::kInvalidSocketHandle) {
            return false;
        }
        socket_ops::Ipv4Endpoint endpoint{};
        endpoint.port = port;
        if (!socket_ops::ParseIpv4Literal("127.0.0.1", &endpoint.address) ||
            ops.bind(handle, endpoint) != 0) {
            ops.close(handle);
            return false;
        }
        handles_.push_back(handle);
        ports_.push_back(port);
        return true;
    }

    /** @brief 已成功占用的端口列表。 */
    [[nodiscard]] const std::vector<std::uint16_t>& ports() const noexcept {
        return ports_;
    }

    /** @brief 关闭全部占用端口并清空记录。 */
    void Release() noexcept {
        auto& ops = socket_ops::CurrentSocketOps();
        for (const auto handle : handles_) {
            ops.close(handle);
        }
        handles_.clear();
        ports_.clear();
    }

private:
    std::vector<socket_ops::SocketHandle> handles_;  ///< 已绑定的套接字句柄。
    std::vector<std::uint16_t> ports_;               ///< 已占用的端口号。
};

/**
 * @brief 旧固定基址上的候选端口全部不可绑定时，从站仍须就绪。
 *
 * @details 回归 Windows CI 上的 `10013` 事故：`44000 + pid % 10000` 起 16 个
 *          连续端口若整段落在 Hyper-V/WinNAT 保留段（或被其他进程占用），旧实现
 *          会连续 16 次"从站提前退出"后抛异常。新实现优先由操作系统动态分配端口
 *          （分配器自动跳过保留段），因此先占住旧候选端口，再断言从站仍能就绪，
 *          且最终端口不在被占集合内。
 */
TEST(XcpliteV12PortAllocationTest, StartsWhenLegacyCandidatePortsOccupied) {
    const auto base =
        static_cast<std::uint16_t>(44000U + (CurrentProcessId() % 10000U));
    PortHolder holder;
    for (std::uint16_t offset = 0; offset < 16; ++offset) {
        holder.TryBind(static_cast<std::uint16_t>(base + offset));
    }
    if (holder.ports().empty()) {
        GTEST_SKIP()
            << "旧候选端口全部无法占用（可能落在系统保留段），前提不成立";
    }

    XcpliteV12SlaveProcess slave("udp");
    ASSERT_NO_THROW(slave.Start());
    EXPECT_GT(slave.Port(), 0U);
    for (const auto occupied : holder.ports()) {
        EXPECT_NE(slave.Port(), occupied)
            << "从站不应使用已被占用的旧候选端口 " << occupied;
    }
    slave.Stop();
}

}  // namespace
}  // namespace calmcar::xcp::test
