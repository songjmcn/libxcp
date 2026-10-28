# XCP on Ethernet V1.5 非 IPv6 缺口可行性评估（兼容 XCPlite V1.2）

> 状态：**只读源码/规范审查形成的方案评估，不是实施批准或互操作验收**。本文件不修改程序或第三方库；工作量仅作相对估计。依据为 [`docs/ASAM-AE-MCD-1-XCP-AS-Ethernet-Transport-Layer-V1-5-0.md`](../docs/ASAM-AE-MCD-1-XCP-AS-Ethernet-Transport-Layer-V1-5-0.md) 和当前 checkout 的代码。**IPv6 不在本次评估范围**。

## 1. 范围、结论与重要纠偏

评估对象是现有 **libxcp Master + TCP/UDP IPv4** 对传输层规范的剩余能力，而不是把协议层 PL V1.5 全部新命令、生产级 Slave 或第三方实现改造混作同一项。§4.2 的 block/interleaved 是可选通信模型，独立于 Ethernet framing；不因为未实现就认定基础 TCP/UDP 不合规。规范 §5 的四个专用 TL 子命令也是 **optional**，未实现的 Slave 甚至可以不返回负响应（[规范 §4.2](../docs/ASAM-AE-MCD-1-XCP-AS-Ethernet-Transport-Layer-V1-5-0.md#L151-L157)、[§5](../docs/ASAM-AE-MCD-1-XCP-AS-Ethernet-Transport-Layer-V1-5-0.md#L234-L246)）。

**总体判断：可以分层补齐，但不能把“对 V1.5 一律加 Tail/多播”当成升级策略。** 必须维持已经存在的 V1.4 兼容 wire profile 和原有 IPv4 单播路径；标准 V1.5 与 XCPlite V1.2 应分别验收，不能由同一自有 loopback 或 V2.2 对手端外推到 V1.2。当前可行性最高的是显式对齐的 UDP 多帧接收和预算/诊断集成；多播发现、IP 设置与 TCP RESUME 属独立需求驱动的高成本能力，不应作为基础 profile 的强依赖。

- `thirdparty/XCPliteV1.2/src/xcp_cfg.h:28-30` 的 **Protocol Layer = 0x0102**，`thirdparty/XCPliteV1.2/src/xcptl_cfg.h:23-24` 的 **Transport Layer = 0x0104**；版本不能混为“第三方实现 V1.5”。默认 `thirdparty/XCPliteV1.2/src/xcptl_cfg.h:52-64` 不启用多播。
- 表中 §5 的 `0xFF/0xFD/0xFC/0xFA` 是 **TRANSPORT_LAYER_CMD (`0xF2`) 内的子码**，不可当作独立 XCP 命令码发送（尤其 `0xFF` 本身是 CONNECT）。XCPlite V1.2 的 `thirdparty/XCPliteV1.2/src/xcpLite.c:3168-3244` 将**整个** `CC_TRANSPORT_LAYER_CMD` 处理分支排除在 PL ≥ 0x0103 的条件之外；其内部 `GET_SERVER_ID` 静默分支不能作为默认构建在已连接状态下的行为证据。已连接时最终走未知命令处理，未连接时仍可能静默。不能以此版本证明四个 TL 命令真实可用。
- `tests/xcplite_tcp_interop_test.cpp` 使用的 `xcp_test_slave` 在 `tests/xcp_test_slave/CMakeLists.txt:8-15` 链接 `thirdparty/XCPlite`，**不是** `thirdparty/XCPliteV1.2`；`tests/CMakeLists.txt:544-560` 的 V1.2 测试只编译示例模型、不链接 V1.2 协议栈及 Master。因此当前自动测试不能证明真实 V1.2 TCP/UDP 端到端兼容；对其源码级评估均标为“待跨进程验证”。
- 旧 [`code-plan/V1.5规范实现与XCPlite兼容性评估.md`](V1.5规范实现与XCPlite兼容性评估.md#L41-L48) 建议按 0.5 秒猜测 TCP Tail，结论不可靠；后续 [`code-plan/TCP传输_实施计划_B-T1至B-T5.md`](TCP传输_实施计划_B-T1至B-T5.md#L700-L702) 已否定该方案。本评估明确**不按 FILL 值或超时猜流边界**。

## 2. 分项可行性与兼容约束

等级是相对实施难度，不是工期；“缺口”包括有组件实现但尚未进入生产数据流的情况。凡涉及 API 变更、测试和编译均需另行设计批准。

| 缺口 / 规范依据 | 现状和最小可行路径 | 对 XCPlite V1.2 的约束 | 难度 / 优先级 |
|---|---|---|---|
| **UDP 拼帧与 8/16/32-bit 对齐**，§4.3.1、§4.3.2.2 | `src/ethernet_header_codec.cpp:164-229` 已有按显式 alignment 消费每帧 LEN 外 Tail 的纯解码（当前对**末帧**也要求预期 Tail，需补规范允许的省略尾部情形）；生产接收 `src/udp_transport.cpp:674-703` 使用 boundary decoder，而 `src/ethernet_header_codec.cpp:134-155` 仅把**末帧** ≤3B 剩余视为 Tail，非零中间 Tail 不能泛化识别。发端 `src/udp_transport.cpp:397-463` 只发送单帧且省略 Tail。建议先引入**由可信 A2L/显式配置选定**的严格对齐接收模式，维持既有 boundary 模式默认路径；在完整数据报内原子验证 LEN、对齐、上限和 CTR。聚合发送另立阶段，只在对端明确支持时开启。 | `thirdparty/XCPliteV1.2/src/xcpQueue64.c:377-426,705-769` 的队列 DTO 把补齐字节**计入 LEN**后聚包，顺序依 LEN 可解析；同步 CRM 不补。`thirdparty/XCPliteV1.2/src/xcpEthTl.c:369-386` 的 UDP CRO 接收只允许单帧，因此不能向它发送多帧 CRO，也不能以其聚包推断它理解 LEN **外** FILL。不能从代码固定 4B 队列对齐推断其 A2L 已声明 PACKET_ALIGNMENT_32。 | 中 / **P1 接收**；发送低优先级 |
| **TCP 可选尾部的 V1.5 语义**，§4.3.2.1/2 | `src/tcp_transport.cpp:588-599` 显式 V1.4 不补 Tail，V1.5 补 Tail；`src/tcp_stream_decoder.cpp:62-71,181-217` 的 V1.5 接收按 alignment 消费中间 Tail，并允许 EOF 时末帧无 Tail。**活连接中“无尾后立刻另一帧”没有独立边界标志**，不能仅凭时间/字节值无歧义地区分下一帧头与 Tail。建议保留双 wire profile；针对标准端做有尾、EOF 无尾和持续连接的明确契约/互操作验证，若规范单帧省尾与连续流存在歧义，记录互操作限制而非冒险自动识别。 | `thirdparty/XCPliteV1.2/src/xcpEthTl.c:331-338,187-195` 接收 4B+LEN、不吞 LEN 外 Tail，发 CRM 也不补；其聚合 DTO LEN 内补齐。V1.2 必须显式走现有 `AsamV14`，不能对它强推 V1.5 TX Tail。 | 中～高 / **P1 保持隔离；P2 独立标准验证** |
| **CTR 与发送错误边界**，§4.3.1.2 | 双方向 CTR 各自独立；`src/udp_transport.cpp:417-459` 在实际 `sendto` 前预增，`src/tcp_transport.cpp:570-585` 甚至在第二个发送上限校验前预增。建议在完成全部本地校验后再分配 CTR；对网络失败/部分 TCP 写明确记录不可重放与连接失步策略，绝不盲目回退已发送部分帧的 CTR。 | V1.2 同一个 slave 计数器为 CRM/DTO 分配 CTR，队列丢帧会导致缺口，重连不保证从 0 开始（`thirdparty/XCPliteV1.2/src/xcpEthTl.c:63-64,185-197,648-651`）；保留接收端宽容前向缺口和 16-bit 回绕，不把 V1.2 的收端不校验 master CTR 当成主站乱序发送许可。 | 小～中 / **P1 稳健性** |
| **§5.1/§5.2 多播发现**，GET_SLAVE_ID `0xFF` / EXTENDED `0xFD` | 需要与单播会话隔离的 UDP 多播 socket、接口选择、加入/退出组、`239.255.0.0:5556`、CTR=0、应答监听/关联、超时和多从站聚合；尤其 EXTENDED 响应布局须按规范各自解析。当前 `src/xcp_master.cpp:331-383` 明确拒绝 OptionalTlSubcommand。 | 默认 V1.2 无多播、其 PL 1.2 排除 TL handler；发现失败或无应答不得妨碍显式 IP/port 的 TCP/UDP CONNECT。**不是 V1.2 兼容性必需项**。 | 高 / **P3 需求驱动** |
| **§5.3 SET_SLAVE_IP_ADDRESS `0xFC`** | 更改对端网络配置涉及授权、防误配、可达性确认、失败回滚和多播响应；不可作为普通已连接 CRO 无保护下发，建议独立管理能力，默认禁用并要求用户明确目标设备和恢复方案。 | V1.2 默认不可用；不得将“可设置”作为互操作验收条件，也不修改第三方来实现。 | 高 + 运维风险 / **P4** |
| **§5.4 GET_DAQ_CLOCK_MULTICAST `0xFA`** | 依赖基础协议时间同步能力与 cluster affiliation，需 multicast 监听、同步事件解码和时间戳精度验证；规范 `docs/ASAM-AE-MCD-1-XCP-AS-Ethernet-Transport-Layer-V1-5-0.md:477-502` 指明派生组播地址。建议先确认多 ECU 时钟同步业务确需此模式，不能由已有单播 GET_DAQ_CLOCK 推断。 | V1.2 默认 PL 1.2 且宏关闭，`thirdparty/XCPliteV1.2/src/xcp_cfg.h:388-390` 与 `thirdparty/XCPliteV1.2/src/xcptl_cfg.h:52-59` 已说明此路径未启用。单一 V1.2 Slave 联调不应强求组播钟。 | 高 / **P3** |
| **§7 带宽预算与 A2L/DAQ 接线** | **不是完全未实现**：`src/link_budget.cpp:15-145`、`include/libxcp/link_budget.hpp:52-83,104-116,197-200` 已有纯预算函数和 Unverified/OverBudget 规则；但是 `src/xcp_master.cpp` / `src/measurement/measurement_session.cpp:216-233,268-300` 的 DAQ 启动路径尚无生产调用、`include/libxcp/link_budget.hpp:12-16` 明确暂不支持 A2L 自动提取；`adapter/a2l/measurement_runtime_profile.hpp:44-53` 的现有事件元数据不足以推断所有 MIN_CYCLE_TIME。§7 的预算只约束 DAQ/STIM 的净 XCP 字节，不是网络/IP 开销或命令响应（规范 §7.1 `docs/ASAM-AE-MCD-1-XCP-AS-Ethernet-Transport-Layer-V1-5-0.md:536-605`）。先在调用方显式提供标称/实际 bit/s、MAX_BUS_LOAD、Event 周期与每 ODT 真实净字节数；在启动前报告 verdict，必要时才加可配置门禁。A2L 提取须先核验 SDK 公共 API、字段缺失、单位和运行时链路速率来源。 | V1.2 A2L 模板 `thirdparty/XCPliteV1.2/src/a2l.c:153-259,638` 未提供上述预算参数/显式 PACKET_ALIGNMENT 声明时应为 **Unverified**、保持默认可用；不能从队列内部 4B 对齐推定 A2L 配置或强制 V1.5 尾部。V1.2 实际 DTO 净长度应以 LEN（含其内部补齐）入预算，避免低估。 | 中（显式接线）/高（可信 A2L 自动化）/ **P1～P2** |
| **TCP 断链/RESUME/Slave 生命周期**，§4.1.1 | `src/tcp_transport.cpp` 已实现主动建连和显式 close，但“永久尝试建连、从站缓存 DAQ”的 RESUME 全流程不是单纯自动重连：需独立持久化策略、资源上限和丢数语义；生产级 Slave 的单连接/FIN 停 DAQ 则属于 Slave 端工程，不应算作 Master 已实现度缺陷。建议把普通断线重连与 RESUME 分成独立需求，未确认设备能力不得暗中启动重连风暴。 | `thirdparty/XCPliteV1.2/src/xcpEthTl.c:319-365` TCP 断链调用 XcpDisconnect；不要假定有离线 DTO 缓冲。主站应用应清理会话并报告采集断点。 | 高 / **P3** |
| **UDP Slave 源地址/端口契约**，§4.1.2 | 规范要求从站仅接受 CONNECT 来源 IP 的后续命令，但后续端口可以变化（`docs/ASAM-AE-MCD-1-XCP-AS-Ethernet-Transport-Layer-V1-5-0.md:145-147`）。现有 `src/udp_transport.cpp:650-663` 是 Master **入站源过滤**，用途不同，不能把它误判为未实现的 Slave CONNECT 地址绑定；如将来实现生产级 Slave，再单独设计并验证该契约。 | 不改变现有 Master 对 V1.2 远端过滤策略，不把 V1.2 测试示例扩展为“符合规范的生产级 Slave”。 | 独立项目 / **非当前 Master 实施项** |

附带稳健性缺陷：`src/udp_transport.cpp:655-670,679-701,705-725` 等多处 `OnTransportWarning()` 不捕获用户回调异常，而 `OnPacketReceived()` 已捕获；应在独立修复批次审查接收线程异常边界，保证回调抛错不导致线程终止/进程 `std::terminate`。此项不是 XCP wire 特性，却直接影响以上 malformed/CTR 诊断路径，建议 **P1**。

## 3. XCPlite V1.2 专项验收矩阵（建议，不代表已运行）

| 维度 | 最低新增验证 | 必须明确的成功/失败界线 |
|---|---|---|
| 真实进程 | 单独启动 `examples/xcp_slave_demo_v12`，由 Master 通过真实 IPv4 TCP **和** UDP 访问；不得把 `tests/xcp_test_slave` 的 V2.2 结果改称 V1.2；两个 XCPlite 版本不要混链同一可执行文件。 | 连接、POLL/标定、DAQ 多 ODT、断开再连、超时/能力降级；失败保留双端日志和抓包。现状仅有 V1.2 模型测试，故本行 **待验证**。 |
| UDP 对齐 | V1.2 双 ODT 同 datagram，检验 **LEN 内补白**；另用独立合成对端构造标准 16/32-bit **LEN 外中间 Tail**、非零 FILL、末帧有/无 Tail、跨报文截断与包上限。 | V1.2 仍走兼容路径且不得向其发多帧 CRO；显式对齐路径能正确解析标准聚包；畸形报文原子拒绝。 |
| TCP profile | V1.2 `AsamV14` 连续无尾帧、CRM/DTO 交错与分片；独立标准 V1.5 对端测有尾/无尾末帧、非零 FILL、FIN、分片、错 profile。 | 不允许自动 sniff 或“等 0.5 秒吞尾”；可判定的布局正确解析，不可判定的组合明确限制/报错。 |
| CTR 与错误 | 双向独立、CRM/DTO 共用 slave 序列、非零初值、队列 gap、`0xFFFF→0x0000`、本地校验失败不消耗 CTR、TCP 部分写失败后连接状态。 | 缺口有诊断、不错误重放命令，不因 V1.2 gap 误断；本地拒绝无无谓 CTR 跳号。 |
| 可选 TL / 带宽 | 已连接 ERR_CMD_UNKNOWN、未连接无响应、缺失 A2L 预算字段、显式超预算、未知周期、真实网卡速率低于 A2L 标称。 | 无 TL 不妨碍单播；Unknown/Unverified 不谎报 Verified；策略为 Reject 或 OverBudget 时才按配置拒启 DAQ。 |
| 回调/跨平台 | V1.2 应用 `A2lInit` 连接回调拒绝或延迟就绪、CONNECT 静默后恢复；UDP warning 回调抛异常；Windows/Linux/macOS 的 socket 行为。 | 静默判为应用/连接超时而非“已连接”；异常不终止进程。跨平台 **待验证**，不得用 Windows 结果外推。 |

`thirdparty/XCPliteV1.2/src/xcpAppl.c:40,55,74-82` 使用单个连接回调；`thirdparty/XCPliteV1.2/src/xcpLite.c:2453-2458` 对回调拒绝不组 CONNECT CRM，`thirdparty/XCPliteV1.2/src/a2l.c:1564-1591,1684-1720` 可注册该回调且涉及 A2L finalize。自动门禁须保留应用就绪流程，**不得覆盖或误判该唯一回调**。

## 4. 建议分期与决策门槛

1. **P0：建立真实 V1.2 基线**。先用独立进程补 TCP/UDP 端到端观测，验证双 ODT/LEN、连接回调、版本及回归；未取得结果之前“兼容 V1.2”只是源码支持推断。此阶段不改 `thirdparty/XCPliteV1.2`。
2. **P1：互操作不回退的局部增强**。在明确规格/对齐依据后接入 UDP 多帧严格接收；完成发送前 CTR 参数校验和回调异常隔离；预算采用可选显式配置先报告、再以用户选择的策略决定门禁。现有 V1.4 TCP、无尾单帧 UDP 必须仍通过。
3. **P2：标准 V1.5 端点专项**。获取独立符合 V1.5 的 TCP/UDP 对端，覆盖非零中间 FILL、尾部可选语义及 A2L 端点声明；若需要 A2L 自动预算，先确认字段公共 API/缺失与速率验证途径，禁止静默猜默认值。
4. **P3/P4：只有明确业务需求才开展组播发现/时钟、RESUME 和 IP 地址设置**。这些 optional 能力必须与基本单播隔离；不应以 V1.2 默认不支持来要求修改第三方实现。

**发布门槛**：每个能力分别标注“规范组件测试通过 / 真实 V1.2 跨进程通过 / 独立 V1.5 对端通过 / Windows、Linux、macOS 通过”，无证据者标 `Unverified`；不要把可选项加入基础兼容的强制百分比。实施前按仓库 `AGENTS.md:11-19,95-101` 逐项澄清设计与测试并获批准，随后在 `tests/` 增回归测试、Release 构建/运行相应测试、在 `code-plan/` 留实施和测试记录；**本文件没有实施、编译或运行新测试**。
