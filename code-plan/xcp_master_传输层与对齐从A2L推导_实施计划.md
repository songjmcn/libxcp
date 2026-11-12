# xcp_master 传输层与 PACKET_ALIGNMENT 从 A2L 自动推导 实施计划

> 状态：**方案待批准**（未修改任何代码）
> 修订：R2 —— 按 m01774 定案改写 Q3（嗅探）并新增 §8；按 m01756 新增 §3（save 字段缺陷）。
> 修订：R3 —— 按 m01798 定案改为 **TCP/UDP 双路嗅探、彻底取消默认 UDP**，新增 §9；§2.2 判定表与 Q1/Q6 已被 §9 取代。
> **状态：全部问题已定案（Q1–Q9，m01774 / m01798 / m01813），可进入实施。**
> 触发问题：`apa_complex.a2l` 明确声明 `XCP_ON_TCP_IP`（192.168.2.137:5559），
> 但 xcp_master 未写 `--transport` 时默认按 UDP 连接，ECU 无响应，报
> `当前状态不允许进入恢复: Connecting`。
> 用户要求（m01706）：「建议A+B一次做，然后增加alignment也从A2L中读（如果有那么就不需要猜测了）。先分析，并出方案，不修改代码。」

---

## 1. 问题分析

### 1.1 现场证据

用户 A2L（`apa_complex.a2l:246775`）的传输层声明：

```
/begin XCP_ON_TCP_IP
  0x104 5559 ADDRESS "192.168.2.137"
/end XCP_ON_TCP_IP
```

整份文件**没有** `XCP_ON_UDP_IP` 段，也没有 `PACKET_ALIGNMENT_*` 关键字
（扫描结果见 §2.3）。用户命令行：

```bash
xcp_master.exe --a2l apa_complex.a2l --host 192.168.2.137 --port 5559 --mode daq
```

输出 `传输=udp Profile=不适用 对齐=8bit(Profile 决定) 端点=192.168.2.137:5559(--host/--port)`，
即：**A2L 的声明完全没被用上，端口/地址恰好由命令行给对，但传输层选成了 UDP。**

### 1.2 根因：传输层是「先定后查」

`TransportSelectionRequest::transport` 默认值是 `TransportKind::Udp`
（`include/libxcp/transport_selection.hpp:45`，枚举 `Udp = 0`），而
`tools/xcp_master/main.cpp:4042-4048` 的过滤逻辑是：

```cpp
const auto wanted = options->transport_request.transport == Tcp
                        ? a2l::TransportEndpoint::Kind::TcpIp
                        : a2l::TransportEndpoint::Kind::UdpIp;
```

即**先决定 kind，再从 A2L 里只捞这一类端点**。当用户没写 `--transport` 时，
kind 恒为 UDP，于是：

1. `xcp_info->transports` 里的 `TcpIp` 端点被当作 `other_kind_declared` 丢掉
   （`main.cpp:4096-4099`）；
2. `a2l_facts.declared` 保持 false，`alignment_declared` 保持 false；
3. `ResolveTransportSelection` 走 UDP 分支，报
   `"A2L 未声明 PACKET_ALIGNMENT，按 8 bit 运行"` —— 这个结论**成立但误导**：
   它描述的是「A2L 里没有 UDP 端点」，而不是「A2L 里没有任何端点」。

这个过滤本身**没有错**（§8.1 规则 5 要求端点/对齐/Profile 同属一个 kind，
禁止跨 kind 借用证据），缺的是**在定 kind 之前先问一句 A2L 声明了什么**。

### 1.3 为什么不能简单「A2L 说什么就用什么」

三个硬约束决定了自动推导必须有边界：

| 约束 | 出处 | 含义 |
|---|---|---|
| 显式优先 | §8.1 规则 7 / 工程配置文件优先级 | `--transport` 与 JSON 的 `transport` 字段必须永远压过 A2L |
| 不猜 Profile | `include/libxcp/tcp_wire_profile.hpp:35-42`（§1.2 禁止事项） | A2L 的 TL 版本、`PACKET_ALIGNMENT` **不得**用于推断 AsamV15/V14 |
| 多实例 fail-closed | `transport_selection.hpp:161-166`（计划 §7.3） | 同类端点多于 1 个且 CLI 未覆盖时不得静默取第一个 |

其中第二条是本次方案最关键的红线：自动切到 TCP 之后，Profile 仍然**必须**由
`--tcp-profile`（或 `--preset xcplite` / `auto`）给出，A2L 只能提供
**PACKET_ALIGNMENT**，不能提供 Profile。否则就等于「用 A2L 推断 wire profile」，
直接违反既有契约。

### 1.4 alignment 现状：能不能读、读到了为什么没用

A2L 的 `PACKET_ALIGNMENT` 有**两套**读取路径，且必须双源核对：

1. **SDK 事实**：`a2lbridge` 解析 `XCP_ON_TCP_IP` 块时填充
   `TransportEndpoint::packet_alignment` 与 `packet_alignment_declared`
   （`thirdparty/a2l-sdk/a2lbridge/include/libxcp/a2l/if_data_xcp.hpp:51-64`）。
   上游对未识别字面量静默保持默认 8 bit，所以 `packet_alignment == 8`
   既可能是「写了 `PACKET_ALIGNMENT_8`」也可能是「根本没写」，只能靠
   `packet_alignment_declared` 区分。
2. **受限原文扫描**：`ScanA2lTcpCandidate()`（`include/libxcp/a2l_tcp_alignment.hpp:65`）
   在受限子集内定位唯一 `XCP_ON_TCP_IP` 块并取出 `alignment`。

两者由 `CrossCheckA2lTcpAlignment()`（`src/a2l_tcp_alignment.cpp:437`）核对，
只有 **kTrusted** 才允许作为连接依据（`main.cpp:4114-4120`）。

**当前失效的三个环节**（逐一核实）：

| # | 环节 | 现状 | 后果 |
|---|---|---|---|
| F1 | `main.cpp:4075` 调 `ScanA2lTcpCandidate(a2l_snapshot->bytes)` **未传 `resolver`** | 契约写明「resolver 为空时任何 `/include` 都导致拒绝」（`a2l_tcp_alignment.hpp:57`） | 任何含 `/include` 的 A2L 都判 `kUntrusted` |
| F2 | 快照只在 TCP 分支生成（`main.cpp:3961-3971`） | 一旦按 UDP 启动就没有快照 | 即使切到 TCP 后也需要重新走快照；现有代码顺序是「快照 → 加载 A2L → 推导 kind」，**顺序要改** |
| F3 | 快照文件名是随机 `a2l-snapshot-<hex>.a2l` 写在 `<exe>/tmp`（`src/trusted_a2l_snapshot.cpp:16,42`） | 相对 `/include` 以快照所在目录为基准（`thirdparty/a2llib/src/a2lscanner.cpp:351`） | 含相对 include 的 A2L 在 TCP 下会找错目录（既有缺陷，见 §6） |

也就是说：**alignment 的读取链路是完整的，但它排在「先定 kind」之后，且被
include 与快照路径两个既有问题挡住。**

---

## 2. 方案设计

### 2.1 总体思路：把「定 kind」提前到「用 kind 过滤」之前

新增一个**纯函数推导步骤**，输入是 A2L 的完整端点清单（未过滤），
输出是「建议的 kind + 依据 + 冲突诊断」。它插在 `main.cpp` 的
**A2L 加载之后、快照与事实过滤之前**：

```
ParseOptions ──► 读工程配置 ──► A2L 加载（UDP 语义：原路径）
                                     │
                                     ▼
                        ┌────────────────────────────┐
                        │ DecideTransportFromA2l()   │  ← 新增纯函数
                        │ (全部端点，不做 kind 过滤) │
                        └────────────────────────────┘
                                     │
              ┌──────────────────────┼───────────────────────┐
              ▼                      ▼                       ▼
        CLI/JSON 已显式         A2L 唯一声明             A2L 双/未声明
        → 保持，冲突时告警      → 采用 + note            → 默认 UDP + note
                                     │
                                     ▼
                     按最终 kind 决定是否建受控快照（TCP 才建）
                                     │
                                     ▼
                     ScanA2lTcpCandidate(bytes, resolver)  ← 补上 resolver
                                     │
                                     ▼
                     CrossCheck → a2l_facts.alignment_declared
                                     │
                                     ▼
                     ResolveTransportSelection（既有逻辑不变）
```

**关键点：`ResolveTransportSelection` 一行都不改。** 推导只负责决定
`request.transport` 的**默认值**，一旦确定，后续规则（含规则 5 的跨 kind
拒绝、规则 1 的 Profile 必填、规则 2 的 alignment 来源）全部照旧生效。

### 2.2 传输层推导规则（方案 A + B）

新增纯函数（建议放在 `tools/xcp_master/`，因为它要读 SDK 的完整端点清单，
而 `include/libxcp/transport_selection.hpp` 的 `A2lTransportFacts` 是**已按 kind 过滤**的）：

```cpp
/// @brief A2L 传输层推导结论
struct A2lTransportInference {
    /// @brief 推导出的 kind；无依据时为空（保持调用方默认 UDP）
    std::optional<TransportKind> transport;
    /// @brief 推导依据（中文，用于日志与摘要）
    std::string reason;
    /// @brief 诊断提示（不阻断，按顺序输出）
    std::vector<std::string> notes;
};

/// @brief 依据 A2L 的传输层声明推导默认 kind
/// @param declared_classic 非 XCPplus 来源的端点清单（全部 kind）
/// @param ambiguous A2L 是否存在多 MODULE 歧义
/// @param explicit_kind 调用方（CLI 或工程配置文件）是否显式给了 kind
[[nodiscard]] A2lTransportInference DecideTransportFromA2l(
    const std::vector<TransportEndpoint>& declared_classic,
    bool ambiguous,
    bool explicit_kind);
```

判定表（**显式优先，绝不推翻用户**）：

| # | 条件 | 结果 | 提示 |
|---|---|---|---|
| R1 | `explicit_kind == true` | 保持显式值 | 无 |
| R2 | `ambiguous == true` | 保持默认 UDP | 「A2L 存在多个 MODULE，传输层不做自动推导」 |
| R3 | 仅有 `TcpIp` 端点（≥1，且无其他经典 kind） | **TCP** | 「传输层取自 A2L：仅声明 XCP_ON_TCP_IP」 |
| R4 | 仅有 `UdpIp` 端点 | UDP | 「传输层取自 A2L：仅声明 XCP_ON_UDP_IP」 |
| R5 | 同时含 `TcpIp` 与 `UdpIp` | 默认 UDP | **告警**：「A2L 同时声明了 TCP(h:p) 与 UDP(h:p)，默认使用 UDP；可用 --transport 指定」 |
| R6 | 仅有 CAN/SxI/USB/FLX 或完全没有端点 | 默认 UDP | 「A2L 未声明 IP 传输端点」 |

**R5 为什么默认仍是 UDP 而不是 TCP**：保持既有默认（规则 9），变更面最小；
且两种都声明时「哪个是真的」A2L 本身没有答案，静默选一个才是猜测。
此时给出告警已经满足方案 B 的目标。

**R3 之后：自动开启嗅探（见 §8）**。若 A2L 只有 TCP 而用户没给
Profile，不再直接报错，而是等价于自动加上 `--tcp-profile auto`：
逐个候选新建连接嗅探出实际布局，成功则正常执行，失败才报错。
候选顺序按 A2L 的 TL 版本与对齐排序（`BuildTcpFramingCandidates`），
A2L 只影响**尝试顺序**，不影响「必须是显式或嗅探得到的确定值」这一约束。

### 2.3 方案 B：冲突诊断（无论是否自动推导都输出）

在 `FormatSelectionSummary` 之后追加一行诊断，来源是本次推导的 `notes`。
对用户的现场场景，输出会变成：

```
传输=tcp Profile=AsamV14(Cli) 对齐=8bit(Profile 决定) 端点=192.168.2.137:5559(--host/--port)
  · 传输层取自 A2L：仅声明 XCP_ON_TCP_IP
```

即使用户显式写了 `--transport udp`（R1 保持显式值），也会输出：

```
  · 注意：A2L 仅声明了 TCP 端点 192.168.2.137:5559，当前按 UDP 连接，可能无法通信
```

这条告警是方案 B 的核心价值：**让「A2L 说 TCP、你在用 UDP」这件事在启动 2 秒内
就可见，而不是等 CONNECT 超时后由状态机报一个无关错误。**

### 2.4 alignment 从 A2L 读取：修 F1 补 resolver

`ScanA2lTcpCandidate` 增加一个基于 `a2l_snapshot` 所在目录的 resolver，
把 `/include "X.a2l"` 解析为绝对路径后读取内容（相对路径按快照目录、绝对路径直接读）。
`IncludeResolver` 类型已存在（`a2l_tcp_alignment.hpp:50-51`），**不需要改库头文件**。

需要注意的风险与对策：

| 风险 | 对策 |
|---|---|
| include 文件很大（用户 A2L 91 MB，AML 缺失） | resolver 只读取 `.a2l` 后缀目标；单文件大小上限 1 MiB，超限返回空 → 判 `kUntrusted`（fail closed） |
| 递归 include | resolver 内部维护深度上限（如 8）与已访问集合，循环即返回空 |
| 读取失败 | 返回空 → 现有代码判 `kUntrusted` 并给出既有中文提示，无需新增错误路径 |

**修完 F1 之后的行为变化**：含 include 的 A2L 在 TCP 下若能通过双源核对，
就会采用 A2L 的 `PACKET_ALIGNMENT`，用户不必再手写 `--packet-alignment`。
这正是用户「如果有那么就不需要猜测了」的诉求。

### 2.5 快照顺序调整（F2）

现在 `main.cpp:3961` 的建快照依赖 `options->transport_request.transport`。
推导需要在 A2L **加载之后**进行，因此执行顺序改为：

1. A2L 从**原路径**加载（UDP 语义，与今天一致）；
2. `DecideTransportFromA2l()` 决定 kind；
3. 若最终 kind 为 TCP：
   a. 重新生成受控快照（从原路径）；
   b. `ScanA2lTcpCandidate(snapshot->bytes, resolver)`；
   c. **可选**：从快照路径**重新加载**一次 A2L，保证「核对的和解析的是同一份字节」。

其中 3c 是新增判断点，需要你拍板（见 §5 Q2）。不做的代价是：快照与加载路径
可能不是同一份字节（理论上 TOCTOU）。做的代价是 TCP 下 A2L 会被解析两次
（用户这份 91 MB 文件第一次是 10.3 秒，翻倍影响启动体验）。

---

## 3. 补充：工程配置文件 save 的字段缺陷（用户 m01756 新发现）

### 3.1 现场结论：`transport` 确实存了，但值可能是「没被采用的那个」

`tools/xcp_master/project_file.hpp:896-923` 的 `ConnectionFromRequest()` 从
`options_.transport_request` 取值。而 `Save()`（`tools/xcp_master/main.cpp:2670-2671`）
调用的正是它：

```cpp
calmcar::xcp::tools::ConnectionFromRequest(options_.transport_request,
                                           file.connection);
```

**问题所在**：`main.cpp:4178-4182` 只把 `selection->host/port` 回写进了
`options->host/port`，**没有回写 `transport_request.transport`**。
因此当传输层最终由 A2L 推导（§2 新能力）或由 preset 展开得到时，
`transport_request.transport` 仍是**用户没写时的默认值 UDP**，
JSON 里就会写出 `"transport": "udp"` —— 与实际连接方式不符。

也就是说：**今天之所以「看起来没存 UDP/TCP」，是因为存了但存的是错的值；
再加上 A2L 推导落地后会把这个错值固化进配置文件。**

### 3.2 字段完整性盘点

| 字段 | 现状 | 结论 |
|---|---|---|
| `connection.transport` | 由 `TransportKindName(request.transport)` 写出 | **代码里有，但值可能错**（见 3.1）；修复 = 回写生效值 |
| `connection.tcp_profile` | 有 | ✅ |
| `connection.packet_alignment_bits` | 取 `request.packet_alignment_bits`（**用户输入值**） | ⚠️ 当 alignment 来自 A2L 或 Profile 默认时，JSON 里是 `null`；与「保存生效配置」的语义不符，建议存生效值 + 来源 |
| `connection.protocol_version` | 取 `request.protocol_version`（用户输入） | ⚠️ 同上；A2L 静态声明（`a2l_facts.protocol_version`）与运行时协商结果都没落盘 |
| `connection.transport_version` | 取 `request.transport_version` | ⚠️ 同上 |
| `session.mode` / `interval_ms` / `module` | 有 | ✅ |
| `connection.endpoint_from_a2l` | **恒为 false（从未被赋值）** | ❌ 死字段 |
| 字节序 / 地址粒度 | **不存在于任何结构** | ❌ 见 3.3 |

### 3.3 关于「协议版本」需要先对齐概念

你提到的「协议版本」在本库里有**三个不同层级**，不能混为一谈：

| 概念 | 结构/字段 | 来源 | 现在有没有落盘 |
|---|---|---|---|
| 协议层版本 PL | `XcpProtocolVersion`（如 1.2 / 1.3 / 1.5） | `--protocol-version` 或 A2L `PROTOCOL_LAYER` 或 CONNECT 协商 | 仅 CLI 输入值 |
| 传输层版本 TL | `XcpProtocolVersion`（如 1.4 / 1.5） | `--transport-version` 或 A2L `XCP_ON_*` 首字段 | 仅 CLI 输入值 |
| Wire Profile | `TcpWireProfile`（AsamV15 / AsamV14） | `--tcp-profile` / preset / auto 试连 | ✅ |

配置文件样例里 `connection.protocol_version` / `transport_version` 属于前两者，
`connection.tcp_profile` 属于第三。**save 时应保存「生效值」而不是「用户输入值」**，
否则用 `-c` 回放时会退化成「靠默认值重来一遍」。

### 3.4 建议的修复方向：save 改为「存生效值」

把 `Save()` 的输入从 `options_.transport_request`（用户输入）改为
**`ResolvedTransportSelection`（解析结果）**，并把它保存进 `CliApp`：

- 新增 `CliApp` 构造参数或 `SetResolvedSelection(const ResolvedTransportSelection&)`；
- 用新增的 `ConnectionFromResolved(const ResolvedTransportSelection&, ProjectConnection&)`
  序列化，字段映射：

| JSON 字段 | 新来源 | 说明 |
|---|---|---|
| `connection.transport` | `resolved.transport` | 修复 3.1 |
| `connection.host` / `port` | `resolved.host` / `resolved.port` | 与今天返回一致 |
| `connection.endpoint_from_a2l` | `resolved.endpoint_source == EndpointSource::A2l` | 激活死字段 |
| `connection.tcp_profile` | `resolved.wire_profile` + `wire_profile_source` | `auto` 试连成功时写 `"asam-v1.5"`/`"asam-v1.4"` 的**实际结果** |
| `connection.packet_alignment_bits` | `resolved.alignment` | 写生效位宽（含 A2L/Profile 默认来源） |
| `connection.protocol_version` | `resolved.protocol_version`（若空则 `a2l_facts.protocol_version`） | |
| `connection.transport_version` | `resolved.transport_version`（若空则 `a2l_facts.transport_version`） | |

**向后兼容**：`schema_version` 仍为 `1`，不新增必填字段、不改已有字段语义；
只改变「写什么值」。旧的 `-c` 加载路径（`MergeProjectFileIntoOptions`）不需要改动，
因为解析入口本来就接受这些字段。

**新增可选来源字段（建议，需拍板 §5 Q5）**：为可审计性增加只读元信息
`session.source: {"transport": "cli|config|a2l", "alignment": "cli|a2l|profile"}`。
`-c` 加载时**忽略**这些字段（只写不读），不参与任何决策。

---

## 4. 实施步骤

| 步骤 | 内容 | 涉及文件 | 依赖 |
|---|---|---|---|
| T1 | 新增 `A2lTransportInference` 与 `DecideTransportFromA2l()`（纯函数，输入全部端点，不做加载） | `tools/xcp_master/transport_inference.hpp`（新） | — |
| T2 | 重排 `main.cpp` 启动链路：加载 A2L → 推导 kind → 条件建快照 → 过滤事实 → 解析 | `tools/xcp_master/main.cpp` | T1 |
| T3 | 为 `ScanA2lTcpCandidate` 补 `IncludeResolver`（相对快照目录、绝对直读、1 MiB 上限、深度 8、循环检测） | `tools/xcp_master/main.cpp` + `tools/xcp_master/include_resolver.hpp`（新） | T2 |
| T4 | 输出推导诊断（`notes` + 冲突告警），命令行/配置文件显式值时也出告警 | `tools/xcp_master/main.cpp` | T1 |
| T5 | 新增 `ConnectionFromResolved()`，`Save()` 改存生效值，激活 `endpoint_from_a2l` | `tools/xcp_master/project_file.hpp`、`tools/xcp_master/main.cpp` | — |
| T6 | `CliApp` 持有 `ResolvedTransportSelection`，`main()` 在 `Run()` 前注入 | `tools/xcp_master/main.cpp` | T5 |
| T7 | 单元测试：推导规则 R1–R6、resolver 边界、`ConnectionFromResolved` 字段映射 | `tests/xcp_master_transport_inference_test.cpp`（新）、`tests/xcp_master_project_file_test.cpp`（追加） | T1/T3/T5 |
| T8 | CLI 冒烟：A2L 仅声明 TCP 时自动走 TCP 的完整输出断言 | `tests/xcp_master_transport_inference_test.cmake`（新） | T2/T4 |
| T9 | 回归 `ctest -R "XcpMaster|A2l|Transport"` + 全量 | — | 全部 |
| T10 | README 与 `code-plan` 实施记录 | `tools/xcp_master/README.md`、`code-plan/` | 全部 |

---

## 5. 待确认问题

| # | 问题 | 我的建议 |
|---|---|---|
| Q1 | ~~A2L 同时声明 TCP+UDP 时默认用谁？~~ | **已定案（m01798）：不设默认，TCP 与 UDP 都嗅探，以嗅探结果为准。见 §9。** |
| Q2 | 推导出 TCP 后，是否从**快照**再加载一次 A2L（3c）以保证「核对的字节=解析的字节」？代价是 TCP 下 A2L 解析两次（本例 91 MB 约 +10 s） | **暂不做**，先记录为已知限制；如果你更看重严格性我再加，并在启动横幅上明示耗时。 |
| Q3 | ~~A2L 推导出的 TCP 但用户没给 `--tcp-profile` 时如何处理？~~ | **已定案（m01774）：自动开启嗅探；嗅探成功则正常执行，失败则报错。详见 §8。** |
| Q4 | ~~`tcp_profile` 存实际结果还是存 `auto`？~~ | **已定案（m01774）：一律存运行时实际参数。详见 §8.4/§8.5。** |
| Q5 | 是否新增只读来源元信息 `session.source`？ | **建议加**，成本低，且能回答「这次为什么是 tcp」这类追问。若你希望 schema 最小化也可以不加。 |
| Q6 | ~~`--transport udp` 与 A2L 仅声明 TCP 时如何处理？~~ | **已定案（m01798）：改走双路嗅探，不默认任何一方。见 §9。** 仅当用户**显式**写了 `--transport` 时仍以显式值为准并输出告警。 |
| Q7 | 快照相对 include 缺陷（§6）是否本次一并修？ | **本次不修**，另立任务。它与传输层推导正交，且修复会牵动快照目录语义。 |

---

## 6. 已知限制（本次不解决，需明确记录）

1. **快照目录破坏相对 `/include`**：`SnapshotFileName()` 写
   `a2l-snapshot-<hex>.a2l` 到 `<exe>/tmp`（`src/trusted_a2l_snapshot.cpp:16,42`），
   而 a2llib 以「当前文件所在目录」为基准解析相对 include
   （`thirdparty/a2llib/src/a2lscanner.cpp:351`）。TCP 下含相对 include 的 A2L
   会找错目录。**本次 T3 的 resolver 只服务于 alignment 扫描，不改变 a2llib
   的加载行为**，所以这个缺陷依旧存在。
2. **Profile 仍必须显式**：A2L 不参与 Profile 选择（`tcp_wire_profile.hpp:35-42`）。
   即使 A2L 声明 `PACKET_ALIGNMENT`，`asam-v1.5` 与 `asam-v1.4` 仍需用户选。
3. **多实例**：同一 kind 多个端点时仍沿用既有「取第一个 + 提示」行为
   （`transport_selection.cpp:390-393`）。
4. **`/include` 内容不参与结构判定**：resolver 只拼接内容用于受限扫描，
   若 include 进来的 `.a2l` 自身含非受限语法，仍会判 `kUntrusted`。

---

## 7. 验收标准

1. 用 `apa_complex.a2l` + `--host 192.168.2.137 --port 5559 --mode daq`
   （**不写 `--transport`、不写 `--packet-alignment`）时，摘要行为
   `传输=tcp`，且提示「传输层取自 A2L：仅声明 XCP_ON_TCP_IP」。
2. 在上述条件下若未给 `--tcp-profile`，错误文本必须是既有的
   `TCP 传输必须显式指定 --tcp-profile …`，而非 UDP 的
   `--a2l, --host 与 --port 为必填项`。
3. 显式 `--transport udp` 时行为不变，但输出
   「注意：A2L 仅声明了 TCP 端点 …；当前按 UDP 连接，可能无法通信」。
4. A2L 同时声明 TCP 与 UDP 时**不得**默认任何一方：必须两路各嗅探一次，逐项输出每路的结论，以首个合法响应者为准。
5. `save` 后的 JSON 中 `transport` 与实际连接方式一致；
   `endpoint_from_a2l` 在端点来自 A2L 时为 `true`；
   `packet_alignment_bits` 为生效值。
6. A2L 仅声明 TCP 且未给 `--tcp-profile` 时，工具自动开启嗅探：
   逐项输出 `试连 <候选>：<结论>`，成功则正常进入 TUI，失败则以退出码 3 或 4 退出。
7. 嗅探成功后 `save` 的 JSON 中 `tcp_profile` 是 `asam-v1.5`/`asam-v1.4`
   （**不是** `auto`），`packet_alignment_bits` 是嗅探得到的实际位宽。
8. `connection.protocol_version` / `transport_version` 与运行期
   `GET_VERSION`/CONNECT 协商出的有效值一致。
9. 全部既有测试不回归（当前基线 1221/1221）。
---

## 8. Q3 修订：推导出 TCP 但未给 `--tcp-profile` 时开启嗅探（用户 m01774 定案）

> 本节**取代** §2.2 末尾「R3 之后仍需 `--tcp-profile`」与 §5 的 Q3 原建议。

### 8.1 用户定案

> 「如果推导出TCP但是没有给--tcp-profile，那么开启嗅探，尝试嗅探出版本，如果能正常嗅探出版本，那么正常执行，如果嗅探不出版本，那么报错，保存configure的时候保存嗅探的版本。配置应该存储最后运行时实际的参数，而非传入的参数。」

拆解为四条可执行的约束：

| # | 约束 | 落点 |
|---|---|---|
| D1 | A2L 推导出 TCP + 无 `--tcp-profile` → 自动进入嗅探 | §8.2 |
| D2 | 嗅探成功 → 用嗅探结果正常执行，不再报错 | §8.2 |
| D3 | 嗅探失败 → **报错退出**，不退回 UDP、不静默降级 | §8.3 |
| D4 | `save` 保存**嗅探出的实际版本**，而不是「auto」 | §8.4 |
| D5 | 配置一律存**运行时实际参数**，而非传入参数 | §8.5（全局原则） |

### 8.2 嗅探机制：复用既有 `--tcp-profile auto`，只是把「开关」改为「推导默认」

关键结论：**库已经具备完整的嗅探能力，本次不需要新造机制。**

- `NegotiateTcpFraming()`（`include/libxcp/tcp_framing_negotiation.hpp:131`）按固定顺序
  为每个候选**新建连接**做协议层验证，首个 `Accepted` 胜出；
- `VerifyTcpFramingCandidate()`（`TcpFramingProbeConfig`）负责单候选取证；
- `BuildTcpFramingCandidates(evidence)`（`include/libxcp/tcp_framing_candidates.hpp:58`）
  按 A2L 的 TL 版本与对齐**排序**候选（只是优先，不排除）；
- `main.cpp:4219-4377` 已有完整的 auto 分支：失败时按
  `TcpNegotiationExitCode()` 区分「3=对端明确拒绝全部布局」「4=没有任何协议层回应」；
- 胜出后严格 auto 会**另建一条**正式连接（绝不复用试连连接）。

因此 D1 的实现只是**改变 `request` 的内容**：

```cpp
// 在 main.cpp 中，DecideTransportFromA2l() 返回 TCP 且
// !transport_explicit_kind && !tcp_profile 相关选项全部未给出时：
options->transport_request.tcp_profile_auto = true;   // 等价于用户写了 --tcp-profile auto
```

之后现有代码**一行不改**：规则 8 接受 auto、规则 1 放行、候选排序、试连、
三态裁决、失败退出码全部复用。

### 8.3 D3：嗅探失败必须硬化

现有 auto 失败路径（`main.cpp:4352-4373`）已经会 `return exit_code`（3 或 4），
满足「报错退出」。但需要补一条**用户可见的原因说明**，因为对本次场景而言，
用户并没有主动选择 auto，看到 `全部候选都被对端拒绝` 会困惑。建议在失败分支
追加一行：

```
嗅探由「A2L 推导出 TCP 但未指定 --tcp-profile」自动开启；若已知道对端实现，
可直接用 --tcp-profile asam-v1.5|asam-v1.4 指定，避免多次试连。
```

同时保留既有退出码语义（3/4），不改。

### 8.4 D4：把嗅探结果写回 selection，供 save 使用

`main.cpp:4404-4406` 目前**只回写了对齐**：

```cpp
options->transport_request.packet_alignment_bits =
    static_cast<std::uint8_t>(outcome.selected->alignment);
```

Profile 没有回写，注释理由是「selection 是 Result 视图，不就地改写它」。
这个顾虑是正确的，但 D4 要求 save 拿到**实际值**，因此建议改为：

- 新增一个**独立的「生效配置」快照**（不修改 `selection` 本身）：

```cpp
/// @brief 运行时实际生效的连接参数（与 ResolvedTransportSelection 分离）
struct EffectiveConnection {
    TransportKind transport = TransportKind::Udp;
    TcpWireProfile profile = TcpWireProfile::Unknown;  ///< 嗅探/显式得到的实际布局
    WireProfileSource profile_source = WireProfileSource::NotApplicable;
    PacketAlignment alignment = PacketAlignment::Unknown;
    AlignmentSource alignment_source = AlignmentSource::ProfileDefault;
    std::string host;
    std::uint16_t port = 0;
    EndpointSource endpoint_source = EndpointSource::Cli;
    XcpProtocolVersion protocol_version;      ///< 运行时实际
    XcpProtocolVersion transport_version;     ///< 运行时实际
    bool profile_sniffed = false;             ///< 是否由嗅探确定（用于日志与 JSON 备注）
    std::string transport_source;             ///< "cli" / "config" / "a2l"
};
```

- 在 `master->Connect()` 成功之后，用 `master->GetProtocolVersionSnapshot()` /
  `GetTransportVersionSnapshot()` 的 `effective` 值填 `protocol_version` /
  `transport_version`（**这就是「运行时实际」而不是「传入」**）；
- 在 auto 成功分支填 `profile` / `alignment` / `profile_sniffed = true`；
- 该对象传给 `CliApp`（T5/T6），`Save()` 用它序列化。

### 8.5 D5：全局原则「存运行时实际参数」的落点清单

| JSON 字段 | 现在（传入参数） | 改为（运行时实际） |
|---|---|---|
| `connection.transport` | `request.transport`（**可能是未被采用的 UDP 默认**） | `effective.transport`（A2L 推导 / 嗅探后确定的那个） |
| `connection.tcp_profile` | `request.tcp_profile`（auto 时是 `"auto"`） | `effective.profile`（嗅探得到的 `asam-v1.5`/`asam-v1.4`） |
| `connection.packet_alignment_bits` | `request.packet_alignment_bits` | `effective.alignment`（含 A2L / Profile 默认 / 嗅探结果） |
| `connection.protocol_version` | `request.protocol_version` | `effective.protocol_version`（CONNECT/GET_VERSION 协商结果） |
| `connection.transport_version` | `request.transport_version` | `effective.transport_version` |
| `connection.endpoint_from_a2l` | 恒 false（死字段） | `effective.endpoint_source == A2l` |
| `connection.host` / `port` | 已是生效值（`main.cpp:4182-4183` 回写） | 同一来源 |

**这条原则带来的一个副作用必须说清楚**：一旦 `save` 存的是运行时实际值，
`-c` 回放就不再是「重放用户的意图」，而是「重放上次实际用的配置」。
好处是可复现（同一次实验能原样重跑）；代价是**若上次嗅探错了，错误会被固化**。
因此建议在 JSON 里额外写一个只读字段：

```json
"session": {
  "mode": "daq",
  "interval_ms": null,
  "module": "demo",
  "config_source": "runtime"          // ← 新增只读；-c 加载时忽略
}
```

用于让人一眼看出「这份配置是运行时快照，不是手写意图」。

### 8.6 对 §4 实施步骤的增补

| 步骤 | 增补内容 |
|---|---|
| T1 | 推导结果增加 `bool needs_profile_sniffing`（TCP 且无 profile 选项时置位） |
| T2 | 推导后按需写 `request.tcp_profile_auto = true`；记录 `transport_source` |
| T4 | 嗅探失败分支追加原因说明（§8.3） |
| T5 | 改为从 `EffectiveConnection` 序列化；新增 `ConnectionFromEffective()` |
| T6 | `EffectiveConnection` 在 `Connect()` 成功后填充并注入 `CliApp` |
| T7 | 新增用例：嗅探成功 → JSON 存实际 Profile；嗅探失败 → 退出码 3/4 且无 JSON 写出 |
| T8 | CLI 冒烟：A2L 仅 TCP + 无 `--tcp-profile` 时的完整输出（含试连逐项结果） |
| T11（新） | 用 `apa_complex.a2l` + 真实 ECU 做一次端到端手测，核对 JSON 内容 |

---

## 9. 修订 R3：TCP 与 UDP 双路嗅探（用户 m01798 定案）

> 本节**取代** §2.2 的判定表、§5 的 Q1/Q6 原建议，以及 §8 中「嗅探只用于找出
> TCP Profile」的隐含前提。§8 的 D1–D5 原则全部保留，适用范围扩展到「传输层本身」。

### 9.1 用户定案

> 「Q1需要分别做TCP和UDP嗅探，不要默认使用UDP」
> 「Q6 需要分别做TCP嗅探和UDP嗅探，不要默认，最终结果以嗅探出的结果为准」

含义：**任何未显式指定传输层的情况，都不再有「默认 UDP」这一档**。
工具必须对 TCP 与 UDP **都发一次 CONNECT**，谁得到合法响应就用谁。
只有显式 `--transport`（命令行或工程配置）才跳过嗅探。

### 9.2 关键限制：TCP 嗅探需要讨论候选，UDP 不需要

两者成本差异很大，方案必须区别对待：

| 维度 | UDP 嗅探 | TCP 嗅探 |
|---|---|---|
| 需要试几次 | **1 次**：UDP 没有帧布局概念，直接发 CONNECT 看有无 CRM | **最多 4 次**：`DefaultFramingCandidates()` 固定 4 个（V14/8、V15/8、V15/16、V15/32） |
| 依据 | 无参数 | 必须有 `--tcp-profile` 或嗅探得到 |
| 失败判定 | 超时 = 未决 | 超时 = 未决；解析错误 = 被拒 |

因此双路嗅探的真实开销是 **1 + 最多 4 次建连**。对用户这份 A2L
（TL 版本 `0x104` → 候选把 `{AsamV14, 8bit}` 排首位）正常情况下 **TCP 1 次、
UDP 1 次**即可收敛。

### 9.3 顺序：A2L 声明优先排序，但两路都要试

新增纯函数 `BuildDualSniffPlan()`，输出**有序**的尝试计划：

| # | A2L 情况 | 尝试顺序（第一个成功即停） |
|---|---|---|
| P1 | 仅声明 TCP | TCP 1 次 → UDP 1 次 |
| P2 | 仅声明 UDP | UDP 1 次 → TCP 1 次 |
| P3 | 同时声明 TCP 与 UDP | TCP 1 次 → UDP 1 次（**按 A2L 中 TCP 块出现的先后**决定；无法判定时 TCP 优先） |
| P4 | 都未声明 | TCP 1 次 → UDP 1 次（同上） |
| P5 | 多 MODULE 歧义 | 同 P4，并输出「A2L 多 MODULE 歧义，传输层按嗅探结果决定」 |

**为什么 A2L 声明只影响顺序、不影响「跳过」**：用户明确要求两路都试，
所以即使 A2L 只写了 TCP，UDP 也要试一次——这样能发现「A2L 声明过期、实际是
UDP」这类现场问题，并在日志里留下证据。

**为什么第一个成功即停、不做「两个都成功」的仲裁**：两个都成功意味着
同一地址端口同时有 TCP 与 UDP 服务在响应，这在真实 ECU 上几乎不可能；
若真出现，按顺序取第一个，并在 `notes` 里明确写出「TCP 与 UDP 均响应，
按 A2L 顺序采用 TCP」。不引入更复杂的仲裁规则。

### 9.4 端点来源：每次嗅探独立解析

UDP 与 TCP 的端口/地址可能不同（A2L 里就是两段声明）。规则：

1. 命令行/工程配置给了 `--host/--port` → 两路都用它（用户显式覆盖）
2. 未给 → **每一路各自从 A2L 取该 kind 的端点**
   （TCP 取 `XCP_ON_TCP_IP`，UDP 取 `XCP_ON_UDP_IP`）
3. 某一路 A2L 没有端点且命令行没给 → 该路**直接跳过**（无从连接，不算失败）
4. 所有路都无法构造端点 → 报错，列出两路各自缺什么

### 9.5 与既有契约的冲突：UDP 的 host/port 必填（规则 9）

`src/transport_selection.cpp:265` 的注释与代码明确写着：

```cpp
// UDP 保持既有契约：host/port 必填，不做 A2L 自动选择（规则 9）
return fail("--a2l, --host 与 --port 为必填项");
```

即**今天 A2L 的 `XCP_ON_UDP_IP` 端点不会被 UDP 采用**。双路嗅探要求
「UDP 也能从 A2L 取端点」，因此必须改这条规则：

- 把 UDP 分支改为与 TCP 对称：`host/port` 未给时回落到 A2L 的 UDP 端点；
- 两者皆无时报错文本改为「UDP 传输需要 --host/--port，或 A2L 中声明
  XCP_ON_UDP_IP 端点」。

**这是一个行为变更**，会影响所有「只写 `--a2l` 不写 `--host/--port`」的现有用法
（此前必然报错，此后会尝试连 A2L 里声明的 UDP 端点）。需要你确认（见 §9.10 Q8）。

### 9.6 嗅探器的落地形态

新增一个独立模块，而不是继续往 `main.cpp` 里加分支：

```cpp
/// @brief 一次双路嗅探的尝试记录
struct SniffAttempt {
    TransportKind transport;          ///< 本次尝试的传输层
    std::string host;                 ///< 本次使用的地址
    std::uint16_t port;               ///< 本次使用的端口
    std::optional<TcpFramingCandidate> candidate;  ///< TCP 时的候选；UDP 为空
    FramingVerdict verdict;           ///< 三态结论
    std::string reason;               ///< 失败原因
    long long elapsed_ms;
};

/// @brief 双路嗅探结果
struct TransportSniffResult {
    std::optional<TransportKind> transport;  ///< 胜出传输层
    std::optional<TcpFramingCandidate> candidate;  ///< TCP 胜出时的布局
    std::vector<SniffAttempt> attempts;      ///< 按时间顺序的全部记录
    bool tcp_responded;                      ///< TCP 是否有过 Accepted
    bool udp_responded;                      ///< UDP 是否有过 Accepted
    std::string summary;                     ///< 中文摘要
};

/// @brief 对 TCP 与 UDP 依次嗅探，首个 Accepted 胜出
[[nodiscard]] TransportSniffResult SniffTransport(
    const std::vector<SniffPlanEntry>& plan,
    const SniffAttemptFn& attempt);
```

`SniffAttemptFn` 与既有的 `FramingAttemptFn` 同构（返回
`FramingVerificationResult`），因此：

- TCP 路直接复用 `VerifyTcpFramingCandidate(probe, candidate)`；
- UDP 路复用同一套判定谓词，只是 transport 换成 `UdpTransport` 且
  `candidate` 为空、对齐取 8 bit。

**复用而非重写**：`NegotiateTcpFraming()` 仍然负责「单路 TCP 内部的候选顺序」，
`SniffTransport()` 只负责「跨传输层两路的顺序」。两层职责不重叠。

### 9.7 成功后如何进入正常流程

嗅探胜出后，把结果**回填成一个显式的、确定的请求**再交给
`ResolveTransportSelection()` 走一遍：

```cpp
// 嗅探胜出（例如 TCP + AsamV14 + 8bit）
options->transport_request.transport = TransportKind::Tcp;
options->transport_request.tcp_profile = TcpWireProfile::AsamV14;
options->transport_request.packet_alignment_bits = 8;
options->transport_request.host = sniff_host;
options->transport_request.port = sniff_port;
// 然后照常调用 ResolveTransportSelection()，由既有规则做最终校验
```

**好处**：`ResolveTransportSelection` 的全部规则（含规则 2 的 alignment 校验、
规则 3 的 V1.4 拒绝 16/32、规则 5 的跨 kind 检查）**一行不改**，
嗅探结果必须「说得通」才能进入正式连接，天然拒绝自相矛盾的组合。

### 9.8 失败语义

| 情况 | 行为 |
|---|---|
| 某一路明确被拒（解析错误） | 记录，继续下一路 |
| 某一路未决（纯超时） | 记录，继续下一路 |
| 全部路都失败 | **报错退出**，退出码沿用 `TcpNegotiationExitCode` 的语义（3/4），并逐行列出每一路的结论 |
| 全部路都无法构造端点 | 报错：「A2L 未声明任何 IP 传输端点，且未给出 --host/--port」 |

失败输出示例：

```
传输层嗅探失败：TCP 与 UDP 均未得到合法响应
  · TCP 192.168.2.137:5559 AsamV14/8bit：未决（超时 1000 ms）
  · TCP 192.168.2.137:5559 AsamV15/8bit：未决（超时 1000 ms）
  · UDP 192.168.2.137:5559：未决（超时 1000 ms）
请依次核查：地址与端口、对端是否运行、对端是否已启用 XCP。
```

### 9.9 与 §8（单路 TCP Profile 嗅探）的关系

两节现在合并为**一个统一的嗅探器**：

- 显式 `--transport tcp` + 无 `--tcp-profile` → 只嗅探 TCP 的 4 个候选（§8 原内容）；
- 未显式指定传输层 → **双路嗅探**（本节），TCP 路内部仍用 §8 的候选机制；
- 显式 `--transport tcp` + 有 `--tcp-profile` → 不嗅探，直接解析（今天的行为）；
- 显式 `--transport udp` → **不嗅探**（用户已明确指定；此时若 A2L 仅声明 TCP，
  输出 §2.3 的告警即可）。

### 9.10 待确认

| # | 问题 | 我的建议 |
|---|---|---|
| Q8 | §9.5：是否允许 UDP 从 A2L 的 `XCP_ON_UDP_IP` 取端点（改变规则 9）？ | **已定案（m01813 同意）：只在嗅探模式下放宽；显式 `--transport udp` 仍要求 `--host/--port`。** |
| Q9 | 显式 `--transport udp` 时是否也要嗅探 UDP 能否连通？ | **已定案（m01813 同意）：不嗅探**。显式指定就是显式指定，嗅探只在「未指定传输层」时发生。 |

---

## 10. 最终实施步骤（合并 §4 / §8.6 / §9 后的权威清单）

> 本节是**唯一的实施顺序**，§4 与 §8.6 的表格已被它取代。

| 步骤 | 内容 | 涉及文件 | 依赖 |
|---|---|---|---|
| **T1** | 新增双路嗅探纯函数与数据结构：`SniffPlanEntry`、`BuildDualSniffPlan(A2lEndpointSummary, explicit_kind)`、`SniffAttempt`、`TransportSniffResult`、`SniffTransport(plan, attempt)`。TCP 路内部复用 `NegotiateTcpFraming()` | `tools/xcp_master/transport_sniff.hpp`（新） | — |
| **T2** | 新增 `EffectiveConnection` 结构与 `ConnectionFromEffective()`，覆盖 transport / profile / alignment / PL / TL / host / port / endpoint_from_a2l / profile_sniffed / transport_source | `tools/xcp_master/project_file.hpp` | — |
| **T3** | `Save()` 改为序列化 `EffectiveConnection`；新增只读字段 `session.config_source = "runtime"`（`-c` 加载时忽略） | `tools/xcp_master/main.cpp`、`tools/xcp_master/project_file.hpp` | T2 |
| **T4** | 规则 9 放宽：仅当 `transport_request` 未显式指定 kind 时，UDP 允许回落到 A2L 的 `XCP_ON_UDP_IP` 端点；显式指定时保持原报错 | `src/transport_selection.cpp` | — |
| **T5** | `main.cpp` 启动链路重排：加载 A2L → 判断是否需要嗅探 → 双路嗅探 → 回填显式请求 → `ResolveTransportSelection()` → 构造传输层 | `tools/xcp_master/main.cpp` | T1/T4 |
| **T6** | 为 `ScanA2lTcpCandidate` 补 `IncludeResolver`（相对快照目录、绝对直读、1 MiB 上限、深度 8、循环检测） | `tools/xcp_master/include_resolver.hpp`（新）、`tools/xcp_master/main.cpp` | T5 |
| **T7** | `CliApp` 持有 `EffectiveConnection`；`main()` 在 `Connect()` 成功后填充并注入，再 `Run()` | `tools/xcp_master/main.cpp` | T2/T5 |
| **T8** | 诊断输出：嗅探逐项结论、失败时两路汇总、显式指定与 A2L 冲突的告警（方案 B） | `tools/xcp_master/main.cpp` | T5 |
| **T9** | 单元测试：`BuildDualSniffPlan` 的 P1–P5、`SniffTransport` 的首胜即停与三态、resolver 边界、`ConnectionFromEffective` 字段映射、规则 9 放宽的前后对比 | `tests/xcp_master_transport_sniff_test.cpp`（新）、`tests/xcp_master_project_file_test.cpp`、`tests/transport_selection_test.cpp`（追加） | T1/T2/T4/T6 |
| **T10** | CLI 冒烟：A2L 仅 TCP 且无 `--tcp-profile` 时的完整输出（含 `试连 ...` 行与嗅探摘要） | `tests/xcp_master_transport_sniff_test.cmake`（新） | T5/T8 |
| **T11** | 回归：`ctest -R "XcpMaster|A2l|Transport"`，再全量 `ctest` | — | 全部 |
| **T12** | 文档：`tools/xcp_master/README.md` + `code-plan/xcp_master_传输层与对齐从A2L推导_实施记录.md` | — | 全部 |

### 10.1 实施顺序说明

- **T1/T2/T4 可并行**：三者互不依赖，且都是「纯函数 + 结构体 + 单条规则」，
  先落这三步能让 T9 的单测尽早跑起来。
- **T5 是唯一的高风险步骤**：它重排了 `main.cpp` 的启动链路，
  改动会与既有的 A2L 快照、`DiscardUnmatchedTcpEndpointEvidence`、
  auto 分支交织。建议 T5 完成后**先跑一次既有全量回归**再继续 T6，
  以便把「重排引入的回归」与「新功能缺陷」分开定位。
- **T6 与 T5 解耦**：即使不做 T6，双路嗅探也已经能工作（只是 alignment
  仍取不到可信值）。因此 T5 先跑通、T6 单独验证，避免一次改动两个变量。

### 10.2 已知会发生的用户可见行为变化

| 场景 | 今天 | 改后 |
|---|---|---|
| `--a2l X`（只给 A2L，A2L 仅声明 TCP） | 默认 UDP，连不上并报 `当前状态不允许进入恢复: Connecting` | 双路嗅探 → TCP 1~4 次 + UDP 1 次 → 成功则正常进入 TUI |
| `--a2l X --host H --port P`（A2L 仅声明 TCP） | 同上（UDP 超时） | 同上 |
| `--a2l X --transport udp --host H --port P` | 直接连 UDP | **不变**（显式指定不嗅探） |
| `--a2l X`（A2L 仅声明 UDP，未给 host/port） | 报 `--a2l, --host 与 --port 为必填项` | 嗅探模式下 UDP 取 A2L 端点并连通 |
| `--transport udp` + A2L 仅声明 TCP | 报错/超时 | **不变**（显式指定 + 告警） |
| `save` 后 JSON 的 `transport` | 可能是未被采用的 UDP 默认值 | 实际使用的传输层 |
| `save` 后 JSON 的 `tcp_profile` | `auto`/用户原值 | 嗅探出的 `asam-v1.5`/`asam-v1.4` |

### 10.3 验收（在 §7 基础上的最终版）

1. `--a2l apa_complex.a2l --mode daq`（**不写 transport / profile / host / port**）
   能正常连接并进入 TUI，摘要行显示 `传输=tcp`。
2. 输出中能看到逐项 `试连 <候选>：<结论>` 以及 `UDP 192.168.2.137:5559：<结论>`。
3. 嗅探全部失败时退出码为 3 或 4，且不写任何配置文件。
4. `save` 出的 JSON：`transport` 为 `tcp`；`tcp_profile` 为 `asam-v1.4`；
   `packet_alignment_bits` 为 8；`protocol_version`/`transport_version`
   与运行期实际协商值一致；`session.config_source` 为 `"runtime"`。
5. 用该 JSON `-c` 回放，直接连上且**不再嗅探**（日志无 `试连` 行）。
6. 显式 `--transport udp` 时行为与今天一致。
7. 既有测试全绿（基线 1221/1221）。
