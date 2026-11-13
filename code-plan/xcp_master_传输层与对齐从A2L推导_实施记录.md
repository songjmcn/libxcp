# xcp_master 传输层与对齐从 A2L 推导 —— 实施记录

> 对应计划：`code-plan/xcp_master_传输层与对齐从A2L推导_实施计划.md`（762 行，§10 步骤 T1–T12）
> 实施范围：T1–T12 全部完成；用户裁决见 §1。
> 日期：2026-11-12

## 1. 问题与目标

改动前 `xcp_master` 只做了一半：TCP 端点能从 A2L 的 `XCP_ON_TCP_IP` 回落，但

1. `--transport` 默认 `udp`，而 UDP 按"规则 9"**永不允许**从 A2L 取端点，于是只给 `--a2l` 时必然报
   `--a2l, --host 与 --port 为必填项`；
2. 显式 `--transport tcp` 时又必须先给 `--tcp-profile`（规则 1），否则直接失败；
3. A2L 里同时声明 TCP 和 UDP 时，没有任何机制替用户试出哪个能连上。

用户诉求（m03846）是"理论上只加载 a2l 文件就可以自动做测量和标定，除非明确 ip 地址或者 port 有问题"，
并选择完整方案（m03888：不做只补提示的最小改动，一次改完不返工）。

改动后的目标行为：

| 输入 | 行为 |
| --- | --- |
| `--a2l x.a2l` | 加载 A2L → 按声明构建嗅探计划（TCP 帧候选在前，UDP 在后）→ 首个合法响应即采用 |
| `--a2l x.a2l --transport tcp` | 只嗅 TCP；无 `--tcp-profile` 等价于 `auto` |
| `--a2l x.a2l --transport tcp --tcp-profile asam-v1.4` | 用该布局，不嗅探 |
| `--a2l x.a2l --transport udp --host … --port …` | 保持原契约，不用 A2L 端点 |
| `-c project.json` | 文件记录的是上次实际可用配置，按原样回放，不嗅探 |

## 2. 改动清单

| 文件 | 改动 | 对应步骤 |
| --- | --- | --- |
| `tools/xcp_master/transport_sniff.hpp` | **新增**（header-only）：`A2lEndpointSummary`、`SniffScope{Dual,TcpOnly}`、`SniffPlanEntry`、`SniffAttempt`、`TransportSniffResult`、`BuildDualSniffPlan()`、`SniffTransport()`、`SniffExitCode()`、`DescribeSniffEntry()`、`SniffTransportName()` | T1 |
| `tools/xcp_master/include_resolver.hpp` | **新增**（header-only）：`IncludeReadOptions`、`ReadIncludeFile()`、`MakeIncludeResolver()`、`kIncludeMaxBytes = 1 MiB` | T6 |
| `include/libxcp/transport_selection.hpp` | `TransportSelectionRequest` 增加 `bool transport_explicit{false}`（+11 行） | T4 |
| `src/transport_selection.cpp` | 规则 9 放宽：未显式指定 kind 时 UDP 可回落 `XCP_ON_UDP_IP`；显式指定仍要求 CLI 端点（+19/−? 行） | T4 |
| `tools/xcp_master/main.cpp` | 启动链路重排 + 嗅探 + 诊断输出（+248 行） | T5/T7/T8 |
| `tools/xcp_master/project_file.hpp` | `ProjectSession::config_source` 只读字段（序列化/解析各一处，+19 行） | T3 |
| `tests/xcp_master_transport_sniff_test.cpp` | **新增**：`TransportSniff*` 21 例 + `IncludeResolverTest` 7 例 | T9 |
| `tests/xcp_master_transport_selection_test.cpp` | 规则 9 前后对比 3 例（39 → 42） | T9 |
| `tests/xcp_master_project_file_test.cpp` | `config_source` 2 例（27 → 29） | T9 |
| `tests/xcp_master_transport_inference_test.cmake` | **新增** CLI 冒烟脚本 | T10 |
| `tests/CMakeLists.txt` | 注册 `XcpMasterToolTransportInference` | T10 |
| `tools/xcp_master/README.md` | 新增 `### Endpoint inference` 小节 + `config_source` 说明 | T12 |

净变更：17 个文件 446 增 38 删（不含本记录与计划文档）。

## 3. 关键实现决策

### D1 嗅探只决定"先试谁"，不决定"试不试"

计划 §9.3 的顺序规则（仅 TCP → TCP 先；仅 UDP → UDP 先；都声明 → TCP 先；都未声明 → TCP 先）落实为
`BuildDualSniffPlan()`：A2L 沉默时两路仍按命令行端点构造条目（P4），只有 `ambiguous`、
端点端口为 0、主机为空这三种"连了也没意义"的情况才跳过。

### D2 首胜即停，不做"两路都成功"仲裁

`SniffTransport()` 在首个 `Accepted` 立即返回。计划 §9.3 里"两路都响应时写 notes"的分支因此不可达——
这是**有意偏离**：让第二路也连一次，会在 ECU 侧多产生一次握手/断连，收益只是多一条注释。
该偏离已在本记录固化，避免后续评审当成漏实现。

### D3 嗅探胜出后不回填 host/port

胜出时只回填 `request.transport`（TCP 还固化 `tcp_profile` 与 `packet_alignment_bits`），
host/port 留给随后的 `ResolveTransportSelection()` 走 A2L 分支。这样 `ResolvedTransportSelection::endpoint_source`
才能如实是 `EndpointSource::A2l`，`save` 写出的 `endpoint_from_a2l` 也才是真话。
若在此处回填 host/port，来源会被误标成命令行，配置文件的语义就错了。

### D4 工程配置文件视为"显式指定"

`MergeProjectFileIntoOptions()` 在写入 `transport_request` 后置 `transport_explicit = true`。
文件里的 transport 是上次**实际连上**的结果，回放时再嗅一遍既慢又可能选出另一条路，
与"加载即复现"的预期相反。

### D5 `include_resolver.hpp` 做成 header-only

`ScanA2lTcpCandidate()` 的 `IncludeResolver` 只被调用一次、不递归（`src/a2l_tcp_alignment.cpp:317-333`
只读取内容判断是否含端点块，不二次下降），因此不需要深度限制与循环检测。做成 header-only 后
`tests/xcp_master_transport_sniff_test.cpp` 可直接包含并覆盖边界（相对/绝对/缺失/超限/目录/空串），
不必为它单开一个测试 target。

### D6 相对 include 以**原始** A2L 路径为基准

受控快照会被改名成 `a2l-snapshot-<hex>.a2l` 放进临时目录，以它为基准会把相对 include 全部解析到
错误目录。因此 `MakeIncludeResolver()` 的 `base_dir` 取 `options->a2l_path` 的父目录。
（这同时解释了为什么"TCP alignment 一直判不受信"的老问题只出现在含 include 的 A2L 上。）

## 4. 用户可见行为变化

1. `--a2l x.a2l` 不再报 `--a2l, --host 与 --port 为必填项`；改为真的去连 A2L 声明的端点。
2. `--transport tcp` 缺 `--tcp-profile` 时不再失败，而是打印
   `提示: 未指定 --tcp-profile，按 TCP 帧布局候选逐个试连（等效 --tcp-profile auto）。`
3. 报错文本变了：UDP 分支从 `--a2l, --host 与 --port 为必填项` 变为
   `UDP 传输需要 --host/--port，或 A2L 中声明 XCP_ON_UDP_IP 端点`（TCP 分支同理）。
   已确认全仓无测试断言旧文本。
4. 新的启动诊断（stdout）：`传输层嗅探：N 个候选依次试连，首个合法响应即采用` + 每条
   `  试连 <TCP host:port 布局/位宽 | UDP host:port>：<被拒|未决|接受>，耗时 N ms[，原因：…]` +
   `嗅探结论：使用 <TCP|UDP> <host:port>[（端点取自 A2L）]`；全部失败时 stderr 给出逐条汇总。
5. 工程配置文件多一个 `session.config_source: "runtime"`（只读，加载时忽略取值）。

## 5. 测试

### 5.1 单元测试

| 目标 | 用例数 | 结果 |
| --- | --- | --- |
| `TransportSniffPlan` / `TransportSniffRun` / `TransportSniffReport` | 11 + 6 + 4 = 21 | 21/21 通过 |
| `IncludeResolverTest` | 7 | 7/7 通过 |
| `TransportSelection.*` | 42（原 39，+3） | 42/42 通过 |
| `ProjectFile.*` | 29（原 27，+2） | 29/29 通过 |

`TransportSniff*` 覆盖：P1–P4 顺序、命令行覆盖 A2L、`ambiguous` 跳过、端口 0 跳过、无端点返回空、
`TcpOnly` 不含 UDP 条目、TCP 多候选展开、首胜即停（断言谓词只被调用 3 次）、三态保留、
退出码 0/3/4、谓词抛异常转未决且不外泄、空谓词转未决、`FailureReport` 行数 = 尝试数 + 1、
成功时报告为空、`DescribeSniffEntry` 与 `SniffTransportName` 的稳定文本。

### 5.2 CLI 冒烟（`tests/xcp_master_transport_inference_test.cmake`）

钉住三条参数层不变量：

1. 只给 `--a2l` 时输出里不得出现 `为必填项`，且必须走到"需要交互式终端"守卫；
2. `--transport tcp` 缺 profile 时必须出现 `未指定 --tcp-profile` 且说明等效 `auto`；
3. `--help` 必须能找到 `XCP_ON_TCP_IP` 与 `XCP_ON_UDP_IP`。

**为什么嗅探本身没法自动断言**：`main()` 在加载 A2L 之前就要求交互式终端
（`tools/xcp_master/main.cpp` 的 `IsInteractiveTerminal()` 守卫），管道重定向时嗅探根本不会执行。
端到端只能人工做（见 §6）。

### 5.3 回归

| 命令 | 结果 | 耗时 |
| --- | --- | --- |
| `ctest -R "XcpMaster|A2l|Transport"` | **344/344 通过** | 89.88 s |
| `ctest -E "Example"` | **1265/1265 通过** | 237.67 s |
| `ctest -R "Example"` | **19/19 通过** | 62.61 s |
| `ctest`（全量，含 Example） | **1284/1284 通过** | 300.72 s |

6 项未运行全部是既有 Disabled/Skipped（`TcpStreamDecoderFinish.HeaderOnlyIsTruncated`、
`Ag1_Cto8`、`SnapshotTest` 3 项、`RendersFrameWithFieldSizeCandidateList`）。

> **踩坑：Example 系列会因残留 slave 假失败。** 首次全量跑出现过 5 个 `Example*` 失败
> （`ExampleXcpMasterUdp` / `ExampleMeasurementDemo` / `ExampleCppDemoMeasurement` /
> `ExampleCppDemoCalibration` / `ExampleXcpIntegrationDemoCppDemo`），输出是
> `[env] 端口 5555 已被占用（疑似已有 XCP slave 在跑），请先停止`，而不是断言失败。
> 原因是上一次被中断的 ctest 留下一个 `xcp_slave_demo.exe` 仍占着 `UDP 127.0.0.1:5555`；
> 该进程自行退出后 `ctest -R "Example"` 与全量 `ctest` 均 100% 通过。
> 处置办法：跑之前 `netstat -ano | findstr :5555`，命中就结束对应 PID 再跑。
> 这不是代码回归，`Examples` 也不能并行跑（并行会互相抢 5555，见
> `code-plan/标定可写性_CALIBRATION_ACCESS参与判定_实施记录.md:114`）。

## 6. 人工验收步骤

1. `xcp_master --a2l apa_complex.a2l`（不写 `--host`/`--port`/`--transport`）
   → 应打印嗅探计划与逐条结果，最后打印 `嗅探结论：…`，随后进入界面并连上。
2. 同一命令加 `--transport tcp`（不给 `--tcp-profile`）→ 应打印等效 `auto` 的提示，只嗅 TCP 候选。
3. 拔掉 ECU 或改错端口再跑第 1 步 → 应对每条候选打印 `未决`，退出码 4，且 stderr 有汇总。
4. 在界面里 `save` → 检查 JSON：`connection.tcp_profile` 是具体布局（不是 `auto`）、
   `packet_alignment_bits` 有值、`protocol_version`/`transport_version` 非 null、
   `session.config_source` 为 `"runtime"`。
5. 用该文件 `-c` 重启 → 不应再出现"传输层嗅探"输出（配置被视为显式指定）。

## 7. 已知限制

1. 嗅探需要真的向 ECU 发起连接尝试，会短暂占用会话；对只允许单连接的 Slave 有侵入性，
   这也是保留 `--transport` 显式通道的原因。
2. `ambiguous` / 多实例的 A2L 端点一律不进嗅探计划，用户必须给 `--module` 或显式端点。
3. 嗅探顺序里 TCP 恒在 UDP 之前（除非 A2L 只声明 UDP），这是计划 §9.3 的定案，不按端口/延迟自适应。
4. CLI 冒烟无法覆盖嗅探执行路径（交互终端守卫在前），只能覆盖参数层。
5. `include_resolver.hpp` 不递归解析 include，与 `ScanA2lTcpCandidate()` 的单层读取语义一致；
   若将来扫描器改成递归，需要同时补深度与循环检测。

## 8. 复现命令

```powershell
# 构建
cmake -B cmake-build-release -S . -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release -j 4

# 单元测试
cmake-build-release\tests\XcpMasterSymbolPolicyTest.exe `
  --gtest_filter="TransportSniff*:IncludeResolverTest.*:TransportSelection.*:ProjectFile.*"

# CLI 冒烟（单独跑）
cmake -DXCP_MASTER="<abs>/cmake-build-release/tools/xcp_master/xcp_master.exe" `
  -P tests/xcp_master_transport_inference_test.cmake

# 回归（Windows 下必须先设可写 TMPDIR，否则既有用例会因 %TEMP% 权限失败）
set TMPDIR=<可写目录>
set TEMP=%TMPDIR%
set TMP=%TMPDIR%
# 先确认没有残留 slave 占着 5555，否则 Example 系列会假失败
netstat -ano | findstr :5555
ctest --test-dir cmake-build-release --output-on-failure
```

## 9. 过程中的三次 EOL 事故（教训）

本仓受版本管理的源码索引一律 **LF**，但工作区里部分文件是 CRLF。补丁脚本若"按探测到的行尾原样写回"，
会让 `git diff` 变成整文件重写（本次分别出现 537 行、795 行、893 行、1556 行的假差异）。

诊断与修复：

```bash
git ls-files --eol <file>            # i/lf w/crlf 即说明工作区行尾被改
git diff --ignore-cr-at-eol --stat   # 看真实改动量
```

规则：**判断行尾用 `raw.count('\r\n') == raw.count('\n')`**（`'\r\n' in raw` 会把"仅个别位置含 CR"
的 LF 文件误判成 CRLF）；每次改完必须用 `git diff --stat` 核对数量级。
