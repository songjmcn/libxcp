# `--tcp-profile auto` 证据收敛与候选验证（方案 A′）设计

## 1. 背景

`XCP on TCP` 的 `PACKET_ALIGNMENT` 与 Wire Profile 无法在会话建立前从线路本身读
出，A2L 是唯一先验来源；一旦两者与对端不一致，主站会按错误边界切帧，产生极难定
位的失步。当前实现的处理是：

- 严格 `--tcp-profile auto`：**无条件拒绝**（`src/transport_selection.cpp:115-119`），
  因为"连接成功"不能证明布局正确。
- `--tcp-profile auto-legacy`：按固定候选顺序"首个成功即采用"，成功不证明正确。

即 `auto` 名义存在但不可用。本设计把它变成**可判定**的自动选择。

## 2. 已证伪的路线（实测）

原设想是在 CONNECT 之前用只读探测（GET_STATUS）判定布局。实测（XCPlite V1.2，
TCP，Windows MSVC Release）证伪：

| 探测方式 | 对端行为 |
| --- | --- |
| 未 CONNECT 直接发 `[LEN=2][CTR=1][0xFD,0x00]` | 一个字节都不回；从站日志 `WARNING: handleXcpCommand: no valid CONNECT command` |
| 未 CONNECT 且带 14/30 字节 Tail | 直接关闭连接（`WinError 10054`） |

结论：未连接状态只接受 CONNECT，探测会退化为"靠超时猜"，正是既有禁令禁止的做法。
**方案 A′ 因此改为 CONNECT 之后的语义验证**，代价是每次候选会真实改变一次从站会
话状态（CONNECT → DISCONNECT），这一点必须如实写进 `--help` 与 README。

## 3. 实测支持的可判定性

```
8bit   CONNECT 响应 CTR=0 len=8 code=ff
       GET_STATUS#2 CTR=1 len=6 payload=ff 00 00 f8 00 00
       GET_STATUS#3 CTR=2 len=6 payload=ff 00 00 f8 00 00
       DISCONNECT   CTR=3 len=1 payload=ff
16bit  发送带 14 字节 Tail 的帧 → 对端立即复位连接（非超时）
32bit  发送带 30 字节 Tail 的帧 → 对端立即复位连接（非超时）
```

错误布局表现为**立即、显式**的协议层拒绝（复位或非法 LEN），而不是安静超时；
因此判定谓词可以严格到"不接受任何未经证实的候选"。

## 4. 目标与非目标

**目标**：`--tcp-profile auto` 在具备可判定证据时自动选出 `(Profile, alignment)`，
并在无法判定时明确失败而不是猜。

**非目标**：

- 不做连接内嗅探，不因首帧字节内容切换布局（既有禁令不变）。
- 不把 A2L 的 `PACKET_ALIGNMENT` 当作已确认证据（仍受受控快照与双源核对约束）。
- 不改变显式 `--tcp-profile` / `--packet-alignment` 路径的任何行为。
- 不改变 `auto-legacy` 现有语义（保留为风险兼容档）。

## 5. 组件设计

### C1 候选构建（新增公共头）

`include/libxcp/tcp_framing_candidates.hpp`

```cpp
struct TcpFramingEvidence {
    std::optional<PacketAlignment> a2l_alignment;   // 仅在 A2L 被双源核对后填写
    std::optional<TcpWireProfile> declared_profile; // 显式 --tcp-profile（非 auto）
    std::optional<XcpProtocolVersion> transport_version;  // 显式 --transport-version
};

[[nodiscard]] std::vector<TcpFramingCandidate> BuildTcpFramingCandidates(
    const TcpFramingEvidence& evidence, std::vector<std::string>* out_notes);
```

规则（顺序固定、可审计、不臆造）：

1. 有受控 A2L 证据的 alignment=16/32 → 前两位放 `{V15,16}` / `{V15,32}`（各按证据
   指向优先），随后 `{V15,8}`、`{V14,8}`。
2. A2L 证据为 8bit：`{V14,8}` 优先（与既有 `DefaultFramingCandidates()` 一致，8bit
   下两 Profile 字节等价），随后 `{V15,8}`、`{V15,16}`、`{V15,32}`。
3. 显式 TL 版本 1.4 → 只保留 V14 候选；TL 1.5 → 只保留 V15 候选（V1.5 语义下
   8bit 与 16/32bit 仍需试连区分）。
4. 无任何证据 → 等同于 `DefaultFramingCandidates()`，并在 notes 记录"无先验证据"。
5. 候选去重、数量上限 4，**不得**在运行期重排。

### C2 候选验证（新增公共头）

`include/libxcp/tcp_framing_verification.hpp`

```cpp
enum class FramingVerdict { Rejected, Inconclusive, Accepted };

struct FramingVerificationResult {
    FramingVerdict verdict{FramingVerdict::Rejected};
    std::string reason;          // 中文，进入日志/错误文本
    long long elapsed_ms{0};
};

[[nodiscard]] FramingVerificationResult VerifyTcpFramingCandidate(
    const TcpFramingProbeConfig& config, const TcpFramingCandidate& candidate);
```

> **实现期的签名修正（已落地）**：初稿把 `XcpMaster&` 作为入参，实现时改为只传
> 连接参数（`host`/`port`/两个超时）。原因是"每次验证必须用全新连接"——若由调用
> 方传入已打开的 master，就无法证明这条连接是新的，也无法保证验证后连接被彻底
> 关闭。由被调方自己 `Open`/`Connect`/`QueryStatus`/`Disconnect`/`Close` 才能把
> "一次验证 = 一条连接"写成不变式。`FramingVerificationResult` 与三态语义不变。

判定序列（全部满足才 `Accepted`）：

1. `Connect()` 成功，且 `GetSessionParameters().connect` 的 `max_cto >= 8`、
   `max_dto >= 8`、PL 主版本 `== 1`、TL 主版本 `== 1`；
2. `QueryStatus()` 连续两次均正响应，且 `state_number == 0`、`resource_protection`
   为空（会话刚建立时的合法状态）；
3. 两条 `GET_STATUS` 之间 CTR 严格 +1（由 transport 既有严格性保证；若 transport
   报 CTR 异常则该候选判 `Rejected`——这条同时覆盖"把 Tail 当下一帧头"的残留问题：
   残留会把第二个命令解析成非法 LEN，从而在第二帧失败）；
4. `Disconnect()` 正响应，且对端有序关闭。

`Inconclusive` 的判定：仅当失败原因是**传输层超时**且没有任何协议层违规证据时。
纯超时**永不**被当作"候选被拒绝"的证据。

### C3 协商结果三态（已定：直接改造公共接口）

用户已拍板采用**破坏性变更**：`FramingAttemptFn` 由 `bool` 改为返回三态结果，
不保留旧重载。为控制依赖方向，三态类型拆到独立小头：

- `include/libxcp/tcp_framing_verdict.hpp`（无重依赖）：

```cpp
enum class FramingVerdict : std::uint8_t {
    Rejected,      // 对端显式拒绝：协议层违规/复位/响应码非法
    Inconclusive,  // 未决：仅传输层超时，无任何协议层违规证据
    Accepted,
};

struct FramingVerificationResult {
    FramingVerdict verdict{FramingVerdict::Rejected};
    std::string reason;        // 中文，进入日志与错误文本
    long long elapsed_ms{0};
};
```

- `include/libxcp/tcp_framing_negotiation.hpp`：包含上面的小头，改为

```cpp
using FramingAttemptFn = std::function<FramingVerificationResult(
    const TcpFramingCandidate&)>;

struct FramingAttemptOutcome {
    TcpFramingCandidate candidate{};
    FramingVerdict verdict{FramingVerdict::Rejected};  // 取代原 bool accepted
    std::string failure_reason;   // Accepted 时为空
    long long elapsed_ms{0};
};
```

- 语义：**首个 `Accepted` 胜出**；`Inconclusive` 不终止扫描但记录在案；全部结束时
  由 `Summary()` 汇总每项 verdict。非法候选（`Unknown`）仍不调用谓词，直接记
  `Rejected`。
- 同步改造的调用点：`tools/xcp_master/main.cpp`（auto 与 auto-legacy 两条路径）、
  `tests/tcp_framing_negotiation_test.cpp`、`tests/tcp_stream_decoder_test.cpp` 中
  相关断言。

### C4 `auto` 语义

`include/libxcp/transport_selection.hpp`：`wire_profile_auto` 保留，但
`src/transport_selection.cpp` 规则 8 不再无条件拒绝，而是：

- 置 `wire_profile_auto = true`、`wire_profile = Unknown`、`alignment = Unknown`；
- 在 `notes` 写明"auto 将对每个候选做一次 CONNECT/验证/DISCONNECT"；
- 与 `--packet-alignment`、`--preset xcplite`、显式 `--tcp-profile` 的互斥检查
  维持现状。

## 6. 数据流（auto 路径）

```
CLI → ResolveTransportSelection（不再拒绝 auto）
    → BuildTcpFramingCandidates(evidence: 受控 A2L / 显式 TL 版本)
    → 对每个候选：新建 TcpTransport + XcpMaster
                  → Connect → QueryStatus ×2 → Disconnect → 关闭连接
                  → VerifyTcpFramingCandidate 给出 Rejected/Inconclusive/Accepted
    → 首个 Accepted 胜出（其余候选不再试连）
    → 用胜出的 (Profile, alignment) 再新建一条连接做正式会话
```

关键点：**正式会话必须重新连接**，不复用试连得到的会话——试连做过
CONNECT/DISCONNECT，复用会把"验证过的状态"与"实际工作状态"混在一起。

## 7. 错误处理与退出码

| 情形 | 行为 |
| --- | --- |
| 有候选 `Accepted` | 用该候选重新连接；日志列出每个候选的 verdict 与耗时 |
| 全部 `Rejected` | 失败退出（非 0），文本说明"这些布局均被对端显式拒绝"，并给出显式指定参数的指引 |
| 存在 `Inconclusive` 且无 `Accepted` | 失败退出，文本明确"未决，不代表对端不支持"，提示核查网络或显式指定 |
| 验证过程中抛异常 | 该候选记 `Rejected` 并记录异常文本，异常不外泄 |
| 候选为空 | 视为配置错误，退出码与既有参数错误一致 |

## 8. 副作用与安全

- 每个候选一次 CONNECT/DISCONNECT，会话状态会被真实改变；`--help`、README、
  `code-plan` 记录都必须声明这一点。
- 不改变既有禁令：连接内不得切换布局；`Unknown` 不得进入 decoder。
- 试连失败必须彻底关闭连接（沿用 `FramingAttemptFn` 的既有约定）。

## 9. 测试方案

**单元（不需要真实对端）**

- `BuildTcpFramingCandidates`：六种证据组合的顺序、去重、上限、notes 文本。
- `VerifyTcpFramingCandidate` 的判定分支：用可脚本化的假 TCP 服务端（进程内
  socket）分别构造——正常响应、CTR 跳号、残留字节导致第二帧非法 LEN、
  超时无响应（→ `Inconclusive`）、错误响应码（→ `Rejected`）。
- `NegotiateTcpFraming` 新重载：三态排序、不重试、不重排、异常封装。

**跨进程（真实对端，沿用既有夹具）**

- XCPlite V1.2 TCP：auto 应选中 `{V14,8}`；日志中 16bit/32bit 候选为 `Rejected`。
- 自建 V1.5 从站 alignment 16/32：auto 应分别选中对应候选（验证"不是永远选 V14"）。
- XCPlite V2.2：auto 选中可解析候选并完成一次标定读回。
- 失败路径：端口无人监听（全部 `Inconclusive`）→ 退出码非 0 且文本为"未决"。

**跨平台**：三个 CI 工作流（Windows/MSVC、Linux、macOS）都要通过；POSIX 上额外
验证候选构建与 verdict 分类不依赖 Windows 特有行为。

## 10. 兼容性

- `--tcp-profile auto` 从"永远拒绝"变为"可用"，属于行为变更，需要在 README 与
  变更说明中标注。
- `auto-legacy`、显式 Profile、UDP 路径行为不变。
- `NegotiateTcpFraming` 的旧 `bool` 谓词重载**已被删除**（用户在 C3 处明确批准
  破坏性改造）：`FramingAttemptFn` 现在返回三态 `FramingVerificationResult`，
  `FramingAttemptOutcome::accepted` 由 `verdict` 取代。仓内调用点（`main.cpp`、
  `tests/tcp_framing_negotiation_test.cpp`、`tests/tcp_stream_decoder_test.cpp`）
  已同步改造，无仓外调用者。

## 11. 验收标准

1. 上述单元与跨进程用例全部通过，且三平台 CI 绿。
2. auto 在 V1.2 上选中 `{V14,8}`、在 V1.5 16/32 从站上选中对应候选，日志可审计。
3. 无人监听时退出码非 0 且文本区分"未决"与"被拒绝"。
4. `code-plan` 增补实施记录，`tools/xcp_master/README.md` 更新 auto 语义与副作用。