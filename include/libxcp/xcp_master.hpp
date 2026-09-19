/**
 * @file xcp_master.hpp
 * @brief XCP Master 顶层门面：用户唯一入口。
 *
 * 依据 code-plan/XCP_1.3.0_详细设计_类接口与头文件.md 第 14 节实现。
 */

#ifndef CALMCAR_XCP_XCP_MASTER_HPP_
#define CALMCAR_XCP_XCP_MASTER_HPP_

#include <memory>

#include "libxcp/command_executor.hpp"
#include "libxcp/ixcp_transport.hpp"
#include "libxcp/memory_access.hpp"
#include "libxcp/session.hpp"
#include "libxcp/xcp_error.hpp"

namespace calmcar::xcp {

/**
 * @brief XCP Master 顶层门面
 *
 * 内部组合 Session、CommandExecutor、MemoryAccess，并持有 Transport 所有权。
 * 典型调用序列见设计文档附录 B.1。
 */
class XcpMaster {
public:
    /**
     * @brief 构造
     * @param transport Transport 实例（XcpMaster 接管所有权）
     * @param timeouts 命令超时配置
     * @param event_listener 事件监听器（可选，非拥有；生命周期须长于本对象）
     */
    explicit XcpMaster(std::unique_ptr<IXcpTransport> transport,
                       CommandTimeouts timeouts = {},
                       IEventListener* event_listener = nullptr);

    /// @brief 析构：尽力断开逻辑会话并关闭 Transport
    ~XcpMaster();

    // 禁止拷贝
    XcpMaster(const XcpMaster&) = delete;
    XcpMaster& operator=(const XcpMaster&) = delete;

    // ---- 连接管理 ----

    /**
     * @brief 建立 XCP 连接
     * @details Transport.open -> CONNECT -> [GET_COMM_MODE_INFO] -> GET_STATUS
     * @throws XcpException 连接失败（失败时已关闭通道并清理本地状态）
     */
    void connect();

    /**
     * @brief 断开 XCP 连接
     * @details DISCONNECT -> Transport.close。即使 DISCONNECT
     * 失败也释放本地资源， 并向调用方抛出原始错误；重复断开在本地幂等。
     */
    void disconnect();

    /// @brief 是否已连接
    [[nodiscard]] bool isConnected() const;

    // ---- 内存读取 ----

    /**
     * @brief 读取内存（以字节为单位）
     * @param byte_count 字节数（必须可被 AG 整除）
     */
    [[nodiscard]] Bytes readMemory(Address address, AddressExtension extension,
                                   ByteCount byte_count);

    /**
     * @brief 读取内存（以元素为单位，按 AG 计数）
     * @details 与字节重载分开命名：ByteCount 与 ElementCount 同为 uint32_t
     * 别名， 若共用 readMemory 名称会导致字面量调用产生重载歧义。
     */
    [[nodiscard]] Bytes readMemoryElements(Address address,
                                           AddressExtension extension,
                                           ElementCount element_count);

    // ---- 状态查询 ----

    /// @brief 获取 Session 参数快照
    [[nodiscard]] SessionParameters sessionParameters() const;

    /// @brief 获取当前 Session 状态
    [[nodiscard]] SessionState sessionState() const;

    /// @brief 手动查询 GET_STATUS 并更新 Session
    [[nodiscard]] GetStatusResponse queryStatus();

private:
    std::unique_ptr<IXcpTransport> m_transport_;  ///< 拥有的 Transport
    Session m_session_;                           ///< 会话状态与协商参数
    std::unique_ptr<CommandExecutor>
        m_executor_;  ///< 命令执行器（同时是包监听器）
    std::unique_ptr<MemoryAccess> m_memory_access_;  ///< 内存访问
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_XCP_MASTER_HPP_
