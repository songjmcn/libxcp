/// @file socket_ops_posix.cpp
/// @brief Linux/macOS 套接字系统调用适配。
#include "socket_ops.hpp"
#ifndef _WIN32
#include "../../event_poller.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <memory>
#include <utility>

namespace calmcar::xcp::detail {
namespace {
/// @brief 将平台句柄提升为统一句柄。
SocketHandle FromNative(int socket) {
    return static_cast<SocketHandle>(socket);
}
/// @brief 将统一句柄转换为平台整数句柄。
int ToNative(SocketHandle handle) { return static_cast<int>(handle); }
/// @brief 将 POD 端点转换为平台地址。
sockaddr_in ToNativeAddress(const Ipv4Endpoint& endpoint) {
    sockaddr_in result{};
    result.sin_family = AF_INET;
    result.sin_port = htons(endpoint.port);
    std::memcpy(&result.sin_addr.s_addr, endpoint.address.data(),
                endpoint.address.size());
    return result;
}
/// @brief 将平台地址转换为 POD 端点。
Ipv4Endpoint FromNativeAddress(const sockaddr_in& native) {
    Ipv4Endpoint result{};
    result.port = ntohs(native.sin_port);
    std::memcpy(result.address.data(), &native.sin_addr.s_addr,
                result.address.size());
    return result;
}
/// @brief 安全限制 I/O 长度到平台 int 返回值范围。
std::size_t LimitSize(std::size_t size) {
    return std::min(size, static_cast<std::size_t>(INT_MAX));
}
/// @brief 以 EventPoller 等待，并保留失败文字而非读取过期 errno。
int WaitSingle(SocketHandle handle, bool writable, std::uint32_t timeout_ms,
               std::string* error_out) {
    const WaitItem item{handle, !writable, writable};
    std::vector<WaitStatus> status;
    std::string error;
    const int result =
        CurrentEventPoller().Wait({&item, 1}, timeout_ms, status, &error);
    if (result < 0 && error_out != nullptr) *error_out = std::move(error);
    return result;
}
/// @brief POSIX 套接字操作实现。
class PosixSocketOps final : public SocketOps {
public:
    SocketHandle create(SocketKind kind) override {
        return FromNative(::socket(
            AF_INET, kind == SocketKind::Tcp ? SOCK_STREAM : SOCK_DGRAM, 0));
    }
    void close(SocketHandle handle) override {
        if (handle != kInvalidSocketHandle) ::close(ToNative(handle));
    }
    int bind(SocketHandle handle, const Ipv4Endpoint& endpoint) override {
        const auto native = ToNativeAddress(endpoint);
        return ::bind(ToNative(handle),
                      reinterpret_cast<const sockaddr*>(&native),
                      sizeof(native));
    }
    int getsockname(SocketHandle handle, Ipv4Endpoint* endpoint) override {
        sockaddr_in native{};
        socklen_t length = sizeof(native);
        const int result = ::getsockname(
            ToNative(handle), reinterpret_cast<sockaddr*>(&native), &length);
        if (result == 0 && endpoint != nullptr)
            *endpoint = FromNativeAddress(native);
        return result;
    }
    int resolve_ipv4(const char* node, const char* service, SocketKind kind,
                     std::vector<Ipv4Endpoint>* endpoints,
                     std::string* error_out) override {
        if (endpoints == nullptr) {
            if (error_out) *error_out = "IPv4 candidate output is null";
            return EAI_FAIL;
        }
        endpoints->clear();
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = kind == SocketKind::Tcp ? SOCK_STREAM : SOCK_DGRAM;
        addrinfo* results = nullptr;
        const int error = ::getaddrinfo(node, service, &hints, &results);
        if (error != 0) {
            if (error_out) *error_out = ::gai_strerror(error);
            return error;
        }
        const std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> result_guard(
            results, &::freeaddrinfo);
        for (const addrinfo* item = results; item != nullptr;
             item = item->ai_next) {
            if (item->ai_family == AF_INET &&
                item->ai_addrlen >= sizeof(sockaddr_in)) {
                endpoints->push_back(FromNativeAddress(
                    *reinterpret_cast<const sockaddr_in*>(item->ai_addr)));
            }
        }
        if (endpoints->empty()) {
            if (error_out) *error_out = "DNS returned no IPv4 candidates";
            return EAI_FAIL;
        }
        return 0;
    }
    int set_non_blocking(SocketHandle handle) override {
        return SetBlocking(handle, true);
    }
    int set_blocking(SocketHandle handle) override {
        return SetBlocking(handle, false);
    }
    int listen(SocketHandle handle, int backlog) override {
        return ::listen(ToNative(handle), backlog);
    }
    SocketHandle accept(SocketHandle handle, Ipv4Endpoint* endpoint) override {
        sockaddr_in native{};
        socklen_t length = sizeof(native);
        const int accepted = ::accept(
            ToNative(handle), reinterpret_cast<sockaddr*>(&native), &length);
        if (accepted >= 0 && endpoint) *endpoint = FromNativeAddress(native);
        return FromNative(accepted);
    }
    int connect(SocketHandle handle, const Ipv4Endpoint& endpoint) override {
        const auto native = ToNativeAddress(endpoint);
        return ::connect(ToNative(handle),
                         reinterpret_cast<const sockaddr*>(&native),
                         sizeof(native));
    }
    int get_so_error(SocketHandle handle, int* error_out) override {
        int value = 0;
        socklen_t length = sizeof(value);
        const int result = ::getsockopt(ToNative(handle), SOL_SOCKET, SO_ERROR,
                                        &value, &length);
        if (result == 0 && error_out) *error_out = value;
        return result;
    }
    int set_option(SocketHandle handle, SocketOption option,
                   bool enabled) override {
        int level = SOL_SOCKET;
        int name = SO_REUSEADDR;
        switch (option) {
            case SocketOption::ReuseAddress:
                break;
            case SocketOption::KeepAlive:
                name = SO_KEEPALIVE;
                break;
            case SocketOption::NoDelay:
                level = IPPROTO_TCP;
                name = TCP_NODELAY;
                break;
            case SocketOption::NoSigPipe:
#ifdef SO_NOSIGPIPE
                name = SO_NOSIGPIPE;
                break;
#else
                return 0;  // Linux 使用 MSG_NOSIGNAL，无对应 socket 选项。
#endif
        }
        const int value = enabled ? 1 : 0;
        return ::setsockopt(ToNative(handle), level, name, &value,
                            sizeof(value));
    }
    int set_receive_timeout(SocketHandle handle,
                            std::uint32_t timeout_ms) override {
        timeval timeout{};
        timeout.tv_sec =
            static_cast<decltype(timeout.tv_sec)>(timeout_ms / 1000);
        timeout.tv_usec =
            static_cast<decltype(timeout.tv_usec)>((timeout_ms % 1000) * 1000);
        return ::setsockopt(ToNative(handle), SOL_SOCKET, SO_RCVTIMEO, &timeout,
                            sizeof(timeout));
    }
    int send(SocketHandle handle, const std::uint8_t* data, std::size_t size,
             bool no_signal) override {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        if (no_signal) flags = MSG_NOSIGNAL;
#else
        (void)no_signal;
#endif
        return static_cast<int>(
            ::send(ToNative(handle), data, LimitSize(size), flags));
    }
    int recv(SocketHandle handle, std::uint8_t* data,
             std::size_t size) override {
        return static_cast<int>(
            ::recv(ToNative(handle), data, LimitSize(size), 0));
    }
    int send_to(SocketHandle handle, const std::uint8_t* data, std::size_t size,
                const Ipv4Endpoint& endpoint) override {
        const auto native = ToNativeAddress(endpoint);
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        return static_cast<int>(::sendto(
            ToNative(handle), data, LimitSize(size), flags,
            reinterpret_cast<const sockaddr*>(&native), sizeof(native)));
    }
    int recv_from(SocketHandle handle, std::uint8_t* data, std::size_t size,
                  Ipv4Endpoint* endpoint) override {
        sockaddr_in native{};
        socklen_t length = sizeof(native);
        const int result = static_cast<int>(
            ::recvfrom(ToNative(handle), data, LimitSize(size), 0,
                       reinterpret_cast<sockaddr*>(&native), &length));
        if (result >= 0 && endpoint) *endpoint = FromNativeAddress(native);
        return result;
    }
    int shutdown_socket(SocketHandle handle) override {
        return ::shutdown(ToNative(handle), SHUT_RDWR);
    }
    int wait_writable(SocketHandle handle, std::uint32_t timeout_ms,
                      std::string* error_out) override {
        return WaitSingle(handle, true, timeout_ms, error_out);
    }
    int wait_readable(SocketHandle handle, std::uint32_t timeout_ms,
                      std::string* error_out) override {
        return WaitSingle(handle, false, timeout_ms, error_out);
    }
    [[nodiscard]] std::string last_error() const override {
        const int code = errno;
        return "errno=" + std::to_string(code) + " " + std::strerror(code);
    }
    [[nodiscard]] bool is_interrupted() const override {
        return errno == EINTR;
    }
    [[nodiscard]] bool is_would_block() const override {
        return errno == EAGAIN || errno == EWOULDBLOCK;
    }
    [[nodiscard]] bool is_in_progress() const override {
        return errno == EINPROGRESS;
    }
    [[nodiscard]] bool is_connection_lost() const override {
        return IsConnectionLostCode(errno);
    }
    [[nodiscard]] bool is_timeout() const override {
        return errno == ETIMEDOUT || errno == EAGAIN || errno == EWOULDBLOCK;
    }
    [[nodiscard]] bool is_invalid_socket() const override {
        return errno == EBADF || errno == ENOTSOCK;
    }

private:
    /// @brief 使用 fcntl 切换阻塞模式。
    int SetBlocking(SocketHandle handle, bool non_blocking) {
        const int flags = ::fcntl(ToNative(handle), F_GETFL, 0);
        if (flags < 0) return -1;
        return ::fcntl(ToNative(handle), F_SETFL,
                       non_blocking ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
    }
};
}  // namespace

bool IsConnectionLostCode(int code) noexcept {
    return code == EPIPE || code == ECONNRESET || code == ECONNABORTED ||
           code == ENOTCONN || code == ESHUTDOWN;
}
SocketOps& RealSocketOps() {
    static PosixSocketOps instance;
    return instance;
}
}  // namespace calmcar::xcp::detail
#endif  // !_WIN32
