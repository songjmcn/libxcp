/**
 * @file xcp_master.hpp
 * @brief XCP Master 顶层门面：用户唯一入口。
 *
 * 依据 code-plan/XCP_1.3.0_详细设计_类接口与头文件.md 第 14 节实现。
 */

#ifndef CALMCAR_XCP_XCP_MASTER_HPP_
#define CALMCAR_XCP_XCP_MASTER_HPP_

#include <functional>
#include <memory>
#include <optional>

#include "libxcp/command_executor.hpp"
#include "libxcp/ixcp_transport.hpp"
#include "libxcp/memory_access.hpp"
#include "libxcp/session.hpp"
#include "libxcp/xcp_error.hpp"

namespace calmcar::xcp {

/**
 * @brief Seed→Key 算法回调（Seed&Key，批次 7）
 *
 * XCP 不规定算法，A2L 的 SEED_AND_KEY_EXTERNAL_FUNCTION 仅给出供应商函数名；
 * 本库以回调注入方式由调用方提供算法（可为函数、lambda 或捕获上下文的可调用
 * 对象）。参数顺序与规范 §9.2 的 XCP_ComputeKeyFromSeed 一致：先特权资源、
 * 后 Seed。
 * @note seed 按 XCP Packet 实际传输顺序原样传入，返回的 Key 字节同样按传输
 *       顺序、禁止按本机字节序重排（docs/XCP_1.3.0_document.md §9.2）。
 */
using SeedKeyCalculator =
    std::function<Bytes(Resource resource, BytesView seed)>;

/**
 * @brief XcpMaster::Unlock 的执行结果（Seed&Key，批次 7）
 */
struct UnlockResult {
    /// @brief true = GET_SEED 返回 Length 0，资源本就未保护（未发送 UNLOCK）
    bool was_already_unlocked{false};
    /// @brief UNLOCK 末帧响应的 Current Resource Protection Status
    /// @details 资源本就未保护时为 std::nullopt——GET_SEED 响应协议上不含该
    ///          字段，不伪造值；需要权威保护掩码请调用 QueryStatus()。
    std::optional<ResourceMask> resource_protection;
};

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
     * @details Transport.Open -> CONNECT -> [GET_COMM_MODE_INFO] -> GET_STATUS
     * @throws XcpException 连接失败（失败时已关闭通道并清理本地状态）
     */
    void Connect();

    /**
     * @brief 断开 XCP 连接
     * @details DISCONNECT -> Transport.Close。即使 DISCONNECT
     * 失败也释放本地资源， 并向调用方抛出原始错误；重复断开在本地幂等。
     */
    void Disconnect();

    /// @brief 是否已连接
    [[nodiscard]] bool IsConnected() const;

    // ---- 内存读取 ----

    /**
     * @brief 读取内存（以字节为单位）
     * @param address 32 位地址
     * @param extension 地址扩展
     * @param byte_count 字节数（必须可被 AG 整除）
     */
    [[nodiscard]] Bytes ReadMemoryBytes(Address address,
                                        AddressExtension extension,
                                        ByteCount byte_count);

    /**
     * @brief 读取内存（以元素为单位，按 AG 计数）
     * @details 与字节版本分开命名：ByteCount 与 ElementCount 同为 uint32_t
     * 别名， 若共用 ReadMemory 名称会导致字面量调用产生重载歧义。
     */
    [[nodiscard]] Bytes ReadMemory(Address address, AddressExtension extension,
                                   ElementCount element_count);

    // ---- Seed&Key 解锁（批次 7）----

    /**
     * @brief 解锁受 Seed&Key 保护的单个资源
     * @param resource 要解锁的资源（必须恰为 CAL/PAG、DAQ、STIM、PGM 之一）
     * @param calculator Seed→Key 算法回调（不可为空）
     * @return 解锁结果（本就未解锁短路 / UNLOCK 末帧保护掩码）
     * @throws XcpException(InvalidArgument) resource 非单资源位、回调为空、
     *         回调返回的 Key 为空或超过 255 字节（Length 字段上限）
     * @throws XcpException(ProtocolError) 协议错误；Key 校验失败时 Session
     *         转入 Failed（Slave 已主动断开，再次 Connect() 自动 Reset 重建）
     * @throws XcpException 超时或恢复失败
     * @details 编排流程：GET_SEED(First) -> [GET_SEED(Remainder)*] ->
     *          calculator(seed) -> [UNLOCK 分段]*，Seed/Key 按 MAX_CTO-2
     *          分段。计划 §6.4：ERR_ACCESS_LOCKED 只报告、不自动触发解锁，
     *          本方法必须由调用方显式调用。
     */
    [[nodiscard]] UnlockResult Unlock(Resource resource,
                                      const SeedKeyCalculator& calculator);

    // ---- 状态查询 ----

    /// @brief 获取 Session 参数快照
    [[nodiscard]] SessionParameters GetSessionParameters() const;

    /// @brief 获取当前 Session 状态
    [[nodiscard]] SessionState GetSessionState() const;

    /// @brief 手动查询 GET_STATUS 并更新 Session
    [[nodiscard]] GetStatusResponse QueryStatus();

private:
    std::unique_ptr<IXcpTransport> m_transport_;  ///< 拥有的 Transport
    Session m_session_;                           ///< 会话状态与协商参数
    std::unique_ptr<CommandExecutor>
        m_executor_;  ///< 命令执行器（同时是包监听器）
    std::unique_ptr<MemoryAccess> m_memory_access_;  ///< 内存访问
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_XCP_MASTER_HPP_
