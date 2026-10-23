# B-T2 实施记录：TCP 传输与可注入 Socket 层

> 对应计划：`code-plan/TCP传输_实施计划_B-T1至B-T5.md` 第 5 章（B-T2）
> 批次：B-T2
> 前置：B-T1（见 `code-plan/B-T1_实施记录_以太网通用帧层与双WireProfile解码器.md`）
> 裁决依据：m00560（编码即拒绝、Profile 归属 B-T1）、m00848（冲突 A 取 A1、冲突 B 取 B1）、m00966（取 T1 方案）

---

## 1. 本批次目标

在 B-T1 已完成的通用帧层与双 Wire Profile 解码器之上，实现 **TCP 承载**：

| 交付 | 职责 |
|---|---|
| `socket_util.hpp/.cpp` | 跨平台 Socket 调用与**可注入函数表** `SocketOps` |
| `tcp_transport_config.hpp` | 角色/端点/超时/Profile/对齐的配置载体 |
| `tcp_transport.hpp/.cpp` | `IXcpTransport` 的 TCP 实现（Master/Slave、字节流解帧、短写补齐） |

与 UDP 的本质差异决定全部设计：**有连接**（无来源过滤、断链不自动重连）、
**字节流**（recv 边界与 Frame 边界无关）、**可能短写**（必须循环发送 + 可写等待超时）。

---

## 2. 交付物清单

### 2.1 新增文件

| 文件 | 行数 | 职责 |
|---|---|---|
| `src/socket_util.hpp` | 193 | 内部头：`SocketHandle`、`SocketOps` 函数表、`ParseIpv4Literal`、`LooksLikeIpv6`、名称解析注入点 |
| `src/socket_util.cpp` | 369 | `RealSocketOpsImpl` 生产实现；平台差异（句柄类型、非阻塞设置、等待原语、错误码判定、SIGPIPE 规避）全部收敛于此 |
| `include/libxcp/tcp_transport_config.hpp` | 127 | `TcpRole{Master,Slave}`、`TcpTransportConfig` |
| `include/libxcp/tcp_transport.hpp` | 186 | `TcpTransport` 对外契约（平台无关） |
| `src/tcp_transport.cpp` | 802 | 连接建立、双 Profile 编帧、接收线程、短写补齐 |
| `tests/tcp_transport_test.cpp` | 839 | 63 个用例（见 §5） |

均已登记：`CMakeLists.txt:159-160`（源文件）、`tests/CMakeLists.txt:56`（测试）。

### 2.2 未新增测试设施

计划 §5.6 提到用假 `SocketOps` 驱动错误分支。**实际情况**：`SocketOps` 的注入点
（`SetNameResolverForTest`）在 B-T2 中只覆盖名称解析；partial write / EINTR /
EAGAIN 三类分支的注入夹具未在本批次建立，相应测试改用真实回环 Socket 覆盖
可确定性复现的部分（见 §6 偏差说明）。**该缺口是已知的、显式记录的。**

---

## 3. 关键设计决策

### 3.1 公开头不得泄漏平台 Socket 类型（裁决 T1，m00966）

初版 `tcp_transport.hpp:131` 的私有方法签名写成 `const sockaddr*` / `socklen_t`，
导致 `<sys/socket.h>` 的类型进入公开 ABI，与 `udp_transport.hpp` 的平台无关风格冲突。

**处置**：改为类型擦除签名，平台差异留在 `.cpp`：

```cpp
// include/libxcp/tcp_transport.hpp
void ConnectWithTimeout(const void* addr, std::size_t addr_len);
```

```cpp
// src/tcp_transport.cpp:346-348
const auto* sa = static_cast<const sockaddr*>(addr);
const auto sa_len = static_cast<socklen_t>(addr_len);
if (ops.connect(m_socket_->handle, sa, sa_len) != 0) { ... }
```

理由：公开接口一旦固化平台类型，后续支持 IPv6 或替换 socket 抽象都要改 ABI。

### 3.2 `SocketOps` 用三态返回值区分"超时"与"故障"

`wait_writable` / `wait_readable` 返回 **1 就绪 / 0 超时 / -1 错误**，而非 `bool`。

理由：TCP 上二者处置完全不同——超时可重试或报 send timeout，故障必须断链。
若压成 `bool`，调用方无法区分"对端慢"和"对端没了"，只能二选一，必然错一边。

### 3.3 `Close()` 靠 `shutdown()` 打断阻塞 `recv`，不在 join 前 close 句柄

```cpp
// src/socket_util.cpp:276
int shutdown_socket(SocketHandle handle) override {
    // 双向关闭：立即让阻塞中的 send/recv 返回，但**不**释放句柄号
    return ::shutdown(handle, SHUT_RDWR);
}
```

顺序必须是 **shutdown → join → close**。若先 `close()`，句柄号会被 OS 立即回收并
可能分配给本进程新建的 Socket，而接收线程此刻仍可能在使用它——这是典型的
句柄复用竞态。实测 `CloseInterruptsBlockingReceive` 在 0 ms 内完成（对端保持连接
不发数据），证明 shutdown 确实立即打断了 `recv`，而非靠轮询周期。

### 3.4 断链**不自动重连**（刻意决策）

XCP 的会话状态（CONNECT / RESOURCE / DAQ 分配 / SEED&KEY）都绑定在一条连接上。
传输层擅自重连会让上层会话视图与真实连接错位：上层以为在跟已 CONNECT 的 Slave
通信，实际是一条全新的、未 CONNECT 的连接。故断链只上报 `OnTransportClosed()`，
重连与重新握手由上层显式决策。

### 3.5 CTR 分配顺序必须等于写入顺序

发送路径由 `m_send_mutex_` 串行化，**CTR 递增在同一把锁下完成**。否则并发 `Send()`
会让对端看到乱序 CTR。`SendCtrAdvancesPerFrame` 用两帧验证 0 → 1 的递增与写入
顺序一致。

### 3.6 暂不支持 IPv6，但必须**显式**报 `UnsupportedFeature`

```cpp
// src/socket_util.cpp:359-362
bool LooksLikeIpv6(const std::string& text) {
    // IPv4 点分十进制与主机名都不含冒号；含冒号即按 IPv6 对待
    return text.find(':') != std::string::npos;
}
```

理由：静默降级成笼统的"地址非法"会让调用方无法区分**笔误**与**未实现能力**。
两个用例（远端/本地 IPv6）断言 `ErrorCategory::UnsupportedFeature`。

---

## 4. 编译期修复（T1 落地过程中发现的 4 类问题）

| # | 现象 | 根因 | 处置 |
|---|---|---|---|
| 1 | `socket_util.hpp:83` `unknown type name 'addrinfo'` | POSIX 分支未引 `<netdb.h>`；Windows 侧由 `ws2tcpip.h` 传递带入 | 补 `#include <netdb.h>`（`src/socket_util.hpp:34`） |
| 2 | `socket_util.cpp:348` `reference to 'RealSocketOps' is ambiguous` + `is an abstract class` | 类名 `RealSocketOps` 与函数名 `RealSocketOps()` 同名；函数体内 `static RealSocketOps instance;` 被解析为函数返回类型 | 类改名 `RealSocketOpsImpl`（`:154`、`:158`、`:348`），对外函数名不变 |
| 3 | `tcp_transport_config.hpp:93` `no member named 'v14LengthInDle'` | `TcpWireProfile` 是 **enum class**，不存在该工厂函数（凭记忆写出的 API） | 改为 `TcpWireProfile::AsamV14` |
| 4 | `tcp_transport.hpp:131` `unknown type name 'sockaddr'` | 同上 §3.1 | 类型擦除为 `const void*` / `std::size_t` |

**测试文件自身的 2 处错误**：漏引平台 socket 头；断言用了不存在的
`XcpException::category()`（实际为 `Category()`，见 `include/libxcp/xcp_error.hpp:72`）。

> 教训：写码前必须 `read` 目标接口确认签名。第 3、4 项与测试的 `category()`
> 都是**凭记忆编码**的产物，读一眼头文件即可避免。

---

## 5. 测试结果

### 5.1 执行

```bash
cmake -B cmake-build-release -S . -DCMAKE_BUILD_TYPE=Release -DLIBXCP_BUILD_TESTS=ON
cmake --build cmake-build-release -j 4 --target libxcp_tests
./tests/libxcp_tests
```

### 5.2 结果（实测）

| 范围 | 结果 |
|---|---|
| TCP 相关（`--gtest_filter='Tcp*'`） | **63 / 63 PASSED**（2334 ms） |
| 全量 `libxcp_tests` | **721 PASSED + 1 SKIPPED = 722**（37691 ms） |

唯一的 SKIPPED 是既有用例 `AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`，
与本次改动无关。**无 FAILED**。

> 核对：日志中 `[ OK ]` 计数 721，`[ SKIPPED ]` 1，合计 722，与
> `Running 722 tests` 一致——这个加总关系是刻意核对的，避免"绿了就宣布通过"。

### 5.3 用例分布（63 个）

| 套件 | 数量 | 覆盖 |
|---|---|---|
| `TcpTransportConfigTest` | 6 | Profile/对齐 Unknown、端口 0、地址空、超时 0、尺寸越界 |
| `TcpTransportCapabilityTest` | 2 | IPv6 远端/本地 → `UnsupportedFeature` |
| `TcpTransportConnectionTest` | 3 | 无监听 connect 失败、accept 超时（实测 301 ms ≈ 配置 300 ms）、端口回填 |
| `TcpEndToEndTest` | 14 | 双 Profile 编帧、分片/粘包、断链、Close 语义、CTR、上限拒绝 |
| `TcpStreamDecoder*`（B-T1 沿用） | 38 | 见 B-T1 记录 |

### 5.4 关键端到端断言（用原生 Socket 手写对端）

测试**不以"两端都用我们的实现"自证**，而是用原生 Socket 手写对端字节序列：

- `MasterSendsAsamV14FrameWithoutTail`：断言 LEN = 真实 Packet 长度，**且绝不附加 Tail**；
- `MasterSendsAsamV15FrameWithAlignmentTail`：Packet 长 3、Header 4 字节 → 7 字节，
  对齐到 4 需 **1 字节 Tail**；断言 LEN **不含** Tail，第 8 字节为 FILL（本库填 0）；
- `ReceiveHandlesFrameSplitAcrossWrites`：逐字节写入 + 2 ms 间隔，验证
  "recv 边界与 Frame 边界无关"；
- `ReceiveHandlesTwoFramesInOneWrite`：一次写入两个完整帧（粘包），验证一次交付两帧。

---

## 6. 与计划的偏差（显式记录）

| 计划条目 | 实际 | 原因 |
|---|---|---|
| §5.6 用假 `SocketOps` 测 partial write / EINTR / EAGAIN | 未建注入夹具，这些分支未测 | `SocketOps` 注入点当前只开放名称解析；建立完整假表需新增设施，本批次未做。**属未覆盖，不是"已通过"** |
| §5.6 发送超时测试 | 未测（真实触发需真的等若干秒） | 同上 |

> **更正（m01092，删除原有错误结论）**：本记录早前版本曾写
> "XCPlite DTO 帧长字段用 DLC 而非 Packet 长度、V15 Bit32 下字段重叠"。
> **该结论是错的，已删除。** 读全 `thirdparty/XCPliteV1.2/src/xcpEthTl.c`
> 收发热路径后的实测事实：
> - 发送侧 `:189` `p.dlc = size;` —— `size` 就是 **XCP Packet 语义长度**，
>   与 ASAM TL V1.5 的 LEN 定义一致；
> - 接收侧 `:332-335` 先读 4 字节头（`XCPTL_TRANSPORT_LAYER_HEADER_SIZE 4`，
>   `xcptl_cfg.h:50`），再按 `msgBuf.dlc` **精确**读取，**不做**对齐跳过；
> - `XCPTL_PACKET_ALIGNMENT 4` 仅出现在 `xcptl_cfg.h:46` 的宏与注释中，
>   TCP 收发热路径**从未引用**它。
>
> 故 XCPlite 的 TCP framing 等同于 `AsamV14`
> （`[LEN=真实Packet长][CTR][Packet]`，无 LEN 外 Tail），
> **不存在字段重叠**。
>
> 教训：先前结论只读了 `xcptl_cfg.h` 的宏定义就外推装配语义，未读实际收发代码。
> 这与 §4 表格中"凭记忆编码"是同一类错误——**结论必须有代码级证据**。

> **诚实声明**：B-T2 目前的绿灯证明的是"自有闭环 + 手写对端字节序列正确"，
> **不等于**与真实第三方实现（XCPlite）的互操作已验证。互操作验证是 B-T3 §6.3
> Gate 的明确要求（"实施记录区分'自有闭环通过'和'独立实现互操作状态'"）。

---

## 7. B-T2 Gate 自评

| 计划 Gate 条目 | 状态 |
|---|---|
| 双 Profile 编帧与独占 Tail 语义 | ✅ 手写对端字节断言 |
| 字节流分片/粘包 | ✅ 逐字节与多帧单写用例 |
| 短写补齐 | ⚠️ 代码已实现（`WriteAll` 循环），**未经注入测试验证** |
| Close 打断阻塞 recv、幂等 | ✅ 实测 0 ms 与重复调用 |
| CTR 顺序 | ✅ 递增且与写入顺序一致 |
| 配置校验 fail closed | ✅ 6 个用例 |
| 全量测试通过 | ✅ 722 项无 FAILED |

**结论**：B-T2 主体完成并通过；短写/错误注入类分支标记为未验证。

---

## 8. 下一步（B-T3）

按计划 §6：`tests/tcp_test_slave.hpp/.cpp` 双 Profile Test Slave +
`XcpMaster` loopback 集成，Golden 样本须**严格复刻 XCPlite 语义**（裁决 A1）。