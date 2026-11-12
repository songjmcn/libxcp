# xcp_master 工程配置文件：保存运行时生效值 + 修复加载往返冲突

- 日期：2026-11-11
- 触发问题：`save` 写出的 `protocol_version` / `transport_version` 恒为 `null`；用 `-c` 加载该文件报 `传输层配置错误: --tcp-profile auto 与 --packet-alignment 冲突：自动模式不接受显式对齐`
- 用户决策（m02580）：Q1 嗅探不到版本时写 `null`；Q2 只做“save 存运行时实际值 + 修往返冲突”，不引入双路传输嗅探

## 1 问题与根因

### 1.1 版本号恒为 null

`Save()` 的连接段取值来源是用户输入，不是运行时结果：

```cpp
// 修改前 main.cpp:2954
calmcar::xcp::tools::ConnectionFromRequest(options_.transport_request, file.connection);
```

`ConnectionFromRequest()` 只读 `TransportSelectionRequest` 里的
`protocol_version` / `transport_version`，也就是**用户是否显式传了**
`--protocol-version` / `--transport-version`。本次通过 `--tcp-profile auto`
启动，参数里本就没有版本号，于是两个字段都写成 `null`。

而运行时的真值其实已经取到了，在 `main.cpp:4706-4707`：

```cpp
const auto pl = master->GetProtocolVersionSnapshot();
const auto tl = master->GetTransportVersionSnapshot();
```

但这段只用于 `FormatRuntimeVersionSummary()` 打印，没有保存，也没有交给
`CliApp`。改动前 `grep -n "EffectiveConnection"` 零命中，确认
`code-plan/xcp_master_传输层与对齐从A2L推导_实施计划.md` §8.4 设计的
`EffectiveConnection` 从未落地。

同类问题：`tcp_profile` 写的是 `"auto"`、`packet_alignment_bits` 写的是输入值，
都不是实际生效的布局。

### 1.2 加载时 auto 与对齐冲突

一次往返把两个“半成品”拼成了自相矛盾的组合：

1. 用户以 `--tcp-profile auto` 启动，实际生效的是嗅探胜出的 `asam-v1.5` + 8 bit；
2. `save` 写盘时 `tcp_profile` 写 `"auto"`（尚未决定的字面量），
   `packet_alignment_bits` 却写了具体值 `8`；
3. `-c` 加载时 `ProjectConnectionToRequest()` 原样还原两个字段，
   `request.tcp_profile_auto = true` 且 `request.packet_alignment_bits = 8`；
4. `ResolveTransportSelection()` 规则 8（`src/transport_selection.cpp:106-109`）
   判定二者互斥 → 整份配置被拒绝。

即：**auto 模式本来就不该落盘对齐值**，写下去必然自相矛盾。

## 2 方案

核心是把 `save` 的取值来源从“用户输入”换成“运行时生效值”。

### 2.1 `EffectiveConnection`（project_file.hpp）

记录“连上 ECU 之后真正在用的那一组值”：

```cpp
struct EffectiveConnection {
    std::string transport{"udp"};
    std::string host;
    std::uint16_t port{0};
    bool endpoint_from_a2l{false};
    std::string tcp_profile;                        // 恒为具体布局
    std::optional<std::uint8_t> packet_alignment_bits;
    std::optional<std::string> protocol_version;    // 无权威证据为空
    std::optional<std::string> transport_version;
    std::optional<bool> preset_xcplite;
    std::optional<std::string> a2l_snapshot_root;
    std::vector<std::string> notes;                 // 仅供人工核对，不写 JSON
};
```

配套转换函数：

| 函数 | 作用 |
| --- | --- |
| `ConnectionFromEffective(const EffectiveConnection&, ProjectConnection&)` | 生效值 → 配置文件连接段 |
| `MakeEffectiveConnection(resolved, pl, tl)` | 解析结果 + 版本证据 → 生效值 |
| `AlignmentBitsOf(PacketAlignment)` / `WireProfileText(TcpWireProfile)` | 枚举 → 落盘文本的边界换算 |

### 2.2 main.cpp 的接线

1. `selection` 由 `const auto` 改为 `auto`，理由写在注释里：嗅探模式要把胜出
   候选的 Profile 与对齐回填进来。
2. 嗅探成功后回填（严格 auto 分支）：

```cpp
selection->wire_profile = outcome.selected->profile;
selection->alignment = outcome.selected->alignment;
```

3. `Connect()` 成功、版本快照取到之后组装生效值，并交给 `CliApp`：

```cpp
const auto authoritative = [](const auto& evidence) {
    return evidence.source != calmcar::xcp::VersionSource::Unknown &&
           evidence.confidence != calmcar::xcp::VersionConfidence::Unknown;
};
const std::optional<calmcar::xcp::XcpProtocolVersion> pl_effective =
    authoritative(pl.evidence) ? std::optional<calmcar::xcp::XcpProtocolVersion>(
                                     pl.effective)
                               : std::nullopt;
// tl 同理
effective_connection =
    calmcar::xcp::tools::MakeEffectiveConnection(*selection, pl_effective, tl_effective);
```

4. `CliApp` 构造函数新增末位缺省参数
   `std::optional<EffectiveConnection> effective_connection = std::nullopt`，
   成员 `effective_connection_`。
5. `Save()` 优先用 `effective_connection_`，退化时才回退到
   `ConnectionFromRequest(options_.transport_request)`。

### 2.3 为什么“无权威证据写 null”

`ProtocolVersionSnapshot::effective` 在证据来源为 `VersionSource::Unknown` 时
只是一个占位值（`1.x-conservative` 策略下的能力标识），**不代表对端真实版本**。
把它写进配置文件，下次加载会以错误前提做版本裁决。写 `null` 则让加载端回到
CONNECT 协商路径重新取证——这是唯一诚实且安全的做法。

`include/libxcp/protocol_version.hpp:182-186` 的注释也印证：`capped` 是额外
状态标志，`reported` 保留原值、`effective` 封顶，两者不能混用。

### 2.4 旧文件的容错（`ProjectConnectionToRequest`）

对 `auto` / `auto-legacy` 与显式对齐并存的组合，丢弃对齐值：

```cpp
const bool profile_is_auto =
    request.tcp_profile_auto || request.tcp_profile_auto_legacy;
if (!profile_is_auto) {
    request.packet_alignment_bits = connection.packet_alignment_bits;
}
```

理由：`auto` 的语义就是“对齐由胜出候选决定”，残留值是冗余字段；这里按语义
舍弃，**不构成推断**（原注释“不做任何补全或推断”因此补充了这一例外说明）。

这不只是一处兼容补丁——**新写的文件本来就不会出现这个组合**，它只用于让用户
手上已有的、由旧版本 `save` 出来的配置文件能够加载。

## 3 改动文件

| 文件 | 改动 |
| --- | --- |
| `tools/xcp_master/project_file.hpp` | 新增 `EffectiveConnection`（含注释约 57 行）、`ConnectionFromEffective`、`MakeEffectiveConnection`、`AlignmentBitsOf`、`WireProfileText`；`ProjectConnectionToRequest` 增加 auto 容错 |
| `tools/xcp_master/main.cpp` | `selection` 去 const；嗅探后回填胜出候选；组装 `effective_connection`；`CliApp` 构造参数与成员；`Save()` 改走生效值 |
| `tests/xcp_master_project_file_test.cpp` | 新增 `ProjectEffectiveConnection` 套件 9 个用例 |
| `tools/xcp_master/README.md` | Project file 段新增“connection 记录实际运行值”三条要点与旧文件容错说明 |

## 4 测试

### 4.1 新增单元用例（9 个，全部通过）

| 用例 | 钉住的行为 |
| --- | --- |
| `WritesConcreteProfileNotAuto` | 生效值写出的是 `asam-v1.5` 而非 `auto` |
| `RoundTripsThroughRequest` | 写出→读回，Profile/对齐/PL/TL 全部一致 |
| `DropsExplicitAlignmentUnderAuto` | 旧文件 auto + 对齐：对齐被丢弃，不报错 |
| `KeepsAlignmentUnderConcreteProfile` | 容错只针对 auto，具体 Profile 下对齐照常保留 |
| `LeavesVersionsNullWithoutEvidence` | 无证据时 JSON 里确实是 `null` 字面量 |
| `WritesAuthoritativeVersions` | 有证据时写 `1.3` / `1.4` |
| `UdpHasNoProfile` | UDP 不写 Profile |
| `RecordsEndpointFromA2l` | 端点来源被正确记录 |
| `AlignmentAndProfileHelpers` | `Unknown` 的边界换算返回空 |

### 4.2 构建

```
cmake --build cmake-build-release -j 4
```

零错误；仅 glog 头文件的既有 C4244/C4996 警告。

### 4.3 回归

```
ctest -R "XcpMaster|A2l"
```

188 项中 **186 通过、1 跳过（既有）、2 失败**。

两个失败都在 `XcpMasterRenderBenchTest` 性能基准，**与本次改动无关**：

| 用例 | 阈值 | 实测（连续 5 次） |
| --- | --- | --- |
| `SearchesAndFiltersLargeA2lWithinBudget` | `90 × 3 = 270 ms`（硬上限 1500 ms） | 229.4 / 280.0 / 257.5 / 247.0 / 267.7 ms |
| `RendersFrameWithFieldSizeCandidateList` | 单独运行自动 `[SKIPPED]`（能力系数 9.27 > 容忍 2.0） | 1412.7 ms（硬上限 1500 ms 内） |

判定为机器抖动而非代码回退，依据：

1. 本次改动未触及检索与渲染路径，全部落在 `Save()`、`EffectiveConnection`、
   `ProjectConnectionToRequest`；
2. 检索耗时在 270 ms 阈值上下双向抖动（229/280/258/247/268），不是单向劣化；
3. 渲染基准单独运行时会自行判定“本机算力与参考机不匹配”并 `GTEST_SKIP`，
   ctest 并行下的失败是在与重负载任务争抢 CPU；
4. 两例的绝对硬上限（1500 ms）均未触及。

## 5 已知限制

1. `notes` 字段当前未填充：诊断信息仍走 `std::cout` 打印，没有并入配置文件。
   如需在文件里留下“Profile 来自嗅探”这类说明，需先确定是否值得让 JSON
   承载诊断文本。
2. 传输层选择仍是“默认 UDP + 用户显式指定”，双路嗅探（T1/T4/T5/T6）未实现，
   见 `code-plan/xcp_master_传输层与对齐从A2L推导_实施计划.md` §9。
3. IncludeResolver（T6）未做：含 `/include` 的 A2L 在 TCP alignment 核对时
   仍判 kUntrusted（既有行为，本次不改）。
4. 版本快照的 `effective` 在 cap 场景下是封顶值；`capped` 标志未写入配置文件，
   重新加载会重走一遍封顶逻辑（结果一致，只是多一次协商）。

## 6 复现命令

```bash
# 构建
cmake --build cmake-build-release --target xcp_master XcpMasterSymbolPolicyTest -j 4

# 新用例
./cmake-build-release/tests/XcpMasterSymbolPolicyTest.exe \
    --gtest_filter="ProjectEffectiveConnection.*"

# 回归（Windows 需先把 TMPDIR/TEMP/TMP 指向可写目录，否则大量既有用例
# 因沙箱 %TEMP% 不可写而误报 Access is denied）
ctest -R "XcpMaster|A2l" --output-on-failure
```

## 7 人工验收

1. `xcp_master.exe --a2l <A2L> --transport tcp --host <IP> --port <P> --tcp-profile auto --mode daq`
2. 连上后登记若干标定对象，输入 `save`
3. 打开生成的 `<A2L名>.xcp.json`，确认：
   - `tcp_profile` 是 `asam-v1.5` 或 `asam-v1.4`，**不是** `auto`
   - `packet_alignment_bits` 是胜出候选的位宽
   - `protocol_version` / `transport_version` 若 ECU 实现了 GET_VERSION 则是具体
     版本，否则为 `null`
4. 用 `xcp_master.exe -c <该文件>` 重启，确认能正常连上，不再报
   `--tcp-profile auto 与 --packet-alignment 冲突`