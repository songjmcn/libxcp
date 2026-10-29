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

#include <gtest/gtest.h>

#include "libxcp/a2l/a2l_bridge.hpp"
#include "libxcp/tcp_transport.hpp"
#include "libxcp/tcp_transport_config.hpp"
#include "libxcp/udp_transport.hpp"
#include "libxcp/xcp_master.hpp"

#include "libxcp/transport_selection.hpp"
#include "xcp_v12_slave_process.hpp"
#include "../examples/xcp_slave_demo_v12/slave_model.hpp"

namespace calmcar::xcp::test {
namespace {

namespace a2l = calmcar::xcp::a2l;
using xcp_slave_demo_v12::CalibrationParameters;
using xcp_slave_demo_v12::FastMeasurements;

/** @brief 按协议类型构造指向独立 V1.2 从站的 Master Transport。 */
std::unique_ptr<IXcpTransport> MakeTransport(
    const std::string& type, std::uint16_t port,
    const std::optional<calmcar::xcp::ResolvedTransportSelection>& selection) {
    if (type == "tcp") {
        TcpTransportConfig config;
        config.role = TcpRole::Master;
        config.remote_host = selection->host;
        config.remote_port = selection->port;
        config.local_host = "127.0.0.1";
        config.local_port = 0;
        config.packet_alignment = selection->alignment;
        config.wire_profile = selection->wire_profile;
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

        // Start 已用真实 CONNECT 探测就绪；此处不声称此前零连接。
        // 仅提取 TCP 端点，显式线路参数不依赖 A2L 对齐推断。
        std::optional<calmcar::xcp::ResolvedTransportSelection> selection;
        if (transport_name_ == "tcp") {
            const auto* info = bridge_->XcpInfo();
            ASSERT_NE(info, nullptr);
            ASSERT_FALSE(info->ambiguous);
            calmcar::xcp::A2lTransportFacts facts;
            for (const auto& endpoint : info->transports) {
                if (endpoint.kind != a2l::TransportEndpoint::Kind::TcpIp) continue;
                if (!facts.declared) {
                    facts.declared = true;
                    facts.host = endpoint.remote_host;
                    facts.port = endpoint.remote_port;
                }
                ++facts.instance_count;
                facts.instances.push_back({endpoint.remote_host, endpoint.remote_port,
                                           0, endpoint.version});
            }
            ASSERT_TRUE(facts.declared) << "A2L 未声明 TCP 端点";
            ASSERT_EQ(facts.host, "127.0.0.1");
            ASSERT_EQ(facts.port, slave_->Port());
            calmcar::xcp::TransportSelectionRequest request;
            request.transport = calmcar::xcp::TransportKind::Tcp;
            request.tcp_profile = TcpWireProfile::AsamV14;
            request.packet_alignment_bits = std::uint8_t{8};
            std::string error;
            selection = calmcar::xcp::ResolveTransportSelection(request, facts, &error);
            ASSERT_TRUE(selection.has_value()) << error;
            ASSERT_EQ(selection->endpoint_source, calmcar::xcp::EndpointSource::A2l);
            ASSERT_EQ(selection->host, facts.host);
            ASSERT_EQ(selection->port, facts.port);
            ASSERT_EQ(selection->wire_profile, TcpWireProfile::AsamV14);
            ASSERT_EQ(selection->alignment, PacketAlignment::Bit8);
        }

        XcpMasterOptions options;
        options.timeouts = CommandTimeouts{std::chrono::milliseconds(1800),
                                           std::chrono::milliseconds(1800), 1};
        master_ = std::make_unique<XcpMaster>(
            MakeTransport(transport_name_, slave_->Port(), selection), options);
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

}  // namespace
}  // namespace calmcar::xcp::test
