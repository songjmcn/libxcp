/**
 * @file xcp_slave_demo_integration_test.cpp
 * @brief 独立 XCPlite Slave 的跨进程 A2L、UDP、标定与 DAQ 集成测试。
 */
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "a2l/a2l_measurement_database.hpp"
#include "a2l/measurement_runtime_profile.hpp"
#include "libxcp/a2l/a2l_bridge.hpp"
#include "libxcp/command_executor.hpp"
#include "libxcp/daq/dto_envelope_decoder.hpp"
#include "libxcp/log.hpp"
#include "libxcp/measurement/measurement_planner.hpp"
#include "libxcp/measurement/measurement_session.hpp"
#include "libxcp/protocol_types.hpp"
#include "libxcp/tcp_transport.hpp"
#include "libxcp/tcp_transport_config.hpp"
#include "libxcp/udp_transport.hpp"
#include "libxcp/xcp_error.hpp"
#include "libxcp/xcp_master.hpp"
#include "slave_model.hpp"
#include "xcplite_slave_fixture.hpp"
#include "../tools/xcp_master/symbol_policy.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#endif

#ifndef XCP_SLAVE_DEMO_EXECUTABLE
#error "CMake must define XCP_SLAVE_DEMO_EXECUTABLE for integration tests"
#endif

namespace calmcar::xcp {
namespace {

namespace a2l = calmcar::xcp::a2l;
namespace demo_test = calmcar::xcp::test;
using xcp_slave_demo::BasicCalibrationValues;
using xcp_slave_demo::CalibrationModel;
using xcp_slave_demo::CalibrationParameters;
using xcp_slave_demo::FastMeasurements;
using xcp_slave_demo::SlowMeasurements;
using xcp_slave_demo::SlowMeasurementStatistics;

#ifdef _WIN32
/// @brief 当前平台的系统 socket 句柄类型。
using NativeSocket = SOCKET;
/// @brief 当前平台的无效 socket 句柄。
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
/// @brief 当前平台的系统 socket 句柄类型。
using NativeSocket = int;
/// @brief 当前平台的无效 socket 句柄。
constexpr NativeSocket kInvalidSocket = -1;
#endif

/// @brief 生成测试临时目录时用于避免同进程用例重名的序号。
std::atomic_uint32_t g_temp_directory_sequence{0};

/**
 * @brief 将 UTF-8 命令行参数转换为 Windows UTF-16 字符串。
 * @param text UTF-8 字符串。
 * @return 转换后的 UTF-16 字符串；转换失败时返回空串。
 */
#ifdef _WIN32
std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int required =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring output(static_cast<std::size_t>(required), L'\0');
    (void)MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                              static_cast<int>(text.size()), output.data(),
                              required);
    return output;
}
#endif

/**
 * @brief 将 Windows 命令行参数按 CRT 规则加引号并转义反斜线。
 * @param argument 待转义的 UTF-16 参数。
 * @return 可放入 CreateProcessW 命令行的带引号参数。
 */
#ifdef _WIN32
std::wstring QuoteWindowsArgument(const std::wstring& argument) {
    std::wstring quoted{L"\""};
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'"') {
            quoted.append(backslashes * 2U + 1U, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(character);
            backslashes = 0;
        }
    }
    quoted.append(backslashes * 2U, L'\\');
    quoted.push_back(L'"');
    return quoted;
}
#endif

/**
 * @brief 管理一个独立的 XCPlite 示例进程，支持参数、工作目录和超时回收。
 */
class DemoChildProcess {
public:
    /** @brief 析构时终止并回收仍存活的子进程。 */
    ~DemoChildProcess() { Stop(); }

    DemoChildProcess() = default;
    DemoChildProcess(const DemoChildProcess&) = delete;  ///< 禁止复制进程句柄。
    DemoChildProcess& operator=(const DemoChildProcess&) =
        delete;  ///< 禁止复制赋值。

    /**
     * @brief 使用给定参数及工作目录启动示例可执行文件。
     * @param arguments 不包含 argv[0] 的 UTF-8 参数列表。
     * @param working_directory 子进程工作目录。
     * @param error_message 启动失败时的错误输出位置。
     * @return 子进程启动成功时返回 true。
     */
    bool Start(const std::vector<std::string>& arguments,
               const std::filesystem::path& working_directory,
               std::string* error_message) {
        if (IsRunning()) {
            if (error_message != nullptr) {
                *error_message = "子进程已经运行";
            }
            return false;
        }
#ifdef _WIN32
        const std::filesystem::path executable{
            Utf8ToWide(XCP_SLAVE_DEMO_EXECUTABLE)};
        const std::wstring executable_wide = executable.wstring();
        std::wstring command_line = QuoteWindowsArgument(executable_wide);
        for (const auto& argument : arguments) {
            const std::wstring wide = Utf8ToWide(argument);
            if (wide.empty() && !argument.empty()) {
                if (error_message != nullptr) {
                    *error_message = "无法将子进程参数转换为 UTF-16";
                }
                return false;
            }
            command_line.push_back(L' ');
            command_line.append(QuoteWindowsArgument(wide));
        }
        STARTUPINFOW startup_info{};
        startup_info.cb = sizeof(startup_info);
        PROCESS_INFORMATION process_info{};
        const std::wstring working_directory_wide = working_directory.wstring();
        if (!CreateProcessW(executable_wide.c_str(), command_line.data(),
                            nullptr, nullptr, FALSE, 0, nullptr,
                            working_directory_wide.c_str(), &startup_info,
                            &process_info)) {
            if (error_message != nullptr) {
                *error_message = "CreateProcessW 失败，错误码 " +
                                 std::to_string(GetLastError());
            }
            return false;
        }
        process_handle_ = process_info.hProcess;
        thread_handle_ = process_info.hThread;
#else
        const std::filesystem::path executable{XCP_SLAVE_DEMO_EXECUTABLE};
        std::vector<std::string> child_arguments;
        child_arguments.reserve(arguments.size() + 1U);
        child_arguments.push_back(executable.string());
        child_arguments.insert(child_arguments.end(), arguments.begin(),
                               arguments.end());
        std::vector<char*> argument_pointers;
        argument_pointers.reserve(child_arguments.size() + 1U);
        for (auto& argument : child_arguments) {
            argument_pointers.push_back(argument.data());
        }
        argument_pointers.push_back(nullptr);
        const pid_t child_pid = fork();
        if (child_pid < 0) {
            if (error_message != nullptr) {
                *error_message = "fork 启动子进程失败";
            }
            return false;
        }
        if (child_pid == 0) {
            if (chdir(working_directory.c_str()) != 0) {
                _exit(126);
            }
            execv(executable.c_str(), argument_pointers.data());
            _exit(127);
        }
        process_id_ = child_pid;
#endif
        return true;
    }

    /**
     * @brief 等待子进程在指定时限内退出。
     * @param timeout 最长等待时间。
     * @return 已退出时返回进程退出码，超时时返回空值。
     */
    [[nodiscard]] std::optional<int> WaitForExit(
        std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
#ifdef _WIN32
            if (process_handle_ == nullptr) {
                return std::nullopt;
            }
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
            const DWORD wait_ms = static_cast<DWORD>(std::max<std::int64_t>(
                0, std::min<std::int64_t>(remaining.count(), 20)));
            const DWORD result = WaitForSingleObject(process_handle_, wait_ms);
            if (result == WAIT_OBJECT_0) {
                DWORD exit_code = 1;
                (void)GetExitCodeProcess(process_handle_, &exit_code);
                CloseHandles();
                return static_cast<int>(exit_code);
            }
            if (result == WAIT_FAILED) {
                CloseHandles();
                return std::nullopt;
            }
#else
            if (process_id_ <= 0) {
                return std::nullopt;
            }
            int status = 0;
            const pid_t result = waitpid(process_id_, &status, WNOHANG);
            if (result == process_id_) {
                process_id_ = 0;
                if (WIFEXITED(status)) {
                    return WEXITSTATUS(status);
                }
                if (WIFSIGNALED(status)) {
                    return 128 + WTERMSIG(status);
                }
                return 1;
            }
            if (result < 0) {
                process_id_ = 0;
                return std::nullopt;
            }
#endif
            if (std::chrono::steady_clock::now() >= deadline) {
                return std::nullopt;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (true);
    }

    /** @brief 查询子进程当前是否存活。 */
    [[nodiscard]] bool IsRunning() {
#ifdef _WIN32
        return process_handle_ != nullptr &&
               WaitForSingleObject(process_handle_, 0) == WAIT_TIMEOUT;
#else
        if (process_id_ <= 0) {
            return false;
        }
        int status = 0;
        const pid_t result = waitpid(process_id_, &status, WNOHANG);
        if (result == process_id_ || result < 0) {
            process_id_ = 0;
            return false;
        }
        return result == 0;
#endif
    }

    /** @brief 终止并回收子进程（幂等）。 */
    void Stop() {
#ifdef _WIN32
        if (process_handle_ != nullptr) {
            (void)TerminateProcess(process_handle_, 1U);
            (void)WaitForSingleObject(process_handle_, 2000U);
            CloseHandles();
        }
#else
        if (process_id_ > 0) {
            (void)kill(process_id_, SIGTERM);
            if (!WaitForExit(std::chrono::milliseconds(2000)).has_value()) {
                (void)kill(process_id_, SIGKILL);
                (void)WaitForExit(std::chrono::milliseconds(2000));
            }
        }
#endif
    }

private:
#ifdef _WIN32
    /** @brief 关闭并清空 Windows 进程与线程句柄。 */
    void CloseHandles() {
        if (thread_handle_ != nullptr) {
            CloseHandle(static_cast<HANDLE>(thread_handle_));
            thread_handle_ = nullptr;
        }
        if (process_handle_ != nullptr) {
            CloseHandle(static_cast<HANDLE>(process_handle_));
            process_handle_ = nullptr;
        }
    }

    void* process_handle_{nullptr};  ///< Windows 子进程句柄。
    void* thread_handle_{nullptr};   ///< Windows 主线程句柄。
#else
    pid_t process_id_{0};  ///< POSIX 子进程编号。
#endif
};

/**
 * @brief 在系统临时目录创建并自动清理本用例的专属目录。
 */
class TemporaryDirectory {
public:
    /**
     * @brief 创建带唯一后缀的临时工作目录。
     * @param label 本用例目录名标签。
     */
    explicit TemporaryDirectory(const std::string& label) {
        const auto sequence = g_temp_directory_sequence.fetch_add(1U);
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("libxcp_xcp_slave_demo_" + label + "_" +
                 std::to_string(stamp) + "_" + std::to_string(sequence));
        std::filesystem::create_directories(path_);
    }

    /** @brief 删除本用例创建的专属临时目录。 */
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) =
        delete;  ///< 禁止复制目录守卫。
    TemporaryDirectory& operator=(const TemporaryDirectory&) =
        delete;  ///< 禁止复制赋值。

    /** @brief 返回专属临时目录绝对路径。 */
    [[nodiscard]] const std::filesystem::path& Path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;  ///< 本用例创建且由析构清理的临时目录。
};

/**
 * @brief 占用一个 loopback UDP 端口，用于证明生成模式不会绑定网络端口。
 */
class UdpPortReservation {
public:
    /** @brief 释放 socket 并撤销本对象启动的 Winsock 引用。 */
    ~UdpPortReservation() { Close(); }

    UdpPortReservation() = default;
    UdpPortReservation(const UdpPortReservation&) =
        delete;  ///< 禁止复制 socket 所有权。
    UdpPortReservation& operator=(const UdpPortReservation&) =
        delete;  ///< 禁止复制赋值。

    /**
     * @brief 绑定 loopback 上的任意可用 UDP 端口。
     * @return 端口保留成功时返回 true。
     */
    bool BindFree() {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return false;
        }
        winsock_started_ = true;
#endif
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == kInvalidSocket) {
            Close();
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(socket_, reinterpret_cast<const sockaddr*>(&address),
                 static_cast<int>(sizeof(address))) != 0) {
            Close();
            return false;
        }
#ifdef _WIN32
        int address_length = sizeof(address);
#else
        socklen_t address_length = sizeof(address);
#endif
        if (getsockname(socket_, reinterpret_cast<sockaddr*>(&address),
                        &address_length) != 0) {
            Close();
            return false;
        }
        port_ = ntohs(address.sin_port);
        return port_ != 0U;
    }

    /**
     * @brief 查询当前保留端口是否仍拒绝第二个独占绑定。
     * @return 第二个 socket 无法绑定该端口时返回 true。
     */
    [[nodiscard]] bool IsExclusive() const {
        if (socket_ == kInvalidSocket || port_ == 0U) {
            return false;
        }
        const NativeSocket probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (probe == kInvalidSocket) {
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port_);
        const bool exclusive =
            bind(probe, reinterpret_cast<const sockaddr*>(&address),
                 static_cast<int>(sizeof(address))) != 0;
        CloseSocket(probe);
        return exclusive;
    }

    /** @brief 返回保留的 UDP 端口。 */
    [[nodiscard]] std::uint16_t Port() const noexcept { return port_; }

    /** @brief 关闭端口保留 socket，允许服务进程随后绑定。 */
    void Close() noexcept {
        if (socket_ != kInvalidSocket) {
            CloseSocket(socket_);
            socket_ = kInvalidSocket;
        }
#ifdef _WIN32
        if (winsock_started_) {
            (void)WSACleanup();
            winsock_started_ = false;
        }
#endif
    }

private:
    /** @brief 关闭给定平台 socket。 */
    static void CloseSocket(NativeSocket socket_handle) noexcept {
#ifdef _WIN32
        (void)closesocket(socket_handle);
#else
        (void)close(socket_handle);
#endif
    }

    NativeSocket socket_{kInvalidSocket};  ///< 端口保留 socket 句柄。
    std::uint16_t port_{0};                ///< 已绑定的 loopback UDP 端口。
#ifdef _WIN32
    bool winsock_started_{false};  ///< 本对象是否成功调用 WSAStartup。
#endif
};

/**
 * @brief 生成模式及服务模式子进程的统一启动封装。
 * @param arguments 传给程序的 UTF-8 命令行参数。
 * @param working_directory 子进程工作目录。
 * @param error_message 启动失败的中文原因。
 * @return 子进程启动成功时返回 true。
 */
bool StartDemoProcess(DemoChildProcess& process,
                      const std::vector<std::string>& arguments,
                      const std::filesystem::path& working_directory,
                      std::string* error_message) {
    return process.Start(arguments, working_directory, error_message);
}

/**
 * @brief 等待独立子进程在时限内结束。
 * @param arguments 子进程参数。
 * @param working_directory 子进程工作目录。
 * @param timeout 最长等待时间。
 * @param error_message 启动或超时错误输出。
 * @return 已退出时的退出码；失败或超时时返回空值。
 */
std::optional<int> RunChildToExit(
    const std::vector<std::string>& arguments,
    const std::filesystem::path& working_directory,
    std::chrono::milliseconds timeout, std::string* error_message) {
    DemoChildProcess process;
    if (!StartDemoProcess(process, arguments, working_directory,
                          error_message)) {
        return std::nullopt;
    }
    const auto exit_code = process.WaitForExit(timeout);
    if (!exit_code.has_value()) {
        if (error_message != nullptr) {
            *error_message = "子进程在时限内未退出";
        }
        process.Stop();
    }
    return exit_code;
}

/**
 * @brief 构造绑定给定端口的 UDP Master。
 * @param port 目标 Slave UDP 端口。
 * @param listener 可选的非拥有事件监听器。
 * @return 指向 loopback Slave 的 Master。
 */
std::unique_ptr<XcpMaster> MakeDemoMaster(std::uint16_t port,
                                          IEventListener* listener) {
    UdpTransportConfig config;
    config.remote_host = "127.0.0.1";
    config.remote_port = port;
    config.local_host = "127.0.0.1";
    config.local_port = 0;
    config.receive_poll_interval_ms = 10;
    XcpMasterOptions options;
    options.timeouts = CommandTimeouts{std::chrono::milliseconds(300),
                                       std::chrono::milliseconds(300), 1};
    options.event_listener = listener;
    return std::make_unique<XcpMaster>(std::make_unique<UdpTransport>(config),
                                       options);
}

/**
 * @brief 带有限重试地连接刚启动的示例 Slave。
 * @param port Slave 监听端口。
 * @param listener 可选 DTO 监听器。
 * @return 成功连接的 Master；超时返回空值。
 */
std::unique_ptr<XcpMaster> ConnectToDemo(std::uint16_t port,
                                         IEventListener* listener) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        auto master = MakeDemoMaster(port, listener);
        try {
            master->Connect();
            if (master->IsConnected()) {
                return master;
            }
        } catch (const XcpException&) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return nullptr;
}

/**
 * @brief 占用一个 loopback TCP 端口，用于把已知空闲端口交给从站进程。
 * @note 与 `UdpPortReservation` 同一策略：先绑定 0 端口拿到具体端口号，
 *       生成 A2L 后立即释放，再由从站进程去绑定；TCP 与 UDP 的端口空间
 *       彼此独立，因此两者可以使用同一个端口号。
 */
class TcpPortReservation {
public:
    /** @brief 释放 socket 并撤销本对象启动的 Winsock 引用。 */
    ~TcpPortReservation() { Close(); }

    TcpPortReservation() = default;
    TcpPortReservation(const TcpPortReservation&) =
        delete;  ///< 禁止复制 socket 所有权。
    TcpPortReservation& operator=(const TcpPortReservation&) =
        delete;  ///< 禁止复制赋值。

    /**
     * @brief 绑定 loopback 上的任意可用 TCP 端口。
     * @return 端口保留成功时返回 true。
     */
    bool BindFree() {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return false;
        }
        winsock_started_ = true;
#endif
        socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == kInvalidSocket) {
            Close();
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(socket_, reinterpret_cast<const sockaddr*>(&address),
                 static_cast<int>(sizeof(address))) != 0) {
            Close();
            return false;
        }
#ifdef _WIN32
        int address_length = sizeof(address);
#else
        socklen_t address_length = sizeof(address);
#endif
        if (getsockname(socket_, reinterpret_cast<sockaddr*>(&address),
                        &address_length) != 0) {
            Close();
            return false;
        }
        port_ = ntohs(address.sin_port);
        return port_ != 0U;
    }

    /** @brief 返回保留的 TCP 端口。 */
    [[nodiscard]] std::uint16_t Port() const noexcept { return port_; }

    /** @brief 关闭端口保留 socket，允许服务进程随后绑定。 */
    void Close() noexcept {
        if (socket_ != kInvalidSocket) {
            CloseSocket(socket_);
            socket_ = kInvalidSocket;
        }
#ifdef _WIN32
        if (winsock_started_) {
            WSACleanup();
            winsock_started_ = false;
        }
#endif
    }

private:
    /** @brief 关闭给定平台 socket。 */
    static void CloseSocket(NativeSocket socket_handle) noexcept {
#ifdef _WIN32
        (void)closesocket(socket_handle);
#else
        (void)close(socket_handle);
#endif
    }

    NativeSocket socket_{kInvalidSocket};  ///< 用于保留端口的 socket。
    std::uint16_t port_{0U};               ///< 已保留的端口号。
#ifdef _WIN32
    bool winsock_started_{false};  ///< 是否需要在本对象析构时调用 WSACleanup。
#endif
};

/**
 * @brief 构造指向 loopback TCP Slave 的 Master。
 * @param port 目标 Slave TCP 端口。
 * @param listener 可选的非拥有事件监听器。
 * @return 使用 AsamV14 帧布局与 8 bit 对齐的 Master。
 * @note 帧布局必须显式给出：库**不**从版本号或 A2L 推断线路布局
 *       （见 `tcp_wire_profile.hpp`）；XCPlite 的默认 TCP 路径对应 AsamV14。
 */
std::unique_ptr<XcpMaster> MakeDemoTcpMaster(std::uint16_t port,
                                             IEventListener* listener) {
    TcpTransportConfig config;
    config.role = TcpRole::Master;
    config.remote_host = "127.0.0.1";
    config.remote_port = port;
    config.local_host = "127.0.0.1";
    config.local_port = 0;
    config.packet_alignment = PacketAlignment::Bit8;
    config.wire_profile = TcpWireProfile::AsamV14;
    config.connect_timeout_ms = 1000;
    XcpMasterOptions options;
    options.timeouts = CommandTimeouts{std::chrono::milliseconds(300),
                                       std::chrono::milliseconds(300), 1};
    options.event_listener = listener;
    return std::make_unique<XcpMaster>(std::make_unique<TcpTransport>(config),
                                       options);
}

/**
 * @brief 带有限重试地通过 TCP 连接刚启动的示例 Slave。
 * @param port Slave 监听端口。
 * @param listener 可选 DTO 监听器。
 * @return 成功连接的 Master；超时返回空值。
 */
std::unique_ptr<XcpMaster> ConnectToDemoOverTcp(std::uint16_t port,
                                                IEventListener* listener) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        auto master = MakeDemoTcpMaster(port, listener);
        try {
            master->Connect();
            if (master->IsConnected()) {
                return master;
            }
        } catch (const XcpException&) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return nullptr;
}

/**
 * @brief 从 A2L 中按事件名查找 DAQ 事件通道号。
 * @param info A2L XCP 接口数据。
 * @param event_name 本示例事件全名或短名。
 * @return 匹配到的通道号。
 */
std::optional<std::uint16_t> FindEventChannel(const a2l::IfDataXcpInfo* info,
                                              const std::string& event_name) {
    if (info == nullptr) {
        return std::nullopt;
    }
    for (const auto& event : info->event_channels) {
        if (event.name == event_name || event.short_name == event_name) {
            return event.channel_number;
        }
    }
    return std::nullopt;
}

/**
 * @brief 将 double 浮点数按本机小端字节序编码为 XCP DOWNLOAD 数据。
 * @param value 待编码数值。
 * @return 与同机 XCPlite 标定段布局匹配的原始字节。
 */
Bytes EncodeDouble(double value) {
    Bytes bytes(sizeof(value));
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

/**
 * @brief 将 float 浮点数编码为本机 XCPlite 标定段布局字节。
 * @param value 待编码数值。
 * @return 原始 float 字节。
 */
Bytes EncodeFloat(float value) {
    Bytes bytes(sizeof(value));
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

/**
 * @brief 单线程 DTO 到达回调的帧收集器。
 */
class DemoDtoCollector final : public IEventListener {
public:
    /** @brief 忽略本示例无关的 XCP EVENT 数据包。 */
    void OnEvent(const EventPacket&) override {}

    /** @brief 忽略本示例无关的 XCP SERV 数据包。 */
    void OnService(const ServicePacket&) override {}

    /** @brief 线程安全地保存完整 DTO 帧副本。 */
    void OnDto(const DtoPacket& packet) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            frames_.push_back(packet.data);
        }
        condition_.notify_all();
    }

    /**
     * @brief 等待出现指定的 double DAQ 值。
     * @param expected 期望的浮点测量值。
     * @param timeout 最长等待时间。
     * @return 在时限内收到匹配 DTO 时返回 true。
     */
    [[nodiscard]] bool WaitForDouble(double expected,
                                     std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock<std::mutex> lock(mutex_);
        while (std::chrono::steady_clock::now() < deadline) {
            for (const auto& frame : frames_) {
                try {
                    const auto decoded =
                        decoder_.Decode(BytesView{frame}, layout_);
                    if (decoded.payload.size() >= sizeof(double)) {
                        double observed = 0.0;
                        std::memcpy(&observed, decoded.payload.data(),
                                    sizeof(observed));
                        if (std::abs(observed - expected) < 1e-9) {
                            return true;
                        }
                    }
                } catch (const XcpException&) {
                }
            }
            if (condition_.wait_until(lock, deadline) ==
                std::cv_status::timeout) {
                break;
            }
        }
        return false;
    }

    /**
     * @brief 等待收到恰含 uint32 测量值且数值非零的 DTO。
     * @param timeout 最长等待时间。
     * @return 在时限内收到非零四字节测量值时返回 true。
     */
    [[nodiscard]] bool WaitForNonzeroUint32(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock<std::mutex> lock(mutex_);
        while (std::chrono::steady_clock::now() < deadline) {
            for (const auto& frame : frames_) {
                try {
                    const auto decoded =
                        decoder_.Decode(BytesView{frame}, layout_);
                    if (decoded.payload.size() == sizeof(std::uint32_t)) {
                        std::uint32_t observed = 0U;
                        std::memcpy(&observed, decoded.payload.data(),
                                    sizeof(observed));
                        if (observed > 0U) {
                            return true;
                        }
                    }
                } catch (const XcpException&) {
                }
            }
            if (condition_.wait_until(lock, deadline) ==
                std::cv_status::timeout) {
                break;
            }
        }
        return false;
    }

private:
    mutable std::mutex mutex_;  ///< 保护从 XCP 接收线程写入的 DTO 帧列表。
    std::condition_variable condition_;  ///< 通知等待线程已有新 DTO 到达。
    std::vector<Bytes> frames_;          ///< 已复制保存的原始 DTO 帧。
    DtoEnvelopeDecoder decoder_{
        ByteOrder::Intel};  ///< XCPlite DTO 信封解码器。
    DtoFrameLayout layout_ = [] {
        DtoFrameLayout layout;
        layout.identification_field_type =
            IdentificationFieldType::RelativeByte;
        layout.first_odt = 0;
        layout.counter_enabled = false;
        layout.timestamp_enabled = true;
        layout.timestamp_size_bits = 32U;
        layout.timestamp_relative_first_only = true;
        layout.header_bytes = 4U;
        return layout;
    }();  ///< XCPlite relative-byte DAQ 信封布局。
};

/**
 * @brief 在已有临时目录中启动生成模式，并校验进程退出码。
 * @param directory 子进程工作目录。
 * @param output_dir A2L/AML 输出目录。
 * @param port 与待验证端点对应的端口。
 * @param overwrite 是否显式允许覆盖。
 * @param transport 传输方式（udp 或 tcp），决定 A2L 声明的端点类别。
 * @return 子进程退出码；启动失败或超时返回空值。
 */
std::optional<int> GenerateA2l(const std::filesystem::path& directory,
                               const std::filesystem::path& output_dir,
                               std::uint16_t port, bool overwrite,
                               std::string_view transport = "udp") {
    std::vector<std::string> arguments{
        "--generate-a2l", "--output-dir", output_dir.string(), "--bind",
        "127.0.0.1",      "--port",       std::to_string(port),
        "--transport",    std::string(transport)};
    if (overwrite) {
        arguments.emplace_back("--overwrite");
    }
    std::string error_message;
    const auto exit_code = RunChildToExit(
        arguments, directory, std::chrono::seconds(20), &error_message);
    if (!exit_code.has_value()) {
        ADD_FAILURE() << "A2L 生成进程失败：" << error_message;
    }
    return exit_code;
}

/// @brief 验证生成模式只写文件、可安全拒绝覆盖并支持显式覆盖。
TEST(XcpSlaveDemoIntegrationTest,
     GenerateA2lWithoutBindingPortAndCheckOverwrite) {
    TemporaryDirectory directory("generate");
    const auto output_dir = directory.Path() / "a2l";
    UdpPortReservation reservation;
    ASSERT_TRUE(reservation.BindFree());
    ASSERT_TRUE(reservation.IsExclusive());

    const auto first_exit =
        GenerateA2l(directory.Path(), output_dir, reservation.Port(), false);
    ASSERT_TRUE(first_exit.has_value());
    ASSERT_EQ(*first_exit, 0);
    EXPECT_TRUE(reservation.IsExclusive())
        << "生成 A2L 不得绑定被测试预先占用的 UDP 端口";

    const auto a2l_path = output_dir / "xcp_slave_demo.a2l";
    const auto aml_path = output_dir / "XCP_104.aml";
    ASSERT_TRUE(std::filesystem::is_regular_file(a2l_path));
    ASSERT_TRUE(std::filesystem::file_size(a2l_path) > 0U);
    ASSERT_TRUE(std::filesystem::is_regular_file(aml_path));
    ASSERT_TRUE(std::filesystem::file_size(aml_path) > 0U);

    std::ifstream input(a2l_path, std::ios::binary);
    ASSERT_TRUE(input.is_open());
    const std::string original((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    ASSERT_FALSE(original.empty());
    EXPECT_NE(original.find("FastMeasurementState"), std::string::npos);
    EXPECT_NE(original.find("meas_fast_10ms"), std::string::npos);
    EXPECT_NE(original.find("corrections"), std::string::npos);

    const auto no_overwrite_exit =
        GenerateA2l(directory.Path(), output_dir, reservation.Port(), false);
    ASSERT_TRUE(no_overwrite_exit.has_value());
    EXPECT_EQ(*no_overwrite_exit, 2);
    std::ifstream unchanged_input(a2l_path, std::ios::binary);
    const std::string unchanged(
        (std::istreambuf_iterator<char>(unchanged_input)),
        std::istreambuf_iterator<char>());
    EXPECT_EQ(unchanged, original);

    const auto overwrite_exit =
        GenerateA2l(directory.Path(), output_dir, reservation.Port(), true);
    ASSERT_TRUE(overwrite_exit.has_value());
    EXPECT_EQ(*overwrite_exit, 0);
    EXPECT_TRUE(reservation.IsExclusive());
}

/// @brief 真实生成 A2L 中实例的固定事件必须传递到测量叶子和 Master 准入。
TEST(XcpSlaveDemoIntegrationTest,
     GeneratedA2lPropagatesFixedEventsToMeasurementLeaves) {
    TemporaryDirectory directory("instance_daq_events");
    const auto output_dir = directory.Path() / "a2l";
    UdpPortReservation reservation;
    ASSERT_TRUE(reservation.BindFree());
    const auto generation_exit =
        GenerateA2l(directory.Path(), output_dir, reservation.Port(), false);
    ASSERT_TRUE(generation_exit.has_value());
    ASSERT_EQ(*generation_exit, 0);

    auto loaded =
        a2l::A2lBridge::Load((output_dir / "xcp_slave_demo.a2l").string());
    ASSERT_TRUE(loaded.HasValue()) << loaded.ErrorInfo().message;
    const auto* database = loaded.Value()->Database();
    ASSERT_NE(database, nullptr);
    const auto fast_event =
        FindEventChannel(loaded.Value()->XcpInfo(), "meas_fast_10ms");
    const auto slow_event =
        FindEventChannel(loaded.Value()->XcpInfo(), "meas_slow_100ms");
    ASSERT_TRUE(fast_event.has_value());
    ASSERT_TRUE(slow_event.has_value());
    EXPECT_EQ(*fast_event, 0U);
    EXPECT_EQ(*slow_event, 1U);
    const std::array<std::uint16_t, 2> events{*fast_event, *slow_event};

    // 根成员与嵌套成员继承各自实例事件；不能把合法事件号 0 当作空值。
    for (const auto& [name, expected_event] :
         {std::pair{"meas_fast.input", *fast_event},
          std::pair{"meas_fast.state.raw_output", *fast_event},
          std::pair{"meas_slow.basic.real_32", *slow_event},
          std::pair{"meas_slow.statistics.sample_count", *slow_event}}) {
        auto symbol = database->Find("xcp_slave_demo::" + std::string(name));
        ASSERT_TRUE(symbol.HasValue()) << name;
        EXPECT_EQ(symbol.Value().daq_event_fixed,
                  (std::vector<std::uint16_t>{expected_event}))
            << name;
        EXPECT_TRUE(
            tools::SymbolDaqObservationUnsupportedReason(symbol.Value(), events)
                .empty())
            << name;
    }
    // 重演工具入口：Search 全库 → 按规范名绑定事件 → Adapter → DAQ 规划。
    auto count = database->Count();
    ASSERT_TRUE(count.HasValue());
    auto all_symbols = database->Search("*", count.Value());
    ASSERT_TRUE(all_symbols.HasValue());
    MeasurementEventBindingCandidates bindings;
    for (const auto& symbol : all_symbols.Value()) {
        if (symbol.kind != a2l::SymbolKind::Measurement) continue;
        auto& candidates = bindings[symbol.module_name + "::" + symbol.name];
        candidates.assign(symbol.daq_event_fixed.begin(),
                          symbol.daq_event_fixed.end());
    }
    A2lMeasurementDatabase inner(*database);
    EventBoundMeasurementDatabase bound(inner, std::move(bindings));
    const std::string fast_name = "xcp_slave_demo::meas_fast.input";
    const std::string slow_name = "xcp_slave_demo::meas_slow.basic.real_32";
    auto fast_binding = bound.Find(fast_name);
    auto slow_binding = bound.Find(slow_name);
    ASSERT_TRUE(fast_binding.HasValue());
    ASSERT_TRUE(slow_binding.HasValue());
    EXPECT_EQ(fast_binding.Value().event_channel, 0U);
    EXPECT_EQ(slow_binding.Value().event_channel, 1U);
    MeasurementPlanner planner(bound, /*max_dto=*/256, AddressGranularity::Byte,
                               /*timestamp_bytes=*/4,
                               /*identification_bytes=*/1);
    auto plan = planner.Build({fast_name, slow_name});
    ASSERT_TRUE(plan.HasValue()) << plan.ErrorInfo().message;
    ASSERT_EQ(plan.Value().daq_lists.size(), 2U);
    EXPECT_EQ(plan.Value().daq_lists[0].event_channel, *fast_event);
    EXPECT_EQ(plan.Value().daq_lists[1].event_channel, *slow_event);
    ASSERT_EQ(plan.Value().routes.size(), 2U);
    EXPECT_EQ(plan.Value().routes[0].name, fast_name);
    EXPECT_EQ(plan.Value().routes[1].name, slow_name);

    // INSTANCE 中的数组与标定叶子不因固定事件继承而变成 DAQ 标量。
    auto array = database->Find("xcp_slave_demo::meas_fast.samples");
    ASSERT_TRUE(array.HasValue());
    EXPECT_FALSE(
        tools::SymbolDaqObservationUnsupportedReason(array.Value(), events)
            .empty());
    auto cal = database->Find("xcp_slave_demo::cal.basic.real_32");
    ASSERT_TRUE(cal.HasValue());
    EXPECT_TRUE(cal.Value().daq_event_fixed.empty());
    EXPECT_FALSE(
        tools::SymbolDaqObservationUnsupportedReason(cal.Value(), events)
            .empty());
    // 「不能周期采集」与「可以被标定」是两件独立的事：独立标定段的叶子必须
    // 拿到 A2L READ_WRITE 权限。XCPlite 在段相对寻址下不输出该关键字，示例
    // 程序会在生成后局部补齐；此处从桥接层复核补齐结果确实生效。
    EXPECT_TRUE(cal.Value().read_write) << "标定叶子缺少 A2L READ_WRITE 权限";
    EXPECT_TRUE(tools::SymbolWriteUnsupportedReason(cal.Value()).empty())
        << "标定叶子未通过写入策略："
        << tools::SymbolWriteUnsupportedReason(cal.Value());

    // 将真实快速实例改为结构数组：逐元素嵌套的叶子仍归同一 INSTANCE。
    std::ifstream input(output_dir / "xcp_slave_demo.a2l", std::ios::binary);
    ASSERT_TRUE(input.is_open());
    std::string array_source((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
    const auto instance_begin = array_source.find(
        "INSTANCE meas_fast \"10 ms 快速测量组\" FastMeasurements 0x0 "
        "ECU_ADDRESS_EXTENSION 3 READ_WRITE");
    ASSERT_NE(instance_begin, std::string::npos);
    const std::string instance_prefix =
        "INSTANCE meas_fast \"10 ms 快速测量组\" FastMeasurements 0x0 "
        "ECU_ADDRESS_EXTENSION 3";
    array_source.insert(instance_begin + instance_prefix.size(),
                        " MATRIX_DIM 2");
    const auto array_path = output_dir / "structure_array.a2l";
    {
        std::ofstream output(array_path, std::ios::binary);
        ASSERT_TRUE(output.is_open());
        output << array_source;
        ASSERT_TRUE(output.good());
    }
    auto array_bridge = a2l::A2lBridge::Load(array_path.string());
    ASSERT_TRUE(array_bridge.HasValue()) << array_bridge.ErrorInfo().message;
    for (const char* name : {"xcp_slave_demo::meas_fast[0].state.raw_output",
                             "xcp_slave_demo::meas_fast[1].state.raw_output"}) {
        auto leaf = array_bridge.Value()->Database()->Find(name);
        ASSERT_TRUE(leaf.HasValue()) << name;
        EXPECT_EQ(leaf.Value().daq_event_fixed,
                  (std::vector<std::uint16_t>{*fast_event}))
            << name;
        EXPECT_TRUE(
            tools::SymbolDaqObservationUnsupportedReason(leaf.Value(), events)
                .empty())
            << name;
    }
}

/// @brief 真实 A2L 的实例多固定事件和仅有默认事件均不允许 DAQ 加入。
TEST(XcpSlaveDemoIntegrationTest,
     GeneratedA2lRejectsAmbiguousOrMissingFixedEvents) {
    TemporaryDirectory directory("instance_daq_rejections");
    const auto output_dir = directory.Path() / "a2l";
    UdpPortReservation reservation;
    ASSERT_TRUE(reservation.BindFree());
    const auto generation_exit =
        GenerateA2l(directory.Path(), output_dir, reservation.Port(), false);
    ASSERT_TRUE(generation_exit.has_value());
    ASSERT_EQ(*generation_exit, 0);
    std::ifstream input(output_dir / "xcp_slave_demo.a2l", std::ios::binary);
    ASSERT_TRUE(input.is_open());
    const std::string original((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    const std::array<std::uint16_t, 2> events{0, 1};

    // INSTANCE 声明两个不同事件，绝不能任选一个给测量叶子采样。
    std::string conflicting = original;
    const auto multi_begin = conflicting.find("FIXED_EVENT_LIST EVENT 0x0");
    ASSERT_NE(multi_begin, std::string::npos);
    conflicting.replace(multi_begin,
                        std::string("FIXED_EVENT_LIST EVENT 0x0").size(),
                        "FIXED_EVENT_LIST EVENT 0x0 EVENT 0x1");
    const auto conflict_path = output_dir / "conflicting_event.a2l";
    {
        std::ofstream output(conflict_path, std::ios::binary);
        ASSERT_TRUE(output.is_open());
        output << conflicting;
        ASSERT_TRUE(output.good());
    }
    auto conflict_bridge = a2l::A2lBridge::Load(conflict_path.string());
    ASSERT_TRUE(conflict_bridge.HasValue())
        << conflict_bridge.ErrorInfo().message;
    auto conflict = conflict_bridge.Value()->Database()->Find(
        "xcp_slave_demo::meas_fast.input");
    ASSERT_TRUE(conflict.HasValue());
    EXPECT_EQ(conflict.Value().daq_event_fixed,
              (std::vector<std::uint16_t>{0, 1}));
    EXPECT_FALSE(
        tools::SymbolDaqObservationUnsupportedReason(conflict.Value(), events)
            .empty());

    // 同号重复不造成伪冲突：策略按不同事件号计数。
    std::string repeated = original;
    const auto repeated_begin = repeated.find("FIXED_EVENT_LIST EVENT 0x0");
    ASSERT_NE(repeated_begin, std::string::npos);
    repeated.replace(repeated_begin,
                     std::string("FIXED_EVENT_LIST EVENT 0x0").size(),
                     "FIXED_EVENT_LIST EVENT 0x0 EVENT 0x0");
    const auto repeated_path = output_dir / "repeated_event.a2l";
    {
        std::ofstream output(repeated_path, std::ios::binary);
        ASSERT_TRUE(output.is_open());
        output << repeated;
        ASSERT_TRUE(output.good());
    }
    auto repeated_bridge = a2l::A2lBridge::Load(repeated_path.string());
    ASSERT_TRUE(repeated_bridge.HasValue())
        << repeated_bridge.ErrorInfo().message;
    auto duplicate = repeated_bridge.Value()->Database()->Find(
        "xcp_slave_demo::meas_fast.input");
    ASSERT_TRUE(duplicate.HasValue());
    EXPECT_EQ(duplicate.Value().daq_event_fixed,
              (std::vector<std::uint16_t>{0, 0}));
    EXPECT_TRUE(
        tools::SymbolDaqObservationUnsupportedReason(duplicate.Value(), events)
            .empty());

    // DEFAULT_EVENT_LIST 即使包含事件 0，也不能补足缺失的固定事件。
    std::string default_only = original;
    const auto fixed_begin = default_only.find("FIXED_EVENT_LIST EVENT 0x0");
    ASSERT_NE(fixed_begin, std::string::npos);
    default_only.replace(fixed_begin, std::string("FIXED_EVENT_LIST").size(),
                         "DEFAULT_EVENT_LIST");
    const auto default_path = output_dir / "default_only.a2l";
    {
        std::ofstream output(default_path, std::ios::binary);
        ASSERT_TRUE(output.is_open());
        output << default_only;
        ASSERT_TRUE(output.good());
    }
    auto default_bridge = a2l::A2lBridge::Load(default_path.string());
    ASSERT_TRUE(default_bridge.HasValue())
        << default_bridge.ErrorInfo().message;
    auto missing = default_bridge.Value()->Database()->Find(
        "xcp_slave_demo::meas_fast.input");
    ASSERT_TRUE(missing.HasValue());
    EXPECT_TRUE(missing.Value().daq_event_fixed.empty());
    EXPECT_FALSE(
        tools::SymbolDaqObservationUnsupportedReason(missing.Value(), events)
            .empty());
    // 修改快速实例的声明不能污染另一慢速 INSTANCE。
    auto slow = default_bridge.Value()->Database()->Find(
        "xcp_slave_demo::meas_slow.basic.real_32");
    ASSERT_TRUE(slow.HasValue());
    EXPECT_EQ(slow.Value().daq_event_fixed, (std::vector<std::uint16_t>{1}));
}

/// @brief 验证独立服务进程使用同布局相对地址、标定写入及 10 ms DAQ。
TEST(XcpSlaveDemoIntegrationTest, CrossProcessCalibrationChangesDaqOutput) {
    TemporaryDirectory directory("cross_process");
    const auto output_dir = directory.Path() / "a2l";
    UdpPortReservation reservation;
    ASSERT_TRUE(reservation.BindFree());
    const std::uint16_t port = reservation.Port();

    const auto generation_exit =
        GenerateA2l(directory.Path(), output_dir, port, false);
    ASSERT_TRUE(generation_exit.has_value());
    ASSERT_EQ(*generation_exit, 0);
    reservation.Close();
    const auto service_directory = directory.Path() / "service";
    std::filesystem::create_directories(service_directory);

    const auto a2l_path = output_dir / "xcp_slave_demo.a2l";
    auto loaded = a2l::A2lBridge::Load(a2l_path.string());
    ASSERT_TRUE(loaded.HasValue()) << loaded.ErrorInfo().message;
    ASSERT_NE(loaded.Value()->Database(), nullptr);
    const auto fast_event =
        FindEventChannel(loaded.Value()->XcpInfo(), "meas_fast_10ms");
    const auto slow_event =
        FindEventChannel(loaded.Value()->XcpInfo(), "meas_slow_100ms");
    ASSERT_TRUE(fast_event.has_value()) << "A2L 缺少快速 DAQ 事件通道";
    ASSERT_TRUE(slow_event.has_value()) << "A2L 缺少慢速 DAQ 事件通道";
    const std::array<std::uint16_t, 2> events{*fast_event, *slow_event};
    auto fast_leaf =
        loaded.Value()->Database()->Find("xcp_slave_demo::meas_fast.output");
    auto slow_leaf = loaded.Value()->Database()->Find(
        "xcp_slave_demo::meas_slow.statistics.fast_cycle_count");
    ASSERT_TRUE(fast_leaf.HasValue());
    ASSERT_TRUE(slow_leaf.HasValue());
    // 与 Master 一样从 A2L 展开叶子获取地址、扩展和固定事件，不再手填 event。
    for (const auto* leaf : {&fast_leaf.Value(), &slow_leaf.Value()}) {
        EXPECT_TRUE(
            tools::SymbolDaqObservationUnsupportedReason(*leaf, events).empty())
            << leaf->name;
        ASSERT_EQ(leaf->daq_event_fixed.size(), 1U);
    }
    EXPECT_EQ(fast_leaf.Value().daq_event_fixed.front(), *fast_event);
    EXPECT_EQ(slow_leaf.Value().daq_event_fixed.front(), *slow_event);

    const auto cal_symbol = demo_test::ResolveA2lSymbol(a2l_path, "cal");
    ASSERT_TRUE(cal_symbol.has_value()) << "A2L 缺少 cal 标定段实例";
    ASSERT_EQ(cal_symbol->extension, 0U) << "CalSeg 应使用段相对地址扩展";

    DemoChildProcess slave_process;
    std::string start_error;
    ASSERT_TRUE(StartDemoProcess(
        slave_process, {"--bind", "127.0.0.1", "--port", std::to_string(port)},
        service_directory, &start_error))
        << start_error;
    DemoDtoCollector collector;
    auto master = ConnectToDemo(port, &collector);
    ASSERT_NE(master, nullptr) << "服务进程未在 8 秒内完成真实 XCP CONNECT";
    EXPECT_FALSE(std::filesystem::exists(service_directory / "a2l"));
    EXPECT_FALSE(
        std::filesystem::exists(service_directory / "xcp_slave_demo.a2l"));

    constexpr double kCalibratedOffset = 42.0;
    const std::uint64_t offset_in_segment =
        offsetof(CalibrationParameters, model) +
        offsetof(CalibrationModel, offset);
    const Address offset_address =
        static_cast<Address>(cal_symbol->address + offset_in_segment);
    master->WriteMemoryBytes(offset_address, cal_symbol->extension,
                             EncodeDouble(kCalibratedOffset));
    const Bytes offset_readback = master->ReadMemoryBytes(
        offset_address, cal_symbol->extension, sizeof(kCalibratedOffset));
    ASSERT_EQ(offset_readback.size(), sizeof(kCalibratedOffset));
    double observed_offset = 0.0;
    std::memcpy(&observed_offset, offset_readback.data(),
                sizeof(observed_offset));
    EXPECT_DOUBLE_EQ(observed_offset, kCalibratedOffset);

    const std::uint64_t basic_signed_8_offset =
        offsetof(CalibrationParameters, basic) +
        offsetof(BasicCalibrationValues, signed_8);
    const Address basic_signed_8_address =
        static_cast<Address>(cal_symbol->address + basic_signed_8_offset);
    const Bytes signed_8_value{
        static_cast<std::uint8_t>(static_cast<std::int8_t>(-11))};
    master->WriteMemoryBytes(basic_signed_8_address, cal_symbol->extension,
                             signed_8_value);
    const Bytes signed_8_readback = master->ReadMemoryBytes(
        basic_signed_8_address, cal_symbol->extension, signed_8_value.size());
    ASSERT_EQ(signed_8_readback, signed_8_value);

    const std::uint64_t correction_7_offset =
        offsetof(CalibrationParameters, corrections) + 7U * sizeof(float);
    const Address correction_7_address =
        static_cast<Address>(cal_symbol->address + correction_7_offset);
    constexpr float kArrayCorrection = -1.25F;
    master->WriteMemoryBytes(correction_7_address, cal_symbol->extension,
                             EncodeFloat(kArrayCorrection));
    const Bytes correction_readback = master->ReadMemoryBytes(
        correction_7_address, cal_symbol->extension, sizeof(kArrayCorrection));
    ASSERT_EQ(correction_readback.size(), sizeof(kArrayCorrection));
    float observed_correction = 0.0F;
    std::memcpy(&observed_correction, correction_readback.data(),
                sizeof(observed_correction));
    EXPECT_FLOAT_EQ(observed_correction, kArrayCorrection);

    DaqEntrySpec output_entry;
    output_entry.address = static_cast<Address>(fast_leaf.Value().xcp_address);
    output_entry.extension = fast_leaf.Value().address_extension;
    output_entry.size = fast_leaf.Value().element_size_bytes;
    DaqOdtSpec odt;
    odt.entries.push_back(output_entry);
    DaqListSpec daq_list;
    daq_list.daq_list = 0U;
    daq_list.event_channel = fast_leaf.Value().daq_event_fixed.front();
    daq_list.timestamp = true;
    daq_list.odts.push_back(odt);
    ASSERT_NO_THROW(master->ConfigureDaqListsDynamic({daq_list}));
    ASSERT_NO_THROW(master->StartDaqSync());
    EXPECT_TRUE(
        collector.WaitForDouble(kCalibratedOffset, std::chrono::seconds(3)))
        << "10 ms DAQ DTO 未观察到标定偏置 42.0；可能是相对基址或 CalSeg "
           "快照未生效";
    ASSERT_NO_THROW(master->StopDaq());

    DaqEntrySpec slow_entry;
    slow_entry.address = static_cast<Address>(slow_leaf.Value().xcp_address);
    slow_entry.extension = slow_leaf.Value().address_extension;
    slow_entry.size = slow_leaf.Value().element_size_bytes;
    DaqOdtSpec slow_odt;
    slow_odt.entries.push_back(slow_entry);
    DaqListSpec slow_daq_list;
    slow_daq_list.daq_list = 0U;
    slow_daq_list.event_channel = slow_leaf.Value().daq_event_fixed.front();
    slow_daq_list.timestamp = true;
    slow_daq_list.odts.push_back(slow_odt);
    ASSERT_NO_THROW(master->ConfigureDaqListsDynamic({slow_daq_list}));
    ASSERT_NO_THROW(master->StartDaqSync());
    EXPECT_TRUE(collector.WaitForNonzeroUint32(std::chrono::seconds(3)))
        << "100 ms DAQ DTO 未到达慢速测量对象";
    ASSERT_NO_THROW(master->StopDaq());
    master->Disconnect();
    slave_process.Stop();
}

/// @brief 真实 Slave DTO 经固定事件绑定、规划及会话解码后产生快慢测量样本。
TEST(XcpSlaveDemoIntegrationTest, GeneratedA2lFeedsLiveMeasurementSession) {
    TemporaryDirectory directory("live_measurement_session");
    const auto output_dir = directory.Path() / "a2l";
    UdpPortReservation reservation;
    ASSERT_TRUE(reservation.BindFree());
    const std::uint16_t port = reservation.Port();
    const auto generation_exit =
        GenerateA2l(directory.Path(), output_dir, port, false);
    ASSERT_TRUE(generation_exit.has_value());
    ASSERT_EQ(*generation_exit, 0);
    reservation.Close();

    auto loaded =
        a2l::A2lBridge::Load((output_dir / "xcp_slave_demo.a2l").string());
    ASSERT_TRUE(loaded.HasValue()) << loaded.ErrorInfo().message;
    auto* database = loaded.Value()->Database();
    ASSERT_NE(database, nullptr);
    auto count = database->Count();
    ASSERT_TRUE(count.HasValue());
    auto symbols = database->Search("*", count.Value());
    ASSERT_TRUE(symbols.HasValue());
    MeasurementEventBindingCandidates bindings;
    for (const auto& symbol : symbols.Value()) {
        if (symbol.kind != a2l::SymbolKind::Measurement) continue;
        auto& candidates = bindings[symbol.module_name + "::" + symbol.name];
        candidates.assign(symbol.daq_event_fixed.begin(),
                          symbol.daq_event_fixed.end());
    }
    A2lMeasurementDatabase adapter(*database);
    EventBoundMeasurementDatabase bound(adapter, std::move(bindings));
    std::vector<A2lEventMetadata> event_metadata;
    const auto* xcp_info = loaded.Value()->XcpInfo();
    ASSERT_NE(xcp_info, nullptr);
    for (const auto& event : xcp_info->event_channels) {
        event_metadata.push_back(A2lEventMetadata{
            event.name, event.short_name, event.channel_number,
            event.time_cycle, event.time_unit, event.type, event.max_daq_list});
    }

    const auto service_directory = directory.Path() / "service";
    std::filesystem::create_directories(service_directory);
    DemoChildProcess slave_process;
    std::string start_error;
    ASSERT_TRUE(StartDemoProcess(
        slave_process, {"--bind", "127.0.0.1", "--port", std::to_string(port)},
        service_directory, &start_error))
        << start_error;
    // master 先声明，确保 session 析构前监听器所依赖的 master 仍存活。
    std::unique_ptr<XcpMaster> master;
    MeasurementSession session(bound);
    master = ConnectToDemo(port, &session);
    ASSERT_NE(master, nullptr);
    session.Bind(*master);
    auto profile =
        BuildMeasurementRuntimeProfile(*master, event_metadata);
    ASSERT_TRUE(profile.HasValue()) << profile.ErrorInfo().message;
    ApplyMeasurementRuntimeProfile(session, profile.Value());
    const std::string fast_name = "xcp_slave_demo::meas_fast.output";
    const std::string slow_name =
        "xcp_slave_demo::meas_slow.statistics.fast_cycle_count";
    session.Add(fast_name);
    session.Add(slow_name);
    ASSERT_NO_THROW(session.Prepare());

    std::mutex samples_mutex;
    std::condition_variable samples_ready;
    std::set<std::string> observed;
    ASSERT_NO_THROW(session.Start([&](const MeasurementFrame& frame) {
        std::lock_guard<std::mutex> lock(samples_mutex);
        for (const auto& sample : frame.samples) {
            if (sample.valid && !sample.raw.empty()) {
                observed.insert(sample.name);
            }
        }
        samples_ready.notify_all();
    }));
    bool complete = false;
    {
        std::unique_lock<std::mutex> lock(samples_mutex);
        complete = samples_ready.wait_for(lock, std::chrono::seconds(4), [&] {
            return observed.contains(fast_name) && observed.contains(slow_name);
        });
    }
    const auto statistics = session.Statistics();
    session.Stop();
    master->Disconnect();
    slave_process.Stop();
    EXPECT_TRUE(complete) << "未同时解码快慢实例叶子，DTO="
                          << statistics.dto_received
                          << " 解码失败=" << statistics.decode_errors;
}

/// @brief 真实跨进程 DAQ 将快慢数组分别作为一条 Entry 送达，并逐元素换算。
TEST(XcpSlaveDemoIntegrationTest, GeneratedA2lFeedsLiveContiguousArrayDaq) {
    TemporaryDirectory directory("live_array_daq");
    const auto output_dir = directory.Path() / "a2l";
    UdpPortReservation reservation;
    ASSERT_TRUE(reservation.BindFree());
    const auto port = reservation.Port();
    const auto generation_exit =
        GenerateA2l(directory.Path(), output_dir, port, false);
    ASSERT_TRUE(generation_exit.has_value());
    ASSERT_EQ(*generation_exit, 0);
    reservation.Close();

    const auto a2l_path = output_dir / "xcp_slave_demo.a2l";
    auto loaded = a2l::A2lBridge::Load(a2l_path.string());
    ASSERT_TRUE(loaded.HasValue()) << loaded.ErrorInfo().message;
    const auto* database = loaded.Value()->Database();
    const auto* xcp_info = loaded.Value()->XcpInfo();
    ASSERT_NE(database, nullptr);
    ASSERT_NE(xcp_info, nullptr);
    const auto fast_event = FindEventChannel(xcp_info, "meas_fast_10ms");
    const auto slow_event = FindEventChannel(xcp_info, "meas_slow_100ms");
    ASSERT_TRUE(fast_event.has_value());
    ASSERT_TRUE(slow_event.has_value());
    const std::array<std::uint16_t, 2> events{*fast_event, *slow_event};
    const std::string fast_array = "xcp_slave_demo::meas_fast.samples";
    const std::string slow_array = "xcp_slave_demo::meas_slow.history";
    const std::string fast_scalar = "xcp_slave_demo::meas_fast.input";
    const std::string slow_scalar =
        "xcp_slave_demo::meas_slow.statistics.fast_cycle_count";
    for (const auto& [name, event] : {std::pair{fast_array, *fast_event},
                                      std::pair{slow_array, *slow_event}}) {
        auto symbol = database->Find(name);
        ASSERT_TRUE(symbol.HasValue()) << name;
        ASSERT_EQ(symbol.Value().daq_event_fixed,
                  (std::vector<std::uint16_t>{event}));
        ASSERT_EQ(symbol.Value().dimensions.size(), 1U);
        EXPECT_EQ(symbol.Value().dimensions.front().extent,
                  xcp_slave_demo::kSampleCount);
        EXPECT_EQ(symbol.Value().dimensions.front().byte_stride,
                  symbol.Value().element_size_bytes);
        EXPECT_TRUE(tools::SymbolDaqObservationUnsupportedReason(symbol.Value(),
                                                                 events, true)
                        .empty())
            << name;
        EXPECT_FALSE(
            tools::SymbolDaqObservationUnsupportedReason(symbol.Value(), events)
                .empty())
            << "未开启一维连续数组准入时必须拒绝数组";
    }
    auto count = database->Count();
    ASSERT_TRUE(count.HasValue());
    auto symbols = database->Search("*", count.Value());
    ASSERT_TRUE(symbols.HasValue());
    MeasurementEventBindingCandidates bindings;
    for (const auto& symbol : symbols.Value()) {
        if (symbol.kind != a2l::SymbolKind::Measurement) continue;
        auto& candidates = bindings[symbol.module_name + "::" + symbol.name];
        candidates.assign(symbol.daq_event_fixed.begin(),
                          symbol.daq_event_fixed.end());
    }
    A2lMeasurementDatabase adapter(*database);
    EventBoundMeasurementDatabase bound(adapter, std::move(bindings));
    for (const auto& [name, event] : {std::pair{fast_array, *fast_event},
                                      std::pair{slow_array, *slow_event}}) {
        auto found = bound.Find(name);
        ASSERT_TRUE(found.HasValue()) << found.ErrorInfo().message;
        EXPECT_EQ(found.Value().event_channel, event);
        EXPECT_TRUE(found.Value().is_array);
        EXPECT_EQ(found.Value().element_count, xcp_slave_demo::kSampleCount);
        EXPECT_EQ(found.Value().element_size_bytes, sizeof(std::uint32_t));
    }
    std::vector<A2lEventMetadata> event_metadata;
    for (const auto& event : xcp_info->event_channels) {
        event_metadata.push_back(A2lEventMetadata{
            event.name, event.short_name, event.channel_number,
            event.time_cycle, event.time_unit, event.type, event.max_daq_list});
    }

    const auto service_directory = directory.Path() / "service";
    std::filesystem::create_directories(service_directory);
    DemoChildProcess slave_process;
    std::string start_error;
    ASSERT_TRUE(StartDemoProcess(
        slave_process, {"--bind", "127.0.0.1", "--port", std::to_string(port)},
        service_directory, &start_error))
        << start_error;
    // 会话先于 Master 构造，以确保 DTO 监听器在连接生命周期内有效。
    std::unique_ptr<XcpMaster> master;
    MeasurementSession session(bound);
    master = ConnectToDemo(port, &session);
    ASSERT_NE(master, nullptr);
    session.Bind(*master);
    auto profile =
        BuildMeasurementRuntimeProfile(*master, event_metadata);
    ASSERT_TRUE(profile.HasValue()) << profile.ErrorInfo().message;
    ASSERT_TRUE(
        profile.Value().daq_entry_capabilities.supports_contiguous_1d_arrays);
    ASSERT_GE(profile.Value().daq_entry_capabilities.max_entry_size_elements,
              sizeof(std::uint32_t) * xcp_slave_demo::kSampleCount);
    ApplyMeasurementRuntimeProfile(session, profile.Value());
    // 使用协商的 MAX_DTO/AG 与运行时 Entry 能力证明每个数组恰占一条 Entry。
    const auto negotiated = master->GetSessionParameters();
    MeasurementPlanner planner(bound, negotiated.connect.max_dto,
                               negotiated.connect.address_granularity,
                               /*timestamp_bytes=*/4,
                               profile.Value().identification_header_bytes,
                               profile.Value().daq_entry_capabilities);
    auto plan =
        planner.Build({fast_array, fast_scalar, slow_array, slow_scalar});
    ASSERT_TRUE(plan.HasValue()) << plan.ErrorInfo().message;
    ASSERT_EQ(plan.Value().daq_lists.size(), 2U);
    for (std::size_t index = 0; index < 2U; ++index) {
        const auto& list = plan.Value().daq_lists[index];
        EXPECT_EQ(list.event_channel, index == 0U ? *fast_event : *slow_event);
        ASSERT_EQ(list.odts.size(), 1U);
        ASSERT_EQ(list.odts.front().entries.size(), 2U);
        EXPECT_EQ(list.odts.front().entries.front().size,
                  sizeof(std::uint32_t) * xcp_slave_demo::kSampleCount);
    }
    ASSERT_EQ(plan.Value().routes.size(), 4U);
    EXPECT_TRUE(plan.Value().routes[0].is_array);
    EXPECT_EQ(plan.Value().routes[0].size,
              sizeof(std::uint32_t) * xcp_slave_demo::kSampleCount);
    EXPECT_TRUE(plan.Value().routes[2].is_array);
    EXPECT_EQ(plan.Value().routes[2].size,
              sizeof(std::uint32_t) * xcp_slave_demo::kSampleCount);

    // 标定最后一个通道：数组逐元素校验不能只检查长度或首元素。
    const auto cal = demo_test::ResolveA2lSymbol(a2l_path, "cal");
    ASSERT_TRUE(cal.has_value());
    constexpr float kLastCorrection = -1.25F;
    const Address correction_address = static_cast<Address>(
        cal->address + offsetof(CalibrationParameters, corrections) +
        (xcp_slave_demo::kSampleCount - 1U) * sizeof(float));
    master->WriteMemoryBytes(correction_address, cal->extension,
                             EncodeFloat(kLastCorrection));
    const auto correction_readback = master->ReadMemoryBytes(
        correction_address, cal->extension, sizeof(float));
    ASSERT_EQ(correction_readback, EncodeFloat(kLastCorrection));

    session.Add(fast_array);
    session.Add(fast_scalar);
    session.Add(slow_array);
    session.Add(slow_scalar);
    ASSERT_NO_THROW(session.Prepare());
    std::mutex samples_mutex;
    std::condition_variable samples_ready;
    bool fast_complete = false;
    bool slow_complete = false;
    std::size_t complete_arrays = 0U;
    ASSERT_NO_THROW(session.Start([&](const MeasurementFrame& frame) {
        const MeasurementSample* array = nullptr;
        const MeasurementSample* scalar = nullptr;
        const bool fast = frame.daq_list == 0U;
        for (const auto& sample : frame.samples) {
            if (sample.name == (fast ? fast_array : slow_array))
                array = &sample;
            if (sample.name == (fast ? fast_scalar : slow_scalar))
                scalar = &sample;
        }
        if (array == nullptr || scalar == nullptr || !array->valid ||
            !scalar->valid || !array->is_array || scalar->is_array ||
            array->elements.size() != xcp_slave_demo::kSampleCount ||
            array->raw.size() !=
                sizeof(std::uint32_t) * xcp_slave_demo::kSampleCount) {
            return;
        }
        bool consistent = true;
        if (fast) {
            const auto* input = std::get_if<double>(&scalar->value);
            if (input == nullptr) return;
            for (std::size_t index = 0; index < xcp_slave_demo::kSampleCount;
                 ++index) {
                const auto* element =
                    std::get_if<double>(&array->elements[index]);
                float raw = 0.0F;
                std::memcpy(&raw, array->raw.data() + index * sizeof(raw),
                            sizeof(raw));
                const float expected =
                    static_cast<float>(*input) +
                    (index + 1U == xcp_slave_demo::kSampleCount
                         ? kLastCorrection
                         : 0.0F);
                consistent =
                    consistent && element != nullptr &&
                    std::abs(raw - expected) < 1e-4F &&
                    std::abs(*element - static_cast<double>(raw)) < 1e-4;
            }
        } else {
            if (scalar->raw.size() != sizeof(std::uint32_t)) return;
            std::uint32_t tick = 0U;
            std::memcpy(&tick, scalar->raw.data(), sizeof(tick));
            if (tick == 0U) return;
            for (std::size_t index = 0; index < xcp_slave_demo::kSampleCount;
                 ++index) {
                std::uint32_t raw = 0U;
                std::memcpy(&raw, array->raw.data() + index * sizeof(raw),
                            sizeof(raw));
                // 数据库可选择 int64/uint64/double 展示物理值，原始逐元素字节
                // 与同一 DTO 内统计标量可直接验证 Slave 的真实环形历史。
                const auto* unsigned_value =
                    std::get_if<std::uint64_t>(&array->elements[index]);
                const auto* signed_value =
                    std::get_if<std::int64_t>(&array->elements[index]);
                const auto* real_value =
                    std::get_if<double>(&array->elements[index]);
                consistent =
                    consistent &&
                    ((unsigned_value != nullptr && *unsigned_value == raw) ||
                     (signed_value != nullptr && *signed_value == raw) ||
                     (real_value != nullptr && *real_value == raw));
                if (index == tick % xcp_slave_demo::kSampleCount) {
                    consistent = consistent && raw == tick;
                }
            }
        }
        if (!consistent) return;
        {
            std::lock_guard<std::mutex> lock(samples_mutex);
            ++complete_arrays;
            if (fast)
                fast_complete = true;
            else
                slow_complete = true;
        }
        samples_ready.notify_all();
    }));
    bool complete = false;
    {
        std::unique_lock<std::mutex> lock(samples_mutex);
        complete = samples_ready.wait_for(lock, std::chrono::seconds(5), [&] {
            return fast_complete && slow_complete;
        });
    }
    const auto statistics = session.Statistics();
    session.Stop();
    master->Disconnect();
    slave_process.Stop();
    EXPECT_TRUE(complete) << "快慢数组未同时收到完整且与标量一致的八元素回调，"
                          << "完整数组帧=" << complete_arrays
                          << " DTO=" << statistics.dto_received
                          << " 解码失败=" << statistics.decode_errors;
}

/// @brief 验证非法 CLI 配置失败退出且不会启动服务。
TEST(XcpSlaveDemoIntegrationTest, InvalidCliArgumentsReturnUsageError) {
    TemporaryDirectory directory("invalid_cli");
    std::string error_message;
    const auto exit_code =
        RunChildToExit({"--port", "0"}, directory.Path(),
                       std::chrono::seconds(5), &error_message);
    ASSERT_TRUE(exit_code.has_value()) << error_message;
    EXPECT_EQ(*exit_code, 2);

    const auto unsafe_output = directory.Path() / "unreachable_endpoint";
    const auto unsafe_endpoint_exit = RunChildToExit(
        {"--generate-a2l", "--output-dir", unsafe_output.string(), "--bind",
         "0.0.0.0"},
        directory.Path(), std::chrono::seconds(5), &error_message);
    ASSERT_TRUE(unsafe_endpoint_exit.has_value()) << error_message;
    EXPECT_EQ(*unsafe_endpoint_exit, 2);
    EXPECT_FALSE(std::filesystem::exists(unsafe_output));
}

/**
 * @brief 汇总指定程序工作目录下从站产生的全部 glog 文件内容。
 * @param service_directory 从站工作目录，日志位于其 logs 子目录下。
 * @return 合并后的日志文本；目录或文件不存在时返回空字符串。
 */
std::string ReadSlaveLogs(const std::filesystem::path& service_directory) {
    const auto log_directory =
        service_directory / "logs" / "examples" / "xcp_slave_demo";
    std::string merged;
    if (!std::filesystem::is_directory(log_directory)) {
        return merged;
    }
    for (const auto& entry :
         std::filesystem::directory_iterator(log_directory)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::ifstream input(entry.path(), std::ios::binary);
        if (!input.is_open()) {
            continue;
        }
        merged.append(std::string((std::istreambuf_iterator<char>(input)),
                                  std::istreambuf_iterator<char>()));
    }
    return merged;
}

/// @brief 常驻服务必须按固定间隔输出降频心跳，且心跳需反映真实协议状态。
/// @note 仅在 LIBXCP_ENABLE_LOG=ON 时日志文件有内容；关闭时本用例退化为
///       校验"不产生日志目录内容"且进程可正常启停。
TEST(XcpSlaveDemoIntegrationTest, ServiceEmitsPeriodicHeartbeatLog) {
    TemporaryDirectory directory("heartbeat");
    const auto service_directory = directory.Path();
    UdpPortReservation reservation;
    ASSERT_TRUE(reservation.BindFree());
    const auto port = reservation.Port();
    // 让从站自行绑定：释放预留端口，避免与子进程争用。
    reservation.Close();

    DemoChildProcess slave_process;
    std::string start_error;
    ASSERT_TRUE(StartDemoProcess(
        slave_process, {"--bind", "127.0.0.1", "--port", std::to_string(port)},
        service_directory, &start_error))
        << start_error;

    // 心跳间隔为 5 秒（500 次 10 ms 快速循环）；等待覆盖至少一个心跳周期。
    // glog 会缓冲写入，日志通常在进程退出（ShutdownGoogleLogging）时才落盘，
    // 因此这里只等待足够时长，随后停止进程再读取文件。
    const std::chrono::seconds kWaitForHeartbeat{9};
    std::this_thread::sleep_for(kWaitForHeartbeat);

    slave_process.Stop();
    const std::string logs = ReadSlaveLogs(service_directory);

#if LIBXCP_ENABLE_LOG
    ASSERT_NE(logs.find("从站心跳"), std::string::npos)
        << "常驻服务未在 " << kWaitForHeartbeat.count()
        << " 秒内产生降频心跳日志；实际日志：" << logs;
    // 心跳必须携带存活证据与真实协议状态字段。
    EXPECT_NE(logs.find("累计快事件次数="), std::string::npos);
    EXPECT_NE(logs.find("已连接="), std::string::npos);
    EXPECT_NE(logs.find("DAQ 运行中="), std::string::npos);
    // 无主站连接时，心跳应如实报告未连接。
    EXPECT_NE(logs.find("已连接=false"), std::string::npos);
#endif
#if !LIBXCP_ENABLE_LOG
    EXPECT_TRUE(logs.empty())
        << "LIBXCP_ENABLE_LOG=OFF 时不应写入日志文件：" << logs;
#endif
}
/// @brief TCP 传输端到端：A2L 声明 TCP 端点、按 TCP 连接、POLL 与 DAQ 均可用。
TEST(XcpSlaveDemoIntegrationTest, GeneratedTcpA2lFeedsLiveSessionOverTcp) {
    TemporaryDirectory directory("live_tcp_session");
    const auto output_dir = directory.Path() / "a2l";
    TcpPortReservation reservation;
    ASSERT_TRUE(reservation.BindFree());
    const std::uint16_t port = reservation.Port();
    const auto generation_exit =
        GenerateA2l(directory.Path(), output_dir, port, false, "tcp");
    ASSERT_TRUE(generation_exit.has_value());
    ASSERT_EQ(*generation_exit, 0);
    reservation.Close();

    // A2L 必须声明 TCP 端点：主站靠它拿到端口并决定传输类别，
    // 声明成 UDP 会让 "用 TCP 连接" 变成连一个没人监听的端口。
    const auto a2l_path = output_dir / "xcp_slave_demo.a2l";
    {
        std::ifstream a2l_stream(a2l_path);
        ASSERT_TRUE(a2l_stream.good()) << "未生成 A2L：" << a2l_path;
        const std::string text((std::istreambuf_iterator<char>(a2l_stream)),
                               std::istreambuf_iterator<char>());
        EXPECT_NE(text.find("/begin XCP_ON_TCP_IP"), std::string::npos)
            << "A2L 未声明 XCP_ON_TCP_IP";
        EXPECT_EQ(text.find("/begin XCP_ON_UDP_IP"), std::string::npos)
            << "TCP 模式下 A2L 不应声明 UDP 端点";
    }

    auto loaded = a2l::A2lBridge::Load(a2l_path.string());
    ASSERT_TRUE(loaded.HasValue()) << loaded.ErrorInfo().message;
    const a2l::IA2lDatabase* database = loaded.Value()->Database();
    ASSERT_NE(database, nullptr);
    auto fast_leaf = database->Find("xcp_slave_demo::meas_fast.output");
    ASSERT_TRUE(fast_leaf.HasValue()) << "A2L 缺少 meas_fast.output";
    auto slow_leaf =
        database->Find("xcp_slave_demo::meas_slow.statistics.fast_cycle_count");
    ASSERT_TRUE(slow_leaf.HasValue())
        << "A2L 缺少 meas_slow.statistics.fast_cycle_count";

    const auto service_directory = directory.Path() / "service";
    std::filesystem::create_directories(service_directory);
    DemoChildProcess slave_process;
    std::string start_error;
    ASSERT_TRUE(StartDemoProcess(
        slave_process, {"--transport", "tcp", "--bind", "127.0.0.1", "--port",
                        std::to_string(port)},
        service_directory, &start_error))
        << start_error;

    DemoDtoCollector collector;
    auto master = ConnectToDemoOverTcp(port, &collector);
    ASSERT_NE(master, nullptr)
        << "TCP 从站未在 8 秒内完成真实 XCP CONNECT（检查 --transport tcp 与端口占用）";

    // POLL：同一叶子连读两次，验证 TCP 上的请求/响应成帧（含长度前缀）。
    const auto read_output = [&master, &fast_leaf]() {
        const Bytes raw = master->ReadMemoryBytes(
            static_cast<Address>(fast_leaf.Value().xcp_address),
            fast_leaf.Value().address_extension,
            fast_leaf.Value().element_size_bytes);
        double value = 0.0;
        if (raw.size() == sizeof(value)) {
            std::memcpy(&value, raw.data(), sizeof(value));
        }
        return std::make_pair(raw.size(), value);
    };
    const auto first = read_output();
    ASSERT_EQ(first.first, sizeof(double)) << "TCP POLL 未返回 8 字节 double";
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const auto second = read_output();
    ASSERT_EQ(second.first, sizeof(double));
    EXPECT_TRUE(std::isfinite(first.second) && std::isfinite(second.second))
        << "TCP POLL 读到的测量值不是有限数";
    // 10 ms 事件驱动的波形在 60 ms 后必然变化；若相等，说明读到的是缓存或
    // 错位的响应（两类问题在 TCP 上都表现为"值不动"）。
    EXPECT_NE(first.second, second.second)
        << "两次 POLL 数值相同，疑似 TCP 响应错位";

    // DAQ：慢速 uint32 叶子经 XCPlite 布局解码后必须出现非零值。
    // 该布局的 DTO 会把 4 字节补白计入 wire LEN，能解码即证明主站按 ODT
    // 布局取前缀而非按 LEN 全量复制。
    DaqEntrySpec slow_entry;
    slow_entry.address = static_cast<Address>(slow_leaf.Value().xcp_address);
    slow_entry.extension = slow_leaf.Value().address_extension;
    slow_entry.size = slow_leaf.Value().element_size_bytes;
    DaqOdtSpec slow_odt;
    slow_odt.entries.push_back(slow_entry);
    DaqListSpec slow_list;
    slow_list.daq_list = 0U;
    slow_list.event_channel = slow_leaf.Value().daq_event_fixed.front();
    slow_list.timestamp = true;
    slow_list.odts.push_back(slow_odt);
    ASSERT_NO_THROW(master->ConfigureDaqListsDynamic({slow_list}));
    ASSERT_NO_THROW(master->StartDaqSync());
    EXPECT_TRUE(collector.WaitForNonzeroUint32(std::chrono::seconds(3)))
        << "TCP DAQ DTO 未解码出非零 uint32";
    ASSERT_NO_THROW(master->StopDaq());
    master->Disconnect();
    slave_process.Stop();
}

}  // namespace
}  // namespace calmcar::xcp
