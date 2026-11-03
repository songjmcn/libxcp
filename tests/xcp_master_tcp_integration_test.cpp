/**
 * @file xcp_master_tcp_integration_test.cpp
 * @brief TCP Master 示例的自动从站、外部端点和标定持久性集成测试。
 */
#include "../examples/xcp_master_udp/demo_process.hpp"

#include <gtest/gtest.h>

#include <libxcp/a2l/a2l_bridge.hpp>
#include <libxcp/tcp_transport.hpp>
#include <libxcp/xcp_master.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
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

#include "socket_runtime_guard.hpp"

#ifndef XCP_MASTER_TCP_EXECUTABLE
#error "XCP_MASTER_TCP_EXECUTABLE must be supplied by CMake"
#endif
#ifndef XCP_SLAVE_DEMO_EXECUTABLE
#error "XCP_SLAVE_DEMO_EXECUTABLE must be supplied by CMake"
#endif

namespace {

namespace fs = std::filesystem;
namespace xcp = calmcar::xcp;
namespace a2l = calmcar::xcp::a2l;

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
void CloseSocket(NativeSocket socket) { closesocket(socket); }
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
void CloseSocket(NativeSocket socket) { close(socket); }
#endif

/**
 * @brief 从操作系统申请一个当前空闲的 loopback TCP 端口。
 * @return 已释放保留 socket 的端口号；失败时返回 0。
 */
std::uint16_t ReserveFreePort() {
    calmcar::xcp::test_support::EnsureSocketRuntime();
    const NativeSocket socket_handle =
        socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == kInvalidSocket) return 0;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(socket_handle, reinterpret_cast<const sockaddr*>(&address),
#ifdef _WIN32
             static_cast<int>(sizeof(address))) != 0) {
#else
             static_cast<socklen_t>(sizeof(address))) != 0) {
#endif
        CloseSocket(socket_handle);
        return 0;
    }
#ifdef _WIN32
    int address_length = sizeof(address);
#else
    socklen_t address_length = sizeof(address);
#endif
    if (getsockname(socket_handle, reinterpret_cast<sockaddr*>(&address),
                    &address_length) != 0) {
        CloseSocket(socket_handle);
        return 0;
    }
    const std::uint16_t port = ntohs(address.sin_port);
    CloseSocket(socket_handle);
    return port;
}

/// @brief 在限时内等待示例程序完成，并断言其退出码为 0。
bool RunChildToExit(const std::string& executable,
                    const fs::path& working_directory,
                    const std::vector<std::string>& arguments,
                    std::chrono::milliseconds timeout,
                    int expected_exit_code = 0) {
    xcp_example::DemoProcess process;
    std::string error;
    if (!process.Start(executable, working_directory, arguments, &error)) {
        ADD_FAILURE() << "启动子进程失败：" << error;
        return false;
    }
    int exit_code = -1;
    if (!process.WaitForExit(timeout, &exit_code)) {
        process.Stop();
        ADD_FAILURE() << "子进程未在时限内退出";
        return false;
    }
    EXPECT_EQ(exit_code, expected_exit_code);
    return exit_code == expected_exit_code;
}

/// @brief 生成与服务端点匹配的演示 A2L/AML。
bool GenerateA2l(const fs::path& directory, std::uint16_t port) {
    return RunChildToExit(XCP_SLAVE_DEMO_EXECUTABLE, directory,
                          {"--generate-a2l", "--transport", "tcp", "--bind",
                           "127.0.0.1", "--port", std::to_string(port),
                           "--output-dir", directory.string(), "--overwrite"},
                          std::chrono::seconds(30));
}

/// @brief 使用 AsamV14/8bit profile 构造 TCP Master。
std::unique_ptr<xcp::XcpMaster> MakeTcpMaster(std::uint16_t port) {
    xcp::TcpTransportConfig config;
    config.remote_host = "127.0.0.1";
    config.remote_port = port;
    config.wire_profile = xcp::TcpWireProfile::AsamV14;
    config.packet_alignment = xcp::PacketAlignment::Bit8;
    config.connect_timeout_ms = 1000;
    return std::make_unique<xcp::XcpMaster>(
        std::make_unique<xcp::TcpTransport>(config));
}

/// @brief 从 A2L 加载目标标量并读取当前物理值。
double ReadCalibrationValue(xcp::XcpMaster& master,
                            const a2l::IA2lDatabase& database,
                            const std::string& symbol_name) {
    const auto symbol = database.Find(symbol_name);
    if (!symbol.HasValue()) throw std::runtime_error("标定符号不存在于 A2L");
    const auto& info = symbol.Value();
    const auto raw = master.ReadMemoryBytes(
        static_cast<xcp::Address>(info.xcp_address),
        static_cast<xcp::AddressExtension>(info.address_extension),
        static_cast<xcp::ByteCount>(info.element_size_bytes));
    const auto physical = database.ToPhysical(symbol_name, raw);
    if (!physical.HasValue())
        throw std::runtime_error("标定值 ToPhysical 失败");
    if (const auto* value = std::get_if<double>(&physical.Value()))
        return *value;
    if (const auto* value = std::get_if<std::int64_t>(&physical.Value())) {
        return static_cast<double>(*value);
    }
    if (const auto* value = std::get_if<std::uint64_t>(&physical.Value())) {
        return static_cast<double>(*value);
    }
    throw std::runtime_error("标定物理值不是数值");
}

/// @brief 等待内置示例 TCP 服务响应 XCP CONNECT。
bool WaitForSlave(std::uint16_t port) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        auto master = MakeTcpMaster(port);
        try {
            master->Connect();
            master->Disconnect();
            return true;
        } catch (const std::exception&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    return false;
}

/// @brief 返回隔离的临时测试工作目录。
fs::path TestDirectory(const std::string& name) {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path path = fs::temp_directory_path() / "libxcp-xcp-master-tcp" /
                          (name + "-" + std::to_string(nonce));
    std::error_code error;
    fs::create_directories(path, error);
    if (error)
        throw std::runtime_error("无法创建 TCP Master 测试目录：" +
                                 error.message());
    return path;
}

}  // namespace

/** @brief 验证外部模式缺少显式 framing 时在连接前拒绝。 */
TEST(XcpMasterTcpExample, ExternalModeRequiresExplicitTcpProfileAndAlignment) {
    const fs::path directory = TestDirectory("invalid-external");
    EXPECT_TRUE(
        RunChildToExit(XCP_MASTER_TCP_EXECUTABLE, directory,
                       {"--mode", "external", "--host", "127.0.0.1", "--port",
                        "5555", "--a2l", (directory / "missing.a2l").string(),
                        "--measurement", "ECU::Measurement"},
                       std::chrono::seconds(5), 2));
}

/** @brief 验证自动模式启动本地 Slave 并从 A2L 读取测量值。 */
TEST(XcpMasterTcpExample, AutomaticModeStartsLocalSlaveAndReadsA2lMeasurement) {
    const std::uint16_t port = ReserveFreePort();
    ASSERT_NE(port, 0);
    const fs::path directory = TestDirectory("automatic");
    ASSERT_TRUE(
        RunChildToExit(XCP_MASTER_TCP_EXECUTABLE, directory,
                       {"--mode", "auto", "--run-dir", directory.string(),
                        "--port", std::to_string(port)},
                       std::chrono::seconds(40)));
}

/** @brief 验证外部模式标定写入后，即使 Master 退出目标值仍然保留。 */
TEST(XcpMasterTcpExample, ExternalModeWritesAndKeepsCalibrationValue) {
    const std::uint16_t port = ReserveFreePort();
    ASSERT_NE(port, 0);
    const fs::path directory = TestDirectory("external");
    ASSERT_TRUE(GenerateA2l(directory, port));
    const fs::path a2l_path = directory / "xcp_slave_demo.a2l";
    ASSERT_TRUE(fs::is_regular_file(a2l_path));

    xcp_example::DemoProcess slave;
    std::string error;
    ASSERT_TRUE(slave.Start(XCP_SLAVE_DEMO_EXECUTABLE, directory,
                            {"--transport", "tcp", "--bind", "127.0.0.1",
                             "--port", std::to_string(port)},
                            &error))
        << error;
    ASSERT_TRUE(WaitForSlave(port));

    constexpr double kTargetValue = 2.375;
    ASSERT_TRUE(RunChildToExit(
        XCP_MASTER_TCP_EXECUTABLE, directory,
        {"--mode", "external", "--host", "127.0.0.1", "--port",
         std::to_string(port), "--a2l", a2l_path.string(), "--tcp-profile",
         "asam-v1.4", "--alignment", "8", "--measurement",
         "xcp_slave_demo::meas_fast.input", "--write-calibration",
         "--calibration-symbol", "xcp_slave_demo::cal.basic.real_32",
         "--calibration-value", std::to_string(kTargetValue)},
        std::chrono::seconds(20)));

    auto loaded = a2l::A2lBridge::Load(a2l_path.string());
    ASSERT_TRUE(loaded.HasValue());
    ASSERT_NE(loaded.Value()->Database(), nullptr);
    auto master = MakeTcpMaster(port);
    ASSERT_NO_THROW(master->Connect());
    EXPECT_NEAR(ReadCalibrationValue(*master, *loaded.Value()->Database(),
                                     "xcp_slave_demo::cal.basic.real_32"),
                kTargetValue, 1.0e-6);
    EXPECT_NO_THROW(master->Disconnect());
}
