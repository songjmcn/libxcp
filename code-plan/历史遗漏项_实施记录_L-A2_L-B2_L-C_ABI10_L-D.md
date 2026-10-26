# 历史遗漏项实施记录：假 socket 注入、对齐声明冲突、ABI 10 多实例端点、格式门禁

- 日期：2026-10-25
- 分支：`update_tcp_udp_transport`
- 方案来源：`code-plan/历史遗漏项_详细方案.md`
- 用户裁决（m06307）：「1. 建议 A2(真注入) 2，L-C 的 ABI 10 做 3. 建议加」
  → L-A2、L-B2、L-C（ABI 10）、L-D 四项全部实施

---

## §0 结论速览

| 项 | 内容 | 状态 | 关键证据 |
|---|---|---|---|
| L-A2 | `SocketOps` 真注入通道 + 5 个确定性用例 | 完成 | `TcpFakeSocketTest` 5 passed |
| L-B2 | `PACKET_ALIGNMENT_DECLARED` 冲突行 | 完成 | `A2lIfDataConflictTest` 2 passed；`A2lGolden` 3 条旧契约未变 |
| L-C | ABI **10** 多实例端点（含生成器与 golden 变体） | 完成 | `A2lMultiEndpointTest` 3 passed；旧字段全为镜像 |
| L-D | 新增文件格式门禁脚本 | 完成（CI 未接线） | 8 个新文件通过；坏文件退出码 1 |

回归：`ctest -E "^Example" -j 4` → **1027/1027 通过**（17.52 s），2 项既有 Skip。
上轮基线 1017 → 本轮 +10（5 假 socket + 2 冲突 + 3 多端点）。

---

## §1 L-A2：`SocketOps` 真注入通道

### §1.1 为什么不是"再造一个假 socket"

原方案（L-A）打算新写 `tests/tcp_fake_socket.hpp` 里的独立假 socket 实现，但
`src/tcp_transport.cpp` 直接调用 `detail::RealSocketOps()`，测试根本无法把它换掉。
若只写一个"看起来能测"的假实现，测试永远只覆盖它自己。

因此改为在库内提供**受控注入点**：生产路径不变，测试可把 `SocketOps` 换成脚本化实现。

### §1.2 改动

`src/socket_util.hpp`（在 `RealSocketOps()` 之后）：

```cpp
[[nodiscard]] SocketOps& CurrentSocketOps() noexcept;
void SetSocketOpsForTesting(SocketOps* ops) noexcept;
```

- `CurrentSocketOps()`：有注入返回注入项，否则等价 `RealSocketOps()`。
- `SetSocketOpsForTesting(nullptr)` 恢复真实实现；用 `std::atomic<SocketOps*>`
  （acquire/release）保证无数据竞争，注入只影响**本进程**。

`src/socket_util.cpp`：匿名命名空间内 `std::atomic<SocketOps*> g_test_socket_ops{nullptr}`。

`src/tcp_transport.cpp`：7 处 `detail::RealSocketOps()` → `detail::CurrentSocketOps()`
（`CreateSocket`/`Connecting`/`SendAll`/`Receive`/`Shutdown` 等路径，行号 :134/:284/:376/:455/:518/:615/:700）。

### §1.3 新增测试

`tests/tcp_fake_socket.hpp`：

- `class FakeSocketOps final : public detail::SocketOps`：默认**全部转发**真实实现，
  只脚本化 `send`/`recv`/`wait_writable`。
- 动作枚举：`SendAction{PassThrough,TruncateTo,FailInterrupted,FailWouldBlock,FailConnectionLost}`、
  `ReceiveAction{PassThrough,FailInterrupted,FailConnectionLost}`。
- 观测：`SendCallCount()`、`PendingSendSteps()`、`ReceiveCallCount()`。
- `class ScopedFakeSocketOps`：RAII 安装/还原。

`tests/tcp_fake_socket_test.cpp`（5 用例，全绿）：

| 用例 | 覆盖行为 |
|---|---|
| `SendCompletesAfterPartialWrite` | 分片写：多次 send 后整体发出 |
| `SendRetriesAfterInterrupt` | `EINTR`/`WSAEINTR` 重试而非失败 |
| `SendFailsWhenWritableWaitTimesOut` | 等待可写超时 → 报「发送超时」 |
| `SendReportsFailureWhenWritableWaitErrors` | `wait_writable` 出错 → 报「等待 TCP 可写失败」 |
| `ReceiveReportsConnectionReset` | `recv` 连接重置 → 断开原因含「重置」 |

CMake：独立目标 `TcpFakeSocketTest`，`PROPERTIES RUN_SERIAL TRUE TIMEOUT 120`
（注入是进程级全局状态，不可与其它用例并行）。

### §1.4 实施期踩到的三个坑（已写进代码注释）

1. **枚举名**：错误类别是 `ErrorCategory::TransportError`，不是 `Transport`。
2. **连接路径也走 `wait_writable`**：非阻塞 connect + 等可写 + `get_so_error`。
   若在 `AcceptPeer()` 之前设 `SetWaitWritableResult(0)`，连接本身会被判超时。
3. **接收脚本必须在 `Open()` 之前安装**：否则接收线程已在真实 `recv` 上阻塞，
   对端 shutdown 会先以 EOF 收尾，得到的是「对端已关闭连接（EOF）」而不是「重置」。

---

## §2 L-B2：对齐"声明状态"差异

`thirdparty/a2l-sdk/src/liba2l_export.cpp` `DiffIfData()`：在 `PACKET_ALIGNMENT` 行之后，
**仅当两侧数值相同**且 `endpoint_alignment_declared` 不同时，追加一行：

```
PACKET_ALIGNMENT_DECLARED: 已声明 / 未声明
```

为什么不能直接复用 `PACKET_ALIGNMENT`：该行的语义是"两边取值不同"，
把"值相同但声明状态不同"塞进去会让下游无法区分两类问题。新增行保持了
"参数名即语义"的约定，也不需要 ABI 变更（`plus_conflicts` 是字符串三元组）。

新增 `tests/a2l_if_data_conflict_test.cpp`：

- `ReportsDeclaredStateMismatchWhenValuesMatch`：两侧都写 `PACKET_ALIGNMENT_8`
  但只有一侧显式声明 → 必须报 `PACKET_ALIGNMENT_DECLARED`。
- `KeepsDeclaredStateQuietWhenBothAgree`：两侧都显式声明且相同 → 不得出现该行。

既有 `a2l_golden_test.cpp` 的 3 条冲突契约（`MAX_CTO`/`PACKET_ALIGNMENT`/`SUB_CMDS`）未受影响。

---

## §3 L-C：ABI 10 多实例端点

### §3.1 为什么必须升 ABI

v9 的 `IfDataXcpDto` 只有一组摊平的 `endpoint_*` 标量字段，物理上只能表达一个端点；
解析层 `GetXcpOnTcpIps()`/`GetXcpOnUdpIps()` 返回的是**列表**，多实例信息在导出时被
`.front()` 丢弃。任何"只改桥接层"的做法都是猜（数据已经不存在了），
因此按用户裁决一次做到 ABI 10。

### §3.2 `thirdparty/a2l-sdk/include/liba2l/liba2l_api.hpp`

- `kLibA2lAbiVersion` 9 → **10**（附 v10 变更说明）。
- 新增 `struct IfDataXcpEndpointDto`：`XcpTransportDto transport`、`port`、`host_name`、
  `ipv4_address`、`ipv6_address`、`packet_alignment`、`alignment_declared`、
  `sub_commands`、`protocol_version`、`transport_version`。
- `IfDataXcpDto` 新增 `std::vector<IfDataXcpEndpointDto> endpoints;`
  旧的 `endpoint_*` 组标注为「自 v10 起 deprecated，= endpoints.front() 镜像」。

**每个端点自带 `transport` 类别是必要的**：同一 IF_DATA 块里 UDP 与 TCP 可以并存，
若按整块的 `transport` 字段猜类别，另一类端点会被整类丢掉（首个实现就踩了这个坑，
测试 `ExposesEveryDeclaredUdpEndpoint` 因此只拿到 TCP 端点）。

### §3.3 `liba2l_export.cpp`

- 泛型 `fill_endpoint(target, source, transport)`；`append_udp_endpoints()` / `append_tcp_endpoints()`。
- **顺序约定**：`transport` 选中的类别在前，另一类随后，同类内按 A2L 声明顺序。
  这样"只看 `endpoints.front()`"的老调用方（含旧 `endpoint_*` 镜像）行为不变。
- 旧 `udp_*` 字段取**首个 UDP** 端点（host 按 主机名 → IPv4 → IPv6 回退）。

### §3.4 `a2lbridge/src/a2l_dto_map.cpp`

- `fill_endpoint(TransportEndpoint&, const IfDataXcpEndpointDto&, std::uint32_t instance_count)`。
- `kind_of(XcpTransportDto) -> std::optional<TransportEndpoint::Kind>`：
  未知类别返回 `nullopt` 并**跳过**，不臆造。
- `instance_count` 用 `std::count_if` 按同类端点计数。
- `endpoints` 为空时走 v9 摊平字段的回退路径（兼容未升级的调用方）。

### §3.5 生成器与 golden 变体

`tests/a2l_gen/gen_a2l.py`：新增 `emit_endpoint(begin_tag, pl_version, endpoint, kind)`，
支持 `udp`/`tcp` 单实例与 `udp_endpoints`/`tcp_endpoints` 多实例；
TCP 复用 UDP 的 `PACKET_ALIGNMENT`/子命令白名单（`xcpontcpip.cpp` 的表已与 UDP 逐项一致）。

新增 `tests/a2l_gen/golden_spec_multi_endpoint.json`（2 TCP + 2 UDP），
由既有 golden 夹具统一生成，CI 覆盖生成器本身。

### §3.6 测试

`tests/a2l_multi_endpoint_test.cpp`（3 用例，全绿）：

- `ExposesEveryDeclaredTcpEndpoint`：2 个 TCP 端点必须都在 `endpoints` 里。
- `ExposesEveryDeclaredUdpEndpoint`：UDP 端点不得因为选中 TCP 而丢失。
- `FirstEndpointStaysUnambiguousForSingleTargetUse`：选中类别在前，`front()` 语义稳定
  （断言首端点为 `TcpIp`、总数 4）。

### §3.7 对 CLI 的影响（关键收益）

`tools/xcp_master/main.cpp` 的端点收集按 `kind` 过滤并逐实例计数，
此前桥接层每种传输最多给 1 个端点，导致"多实例 fail closed"是**永远不会触发的死代码**。
ABI 10 之后该保护才真正生效：A2L 声明 2 个不同 TCP 端点且未给 `--host/--port`
时会 fail closed，并提示显式指定。

---

## §4 L-D：新增文件格式门禁

### §4.1 为什么只查"新增文件"

本仓库既有代码与 `.clang-format` 存在大面积漂移（`clang-format` 22.1.0 对本仓库
报出数千处差异）。若做全仓门禁，一开启就红；若要求"修改过的文件也必须干净"，
真正的改动会被淹没在几千行重排里，评审无法进行。

因此门禁只覆盖**新增文件**：新代码从第一天起就是干净的，存量代码按需另行整理。

### §4.2 `scripts/check_new_file_format.py`

- 收集范围：`git diff --diff-filter=A <base>...HEAD` + 工作区未跟踪文件（`??`）。
- 排除：`thirdparty/`（外部项目）、`cmake-build*`、`build/`、`.tmp/`。
- 检查：`clang-format --dry-run --Werror --style=file:<repo>/.clang-format`。
- 选项：`--base`（默认 `origin/master`）、`--files`（只查指定文件）、
  `--require-version`（团队约定 22.x；不同主版本对同一配置可能给出不同结果）。
- 退出码：0 通过 / 1 有差异或工具/版本不满足 / 2 参数或 git 错误。

### §4.3 验证

- 8 个新文件已 `clang-format -i` 整理，门禁 **全部通过**（退出码 0）。
- 反向验证：故意写坏的 `.tmp/bad_format_probe.cpp` → 报
  `error: code should be clang-formatted`，退出码 **1**（直接取脚本退出码，未经管道）。
- 说明：`--files` 模式可用于 CI 里按需调用。

### §4.4 CI 未接线（需在 rebase 后进行）

本分支相对 `origin/master` **落后 20 个提交**，`.github/workflows/` 不在本分支树内，
而 `origin/master` 上的三个 workflow（`msbuild.yml`/`linux.yml`/`macos.yml`）
只在 master 的 push/PR 上触发。为避免与真实 CI 结构冲突，本轮**不新建 workflow 文件**。

rebase 之后建议新增独立 job（放 `linux.yml` 里减少重复配置）：

```yaml
  format-check:
    runs-on: ubuntu-22.04
    timeout-minutes: 10
    steps:
      - uses: actions/checkout@v4
        with:
          fetch-depth: 0          # 需要 history 才能做 base...HEAD 比较
      - name: 安装 clang-format 22
        run: sudo apt-get update && sudo apt-get install -y clang-format-22
      - name: 检查新增文件格式
        run: python3 scripts/check_new_file_format.py --base origin/${{ github.base_ref }} --require-version 22
```

---

## §5 测试数字

| 范围 | 结果 |
|---|---|
| `TcpFakeSocketTest` | 5/5 通过 |
| `A2lIfDataConflictTest` | 2/2 通过 |
| `A2lMultiEndpointTest` | 3/3 通过 |
| `A2lGolden`（既有 ABI 契约） | 通过 |
| `ctest -E "^Example" -j 4` | **1027/1027 通过**（17.52 s），2 项既有 Skip |
| Examples（串行，易抢端口） | 13/14，唯一失败 `ExampleXcpIntegrationDemoCppDemo` 为既有问题 |

2 项 Skip：`AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/2-byte`、
`XcpliteProtocolTest.ReadOutOfRangeAddressIsRejected`（环境相关，非本次改动引入）。

---

## §6 与方案的偏离

1. **L-A 由"新写假 socket"改为"库内注入点 + 脚本化假实现"**：原方案无法命中真实代码路径。
2. **L-C 一次做到 ABI 10 且同时改生成器**：方案已说明断点在 SDK；实施中额外补齐了
   生成器对多端点的支持，否则新能力没有可复现的输入。
3. **L-D 只覆盖新增文件**：方案倾向全仓，实测不可行（数千处存量漂移）；收窄后仍能
   防止新增代码继续恶化。
4. **L-D 未改 CI**：本分支落后 20 个提交、`.github/` 不在树内，强行新建 workflow 有冲突风险，
   改为文档给出接线片段。

---

## §7 未验证与未做

- **Windows 实机**：本批次全部在 macOS Release 验证；`SocketOps` 注入只在 macOS 跑过。
- **CI 格式门禁**：脚本可用但未接入 workflow（见 §4.4），需 rebase 后接线。
- **`project(... LANGUAGES CXX)` 的 C 源**：未涉及，无变化。
- **`examples/xcp_slave_demo_v12`**：未改动。

---

## §8 回滚

```bash
# 撤销本批次全部改动（按文件粒度，避免影响其它工作）
git checkout -- thirdparty/a2l-sdk/include/liba2l/liba2l_api.hpp \
               thirdparty/a2l-sdk/src/liba2l_export.cpp \
               thirdparty/a2l-sdk/a2lbridge/src/a2l_dto_map.cpp \
               src/socket_util.hpp src/socket_util.cpp src/tcp_transport.cpp \
               tests/CMakeLists.txt tests/a2l_gen/gen_a2l.py \
               tests/tcp_transport_test.cpp
rm -f tests/tcp_fake_socket.hpp tests/tcp_fake_socket_test.cpp \
      tests/a2l_if_data_conflict_test.cpp tests/a2l_multi_endpoint_test.cpp \
      tests/a2l_gen/golden_spec_multi_endpoint.json \
      scripts/check_new_file_format.py
```

注意：ABI 版本号 10 与 v10 注释块同在 `liba2l_api.hpp`，回滚该文件即一并撤销；