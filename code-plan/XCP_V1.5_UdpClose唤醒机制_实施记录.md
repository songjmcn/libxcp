# UDP Transport `Close()` 判据收口与唤醒机制实施记录

- **批次**：收尾修复（承接批次 B2 §4.3）
- **改动文件**：`src/udp_transport.cpp`、`include/libxcp/udp_transport.hpp`、`tests/udp_transport_test.cpp`
- **实施日期**：2026-10-22
- **用户授权**：`两者都做：先收口判据，再做唤醒机制`

---

## 1. 背景：从"flake"到"判据缺陷"再到"设计改进"

本条目的问题源自 `UdpTransportClose.CloseWhileBlockedReturnsPromptly` 长期被记为
"既有 flake"。经五个探针实测（见批次 B2 实施记录 §4.3）后确认：

1. **它不是 flake**，而是判据 off-by-one：原断言 `< 1000ms` 漏掉等号，
   而配置的轮询周期恰为 `1000ms`，实测耗时 1000.0～1001.1ms（200 次采样值域
   `[999,1001]`）。
2. **"flake"印象的真正来源**：约 **0.5%** 概率因"接收线程尚未进入 `recvfrom`
   就被要求退出"的启动竞态而在 30～74µs 内返回、意外过测。

**设计依据**（历史记录 `code-plan/XCP_1.3.0_实施记录_批次3_D5收口与命名一致性.md:265-269` 原文）：

> `Close()` 的返回延迟上限变为一个轮询周期。……若后续需要更高关闭实时性，
> 应改用自管道/事件对象唤醒，而**不应回退到"先关 Socket"**。

本记录同时落实上述两件事：判据收口 + 唤醒机制。

---

## 2. 第一步：判据收口

### 2.1 修正内容

`tests/udp_transport_test.cpp` 的 `CloseWhileBlockedReturnsWithinOnePollInterval`：

| 项 | 修正前 | 修正后 |
|---|---|---|
| 判据 | `EXPECT_LT(elapsed, 1000)` | `EXPECT_LE(elapsed, 1000 + 100)` |
| 语义 | 隐含"严格小于一个周期" | 明确"不超过一个周期 + 调度余量" |
| 同步 | 无 | 先做一次收发往返确认线程已阻塞 |
| 用例名 | `CloseWhileBlockedReturnsPromptly`（承诺实现没做的事） | 已更正 |

### 2.2 关于余量的两次修正（重要，供后人参考）

余量取值经过两轮修正，两次的错误性质不同：

1. **+1ms（第一版）**：为纠正 off-by-one 而把余量压到 1ms。方向对（off-by-one
   只需包含等号），但**幅度错**——它只考虑了 `duration_cast` 的截断误差，
   没考虑兜底路径的真实开销。
2. **+100ms（最终）**：由变异 E（人为让唤醒字节丢失）暴露。兜底路径实测耗时
   1005～1019ms，即「一个周期 + `select` 超时唤醒 + 线程调度 + join 握手」
   约 15～19ms。故取周期的 10% 作为余量，既容纳真实开销，又远小于两个周期，
   仍能捕获死锁与严重延迟。

### 2.3 判据仍有效的证据（变异 A）

把判据改回有缺陷的 `EXPECT_LT(elapsed, kPollIntervalMs)`：

```
tests/udp_transport_test.cpp:993: Failure
Expected: (elapsed.count()) < (static_cast<long long>(kPollIntervalMs)),
          actual: 1001 vs 1000
[  FAILED  ] UdpTransportClose.CloseWhileBlockedReturnsWithinOnePollInterval
```

**失败签名 `1001 vs 1000` 与历史缺陷的原始签名一致**，证明新判据确实修正了它。
且因用例已加入收发预热，此变异下是**稳定失败**而非 0.5% 偶发——这本身证明
预热的必要性。

> **证据边界（如实声明）**：变异 B（移除预热）连跑 12 次全部失败，**未复现**
> 偶发过测。这是预期的——0.5% 概率在 12 次样本中的期望命中次数仅 0.06。
> 因此"预热消除了竞态掩盖"这一结论，由实测概率数据（200 次采样 + 分档过测率
> 表）支撑，**不由变异 B 支撑**。

---

## 3. 第二步：唤醒机制（回环 UDP 自连 + `select`）

### 3.1 设计决策与理由

在三条候选路径中，经用户裁定采用**回环 UDP 自连**：

| 方案 | 结论 |
|---|---|
| `socketpair()` | POSIX 专有，**Windows 无对应原语**，否决 |
| `WSAEventSelect` / 平台分派 | Windows 与 POSIX 需两套等待逻辑，回归面翻倍，否决 |
| **回环 UDP 自连（采用）** | 三平台语义一致，且与既有 UDP 代码共用同一套错误处理辅助函数 |

**明确不回退到"`Close()` 先关数据 Socket"**：join 前关 Socket 会让句柄被 OS
立即回收并可能分配给本进程新建的 Socket，而接收线程此刻仍可能回调监听器，
迟到的操作会命中被复用的句柄，造成跨会话串扰（见 `src/udp_transport.cpp`
`Close()` 内注释）。

### 3.2 实现结构

**`include/libxcp/udp_transport.hpp`**（private 区新增）：

```cpp
/// @brief 唤醒 Socket 句柄对：用于让 Close() 立即打断阻塞中的接收线程
struct WakeupPair;

/// @brief 创建回环 UDP 自连唤醒对；失败返回 nullptr（调用方回退到轮询）
static std::unique_ptr<WakeupPair> CreateWakeupPair();

/// @brief 触发一次唤醒（幂等、非阻塞、线程安全）；Close() 调用
static void SignalWakeup(WakeupPair* pair) noexcept;

/// @brief 排空唤醒 Socket 中的待读字节，避免误当作数据
static void DrainWakeup(WakeupPair* pair) noexcept;

/// @brief 唤醒 Socket 对（非拥有指针的 pimpl）；Open() 创建、Close() 释放
std::unique_ptr<WakeupPair> m_wakeup_;
```

**`src/udp_transport.cpp`**：

```cpp
struct UdpTransport::WakeupPair {
#if defined(_WIN32)
    WinsockSession winsock;                      ///< RAII 引用 WSAStartup/WSACleanup
#endif
    SocketHandle read_handle{kInvalidSocket};    ///< 本端（绑定后等待可读）
    SocketHandle write_handle{kInvalidSocket};   ///< 对端（用于写入唤醒字节）
    sockaddr_in read_addr{};                     ///< read_handle 绑定的实际地址
};
```

关键实现要点（均由实测驱动，非凭经验假定）：

1. **读端必须设为非阻塞**（POSIX `fcntl O_NONBLOCK` / Windows `ioctlsocket FIONBIO`）。
   原设计遗漏此项，会导致 `DrainWakeup` 的循环在最后一次 `recv` 上挂住。
2. **`Open()` 中创建失败不致命**：`m_wakeup_ == nullptr` 时仅告警，
   `ReceiveLoop` 用 `receive_poll_interval_ms` 兜底，退出延迟退化为修复前行为。
3. **`Close()` 顺序**：置 `m_running_=false` → `SignalWakeup()` → `join()`
   → 关数据 Socket → 释放唤醒对。
4. **`ReceiveLoop` 用 `select` 同时等数据 Socket 与唤醒 Socket**，
   timeout 仍取 `receive_poll_interval_ms` 作为**兜底轮询**。

新增 include：`#include <sys/select.h>`（POSIX 侧）。

### 3.3 实测效果

| 指标 | 唤醒前 | 唤醒后 |
|---|---|---|
| `CloseWhileBlocked…` 耗时 | 1018ms | **52ms** |
| `WakeupRepeatedOpenCloseStaysImmediate`（200 次） | — | **11ms 总计** |
| `UdpTransportClose` 套件总计 | 12492ms | **368ms** |

### 3.4 验证矩阵（变异测试）

| 变异 | 内容 | 结果 | 结论 |
|---|---|---|---|
| **A** | 判据改回 `EXPECT_LT(…, 1000)` | `1001 vs 1000` 失败 | 判据修正有效 |
| **B** | 移除收发预热 | 12/12 失败，未复现偶发 | 证据不足，见 §2.3 边界声明 |
| **C** | 禁用 `SignalWakeup` | 两用例失败：`949 vs 100`；85/200 次超时 | 唤醒路径断言有效 |
| **E** | 模拟唤醒字节丢失（保留真实兜底） | **兜底用例 ✅ 1019ms**；两唤醒用例 ❌ | 两条路径断言**各司其职、互不误报** |

**变异 E 是最有价值的验证**：它形成了理想的区分——唤醒失效时，
"兜底仍生效"与"快路径失效"被两组不同断言分别、正确地判定。

### 3.5 两个被证伪的假设（如实记录）

实施过程中我有两个判断被实测推翻，记录于此以免后人重蹈：

1. **变异 D（"删掉兜底超时 → 死锁"）设计错误。**
   POSIX `select(n, ..., &{0,0})` 的语义是**立即返回**，而非永久等待
   （永久等待须传 `nullptr`）。故该变异制造的是**忙等空转**，不是死锁。
   探针显示 `select returned ready=0` 在 31µs 内循环 10 次，CPU 空转。
   该变异**无效**，不能用来验证兜底。
2. **"`receive_poll_interval_ms = 0` 会导致忙等烧 CPU"——不成立。**
   我曾准备为此加保护，但查证发现 `src/udp_transport.cpp:367-372` 已有校验：

   ```cpp
   if (m_config_.receive_poll_interval_ms == 0U) {
       LIBXCP_LOG_ERROR(
           "UDP open rejected because receive poll interval is zero");
       throw detail::MakeInvalidArgument(
           "receive_poll_interval_ms 必须大于 0");
   }
   ```

   既有防线已覆盖该风险，未做多余改动（避免了"修一个不存在的问题"）。

### 3.6 资源泄漏压力测试

探针 `/tmp/probe_leak.cpp`：两轮各 500 次 `Open()`/`Close()`，观测 `/dev/fd` 句柄数：

```
起始 fd 数 = 4
第 1 轮 500 次开关后 fd 数 = 4
第 2 轮 500 次开关后 fd 数 = 4
```

**1000 次开关后 fd 数恒定，零句柄泄漏**（唤醒对每对 2 个句柄均正确释放）。

### 3.7 失效回退路径验证

`WakeupRepeatedOpenCloseStaysImmediate` 在变异 C 下的数据（85/200 次 ≥100ms）
与 §1 所述 0.5% 启动竞态自洽：未预热时部分样本撞上启动窗口而快速返回，
其余等满周期。这从另一个角度交叉验证了探针结论。

---

## 4. 测试用例增删

| 用例 | 动作 | 说明 |
|---|---|---|
| `CloseWhileBlockedReturnsWithinOnePollInterval` | 修正判据 + 加预热 | 锁定**慢路径兜底**上界 |
| `WakeupMakesCloseImmediateWhenBlocked` | **新增** | 锁定**快路径**：阻塞态下 `Close()` < 100ms |
| `WakeupRepeatedOpenCloseStaysImmediate` | **新增** | 200 次开关，验证唤醒**稳定生效**、无退化 |
| `StartupRaceWindowReturnsImmediately` | **删除** | 前提被撤销（见下） |

**关于删除**：该用例的前提是"瞬间返回只能靠启动竞态偶然发生（0.5%）"。
唤醒机制落地后，瞬间返回成为**常态能力**（实测 200/200 全部微秒级返回），
前提不成立。经用户裁定删除，其价值由 `WakeupRepeatedOpenCloseStaysImmediate`
承接——用重复次数直接断言稳定性，比"能否命中竞态窗口"更有意义。

---

## 5. 回归结果

| 测试目标 | 结果 |
|---|---|
| `libxcp_tests` | **614 tests，613 passed / 1 skipped / 0 failed** |
| `XcpliteIntegration`（真实跨进程 UDP） | **49 tests，49 passed** |
| `A2lGolden` | 45 tests，43 passed / 2 skipped（既有 skip） |
| `A2lE2E` | 10 passed |
| `A2lSmoke` | 111 checks，0 failures |
| `MeasurementAdapterTest` | 16 passed |

用例总数由 612 → **614**（新增 2 个唤醒用例，删除 1 个竞态用例，净 +1；
另有既有用例名更正）。

**跨平台**：Windows 分支（`ioctlsocket`/`FIONBIO`/`select(0,…)`）与 POSIX 分支
（`fcntl`/`O_NONBLOCK`/`select(FD_SETSIZE,…)`）均已编写并逐行复核；
本机为 macOS，Windows 分支**未在真机编译验证**（如实声明）。

---

## 6. 代码风格

对新增代码执行 `clang-format --dry-run --Werror` 检查，**新增区间零违规**。
刻意只格式化新增代码，未触碰既有代码的格式违规（如
`src/udp_transport.cpp:388/395/597/829` 四处为既有风格，避免污染 diff）。

---

## 7. 经验教训

1. **`git diff` 后必须确认二进制真的重建了。** 本实施中曾有两次 `cmake --build`
   报 `Built target` 而未重编 `udp_transport.cpp`，导致变异"未复现"是假象。
   发现手段：把临时探针日志（`PROBE:`）编入后用 `strings lib.a` 核对，
   再用独立探针交叉验证。**改后验不能只看构建命令的退出码。**
2. **引用接口前必须核实其存在。** 本实施中我曾两次凭记忆写出不存在的接口
   （`slave.WaitForPeerRegistered()`、`transport.LocalPort()` /
   `listener.WaitForWarning()`），均在编译前查证并改正。教训：写测试代码同样
   适用"写前读"。
3. **变异测试的变异本身要验证有效性。** 变异 D 设计错误（`select` 的 `{0,0}`
   是立即返回而非永久等待），若不复核就会得出"兜底无效"的错误结论；
   而变异 D 初次运行时甚至因未重编而"通过"，双重假象。
4. **判据容差落在实现真实值的抖动区间内时，必须重复采样定分布。**
   本条目最早的"flake"定性、以及我第一版的"稳定在边界打平"说法，都是
   同一类错误——用少量观测给一个应测分布的现象下定性。
5. **余量既要覆盖实现开销，又要保持鉴别力。** 压到 +1ms 会误报真实开销，
   放到 +1000ms（两个周期）会失去鉴别力。取周期的 10% 是两者的折中，
   且该值由实测开销（15～19ms）反推，而非拍脑袋。