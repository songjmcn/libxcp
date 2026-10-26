# `xcp_slave_demo` 增加 TCP 传输支持 实施记录

- 日期：2026-10-26
- 触发问题：`xcp_master --transport tcp --tcp-profile auto` 对接 `examples/xcp_slave_demo` 时报"协议版本探测失败"
- 结论：**不是协商/版本探测缺陷**，而是 `xcp_slave_demo` 此前只实现了 UDP，TCP 端口无人监听

---

## §1 问题定性（先纠正前提）

用户提供的日志
`cmake-build-release/tools/xcp_master/logs/tools/xcp_master/xcp_master.INFO.20261026-091702.15915.glog`
全部 18 行，其中 4 条关键行完全一致：

```
E20261026 09:17:02.074698 tcp_transport.cpp:438] TCP connect rejected so_error=61
（随后 3 次同样内容，间隔约 0.1 ms 级）
```

`so_error=61` 在 macOS 是 `ECONNREFUSED`（Linux 同为 61；Windows 对应 `WSAECONNREFUSED=10061`）。
即：**4 个候选帧布局都在"建连"这一步就失败了**，一次 TCP 报文都没发出去，谈不上协议版本探测。

根因（改造前代码）：

```
examples/xcp_slave_demo/main.cpp:812  XcpEthServerInit(address, port, false, ...)   ← 硬编码 false = UDP
examples/xcp_slave_demo/main.cpp:679  A2lInit(..., false, ...)                      ← A2L 只声明 XCP_ON_UDP_IP
```

实测复现：启动 demo 后对同端口发起 TCP 连接 →

```
TCP 5661: [Errno 61] Connection refused
```

因此主站的 `--tcp-profile auto` 逻辑本身工作正常（按序试完 4 个候选才放弃），
问题在于**对端没有 TCP 服务**。

---

## §2 改动清单

### §2.1 `examples/xcp_slave_demo/main.cpp`

| 位置 | 改动 |
| --- | --- |
| 文件头注释 | "UDP Slave" → "UDP/TCP Slave" |
| `kDefaultPort` 注释 | 说明 TCP/UDP 端口空间独立、可用同一端口号 |
| `CommandLineOptions` | 新增 `bool use_tcp{false};`（附注：单例约束，一进程一种传输） |
| `ParseArguments` | 新增 `--transport <udp|tcp>`，只接受两个小写字面量，其它值报 `--transport 必须是 udp 或 tcp` |
| 新增 `TransportName(bool)` | 返回 `TCP`/`UDP` 字面量，供日志与提示复用 |
| `PrintUsage` | 增加 `--transport` 行，并说明"需要同时提供 UDP 与 TCP 时请起两个进程" |
| 生成模式 `A2lInit` | 第三参 `false` → `options.use_tcp`，A2L 据此写出 `XCP_ON_TCP_IP` / `XCP_ON_UDP_IP` |
| 生成模式自校验 | 新增"端点传输类别"校验：`transports.front().kind` 必须等于 `--transport` 对应的 `TcpIp`/`UdpIp`，否则生成失败 |
| 服务模式 `XcpEthServerInit` | 第三参 `false` → `options.use_tcp` |
| 启动日志/提示 | `从站已启动：UDP …` → `从站已启动：{TCP|UDP} …`；`XCPlite UDP Slave 正在监听` → `XCPlite {TCP|UDP} Slave 正在监听` |

为什么默认保持 UDP：现有 6 个集成用例与外部脚本都不传 `--transport`，默认值一变就是破坏性改动。

### §2.2 `tests/xcp_slave_demo_integration_test.cpp`

新增（全部为测试侧辅助，不改产品接口）：

- `class TcpPortReservation`：绑定 loopback 的 0 端口拿到空闲 TCP 端口，生成 A2L 后释放交给从站进程；与既有 `UdpPortReservation` 同构。
- `MakeDemoTcpMaster(port, listener)`：`TcpTransportConfig{role=Master, remote 127.0.0.1:port, packet_alignment=Bit8, wire_profile=AsamV14, connect_timeout_ms=1000}` + 300 ms 命令超时。
- `ConnectToDemoOverTcp(port, listener)`：8 秒内重试建连。
- `GenerateA2l(...)` 增加第 5 参 `std::string_view transport = "udp"`，透传 `--transport`。

新增用例 `GeneratedTcpA2lFeedsLiveSessionOverTcp`：

1. 以 `--transport tcp` 生成 A2L，断言文件含 `/begin XCP_ON_TCP_IP` 且**不含** `/begin XCP_ON_UDP_IP`；
2. 起 `xcp_slave_demo --transport tcp`；
3. TCP 建连后 `POLL` 连读两次 `meas_fast.output`（FLOAT64_IEEE，地址扩展 3 相对寻址）：断言两次都是 8 字节有限数、且数值变化（10 ms 事件驱动的波形必然变；若相等说明响应错位或读到缓存）；
4. 对 `meas_slow.statistics.fast_cycle_count` 配置动态 DAQ 并 `START_STOP_SYNCH`，用 XCPlite DTO 信封布局解码，断言出现非零 `uint32`。

第 4 步是本用例的关键：XCPlite 的 DTO 会把 4 字节补白计入 wire LEN，能解码即证明主站按 ODT 布局取前缀，而不是按 LEN 全量复制。

### §2.3 `examples/xcp_slave_demo/README.md`

- 首段改为"支持 UDP 与 TCP"，并加引用块说明单例约束（同时提供两种要起两个进程）。
- 生成/启动示例各给 UDP、TCP 两条命令。
- 参数表补 `--transport <udp|tcp>`，`--port` 说明改为通用端口。
- A2L 自校验说明补"传输类别必须一致，否则生成失败"。
- 增加主站侧连接示例（`--tcp-profile asam-v1.4` 或 `auto`）。
- 集成测试段落登记新用例。

---

## §3 设计取舍

1. **不实现"同进程同时 UDP+TCP"**：`thirdparty/XCPlite/src/xcpEthServer.c` 的
   `XcpEthServerInit` 在已有实例时打印 `"XCP server already running!"` 并返回 false，
   `gXcpServer` 是进程级单例、`use_tcp` 在初始化时定死。要在同进程双传输必须改 thirdparty，
   与"不改第三方库"的既有约束冲突；用户已确认走"命令行参数指定传输方式"。
2. **`--transport` 而非 `--tcp` 开关**：与 `xcp_slave_demo_v12`、`xcp_test_slave --tcp`
   之外的现有 CLI 习惯统一为"显式值"，也为将来新增传输（如 Sxi）留位置。
3. **A2L 端点类别纳入自校验**：改造前只比对 host/port。TCP 能力加上后，
   "生成了 UDP 的 A2L 却用 TCP 启动"会变成难以定位的问题（主站读到 UDP 端点后连不上），
   因此在生成阶段就 fail fast。
4. **主站侧默认 Profile 保持不变**：库继续要求调用方显式给 `wire_profile`
   （见 `include/libxcp/tcp_wire_profile.hpp`），测试与 README 里显式写 `AsamV14`。

---

## §4 测试结果

| 范围 | 结果 |
| --- | --- |
| `XcpSlaveDemoIntegrationTest.*` | **9/9 通过**（8 个既有 UDP 用例 + 新增 TCP 用例） |
| 新增用例 | `GeneratedTcpA2lFeedsLiveSessionOverTcp` 272 ms 通过 |
| `ctest -E "^Example" -j 4` | **1028/1028 通过**（较上轮 1027 增 1） |
| Examples（串行） | 13/14，唯一失败 `ExampleXcpIntegrationDemoCppDemo`（既有问题，与本改动无关） |

目录/manual 验证：

```
$ ./xcp_slave_demo --transport sctp
--transport 必须是 udp 或 tcp          （退出码 2）

$ ./xcp_slave_demo --generate-a2l --transport tcp --output-dir .tmp/demotcp ...
A2L 已生成并通过桥接解析：.../xcp_slave_demo.a2l   （退出码 0）
$ grep XCP_ON_ .tmp/demotcp/xcp_slave_demo.a2l
/begin XCP_ON_TCP_IP ... /end XCP_ON_TCP_IP

$ ./xcp_slave_demo --transport tcp --bind 127.0.0.1 --port 5671
XCPlite TCP Slave 正在监听 127.0.0.1:5671（快事件 10 ms，慢事件 100 ms）
→ 客户端 TCP connect 成功（改造前同端口为 Connection refused）
```

---

## §5 未验证项

- **Windows / Linux 实机**：本批次只在 macOS Release 构建与运行；新增代码只用标准 C++ 与
  既有跨平台 socket 辅助（`TcpPortReservation` 的 `SOCK_STREAM`/`IPPROTO_TCP` 两平台一致）。
- **`xcp_master` 交互界面端到端**：`xcp_master` 要求真实 TTY（`IsInteractiveTerminal()`），
  本环境 `pty.openpty()` 报 `out of pty devices`，无法构造伪终端；TCP 主站能力改由
  `XcpliteIntegration` 的真实 socket 用例覆盖。
- **`--tcp-profile auto` 对接该 demo**：协商逻辑已在库层验证（候选顺序/失败原因/耗时），
  与真实从站的组合验证见 `XcpliteTcpInteropTest`（V1.2 从站）与本用例（V2.2 从站显式 Profile）。

---

## §6 回滚

```bash
git checkout -- examples/xcp_slave_demo/main.cpp \
                examples/xcp_slave_demo/README.md \
                tests/xcp_slave_demo_integration_test.cpp
```

无 thirdparty 改动，无新增文件，回滚不涉及构建系统。