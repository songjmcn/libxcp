# Platform 接口分层：EventPoller 与 SocketHandle POD（实施计划）

> 目标读者：维护 `src/socket_util.*` 与 `src/{tcp,udp}_transport.cpp` 的开发者。
> 状态：**方案已由用户批准（A2 与 A1 一并实施）**，本文档既是计划也是实施记录。

---

## §0 结论速览

| 项 | 结论 |
| --- | --- |
| A2 目标 | 用**一个** `EventPoller` 抽象收掉 `select`(Windows) / `poll`(POSIX) 两套等待实现，并消除 `select` 的 `FD_SETSIZE` 与 POSIX `maxfd+1` 语义问题 |
| A1 目标 | `SocketHandle` 改为平台无关 POD（`std::intptr_t`），`SOCKET` / `int` 的分叉只留在 `RealSocketOpsImpl` 内部 |
| 改动文件 | 新增 `src/event_poller.hpp/.cpp`、`tests/event_poller_test.cpp`；修改 `src/socket_util.hpp/.cpp`、`src/udp_transport.cpp`、`tests/CMakeLists.txt` |
| 不动 | `SocketOps` 虚表（避免所有测试替身被迫实现新方法）、`tcp_transport.cpp`（它已 100% 走 `SocketOps`，无直接 `select`） |
| 验收 | 全量 `ctest` 零失败 + 三平台 CI 绿 |

---

## §1 现状：平台差异集中在两类

实测（`grep -n "_WIN32" src/`）：全部 29 处分叉里，真正有语义差异的只有两类，其余都是**同义重复**。

### 1.1 等待原语：两套实现，且语义并不等价

`src/socket_util.cpp` 里有同名两版 `waitFor()`：

```cpp
// Windows 版（:94-110）
fd_set set;  FD_ZERO(&set);  FD_SET(handle, &set);
const int ready = ::select(0, writable ? nullptr : &set, writable ? &set : nullptr, nullptr, &tv);
```

```cpp
// POSIX 版（:126-143）
pollfd pfd{};  pfd.fd = handle;  pfd.events = static_cast<short>(writable ? POLLOUT : POLLIN);
const int ready = ::poll(&pfd, 1, static_cast<int>(clamped));
```

问题清单：

| # | 问题 | 后果 |
| --- | --- | --- |
| 1 | Windows 用 `select`，受 `FD_SETSIZE`（默认 64）限制 | 进程内句柄数多时 `FD_SET` 越界写，属未定义行为 |
| 2 | POSIX 版首参传 `FD_SETSIZE`（见 `src/udp_transport.cpp:694`）而非 `maxfd+1` | 句柄值 ≥ 1024 时该句柄不会被检查，等不到就绪就超时 |
| 3 | 两版对 `POLLERR/POLLHUP` 的处理靠注释约定（`socket_util.cpp:140-142`），`select` 侧靠 `exceptfds` 缺省 | 断链在两平台可能落到"超时"分支，延迟一个周期才被发现 |
| 4 | 两套实现意味着同一段逻辑要写两遍、测两遍 | 已有 `tests/tcp_transport_test.cpp` 的三态等待用例无法覆盖 Windows 分支 |

### 1.2 句柄类型：`SOCKET` 与 `int` 分叉

`src/socket_util.hpp:41-50`：

```cpp
#if defined(_WIN32)
using SocketHandle = SOCKET;                  // UINT_PTR，无符号
inline constexpr SocketHandle kInvalidSocketHandle = INVALID_SOCKET;
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocketHandle = -1;
#endif
```

问题：

1. `SOCKET` 是无符号，`-1` 比较在 Windows 上必须写 `INVALID_SOCKET`，于是 `handle < 0` 这类判断不能跨平台复用；
2. `src/udp_transport.cpp:41-43` 又**独立定义**了一份 `SocketHandle` / `kInvalidSocket`（与 `socket_util.hpp` 语义相同、名字不同），同一项目里两套句柄别名；
3. 句柄一旦跨 DLL/ABI 传递（本库不做，但日志与错误路径会把句柄打进格式化字符串），有符号/无符号决定了打印与比较写法。

`INVALID_SOCKET` 在 x64 上等于 `0xFFFFFFFFFFFFFFFF`，转成 `std::intptr_t` 恰为 `-1`，因此**统一哨兵为 `-1` 是安全的**。

---

## §2 A2 设计：`EventPoller`

### 2.1 接口（`src/event_poller.hpp`，内部头，不对外安装）

```cpp
namespace calmcar::xcp::detail {

/// @brief 单个等待项：句柄 + 关注的事件。
struct WaitItem {
    SocketHandle handle = kInvalidSocketHandle;  ///< 目标句柄
    bool readable = false;                       ///< 关注可读
    bool writable = false;                       ///< 关注可写
};

/// @brief 单个等待项的结果。
struct WaitStatus {
    bool readable = false;  ///< 该句柄当前可读
    bool writable = false;  ///< 该句柄当前可写
    bool failed = false;    ///< 该句柄出现 POLLERR/POLLHUP/POLLNVAL
};

/**
 * @brief 多句柄等待抽象（POSIX `poll` / Windows `WSAPoll`）
 * @return 1 至少一个句柄就绪、0 超时、-1 失败（错误文本写入 error_out）
 */
class EventPoller {
public:
    virtual ~EventPoller() = default;
    virtual int Wait(std::span<const WaitItem> items, std::uint32_t timeout_ms,
                     std::vector<WaitStatus>& status_out,
                     std::string* error_out) = 0;
};

[[nodiscard]] EventPoller& RealEventPoller();
[[nodiscard]] EventPoller& CurrentEventPoller() noexcept;
void SetEventPollerForTesting(EventPoller* poller) noexcept;

/**
 * @brief 等待任意句柄就绪（走当前生效的 EventPoller）
 * @param items 等待项列表
 * @param timeout_ms 超时（毫秒）
 * @param status_out 与 items 等长的结果数组
 * @return 1 就绪、0 超时、-1 失败
 */
int WaitAnySocket(std::span<const WaitItem> items, std::uint32_t timeout_ms,
                  std::vector<WaitStatus>& status_out);
```

### 2.2 实现要点

- **POSIX**：`::poll(items, n, timeout)`，`timeout` 钳到 `0x7FFFFFFF`（沿用现有注释的理由：上限约 24 天）。
- **Windows**：`::WSAPoll(items, n, timeout)`。相对 `select` 的两个直接收益：不受 `FD_SETSIZE` 限制、支持"多句柄 + 不同事件"一次等待。
  - 已知缺陷记录：微软文档明确 `WSAPoll` **不上报连接失败事件**（`POLLERR`/`POLLHUP` 在"连接被拒"场景可能不置位）。缓解：`WaitAnySocket` 把 `POLLERR/POLLHUP/POLLNVAL` 映射为 `failed=true` 且**同时**返回 1（视为就绪），真正原因仍由随后的 `SO_ERROR` / `recv` 判定——这与现有 `waitFor` 的注释约定一致，不改变上层语义。
- **错误判定**：两平台都用 `detail::IsConnectionLostCode` 之外的另一路——等待本身失败只看"是否 interrupted"，与现有 `waitFor` 相同。
- `EventPoller` **不持有**句柄生命周期，只做等待。

### 2.3 与既有接口的关系

| 既有接口 | 变化 |
| --- | --- |
| `SocketOps::wait_readable(handle, timeout)` / `wait_writable` | 签名不变、返回值三态不变；实现改为委托 `WaitAnySocket` 单项 |
| `SocketOps` 虚表 | **不变**（新增方法会让所有测试替身被迫实现，收益不足） |
| `src/udp_transport.cpp` 接收循环的 `select` | 改为一次 `WaitAnySocket({数据句柄, 唤醒句柄})`，据此判断该 `recvfrom` 还是 `DrainWakeup` |
| 测试注入 | 新增 `SetEventPollerForTesting()`；`SocketOps` 注入路径保持不变（两条路径互不影响） |

---

## §3 A1 设计：`SocketHandle` POD 化

```cpp
namespace calmcar::xcp::detail {

/// @brief 平台无关的 Socket 句柄（Windows 的 `SOCKET` 收窄到此类型）。
using SocketHandle = std::intptr_t;

/// @brief 无效句柄哨兵（Windows 侧 `INVALID_SOCKET` 转 `intptr_t` 也是 -1）。
inline constexpr SocketHandle kInvalidSocketHandle = -1;
}
```

平台知识只留在 `src/socket_util.cpp` 的 `RealSocketOpsImpl` 内部：

```cpp
#if defined(_WIN32)
using NativeSocket = SOCKET;
[[nodiscard]] SOCKET ToNative(SocketHandle handle) noexcept {
    return static_cast<SOCKET>(handle);
}
[[nodiscard]] SocketHandle FromNative(SOCKET handle) noexcept {
    return static_cast<SocketHandle>(handle);
}
#else
using NativeSocket = int;
// 同形，仅类型不同
#endif
```

规则：

1. **所有** `::socket` / `::accept` 的返回值经 `FromNative()`；
2. **所有** `::bind` / `::connect` / `::send` / `::recv` / `::shutdown` / `::setsockopt` / `::getsockname` / `::closesocket` 的句柄入参经 `ToNative()`；
3. 比较一律用 `handle == kInvalidSocketHandle`，不再出现 `INVALID_SOCKET` / `< 0` 的分叉；
4. `src/udp_transport.cpp` 删除自己的 `SocketHandle` / `kInvalidSocket` 别名，统一用 `detail::SocketHandle` / `kInvalidSocketHandle`。

---

## §4 改动清单

| 文件 | 动作 | 说明 |
| --- | --- | --- |
| `src/event_poller.hpp` | 新增 | `WaitItem` / `WaitStatus` / `EventPoller` / `WaitAnySocket` / 注入钩子 |
| `src/event_poller.cpp` | 新增 | POSIX `poll` 与 Windows `WSAPoll` 两版实现 + 注入指针（原子） |
| `src/socket_util.hpp` | 修改 | `SocketHandle` → `std::intptr_t`；`kInvalidSocketHandle` → `-1`；补 `<cstdint>` 已有 |
| `src/socket_util.cpp` | 修改 | 删 `waitFor` 两套实现，改为 `WaitAnySocket` 单项；新增 `ToNative`/`FromNative` 并在全部平台调用点应用 |
| `src/udp_transport.cpp` | 修改 | 接收循环 `select` → `WaitAnySocket`；删本地句柄别名；`SO_RCVTIMEO`/`inet_ntop` 等其余分叉保持不变（本批不动） |
| `src/tcp_transport.cpp` | **不改** | 已全部经 `SocketOps`，无直接等待调用 |
| `tests/event_poller_test.cpp` | 新增 | 见 §5 |
| `tests/CMakeLists.txt` | 修改 | 登记新测试文件 |
| `code-plan/Platform接口分层_实施计划_EventPoller与SocketHandle.md` | 新增 | 本文件 |

---

## §5 测试方案

`tests/event_poller_test.cpp`（gtest，加入 `libxcp_tests`）：

| 用例 | 断言 |
| --- | --- |
| `RealPollerReportsReadyForWritableSocket` | 新建 TCP socket 未连接时 `Wait(writable)` 返回 1 或多句柄中该句柄 `failed||writable`（不同平台可能有差异，按"不返回 0"断言） |
| `RealPollerTimesOutWithinBudget` | 对已连接但对端静默的 socket 等可读，`timeout_ms=20` 返回 0，且耗时 ≥ 15 ms、< 1 s |
| `WaitAnySocketMapsTwoHandlesIndependently` | UDP 自连两个 socket，只有发送方写入的那个"可读"（用 `WaitAnySocket` 断言另一个未就绪） |
| `InjectedPollerDrivesReadyPath` | `SetEventPollerForTesting(假实现)` 后 `WaitAnySocket` 返回假实现给的值，验证注入通道 |
| `InjectedPollerDrivesTimeoutPath` | 同上，返 0 时 `status_out` 全 false |
| `HandleRoundTripsThroughInvalidSentinel` | `kInvalidSocketHandle == -1`，且 `static_cast<std::intptr_t>(INVALID_SOCKET) == -1`（仅 Windows 编译） |

回归范围（必须全绿）：

- `libxcp_tests` 全量（含 `TcpTransportConnectionTest` / `UdpLoopback` / `UdpBoundaryFraming` / `TcpReceiveCtrTest`）；
- `XcpliteIntegration`（真实 TCP/UDP 互操作）；
- `XcpSlaveDemoIntegrationTest`（真实子进程）；
- 三平台 CI（`build-linux` / `build-macos` / `build-windows` 的 `Test` 都是硬门）。

---

## §6 风险与取舍

| 风险 | 说明 | 缓解 |
| --- | --- | --- |
| `WSAPoll` 不上报连接失败 | 微软文档明载 | 把 `failed` 与"就绪"同时置位，真实原因仍由 `SO_ERROR`/`recv` 判定，与现状语义一致；连接建立路径**不**依赖 `WSAPoll` 的失败位（仍是 `select` 可写 + `SO_ERROR`，见 `tcp_transport` 的 connect 逻辑） |
| 注入通道双份 | `SocketOps` 与 `EventPoller` 都能注入 | 两者职责不同：前者替代系统调用，后者替代等待原语；测试中各用所需，文档与本文件写明 |
| `std::intptr_t` 打印/比较 | 已有日志把句柄打进字符串 | 统一用 `%lld`/`std::to_string`，Windows 侧不再需要 `%llu` 分支 |
| 句柄值 ≥ FD_SETSIZE | 改造后不再受此限制 | 属本次改造的直接收益 |

---

## §7 实施记录

### 7.1 结论速览

| 项 | 结果 |
| --- | --- |
| 新增文件 | `src/event_poller.hpp`（127 行）、`src/event_poller.cpp`（215 行）、`tests/event_poller_test.cpp`（5 用例） |
| 修改文件 | `CMakeLists.txt`、`src/socket_util.hpp`、`src/socket_util.cpp`、`src/udp_transport.cpp`、`tests/CMakeLists.txt` |
| 平台等待原语 | 由 3 份 `select`/`poll` 收敛为 1 份（`poll` / `WSAPoll`） |
| 平台句柄类型 | 由 2 类（`SOCKET`、`int`）收敛为 1 类（`std::intptr_t` 别名） |
| 本地全量回归 | `ctest -E "<4 个耗时用例>"` → **1039 项 / 0 失败 / 112.21 s**（1 项既有 Skip） |
| 新用例 | `EventPollerTest.*` 5 项全绿（51 ms） |
| 格式门禁 | `scripts/check_new_file_format.py --base origin/master` → 3 个新文件全部通过 |

### 7.2 A2：`EventPoller`

- 新增 `src/event_poller.hpp`：`WaitItem{handle, readable, writable}`、`WaitStatus{readable, writable, failed}` + `Ready()`、抽象基类 `EventPoller::Wait(items, timeout_ms, status_out, error_out)`、`RealEventPoller()`、`CurrentEventPoller()`、`SetEventPollerForTesting()`、`WaitAnySocket(...)` 便捷函数。
- 新增 `src/event_poller.cpp`：POSIX 走 `::poll()`（`timeout` 钳到 `0x7FFFFFFF`，`POLLERR/POLLHUP/POLLNVAL` 记入 `failed`），Windows 走 `::WSAPoll()`（`POLLRDNORM`/`POLLWRNORM`，不再受 `FD_SETSIZE` 限制）；Windows 侧自带 `WinsockGuard` 引用计数，避免"先建 Poller 后建 Socket"的初始化顺序坑。
- `src/socket_util.cpp`：删除 `waitForSocket()` 内两套 `select`/`poll` 实现，改由文件内 `WaitSingle(handle, writable, timeout_ms)` 统一转发到 `CurrentEventPoller()`；`SocketOps::wait_readable/wait_writable` 的三态返回值（1/0/-1）保持不变，故 `TcpTransport` 调用点零改动。
- `src/udp_transport.cpp`：接收循环的"数据 Socket + 唤醒 Socket"等待由 `fd_set` + `select` 改为一次 `WaitAnySocket({data, wake})`，索引按需动态组装（两个句柄都可能缺失），唤醒分支与数据分支语义逐条对齐改造前实现。
- 附带收益：Windows 侧 `select(0, ...)` 的 `FD_SETSIZE` 上限消失；`<poll.h>`/`<sys/select.h>` 从 `socket_util.cpp`、`udp_transport.cpp` 移除，`poll` 只出现在 `event_poller.cpp` 一处。

### 7.3 A1：`SocketHandle` POD 化

- `src/socket_util.hpp`：`#if defined(_WIN32) using SocketHandle = SOCKET;` / `using SocketHandle = int;` 改为
  `using SocketHandle = std::intptr_t;`，`kInvalidSocketHandle` 统一为 `-1`；`ToNative()` / `FromNative()` 两个文件内联转换函数按平台各一份（Windows 转 `SOCKET`，POSIX 转 `int`）。
- `src/udp_transport.cpp`：删除文件内自己的 `using SocketHandle = SOCKET/int;` 与 `kInvalidSocket`，改为引用 `detail::SocketHandle` / `detail::kInvalidSocketHandle`；21 处平台调用点（`socket/bind/getsockname/connect/setsockopt/ioctlsocket/fcntl/send/recv/sendto/recvfrom/closesocket/close`）经 `ToNative()` 收窄，`socket()` 返回值经 `FromNative()` 归一。
- 效果：`INVALID_SOCKET`（`~0ULL`）不再需要与 `-1` 判等，比较统一为 `== kInvalidSocketHandle`；同进程只有一种句柄类型，测试替身与生产实现可以互换。

### 7.4 测试

`tests/event_poller_test.cpp`（5 用例，全部使用回环 UDP Socket，不依赖外部网络）：

| 用例 | 固定住的行为 |
| --- | --- |
| `EmptyItemSetReturnsTimeout` | 空等待集返回 0 且清空结果表（接收循环依赖此语义回到关闭标记检查） |
| `QuietSocketTimesOut` | 静默句柄 50 ms 后返回 0，`status[0]` 不就绪 |
| `ReportsOnlyTheSignalledSocket` | 两个句柄同时等待时只报告被写入的那一个，且**下标与入参一一对应**（改造前该能力只有间接覆盖） |
| `ReportsWritableForIdleUdpSocket` | `writable` 分支（TCP 发送路径）独立可用 |
| `SocketOpsWaitsDelegateToInjectedPoller` | `SetEventPollerForTesting` 注入后，`SocketOps::wait_readable/wait_writable` 确实改走注入实现，且 `readable`/`writable` 标志与超时值被正确转发 |

既有传输层用例（`TcpTransport*`、`Udp*`、`TcpEndToEnd*`、`TcpLoopback*`、`UdpBoundaryFraming`）全部继续通过，说明三态返回值与唤醒语义没有回归。

### 7.5 未做 / 后续

- `src/tcp_transport.cpp` 仍保留 2 处平台分支（`winsock2.h` 包含、`SO_NOSIGPIPE` 的 `setsockopt`）——按 §4 属于 A3（平台头与 Socket 选项单点化），未纳入本批。
- 未实测 Windows/macOS-Intel 构建，平台正确性依赖 `event_poller.cpp` 的平台分支 + PR CI 三平台。

