#ifndef CALMCAR_XCP_DETAIL_SOCKET_SOCKET_OPS_HPP_
#define CALMCAR_XCP_DETAIL_SOCKET_SOCKET_OPS_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace calmcar::xcp::detail {

/// @brief 跨平台套接字句柄。
using SocketHandle = std::intptr_t;
/// @brief 无效套接字句柄。
inline constexpr SocketHandle kInvalidSocketHandle = -1;
/// @brief 按网络字节顺序排列的 IPv4 四个字节。
using Ipv4Address = std::array<std::uint8_t, 4>;

/// @brief IPv4 端点，端口采用主机字节序。
struct Ipv4Endpoint {
    Ipv4Address address{};  ///< 网络字节顺序的地址。
    std::uint16_t port{};   ///< 主机字节顺序的端口。
};

/// @brief 套接字传输种类。
enum class SocketKind { Tcp, Udp };
/// @brief 平台无关套接字选项。
enum class SocketOption { ReuseAddress, KeepAlive, NoDelay, NoSigPipe };

/// @brief 可注入的平台套接字操作接口；失败返回 -1 或无效句柄。
struct SocketOps {
    /// @brief 虚析构，允许测试替身。
    virtual ~SocketOps() = default;
    /// @brief 创建指定传输种类的 IPv4 套接字。
    virtual SocketHandle create(SocketKind kind) = 0;
    /// @brief 关闭套接字。
    virtual void close(SocketHandle handle) = 0;
    /// @brief 绑定本地端点。
    virtual int bind(SocketHandle handle, const Ipv4Endpoint& endpoint) = 0;
    /// @brief 查询本地端点。
    virtual int getsockname(SocketHandle handle, Ipv4Endpoint* endpoint) = 0;
    /// @brief 解析 IPv4 候选端点。
    virtual int resolve_ipv4(const char* node, const char* service, SocketKind kind,
                             std::vector<Ipv4Endpoint>* endpoints, std::string* error_out) = 0;
    /// @brief 设置非阻塞模式。
    virtual int set_non_blocking(SocketHandle handle) = 0;
    /// @brief 恢复阻塞模式。
    virtual int set_blocking(SocketHandle handle) = 0;
    /// @brief 监听传入连接。
    virtual int listen(SocketHandle handle, int backlog) = 0;
    /// @brief 接受连接并回填远端地址。
    virtual SocketHandle accept(SocketHandle handle, Ipv4Endpoint* endpoint) = 0;
    /// @brief 连接到远端端点。
    virtual int connect(SocketHandle handle, const Ipv4Endpoint& endpoint) = 0;
    /// @brief 查询非阻塞连接最终错误码。
    virtual int get_so_error(SocketHandle handle, int* error_out) = 0;
    /// @brief 设置布尔套接字选项。
    virtual int set_option(SocketHandle handle, SocketOption option, bool enabled) = 0;
    /// @brief 设置接收超时，单位毫秒。
    virtual int set_receive_timeout(SocketHandle handle, std::uint32_t timeout_ms) = 0;
    /// @brief 发送数据；请求长度由平台实现安全钳制。
    virtual int send(SocketHandle handle, const std::uint8_t* data, std::size_t size,
                     bool no_signal = true) = 0;
    /// @brief 接收数据；请求长度由平台实现安全钳制。
    virtual int recv(SocketHandle handle, std::uint8_t* data, std::size_t size) = 0;
    /// @brief 发送 UDP 数据报。
    virtual int send_to(SocketHandle handle, const std::uint8_t* data, std::size_t size,
                        const Ipv4Endpoint& endpoint) = 0;
    /// @brief 接收 UDP 数据报并回填远端地址。
    virtual int recv_from(SocketHandle handle, std::uint8_t* data, std::size_t size,
                          Ipv4Endpoint* endpoint) = 0;
    /// @brief 双向关闭连接而不释放句柄。
    virtual int shutdown_socket(SocketHandle handle) = 0;
    /// @brief 等待可写；返回 1 就绪、0 超时、-1 失败。
    virtual int wait_writable(SocketHandle handle, std::uint32_t timeout_ms,
                              std::string* error_out = nullptr) = 0;
    /// @brief 等待可读；返回 1 就绪、0 超时、-1 失败。
    virtual int wait_readable(SocketHandle handle, std::uint32_t timeout_ms,
                              std::string* error_out = nullptr) = 0;
    /// @brief 最近一次系统套接字错误文本。
    [[nodiscard]] virtual std::string last_error() const = 0;
    /// @brief 最近一次错误是否为中断。
    [[nodiscard]] virtual bool is_interrupted() const = 0;
    /// @brief 最近一次错误是否为暂不可用。
    [[nodiscard]] virtual bool is_would_block() const = 0;
    /// @brief 最近一次错误是否为连接进行中。
    [[nodiscard]] virtual bool is_in_progress() const = 0;
    /// @brief 最近一次错误是否为连接丢失。
    [[nodiscard]] virtual bool is_connection_lost() const = 0;
    /// @brief 最近一次错误是否为超时。
    [[nodiscard]] virtual bool is_timeout() const = 0;
    /// @brief 最近一次错误是否为无效句柄。
    [[nodiscard]] virtual bool is_invalid_socket() const = 0;
};

/// @brief 取得真实平台套接字实现。
[[nodiscard]] SocketOps& RealSocketOps();
/// @brief 取得注入实现或真实平台实现。
[[nodiscard]] SocketOps& CurrentSocketOps() noexcept;
/// @brief 设置测试替身；空指针恢复真实实现。
void SetSocketOpsForTesting(SocketOps* ops) noexcept;
/// @brief 判断平台错误码是否表示连接丢失。
[[nodiscard]] bool IsConnectionLostCode(int code) noexcept;
/// @brief 解析 IPv4 字面量为网络顺序字节。
[[nodiscard]] bool ParseIpv4Literal(const std::string& text, Ipv4Address* out);
/// @brief 将网络顺序的 IPv4 地址格式化为点分字面量。
[[nodiscard]] std::string FormatIpv4(const Ipv4Address& address);
/// @brief 判断主机文本是否可能为 IPv6 字面量。
[[nodiscard]] bool LooksLikeIpv6(const std::string& text);
/// @brief 测试注入的 POD 解析函数指针类型。
using NameResolver = int (*)(const char*, const char*, SocketKind,
                             std::vector<Ipv4Endpoint>*, std::string*);
/// @brief 设置测试解析器；空指针恢复系统解析。
void SetNameResolverForTest(NameResolver resolver);
/// @brief 查询当前测试解析器，未注入则为空。
[[nodiscard]] NameResolver CurrentNameResolver();
/// @brief 经测试钩子或真实平台解析 IPv4 候选地址。
int ResolveIpv4Candidates(const char* node, const char* service, SocketKind kind,
                          std::vector<Ipv4Endpoint>* endpoints, std::string* error_out);

}  // namespace calmcar::xcp::detail
#endif  // CALMCAR_XCP_DETAIL_SOCKET_SOCKET_OPS_HPP_
