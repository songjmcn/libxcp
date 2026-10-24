/**
 * @file socket_util.hpp
 * @brief internal 跨平台 Socket 工具与**可注入** SocketOps 函数表。
 *
 * 依据 code-plan/TCP传输_实施计划_B-T1至B-T5.md 第 5.6 节实现（B-T2）。
 *
 * @par 为什么需要可注入函数表
 *   计划 §5.6 明确要求 partial write、EINTR、EAGAIN 与 timeout 的测试必须
 *   **确定性**运行。用真实 Socket 触发这些条件极不可靠：
 *   - 真实 TCP 的内核发送缓冲通常能一次吞下小报文，partial write 难以复现；
 *   - EINTR/EAGAIN 依赖信号与缓冲状态的时序，天然不可稳定制造；
 *   - 发送超时测试会真的等待若干秒，拖慢整个测试套件。
 *
 *   因此所有 Socket 调用都经过 `SocketOps`：生产路径使用 `RealSocketOps()`，
 *   测试路径注入一个按脚本行事的假实现，从而精确、即时地复现每一条错误分支。
 *
 * @note 本文件位于 `src/`，属于**内部实现**头，不对外安装。
 */

#ifndef CALMCAR_XCP_SOCKET_UTIL_HPP_
#define CALMCAR_XCP_SOCKET_UTIL_HPP_

#include <cstddef>
#include <cstdint>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>  // addrinfo / getaddrinfo：Windows 侧由 ws2tcpip.h 传递带入
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace calmcar::xcp::detail {

#if defined(_WIN32)
/// @brief 平台 Socket 句柄类型（Windows 为 `SOCKET`，POSIX 为 `int`）
using SocketHandle = SOCKET;

/// @brief 无效 Socket 句柄哨兵
inline constexpr SocketHandle kInvalidSocketHandle = INVALID_SOCKET;
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocketHandle = -1;
#endif

/// @brief `shutdown()` 的"双向关闭"平台常量
/// @details POSIX 取 `SHUT_RDWR`，Windows 取 `SD_BOTH`。两者**不可混用**：
///          Winsock 头不定义 `SHUT_RDWR`，POSIX 头也不定义 `SD_BOTH`，
///          写成单个常量会让另一平台直接编译失败（Windows CI 已踩过）。
#if defined(_WIN32)
inline constexpr int kShutdownBoth = SD_BOTH;
#else
inline constexpr int kShutdownBoth = SHUT_RDWR;
#endif

/**
 * @brief Socket 操作函数表（生产实现 + 测试替身共用同一接口）
 *
 * @details 每个成员对应一个平台 Socket API。返回值约定与底层 API 一致：
 *          - 成功返回 `>= 0`（`recv`/`send` 返回字节数，其余返回 0）；
 *          - 失败返回 `-1`，错误细节由 `last_error()` 提供。
 *
 * @note `wait_writable` / `wait_readable` 返回 1 表示就绪、0 表示超时、
 *       -1 表示错误。采用该三态返回值而非 `bool`，是为了让调用方能够把
 *       "超时" 与 "故障" 区分开——二者在 TCP 上的处理方式完全不同
 *       （超时可重试/报 send timeout，故障要断链）。
 */
struct SocketOps {
    /// @brief 虚析构，允许测试派生
    virtual ~SocketOps() = default;

    /// @brief 创建 Socket；失败返回 `kInvalidSocketHandle`
    virtual SocketHandle create(int domain, int type, int protocol) = 0;

    /// @brief 关闭 Socket
    virtual void close(SocketHandle handle) = 0;

    /// @brief 绑定本地地址
    virtual int bind(SocketHandle handle, const sockaddr* addr,
                     socklen_t addr_len) = 0;

    /// @brief 查询本地实际地址（用于回填 OS 分配的临时端口）
    virtual int getsockname(SocketHandle handle, sockaddr* addr,
                            socklen_t* addr_len) = 0;

    /// @brief 解析主机名与服务（`getaddrinfo`）
    virtual int getaddrinfo(const char* node, const char* service,
                            const addrinfo* hints, addrinfo** result) = 0;

    /// @brief 释放 `getaddrinfo` 结果
    virtual void freeaddrinfo(addrinfo* result) = 0;

    /// @brief 把 Socket 设为非阻塞
    virtual int set_non_blocking(SocketHandle handle) = 0;

    /// @brief 把 Socket 恢复为阻塞模式
    virtual int set_blocking(SocketHandle handle) = 0;

    /// @brief 开始监听（仅 Slave 角色使用）
    virtual int listen(SocketHandle handle, int backlog) = 0;

    /// @brief 接受一个入站连接
    /// @return 新连接的句柄；失败返回 `kInvalidSocketHandle`
    virtual SocketHandle accept(SocketHandle handle, sockaddr* addr,
                                socklen_t* addr_len) = 0;

    /// @brief 发起连接（非阻塞下通常返回 `-1` 且错误为 in-progress）
    virtual int connect(SocketHandle handle, const sockaddr* addr,
                        socklen_t addr_len) = 0;

    /// @brief 读取 `SO_ERROR`，用于判定非阻塞 connect 的最终结果
    virtual int get_so_error(SocketHandle handle, int* error_out) = 0;

    /// @brief 设置整数型 Socket 选项
    virtual int set_int_option(SocketHandle handle, int level, int option,
                               int value) = 0;

    /// @brief 发送数据；非阻塞下可能返回短写
    virtual int send(SocketHandle handle, const std::uint8_t* data,
                     std::size_t size, bool no_signal) = 0;

    /// @brief 接收数据；对端关闭返回 0
    virtual int recv(SocketHandle handle, std::uint8_t* data,
                     std::size_t size) = 0;

    /// @brief 半关闭/关闭连接方向，用于立即打断阻塞中的 send/recv
    virtual int shutdown_socket(SocketHandle handle) = 0;

    /// @brief 等待可写
    /// @return 1 就绪、0 超时、-1 错误
    virtual int wait_writable(SocketHandle handle, std::uint32_t timeout_ms) = 0;

    /// @brief 等待可读
    /// @return 1 就绪、0 超时、-1 错误
    virtual int wait_readable(SocketHandle handle, std::uint32_t timeout_ms) = 0;

    /// @brief 最近一次失败的平台错误文本（用于日志与异常明细）
    [[nodiscard]] virtual std::string last_error() const = 0;

    /// @brief 错误是否表示 "操作被信号打断"（EINTR / WSAEINTR）
    [[nodiscard]] virtual bool is_interrupted() const = 0;

    /// @brief 错误是否表示 "当前无法完成，稍后重试"（EAGAIN/EWOULDBLOCK）
    [[nodiscard]] virtual bool is_would_block() const = 0;

    /// @brief 错误是否表示 "连接正在建立中"（EINPROGRESS）
    [[nodiscard]] virtual bool is_in_progress() const = 0;

    /// @brief 错误是否表示 "对端已关闭/连接已断"（EPIPE/ECONNRESET/...）
    [[nodiscard]] virtual bool is_connection_lost() const = 0;
};

/**
 * @brief 取得生产环境的 Socket 操作函数表（进程内单例）
 * @return 指向真实平台实现的引用，永不为空
 *
 * @note 单例无状态，可安全跨线程共享。
 */
[[nodiscard]] SocketOps& RealSocketOps();

/**
 * @brief 判断平台错误码是否表示 "连接已丢失/对端已关闭"
 * @param code 平台错误码（POSIX 为 `errno`，Windows 为 `WSAGetLastError()`）
 * @return 表示连接不可继续使用返回 true
 *
 * @details 抽成独立函数而非埋在 `SocketOps` 里，是为了让**判定集合本身**
 *          可以在两个平台上被单元测试直接覆盖。历史上这里误用过
 *          `WSAEPIPE`——Winsock 并没有这个错误码（`EPIPE` 只是 MSVC CRT 的
 *          `errno`），只有 Windows 编译才会暴露，故必须可测。
 *
 * @note Windows 侧没有与 POSIX `EPIPE` 等价的 WSA 码：对端关闭在 Windows 上
 *       表现为 `WSAECONNRESET` / `WSAECONNABORTED`，本地已 shutdown 时为
 *       `WSAESHUTDOWN`，均已包含在判定集合内，因此删除 `WSAEPIPE` 不损失判据。
 */
[[nodiscard]] bool IsConnectionLostCode(int code) noexcept;

/**
 * @brief 把点分 IPv4 文本解析为网络序地址
 * @param text IPv4 文本（如 `"127.0.0.1"`）
 * @param out 输出地址；仅在返回 true 时被写入
 * @return 解析成功返回 true
 */
[[nodiscard]] bool ParseIpv4Literal(const std::string& text, in_addr* out);

/**
 * @brief 判断文本是否为 IPv6 字面量
 * @param text 主机文本（不含端口）
 * @return 含 `':'` 返回 true
 *
 * @details 用于能力边界：本库只承诺 IPv4，IPv6 必须**显式识别并拒绝**为
 *          `UnsupportedFeature`，而不是静默降级成笼统的 "地址非法"。
 */
[[nodiscard]] bool LooksLikeIpv6(const std::string& text);

/**
 * @brief 名称解析可注入钩子（仅测试使用）
 *
 * @details DNS 失败与多 candidate 的行为无法在生产环境确定性复现（真实
 *          `getaddrinfo` 依赖外部 DNS）。测试通过 `SetNameResolverForTest()`
 *          安装自己的解析器，精确控制返回的候选列表。
 */
using NameResolver = int (*)(const char* node, const char* service,
                             const addrinfo* hints, addrinfo** result,
                             std::string* error_out);

/// @brief 设置测试用名称解析器；传 `nullptr` 恢复生产实现
void SetNameResolverForTest(NameResolver resolver);

/// @brief 取当前生效的名称解析器
[[nodiscard]] NameResolver CurrentNameResolver();

}  // namespace calmcar::xcp::detail

#endif  // CALMCAR_XCP_SOCKET_UTIL_HPP_