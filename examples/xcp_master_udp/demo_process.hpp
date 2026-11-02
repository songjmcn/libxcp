/**
 * @file demo_process.hpp
 * @brief 示例自包含的子进程封装（不依赖 tests/ 夹具）。
 *
 * 仅提供示例需要的最小能力：在指定工作目录启动一个可执行文件（可传参数）、
 * 等待退出并取得退出码、判定存活、终止（超时后强制结束）。
 * Windows 使用 CreateProcess/TerminateProcess；POSIX 使用 posix_spawn 与
 * SIGTERM/SIGKILL。
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xcp_example {

/**
 * @brief 子进程句柄封装（不可拷贝，析构自动 Stop）。
 */
class DemoProcess {
public:
    DemoProcess() = default;
    ~DemoProcess();

    DemoProcess(const DemoProcess&) = delete;
    DemoProcess& operator=(const DemoProcess&) = delete;

    /**
     * @brief 在 workingDir 作为工作目录启动 exePath（无命令行参数）。
     * @details 需要命令行参数时调用接收 arguments 的重载。
     *
     * stdout/stderr 继承父进程控制台（与测试夹具行为一致）。
     * @param exePath     可执行文件路径
     * @param workingDir  子进程工作目录（A2L/.bin 产物落在这里）
     * @param errorMessage 失败时回填人类可读原因（可为 nullptr）
     * @return 启动成功返回 true
     */
    bool Start(const std::string& exePath,
               const std::filesystem::path& workingDir,
               std::string* errorMessage);

    /**
     * @brief 启动带独立命令行参数的子进程。
     * @param exePath 可执行文件路径
     * @param workingDir 子进程工作目录
     * @param arguments 命令行参数列表；不经过 shell 解释
     * @param errorMessage 失败时回填人类可读原因（可为 nullptr）
     * @return 启动成功返回 true
     */
    bool Start(const std::string& exePath,
               const std::filesystem::path& workingDir,
               const std::vector<std::string>& arguments,
               std::string* errorMessage);

    /** @brief 进程是否仍在运行。 */
    [[nodiscard]] bool IsRunning() const;

    /**
     * @brief 等待子进程退出并取得退出码。
     * @param timeout 最长等待时间。
     * @param exitCode 子进程退出码输出指针，可为 nullptr。
     * @return 在时限内观察到进程退出时返回 true。
     */
    [[nodiscard]] bool WaitForExit(std::chrono::milliseconds timeout,
                                   int* exitCode);

    /**
     * @brief 终止进程：先等待 graceTime 优雅退出，超时则强制结束。
     *
     * 示例子进程没有统一的业务层退出协议；超时后由平台 API 强制结束进程。
     */
    void Stop(
        std::chrono::milliseconds graceTime = std::chrono::milliseconds(2000));

    /** @brief 子进程 pid（未启动/已回收时为 0）。 */
    [[nodiscard]] std::uint32_t ProcessId() const { return pid_; }

private:
#ifdef _WIN32
    void* processHandle_ =
        nullptr;  ///< HANDLE 句柄（以 void* 存放避免头文件污染）
    void* threadHandle_ = nullptr;  ///< 主线程句柄
#endif
    std::uint32_t pid_ = 0;
};

}  // namespace xcp_example
