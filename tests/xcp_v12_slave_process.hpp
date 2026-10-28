/**
 * @file xcp_v12_slave_process.hpp
 * @brief V1.2 从站真实进程验收夹具声明。
 */
#ifndef LIBXCP_TEST_XCP_V12_SLAVE_PROCESS_HPP_
#define LIBXCP_TEST_XCP_V12_SLAVE_PROCESS_HPP_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace calmcar::xcp::test {

/**
 * @brief 启停独立的 XCPlite V1.2 从站进程。
 * @details 仅供测试使用。父进程不链接 XCPlite V1.2，避免与 V2.2 C 符号冲突。
 */
class XcpliteV12SlaveProcess {
public:
    /**
     * @brief 创建从站进程控制器。
     * @param transport 传输名称，只允许 udp 或 tcp。
     */
    explicit XcpliteV12SlaveProcess(std::string transport);
    /** @brief 终止并回收子进程，清理本次运行目录。 */
    ~XcpliteV12SlaveProcess();

    XcpliteV12SlaveProcess(const XcpliteV12SlaveProcess&) = delete;
    XcpliteV12SlaveProcess& operator=(const XcpliteV12SlaveProcess&) = delete;

    /** @brief 启动从站，并通过真实 XCP CONNECT/DISCONNECT 等待就绪。 */
    void Start();
    /** @brief 在同一目录连续生成两次 A2L，校验覆盖与自解析流程。 */
    void VerifyA2lOverwrite();
    /** @brief 幂等停止并回收子进程。 */
    void Stop() noexcept;
    /** @brief 返回实际使用的 TCP/UDP 端口。 */
    [[nodiscard]] std::uint16_t Port() const noexcept;
    /** @brief 返回从站生成的 A2L 文件路径。 */
    [[nodiscard]] std::filesystem::path A2lPath() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace calmcar::xcp::test

#endif  // LIBXCP_TEST_XCP_V12_SLAVE_PROCESS_HPP_
