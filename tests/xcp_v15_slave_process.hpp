/**
 * @file xcp_v15_slave_process.hpp
 * @brief V1.5 对齐矩阵验收用的独立从站进程控制夹具声明。
 *
 * @par 为什么需要独立进程
 *   同一进程内同时运行 Master 与 Slave 只能证明本库自洽。把从站放到独立进程
 *   （并让它使用 `tests/tcp_test_slave.*` 里独立实现的 V1.5 framing），才能拿到
 *   接近真实部署的线路证据。
 */
#ifndef LIBXCP_TEST_XCP_V15_SLAVE_PROCESS_HPP_
#define LIBXCP_TEST_XCP_V15_SLAVE_PROCESS_HPP_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace calmcar::xcp::test {

/**
 * @brief 启停独立的 V1.5 测试从站进程。
 * @details 仅供测试使用；不链接任何第三方从站实现。
 */
class XcpV15SlaveProcess {
public:
    /**
     * @brief 创建从站进程控制器。
     * @param alignment 从站宣告并使用的 PACKET_ALIGNMENT 位宽（8/16/32）
     * @param corrupt_len_at 把第 N 个响应的 LEN 写大（0 = 不注入）；负向用例用
     */
    explicit XcpV15SlaveProcess(std::uint8_t alignment,
                                std::size_t corrupt_len_at = 0);
    /// @brief 终止并回收子进程，清理本次运行目录。
    ~XcpV15SlaveProcess();

    XcpV15SlaveProcess(const XcpV15SlaveProcess&) = delete;
    XcpV15SlaveProcess& operator=(const XcpV15SlaveProcess&) = delete;

    /**
     * @brief 启动从站并等待其就绪文件出现。
     * @throws std::runtime_error 启动失败、就绪超时或端口不可解析
     */
    void Start();

    /// @brief 幂等停止并回收子进程。
    void Stop() noexcept;

    /// @brief 返回从站实际绑定的回环端口。
    [[nodiscard]] std::uint16_t Port() const noexcept;

    /// @brief 返回从站日志文件路径（失败诊断用）。
    [[nodiscard]] std::filesystem::path LogPath() const;

private:
    struct Impl;  ///< 平台相关实现（隐藏 Windows/POSIX 差异）
    std::unique_ptr<Impl> impl_;  ///< 实现体
};

}  // namespace calmcar::xcp::test

#endif  // LIBXCP_TEST_XCP_V15_SLAVE_PROCESS_HPP_