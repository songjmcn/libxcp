# TCP/UDP 平台隔离重构 —— 实施计划（POD 接口 + 平台分文件）

- 文档版本：v2（2026-10-26，增补 §8.3 多平台验证；v1 为初版）
- 状态：**待批准**
- 范围：`src/socket_util.hpp`、`src/socket_util.cpp`、`src/tcp_transport.cpp`、`src/udp_transport.cpp`、`CMakeLists.txt`、`tests/tcp_fake_socket.hpp`、`tests/tcp_transport_test.cpp`
- 不在范围：公共头 `include/libxcp/**` 的 ABI/签名、framing/CTR 逻辑、`thirdparty/**`

---

## §0 结论速览

现有实现把平台差异用 `#if defined(_WIN32)` 分散在 **4 个文件共 29 处**，且内部头 `src/socket_util.hpp` 直接 include `winsock2.h` / `sys/socket.h`、接口签名使用 `sockaddr` / `socklen_t` / `in_addr`。这带来两个结构性代价：

1. **Windows 代码在本机永不被编译** —— 已连续踩中三次只在 Windows CI 暴露的缺陷（`SHUT_RDWR`、`WSAEPIPE`、`std::min` 被宏展开）。
2. **UDP 另起一套平台实现** —— `SocketHandle`、`WinsockSession`、`lastSocketError`、`inet_pton` 各有一份副本，任何平台差异要改两处。

本计划通过"**接口 POD 化 + 平台分文件**"两件事一起做来消除这两点：

- 接口只使用自有 POD（`SocketHandle = std::intptr_t`、`SocketAddress`），平台头文件只出现在平台实现文件内部；
- 实现按平台拆成 `socket_ops_win.cpp` / `socket_ops_posix.cpp`，由 CMake 选源，文件内零 `#if`；
- UDP 迁到同一套 `SocketOps` 上，删除其第二套实现；
- 加两条 CI 判据把"彻底隔离"变成可强制执行的规则。

预期结果：`src/` 下平台分支从 29 处降到个位数（平台实现文件内零 `#if`；残留仅 CMake 的 `if(WIN32)` 与 POSIX 内 macOS 的 `SO_NOSIGPIPE` 一处），`src/*.hpp` 不再 include 任何平台 Socket 头。

**验证前提已解决**：三平台（Windows / Linux / macOS）GitHub Actions 已实测可用，平台相关特性（编译期分支、运行时行为、常量集合、边界语义）均可由 CI 覆盖；仅**耗时类**用例不进 CI。详见 §8.3。

---

## §1 现状核证（全部为工具实测，行号以本文档写作时为准）

### 1.1 平台分支分布

| 文件 | 行数 | `_WIN32` 命中 | 性质 |
| --- | --- | --- | --- |
| `src/socket_util.hpp` | 244 | 3 | 头文件、句柄类型、`shutdown` 常量 |
| `src/socket_util.cpp` | 399 | 11 | 生产平台实现（`RealSocketOpsImpl`） |
| `src/udp_transport.cpp` | 943 | **13** | **第二套**平台实现（自带 `WinsockSession`） |
| `src/tcp_transport.cpp` | 927 | 2 | 平台头 include、macOS `SO_NOSIGPIPE` |

### 1.2 `src/socket_util.hpp` 的平台类型泄漏点

| 位置 | 内容 | 问题 |
| --- | --- | --- |
| `:27-37` | `#if defined(_WIN32)` → `winsock2.h` + `ws2tcpip.h`，否则 `netdb.h`/`netinet/in.h`/`sys/socket.h` | 任何 include 该头的 TU 都被平台污染 |
| `:41-50` | `using SocketHandle = SOCKET;` / `int`；`kInvalidSocketHandle` = `INVALID_SOCKET` / `-1` | 句柄类型与平台绑定 |
| `:52-60` | `kShutdownBoth` = `SD_BOTH` / `SHUT_RDWR` | 常量与平台绑定 |
| `:85-86` | `virtual int bind(SocketHandle, const sockaddr*, socklen_t)` | 接口签名泄漏 `sockaddr`/`socklen_t` |
| `:89-90` | `virtual int getsockname(SocketHandle, sockaddr*, socklen_t*)` | 同上 |
| `:93-97` | `getaddrinfo` / `freeaddrinfo`（形参 `addrinfo**`） | 同上 |
| `:213` | `ParseIpv4Literal(const std::string&, in_addr*)` | 泄漏 `in_addr` |
| `:232-240` | `NameResolver` 函数指针（`addrinfo**`） | 同上 |

### 1.3 `src/socket_util.cpp` 平台段

| 行号 | 内容 |
| --- | --- |
| `:18-26` | 平台头 + POSIX 额外 `poll.h` |
| `:31-110` | `WinsockSession`（RAII 引用计数 `WSAStartup`/`WSACleanup`）、`formatWsaError`、`lastSocketErrorCode()=WSAGetLastError()`、`waitFor()` 用 `select(0, …)` |
| `:112-145` | POSIX `formatErrno`、`lastSocketErrorCode()=errno`、`waitFor()` 用 `poll` |
| `:157-164` | Windows 构造器检查 Winsock 可用 |
| `:171-175` | `closesocket` / `close` |
| `:196-205` | `set_non_blocking`：`ioctlsocket(FIONBIO)` / `fcntl` |
| `:209-218` | `set_blocking`：同上 |
| `:258-264` | `MSG_NOSIGNAL` 分支 |
| `:297-320` | `is_interrupted` / `is_would_block` / `is_in_progress` 的 `WSAE*` 与 `E*` 分叉 |
| `:341-350` | `IsConnectionLostCode`：Windows 4 码 / POSIX 5 码 |

### 1.4 `src/udp_transport.cpp` 的第二套实现（13 处）

| 行号 | 内容 | 说明 |
| --- | --- | --- |
| `:20-24` | `winsock2.h` + `WIN32_LEAN_AND_MEAN` | 与 socket_util 重复 |
| `:41-45` | `using SocketHandle = SOCKET;` + `kInvalidSocket` + `lastSocketError` | **与 socket_util 重复定义** |
| `:86` | `class WinsockSession` | **第二份 WSA RAII** |
| `:124-128` | `inet_pton` 的 `#if`/`#else` **两侧代码完全相同** | 纯噪音 |
| `:176`、`:199` | `WinsockSession winsock;` 成员 | — |
| `:241-245` | 唤醒 socket `ioctlsocket(FIONBIO, 1)` | 应走 `set_non_blocking` |
| `:290-294` | 唤醒写入 `send(..., MSG_NOSIGNAL)` / `send(..., 0)` | 应走 `send(no_signal)` |
| `:304-308` | 唤醒读取 `recv` | 可直接复用 |
| `:399-403` | `winsock.ok()` + "UDP Winsock initialization failed" | 应统一 |
| `:464-468` | `SO_RCVTIMEO`：`DWORD`（毫秒）/ `timeval` | 类型差异，需接口化 |
| `:687-691` | `select(0, …)` / `select(FD_SETSIZE, …)` | 应走多句柄等待 |
| `:736-740` | `inet_ntop` 长度参数 `INET_ADDRSTRLEN` / `sizeof(buffer)` | 应走地址格式化 |

### 1.5 `src/tcp_transport.cpp`

- `:26-34`：平台头 include（经 `socket_util.hpp` 传递带入）。
- `:383-388`：`#if !defined(_WIN32) && defined(SO_NOSIGPIPE)` → macOS/BSD 用 `setsockopt(SO_NOSIGPIPE)`。**这是 TCP 传输层唯一的平台分支。**
- `SocketOps` 取用点：`:134`、`:284`、`:376`（`auto& ops = detail::CurrentSocketOps();`）。
- 依赖的平台类型：`:317`/`:332` 把 `sockaddr_in` 强转成 `const sockaddr*` / `sockaddr*` 传给 `ops.bind` / `ops.getsockname`。

### 1.6 现有辅助函数的使用面

| 函数 | 使用点 |
| --- | --- |
| `detail::LooksLikeIpv6` | `src/tcp_transport.cpp:194`、`:201` |
| `detail::ParseIpv4Literal` | `src/tcp_transport.cpp:293`、`:343` |

### 1.7 测试与构建

| 项 | 现状 |
| --- | --- |
| `tests/tcp_fake_socket.hpp` | `class FakeSocketOps final : public detail::SocketOps`（`:45`），**27 个 override**；`ScopedFakeSocketOps`（`:325`） |
| `tests/tcp_fake_socket_test.cpp` | 5 用例（partial write / EINTR / 等待超时 / 等待错误 / 连接重置），目标带 `RUN_SERIAL` |
| `tests/tcp_transport_test.cpp` | `SocketPlatformTest` 3 用例 + `static_assert(kShutdownBoth == SD_BOTH/SHUT_RDWR)` |
| `CMakeLists.txt:189-196` | `if(WIN32)` → `target_link_libraries(libxcp PUBLIC ws2_32)` + `target_compile_definitions(libxcp PRIVATE NOMINMAX)` |
| 公共头契约 | `include/libxcp/tcp_transport.hpp:151-152` 明确"不得把 `sockaddr`/`socklen_t` 这些平台 Socket 类型"暴露；`include/` 实测零平台类型 |

### 1.8 已发生的三次只在 Windows 暴露的缺陷

| 缺陷 | 表现 | 根因 |
| --- | --- | --- |
| `SHUT_RDWR` 未定义 | MSVC 编译失败 | POSIX 常量误用于 Windows |
| `WSAEPIPE` 未定义 | MSVC 编译失败 | Winsock 无此错误码（只有 CRT `EPIPE`） |
| `std::min` 被宏展开 | C2589/C2059 | `minwindef.h` 的 `min`/`max` 宏 |

三者都是"本机 macOS 编译通过、Windows CI 才炸"，属 §1.1/§1.2 结构的直接后果。

---

## §2 目标与非目标

### 2.1 目标

| 编号 | 目标 | 验收方式 |
| --- | --- | --- |
| G1 | `src/**/*.hpp` 不再 include 任何平台 Socket 头 | `grep -rn "winsock2.h\|ws2tcpip.h\|sys/socket.h\|netinet/in.h" src/ --include=*.hpp` 无命中 |
| G2 | `src/` 下除平台实现文件外零 `_WIN32` | `grep -rn "_WIN32" src/ --include=*.cpp --include=*.hpp --exclude=socket_ops_win.cpp` 无命中 |
| G3 | UDP 复用唯一一套 `SocketOps`，删除第二套实现 | `udp_transport.cpp` 中 `WinsockSession`/`SOCKET`/`ioctlsocket`/`inet_ntop` 归零 |
| G4 | 平台实现文件内零 `#if defined(_WIN32)`（编译期由 CMake 选源） | 人工核对 + CI |
| G5 | 平台差异可被 `FakeSocketOps` 注入测试（含 UDP 路径） | 新增注入用例 |
| G6 | 公共头契约不变 | `include/libxcp/**` 零改动 |

### 2.2 非目标（明确不做）

1. **不引入 Boost.Asio**（详见 §10）。
2. 不改 framing / CTR / 解码逻辑（纯搬迁）。
3. 不改 `include/libxcp/**` 的任何签名或 ABI。
4. 不改 `thirdparty/**`。
5. 不新增 IPv6 能力（保持"显式识别并拒绝"的现状）。
6. 不重构 `tests/`、`examples/`、`tools/` 里的平台分支（见 §9.3）。

---

## §3 目标结构

```
src/detail/socket/
├── socket_ops.hpp          # 平台无关：POD 类型 + SocketOps 接口 + 工厂声明
├── socket_ops_win.cpp      # Winsock 实现（含 WinsockSession RAII）
└── socket_ops_posix.cpp    # POSIX 实现（Linux/macOS）
```

CMake（根 `CMakeLists.txt`，替换现有 `if(WIN32)` 段）：

```cmake
if(WIN32)
  target_sources(libxcp PRIVATE src/detail/socket/socket_ops_win.cpp)
  target_link_libraries(libxcp PUBLIC ws2_32)
  target_compile_definitions(libxcp PRIVATE NOMINMAX)
else()
  target_sources(libxcp PRIVATE src/detail/socket/socket_ops_posix.cpp)
endif()
```

> `NOMINMAX` 与 `ws2_32` 属于**构建期**平台差异，无法消除，但已集中在根 CMakeLists 一处，符合"单点化"要求。

---

## §4 接口设计

### 4.1 POD 类型（`socket_ops.hpp`）

```cpp
namespace calmcar::xcp::detail {

/// @brief 平台 Socket 句柄的**不透明**表示
/// @details Windows 的 SOCKET 是 UINT_PTR，POSIX 是 int；两者都能无损放进
///          intptr_t。用 intptr_t 后头文件不再需要 winsock2.h。
using SocketHandle = std::intptr_t;

/// @brief 无效句柄哨兵
inline constexpr SocketHandle kInvalidSocketHandle = -1;

/// @brief 与平台无关的 Socket 地址（替代 sockaddr / sockaddr_in）
/// @details ip 为 16 字节：IPv4 用前 4 字节，IPv6 用全部 16 字节（当前仅用于
///          "识别并拒绝"，见 LooksLikeIpv6）。
struct SocketAddress {
    std::uint16_t family{0};   ///< 2=IPv4, 10=IPv6（内部标记，不代表平台 AF_ 值）
    std::uint16_t port{0};     ///< 主机字节序
    std::uint8_t  ip[16]{};    ///< 网络字节序地址
};

}  // namespace calmcar::xcp::detail
```

### 4.2 接口方法清单

现有 24 个方法全部保留，签名 POD 化；再新增 5 个（UDP 需要）+ 1 个（SIGPIPE 抽象）：

| # | 方法 | 变更 |
| --- | --- | --- |
| 1 | `create(domain, type, protocol)` | 不变 |
| 2 | `close(handle)` | 不变 |
| 3 | `bind(handle, const SocketAddress&)` | **签名改**（原 `const sockaddr*, socklen_t`） |
| 4 | `getsockname(handle, SocketAddress&)` | **签名改** |
| 5 | `getaddrinfo` / `freeaddrinfo` | **签名改**（改为返回 `std::vector<SocketAddress>`，不再暴露 `addrinfo`） |
| 6 | `set_non_blocking` / `set_blocking` | 不变 |
| 7 | `listen` / `accept(handle, SocketAddress&)` | `accept` 签名改 |
| 8 | `connect(handle, const SocketAddress&)` | 签名改 |
| 9 | `get_so_error` / `set_int_option` | 不变 |
| 10 | `send(handle, data, size, no_signal)` / `recv` | 不变 |
| 11 | `shutdown_socket(handle)` | 不变（`kShutdownBoth` 移入平台实现内部常量） |
| 12 | `wait_writable` / `wait_readable` | 不变 |
| 13 | `last_error` / `is_interrupted` / `is_would_block` / `is_in_progress` / `is_connection_lost` | 不变 |
| **14** | **`send_to(handle, const SocketAddress&, data, size)`** | 新增（UDP） |
| **15** | **`recv_from(handle, data, size, SocketAddress* source)`** | 新增（UDP 取源地址） |
| **16** | **`wait_readable_multi(const SocketHandle*, count, timeout_ms)`** | 新增（UDP 同时等数据 socket + 唤醒 socket） |
| **17** | **`set_receive_timeout_ms(handle, ms)`** | 新增（吸收 `DWORD`/`timeval` 差异） |
| **18** | **`address_to_text(const SocketAddress&)`** | 新增（吸收 `inet_ntop` 长度差异） |
| **19** | **`set_no_sigpipe(handle)`** | 新增（Linux 空实现 / macOS `SO_NOSIGPIPE` / Windows 空实现） |

自由函数调整为：

| 现状 | 变更后 |
| --- | --- |
| `ParseIpv4Literal(const std::string&, in_addr*)` | `ParseIpv4(const std::string&, SocketAddress&)` |
| `LooksLikeIpv6(const std::string&)` | 不变（不涉及平台类型） |
| `NameResolver`（`addrinfo**`） | 改为 `std::function<int(const char*, const char*, std::vector<SocketAddress>*, std::string*)>` |
| `RealSocketOps()` / `CurrentSocketOps()` / `SetSocketOpsForTesting()` | 签名不变 |

### 4.3 平台实现文件内部分工

| 文件 | 内容 | 平台分支 |
| --- | --- | --- |
| `socket_ops_win.cpp` | `WinsockSession`（RAII 引用计数）、`closesocket`、`ioctlsocket`、`select`、`WSAE*` 分类、`SO_RCVTIMEO`=`DWORD` | **零 `#if`**（整个文件只编 Windows） |
| `socket_ops_posix.cpp` | `close`、`fcntl`、`poll`、`errno` 分类、`MSG_NOSIGNAL`、`SO_RCVTIMEO`=`timeval` | 仅 `set_no_sigpipe()` 内一处 `#if defined(__APPLE__)` |

> 不把 POSIX 再拆成 linux/macos 两个文件：二者差异目前只有 SIGPIPE 处理一项，拆开带来的重复大于收益。若将来差异增多再拆。

---

## §5 关键技术决策

| 编号 | 决策 | 理由 | 否决策略与原因 |
| --- | --- | --- | --- |
| D1 | `SocketHandle = std::intptr_t` | 同时容纳 `SOCKET`(UINT_PTR) 与 `int`，且 `-1` 可继续表示无效句柄；头文件彻底去平台化 | 否决 `void*`：需额外转换且丢失"句柄是整数"的语义 |
| D2 | 自有 `SocketAddress` POD | `sockaddr`/`socklen_t` 是平台类型，无法在平台无关头中出现 | 否决"前向声明 `struct sockaddr`"：尺寸未知，无法按值传递 |
| D3 | 保留 `SocketOps` 虚接口 | 它是 `FakeSocketOps` 注入的唯一入口（5 个确定性用例依赖） | 否决"直接调平台 API"：会丢掉可测性 |
| D4 | 平台分文件 + CMake 选源 | 每个 TU 只编一个平台 → Windows 代码在 Windows 上真正参与编译 | 否决"继续 `#if`"：Windows 分支永不在本机编译，已致 3 次事故 |
| D5 | UDP 迁到同一套 `SocketOps` | 消除第二份 `WinsockSession`/句柄类型/错误分类 | 否决"保留两套"：任何平台差异要改两处 |
| D6 | POSIX 内保留一处 `__APPLE__` | Linux/macOS 的 SIGPIPE 机制不同（`MSG_NOSIGNAL` vs `SO_NOSIGPIPE`） | 否决"拆三文件"：重复成本高于收益 |

---

## §6 分阶段实施

每个阶段独立可验证，前一阶段验收未过不得进入下一阶段。

### P1 —— 接口 POD 化 + 接口一次性扩到 30 方法（原 P1 与 P3 接口部分合并，见 Q4）

- 新增 `src/detail/socket/socket_ops.hpp`（POD 类型 + **30 个方法**接口 + 自由函数 + 工厂声明）。
- `SocketOps` 一次性补齐 §4.2 的 #14–#19（`send_to`/`recv_from`/`wait_readable_multi`/`set_receive_timeout_ms`/`address_to_text`/`set_no_sigpipe`）；`ParseIpv4`、`NameResolver` 的自由函数签名一并迁入（Q3）。
- 把 `src/socket_util.cpp` 的实现适配新签名（**仍是单文件、仍有 `#if`**）——本阶段只改接口形状，不搬平台代码。
- `src/socket_util.hpp` 改为**只转发**（`#include "detail/socket/socket_ops.hpp"`，保留旧名兼容）。
- 同步修改调用点：`src/tcp_transport.cpp`（`:194/:201/:293/:317/:332/:343`）、`tests/tcp_fake_socket.hpp`（27 → 30 个 override）、`tests/tcp_transport_test.cpp`。
- **同时给 macOS / Windows 补 `Test` 步骤**（§8.3.5）——P2 的验收门依赖它，不能等到 P4。
- 交付测试：T1（`SocketAddressRoundTripsIpv4`）、T2（`ParsesIpv4TextIntoPod`）；`udp_transport.cpp` 本阶段不动。
- **验收门 G-P1**：本地构建通过；`ctest -E "^Example" -j 4` 全绿（基线 1046 项）；`SocketPlatformTest`、`TcpFakeSocketTest`、`UdpBoundaryFraming` 全通过；**三平台 CI 的 Build + Test 全绿**。

### P2 —— 拆平台实现文件

- 实现拆为 `socket_ops_win.cpp` / `socket_ops_posix.cpp`；CMake 选源。
- `socket_ops.hpp` 中删除平台 include（G1 达成）。
- 删除 `src/socket_util.cpp`（或保留为极薄的工厂转发）。
- **验收门 G-P2**：G1、G2、G4 的 grep 判据通过；全量测试同 P1；**`build-windows` 与 `build-macos` 的 Build + Test 均绿**（本机无法验证，见 §8.3 / §9.1）。

### P3 —— UDP 合并（接口已在 P1 就位）

- `src/udp_transport.cpp` 改用 `detail::CurrentSocketOps()`，删除自身 `WinsockSession`（`:86`）、`SocketHandle`（`:41-45`）、`inet_pton` 分叉（`:124-128`）、`ioctlsocket`（`:241`）、`select` 分叉（`:687`）、`inet_ntop`（`:736`），约 −250 行。
- 把 `src/tcp_transport.cpp:383-388` 的 `SO_NOSIGPIPE` 改为调用 `SocketOps::set_no_sigpipe()`（实现已在 P1 落地，本阶段只改调用方）。
- 交付测试：T3（`UdpReceiveWakesImmediatelyOnClose`）、T4（`UdpInjectedRecvFailureIsReported`）、T5（`SetNoSigpipeMatchesPlatform`）。
- **验收门 G-P3**：G3 达成（`udp_transport.cpp` 的 `_WIN32` 归零）；`XcpliteIntegration`(56) 与 UDP 全部用例通过；T3/T4/T5 通过；三平台 CI 全绿。

### P4 —— 加 CI 判据（三平台矩阵）

- 新增 `scripts/check_platform_isolation.sh`，内含两条检查（命中即非零退出）：

```bash
# 1) 头文件不得 include 平台 Socket 头
grep -rn "winsock2.h\|ws2tcpip.h\|sys/socket.h\|netinet/in.h" src/ --include=*.hpp && exit 1
# 2) 除平台实现文件外不得出现 _WIN32
grep -rn "_WIN32" src/ --include=*.cpp --include=*.hpp --exclude=socket_ops_win.cpp && exit 1
```

- 在 `linux.yml` / `macos.yml` / `msbuild.yml` 三个 workflow 中各加一步运行该脚本（Windows 用 `shell: bash`）。macOS / Windows 的 `Test` 步骤已在 P1 落地（§8.3.5），本阶段只补 grep 判据。
- **验收门 G-P4**：两条命令在本地与三平台 CI 均为空；三平台 CI 全绿（含新加的 `Test` 步骤）；更新 `README.md` 相关约束说明。

---

## §7 影响面

| 文件 | 改动性质 |
| --- | --- |
| `src/socket_util.hpp` | 重写为转发头（或删除并更新 include） |
| `src/socket_util.cpp` | 拆成两个平台文件 |
| `src/detail/socket/socket_ops.hpp` | **新增** |
| `src/detail/socket/socket_ops_win.cpp` | **新增** |
| `src/detail/socket/socket_ops_posix.cpp` | **新增** |
| `src/tcp_transport.cpp` | 调用点签名适配（`:134/:284/:376` 取 ops；`:317/:332/:343` 用 `SocketAddress`）；删除 `SO_NOSIGPIPE` 分支 |
| `src/udp_transport.cpp` | 删除第二套实现（约 −250 行），改用 `SocketOps` |
| `tests/tcp_fake_socket.hpp` | 27 个 override 签名适配 + 新增方法实现 |
| `tests/tcp_transport_test.cpp` | `SocketPlatformTest` 断言改为测 `SocketOps` 行为 |
| `CMakeLists.txt` | `if(WIN32)` 段改为选源 |

**不改动**：`include/libxcp/**`（公共 ABI 零变化）、`thirdparty/**`、framing/CTR 逻辑。

---

## §8 测试方案

### 8.1 既有用例（必须保持全绿）

| 用例组 | 数量 | 作用 |
| --- | --- | --- |
| `TcpFakeSocketTest` | 5 | partial write / EINTR / 等待超时 / 等待错误 / 连接重置（注入驱动） |
| `SocketPlatformTest` | 3 | 连接丢失判定集合、`kShutdownBoth` 编译期断言 |
| `TcpLoopbackTest` + `TcpLoopbackProfileTest` | 7 + 12 | TCP 端到端 |
| `TcpEndToEndTest` / `TcpReceiveCtrTest` 等 | 14 / 9 | TCP 行为 |
| `UdpBoundaryFraming` | 10 | UDP 边界解码 |
| `XcpliteIntegration` | 56 | 真实从站互操作 |
| 全量基线 | **1046**（`ctest -E "^Example"`） | 回归门 |

### 8.2 新增用例

| 编号 | 用例 | 目的 |
| --- | --- | --- |
| T1 | `SocketAddressRoundTripsIpv4` | POD ↔ `sockaddr_in` 转换正确（端口/地址字节序） |
| T2 | `ParsesIpv4TextIntoPod` | `ParseIpv4` 替代 `ParseIpv4Literal` 后行为一致 |
| T3 | `UdpReceiveWakesImmediatelyOnClose` | UDP 走 `wait_readable_multi` 后唤醒延迟不退化（原 `select` 双句柄语义） |
| T4 | `UdpInjectedRecvFailureIsReported` | UDP 也能被 `FakeSocketOps` 注入错误 |
| T5 | `SetNoSigpipeMatchesPlatform` | 确认 macOS 走选项、Windows 为 no-op |

### 8.3 多平台验证（GitHub Actions，已实测可用）

本节回答一个前提问题：**Windows 实现既然本机编不了，靠什么验证？** 答案是 GitHub Actions 三平台矩阵，且已在 2026-10-26 实测跑通。

#### 8.3.1 现状实测

三个 workflow 已在树内（`.github/workflows/`），均 `submodules: recursive`、`BUILD_TYPE=Release`、`LIBXCP_BUILD_TESTS=ON`、`BUILD_TOOLS=ON`、`BUILD_EXAMPLES=ON`、`LIBXCP_ENABLE_LOG=ON`，并用 `concurrency` 取消同 ref 的旧运行：

| 文件 | Job | Runner / 工具链 | Build | Test 步骤 |
| --- | --- | --- | --- | --- |
| `linux.yml` | `build-linux` | ubuntu-22.04 / GCC 13（PPA）/ Boost 1.86 预编译 | ✅ | ✅ `ctest --output-on-failure --timeout 300 -E XcpMasterRenderBenchTest` |
| `macos.yml` | `build-macos` | macos-14 / Homebrew LLVM（自带 libc++）/ brew boost | ✅ | ❌ **无** |
| `msbuild.yml` | `build-windows` | windows-2022 / VS 17 2022 x64 / Boost 1.86 源码 `b2` 编译并缓存 `C:\boost` | ✅ | ❌ **无** |

**实证（PR #14，head `ec3078b` = 本分支 merge master 后的提交）**：

| Job | run id | 耗时 | 结论 |
| --- | --- | --- | --- |
| `build-macos` | 40259393102 | 02:30:08 → 02:33:09（3m01s） | success |
| `build-linux` | 40259393022 | 02:30:08 → 02:36:38（6m30s，含 ctest） | success |
| `build-windows` | 40259393027 | 02:30:08 → 02:39:46（9m38s） | success |

结论：**三平台编译能力可用，但只有 Linux 在 CI 上跑测试**。Windows/macOS 的平台分支目前只被"编译"覆盖，运行时行为（`select` 语义、`SO_RCVTIMEO` 类型、`SD_BOTH`、`WSAE*` 分类）尚无自动化保障——这正是本次重构必须补的短板。

#### 8.3.2 本次要补的三件事

1. **给 macOS / Windows 加 `Test` 步骤**（否则 P2/P3 的平台实现只有编译层证据）；
2. **统一耗时用例排除清单**（Linux 当前只排了 1 项，见 8.3.4）；
3. **把 §6 P4 的两条 grep 判据挂进 workflow**，使其成为失败门而非提示。

#### 8.3.3 验证矩阵：哪些必须由 CI 验证

| 验证项 | 三平台 | 为什么不能只在本机验 |
| --- | --- | --- |
| `socket_ops_win.cpp` 可编译（`winsock2.h`/`ws2tcpip.h`/`SD_BOTH`/`WSAE*`） | Windows | 本机 macOS 无 MSVC，Windows 代码永不参与本地编译（已踩 3 次） |
| POD 化后三平台同一接口签名 | 三平台 | 头文件单一，符号缺失只在目标平台暴露 |
| `kShutdownBoth` / `IsConnectionLostCode` 常量与错误码集合 | 三平台 | `static_assert` 只在各自平台运行 |
| `SocketAddress` ↔ `sockaddr_in` 字节序（T1/T2） | 三平台 | 少量实现差异（Windows 无 `socklen_t`） |
| UDP `wait_readable_multi` 双句柄语义（数据 socket + 唤醒 socket） | 三平台 | Windows 走 `select`、POSIX 走 `poll`，路径不同（T3/T4） |
| `set_no_sigpipe`（macOS `SO_NOSIGPIPE` / Linux `MSG_NOSIGNAL` / Windows no-op）（T5） | 三平台 | 三平台各一条路径 |
| `SO_RCVTIMEO` 参数类型（Windows `DWORD` vs POSIX `timeval`） | 三平台 | 类型不兼容只在对方平台报错 |
| 测试辅助：端口预留、子进程 spawn、就绪探测 | 三平台 | `tests/` 有 61 处 `_WIN32`，行为需真机验证 |

#### 8.3.4 明确不验证的项（仅耗时类）

GitHub 共享 runner 算力弱且不稳定，**性能类门槛只作本地门禁**，CI 上用 `-E` 排除：

```bash
ctest --output-on-failure --timeout 600 \
  -E "XcpMasterRenderBenchTest|a2l_ui_perf_gen|a2l_perf_gen|ParseLargeFileUnderBudget"
```

| 用例 / fixture | 本地参考 | 不进 CI 的理由 |
| --- | --- | --- |
| `XcpMasterRenderBenchTest`（3 例） | 单帧 227–233 ms @ 198,556 行 | 阈值直接依赖 CPU，runner 上恒定超限 |
| `a2l_ui_perf_gen`（`--perf 40`） | 生成 40 MB 样本 | 只服务于上一条 |
| `a2l_perf_gen`（`--perf 5`） + `ParseLargeFileUnderBudget` | 参考 264 ms / 硬限 3000 ms | 抖动大，且是"耗时断言"本身 |

除此之外**全部进 CI**：功能、协议编解码、平台常量、边界、真实从站互操作（`XcpliteIntegration` 56 项）、无 TTY 的纯函数层。即：除"耗时"外，平台相关特性都在 CI 覆盖范围内。

#### 8.3.5 workflow 改动清单（**已于 2026-10-26 落地**；grep 判据仍随 P4）

本次已修改三个 workflow 并提交 PR：

| 文件 | 改动 |
| --- | --- |
| `linux.yml` | 排除清单由 1 项补全为 4 项；`--timeout 300 → 600`；注释写明排除原因与串行理由 |
| `macos.yml` | **新增** `Test` 步骤（已是硬门）；Boost 改由 `install-boost@v2.4.5` 装 **1.86.0**，`Configure` 导出 `BOOST_LIB_DIR`，`Test` 用 `DYLD_LIBRARY_PATH` 指向该 Boost 动态库 |
| `msbuild.yml` | **新增** `Test` 步骤（已是硬门），带 `-C $env:BUILD_TYPE` 与 `shell: pwsh`；Boost 保持源码编译 1.86.0 |

三处统一命令：

```bash
# POSIX（linux / macos）
ctest --output-on-failure --timeout 600 \
  -E "XcpMasterRenderBenchTest|a2l_ui_perf_gen|a2l_perf_gen|ParseLargeFileUnderBudget"

# Windows（VS 多配置生成器，必须带 -C）
ctest -C Release --output-on-failure --timeout 600 \
  -E "XcpMasterRenderBenchTest|a2l_ui_perf_gen|a2l_perf_gen|ParseLargeFileUnderBudget"
```

**本地等价验证（macOS Release，2026-10-26）**：同一条命令串行跑完 **1051 项 / 0 失败 / 112.24 s**（1 项既有 Skip），比 `-j 4` 慢但无端口冲突——此前并行时失败的 `ExampleXcpIntegrationDemoCppDemo` 串行下 5.33 s 通过，反证了"串行是必需的"。

`grep` 判据仍按 §6 P4 在重构末期接入（脚本 `scripts/check_platform_isolation.sh`，三个 workflow 各一步，Windows 用 `shell: bash` 调用同一脚本）。

注意事项：

1. **不要给 ctest 加 `-j`**：`Example*` 系列并行会抢默认端口 5555 造成假失败（本地已复现 4 例，串行全过）；
2. Windows runner 较慢，`--timeout` 用 600 而非 300（`XcpliteIntegration` 要起真实子进程）；
3. Windows 构建不加 `--parallel`（VS 生成器下并行会在 `ZERO_CHECK` 解析阶段失败，已在 `CMakeLists.txt` 头注释记录）；
4. `continue-on-error: true` 曾是**过渡措施**（首轮用于把"既有平台失败"与"本次改动引入"分开），**已于 2026-10-26 移除**：三平台 `Test` 现在都是硬门（见 §8.3.10）。

#### 8.3.6 触发方式与判据

| 场景 | 做法 |
| --- | --- |
| 开发分支验证（推荐） | 开 PR 到 `master`——三个 workflow 都监听 `pull_request`，PR 页看三个 job |
| 不建 PR 的临时验证 | Actions 页 `Run workflow`（`workflow_dispatch`）选目标分支 |
| 合入后 | `master` push 自动运行 |

**G-P4 验收门（扩展）**：三平台 CI **全绿**（三个 job 的 `Test` 步骤均通过）+ 两条 grep 判据为零命中 + `README.md` 约束说明更新。

#### 8.3.7 已知限制

- 共享 runner 偶发失败（Boost 下载、端口占用），重跑即可，不作为代码缺陷证据；
- 耗时判定一律以本地为准，CI 不承担性能回归职责；
- `build-windows` 在 Boost 缓存未命中时约 9–10 分钟，命中时约 4 分钟。

**macOS 上 4 个时序敏感的既有用例**（macos-26 首轮 1034 项中仅这 4 项失败，Linux 同版本全绿，故判定为 runner 速度差异而非产品缺陷，待单独治理）：

| 用例 | 位置 | 现象 | 处置建议 |
| --- | --- | --- | --- |
| `RecoveryBusy.BusyIsRetriedUntilTotalDeadlineExhausted` | `tests/recovery_test.cpp:606` | `busy_total_timeout=150ms` + `delay=20ms`，期望 `GetStatus` ≥3 次，慢机上只发出 2 次 | 放宽 `busy_total_timeout`（如 500 ms），保留“多次重试”的语义 |
| `XcpSlaveDemoIntegrationTest.ServiceEmitsPeriodicHeartbeatLog` | `tests/xcp_slave_demo_integration_test.cpp:1745` | 等 9 s 仍无“从站心跳”；日志显示 9 s 墙钟只推进 265 次快事件（≈2.65 s 逻辑时间），而心跳按事件计数（500 次）触发 | **产品侧**改为按墙钟判心跳间隔（语义本来也是“固定间隔”），而非改测试等待 |
| `ExampleMeasurementDemo` | `examples/`（示例自检） | `[FAIL] 变化性：cpp_demo::temperature 窗口内 ≥2 个不同原始值` | 放宽采样窗口或降低该变量的变动门槛 |
| `ExampleCppDemoMeasurement` | 同上 | 同一条断言，其余 6 个变量均 `[ OK ]` | 同上 |

这 4 项在 Linux（GCC 13，同一次运行）全部通过，因此不建议为它们改产品行为，只在测试侧给足容差。

#### 8.3.8 顺带可接线（不属于本次验收门）

`scripts/check_new_file_format.py`（新增文件 clang-format 门禁）此前因 `.github/workflows/` 不在开发分支而未能接线；现在 workflow 已在树内，可在三个 workflow 的 `Test` 之前各加一步：

```yaml
      - name: Format gate (new files only)
        run: python3 scripts/check_new_file_format.py --base origin/master --require-version 22
```

它只检查**新增**文件（`--diff-filter=A`），不会与既有代码的历史格式漂移冲突。是否一并接线，待用户决定；不影响本重构的 P1–P4 验收。

#### 8.3.9 macOS libc++ 异常捕获失效（已修，2026-10-26）

首次给 `macos.yml` 补 `Test` 步骤后，该 job 跑满 60 分钟被取消（进度 831/1034）。日志不是"某些用例失败"，而是整体异常机制坏掉，必须记录清楚，否则会被误当成产品缺陷：

| 现象 | 数量 |
| --- | --- |
| `Subprocess aborted***Exception` | 310 |
| `***Timeout 600.00 sec` | 5（合计约 50 分钟，是 60 分钟的主体） |

**铁证**：`tests/command_codec_test.cpp:89` 的 `CommandCodecBoundary.UploadRejectsZeroAndOverOneByte` 是纯函数 + 紧邻 `EXPECT_THROW`，本地 1 ms 通过，CI 上却直接 `libc++abi: terminating due to uncaught exception of type calmcar::xcp::XcpException`。同一个函数里 throw 完紧接着 catch 都接不住，只能是 `__cxa_throw` 与 `__cxa_begin_catch` 分属两套 libc++abi 实例。

**根因**：`Configure` 只给了 `-L$LLVM_PREFIX/lib/c++`，brew 的 `libc++.1.dylib` 仍按 install name 加载系统 `/usr/lib/libc++abi.dylib` —— 头文件用 brew libc++、abi 用系统版，异常栈不共享。Linux（GCC 13）与 Windows（MSVC）是单实例实现，所以两平台全绿，问题只在 macOS 的工具链混搭上。

**第一次修复尝试（未采用）**：给链接参数加 `-nostdlib++ -L$LLVM_PREFIX/lib/c++ -Wl,-rpath,... -lc++ -lc++abi`，想用 `-nostdlib++` 挡掉 clang 驱动的隐式 `-lc++`、再显式钉住 brew 的两份库。这条路验证成本太高（每次改完要等一次 20 分钟级的 job 才知道结果），放弃。

**最终修复（采用）**：`macos-14 → macos-26`，不再安装 brew LLVM。macos-26 镜像自带 **Apple Clang 21.0.0**，其 libc++ 已完整支持 C++20 `<format>`（`include/libxcp/log.hpp` 的唯一依赖），于是 `-DCMAKE_C_COMPILER` / `-DCMAKE_CXX_COMPILER` / `-nostdinc++` / `CMAKE_EXE_LINKER_FLAGS` / `CMAKE_SHARED_LINKER_FLAGS` 五组参数全部删除 —— 单一工具链天然只有一个 libc++abi 实例，异常机制不可能再分裂。镜像核对自 `actions/runner-images` 的 `macos-26-arm64-Readme.md`：Clang 21.0.0、CMake 3.31.5/4.1.2、Ninja 1.13.2，**未预装 Boost**，故保留一步 `brew install boost`。

**实测对比**（同一批测试）：

| 指标 | macos-14 + brew LLVM | macos-26 + Apple Clang 21 |
| --- | --- | --- |
| job 结论与耗时 | cancelled，60m20s（到 831/1034） | success，**6m47s** |
| `Subprocess aborted` | 310 | **0** |
| `***Timeout` | 5（5×600 s） | **0** |
| Test 步骤耗时 | 未跑完 | **139.4 s**（1034 项，99% passed） |
| `otool -L libxcp_tests` | 混搭 | 仅 `/usr/lib/libc++.1.dylib` |

诊断步骤实测输出（可作为根因闭环的证据）：

```
Apple clang version 21.0.0 (clang-2100.1.1.101)
	/usr/lib/libc++.1.dylib (compatibility version 1.0.0, current version 2100.43.0)
[  PASSED  ] 1 test.        # CommandCodecBoundary.UploadRejectsZeroAndOverOneByte
```

那个纯函数 + `EXPECT_THROW` 的用例在旧配置上直接 terminate，在新配置上 1 ms PASSED。

**取证手段**（已作为临时步骤加入 workflow，定位完成后删除）：

```bash
otool -L build/tests/libxcp_tests | grep -E 'c\+\+'
DYLD_PRINT_LIBRARIES=1 ./build/tests/libxcp_tests \
  --gtest_filter=CommandCodecBoundary.UploadRejectsZeroAndOverOneByte
```

**超时收紧**：`--timeout 600 → 180`。本机最慢用例 10 s，CI 慢 5 倍也才 50 s；180 s 足够容忍抖动，又能让真正挂死的用例在 3 分钟内被点名，而不是每条合法烧 10 分钟。

**教训**：在这类"混搭工具链"的 CI 上，`continue-on-error: true` 会让 job 显示 success 而测试实际崩溃（本次 `build-windows` 就是 success），判断"测试是否真绿"必须看无该标记的 job 或直接读日志统计。

#### 8.3.10 macos-26 首轮暴露的 4 个时序 flaky（已修，2026-10-26）

切到 macos-26 后 Test 首次真正跑完 1034 项：**0 abort / 0 timeout / 139.4 s / 99% passed**，只剩 4 条失败。它们都是"把本机速度当常量"的时序假设，不是平台实现差异——Linux 上 1034 项全过即为佐证：

| 用例 | 失败断言 | 根因 | 修法 |
| --- | --- | --- | --- |
| `RecoveryBusy.BusyIsRetriedUntilTotalDeadlineExhausted` | `Expect: (f.slave.Count(GetStatus)) >= (3), actual: 2 vs 3` | 测试用 150 ms 总预算 + 40 ms 退避，前两次 BUSY 已耗掉 80 ms，第 3 次取命令时已过预算 → 只发生 2 次重试 | `tests/recovery_test.cpp` 把 `busy_total_timeout` 由 150 ms 提到 **500 ms**；断言意图（"预算耗尽前一直重试"）不变 |
| `XcpSlaveDemoIntegrationTest.ServiceEmitsPeriodicHeartbeatLog` | `logs.find("从站心跳")` 为 npos（从站跑了 9.09 s、265 个快事件却没打心跳） | 心跳用**事件计数**触发（每 1000 个快事件一次），慢 runner 上 265 事件远不到阈值 | `examples/xcp_slave_demo/main.cpp` 改为**按墙钟**每 5 s 必打一次心跳（快事件里顺带检查） |
| `ExampleMeasurementDemo` | `[FAIL] 变化性：cpp_demo::temperature 窗口内 ≥2 个不同原始值` | 从站 `temperature += 1` 只在 mainloop `counter` 回绕到 0 时执行（`counter_max=1000`、1 ms/轮 → 约 1 s 一次）；慢 runner 上 4 s 窗口可能一次都没回绕 | `measurement_demo` 新增 `--static-symbols`（只断言覆盖、跳过变化性断言），三个 Example 都传 `--static-symbols temperature` |
| `ExampleCppDemoMeasurement` | 同上 | 同上 | 同上 |

**修复后本地实测**：8 项（`RecoveryBusy.*` 4 项 + 心跳 1 项 + 三个 Example）**全过**，`ExampleMeasurementDemo` 10.08 s、`ExampleCppDemoMeasurement` 9.86 s、`ExampleCppDemoCalibration` 9.82 s。

**三平台 Boost 版本统一为 1.86.0**（用户要求）：Linux 本就用 `install-boost` 装 1.86.0；macOS 原先 `brew install boost` 拿的是 formula stable（实测已漂到 **1.92.0**，且无 `boost@1.86` formula，只有 `boost@1.85`），现改用 `MarkusJx/install-boost@v2.4.5` + `boost_version: 1.86.0` + `platform_version: 15` + `toolset: clang`（prebuilt-boost 提供 `boost-1.86.0-macos-15-clang-static+shared-aarch64.tar.gz`，无需源码构建）；Windows 仍用源码 `b2` 编译 1.86.0 并缓存 `C:\boost`。

**G-P4 判据更新**：三平台 `Test` 均为硬门（无 `continue-on-error`），`--timeout 180`，且 `Test` 步骤出现任何非零退出即视为未通过。

### 8.4 CI 判据

见 §6 P4；两条 grep 必须在三个 workflow 中作为**失败门**，而不是提示。


---

## §9 风险与回滚

### 9.1 Windows 侧只能靠 CI 验证（最高风险，已有可执行手段）

本机为 macOS，Windows 实现无法本地编译。**该风险已于 2026-10-26 实测降级**：三平台 workflow 全部存在且可用（PR #14 三个 job 全绿，见 §8.3.1），因此不再是"无验证手段"，而是"验证延迟一轮 PR"。缓解措施：

1. 分阶段提交，P2 完成后**先在 PR 上看到 `build-windows` 的 `Test` 步骤绿**再进 P3（`Test` 步骤已按 §8.3.5 提前到 P1 落地，故 P2 阶段就有运行时证据）；
2. P1（接口 POD 化）不动 `#if` 逻辑，风险最低，可先合；
3. 保留三次事故的防回归断言（`static_assert(kShutdownBoth …)`、`IsConnectionLostCode` 用例、`NOMINMAX`）；
4. 每个阶段结束都以"三平台 CI 全绿 + 本地全量回归"为出口条件，不靠单侧推断。

### 9.2 地址 POD 转换遗漏

`SocketAddress` 与 `sockaddr_in` 的转换涉及字节序（`port` 用主机序、`ip` 用网络序）。缓解：T1/T2 用例 + 现有 IPv4 拒绝逻辑（`LooksLikeIpv6`）不改。

### 9.3 测试/示例/工具层的平台分支不纳入本次

实测 `tests/` 61 处、`examples/xcp_master_udp/` 5 处、`tools/xcp_master/main.cpp` 3 处 `_WIN32`。它们服务于"预留端口、spawn 子进程、关闭 socket"，不适合塞进 `SocketOps`。约定：**产品运行时零平台分支；测试层允许，但必须复用现成辅助**。

### 9.4 回滚

| 阶段 | 回滚方式 |
| --- | --- |
| P1 | `git checkout -- src/socket_util.hpp src/socket_util.cpp src/tcp_transport.cpp tests/tcp_fake_socket.hpp tests/tcp_transport_test.cpp` + 删除新目录 |
| P2 | 恢复 `src/socket_util.cpp` 与 `CMakeLists.txt` 的 `if(WIN32)` 段 |
| P3 | `git checkout -- src/udp_transport.cpp src/tcp_transport.cpp`，保留 `SocketOps` 扩展方法（不影响兼容） |
| P4 | 删除 CI 判据步骤（无代码影响） |

---

## §10 明确不做的事

1. **不引入 Boost.Asio。** 理由：现有实现已达成三平台可用；Asio 在"同步阻塞 + 单连接 + 强超时控制"场景下拿不到异步收益，反而要重建 `send_timeout_ms` 的三态（1/0/-1）语义与 `shutdown` 打断模式；且 Asio 同步调用不可注入，5 个确定性用例需重写。
   **但本重构为该路径留了口子**：将来若要 Asio，只需新增 `socket_ops_asio.cpp` 实现同一接口，由工厂函数选择，属"加文件"而非"改代码"。
2. 不改 framing/CTR/解码。
3. 不新增 IPv6 能力。
4. 不重构测试层平台分支。
5. 不动 `thirdparty/`。

---

## §11 决策记录（原"待确认决策"，2026-10-26 已全部裁决）

| 编号 | 问题 | 裁决（用户 2026-10-26） |
| --- | --- | --- |
| Q1 | POSIX 是否再拆成 `linux` / `macos` 两个文件？ | **不拆**（差异仅 SIGPIPE 一项，见 §4.3 / D6） |
| Q2 | `SocketHandle` 用 `std::intptr_t` 还是 `std::uintptr_t`？ | **`intptr_t`**（保留 `-1` 无效哨兵语义，见 §4.1 / D1） |
| Q3 | `ParseIpv4` 等自由函数是否一并迁入 `socket_ops.hpp`？ | **迁入**（它们同样依赖平台类型，见 §4.2） |
| Q4 | P1 与 P3 的接口扩展是否合并为一次改动？ | **合并**：P1 一次改完 **30 个方法**，P3 只做 UDP 合并与调用方切换（见 §6） |

四项全部按建议执行，无分歧项。

---

## §12 批准

- [x] 确认三平台 CI 可用（2026-10-26 实测 PR #14：`build-macos` / `build-linux` / `build-windows` 三个 job 全绿）
- [x] 用户裁决 §11 四项（2026-10-26）：不拆 POSIX / `intptr_t` / `ParseIpv4` 迁入 / P1+P3 接口合并
- [ ] 用户批准按 P1 → P2 → P3 → P4 实施
- [x] workflow 改动已完成（2026-10-26，见 §8.3.5）：macOS/Windows 补 `Test` 步骤 + Linux 统一排除清单（P4 只剩 grep 判据与 README）
- [ ] 同意"每阶段出口条件 = 三平台 CI 全绿 + 本地全量回归"（§9.1）
- [ ] （可选）是否顺带接格式门禁 `check_new_file_format.py`（§8.3.8）

批准后按 P1 起步；每阶段完成后提交实施记录到 `code-plan/`，格式与其他批次一致。
