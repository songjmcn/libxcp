/**
 * @file xcp_v15_slave_process.cpp
 * @brief V1.5 对齐矩阵验收用的独立从站进程控制夹具实现。
 */
#include "xcp_v15_slave_process.hpp"

#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

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

#ifndef XCP_V15_SLAVE_EXECUTABLE
#error "XCP_V15_SLAVE_EXECUTABLE must be supplied by CMake"
#endif
#ifndef XCP_V15_TEST_ROOT
#error "XCP_V15_TEST_ROOT must be supplied by CMake"
#endif

namespace calmcar::xcp::test {
namespace {

/// @brief 等待就绪文件的时限。
constexpr auto kStartupTimeout = std::chrono::seconds(15);

/// @brief 轮询就绪文件的间隔。
constexpr auto kPollInterval = std::chrono::milliseconds(20);

/// @brief 从站运行时长上限，留足用例执行余量。
constexpr std::uint32_t kMaxRuntimeMs = 55000;

#ifdef _WIN32
/**
 * @brief 按 Windows 命令行转义规则引用一个参数。
 * @param value 待引用参数
 * @return 可直接拼入命令行的字符串
 */
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

/// @brief 获取当前测试父进程 ID，用于隔离运行目录。
std::uint32_t CurrentProcessId() noexcept {
#ifdef _WIN32
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}

/**
 * @brief 从就绪文件内容中解析 PORT 字段。
 * @param content 就绪文件全文
 * @return 解析出的端口，失败返回 0
 */
std::uint16_t ParsePort(const std::string& content) noexcept {
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        constexpr std::string_view kPrefix = "PORT=";
        if (line.rfind(std::string(kPrefix), 0) != 0) {
            continue;
        }
        const std::string digits = line.substr(kPrefix.size());
        if (digits.empty()) {
            return 0;
        }
        unsigned long value = 0;
        try {
            value = std::stoul(digits);
        } catch (const std::exception&) {
            return 0;
        }
        if (value == 0 || value > 65535) {
            return 0;
        }
        return static_cast<std::uint16_t>(value);
    }
    return 0;
}

}  // namespace

struct XcpV15SlaveProcess::Impl {
    /// @brief 从站宣告的 PACKET_ALIGNMENT 位宽。
    std::uint8_t alignment{8};
    /// @brief 是否注入畸形 LEN（0 = 不注入）。
    std::size_t corrupt_len_at{0};
    /// @brief 实际绑定的端口（就绪后填入）。
    std::uint16_t port{0};
    /// @brief 本次运行目录，结束时清理。
    std::filesystem::path run_directory;
    /// @brief 就绪文件路径。
    std::filesystem::path ready_file;
    /// @brief 日志文件路径。
    std::filesystem::path log_file;

#ifdef _WIN32
    HANDLE process{nullptr};  ///< 子进程句柄
    HANDLE thread{nullptr};   ///< 子进程主线程句柄
#else
    pid_t pid{-1};  ///< 子进程 PID
#endif
};

XcpV15SlaveProcess::XcpV15SlaveProcess(std::uint8_t alignment,
                                       std::size_t corrupt_len_at)
    : impl_(std::make_unique<Impl>()) {
    if (alignment != 8 && alignment != 16 && alignment != 32) {
        throw std::invalid_argument("XcpV15SlaveProcess: alignment 只允许 8/16/32");
    }
    impl_->alignment = alignment;
    impl_->corrupt_len_at = corrupt_len_at;
}

XcpV15SlaveProcess::~XcpV15SlaveProcess() {
    Stop();
}

void XcpV15SlaveProcess::Start() {
    if (impl_->port != 0) {
        return;  // 幂等
    }

    impl_->run_directory =
        std::filesystem::path(XCP_V15_TEST_ROOT) /
        ("a" + std::to_string(static_cast<int>(impl_->alignment)) + "_" +
         std::to_string(CurrentProcessId()) + "_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() %
                        100000));
    std::error_code error;
    std::filesystem::remove_all(impl_->run_directory, error);
    error.clear();
    std::filesystem::create_directories(impl_->run_directory, error);
    if (error) {
        throw std::runtime_error("创建 V1.5 测试目录失败: " + error.message());
    }
    impl_->ready_file = impl_->run_directory / "ready.txt";
    impl_->log_file = impl_->run_directory / "slave.log";

    const std::string executable = XCP_V15_SLAVE_EXECUTABLE;
    const std::string alignment_arg = std::to_string(
        static_cast<int>(impl_->alignment));
    const std::string runtime_arg = std::to_string(kMaxRuntimeMs);
    const std::string ready_arg = impl_->ready_file.string();
    const std::string log_arg = impl_->log_file.string();
    const std::string directory_arg = impl_->run_directory.string();
    const std::string corrupt_arg = std::to_string(impl_->corrupt_len_at);

#ifdef _WIN32
    std::string command = QuoteWindowsArgument(executable) +
                          " --alignment " + alignment_arg + " --port 0" +
                          " --ready-file " + QuoteWindowsArgument(ready_arg) +
                          " --max-runtime-ms " + runtime_arg +
                          " --log-file " + QuoteWindowsArgument(log_arg);
    if (impl_->corrupt_len_at != 0) {
        command += " --corrupt-len-at " + std::to_string(impl_->corrupt_len_at);
    }
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, 0,
                        nullptr, directory_arg.c_str(), &startup, &info)) {
        throw std::runtime_error("创建 V1.5 从站进程失败，错误码 " +
                                 std::to_string(GetLastError()));
    }
    impl_->process = info.hProcess;
    impl_->thread = info.hThread;
#else
    const pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("fork 失败");
    }
    if (pid == 0) {
        if (chdir(directory_arg.c_str()) != 0) {
            _exit(127);
        }
        if (impl_->corrupt_len_at != 0) {
            execl(executable.c_str(), executable.c_str(), "--alignment",
                  alignment_arg.c_str(), "--port", "0", "--ready-file",
                  ready_arg.c_str(), "--max-runtime-ms", runtime_arg.c_str(),
                  "--log-file", log_arg.c_str(), "--corrupt-len-at",
                  corrupt_arg.c_str(), static_cast<char*>(nullptr));
        } else {
            execl(executable.c_str(), executable.c_str(), "--alignment",
                  alignment_arg.c_str(), "--port", "0", "--ready-file",
                  ready_arg.c_str(), "--max-runtime-ms", runtime_arg.c_str(),
                  "--log-file", log_arg.c_str(), static_cast<char*>(nullptr));
        }
        _exit(126);
    }
    impl_->pid = pid;
#endif

    // 轮询就绪文件；超时则回收子进程并报告日志尾部，避免只看到"连接超时"。
    const auto deadline = std::chrono::steady_clock::now() + kStartupTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream input(impl_->ready_file, std::ios::binary);
        if (input.is_open()) {
            std::ostringstream buffer;
            buffer << input.rdbuf();
            const std::uint16_t port = ParsePort(buffer.str());
            if (port != 0) {
                impl_->port = port;
                return;
            }
        }
        std::this_thread::sleep_for(kPollInterval);
    }

    std::string detail;
    {
        std::ifstream input(impl_->log_file, std::ios::binary);
        if (input.is_open()) {
            std::ostringstream buffer;
            buffer << input.rdbuf();
            detail = buffer.str();
        }
    }
    Stop();
    throw std::runtime_error("V1.5 从站未在时限内就绪；日志: " + detail);
}

void XcpV15SlaveProcess::Stop() noexcept {
#ifdef _WIN32
    if (impl_->process != nullptr) {
        const DWORD wait_result = WaitForSingleObject(impl_->process, 0);
        if (wait_result == WAIT_TIMEOUT) {
            TerminateProcess(impl_->process, 1);
            WaitForSingleObject(impl_->process, 5000);
        }
        CloseHandle(impl_->process);
        impl_->process = nullptr;
    }
    if (impl_->thread != nullptr) {
        CloseHandle(impl_->thread);
        impl_->thread = nullptr;
    }
#else
    if (impl_->pid > 0) {
        int status = 0;
        pid_t result = waitpid(impl_->pid, &status, WNOHANG);
        while (result == -1 && errno == EINTR) {
            result = waitpid(impl_->pid, &status, WNOHANG);
        }
        if (result == 0 || (result == -1 && errno == ECHILD)) {
            // 仍在运行：先尝试 SIGTERM 让其自行清理，再强制结束。
            kill(impl_->pid, SIGTERM);
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < deadline) {
                result = waitpid(impl_->pid, &status, WNOHANG);
                if (result == impl_->pid) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (result == 0) {
                kill(impl_->pid, SIGKILL);
                while (waitpid(impl_->pid, &status, 0) == -1 && errno == EINTR) {
                }
            }
        }
        impl_->pid = -1;
    }
#endif
    impl_->port = 0;
    if (!impl_->run_directory.empty()) {
        std::error_code error;
        std::filesystem::remove_all(impl_->run_directory, error);
    }
}

std::uint16_t XcpV15SlaveProcess::Port() const noexcept {
    return impl_->port;
}

std::filesystem::path XcpV15SlaveProcess::LogPath() const {
    return impl_->log_file;
}

}  // namespace calmcar::xcp::test