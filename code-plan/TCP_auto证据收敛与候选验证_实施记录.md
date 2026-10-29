# `--tcp-profile auto` 证据收敛与候选验证（方案 A′）实施记录

- 设计依据：`code-plan/TCP_auto证据收敛与候选验证_设计.md`
- 用户批准：方案 A′（"确认按照方案 A′ 做。"）；C3 处批准**破坏性**改造三态接口
- 实施环境：Windows 10.19045 + MSVC 2022 17.x，构建目录 `cmake-build-vs`
  （Visual Studio 17 2022 generator，`LIBXCP_BUILD_TESTS=ON`，Release）

## 1. 变更清单

### 1.1 新增公共头

| 文件 | 职责 |
| --- | --- |
| `include/libxcp/tcp_framing_verdict.hpp` | 三态 `FramingVerdict{Rejected,Inconclusive,Accepted}`、`FramingVerificationResult`、`FramingVerdictName()`；无重依赖，供协商层与验证层共用 |
| `include/libxcp/tcp_framing_candidates.hpp` | `TcpFramingCandidateRequest`（A2L 对齐 / 显式 TL 版本）+ `BuildTcpFramingCandidates()`：证据只决定**顺序**，不决定取舍；候选去重、上限 4 |
| `include/libxcp/tcp_framing_verification.hpp` | `TcpFramingProbeConfig`、`ClassifyFramingFailure()`、`VerifyTcpFramingCandidate()`：用库自身协议栈在**全新连接**上验证单个候选 |

### 1.2 改造

| 文件 | 变更 |
| --- | --- |
| `include/libxcp/tcp_framing_negotiation.hpp` | `FramingAttemptFn` 由 `bool` 改为返回 `FramingVerificationResult`；`FramingAttemptOutcome::accepted` → `verdict`；删除旧 `bool` 重载 |
| `src/tcp_framing_negotiation.cpp` | 首个 `Accepted` 胜出；`Inconclusive` 不终止扫描且逐项记录；非法候选不调用谓词；谓词异常包装为 `Rejected` 不外泄；`Summary()` 区分"全部被拒"与"存在未决" |
| `src/transport_selection.cpp` | 规则 8 不再无条件拒绝 `auto`：置 `wire_profile_auto=true`、`wire_profile=Unknown`、`alignment=Unknown`，并在 notes 写明"每个候选会真实 CONNECT/DISCONNECT 一次" |
| `tools/xcp_master/cli_selection.hpp` | 新增 `TcpNegotiationExitCode()` 与 `kAutoNegotiationOk/AllRejected/Inconclusive`（0/3/4），把退出码语义从 `main.cpp` 里抽成可单测函数 |
| `tools/xcp_master/main.cpp` | strict auto：`BuildTcpFramingCandidates` → `NegotiateTcpFraming(VerifyTcpFramingCandidate)` → 打印每次试连 → 胜出后**另建连接**开正式会话；失败时按 `TcpNegotiationExitCode()` 返回 3/4 并打印区分性文本；`--help` 增补 auto 语义与退出码 |
| `tools/xcp_master/README.md` | 重写 `auto` 段落（候选顺序、三态、退出码 0/3/4、副作用） |

### 1.3 新增/改造测试

| 文件 | 内容 |
| --- | --- |
| `tests/tcp_framing_candidates_test.cpp`（新） | 8 项：无证据回退默认序、仅版本不能推对齐、8/16/32 bit 证据只改顺序、去重 |
| `tests/tcp_framing_verification_test.cpp`（新） | 7 项：超时必须未决、连接前后 `TransportError` 的不同结论、协议违规被拒、`OutcomeUnknown` 未决、非法候选/非法配置不建连 |
| `tests/tcp_framing_auto_interop_test.cpp`（新） | 4 项跨进程：真实 V1.5 从站 8/16/32 三档都能收敛；证据只改顺序；错误证据仍能收敛；**无监听者时全部未决**（退出码 4 的文本依据） |
| `tests/tcp_framing_negotiation_test.cpp` | 改用三态谓词，新增"未决不终止扫描""摘要区分被拒/未决"等断言（9 项） |
| `tests/transport_selection_standalone_test.cpp` | 新增 `StrictAutoDefersLayoutToVerification`（只看公共头的独立调用者视角） |
| `tests/xcp_master_transport_selection_test.cpp` | 新增 `NegotiationExitCodeIsThreeWhenAllRejected` / `IsFourWhenAnyInconclusive` / `IsZeroWhenSelected` 三项，把退出码语义纳入门禁 |
| `tests/xcp_v12_interop_test.cpp` | 新增 `XcpliteV12AutoProfileTest.ResolvesAsamV14EightBitFromRealV12Slave`：auto 在真实 XCPlite V1.2 上选中 `{AsamV14,8bit}` 且只试连 1 次 |
| `tests/xcplite_tcp_interop_test.cpp` | 新增 auto 在真实 XCPlite V2.2 上选中 `{AsamV14,8bit}` |

## 2. 关键决策与理由

1. **验证入参用连接参数而非现成 `XcpMaster&`**（设计文档 C2 已同步标注该修正）。
   只有由被调方自己 `Open → Connect → QueryStatus×2 → Disconnect → Close`，才能
   把"一次验证 = 一条新连接、验证后连接必关闭"写成不变式；传入已打开的 master
   无法证明这两点。

2. **`Inconclusive` 不终止扫描**。若"超时即停"，把端口写错会伪装成"布局不支持"；
   继续试其余候选可在"某候选无回应、另一候选明确可用"时正确收敛，同时保留每项
   结论供 CLI 区分退出码。

3. **退出码 3 / 4 分离**。3 表示对端在回话但所有布局都被显式拒绝（该改配置）；
   4 表示至少一个候选没有任何协议层回应（该查地址/端口/从站）。二者混用会把
   "端口写错"误报成"四种布局都不支持"。

4. **正式会话必须重新连接**。试连已执行 CONNECT/DISCONNECT，复用会把"被验证过
   的状态"与"实际工作状态"混在一起。

5. **A2L 对齐证据只用于排序**。其可信度仍受既有受控快照 + 双源核对约束；本方案
   不把 A2L 声明升级为"已确认配置"——真正确认它的是线路上的三态验证。

## 3. 验证证据

### 3.1 全量回归

```
cd cmake-build-vs
TEMP/TMP 指向 cmake-build-vs/tests（必须是 Windows 反斜杠路径）
ctest -C Release -j 4
→ 100% tests passed, 0 tests failed out of 1157
  （1 项 Disabled、5 项 Skipped 为既有状态；总数由 1122 增至 1157，净增 35 项）
```

### 3.2 真实对端（跨进程，均为独立进程的真实 framing 实现）

| 对端 | 断言 | 结果 |
| --- | --- | --- |
| XCPlite V1.2 TCP | auto 选中 `AsamV14/8bit`，仅 1 次试连 | 通过 |
| XCPlite V2.2 TCP | auto 选中 `AsamV14/8bit`，仅 1 次试连 | 通过 |
| 本仓库 V1.5 从站（独立进程） | 8/16/32 三档均收敛到真实布局；错误证据仍能收敛；证据只改顺序 | 4/4 通过 |
| 端口已关闭 | 4 个候选全部 `Inconclusive`，摘要含"未决" | 通过 |

### 3.3 构建期遇到的坑（供复现参考）

- 在 Git Bash 里调用 MSBuild 前必须清掉重复的代理变量，否则报
  `error MSB6001: “CL.exe”的命令行开关无效 … no_proxy`：
  `unset no_proxy NO_PROXY http_proxy HTTP_PROXY https_proxy HTTPS_PROXY all_proxy ALL_PROXY`。
- `ctest` 的 `TEMP`/`TMP` 必须写成 Windows 反斜杠路径（`D:\...\cmake-build-vs\tests`）。
  写成 `D:/...` 时部分用例的 `create_directories` 会拿到 `Access is denied`，
  表现为 `SnapshotTest`、`A2lMultiEndpointTest` 等批量失败——这是环境问题，
  不是被测代码缺陷。

## 4. 未覆盖 / 限制（如实声明）

1. **Auto 的 POSIX 验证只到 Linux**。Linux（ubuntu-22.04 + GCC 13）已实跑通过；
   macOS 作业为红（失败用例与本方案无关，见 §5），因此 macOS 上"三态分类可用"
   尚不能记为已验证。
2. **CLI 端到端仍需交互终端**。`xcp_master` 的交互界面需要 TTY，自动化验收走
   "库级跨进程用例 + `TcpNegotiationExitCode()` 单测"两段证据，未直接断言进程
   退出码 3/4。
3. **V1.5 从站是本仓库夹具**，只能证明"我方验证逻辑可分辨三种对齐"，不构成与
   第三方 V1.5 实现的互操作证据。
4. **每次 auto 会真实改变从站会话状态**（每个候选一次 CONNECT/DISCONNECT）。
   这一点已写入 `--help`、README 与设计文档；对 ECU 有会话审计需求的场景应显式
   指定 `--tcp-profile` + `--packet-alignment`。
5. **A2L 正向信任仍是保守态**：ACL/重解析无法证明时 A2L 对齐不作配置；本方案
   只把它降级为"排序建议"，未放宽该限制。
## 5. 三平台 CI 实测（PR #17，提交 `0de843d`）

| 平台 | 工作流 | 结果 | 说明 |
| --- | --- | --- | --- |
| Windows | `MSBuild` | 通过 | `cmake-build-vs` 本地全量 1157/1157 与 CI 一致 |
| Linux | `Linux` (ubuntu-22.04, GCC 13) | 通过 | 方案 A′ 在 POSIX 上首次实跑成立，限制 §4.1 已消除 |
| macOS | `macOS` (macos-26, arm64) | **失败** | 失败用例 `XcpSlaveDemoIntegrationTest.CrossProcessCalibrationChangesDaqOutput`（exit code 8，Runner 标注 `878:`） |

### 5.1 macOS 失败的定性证据

- 同一用例在本仓库 `master` 上**已出现过相同失败**：
  run `40449345161`（提交 `b9f49481`）macOS 作业标注同为
  `XcpSlaveDemoIntegrationTest.CrossProcessCalibrationChangesDaqOutput`；
  而 run `40693262176`（提交 `f6554dfa`）macOS 通过。
- 本分支自身历史：`445bab86` macOS **通过** → `ffbb7c0`（只改断言措辞的测试提交）
  macOS 失败 → `0de843d` macOS 失败。
- 结论：这是**与本次改动无关的 macOS 侧不稳定用例**（UDP 跨进程标定+DAQ 的
  `WaitForDouble`/`WaitForNonzeroUint32` 3 秒等待），不是方案 A′ 引入的回归。
  但它是真实存在的红灯，不能因此把 macOS 记为已验证。

### 5.2 仍然成立的未验证项

- macOS 上 auto 的三态分类“跑过测试”这一结论**尚不成立**：macOS 作业虽在本提交
  跑到了自动协商用例之后才失败，但整条流水线为红，不能当作通过证据使用。
