# xcp_master PR#24 评审问题修复 实施计划

> 对应 GitHub：`songjmcn/libxcp` PR #24（`feat: xcp_master 传输层与对齐从 A2L 推导（端点嗅探）+ A2L adapter 并入 a2l-sdk`）
> 评审来源：Copilot Pull Request Reviewer，6 条未解决问题（3 High / 3 Medium）
> 本文档只覆盖「嗅探与端点推断」相关的评审问题；A2L adapter 归并部分未被提出问题，不在范围内。
>
> **状态：已按 F1–F7 实施完毕**（2026-11-13）。实施结果、测试数据与偏离说明见
> [xcp_master_PR24评审修复_实施记录.md](xcp_master_PR24评审修复_实施记录.md)。
> 与计划的唯一偏离：测试用例实际为 `TransportSniffSummary` 10 例 + `TransportSniffReport` 4 例
> （计划写 9 + 3），多出的两例分别钉「未声明版本是空 optional」与「空计划汇总文案」。

---

## 1. 评审问题与本地核实结论

逐条对着当前工作树核对过代码，**6 条全部成立**；另列 3 条评审未覆盖的同类缺陷（E1–E3）。

| # | 严重度 | 评审指出 | 代码位置 | 核实结论 |
| --- | --- | --- | --- | --- |
| 1 | High | `instance_count` 只保存不参与歧义判定 | `tools/xcp_master/main.cpp:4354` | 成立。摘要只按 host/port 判歧义，多实例时仍用首实例生成计划 |
| 2 | High | 歧义检查忽略 TL 版本与对齐冲突 | `tools/xcp_master/main.cpp:4363` | 成立。核心契约是四要素（`SameWireTarget`），这里只比两要素 |
| 3 | High | 不完整 CLI 端点会先嗅探 A2L 目标 | `tools/xcp_master/main.cpp:4533` | 成立。成对校验在 `ResolveTransportSelection()` 里，跑在嗅探之后 |
| 4 | Medium | UDP 嗅探未禁用重试与版本探测 | `tools/xcp_master/main.cpp:4578` | 成立。默认 `max_retries=2`、`autodetect_version=true` |
| 5 | Medium | UDP 异常被记为「对端拒绝」而非「未决」 | `tools/xcp_master/main.cpp:4599` | 成立，且与同段注释自相矛盾（注释写「按未决记录」） |
| 6 | Medium | 失败摘要固定宣称 TCP 与 UDP 都试过 | `tools/xcp_master/transport_sniff.hpp:291` | 成立。`TcpOnly` 或 UDP 被跳过时文案错误 |

### 1.1 三个结构性根因

1. **证据链被重写而不是复用**：`SummarizeA2lEndpoint()` 是 `A2lTransportFacts` 构建逻辑（`main.cpp:4651` 起）的简化副本，只保留 host/port，丢掉多实例计数与四要素一致性 → 问题 1、2。
2. **校验后置**：host/port 成对校验活在 `ResolveTransportSelection()`（`src/transport_selection.cpp:191`），而嗅探在它之前，于是「缺一个参数」会先向 A2L 目标发起握手 → 问题 3。
3. **UDP 探测枝是 TCP 验证组件的手写近似**：没有复用 `VerifyTcpFramingCandidate()` 的探测预算（`max_retries=0`、`autodetect_version=false`、二次往返）与三态分类（`ClassifyFramingFailure`）→ 问题 4、5。

### 1.2 关键事实核查（决定修法）

- `IfDataXcpInfo::transports` **逐端点**展开（`thirdparty/a2l-sdk/a2lbridge/src/a2l_dto_map.cpp:566-577`：`for (const auto& source : dto.endpoints)`），因此同类多实例的每一项都在 `transports` 里，可以逐项比较四要素。
- `instance_count` 是「同类声明个数」（`a2l_dto_map.cpp:557-563`），在 v9 兼容路径（`dto.endpoints` 为空）下只有一条记录但计数可能 >1 —— 此时无法去重，必须 fail closed。
- `BuildTcpFramingCandidates()` 只做**排序**，`PacketAlignment::Unknown` 时返回全部 4 个默认候选（`src/tcp_framing_candidates.cpp:92-100`），不会丢候选。
- `FramingVerificationResult::verdict` 默认值就是 `Rejected`（`include/libxcp/tcp_framing_verdict.hpp:67`），这正是问题 5 的机制。

---

## 2. 设计决策

| 编号 | 决策 | 理由 |
| --- | --- | --- |
| D1 | 把端点摘要下沉为**纯函数** `SummarizeA2lEndpoints(declarations, module_ambiguous)`，放进 `tools/xcp_master/transport_sniff.hpp` | `main.cpp` 匿名命名空间里的函数无法单测；与 `BuildDualSniffPlan()` 同层，天然可测，不引入 a2lbridge 依赖（用自有的 `A2lEndpointDeclaration` 结构承接） |
| D2 | 歧义判定改为「四要素去重后 distinct > 1」 | 与核心 `SameWireTarget()`（`include/libxcp/transport_selection.hpp:134`）完全同义，避免 `ResolveTransportSelection()` 阶段推翻嗅探结论 |
| D3 | v9 兼容路径（只有一条记录但 `instance_count > 1`）直接 `ambiguous` | 无法验证唯一性时 fail closed，宁可让用户显式给 `--host/--port` |
| D4 | host/port 成对校验前置到 `ParseOptions()`，文本与 `transport_selection.cpp` 保持一致 | 嗅探之前就拒绝，不再向用户没指定的 ECU 握手；`ResolveTransportSelection()` 的检查保留作纵深防御 |
| D5 | UDP 探测预算与 TCP 验证组件对齐（1000 ms / `max_retries=0` / `autodetect_version=false` / 二次往返 / 显式断开） | 一次「每个条目只尝试一次」的尝试预算才是 `SniffTransport()` 承诺的语义 |
| D6 | UDP 失败分类复用 `ClassifyFramingFailure(error.Category(), connected)` | 与 TCP 同源，`Timeout/TransportError(未建立)/RecoveryFailed → Inconclusive`，避免把「没回话」报成「拒绝」 |
| D7 | 失败摘要按 `attempts` 实际出现的传输层生成 | 文案必须与逐条列表一致 |
| D8 | 空计划退出码从 `3` 改为 `2` | 3 的语义是「对端在回话但都不认」，空计划是配置不全（参数错误） |
| D9 | E1（排序用未核验对齐）**只加注释不改行为** | 所有候选最终都会被尝试，改了只会让 16/32 bit ECU 的首次命中变慢；注释说明「仅排序、不采信」即可 |

---

## 3. 修复步骤

### F1 端点摘要下沉 + 四要素判歧义（问题 1、2）

`tools/xcp_master/transport_sniff.hpp`：

```cpp
/// @brief 一条端点声明（由调用方从 IfDataXcpInfo::transports 映射而来）
struct A2lEndpointDeclaration {
    std::string host;
    std::uint16_t port{0};
    std::uint16_t transport_layer_version{0};
    std::uint8_t packet_alignment_bits{0};   ///< 未声明时为 0
    std::uint32_t instance_count{0};
};

/// @brief 归纳同类端点：四要素去重后不止一个 → ambiguous
A2lEndpointSummary SummarizeA2lEndpoints(
    const std::vector<A2lEndpointDeclaration>& declarations,
    bool module_ambiguous);
```

规则：
1. 空列表 → `declared = false`；
2. 逐项归一化（`alignment = 声明 ? 值 : 0`；`version = 0` 表示未声明），按 `(host, port, alignment, version)` 去重；
3. `distinct.size() > 1` → `ambiguous = true`；
4. 只有一条记录但 `instance_count > 1`（v9 兼容路径）→ `ambiguous = true`；
5. `module_ambiguous == true` → `ambiguous = true`；
6. 唯一时用该条填 host/port/version/alignment，`instance_count` 取各条最大值。

`tools/xcp_master/main.cpp` 的 `SummarizeA2lEndpoint()` 改为薄适配层：按 `kind` 过滤 `info.transports` 映射成 `A2lEndpointDeclaration`，再调上面的纯函数。

### F2 CLI 端点成对校验前置（问题 3）

`tools/xcp_master/main.cpp` `ParseOptions()` 尾部、`--a2l` 必填检查之前插入：

```cpp
if (host_given != port_given) {
    error = "--host 与 --port 必须成对出现（只给其中一个无法确定端点）";
    return std::nullopt;
}
```

### F3 UDP 探测预算与收尾对齐（问题 4 + E3）

`tools/xcp_master/main.cpp` UDP 分支：

```cpp
calmcar::xcp::XcpMasterOptions sniff_options;
sniff_options.timeouts.command_timeout = std::chrono::milliseconds(1000);
sniff_options.timeouts.synch_timeout = std::chrono::milliseconds(1000);
sniff_options.timeouts.max_retries = 0;        // 每个条目只尝试一次
sniff_options.autodetect_version = false;      // 探测阶段不追版本
```

并在 `Connect()` 之后补 `(void)candidate_master->QueryStatus();`、在给出结论前显式 `Disconnect()`（吞异常），与 `VerifyTcpFramingCandidate()` 的收尾一致。

### F4 UDP 失败三态分类（问题 5）

```cpp
bool connected = false;
try {
    candidate_master->Connect();
    connected = true;
    ...
} catch (const calmcar::xcp::XcpException& error) {
    verdict.verdict = calmcar::xcp::ClassifyFramingFailure(
        error.Category(), connected);
    verdict.reason = error.what();
} catch (const std::exception& error) {
    verdict.reason = error.what();   // 非 XCP 异常与 TCP 侧一致，保持 Rejected
}
```

### F5 失败摘要按实际尝试生成（问题 6）

`TransportSniffResult::FailureReport()` 汇总行三态：

| 实际尝试 | 文案 |
| --- | --- |
| TCP + UDP | `TCP 与 UDP 均未得到合法响应` |
| 仅 TCP | `TCP 候选均未得到合法响应` |
| 仅 UDP | `UDP 端点未得到合法响应` |
| 空计划 | `没有任何候选可试` |

### F6 空计划退出码（E2）

`main.cpp` 空计划分支返回 `2`（参数/配置错误），并在文案里补一句「请用 --host/--port 指定端点，或检查 A2L」。

### F7 E1 注释澄清

`BuildDualSniffPlan()` 里 alignment 证据的用法处补注释：仅用于候选排序，不代表采信该声明；`SummarizeA2lEndpoint()` 处同步说明「对齐在此只作排序线索，正式连接由 `CrossCheckA2lTcpAlignment` 双源核验」。

---

## 4. 测试计划

### 4.1 新增单测（`tests/xcp_master_transport_sniff_test.cpp`）

新增 suite `TransportSniffSummary`：

| 用例 | 断言 |
| --- | --- |
| `SingleDeclarationIsNotAmbiguous` | 1 条 → `declared`、字段正确、非歧义 |
| `IdenticalDuplicatesAreNotAmbiguous` | 2 条完全相同 → 非歧义（去重生效），`instance_count == 2` |
| `DifferentHostIsAmbiguous` | 同端口不同 host → 歧义 |
| `DifferentPortIsAmbiguous` | 同 host 不同端口 → 歧义 |
| `DifferentTransportLayerVersionIsAmbiguous` | 同址不同 TL 版本 → 歧义 |
| `DifferentAlignmentIsAmbiguous` | 同址不同对齐 → 歧义 |
| `InstanceCountAboveOneWithoutDetailsIsAmbiguous` | 单条但 `instance_count = 2` → 歧义（v9 路径） |
| `ModuleAmbiguityPropagates` | `module_ambiguous = true` → 歧义 |
| `EmptyDeclarationsStayUndeclared` | 空列表 → `declared = false` |

### 4.2 补充报告文案测试（同文件 `TransportSniffReport`）

`FailureReportNamesOnlyAttemptedTransports`（仅 TCP → 不含「UDP」）、`FailureReportNamesUdpOnly`、`FailureReportNamesBothTransports`。

### 4.3 CLI 冒烟（`tests/xcp_master_transport_inference_test.cmake`）

新增一条：`xcp_master --a2l <a2l> --host 127.0.0.1`（只给 host）→

- 必须出现 `--host 与 --port 必须成对出现`；
- 输出中**不得**出现 `试连`（证明嗅探没有先行）。

### 4.4 关于 UDP 预算（F3）的可测性说明

嗅探谓词依赖真实 UDP 端点，单测不可达；`MakeUdpSniffOptions()` 若抽成独立头文件又会把 `XcpMasterOptions` 拖进 `transport_sniff.hpp`（该头刻意保持无 socket 依赖）。因此该项以「代码对齐 TCP 验证组件」+ 交互冒烟验证，`实施记录` 里注明。

---

## 5. 验证命令

```bash
# 构建
cmd.exe /c /tmp/b.bat          # vcvars64 + cmake --build cmake-build-release -j 4

# 单测（新增 12 例 + 既有 99 例）
./cmake-build-release/tests/XcpMasterSymbolPolicyTest.exe \
    --gtest_filter="TransportSniff*:IncludeResolverTest.*:TransportSelection.*:ProjectFile.*"

# 定向回归（基线 344）
ctest -R "XcpMaster|A2l|Transport"

# 全量（基线 1284；先确认 5555 未被残留 slave 占用）
netstat -ano | findstr :5555
ctest
```

---

## 6. 风险与边界

| 风险 | 处置 |
| --- | --- |
| 判歧义变严可能让原本能连的 A2L 变成「请显式指定端点」 | 只在四要素**确实不同**时才判歧义；完全重复声明（生成器重复输出）仍可自动判定 |
| 前置成对校验可能改变既有 CLI 诊断顺序 | 文本与 `transport_selection.cpp` 完全一致，既有测试未断言该分支的执行时机 |
| 空计划退出码 3→2 影响脚本 | 仓库内无测试断言该路径的退出码；`README` 与实施记录同步说明 |
| `SummarizeA2lEndpoints` 下沉后 `main.cpp` 行数变化 | 补丁脚本按 `count == 1` 锚定，改完用 `git diff --stat` 核对数量级（本仓索引一律 LF） |

---

## 7. 交付物

1. `tools/xcp_master/transport_sniff.hpp`：`A2lEndpointDeclaration` + `SummarizeA2lEndpoints()` + `FailureReport()` 文案三态 + E1 注释。
2. `tools/xcp_master/main.cpp`：摘要适配层、成对校验前置、UDP 预算/收尾/三态分类、空计划退出码、E1 注释。
3. `tests/xcp_master_transport_sniff_test.cpp`：新增 12 例。
4. `tests/xcp_master_transport_inference_test.cmake`：补一条不成对端点冒烟。
5. `tools/xcp_master/README.md`：失败摘要与退出码语义更新。
6. `code-plan/xcp_master_PR24评审修复_实施记录.md`：改动、测试数据、逐条对应评审编号。

