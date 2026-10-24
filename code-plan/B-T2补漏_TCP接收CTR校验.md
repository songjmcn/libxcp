# B-T2 补漏记录：TCP 接收 CTR 校验

> 触发：用户要求"先补齐 CTR 三项"（`m01455`）。核查实现时发现 **CTR 校验在
> TCP 侧完全未实现**，遂按用户裁决的方案 A（`m01502` 同意）先实现功能、
> 再补测试。
> 契约依据：`code-plan/TCP传输_实施计划_B-T1至B-T5.md` §1.5（CTR 契约）。

---

## 1. 问题定性：不是测试缺口，是功能缺口

B-T2 声称完成了 TCP 传输层，但 §1.5 定义的接收 CTR 契约**一行都没实现**。

### 1.1 证据（全部工具实测）

| 位置 | 实际代码 | 结论 |
|---|---|---|
| `src/tcp_stream_decoder.cpp:160` | `current_ctr_ = static_cast<DatagramCtr>(ctr);` | 仅写入视图，**不校验** |
| `src/tcp_transport.cpp:788` | `LIBXCP_LOG_INFO("TCP RX frame ctr={}...")` | 仅打日志，**不校验** |
| `grep expected_ctr\|last_ctr\|recv_ctr` | 命中**全部**在 `src/udp_transport.cpp` | TCP 侧**无任何**基线状态 |

对比：UDP 侧实现完整——`src/udp_transport.cpp:865-920` 的
`UdpTransport::HandleFrame` 含基线建立、模 65536 期望值、前向距离分类、
重复/后向丢弃；成员见 `include/libxcp/udp_transport.hpp:162-166`
（`m_recv_mutex_`、`m_last_recv_ctr_`、`m_recv_baseline_established_`）。

**即：`frame.header.ctr` 被解出来、被打印出来、然后丢掉。**

### 1.2 为什么这比"测试没写"更严重

若直接按原计划补三个 CTR 用例，它们只能断言到"帧被正常接收"——因为库确实
不检查 CTR。那等于**用绿灯把一个未实现的契约固定成"已验证"**，比不写测试
更有害。

且这正是 TCP 与 UDP 差异最大的地方：UDP 有消息边界，丢包由协议层语义承担；
TCP 是无边界字节流，**CTR 是发现"对端漏发 / 我方漏读"的唯一手段**。
计划 §8.2 也把"严重 CTR violation"列为断链原因。

---

## 2. 实现（方案 A）

### 2.1 新增私有接口

`include/libxcp/tcp_transport.hpp`：

```cpp
bool CheckReceiveCtr(DatagramCtr ctr, std::string* out_reason);
```

新增私有成员（非公开接口，实现既有契约所必需）：

```cpp
std::optional<DatagramCtr> m_last_recv_ctr_;      // 最近一次已接受的接收 CTR
bool m_recv_baseline_established_{false};         // 基线是否已建立
```

> 与 UDP 的差异：UDP 因多条并发收包路径需要 `m_recv_mutex_` 保护；
> TCP 的 RX 状态**只在接收线程内**读写（`DispatchDecodeResult` 的唯一调用者
> 是 `ReceiveLoop`），故**不需要额外互斥**。

### 2.2 校验流程（`src/tcp_transport.cpp`）

按 §1.5 逐条落地：

| 步骤 | 条件 | 动作 |
|---|---|---|
| 1 | 基线未建立 | 接受并记基线，**首值任意**（§1.5："允许首值任意"） |
| 2 | `ctr == previous + 1 mod 65536` | 接受，推进基线 |
| 3 | `AsamV15` 且非连续 | **断链**（缺号/重复/后向/歧义一律违约） |
| 4 | `AsamV14` 且前向距离 `1..0x7FFF` | **接受并推进**，上报诊断 |
| 5 | `AsamV14` 且距离 `0x8000..0xFFFF` | **断链**（后向/重复/方向歧义） |

关键设计点：

- **前向距离**用 `CtrForwardDistance(base, value)`（模 65536），与
  `src/udp_transport.cpp:152` 算法一致，但 TCP 侧按自身策略重新实现——
  §1.5 末条明确"UDP 原有半域容错策略不变，**禁止直接复用为 TCP 策略**"。
  UDP 是"丢弃该帧但保持连接"，TCP 是"一律断链"，语义不同，不做共享抽象。
- **`0xFFFF -> 0` 回绕**：因用模运算判等，天然被接受，无需特判
  （§1.5："`0xFFFF -> 0` 是正常回绕"）。
- **断链走既有单一出口**：`CheckReceiveCtr` 返回 false → `DispatchDecodeResult`
  返回 false → `ReceiveLoop` 设 `close_reason` 并 break → 统一
  `OnTransportClosed`（符合 §397"恰好一次"要求）。
- **CTR 校验先于长度上限检查**：CTR 违约意物流已不可信，此时"继续投递后续帧"
  没有意义，必须整条连接终止。

### 2.3 前向缺号的双通道上报

`AsamV14` 前向缺号按 §1.5 应"只记录丢帧诊断并继续"。**仅写日志不足以满足
"诊断"**——`OnTransportWarning` 才是调用方可观测的面。故同时：

```cpp
LIBXCP_LOG_WARNING("{}", gap_text);            // 事后追溯
listener->OnTransportWarning(SanitizeReason(gap_text));  // 上层即时感知
```

> 这是测试驱动发现的：首版只写了 `LIBXCP_LOG_WARNING`，
> `AsamV14ToleratesForwardGap` 用例断言 `Warnings()` 含"前向缺号"时失败。

### 2.4 `Open()` 复位

```cpp
m_last_recv_ctr_.reset();
m_recv_baseline_established_ = false;
```

必要性：若不复位，重连后首帧会拿**上一条连接**的最后 CTR 去比对，从而被误判
为违约。与发送 CTR 复位同址（`m_send_ctr_.store(0)`），两者都是"每次成功
Open 后重新开始"。

### 2.5 类文档补充

`include/libxcp/tcp_transport.hpp` 新增"@par 接收 CTR 策略"段，逐条写明共同
规则与两种 Profile 的差异，并显式标注"**这是 TCP 与 UDP 的关键分歧点**"。

---

## 3. 测试（9 项，全绿）

新增套件 `TcpReceiveCtrTest`（`tests/tcp_transport_test.cpp`），复用既有
`TcpEndToEndTest` 的原始 Socket 对端夹具，自行手写 wire 字节：

| 用例 | 验证 |
|---|---|
| `AsamV14AcceptsArbitraryFirstCtr` | 首值 `0xABCD` 被接受（§1.5 首值任意） |
| `AsamV14AcceptsConsecutiveCtr` | 连续 CTR 0/1/2 全部投递 |
| `AsamV14ToleratesForwardGap` | 跳号 +5 后**继续**，且上报"前向缺号"诊断；后续以新值为基线 |
| `AsamV14RejectsBackwardCtr` | 后向 `0x0005 -> 0x0004` **断链**，违约帧不投递 |
| `AsamV14RejectsDuplicateCtr` | 重复 `0x0007 -> 0x0007` **断链** |
| `AsamV15RejectsForwardGap` | 缺号 `0x0000 -> 0x0003` **断链**（与 V14 相反） |
| `AsamV15RejectsBackwardAndDuplicateCtr` | 后向 `0x0010 -> 0x000F` **断链** |
| `CtrWrapsFromFfffToZero` | `0xFFFF -> 0x0000` 两种 Profile 均接受（回绕） |
| `FirstFrameAfterReopenEstablishesFreshBaseline` | 重开后首帧 `0x9999` 重建基线，不与旧基线比对 |

### 3.1 测试侧不共用生产编码器

新增 `MakeWireFrame(payload, ctr)` helper **手写** wire 字节，不调用
`EncodeEthernetFrameAligned`。理由：若收发两侧共用同一份编码实现，"能解析"
只能证明本库自洽——同一个误解会在两侧对称出现而照样通过。且手写才能任意
构造违约 CTR。

### 3.2 一处既有用例的必要修正（真实回归）

`TcpEndToEndTest.ReceiveHandlesTwoFramesInOneWrite` 在加入校验后**失败**。

**根因**：该用例（`tests/tcp_transport_test.cpp:606-607`）原本给两帧都写
CTR `0x00`。CTR 校验上线前这无关紧要；上线后第二帧被**正确判定**为
duplicate 而断链。

**判定**：这是**用例自身的缺陷**，不是实现回归。该用例的意图是"一次 write
的粘包能被拆成两帧"（TCP 重组），与 CTR 语义无关。

**修复**：两帧改用连续 CTR（`0x0000`、`0x0001`），并在注释中说明原因，
使该用例继续只验证它本该验证的重组行为。

> **教训**：新增的校验逻辑会让"以前恰好能过"的用例暴露出来。修复时必须先
> 判断"是对端/用例违规"还是"实现错了"——本例前者，故改用例而非改实现。

---

## 4. 测试结果

```
[==========] 749 tests from 48 test suites ran.
[  PASSED  ] 749 tests.
```

| 阶段 | 用例数 |
|---|---|
| B-T3 结束 | 740 |
| 本批次新增 CTR 用例 | +9 |
| **合计** | **749，全绿** |

回归无遗漏：全量套件仅 `ReceiveHandlesTwoFramesInOneWrite` 一项曾失败，
已定性为用例缺陷并修正（见 §3.2）。

---

## 5. 对计划文档 Gate 的影响

B-T2 的既有 Gate 未包含 CTR 校验条目，本次补漏后应视为**已达到** §1.5
契约。建议后续在 B-T5 验收中把 CTR 行为纳入互操作检查：

- 与真实 XCPlite（AsamV14 语义）连接时，对端队列丢弃导致的 CTR 缺号
  **不得**导致我方断链——这正是 `AsamV14ToleratesForwardGap` 保护的场景；
- 对端若出现重复/后向 CTR，应断链并给出可定位的诊断文本。

---

## 6. 剩余未覆盖（诚实声明）

本批次只补齐 CTR 校验与其测试。B-T3 记录 §5 列出的其余故障注入缺口
（RST、AsamV15 三种 Tail 形态、corrupt LEN、标定写入回读、
Disconnect 顺序断言等）**仍未覆盖**，未因本次改动而改变。

`corrupt_len` 与多帧单 write 的注入能力已存在（`TcpFaultInjection`
的 `corrupt_len_n`；`ReceiveHandlesTwoFramesInOneWrite` 用例）。

---

## 7. 下一批次

**B-T4**：`thirdparty/a2l-sdk` endpoint 语义与 Profile/版本来源。
用户已确认（`m01456` 更新后的 AGENTS.md）：**`thirdparty/a2l-sdk` 允许修改**，
其余三方库仍禁止。