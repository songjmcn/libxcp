/**
 * @file demo_process.hpp
 * @brief 示例自包含的 cpp_demo 子进程封装（不依赖 tests/ 夹具）。
 *
 * 仅提供示例需要的最小能力：在指定工作目录启动一个可执行文件、
 * 判定存活、终止（先礼后兵：等待优雅退出超时后强制结束）。
 * Windows 走 CreateProcess/TerminateProcess；POSIX 走 fork/exec + SIGTERM/SIGKILL
 * （POSIX 路径为对齐夹具做法的移植实现，实跑环境受宿主限制，见实施记录 21-1）。
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

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

    /** @brief 进程是否仍在运行。 */
    [[nodiscard]] bool IsRunning() const;

    /**
     * @brief 终止进程：先等待 graceTime 优雅退出，超时则强制结束。
     *
     * cpp_demo 主循环无退出命令通道，正常路径都是强制结束——
     * A2L 产物在首个 CONNECT 即落盘（WRITE_ONCE+FINALIZE_ON_CONNECT），硬杀无损。
     */
    void Stop(std::chrono::milliseconds graceTime = std::chrono::milliseconds(2000));

    /** @brief 子进程 pid（未启动/已回收时为 0）。 */
    [[nodiscard]] std::uint32_t ProcessId() const { return pid_; }

private:
#ifdef _WIN32
    void* processHandle_ = nullptr; ///< HANDLE 句柄（以 void* 存放避免头文件污染）
    void* threadHandle_ = nullptr;  ///< 主线程句柄
#endif
    std::uint32_t pid_ = 0;
};

}  // namespace xcp_example
