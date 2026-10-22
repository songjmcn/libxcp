#pragma once

/**
 * @file program_logging.hpp
 * @brief 项目自有可执行程序的内部 glog 文件日志生命周期辅助类。
 */

#include <string>
#include <string_view>

namespace calmcar::xcp::internal {

/**
 * @brief 管理一个自有可执行程序进程的文件日志生命周期。
 * @details 仅用于 tools、examples 与 tests 的可执行入口，不属于 libxcp 公共
 * API。
 */
class ProgramLogSession final {
public:
    /** @brief 析构时刷新并关闭本对象初始化的 glog 实例。 */
    ~ProgramLogSession();

    /** @brief 创建尚未初始化的日志生命周期对象。 */
    ProgramLogSession() = default;

    /** @brief 禁止复制，避免多个对象重复关闭同一 glog 实例。 */
    ProgramLogSession(const ProgramLogSession&) = delete;

    /** @brief 禁止复制赋值，避免多个对象重复关闭同一 glog 实例。 */
    ProgramLogSession& operator=(const ProgramLogSession&) = delete;

    /** @brief 禁止移动，确保日志关闭责任保持在初始化对象上。 */
    ProgramLogSession(ProgramLogSession&&) = delete;

    /** @brief 禁止移动赋值，确保日志关闭责任保持在初始化对象上。 */
    ProgramLogSession& operator=(ProgramLogSession&&) = delete;

    /**
     * @brief 在相对当前工作目录的 logs/<module>/<program>/ 下初始化文件日志。
     * @param executable_path 当前进程的 argv[0]。
     * @param module 模块目录名称，例如 tools、examples 或 tests。
     * @param error_message 初始化失败时接收可供普通启动诊断使用的原因。
     * @return 初始化成功时返回 true；若 glog
     * 已由其他代码初始化或初始化失败则返回 false。
     */
    bool Initialize(std::string_view executable_path, std::string_view module,
                    std::string& error_message);

    /** @brief 刷新并关闭本对象拥有的 glog 实例；可重复调用。 */
    void Shutdown() noexcept;

private:
    /** @brief 保持 argv0 存储有效，直到 glog 完成关闭。 */
    std::string executable_name_{};
    /** @brief 本对象是否负责关闭 glog。 */
    bool owns_glog_{false};
};

}  // namespace calmcar::xcp::internal
