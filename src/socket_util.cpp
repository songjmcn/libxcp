/// @file socket_util.cpp
/// @brief 无平台套接字测试注入和 IPv4 文本处理。
#include "socket_util.hpp"

#include <atomic>
#include <charconv>
#include <system_error>

namespace calmcar::xcp::detail {
namespace {
/// @brief 测试套接字替身指针，读取采用原子操作。
std::atomic<SocketOps*> g_test_socket_ops{nullptr};
/// @brief 测试名称解析函数指针，读取采用原子操作。
std::atomic<NameResolver> g_test_name_resolver{nullptr};
}  // namespace

SocketOps& CurrentSocketOps() noexcept {
    SocketOps* const injected =
        g_test_socket_ops.load(std::memory_order_acquire);
    return injected ? *injected : RealSocketOps();
}

void SetSocketOpsForTesting(SocketOps* ops) noexcept {
    g_test_socket_ops.store(ops, std::memory_order_release);
}

bool ParseIpv4Literal(const std::string& text, Ipv4Address* out) {
    if (out == nullptr) return false;
    Ipv4Address parsed{};
    std::size_t start = 0;
    for (std::size_t i = 0; i < parsed.size(); ++i) {
        const auto end = text.find('.', start);
        if ((end == std::string::npos) != (i == parsed.size() - 1))
            return false;
        const auto stop = end == std::string::npos ? text.size() : end;
        if (stop == start || stop - start > 3) return false;
        // 与原 inet_pton 的严格字面量校验一致，禁止多位十进制段以 0 开头。
        if (stop - start > 1 && text[start] == '0') return false;
        unsigned value = 0;
        const char* const begin = text.data() + start;
        const char* const finish = text.data() + stop;
        const auto result = std::from_chars(begin, finish, value);
        if (result.ec != std::errc{} || result.ptr != finish || value > 255)
            return false;
        parsed[i] = static_cast<std::uint8_t>(value);
        start = stop + 1;
    }
    *out = parsed;
    return true;
}

std::string FormatIpv4(const Ipv4Address& address) {
    return std::to_string(address[0]) + "." + std::to_string(address[1]) + "." +
           std::to_string(address[2]) + "." + std::to_string(address[3]);
}

bool LooksLikeIpv6(const std::string& text) {
    return text.find(':') != std::string::npos;
}

void SetNameResolverForTest(NameResolver resolver) {
    g_test_name_resolver.store(resolver, std::memory_order_release);
}

NameResolver CurrentNameResolver() {
    return g_test_name_resolver.load(std::memory_order_acquire);
}

int ResolveIpv4Candidates(const char* node, const char* service,
                          SocketKind kind, std::vector<Ipv4Endpoint>* endpoints,
                          std::string* error_out) {
    if (endpoints == nullptr) {
        if (error_out != nullptr) *error_out = "IPv4 candidate output is null";
        return -1;
    }
    endpoints->clear();
    if (error_out != nullptr) error_out->clear();

    const NameResolver resolver = CurrentNameResolver();
    const int result = resolver != nullptr
                           ? resolver(node, service, kind, endpoints, error_out)
                           : RealSocketOps().resolve_ipv4(node, service, kind,
                                                          endpoints, error_out);
    if (result != 0 || endpoints->empty()) {
        endpoints->clear();
        if (error_out != nullptr && error_out->empty()) {
            *error_out = result == 0 ? "DNS returned no IPv4 candidates"
                                     : "IPv4 candidate resolution failed";
        }
        return result == 0 ? -1 : result;
    }
    if (error_out != nullptr) error_out->clear();
    return 0;
}
}  // namespace calmcar::xcp::detail
