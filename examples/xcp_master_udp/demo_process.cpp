/**
 * @file demo_process.cpp
 * @brief DemoProcess 的平台相关实现（仿 tests/xcplite_slave_fixture.cpp
 * 的做法，独立重写）。
 */
#include "demo_process.hpp"

#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <spawn.h>
extern char** environ;
#endif

namespace xcp_example {
namespace {

#ifdef _WIN32
/**
 * @brief 按 Windows CRT 规则引用一个命令行参数。
 * @param value 待引用参数
 * @return 可安全拼接到 CreateProcessA 命令行的参数
 */
std::string QuoteWindowsArgument(const std::string& value) {
    std::string quoted = "\"";
    std::size_t slashes = 0;
    for (const char ch : value) {
        if (ch == '\\') {
            ++slashes;
            continue;
        }
        if (ch == '\"') {
            quoted.append(slashes * 2U + 1U, '\\');
            quoted.push_back('\"');
            slashes = 0;
            continue;
        }
        quoted.append(slashes, '\\');
        slashes = 0;
        quoted.push_back(ch);
    }
    quoted.append(slashes * 2U, '\\');
    quoted.push_back('\"');
    return quoted;
}
#endif

}  // namespace

DemoProcess::~DemoProcess() { Stop(); }

bool DemoProcess::Start(const std::string& exePath,
                        const std::filesystem::path& workingDir,
                        std::string* errorMessage) {
    return Start(exePath, workingDir, {}, errorMessage);
}

bool DemoProcess::Start(const std::string& exePath,
                        const std::filesystem::path& workingDir,
                        const std::vector<std::string>& arguments,
                        std::string* errorMessage) {
    Stop();  // 幂等：先回收上一个

#ifdef _WIN32
    std::string cmdline = QuoteWindowsArgument(exePath);
    for (const auto& argument : arguments) {
        cmdline.push_back(' ');
        cmdline += QuoteWindowsArgument(argument);
    }
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // bInheritHandles=TRUE：子进程输出直接进入父控制台
    const BOOL ok =
        CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE, 0,
                       nullptr, workingDir.string().c_str(), &si, &pi);
    if (!ok) {
        if (errorMessage != nullptr) {
            *errorMessage =
                "CreateProcessA 失败，GetLastError=" +
                std::to_string(static_cast<unsigned long>(GetLastError()));
        }
        return false;
    }
    processHandle_ = pi.hProcess;
    threadHandle_ = pi.hThread;
    pid_ = static_cast<std::uint32_t>(pi.dwProcessId);
    return true;
#else
    pid_t child = -1;
    // 子进程里先 chdir 到工作目录再 exec，等价于 Windows 的 lpCurrentDirectory
    posix_spawn_file_actions_t actions{};
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc != 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "posix_spawn_file_actions_init 失败，errno=" +
                            std::to_string(rc);
        }
        return false;
    }
    rc = posix_spawn_file_actions_addchdir_np(&actions, workingDir.c_str());
    if (rc != 0) {
        posix_spawn_file_actions_destroy(&actions);
        if (errorMessage != nullptr) {
            *errorMessage =
                "设置子进程工作目录失败，errno=" + std::to_string(rc);
        }
        return false;
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2U);
    argv.push_back(const_cast<char*>(exePath.c_str()));
    for (const auto& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    rc = posix_spawn(&child, exePath.c_str(), &actions, nullptr, argv.data(),
                     environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "posix_spawn 失败，errno=" + std::to_string(rc);
        }
        return false;
    }
    pid_ = static_cast<std::uint32_t>(child);
    return true;
#endif
}

bool DemoProcess::IsRunning() const {
#ifdef _WIN32
    if (processHandle_ == nullptr) {
        return false;
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(static_cast<HANDLE>(processHandle_), &exitCode)) {
        return false;
    }
    return exitCode == STILL_ACTIVE;
#else
    if (pid_ == 0) {
        return false;
    }
    int status = 0;
    const pid_t r = waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    if (r == static_cast<pid_t>(pid_)) {
        const_cast<DemoProcess*>(this)->pid_ = 0;  // 已自然退出，回收
        return false;
    }
    return r == 0;
#endif
}

bool DemoProcess::WaitForExit(std::chrono::milliseconds timeout,
                              int* exitCode) {
    if (pid_ == 0) return false;
#ifdef _WIN32
    if (processHandle_ == nullptr) return false;
    const DWORD wait_result =
        WaitForSingleObject(static_cast<HANDLE>(processHandle_),
                            static_cast<DWORD>(timeout.count()));
    if (wait_result != WAIT_OBJECT_0) return false;
    DWORD process_exit_code = 0;
    if (!GetExitCodeProcess(static_cast<HANDLE>(processHandle_),
                            &process_exit_code)) {
        return false;
    }
    if (exitCode != nullptr) *exitCode = static_cast<int>(process_exit_code);
    pid_ = 0;
    return true;
#else
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const pid_t child = static_cast<pid_t>(pid_);
    int status = 0;
    do {
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            pid_ = 0;
            if (exitCode != nullptr) {
                *exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
            return true;
        }
        if (result < 0) return false;
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (true);
    return false;
#endif
}

void DemoProcess::Stop(std::chrono::milliseconds graceTime) {
#ifdef _WIN32
    if (processHandle_ != nullptr) {
        WaitForSingleObject(static_cast<HANDLE>(processHandle_),
                            static_cast<DWORD>(graceTime.count()));
        DWORD exitCode = 0;
        if (GetExitCodeProcess(static_cast<HANDLE>(processHandle_),
                               &exitCode) &&
            exitCode == STILL_ACTIVE) {
            TerminateProcess(static_cast<HANDLE>(processHandle_), 0);
            WaitForSingleObject(static_cast<HANDLE>(processHandle_), 2000);
        }
        CloseHandle(static_cast<HANDLE>(processHandle_));
        processHandle_ = nullptr;
    }
    if (threadHandle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(threadHandle_));
        threadHandle_ = nullptr;
    }
    pid_ = 0;
#else
    if (pid_ != 0) {
        const pid_t child = static_cast<pid_t>(pid_);
        kill(child, SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() + graceTime;
        int status = 0;
        bool reaped = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (waitpid(child, &status, WNOHANG) == child) {
                reaped = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!reaped) {
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
        }
        pid_ = 0;
    }
#endif
}

}  // namespace xcp_example
