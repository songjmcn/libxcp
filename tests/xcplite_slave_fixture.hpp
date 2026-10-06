/**
 * @file xcplite_slave_fixture.hpp
 * @brief XCPlite Slave 对手端的测试夹具：子进程管理、就绪探测与 A2L 轻量解析。
 *
 * 依据 code-plan/XCPlite_Slave_协议调试集成计划.md Phase1-03 实现：
 *  - 以独立子进程启动 `xcp_test_slave`（XCPlite Slave），端口自动分配并在
 *    被占用时顺延重试；
 *  - 就绪判定用真实的 Master CONNECT/DISCONNECT 探测（无固定 sleep）；
 *  - 提供对 Slave 运行时生成 A2L 的轻量文本扫描（MEASUREMENT/INSTANCE 的
 *    地址+扩展、TYPEDEF_STRUCTURE 的字节大小），使不依赖桥接层的协议测试
 *    也能拿到运行时地址。
 *
 * 平台差异（Windows: CreateProcess/TerminateProcess；POSIX: fork/exec +
 * SIGTERM/SIGKILL）全部封装在实现文件中。
 */

#ifndef CALMCAR_XCP_TEST_XCPLITE_SLAVE_FIXTURE_HPP_
#define CALMCAR_XCP_TEST_XCPLITE_SLAVE_FIXTURE_HPP_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "libxcp/command_executor.hpp"
#include "libxcp/xcp_master.hpp"

namespace calmcar::xcp::test {

/// @brief 从 Slave A2L 文本解析出的符号寻址信息
struct XcpliteSymbolInfo {
    std::uint32_t address{0};   ///< ECU 地址（A2L ECU_ADDRESS）
    std::uint8_t extension{0};  ///< 地址扩展（A2L ECU_ADDRESS_EXTENSION）
};

/**
 * @brief XCPlite Slave 子进程控制器（不依赖 gtest，可独立使用）
 *
 * 生命周期：构造 → Start()（子进程标记与 XCP 会话探测均就绪）→ Stop()。
 * 析构自动 Stop（幂等）。禁止拷贝。
 */
class XcpliteSlaveProcess {
public:
    /**
     * @brief 构造子进程控制器
     * @param first_candidate_port 可选首选端口，仅用于确定性端口冲突测试
     * @param first_attempt_startup_delay_ms 首次启动延迟，仅用于就绪竞态测试
     */
    explicit XcpliteSlaveProcess(
        std::optional<std::uint16_t> first_candidate_port = std::nullopt,
        std::uint32_t first_attempt_startup_delay_ms = 0);
    ~XcpliteSlaveProcess();

    XcpliteSlaveProcess(const XcpliteSlaveProcess&) = delete;
    XcpliteSlaveProcess& operator=(const XcpliteSlaveProcess&) = delete;

    /**
     * @brief 启动 Slave 并等待就绪
     * @details 自动选择端口（避开并发 ctest 任务冲突）；Slave 因端口占用
     *          早退时顺延换端口重试；就绪要求子进程写入本次启动标记且
     *          一次完整的 CONNECT+DISCONNECT 成功。超时抛出 std::runtime_error。
     * @throws std::runtime_error 无法启动 Slave 可执行文件或探测超时
     */
    void Start();

    /// @brief 终止 Slave 进程并回收（幂等，可安全重复调用）
    void Stop();

    /// @brief Slave 进程是否仍在运行
    [[nodiscard]] bool IsRunning() const;

    /// @brief Slave 实际监听的 UDP 端口（Start() 之后有效）
    [[nodiscard]] std::uint16_t Port() const;

    /// @brief Slave 的工作目录（A2L/AML 落盘位置）
    [[nodiscard]] const std::filesystem::path& WorkDir() const;

    /**
     * @brief Slave 运行时生成的 A2L 路径
     * @details 文件名由 Slave 固定为 <project>.a2l（WRITE_ALWAYS 模式）；
     *          Start() 成功后若文件不存在会抛异常（尽早暴露环境问题）。
     * @throws std::runtime_error A2L 文件缺失
     */
    [[nodiscard]] std::filesystem::path A2lPath() const;

private:
    /// @brief 平台相关实现（进程句柄、端口、工作目录等）
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief 轻量 A2L 文本扫描：解析 MEASUREMENT / INSTANCE 的 ECU 地址与扩展
 * @param a2l_path A2L 文件路径
 * @param symbol 符号名（XCPlite 按变量名注册，无 MODULE:: 前缀）
 * @return 解析结果；未找到或格式不含地址扩展时 nullopt
 * @details 只扫描 Slave 自己生成的规整单行格式（XCPlite 的每个
 *          MEASUREMENT/INSTANCE 占一行），不是一般的 A2L 解析器；
 *          地址取 ECU_ADDRESS_EXTENSION 前一个 token（MEASUREMENT 的
 *          `ECU_ADDRESS 0xNN` 与 INSTANCE 的裸地址在该位置同构）。
 */
[[nodiscard]] std::optional<XcpliteSymbolInfo> ResolveA2lSymbol(
    const std::filesystem::path& a2l_path, const std::string& symbol);

/**
 * @brief 轻量 A2L 文本扫描：解析 TYPEDEF_STRUCTURE 的字节大小
 * @param a2l_path A2L 文件路径
 * @param typedef_name 类型名
 * @return 大小（字节）；未找到时 nullopt
 */
[[nodiscard]] std::optional<std::uint32_t> ResolveA2lTypedefSize(
    const std::filesystem::path& a2l_path, const std::string& typedef_name);

/**
 * @brief gtest 基类夹具：为每个用例提供独立的 XCPlite Slave 进程
 * @details SetUp 启动 Slave（含就绪探测），TearDown 终止；用例内用
 *          MakeConnectedMaster() 建立真实的 UDP 会话。
 */
class XcpliteSlaveTest : public ::testing::Test {
protected:
    /// @brief 启动 Slave；失败即 FAIL（环境问题不静默）
    void SetUp() override;

    /// @brief 终止 Slave 进程
    void TearDown() override;

    /**
     * @brief 构造指向 Slave 的 Master（未连接）
     * @param listener 事件监听器（可选，非拥有；生命周期须长于返回的 Master）
     * @return 持有独立 UdpTransport 的 XcpMaster
     */
    [[nodiscard]] std::unique_ptr<XcpMaster> MakeMaster(
        IEventListener* listener = nullptr) const;

    /**
     * @brief 构造并已 Connect 的 Master（默认命令超时 2s，面向真实进程）
     * @param listener 事件监听器（可选，非拥有）
     * @return 完成建链的 XcpMaster（调用方负责 Disconnect/析构）
     */
    [[nodiscard]] std::unique_ptr<XcpMaster> MakeConnectedMaster(
        IEventListener* listener = nullptr) const;

    /// @brief Slave 进程句柄（用例可直接查询端口/A2L 路径）
    XcpliteSlaveProcess slave_;
};

}  // namespace calmcar::xcp::test

#endif  // CALMCAR_XCP_TEST_XCPLITE_SLAVE_FIXTURE_HPP_
