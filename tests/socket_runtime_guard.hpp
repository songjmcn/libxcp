/**
 * @file socket_runtime_guard.hpp
 * @brief 测试侧 Socket 运行时的前置保障（Windows 必须先 WSAStartup）。
 *
 * @details
 * Windows 上与 POSIX 有一个不可忽略的差异：调用 `::socket()` 之前必须已经执行过
 * `WSAStartup()`，否则 `::socket()` 直接返回 `INVALID_SOCKET`。
 *
 * 生产代码不需要关心这一点——所有 Socket 都经由 `detail::RealSocketOps()` 创建，
 * 而该函数表实现的构造函数内部就持有 `WSAStartup`/`WSACleanup` 的引用计数。
 *
 * 但若干测试（`tests/tcp_transport_test.cpp`、`tests/tcp_fake_socket_test.cpp`）
 * 为了扮演"裸对端"，会**直接用** `::socket()`/`::bind()`/`::connect()`，
 * 绕过了函数表。此时若进程里还没有任何代码触碰过 `RealSocketOps()`，
 * Winsock 便处于未初始化状态，表现为 `StartRawListener()` 返回 0、
 * `m_listen_handle == kRawInvalid` 之类的失败（Linux/macOS 上不会出现）。
 *
 * 因此这些测试在创建裸 Socket 之前先调用 `EnsureSocketRuntime()`：
 * 它借用 `RealSocketOps()` 的构造副作用完成 `WSAStartup`，该单例活到进程结束，
 * 引用计数不会被提前释放。
 */

#ifndef CALMCAR_XCP_TESTS_SOCKET_RUNTIME_GUARD_HPP_
#define CALMCAR_XCP_TESTS_SOCKET_RUNTIME_GUARD_HPP_

#include "src/socket_util.hpp"

namespace calmcar::xcp::test_support {

/**
 * @brief 确保进程级 Socket 运行时已初始化（可重复调用，幂等）。
 *
 * @details POSIX 上无副作用；Windows 上首次调用执行 `WSAStartup`。
 *          调用后即可安全使用 `::socket()` 等原生 Socket API。
 */
inline void EnsureSocketRuntime() {
    // 借用函数表单例的构造副作用：它内部持有 WSAStartup/WSACleanup 引用计数。
    static_cast<void>(detail::RealSocketOps());
}

}  // namespace calmcar::xcp::test_support

#endif  // CALMCAR_XCP_TESTS_SOCKET_RUNTIME_GUARD_HPP_
