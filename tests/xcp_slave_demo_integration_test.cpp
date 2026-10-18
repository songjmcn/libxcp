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
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "libxcp/a2l/a2l_bridge.hpp"
#include "libxcp/command_executor.hpp"
#include "libxcp/daq/dto_envelope_decoder.hpp"
#include "libxcp/protocol_types.hpp"
#include "libxcp/udp_transport.hpp"
#include "libxcp/xcp_error.hpp"
#include "libxcp/xcp_master.hpp"
#include "slave_model.hpp"
#include "xcplite_slave_fixture.hpp"

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
    return std::make_unique<XcpMaster>(
        std::make_unique<UdpTransport>(config),
        CommandTimeouts{std::chrono::milliseconds(300),
                        std::chrono::milliseconds(300), 1},
        listener);
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
 * @param port 与待验证端点对应的 UDP 端口。
 * @param overwrite 是否显式允许覆盖。
 * @return 子进程退出码；启动失败或超时返回空值。
 */
std::optional<int> GenerateA2l(const std::filesystem::path& directory,
                               const std::filesystem::path& output_dir,
                               std::uint16_t port, bool overwrite) {
    std::vector<std::string> arguments{
        "--generate-a2l", "--output-dir", output_dir.string(), "--bind",
        "127.0.0.1",      "--port",       std::to_string(port)};
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

    const auto cal_symbol = demo_test::ResolveA2lSymbol(a2l_path, "cal");
    const auto fast_symbol = demo_test::ResolveA2lSymbol(a2l_path, "meas_fast");
    const auto slow_symbol = demo_test::ResolveA2lSymbol(a2l_path, "meas_slow");
    ASSERT_TRUE(cal_symbol.has_value()) << "A2L 缺少 cal 标定段实例";
    ASSERT_TRUE(fast_symbol.has_value()) << "A2L 缺少 meas_fast 测量实例";
    ASSERT_TRUE(slow_symbol.has_value()) << "A2L 缺少 meas_slow 测量实例";
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
    output_entry.address = static_cast<Address>(
        fast_symbol->address + offsetof(FastMeasurements, output));
    output_entry.extension = fast_symbol->extension;
    output_entry.size = static_cast<std::uint8_t>(sizeof(double));
    DaqOdtSpec odt;
    odt.entries.push_back(output_entry);
    DaqListSpec daq_list;
    daq_list.daq_list = 0U;
    daq_list.event_channel = *fast_event;
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
    slow_entry.address = static_cast<Address>(
        slow_symbol->address + offsetof(SlowMeasurements, statistics) +
        offsetof(SlowMeasurementStatistics, fast_cycle_count));
    slow_entry.extension = slow_symbol->extension;
    slow_entry.size = static_cast<std::uint8_t>(sizeof(std::uint32_t));
    DaqOdtSpec slow_odt;
    slow_odt.entries.push_back(slow_entry);
    DaqListSpec slow_daq_list;
    slow_daq_list.daq_list = 0U;
    slow_daq_list.event_channel = *slow_event;
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

}  // namespace
}  // namespace calmcar::xcp
