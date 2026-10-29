# TCP A2L 与线路证据联合决策 —— 第二阶段实施记录

- 依据设计：`code-plan/TCP_A2L与线路证据联合决策_两阶段计划.md`（§5 提取范围、§5.3 验收门）
- 批准来源：用户逐节确认 M3 设计后于本轮明确"批准，开始实施"，并选择"直接删除旧头"而不是保留转发头
- 实施环境：Windows 11 + Visual Studio 2022（MSVC 19.44）+ CMake 4.0，构建目录 `cmake-build-vs`（`LIBXCP_BUILD_TESTS=ON`、`LIBXCP_BUILD_TOOLS=ON`）
- 结论先行：**提取完成，验收门全部满足**；两套 XCPlite 真实对端与 V1.5 三种 alignment 的真实跨进程用例全绿；唯一两次失败是 `XcpMasterRenderBenchTest` 的 UI 性能预算，已用逐行能力系数证明与本改动无关（见 §7.2）

---

## 1. 交付物

### 1.1 新的库内公共组件（`include/libxcp/`，随 `install(DIRECTORY include/libxcp ...)` 一起安装）

| 公共头 | 实现 | 职责 |
| --- | --- | --- |
| `include/libxcp/a2l_tcp_alignment.hpp`（103 行） | `src/a2l_tcp_alignment.cpp`（498 行） | 受限文法下从 A2L 原文扫描唯一 `XCP_ON_TCP_IP` 端点；与 SDK 端点事实做**双源核对** |
| `include/libxcp/transport_selection.hpp`（263 行） | `src/transport_selection.cpp`（387 行） | CLI 意图 + A2L 声明 + 既有连接状态 → 传输层选择结论（§8.1 规则）；含端点专属证据丢弃规则 |
| `include/libxcp/trusted_a2l_snapshot.hpp`（52 行） | `src/trusted_a2l_snapshot.cpp`（107 行，平台无关编排） | 把原始 A2L 复制进可信目录、独占创建、逐字节读回校验、守卫释放即删除 |

### 1.2 平台原生实现（按平台二选一，延续仓库既有 socket 目录约定）

| 文件 | 行数 | 说明 |
| --- | --- | --- |
| `src/detail/fs/trusted_snapshot_win.cpp` | 311 | 句柄式 ACL 校验（相对安全对象取 DACL）、祖先链、"能否被他人删除/替换"位宽、独占创建、只读提交 |
| `src/detail/fs/trusted_snapshot_posix.cpp` | 180 | `stat`/`fstat` 权限与属主校验、祖先链、`O_CREAT|O_EXCL` |

`src/trusted_a2l_snapshot.cpp` 只保留平台无关的编排（读源 → 定位目录 → 唯一文件名 → 汇总），通过 `src/detail/fs/trusted_snapshot_platform.hpp` 调用原生部分。

### 1.3 构建接线

- `CMakeLists.txt`：三个新源加入 `libxcp`；Windows 加 `advapi32`（ACL API）；平台原生实现按 `WIN32` 二选一。
- `tests/CMakeLists.txt`：新增 `TransportSelectionStandaloneTest`；`check_socket_platform_boundary.py` 白名单更新以接受新 `src/detail/fs/` 平台文件。

### 1.4 生产接入

`tools/xcp_master/main.cpp` 由"自带实现"改为"调用库组件"：包含改为 `libxcp/{a2l_tcp_alignment,transport_selection,trusted_a2l_snapshot}.hpp`，命名空间由 `calmcar::xcp::tools` 改为 `calmcar::xcp::`。对用户的可见行为不变：诊断文本、默认值、`--tcp-profile auto|auto-legacy`、`--packet-alignment`、`--a2l-trusted-snapshot-root` 语义全部保持。

### 1.5 删除的旧头（按要求不留转发头）

- `tools/xcp_master/transport_selection.hpp`（-773 行）
- `tools/xcp_master/a2l_tcp_alignment_candidate.hpp`
- `tools/xcp_master/a2l_trusted_snapshot.hpp`

---

## 2. 依赖边界与头自足性

验收不考虑"我本地能编过"，而是把边界做成编译期约束：

1. `TransportSelectionStandaloneTest` 的 `target_include_directories` **只给** `${PROJECT_SOURCE_DIR}/include`，**不给** `${PROJECT_SOURCE_DIR}`。任何公共头反向依赖 `tools/` 或 `src/` 的私有头，该目标直接编译失败。
2. 三个"单头 TU"在文件最前面只包含一个公共头（gtest 在其后）：
   - `tests/public_header_a2l_tcp_alignment_test.cpp`
   - `tests/public_header_transport_selection_test.cpp`
   - `tests/public_header_trusted_snapshot_test.cpp`

   头文件漏包含自身依赖（`<optional>`、`<filesystem>`、`<memory>` 等）时会立刻报错，而不是靠别的 TU 的包含顺序兜底。
3. `tests/transport_selection_standalone_test.cpp` 全文不出现任何 `tools/` 路径的 `#include`。

---

## 3. 独立调用者验收（9 例，全绿）

`TransportSelectionStandaloneTest` 共 9 例：

| 用例 | 钉住的行为 |
| --- | --- |
| `StrictAutoRejectsKnownAmbiguity` | 已知多端点歧义时严格 `auto` 拒绝建连并给出中文原因 |
| `AutoLegacyIsExplicitlyFlagged` | `auto-legacy` 必须回填"待试连"标志，不假装已知 Profile |
| `MismatchedEndpointDropsEvidence` | CLI 端点与 A2L 唯一端点不一致时丢弃端点专属证据（对齐/版本/提示） |
| `ScannerWorksWithoutToolSideHelpers` | 扫描器是"字节进、候选出"的纯函数，可被库外直接调用 |
| `ScannerRejectsInputWithoutTcpBlock` | 缺 TCP 块必须拒绝，不得返回"默认 8 bit"的假声明 |
| `CrossCheckClassifiesThreeOutcomes` | 双源核对的三种结论：可信 / 未声明 / 不可信 |
| `PublicHeaderA2lTcpAlignmentTest.DefaultConstructedCandidateIsEmpty` | 头自足 |
| `PublicHeaderTransportSelectionTest.DefaultRequestIsUdp` | 头自足 + 默认传输仍为 UDP |
| `PublicHeaderTrustedSnapshotTest.DefaultResultIsUntrusted` | 头自足 + 默认结果必须"不可信" |

---

## 4. 平台边界门禁

```
$ python tests/check_socket_platform_boundary.py .
平台边界通过：已扫描 33 个 src/ 非平台文件
```

新增的 `src/detail/fs/trusted_snapshot_{win,posix}.cpp` 按既有约定进入平台文件白名单，平台无关文件仍只有 `src/trusted_a2l_snapshot.cpp` 等 33 个。

---

## 5. 行为不变性

- 既有 `tests/xcp_master_transport_selection_test.cpp` 的断言未改，只把包含路径与命名空间换成新的库入口 → 迁移没有借机放宽任何断言。
- `tools/xcp_master/README.md` 已同步：TCP 需端点或 A2L 声明、严格 `auto` 的停止语义、`auto-legacy` 的风险、受控快照目录与"不可信则要求显式 `--packet-alignment`"的降级路径。

---

## 6. 真实对端（跨进程，本轮全绿）

| 用例 | 结果 | 说明 |
| --- | --- | --- |
| `XcpliteV12InteropTest`（UDP/TCP 各 2 例，共 5 例含夹具） | 5/5 通过 | 对端为 `examples/xcp_slave_demo_v12`（XCPliteV1.2，协议层 1.2 / 传输层 1.4），独立进程 |
| `XcpliteTcpInteropTest`（V2.2 对端，7 例） | 7/7 通过 | 对端为 `thirdparty/XCPlite`（协议层 V2.2），不与 V1.2 混链 |
| `Alignment8_16_32/XcpV15AlignmentInteropTest`（8/16/32 × 读/重连，6 例） | 6/6 通过 | 对端为独立进程的 V1.5 对齐夹具 |

上述用例的自动生成 A2L 在 TCP 路径上都会经过受控快照与双源核对；对端未声明对齐时结论为"不可信/未声明"，不会退化成默认 8 bit。

---

## 7. 全量回归

命令（Windows，清空代理变量以免 MSBuild 因环境变量大小写重复而失败）：

```bash
unset no_proxy NO_PROXY http_proxy HTTP_PROXY https_proxy HTTPS_PROXY all_proxy ALL_PROXY
export TEMP='D:\project\libxcp\cmake-build-vs\.tmp\snaptmp'
export TMP='D:\project\libxcp\cmake-build-vs\.tmp\snaptmp'
cmake --build cmake-build-vs --config Release
ctest --test-dir cmake-build-vs -C Release -j 4
```

### 7.1 结果

- 全量构建：成功（含 `libxcp_tests`、`xcp_master`、示例）。
- 注册用例：1132；实际运行 1131；**1129 通过 / 2 失败**；5 项未运行（1 项既存 `Disabled`、1 项参数化跳过、3 项快照成功路径跳过）。

### 7.2 两项失败的环境证据（与本改动无因果关系）

| 用例 | 实测 | 预算 | 判定 |
| --- | --- | --- | --- |
| `XcpMasterRenderBenchTest.RendersFrameWithFieldSizeCandidateList` | 单帧渲染 1537.4 ms，每行 11.14 µs | 硬上限 1500 ms，参考每行 0.86 µs | 本机逐行能力系数 **13.01**（参考机器约快 13 倍） |
| `XcpMasterRenderBenchTest.SearchesAndFiltersLargeA2lWithinBudget` | 全库检索 329–499 ms（三次 371.6 / 329.0 / 499.5） | 参考 90 ms × 3 = 270 ms，硬上限 1500 ms | 硬上限全部通过；仅"3 倍参考"这一条越界 |

两条都只测 FTXUI 渲染与 `database->Search("*")` 的吞吐，路径上没有任何本次改动的代码；`git diff` 对 `src/` 的 A2L/UI 相关文件零改动。同一用例在并行 `-j 4` 下与单独运行时都越界，说明是机器吞吐（沙箱虚拟机）而非并行争用。

> 记录不作"性能没问题"的结论：本机无法证明该预算；能证明的是该预算失败不来自本次提取。

### 7.3 三项快照成功路径跳过的具体原因

用例现在会把被测函数给出的拒绝点打印出来，不再只写"本机没有私有目录"：

```
本机没有任何仅本人可写的目录，快照成功路径不可验证；被测函数在探测目录上的拒绝点：
...\snaptmp\libxcp_snapshot_base → 目录的写权限授予了非可信主体
S-1-4-451665580-940089810：...\libxcp_snapshot_base\probe
```

原因是本机目录链（`D:\project\...` 及快照目录）对 `BUILTIN\Users` / 登录会话 SID 授予写权限，而快照组件**必须**据此拒绝——这正是它的设计行为。把成功路径"造"成通过只能靠改写机器级 ACL，测试不做这种事。

---

## 8. 遗留限制（不得据此宣称的能力）

1. **POSIX 分支未实测**：`trusted_snapshot_posix.cpp` 只做了编译期接线与代码审阅，Linux/macOS 上的权限/`O_EXCL`/祖先链行为未经运行验证。
2. **A2L 正向信任在本机不可验证**：受 §7.3 的 ACL 环境限制，`asam-v1.5` 走 TCP 时必须显式 `--packet-alignment`；这不是缺陷登记，而是设计上的失败关闭。
3. **`auto` 仍未实现真实探针**：严格 `auto` 在歧义时停止；`auto-legacy` 仍是"试连取首个成功"的风险路径，成功 CONNECT 不能唯一确定线格式。
4. **性能预算与参考机器不匹配**（§7.2）：需在参考硬件或 CI 专用机上复测，才能对这两个预算给出结论。

---

## 9. 验收门对照（§5.3）

| 门 | 状态 | 证据 |
| --- | --- | --- |
| 组件可被库外调用方使用（无 `tools/` 依赖） | 通过 | `TransportSelectionStandaloneTest` 只给 `include/`；9/9 通过（§3） |
| 公共头自足 | 通过 | 三个单头 TU 编译通过（§2） |
| 平台实现不污染平台无关层 | 通过 | `check_socket_platform_boundary.py` 通过（§4） |
| 行为不变 | 通过 | 既有断言未改；真实对端 18 例全绿（§6） |
| 全量回归 | 通过（附环境说明） | 1129/1131，2 项 UI 性能预算受机器限制（§7） |
| 文档同步 | 通过 | `tools/xcp_master/README.md`、本记录 |

---

## 10. 后续建议（需另立批准）

1. 在 Linux/macOS CI 上跑 `SnapshotTest` 与 `check_socket_platform_boundary.py`，补齐 POSIX 证据。
2. 给 `auto` 增加"只读探测"（例如在不影响会话的前提下比对 CONNECT 响应字节），降低 `auto-legacy` 的使用面。
3. 在生产机复核 UI 性能预算的参考值，或把该用例改为"仅记录不设门"。