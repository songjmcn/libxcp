# XCP V1.5 批次 B2 实施记录（T-02～T-07 传输层对齐与带宽治理）

- 批次：B2（方案 §1.9A 的传输层收口；与 B1「版本发现」同属批次 B，拆为两段实施）
- 实施日期：2026-10-22
- 规范基准：`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.pdf`（协议层，**唯一权威**）
  - 传输层依据：**官方 Ethernet TL V1.5 原始 PDF**，经 `pypdfium2` 提取为 `/tmp/xcp15_tl.txt` 后逐条引用
  - ⚠ 用户裁决 m01451：不再参考 Markdown 版规范（图片与正文存在错位，已证实会误导）
- 前置批次：C / D / E1 / E2 / F / B1 均已交付

---

## 1. 规范依据

### 1.1 PACKET_ALIGNMENT 与 Tail FILL（TL V1.5 §4.3.2.2）

规范原文（`/tmp/xcp15_tl.txt`，TL PDF 第 15 页附近）：

> "Depending on the value of PACKET_ALIGNMENT, the **tail** can consist of
> 0, 1, 2 or 3 FILL bytes."

关键结论（决定了整个批次的设计）：

1. **对齐是 Packet 级（XCP Packet + 4 字节 Header），不是 Frame 级。** 校验对象是
   `Header + XCP Packet + Tail`，即"后继 Packet 的起始地址"必须对齐。
2. **只对后继 Frame 有意义。** 单帧 Datagram 无需 Tail（后面没有东西可对齐）；
   多帧 Datagram 中每个非末帧都需按对齐补齐。
3. **FILL 字节内容必须可被忽略。** 解码侧不能依赖 FILL 的值。
4. **LEN 字段不包含 Tail。** `LEN` 只数 `CTR + XCP Packet`，Tail 长度由
   `LEN` 与 alignment **推导**，不是独立字段。
5. **合法 Tail 长度只有 0/1/2/3。** 超出即说明对齐参数不一致或报文损坏。

### 1.2 带宽上限（TL V1.5 §7.1）

规范原文（`/tmp/xcp15_tl.txt:650-724`，TL PDF 第 25-27 页）：

> "For the Ethernet transport layer, it is only possible to ... the limit of bus
> bandwidth ..."

§7.1 的 worked example 经复算确认：**规范文本与其示例的算术一致**，
本库按规范公式实现（`XcpFrameBytes()` 与 `TailBytesForAlignment()` 共用同一
Tail 公式，杜绝两处独立实现产生偏差）。

### 1.3 对齐与 TL 版本无关（重要边界）

- **确认 TL V1.5 不代表可假定 16/32-bit 对齐。** `PACKET_ALIGNMENT` 是**独立的
  传输布局参数**，必须在 A2L 中声明或由调用方显式指定。
- 因此本批次**不提供**"检测到 V1.5 就自动用 32-bit"这类推断逻辑。

---

## 2. 修改范围

### 2.1 T-02：`PacketAlignment` 强类型（`include/libxcp/protocol_types.hpp`）

新增 143 行。核心类型：

```cpp
enum class PacketAlignment : std::uint8_t {
    Unknown = 0,  ///< 未声明/未知（禁止静默当 8-bit 处理）
    Bit8,         ///< 8 bit = 1 字节
    Bit16,        ///< 16 bit = 2 字节
    Bit32,        ///< 32 bit = 4 字节
};

[[nodiscard]] constexpr std::uint8_t AlignmentBytes(PacketAlignment) noexcept;
[[nodiscard]] constexpr PacketAlignment ParsePacketAlignment(std::uint8_t raw_bits) noexcept;
[[nodiscard]] constexpr std::uint8_t TailBytesForAlignment(PacketAlignment,
                                                           std::size_t frame_without_tail_bytes) noexcept;
```

设计要点：

- **位（bit）与字节（byte）严格分离**：`PacketAlignment` 存位宽，`AlignmentBytes()`
  给字节数。文档注释明确要求"字节数请用 `AlignmentBytes()`"，防止调用方自行换算出错。
- **`Unknown` 是首类公民**：`ParsePacketAlignment(0)` 返回 `Unknown` 而非 8，
  且 `Unknown` 在传输层 `Open()` 会被**显式拒绝**（见 T-06），不会静默退化。
- `TailBytesForAlignment()` 是**唯一** Tail 公式来源，编码/解码/预算三处共用。

### 2.2 T-03/T-04：按对齐编解码（`include/libxcp/udp_header_codec.hpp`、`src/udp_header_codec.cpp`）

新增：

```cpp
[[nodiscard]] UdpFrame EncodeUdpFrameAligned(BytesView xcp_packet,
                                             DatagramCtr ctr,
                                             PacketAlignment alignment);
[[nodiscard]] std::optional<std::vector<UdpFrame>> DecodeUdpDatagramAligned(
    BytesView datagram, PacketAlignment alignment);
```

- 原 `EncodeUdpFrame()` / `DecodeUdpDatagram()` **保持不变**，语义等价于 `Bit8`
  （有专门用例 `UdpAlignedEncode_LegacyEncodeIsExactlyBit8` 锁死该等价关系，
  保证既有调用方零行为变化）。
- 解码侧：按推导出的 Tail 长度**跳过 FILL**，并把 Tail 计入帧总长；
  **截断的 Tail 导致整个 Datagram 被拒绝**（原子性策略，§8.1 第 7 条）。
- `Unknown` 传入编解码器**抛错而非假定 8-bit**。

### 2.3 T-05：A2L 对齐自动贯通（`tools/xcp_master/main.cpp`）

`UdpTransportConfig::packet_alignment` 现在从 A2L 的 `IF_DATA XCP` 取值：

- 遍历 `XcpInfo()->transports`，取 UDP 端点的 `packet_alignment`（位宽）；
- 命中多个**互相矛盾**的声明时**拒绝自动建链并报错**，不静默覆盖；
- 无声明时保持默认 8-bit，但**明确打印告知依据**，避免用户误以为对齐来自 A2L：

```
A2L 未声明 PACKET_ALIGNMENT，使用默认 8-bit 对齐；如对端使用 16/32-bit 请在 A2L 中声明。
```

上游取证（`thirdparty/a2l-sdk/a2lbridge/src/a2l_dto_map.cpp:488-491`）确认
a2lbridge 已把上游原码 `0/1/2 → 8/16/32` 换算为位宽，故本侧用
`ParsePacketAlignment()` 直接收强类型，映射链完整闭合。

### 2.4 T-06：能力边界与配置门禁（`src/udp_transport.cpp`）

`Open()` 新增两道前置校验：

1. **`packet_alignment == Unknown` → 抛 `InvalidArgument`**。
   理由：报文布局（是否含 Tail、含几字节）无法确定时静默按 8-bit 处理会与对端不兼容，
   且故障表现为"数据错位"而非"明确报错"，极难定位。
2. **IPv6 字面量 → 抛 `UnsupportedFeature`**。
   本版本基础 Profile 只承诺 **IPv4 UDP 单播**。用 `looksLikeIpv6()`（判据：含 `':'`）
   把 IPv6 **显式识别为能力缺失**，而不是让 `inet_pton(AF_INET,...)` 静默失败后
   给出笼统的"地址非法"——后者会让调用方误以为是笔误。

   `remote_host` 与 `local_host` 都做该校验；`127.0.0.1` / `0.0.0.0` 等正常 IPv4
   有回归保护用例（`Ipv4HostsWithColonFreeTextStillAccepted`）。

### 2.5 T-07：链路带宽预算（**新增文件** `include/libxcp/link_budget.hpp`、`src/link_budget.cpp`）

350 行新模块，公开接口：

```cpp
enum class UnknownLinkPolicy : std::uint8_t;   // AllowUnverified / Reject
struct LinkBudgetConfig;                        // 标称速率/实际可用速率/周期等
enum class BudgetVerdict : std::uint8_t;        // Verified / Unverified / OverBudget
struct EventBudgetInput;   struct EventBudgetResult;   struct LinkBudgetResult;

[[nodiscard]] std::size_t XcpFrameBytes(std::size_t dto_packet_bytes,
                                        PacketAlignment alignment) noexcept;
[[nodiscard]] LinkBudgetResult ComputeLinkBudget(const LinkBudgetConfig&,
                                                 std::span<const EventBudgetInput>,
                                                 PacketAlignment);
```

设计要点（均为"不伪造证据"原则的落实）：

- **Tail 计入预算**：`XcpFrameBytes()` 复用 `TailBytesForAlignment()`，与真实编码
  **同一个**公式。
- **`Unknown` 对齐或未知周期 → 绝不判 `Verified`**，最多 `Unverified`；
  `Reject` 策略下直接阻断 DAQ 启用。
- **实际可用速率只向下修正**：`actual_available` 高于标称时被忽略（不放大预算）。
- **空 Event 列表 + 参数齐全 → `Verified`**（不是缺陷，是正确的边界）；
  `nominal_rate == 0` 视为**未配置**而非"零带宽"。
- `AllowOverBudget` 只把结论**降级为 `Unverified`**，绝不升为 `Verified`。

---

## 3. 测试更新与新增

### 3.1 新增用例（均为本批次新写）

| 文件 | 用例组 | 说明 |
|---|---|---|
| `tests/udp_header_codec_test.cpp` | `UdpAlignedEncode.*`、`UdpAlignedDecode.*` | 11 个：Bit8/16/32 编码、FILL 内容不参与语义、截断 Tail 拒绝整个 Datagram、Bit8 把残留字节当非法、旧 API 与 Bit8 完全等价、`Unknown` 抛错 |
| `tests/link_budget_test.cpp` | `LinkBudgetFrameBytes.*`、`LinkBudget.*` | **15 个**：规范 worked example 复算、Tail 计入预算、缺配置默认 AllowUnverified、Reject 策略阻断、超预算默认拒绝、AllowOverBudget 只降级、未知周期不臆造、`Unknown` 对齐不判 Verified、实际速率向下修正/不向上放大、多 Event 求和、空列表 Verified、零标称速率视为未配置 |
| `tests/udp_transport_test.cpp` | `UdpTransportCapability.*` | IPv6 显式 `UnsupportedFeature`（remote + local）、IPv4 不被误伤 |
| `tests/xcp_udp_loopback_test.cpp` | `UdpLoopbackAlignment.*` | **4 个端到端**：16-bit / 32-bit / 8-bit 真实 UDP Socket 会话读回、**双方对齐不一致时必须失败而非静默错位** |
| `tests/a2l_golden_test.cpp` | `A2lGoldenTest.PacketAlignmentFlowsIntoLibxcpType` | A2L → libxcp 强类型 → Tail 公式的完整贯通 |

**测试夹具增强**：`test::UdpTestSlave` 新增 `SetPacketAlignment(PacketAlignment)`，
使测试 Slave 的**收发两侧**都按指定对齐工作（默认 `Bit8`，既有用例语义不变）。
其 `Unknown` 传入会抛错——测试 Slave 故意**不做**"猜测对齐"的容错，以便暴露
Master 侧的静默假定。为此测试目标新增了对 `libxcp::libxcp` 的链接（`tests/CMakeLists.txt:197`），
因为 `A2lGolden` 原先只链 `a2lbridge`。

### 3.2 端到端用例的价值

单元测试只证明编解码器本身正确；`UdpLoopbackAlignment.*` 走**真实 UDP Socket**，
证明**传输层确实把配置传了下去**——即 `UdpTransportConfig::packet_alignment`
没有被某处硬编码的 8-bit 截断。这正是方案担心的集成缺陷形态。

其中 `MismatchedAlignmentFailsRatherThanCorrupts` 是**反例测试**：
Slave 用 32-bit、Master 用 8-bit，断言 `Connect()` **必须抛异常**。
它守护的是"宁可失败也不静默错位"这一安全属性。

---

## 4. 验证结果

### 4.1 全量回归（六个测试二进制，全绿）

| 二进制 | 结果 |
|---|---|
| `libxcp_tests` | **612 tests：611 passed / 1 skipped / 0 failed** |
| `A2lGolden` | 45 tests：43 passed / 2 skipped |
| `A2lE2E` | 10 passed |
| `MeasurementAdapterTest` | 16 passed |
| `XcpMasterSymbolPolicyTest` | 74 passed |
| `A2lSmoke` | 111 checks / 0 failures |
| `XcpliteIntegration` | 49 passed（含 XCPlite 真实互操作） |

### 4.2 变异测试（五组，证明新断言真的"咬人"）

| # | 注入的缺陷 | 结果 |
|---|---|---|
| 1 | `TailBytesForAlignment()` 恒返回 0（"忘记按对齐补 Tail"） | **8 个用例失败**，含编码、解码、预算、端到端 |
| 2 | 解码侧忽略 Tail（`tail = 0`，不跳过 FILL） | **5 个用例失败**，含两个端到端用例 |
| 3 | (`ParsePacketAlignment(32)` 误判为 `Bit8`) | **1 个用例失败**（A2L 贯通用例） |
| 4 | 传输层轮询超时放大到 30s（唤醒机制失效） | **1 个用例失败**（30s 后超时判失败） |
| 5 | （见 §5.3 的既有缺陷修复） | — |

变异全部还原，全仓 `grep MUTATION` 无残留。

### 4.3 既有 flake 的定性修正与修复

`UdpTransportClose.CloseWhileBlockedReturnsPromptly` 长期被记为"既有 flake"。
本批次核实后**推翻该定性**，但**第一版记录里的"稳定在边界打平"说法也不准确**，
经探针复测后更正如下。

#### 4.3.1 实测数据（探针见 §4.3.4）

轮询周期与 `Close()` 实际耗时的关系（每档 6 次，`/tmp/probe_close`）：

| 轮询周期(ms) | Close() 实测(ms) | 规律 |
|---|---|---|
| 50 | 50, 51, 0, 51, 51, 51 | |
| 100 | 101×5, 100 | |
| 250 | 251×6 | |
| 500 | 500, 501×5 | |
| **1000** | **1001×5, 1000** | ← 断言 `< 1000` 的档位 |
| 1500 | 1500, 1501×5 | |
| 2000 | 2001×5, 2000 | |

**规律：实测耗时 ≈ 轮询周期（+0～1ms 量化误差），完全线性、无离散步进。**

微秒级复测（`/tmp/probe_timing`，1000ms 档 12 次）：
`1000606, 1001065, 1000179, 1001055, 1001038, 1000514, 1001049, 1000388,
1001037, 1001031, 1001084, 1001060` —— **12/12 全部 ≥ 1000.000ms**。

#### 4.3.2 正确的机理：不是"打平"，而是"判据自始不成立 + 偶发竞态翻盘"

判据门槛扫描（`/tmp/probe_extreme`，1000ms 档 200 次，样本毫秒值域 `[999, 1001]`）：

| 判据 | 过测率 | 备注 |
|---|---|---|
| `< 500ms` | **0.0%** | 即第一版曾打算改成的"半个周期"方案——必然全失败 |
| `< 900ms` | 0.0% | |
| `< 999ms` | 0.0% | 真正"严于周期"的写法 |
| **`< 1000ms`（原判据）** | **0.5%（1/200）** | |
| `< 1001ms` | 24.0% | 仅差 1ms，过测率差 48 倍 |
| `< 1050ms` | 100.0% | |

结论分两层，第一版记录漏掉了第二层：

1. **主路径**：`Close()` 的正常路径就是**等满一个轮询周期**
   （接收线程阻塞在 `recvfrom`，靠 `SO_RCVTIMEO` 超时才发现 `m_running_` 为假）。
   因此耗时 ≈ 1000ms，**天生不满足 `< 1000`**。这不是"打平"，是判据与实现契约
   根本不相容——只不过差值恰好在 1ms 量级，看起来像打平。
2. **偶发翻盘**：约 **0.5%** 的概率它会以 **30～74µs** 返回而"意外过测"。
   机理是竞态：若 `Close()` 在接收线程**尚未进入 `recvfrom` 阻塞**时就把
   `m_running_` 置为 `false`，`while` 条件在进入循环体前即为假，线程立即退出，
   无需等待任何超时。用不同 `Open()`→`Close()` 间隔验证（`/tmp/probe_race`，
   各 30 次）：

   | Open 后等待 | 瞬间返回(<500ms)次数 | 等满超时次数 |
   |---|---|---|
   | **0µs** | **1/30**（最大 37µs） | 29/30 |
   | 1µs | 0/30 | 30/30 |
   | 10µs / 50µs / 100µs / 500µs / 2ms / 20ms | 0/30 | 30/30 |

   **只有 0µs 窗口能撞上该竞态**，与 0.5% 的实测概率吻合。

   **这解释了"既有 flake"这个错误印象的来源**：0.5% 的偶发通过足以让人在
   若干次运行中看到过它成功，从而误判为"环境抖动"。

#### 4.3.3 实现契约（决定了谁对谁错）

`Close()` **故意**不提前关 Socket（`src/udp_transport.cpp:378-386` 的长注释）：
join 前关 Socket 会让句柄被 OS 立即回收并可能分配给本进程新建的 Socket，
而接收线程此刻仍可能回调监听器，迟到的操作会命中被复用句柄，造成跨会话串扰。
因此唤醒依赖 `Open()` 设置的 `SO_RCVTIMEO` 轮询（`src/udp_transport.cpp:325-338`），
接收线程在 `src/udp_transport.cpp:537` 的 `isTimeoutError()` 分支 `continue`
回到 `while` 条件处检查关闭标记。

**设计上，`Close()` 的退出上界就是一个轮询周期。**"立刻返回"从来不是它的目标，
所以**错的是用例**（名字 `ReturnsPromptly` 与判据 `< 1000ms` 都承诺了实现
没做的事），不是生产代码。若当时真去"优化 Close 速度"，反而会破坏句柄复用防护。

#### 4.3.4 处置与验证

- 用例更名 `CloseWhileBlockedReturnsWithinOnePollInterval`，判据改为
  **不超过一个轮询周期 + 调度余量**（`< kPollIntervalMs + 500`），
  并把上述设计契约写进注释。
- 修正后连跑 **10 次全部通过**；反向变异（`tv.tv_sec` 放大到 30s）能让它
  失败（30s 后判失败），证明判据仍有效。
- 全量回归因此从"1 failed"变为 **0 failed**。
- 探针脚本：`/tmp/probe_close.cpp`（周期-耗时线性关系）、
  `/tmp/probe_timing.cpp`（微秒级复测）、`/tmp/probe_boundary.cpp`（旧判据
  PASS/FAIL 分布）、`/tmp/probe_race.cpp`（竞态窗口验证）、
  `/tmp/probe_extreme.cpp`（200 次门槛扫描）。均为临时探针，未纳入测试套件。

---

## 5. 未覆盖项与遗留

1. **`PacketAlignment` 尚未接入 DAQ 实际建链路径**。当前 `UdpTransport` 已按对齐
   收发，DAQ 的 DTO 打包也走同一传输层，但**DAQ 侧没有独立的对齐断言**。
   若后续要为 DAQ 增加带宽预算门禁，应调用 `ComputeLinkBudget()`。
2. **`ComputeLinkBudget()` 尚未被 `XcpMaster` 调用**。目前是可用的公开模块，
   接入点（DAQ 启用前校验）留待后续批次。
3. **IPv6 是显式不支持**，非"部分支持"。若后续需要，应实现完整路径而非放宽校验。
4. **TL V1.5 发布门禁**：`packet_alignment` 默认值仍为 `Bit8`
   （`include/libxcp/udp_transport_config.hpp:72`），这是**调用方的保守显式选择**，
   不代表"A2L 未声明时猜 8-bit"。传输层 `Open()` 的 `Unknown` 拒绝保证了
   不会出现"没配置却静默按 8-bit 跑"的情况。
5. **XCPlite 端到端互操作**：XCPlite 侧固定 8-bit 对齐，与本批次新增的
   16/32-bit 路径不冲突（默认值即 Bit8）。未做 XCPlite 的 16/32-bit 对拉验证。

---

## 6. 批次状态

| 批次 | 内容 | 状态 |
|---|---|---|
| A | 规范裁决与测试 Oracle | 已完成（`code-plan/XCP_V1.5_规范符合性裁决与修复方案.md`） |
| C | F-01 + F-02 + F-04 | 已交付 |
| D | F-09 + F-10 | 已交付 |
| E1 | F-08 → F-06 → F-07 | 已交付 |
| E2 | F-05 + F-12 | 已交付 |
| F | F-03 + F-11 | 已交付 |
| B1 | GET_VERSION + 版本发现 + V1.3/V1.4 降级回退 | 已交付 |
| **B2** | **T-02～T-07 传输层对齐与带宽治理** | **本批次已交付** |
| G | 文档收口与三平台验收 | 未实施 |

至此方案 §1.9A 传输层收口项（T-02～T-07）全部落地。仅剩**批次 G**
（变更日志、API/ABI 迁移说明、Windows/Linux/macOS 三平台验收）。

---

## 7. 关键教训

1. **"不是我引入的" ≠ "可以不管"，但也不能反过来急着"修"。** 本批次最初打算把
   `CloseWhileBlockedReturnsPromptly` 的断言上限改成半个周期，结果它变成
   **稳定失败**——这暴露了真相：`Close()` 本来就设计成等一个轮询周期。
   若当时直接改生产代码去"优化 Close 速度"，反而会**破坏**句柄复用防护这个
   真实设计意图。**先读懂实现为什么这样写，再判它与判据谁错了。**
2. **"偶发通过"比"稳定失败"危险得多。** 该用例主路径其实是**必然失败**
   （耗时恒 ≈ 一个轮询周期 ≥ 1000ms），但有 **0.5%** 概率被一个启动竞态
   （接收线程尚未进入 `recvfrom` 就已被要求退出）翻成 30～74µs 而意外过测。
   这 0.5% 足以让人在若干次运行中见过它成功，进而误判为"环境抖动"并放过它。
   **判据的容差落在实现真实值的抖动区间内时，必须用重复采样定分布，不能靠几次运行下结论。**
   我第一版记录把机理写成"稳定在边界打平"，也是只看了失败输出、没测分布——同一类错误的另一种形态。
3. **端到端测试不可省略。** 编解码器单测全绿，仍可能有"配置没传下去"的集成缺陷；
   真实 Socket 用例才能证明贯通。
4. **反例测试和正例同样重要。** `MismatchedAlignmentFailsRatherThanCorrupts`
   守护的是"不静默出错"这一安全属性，正例无法替代。