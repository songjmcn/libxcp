/**
 * @file socket_util.cpp
 * @brief `socket_util.hpp` 的生产实现（真实平台 Socket 调用）。
 *
 * 依据 code-plan/TCP传输_实施计划_B-T1至B-T5.md 第 5.6 节实现（B-T2）。
 *
 * @par 平台差异集中在本文件
 *   句柄类型、非阻塞设置方式、等待原语、错误码判定与 SIGPIPE 规避策略
 *   四类差异全部收敛于此，`tcp_transport.cpp` 只使用抽象后的三态返回值。
 */

#include "socket_util.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>

#if defined(_WIN32)
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace calmcar::xcp::detail {
namespace {

#if defined(_WIN32)

/**
 * @brief Windows 进程内 WSAStartup/WSACleanup 引用计数
 *
 * @details Winsock 要求每个使用者配平 Startup/Cleanup。本类用引用计数保证
 *          多个 Transport 实例并存时只初始化一次、全部销毁后清理一次。
 */
class WinsockSession {
public:
    /// @brief 首个实例执行 WSAStartup，其余仅增加引用计数
    WinsockSession() {
        if (s_ref_count_ == 0) {
            WSADATA data;
            s_ok_ = (::WSAStartup(MAKEWORD(2, 2), &data) == 0);
        } else {
            s_ok_ = true;
        }
        ++s_ref_count_;
    }

    /// @brief 最后一个实例析构时执行 WSACleanup
    ~WinsockSession() {
        if (--s_ref_count_ == 0 && s_ok_) {
            ::WSACleanup();
        }
    }

    WinsockSession(const WinsockSession&) = delete;
    WinsockSession& operator=(const WinsockSession&) = delete;

    /// @brief 初始化是否成功
    [[nodiscard]] bool ok() const noexcept { return s_ok_; }

private:
    static inline int s_ref_count_{0};  ///< 活动引用数
    static inline bool s_ok_{false};    ///< WSAStartup 结果
};

/// @brief 把 WinSock 错误码格式化为可读文本
std::string formatWsaError(int code) {
    char buffer[256] = {0};
    ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                     nullptr, static_cast<DWORD>(code), 0, buffer,
                     static_cast<DWORD>(sizeof(buffer)), nullptr);
    return "WSAErr=" + std::to_string(code) + " " + std::string(buffer);
}

/// @brief 把 POSIX errno 格式化为可读文本
std::string formatErrno(int code) {
    return "errno=" + std::to_string(code) + " " + std::strerror(code);
}

/// @brief 取当前线程最近一次 Socket 错误码
int lastSocketErrorCode() { return static_cast<int>(WSAGetLastError()); }

/**
 * @brief 等待句柄进入指定状态
 * @param handle 目标句柄
 * @param writable true 等可写、false 等可读
 * @param timeout_ms 超时（毫秒）
 * @return 1 就绪、0 超时、-1 错误
 */
int waitFor(SocketHandle handle, bool writable, std::uint32_t timeout_ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(handle, &set);

    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout_ms / 1000U);
    tv.tv_usec = static_cast<long>((timeout_ms % 1000U) * 1000U);

    // select 的第一个参数在 Windows 上被忽略，传 0 即可
    const int ready = ::select(0, writable ? nullptr : &set,
                               writable ? &set : nullptr, nullptr, &tv);
    if (ready < 0) {
        return -1;
    }
    return ready == 0 ? 0 : 1;
}

#else  // POSIX

/// @brief 把 POSIX errno 格式化为可读文本
std::string formatErrno(int code) {
    return "errno=" + std::to_string(code) + " " + std::strerror(code);
}

/// @brief 取当前线程最近一次 Socket 错误码
int lastSocketErrorCode() { return errno; }

/**
 * @brief 等待句柄进入指定状态（poll 版本，避免 fd_set 的 FD_SETSIZE 上限）
 * @return 1 就绪、0 超时、-1 错误
 */
int waitFor(SocketHandle handle, bool writable, std::uint32_t timeout_ms) {
    pollfd pfd{};
    pfd.fd = handle;
    pfd.events = static_cast<short>(writable ? POLLOUT : POLLIN);

    // poll 的 timeout 是 int；上限钳制到 24 天，足以覆盖任何合理配置
    const auto clamped = std::min<std::uint32_t>(timeout_ms, 0x7FFFFFFFU);
    const int ready = ::poll(&pfd, 1, static_cast<int>(clamped));
    if (ready < 0) {
        return -1;
    }
    if (ready == 0) {
        return 0;
    }
    // POLLERR/POLLHUP 也算"就绪"：由调用方的 SO_ERROR / recv 读取真实结果，
    // 这样断链不会被误报成"超时"。
    return 1;
}

#endif

/**
 * @brief 真实平台 SocketOps 实现
 *
 * @details 每个方法都在失败时保持 `errno`/`WSAGetLastError()` 原样，使
 *          `last_error()` 能取到与失败调用对应的错误码。**注意**：任何
 *          中间调用（如日志）都可能覆盖该错误码，因此调用方必须在失败后
 *          **立即**读取 `last_error()`，不得先做其它操作。
 */
class RealSocketOpsImpl final : public SocketOps {
public:
#if defined(_WIN32)
    /// @brief 首个实例确保 Winsock 已初始化
    RealSocketOpsImpl() {
        if (!m_winsock.ok()) {
            m_winsock_ok = false;
        }
    }
#endif

    SocketHandle create(int domain, int type, int protocol) override {
        return ::socket(domain, type, protocol);
    }

    void close(SocketHandle handle) override {
#if defined(_WIN32)
        ::closesocket(handle);
#else
        ::close(handle);
#endif
    }

    int bind(SocketHandle handle, const sockaddr* addr,
             socklen_t addr_len) override {
        return ::bind(handle, addr, static_cast<socklen_t>(addr_len));
    }

    int getsockname(SocketHandle handle, sockaddr* addr,
                    socklen_t* addr_len) override {
        return ::getsockname(handle, addr, addr_len);
    }

    int getaddrinfo(const char* node, const char* service,
                    const addrinfo* hints, addrinfo** result) override {
        return ::getaddrinfo(node, service, hints, result);
    }

    void freeaddrinfo(addrinfo* result) override { ::freeaddrinfo(result); }

    int set_non_blocking(SocketHandle handle) override {
#if defined(_WIN32)
        u_long non_blocking = 1;
        return ::ioctlsocket(handle, FIONBIO, &non_blocking);
#else
        const int flags = ::fcntl(handle, F_GETFL, 0);
        if (flags < 0) {
            return -1;
        }
        return ::fcntl(handle, F_SETFL, flags | O_NONBLOCK);
#endif
    }

    int set_blocking(SocketHandle handle) override {
#if defined(_WIN32)
        u_long non_blocking = 0;
        return ::ioctlsocket(handle, FIONBIO, &non_blocking);
#else
        const int flags = ::fcntl(handle, F_GETFL, 0);
        if (flags < 0) {
            return -1;
        }
        return ::fcntl(handle, F_SETFL, flags & ~O_NONBLOCK);
#endif
    }

    int listen(SocketHandle handle, int backlog) override {
        return ::listen(handle, backlog);
    }

    SocketHandle accept(SocketHandle handle, sockaddr* addr,
                        socklen_t* addr_len) override {
        return ::accept(handle, addr, addr_len);
    }

    int connect(SocketHandle handle, const sockaddr* addr,
                socklen_t addr_len) override {
        return ::connect(handle, addr, static_cast<socklen_t>(addr_len));
    }

    int get_so_error(SocketHandle handle, int* error_out) override {
        int value = 0;
        socklen_t len = sizeof(value);
        const int rc = ::getsockopt(
            handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&value),
            &len);
        if (rc == 0 && error_out != nullptr) {
            *error_out = value;
        }
        return rc;
    }

    int set_int_option(SocketHandle handle, int level, int option,
                       int value) override {
        return ::setsockopt(handle, level, option,
                            reinterpret_cast<const char*>(&value),
                            sizeof(value));
    }

    int send(SocketHandle handle, const std::uint8_t* data, std::size_t size,
             bool no_signal) override {
        // MSG_NOSIGNAL 是 Linux 专有；macOS 用 SO_NOSIGPIPE 选项（见 SetNoSigpipe）
        int flags = 0;
#if defined(MSG_NOSIGNAL)
        if (no_signal) {
            flags |= MSG_NOSIGNAL;
        }
#else
        (void)no_signal;  // Windows 的 send 本身不产生 SIGPIPE
#endif
        return static_cast<int>(
            ::send(handle, reinterpret_cast<const char*>(data),
                   static_cast<int>(size), flags));
    }

    int recv(SocketHandle handle, std::uint8_t* data,
             std::size_t size) override {
        return static_cast<int>(::recv(handle,
                                       reinterpret_cast<char*>(data),
                                       static_cast<int>(size), 0));
    }

    int shutdown_socket(SocketHandle handle) override {
        // 双向关闭：立即让阻塞中的 send/recv 返回，但**不**释放句柄号。
        // 常量按平台取（POSIX SHUT_RDWR / Winsock SD_BOTH），见 kShutdownBoth。
        return ::shutdown(handle, kShutdownBoth);
    }

    int wait_writable(SocketHandle handle, std::uint32_t timeout_ms) override {
        return waitFor(handle, true, timeout_ms);
    }

    int wait_readable(SocketHandle handle, std::uint32_t timeout_ms) override {
        return waitFor(handle, false, timeout_ms);
    }

    [[nodiscard]] std::string last_error() const override {
        return formatErrno(lastSocketErrorCode());
    }

    [[nodiscard]] bool is_interrupted() const override {
        const int code = lastSocketErrorCode();
#if defined(_WIN32)
        return code == WSAEINTR;
#else
        return code == EINTR;
#endif
    }

    [[nodiscard]] bool is_would_block() const override {
        const int code = lastSocketErrorCode();
#if defined(_WIN32)
        return code == WSAEWOULDBLOCK;
#else
        return code == EAGAIN || code == EWOULDBLOCK;
#endif
    }

    [[nodiscard]] bool is_in_progress() const override {
        const int code = lastSocketErrorCode();
#if defined(_WIN32)
        return code == WSAEWOULDBLOCK || code == WSAEINPROGRESS;
#else
        return code == EINPROGRESS;
#endif
    }

    [[nodiscard]] bool is_connection_lost() const override {
        return IsConnectionLostCode(lastSocketErrorCode());
    }

#if defined(_WIN32)
    /// @brief Winsock 是否可用（供 Open 早期失败判定）
    [[nodiscard]] bool winsock_ok() const noexcept { return m_winsock_ok; }

private:
    WinsockSession m_winsock;                ///< RAII 引用 WSAStartup/WSACleanup
    bool m_winsock_ok{true};                 ///< 初始化结果
#endif
};

/// @brief 测试用名称解析器（默认为空，表示走生产实现）
NameResolver g_test_name_resolver = nullptr;

}  // namespace

bool IsConnectionLostCode(int code) noexcept {
#if defined(_WIN32)
    // Winsock 无 WSAEPIPE：对端关闭表现为 CONNRESET/CONNABORTED，
    // 本地已 shutdown 后为 SHUTDOWN，判定集合不因此缺失任何情形。
    return code == WSAECONNRESET || code == WSAECONNABORTED ||
           code == WSAENOTCONN || code == WSAESHUTDOWN;
#else
    return code == EPIPE || code == ECONNRESET || code == ECONNABORTED ||
           code == ENOTCONN || code == ESHUTDOWN;
#endif
}

SocketOps& RealSocketOps() {
    static RealSocketOpsImpl instance;
    return instance;
}

namespace {

/**
 * @brief 测试注入的替代实现；`nullptr` 表示"未注入，使用真实实现"。
 * @details 用原子指针而非加锁：读取在每条 Socket 调用路径上发生，
 *          必须是无等待的；写入只发生在测试的安装/卸载两个时刻。
 */
std::atomic<SocketOps*> g_test_socket_ops{nullptr};

}  // namespace

SocketOps& CurrentSocketOps() noexcept {
    SocketOps* const override_ops =
        g_test_socket_ops.load(std::memory_order_acquire);
    if (override_ops != nullptr) {
        return *override_ops;
    }
    return RealSocketOps();
}

void SetSocketOpsForTesting(SocketOps* ops) noexcept {
    g_test_socket_ops.store(ops, std::memory_order_release);
}

bool ParseIpv4Literal(const std::string& text, in_addr* out) {
    if (text.empty() || out == nullptr) {
        return false;
    }
    return ::inet_pton(AF_INET, text.c_str(), out) == 1;
}

bool LooksLikeIpv6(const std::string& text) {
    // IPv4 点分十进制与主机名都不含冒号；含冒号即按 IPv6 对待
    return text.find(':') != std::string::npos;
}

void SetNameResolverForTest(NameResolver resolver) {
    g_test_name_resolver = resolver;
}

NameResolver CurrentNameResolver() { return g_test_name_resolver; }

}  // namespace calmcar::xcp::detail