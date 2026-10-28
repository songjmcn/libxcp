/// @file socket_ops_win.cpp
/// @brief Windows Winsock 套接字系统调用适配。
#include "socket_ops.hpp"
#ifdef _WIN32
#include "../../event_poller.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <algorithm>
#include <bit>
#include <climits>
#include <cstring>
#include <memory>
#include <utility>

namespace calmcar::xcp::detail {
namespace {
/// @brief 验证并转换统一句柄为 Winsock 句柄。
SOCKET ToNative(SocketHandle handle) {
    static_assert(sizeof(SOCKET) == sizeof(SocketHandle));
    return std::bit_cast<SOCKET>(handle);
}
/// @brief 转换 Winsock 句柄并保留 INVALID_SOCKET 哨兵。
SocketHandle FromNative(SOCKET socket) {
    static_assert(sizeof(SOCKET) == sizeof(SocketHandle));
    return socket == INVALID_SOCKET ? kInvalidSocketHandle
                                    : std::bit_cast<SocketHandle>(socket);
}
/// @brief 将 IPv4 POD 端点转换为系统地址。
sockaddr_in ToNativeAddress(const Ipv4Endpoint& endpoint) {
    sockaddr_in result{};
    result.sin_family = AF_INET;
    result.sin_port = htons(endpoint.port);
    std::memcpy(&result.sin_addr.s_addr, endpoint.address.data(),
                endpoint.address.size());
    return result;
}
/// @brief 从系统地址提取网络顺序字节和主机序端口。
Ipv4Endpoint FromNativeAddress(const sockaddr_in& native) {
    Ipv4Endpoint result{};
    result.port = ntohs(native.sin_port);
    std::memcpy(result.address.data(), &native.sin_addr.s_addr,
                result.address.size());
    return result;
}
/// @brief Winsock I/O 长度限制为 int 最大值。
int LimitSize(std::size_t size) {
    return static_cast<int>(std::min(size, static_cast<std::size_t>(INT_MAX)));
}
/// @brief Windows 系统错误码格式化。
std::string FormatError(int code) {
    char buffer[256]{};
    ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                     nullptr, static_cast<DWORD>(code), 0, buffer,
                     sizeof(buffer), nullptr);
    return "WSAErr=" + std::to_string(code) + " " + buffer;
}
/// @brief 经 EventPoller 单句柄等待，错误文本从返回值捕获。
int WaitSingle(SocketHandle handle, bool writable, std::uint32_t timeout_ms,
               std::string* error_out) {
    const WaitItem item{handle, !writable, writable};
    std::vector<WaitStatus> status;
    std::string error;
    const int result =
        CurrentEventPoller().Wait({&item, 1}, timeout_ms, status, &error);
    if (result < 0 && error_out) *error_out = std::move(error);
    return result;
}
/// @brief WSAStartup/WSACleanup 成对管理，静态局部变量初始化线程安全。
class WinsockSession {
public:
    /// @brief 初始化一次 Winsock 并记录实际返回错误。
    WinsockSession() { error_ = ::WSAStartup(MAKEWORD(2, 2), &data_); }
    /// @brief 在真实操作实例销毁时配平 Winsock 初始化。
    ~WinsockSession() {
        if (error_ == 0) ::WSACleanup();
    }
    /// @brief 返回初始化错误码，零表示成功。
    [[nodiscard]] int error() const { return error_; }

private:
    WSADATA data_{};  ///< Winsock 版本信息。
    int error_{};     ///< 初始化错误码。
};
/// @brief Windows 平台套接字操作实现。
class WinSocketOps final : public SocketOps {
public:
    SocketHandle create(SocketKind kind) override {
        if (session_.error() != 0) {
            ::WSASetLastError(session_.error());
            return kInvalidSocketHandle;
        }
        return FromNative(::socket(
            AF_INET, kind == SocketKind::Tcp ? SOCK_STREAM : SOCK_DGRAM, 0));
    }
    void close(SocketHandle handle) override {
        if (handle != kInvalidSocketHandle) ::closesocket(ToNative(handle));
    }
    int bind(SocketHandle handle, const Ipv4Endpoint& endpoint) override {
        const auto native = ToNativeAddress(endpoint);
        return ::bind(ToNative(handle),
                      reinterpret_cast<const sockaddr*>(&native),
                      sizeof(native));
    }
    int getsockname(SocketHandle handle, Ipv4Endpoint* endpoint) override {
        sockaddr_in native{};
        int length = sizeof(native);
        const int result = ::getsockname(
            ToNative(handle), reinterpret_cast<sockaddr*>(&native), &length);
        if (result == 0 && endpoint) *endpoint = FromNativeAddress(native);
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
            if (error_out)
                *error_out = "getaddrinfo=" + std::to_string(error) + " " +
                             ::gai_strerrorA(error);
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
        u_long flag = 1;
        return ::ioctlsocket(ToNative(handle), FIONBIO, &flag);
    }
    int set_blocking(SocketHandle handle) override {
        u_long flag = 0;
        return ::ioctlsocket(ToNative(handle), FIONBIO, &flag);
    }
    int listen(SocketHandle handle, int backlog) override {
        return ::listen(ToNative(handle), backlog);
    }
    SocketHandle accept(SocketHandle handle, Ipv4Endpoint* endpoint) override {
        sockaddr_in native{};
        int length = sizeof(native);
        const SOCKET accepted = ::accept(
            ToNative(handle), reinterpret_cast<sockaddr*>(&native), &length);
        if (accepted != INVALID_SOCKET && endpoint)
            *endpoint = FromNativeAddress(native);
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
        int length = sizeof(value);
        const int result =
            ::getsockopt(ToNative(handle), SOL_SOCKET, SO_ERROR,
                         reinterpret_cast<char*>(&value), &length);
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
                return 0;  // Winsock 不产生 SIGPIPE。
        }
        const int value = enabled ? 1 : 0;
        return ::setsockopt(ToNative(handle), level, name,
                            reinterpret_cast<const char*>(&value),
                            sizeof(value));
    }
    int set_receive_timeout(SocketHandle handle,
                            std::uint32_t timeout_ms) override {
        const DWORD timeout = timeout_ms;
        return ::setsockopt(ToNative(handle), SOL_SOCKET, SO_RCVTIMEO,
                            reinterpret_cast<const char*>(&timeout),
                            sizeof(timeout));
    }
    int send(SocketHandle handle, const std::uint8_t* data, std::size_t size,
             bool) override {
        return ::send(ToNative(handle), reinterpret_cast<const char*>(data),
                      LimitSize(size), 0);
    }
    int recv(SocketHandle handle, std::uint8_t* data,
             std::size_t size) override {
        return ::recv(ToNative(handle), reinterpret_cast<char*>(data),
                      LimitSize(size), 0);
    }
    int send_to(SocketHandle handle, const std::uint8_t* data, std::size_t size,
                const Ipv4Endpoint& endpoint) override {
        const auto native = ToNativeAddress(endpoint);
        return ::sendto(ToNative(handle), reinterpret_cast<const char*>(data),
                        LimitSize(size), 0,
                        reinterpret_cast<const sockaddr*>(&native),
                        sizeof(native));
    }
    int recv_from(SocketHandle handle, std::uint8_t* data, std::size_t size,
                  Ipv4Endpoint* endpoint) override {
        sockaddr_in native{};
        int length = sizeof(native);
        const int result = ::recvfrom(
            ToNative(handle), reinterpret_cast<char*>(data), LimitSize(size), 0,
            reinterpret_cast<sockaddr*>(&native), &length);
        if (result >= 0 && endpoint) *endpoint = FromNativeAddress(native);
        return result;
    }
    int shutdown_socket(SocketHandle handle) override {
        return ::shutdown(ToNative(handle), SD_BOTH);
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
        return FormatError(::WSAGetLastError());
    }
    [[nodiscard]] bool is_interrupted() const override {
        return ::WSAGetLastError() == WSAEINTR;
    }
    [[nodiscard]] bool is_would_block() const override {
        return ::WSAGetLastError() == WSAEWOULDBLOCK;
    }
    [[nodiscard]] bool is_in_progress() const override {
        const int error = ::WSAGetLastError();
        return error == WSAEINPROGRESS || error == WSAEWOULDBLOCK;
    }
    [[nodiscard]] bool is_connection_lost() const override {
        return IsConnectionLostCode(::WSAGetLastError());
    }
    [[nodiscard]] bool is_timeout() const override {
        return ::WSAGetLastError() == WSAETIMEDOUT;
    }
    [[nodiscard]] bool is_invalid_socket() const override {
        // UDP 的历史退出分类还覆盖 WSAEINVAL（如关闭中的无效参数）。
        const int error = ::WSAGetLastError();
        return error == WSAENOTSOCK || error == WSAEINVAL;
    }

private:
    WinsockSession session_;  ///< 线程安全静态实例拥有的 Winsock 生命周期。
};
}  // namespace

bool IsConnectionLostCode(int code) noexcept {
    return code == WSAECONNRESET || code == WSAECONNABORTED ||
           code == WSAENOTCONN || code == WSAESHUTDOWN;
}
SocketOps& RealSocketOps() {
    static WinSocketOps instance;
    return instance;
}
}  // namespace calmcar::xcp::detail
#endif  // _WIN32
