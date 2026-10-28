/**
 * @file xcp_v12_slave_process.cpp
 * @brief V1.2 从站真实进程验收夹具实现。
 */
#include "xcp_v12_slave_process.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "libxcp/tcp_transport.hpp"
#include "libxcp/tcp_transport_config.hpp"
#include "libxcp/udp_transport.hpp"
#include "libxcp/xcp_master.hpp"

#ifndef XCP_V12_SLAVE_EXECUTABLE
#error "XCP_V12_SLAVE_EXECUTABLE must be supplied by CMake"
#endif
#ifndef XCP_V12_TEST_ROOT
#error "XCP_V12_TEST_ROOT must be supplied by CMake"
#endif

namespace calmcar::xcp::test {
namespace {

constexpr auto kStartupTimeout = std::chrono::seconds(12);  ///< 从站启动探测时限。
constexpr auto kProbeInterval = std::chrono::milliseconds(100);  ///< 探测间隔。
constexpr auto kStopTimeout = std::chrono::seconds(2);  ///< POSIX 优雅退出等待时限。
constexpr std::uint16_t kPortSpan = 16;  ///< 发生端口冲突时尝试的连续端口数。

/** @brief 获取当前测试父进程 ID，用于隔离端口与临时目录。 */
std::uint32_t CurrentProcessId() noexcept {
#ifdef _WIN32
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}

/** @brief 构造用于确认从站就绪的 TCP/UDP Master Transport。 */
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
        config.connect_timeout_ms = 1000;
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

/** @brief 用真实 CONNECT/DISCONNECT 验证从站已开始服务。 */
bool Probe(std::string_view transport, std::uint16_t port) noexcept {
    try {
        XcpMasterOptions options;
        options.timeouts = CommandTimeouts{std::chrono::milliseconds(500),
                                           std::chrono::milliseconds(500), 0};
        XcpMaster master(MakeTransport(std::string(transport), port), options);
        master.Connect();
        master.Disconnect();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

#ifdef _WIN32
/** @brief 按 Windows 命令行转义规则引用一个参数。 */
std::string QuoteWindowsArgument(const std::string& value) {
    std::string quoted = "\"";
    std::size_t slashes = 0;
    for (const char ch : value) {
        if (ch == '\\') {
            ++slashes;
            continue;
        }
        if (ch == '"') {
            quoted.append(slashes * 2U + 1U, '\\');
            quoted.push_back('"');
            slashes = 0;
            continue;
        }
        quoted.append(slashes, '\\');
        slashes = 0;
        quoted.push_back(ch);
    }
    quoted.append(slashes * 2U, '\\');
    quoted.push_back('"');
    return quoted;
}
#endif

}  // namespace

struct XcpliteV12SlaveProcess::Impl {
    explicit Impl(std::string selected_transport)
        : transport(std::move(selected_transport)) {}

    std::string transport;  ///< 当前从站使用的传输方式。
    std::uint16_t port{0};  ///< 当前测试预留的 TCP/UDP 端口。
    std::filesystem::path run_directory;  ///< 当前子进程与 A2L 的隔离目录。
    bool started{false};  ///< 子进程是否已通过 XCP 就绪探测。
#ifdef _WIN32
    HANDLE process{nullptr};  ///< Windows 子进程句柄。
    HANDLE thread{nullptr};  ///< Windows 子进程主线程句柄。

    /** @brief 检查 Windows 子进程是否仍运行。 */
    /** @brief 检查 Windows 子进程是否仍运行。 */
    [[nodiscard]] bool Alive() const noexcept {
        if (process == nullptr) {
            return false;
        }
        DWORD code = 0;
        return GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    }

    /** @brief 终止并关闭 Windows 子进程句柄。 */
    void Kill() noexcept {
        if (process != nullptr) {
            if (Alive()) {
                TerminateProcess(process, 1);
            }
            WaitForSingleObject(process, 2000);
            CloseHandle(process);
            process = nullptr;
        }
        if (thread != nullptr) {
            CloseHandle(thread);
            thread = nullptr;
        }
    }
#else
    pid_t pid{-1};  ///< POSIX 子进程 ID。

    /** @brief 检查 POSIX 子进程是否仍运行并回收已退出进程。 */
    [[nodiscard]] bool Alive() noexcept {
        if (pid <= 0) {
            return false;
        }
        int status = 0;
        const pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid || (result == -1 && errno == ECHILD)) {
            pid = -1;
            return false;
        }
        return result == 0 || (result == -1 && errno == EINTR);
    }

    /** @brief 先发送 SIGTERM，超时后强制终止并 waitpid 回收。 */
    void Kill() noexcept {
        if (pid <= 0) {
            return;
        }
        int status = 0;
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid || (result == -1 && errno == ECHILD)) {
            pid = -1;
            return;
        }
        kill(pid, SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() + kStopTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            result = waitpid(pid, &status, WNOHANG);
            if (result == pid || (result == -1 && errno == ECHILD)) {
                pid = -1;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
        }
        pid = -1;
    }
#endif

    /** @brief 以生成模式运行 V1.2 程序并等待其退出码。 */
    bool RunGenerator(std::uint16_t selected_port) {
        port = selected_port;
        const std::string executable = XCP_V12_SLAVE_EXECUTABLE;
        const std::string port_arg = std::to_string(port);
        const std::string directory_arg = run_directory.string();
#ifdef _WIN32
        std::string command = QuoteWindowsArgument(executable) +
                              " --generate-a2l --overwrite --verify-a2l --transport " +
                              transport + " --bind 127.0.0.1 --port " + port_arg +
                              " --output-dir " + QuoteWindowsArgument(directory_arg);
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, 0,
                            nullptr, directory_arg.c_str(), &startup, &info)) {
            return false;
        }
        CloseHandle(info.hThread);
        const DWORD wait_result = WaitForSingleObject(info.hProcess, 30000);
        DWORD exit_code = 1;
        const bool exited = wait_result == WAIT_OBJECT_0 &&
                            GetExitCodeProcess(info.hProcess, &exit_code);
        CloseHandle(info.hProcess);
        return exited && exit_code == 0;
#else
        const pid_t generator = fork();
        if (generator < 0) {
            return false;
        }
        if (generator == 0) {
            if (chdir(directory_arg.c_str()) != 0) {
                _exit(127);
            }
            execl(executable.c_str(), executable.c_str(), "--generate-a2l",
                  "--overwrite", "--verify-a2l", "--transport", transport.c_str(),
                  "--bind", "127.0.0.1", "--port", port_arg.c_str(),
                  "--output-dir", directory_arg.c_str(),
                  static_cast<char*>(nullptr));
            _exit(126);
        }
        int status = 0;
        while (waitpid(generator, &status, 0) == -1) {
            if (errno != EINTR) {
                return false;
            }
        }
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
    }

    /** @brief 在独立目录启动常驻服务模式 V1.2 从站。 */
    bool Spawn(std::uint16_t selected_port) {
        port = selected_port;
        run_directory = std::filesystem::path(XCP_V12_TEST_ROOT) /
                        (transport + "_" + std::to_string(CurrentProcessId()) +
                         "_" + std::to_string(port));
        std::error_code error;
        std::filesystem::remove_all(run_directory, error);
        error.clear();
        std::filesystem::create_directories(run_directory, error);
        if (error) {
            throw std::runtime_error("创建 V1.2 测试目录失败: " + error.message());
        }
        const std::string executable = XCP_V12_SLAVE_EXECUTABLE;
        const std::string port_arg = std::to_string(port);
        const std::string directory_arg = run_directory.string();
#ifdef _WIN32
        std::string command = QuoteWindowsArgument(executable) +
                              " --transport " + transport +
                              " --bind 127.0.0.1 --port " + port_arg +
                              " --output-dir " + QuoteWindowsArgument(directory_arg) +
                              " --ready-token v12-test-ready";
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, 0,
                            nullptr, directory_arg.c_str(), &startup, &info)) {
            return false;
        }
        process = info.hProcess;
        thread = info.hThread;
        return true;
#else
        pid = fork();
        if (pid < 0) {
            return false;
        }
        if (pid == 0) {
            if (chdir(directory_arg.c_str()) != 0) {
                _exit(127);
            }
            execl(executable.c_str(), executable.c_str(), "--transport",
                  transport.c_str(), "--bind", "127.0.0.1", "--port",
                  port_arg.c_str(), "--output-dir", directory_arg.c_str(),
                  "--ready-token", "v12-test-ready",
                  static_cast<char*>(nullptr));
            _exit(126);
        }
        return true;
#endif
    }
};

XcpliteV12SlaveProcess::XcpliteV12SlaveProcess(std::string transport)
    : impl_(std::make_unique<Impl>(std::move(transport))) {
    if (impl_->transport != "tcp" && impl_->transport != "udp") {
        throw std::invalid_argument("V1.2 transport 只能是 tcp 或 udp");
    }
}

XcpliteV12SlaveProcess::~XcpliteV12SlaveProcess() { Stop(); }

void XcpliteV12SlaveProcess::VerifyA2lOverwrite() {
    if (impl_->started) {
        throw std::logic_error("从站已启动，不能运行 A2L 覆盖验证");
    }
    const auto root = std::filesystem::path(XCP_V12_TEST_ROOT);
    impl_->run_directory =
        root / ("a2l_" + impl_->transport + "_" +
                std::to_string(CurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(impl_->run_directory, error);
    error.clear();
    std::filesystem::create_directories(impl_->run_directory, error);
    if (error) {
        throw std::runtime_error("创建 A2L 覆盖测试目录失败: " +
                                 error.message());
    }
    const auto generator_port = static_cast<std::uint16_t>(
        43000U + (CurrentProcessId() % 10000U));
    for (int pass = 0; pass < 2; ++pass) {
        if (!impl_->RunGenerator(generator_port) ||
            !std::filesystem::exists(A2lPath())) {
            throw std::runtime_error("A2L 第 " + std::to_string(pass + 1) +
                                     " 次生成/覆盖失败");
        }
    }
}

void XcpliteV12SlaveProcess::Start() {
    if (impl_->started) {
        return;
    }
    const std::uint16_t base = static_cast<std::uint16_t>(
        44000U + (CurrentProcessId() % 10000U));
    std::string last_error = "从站进程无法启动";
    for (std::uint16_t attempt = 0; attempt < kPortSpan; ++attempt) {
        const auto candidate = static_cast<std::uint16_t>(base + attempt);
        if (!impl_->Spawn(candidate)) {
            last_error = "创建 XCPlite V1.2 从站子进程失败";
            continue;
        }
        const auto deadline = std::chrono::steady_clock::now() + kStartupTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!impl_->Alive()) {
                last_error = "XCPlite V1.2 从站提前退出（端口可能已占用）";
                break;
            }
            if (Probe(impl_->transport, candidate)) {
                if (!std::filesystem::exists(A2lPath())) {
                    last_error = "从站已响应但未生成 A2L 文件";
                    break;
                }
                impl_->started = true;
                return;
            }
            std::this_thread::sleep_for(kProbeInterval);
        }
        impl_->Kill();
        std::error_code ignored;
        std::filesystem::remove_all(impl_->run_directory, ignored);
    }
    Stop();
    throw std::runtime_error("XCPlite V1.2 " + impl_->transport +
                             " 从站就绪失败: " + last_error);
}

void XcpliteV12SlaveProcess::Stop() noexcept {
    if (!impl_) {
        return;
    }
    impl_->Kill();
    impl_->started = false;
    if (!impl_->run_directory.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(impl_->run_directory, ignored);
    }
}

std::uint16_t XcpliteV12SlaveProcess::Port() const noexcept {
    return impl_->port;
}

std::filesystem::path XcpliteV12SlaveProcess::A2lPath() const {
    return impl_->run_directory / "xcp_slave_demo_v12.a2l";
}

}  // namespace calmcar::xcp::test
