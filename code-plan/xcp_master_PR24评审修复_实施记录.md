# xcp_master PR#24 评审批次修复 —— 实施记录

- 对应评审：`https://github.com/songjmcn/libxcp/pull/24`，`copilot-pull-request-reviewer[bot]`，review id 5890486594，
  「🟡 Changes recommended」，6 open issues（3 High / 3 Medium）。
- 对应计划：[xcp_master_PR24评审修复_实施计划.md](xcp_master_PR24评审修复_实施计划.md)。
- 本记录只覆盖**评审修复这一批**改动；PR 本身的端点嗅探特性见
  [xcp_master_传输层与对齐从A2L推导_实施记录.md](xcp_master_传输层与对齐从A2L推导_实施记录.md)。

---

## 1 评审问题与修复对照

| # | 严重度 | 原位置 | 问题 | 修复 | 证据 |
| --- | --- | --- | --- | --- | --- |
| 1 | High | `tools/xcp_master/main.cpp:4354` | `instance_count` 只保存、不参与判歧义；v9 兼容路径（一条记录 + 计数 > 1）被当成唯一端点 | 摘要归纳下沉为纯函数，`instance_count > 1` 且无展开明细时直接 `ambiguous`（fail closed） | `tools/xcp_master/transport_sniff.hpp:386`、`tests/xcp_master_transport_sniff_test.cpp` `InstanceCountAboveOneWithoutDetailsIsAmbiguous` |
| 2 | High | `tools/xcp_master/main.cpp:4363` | 判歧义只比 host/port，漏对齐与传输层版本 | 四要素（host / port / `PACKET_ALIGNMENT` / TL 版本）去重，与核心 `SameWireTarget()` 同义 | `transport_sniff.hpp` `SummarizeA2lEndpoints()`；`DifferentTransportLayerVersionIsAmbiguous`、`DifferentAlignmentIsAmbiguous` |
| 3 | High | `tools/xcp_master/main.cpp:4533` | 只给 `--host`（或只给 `--port`）时，成对校验在 `ResolveTransportSelection()` 里，晚于嗅探 → 先把 A2L 端点连一遍才发现参数不全 | 成对校验前移到 `ParseOptions()`，解析层保留同名检查作纵深防御 | `main.cpp:456`；CLI 冒烟第 4 段 |
| 4 | Medium | `tools/xcp_master/main.cpp:4578` | UDP 嗅探沿用 `XcpMasterOptions` 默认：`max_retries = 2`、`autodetect_version = true` | 预算对齐 `VerifyTcpFramingCandidate()`：1000 ms / `max_retries = 0` / `autodetect_version = false` | `main.cpp` UDP 分支 |
| 5 | Medium | `tools/xcp_master/main.cpp:4599` | 任何异常都留给 `verdict` 的默认值 `Rejected`，与注释「按未决记录（退出码 4）」不符 | 复用 `ClassifyFramingFailure(error.Category(), udp_connected)`；非 XCP 异常记 `Inconclusive` | `main.cpp:4632`；`udp_connected` 于 `:4563` 声明、`:4603` 置位 |
| 6 | Medium | `tools/xcp_master/transport_sniff.hpp:291` | 失败摘要固定写「TCP 与 UDP 均未得到合法响应」 | 按 `attempts` 里实际出现的传输层生成四态文案 | `transport_sniff.hpp:345-362`；`FailureReportNamesOnlyAttemptedTransports` 等 4 例 |

### 评审未覆盖、本批一并处理

| 编号 | 处理方式 |
| --- | --- |
| E1 排序使用未核验的 A2L 对齐 | **只加注释不改行为**：候选序列只决定试连顺序，全部候选最终都会尝试，正式对齐仍由 `CrossCheckA2lTcpAlignment()` 双源核对决定 |
| E2 空计划退出码 `3` 语义错误 | 新增 `kAutoNegotiationNoEndpoint = 2`，与「对端全部拒绝」区分 |
| E3 UDP 缺二次往返与显式断开 | 补 `(void)candidate_master->QueryStatus()` 与 `candidate_master->Disconnect()`（断开异常吞掉，结论不变） |

---

## 2 改动清单

| 文件 | 行数 | 关键改动 |
| --- | --- | --- |
| `tools/xcp_master/transport_sniff.hpp` | +147 | 新增 `A2lEndpointDeclaration`、`SummarizeA2lEndpoints()`（声明 + inline 实现）；`FailureReport()` 四态汇总；候选序列注释补充 E1 说明 |
| `tools/xcp_master/main.cpp` | +73 / −30 | `SummarizeA2lEndpoint()` 改为薄适配层；`ParseOptions()` 增加 host/port 成对校验；空计划返回 `kAutoNegotiationNoEndpoint`；UDP 分支重写（预算 / `QueryStatus` / 显式断开 / 三态分类） |
| `tools/xcp_master/cli_selection.hpp` | +5 | `kAutoNegotiationNoEndpoint = 2` |
| `tests/xcp_master_transport_sniff_test.cpp` | +222 | `TransportSniffSummary` 10 例、`TransportSniffReport` 新增 4 例 |
| `tests/xcp_master_transport_inference_test.cmake` | +26 / −2 | 第 4 段：只给 `--host` 必须在发起连接前被拒，且输出不含 `试连`，退出码 2 |
| `tools/xcp_master/README.md` | +3 / −2 | 退出码补 `2`；行为矩阵补「端点不完整」一行；判歧义口径写明四要素与 v9 fail closed |

合计 6 文件、466 增 / 37 删。全部 LF（`git ls-files --eol` 均为 `i/lf w/lf`），`git diff --stat` 无整文件重写的假差异。

---

## 3 设计决策落实与偏离

| 决策 | 落实 |
| --- | --- |
| D1 摘要归纳下沉为纯函数 | `SummarizeA2lEndpoints()` 放在 `transport_sniff.hpp`，入参是自有的 `A2lEndpointDeclaration`，不引入 a2lbridge 依赖；`main.cpp` 的 `SummarizeA2lEndpoint()` 只做结构映射 |
| D2 四要素判歧义 | 与 `SameWireTarget()` 同口径，未声明的对齐/TL 版本归一化为 0 后参与去重 |
| D3 v9 路径 fail closed | 单条记录 `instance_count > 1` → `ambiguous` |
| D4 成对校验前置 | `ParseOptions()` 里 `host_given != port_given` 即报错，早于任何连接 |
| D5 UDP 预算对齐 TCP | 1000 ms 命令/恢复超时、`max_retries = 0`、`autodetect_version = false`、二次往返、显式断开 |
| D6 三态分类复用 | `ClassifyFramingFailure()` 直接复用，不另写判定 |
| D7 摘要按实际尝试生成 | 四态：`TCP 与 UDP 均未…` / `TCP 候选均未…` / `UDP 端点未…` / `没有任何候选可试` |
| D8 空计划退出码 2 | `kAutoNegotiationNoEndpoint` |
| D9 E1 只加注释 | 未改候选排序行为 |

**偏离说明（1 处）**：计划 §4 写的是「`TransportSniffSummary` 9 例 + `TransportSniffReport` 3 例」，
实际落地 10 + 4 例。多出的两例是 `UndeclaredVersionStaysEmpty`（钉住「未声明版本」在摘要里是空
`optional`，而不是版本 0）与 `FailureReportOnEmptyPlan`（钉住空计划的汇总文案，配合 D8 的退出码改动）。
两者都在计划意图内，只是把「未声明」和「没有候选」两种边界单独拆成了用例。

---

## 4 测试结果

### 4.1 单元测试（`cmake-build-release/tests/XcpMasterSymbolPolicyTest.exe`）

```
--gtest_filter="TransportSniff*:IncludeResolverTest.*:TransportSelection.*:ProjectFile.*"
[==========] 113 tests from 7 test suites ran. (20 ms total)
[  PASSED  ] 113 tests.
```

其中本批新增：`TransportSniffSummary` 10 例、`TransportSniffReport` 4 例。
（修复前同一过滤器的基线是 99 例。）

### 4.2 CLI 冒烟（`tests/xcp_master_transport_inference_test.cmake`）

```
cmake -DXCP_MASTER="<cmake-build-release/tools/xcp_master/xcp_master.exe>" \
      -P tests/xcp_master_transport_inference_test.cmake
-- 传输层推导 CLI 冒烟通过
```

第 4 段实测：`xcp_master --a2l <dummy> --host 127.0.0.1` → 输出含
`--host 与 --port 必须成对出现（只给其中一个无法确定端点）`、不含 `试连`、退出码 `2`。

### 4.3 定向回归

```
ctest -R "XcpMaster|A2l|Transport"
100% tests passed, 0 tests failed out of 358
Total Test time (real) = 97.69 sec
```

2 项未运行均为既有 Skipped：`AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`、
`XcpMasterRenderBenchTest.RendersFrameWithFieldSizeCandidateList`。
（修复前基线 344，差值即本批新增的 14 个用例。）

### 4.4 全量回归

```
ctest --output-on-failure
100% tests passed, 0 tests failed out of 1446
Total Test time (real) = 327.60 sec
```

6 项未运行均为既有 Disabled/Skipped：`TcpStreamDecoderFinish.HeaderOnlyIsTruncated`(Disabled)、
`AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`、`SnapshotTest` 3 项、
`XcpMasterRenderBenchTest.RendersFrameWithFieldSizeCandidateList`。
（这次全量是在**行尾归一后重新构建**的产物上跑的，`ctest -N` 报告 `Total Tests: 1447`，即
1446 项实际运行 + 1 项既有 Disabled。此前一次全量报 `1299` 是构建树里 `gtest_discover_tests()`
产出未刷新的旧清单（`TcpAndUdp/XcpliteV12InteropTest.*` 这类参数化用例没有被登记），重新构建后
清单恢复完整。两次运行都是「0 失败」，差异只在于发现到的用例条数。

### 4.5 未覆盖项（诚实说明）

- **UDP 预算 / 三态分类没有单测**：需要真实 UDP 端点才能触发，抽成头文件会把 `XcpMasterOptions`
  拖进刻意保持「无 socket 依赖」的 `transport_sniff.hpp`。目前靠 CLI 行为与人工验收覆盖。
- **嗅探路径本身没有自动化端到端断言**：`main()` 的 `IsInteractiveTerminal()` 守卫早于 A2L 加载，
  管道下嗅探根本不执行，只能用交互终端人工验证（见 §6）。

---

## 5 已知限制

1. 判歧义变严后，同一 A2L 里声明了「同址同端口但 TL 版本不同」的端点会从「静默取首实例」变成
   「要求显式给端点」。这是有意的行为变化：原先的结果取决于 A2L 里的书写顺序。
2. 成对校验前移后，`--host` 单独出现不再能「靠 A2L 补端口」。与解析层原有契约一致，但现在拒绝得更早。
3. 退出码新增 `2` 后，自动化脚本若把「非 0 即失败」改成「分类处理」，需要区分 2（配置不全）/ 3（对端拒绝）/ 4（未决）。
4. E1 只加注释：嗅探仍用未核验的 A2L 对齐排候选顺序，命中顺序可能不是最优（不影响正确性）。
5. UDP 嗅探固定按 8 bit 试探，`16/32 bit` 的对齐仍由解析阶段依据 A2L 证据决定。

---

## 6 复现命令

```bash
# 构建（MSVC Release，需先 call vcvars64.bat）
cmake --build cmake-build-release --target xcp_master -j 4
cmake --build cmake-build-release --target XcpMasterSymbolPolicyTest -j 4

# 单元测试
cmake-build-release/tests/XcpMasterSymbolPolicyTest.exe \
  --gtest_filter="TransportSniff*:IncludeResolverTest.*:TransportSelection.*:ProjectFile.*"

# CLI 冒烟
cmake -DXCP_MASTER="$(cygpath -w "$PWD/cmake-build-release/tools/xcp_master/xcp_master.exe")" \
      -P tests/xcp_master_transport_inference_test.cmake

# 回归（Windows 沙箱下必须先设临时目录，否则既有用例误报 Access is denied）
#   set TMPDIR/TEMP/TMP 指向可写目录后：
netstat -ano | findstr :5555   # 残留 slave 会让 Example 系列假失败
ctest -R "XcpMaster|A2l|Transport" --output-on-failure
ctest --output-on-failure
```

### 人工验收（交互终端）

1. `xcp_master --a2l <只声明 TCP 的 A2L>` → 应打印嗅探计划与逐条 `试连`，最终连上。
2. `xcp_master --a2l <同时声明 TCP/UDP 的 A2L>` → TCP 候选在前。
3. `xcp_master --a2l <x> --host 127.0.0.1` → 立即报成对错误，不出现任何 `试连`。
4. `xcp_master --a2l <只声明 UDP 的 A2L>` → 只出现 UDP 试连；失败时汇总行只提 UDP。
5. 多 MODULE 的 A2L 不给 `--module` → 打印歧义提示且不把端点当嗅探依据。

---

## 7 改动文件清单

```
 M tools/xcp_master/cli_selection.hpp
 M tools/xcp_master/main.cpp
 M tools/xcp_master/transport_sniff.hpp
 M tools/xcp_master/README.md
 M tests/xcp_master_transport_sniff_test.cpp
 M tests/xcp_master_transport_inference_test.cmake
?? code-plan/xcp_master_PR24评审修复_实施计划.md
?? code-plan/xcp_master_PR24评审修复_实施记录.md
```

---

## 8 复跑记录

| 时间 | 命令 | 结果 |
| --- | --- | --- |
| 2026-11-13 23:00 | `cmake --build cmake-build-release --target xcp_master -j 4` | `[30/30] Linking CXX executable tools\xcp_master\xcp_master.exe`，零错误 |
| 2026-11-13 23:10 | `cmake --build cmake-build-release --target XcpMasterSymbolPolicyTest -j 4` | `[6/6] Linking CXX executable tests\XcpMasterSymbolPolicyTest.exe`，零错误 |
| 2026-11-13 23:10 | 定向单元测试 | 113/113 通过（20 ms） |
| 2026-11-13 23:12 | CLI 冒烟 | 通过 |
| 2026-11-13 23:14 | `ctest -R "XcpMaster\|A2l\|Transport"` | 358/358 通过（97.69 s） |
| 2026-11-13 23:17 | `ctest --output-on-failure`（全量，归一前构建树） | 1299/1299 通过（308.92 s），6 项既有 Disabled/Skipped；发现清单未刷新，条数偏少 |
| 2026-11-13 23:26 | 行尾归一（505 个文件 CRLF→LF） | 内容零变化；`git ls-files --eol` 中 `i/lf w/crlf` 归零 |
| 2026-11-13 23:27 | `cmake --build cmake-build-release -j 4` | `[212/212] Linking ... XcpMasterTcpIntegration.exe`，零错误 |
| 2026-11-13 23:34 | `ctest --output-on-failure`（全量，归一后重建） | 1446/1446 通过（327.60 s），`Total Tests: 1447`，6 项既有 Disabled/Skipped |

---

## 9 行尾归一（收尾工作）

`git status` 里曾长期存在两类「看不出原因」的修改：

1. **纯行尾差异**：仓库大量文件处于「索引 LF / 工作区 CRLF」，但没有 `.gitattributes`，
   `core.autocrlf` 也未设置，git 每次都把它们当成整文件重写（`CMakeLists.txt` 888 行、
   `src/memory_access.cpp` 1192 行等）。
2. **stat 缓存陈旧**：文件被脚本重写后 mtime 变了，`git status` 依据 stat 判定「已修改」，
   而 `git diff` 与 `git hash-object` 都证明内容与索引逐字节相同。

处置：

- 对 `git ls-files --eol` 中 `i/lf w/crlf` 的 **505 个文件**做 CRLF→LF 归一（二进制安全读写，
  只替换 `\r\n`）；归一后用 `git hash-object` 与 `git rev-parse :<path>` 逐文件比对，
  505 个全部与索引相同，确认**内容零变化**。
- 对内容相同的条目执行 `git add`，让索引写入新的 stat 信息，`git status` 不再误报。

结果：`git status --short` 只剩 8 个真实修改 + 2 个新增文档；`git diff --stat` 为
`8 files changed, 482 insertions(+), 39 deletions(-)`，与 `git diff --ignore-cr-at-eol --stat`
完全一致。

**后续约定**：本仓索引一律 LF；新增或修改文件时一律写 LF；提交前用
`git diff --ignore-cr-at-eol --stat` 复核真实改动量，若 `git diff --stat` 出现整文件行数即为行尾事故。
