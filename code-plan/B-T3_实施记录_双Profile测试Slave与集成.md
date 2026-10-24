# B-T3 实施记录：双 Profile Test Slave 与 XcpMaster 集成

> 计划依据：`code-plan/TCP传输_实施计划_B-T1至B-T5.md` 第 6 章（§6.1 Test Slave、
> §6.2 集成场景、§6.3 Gate）。
> 用户裁决：`m01093` 选定 **S1** —— 两个 Profile 各自独立实现 framing，不共用生产 decoder。

---

## 1. 交付物

| 文件 | 说明 |
|---|---|
| `tests/tcp_test_slave.hpp` | `TcpTestSlave` 声明 + `TcpFaultInjection` 故障注入配置 |
| `tests/tcp_test_slave.cpp` | 实现（Socket、双 Profile framing、命令分发、故障注入） |
| `tests/xcp_tcp_loopback_test.cpp` | Loopback 集成测试（双 Profile 参数化 + TCP 特有场景） |
| `tests/CMakeLists.txt` | 加入上述两个 `.cpp`（:67、:68） |

---

## 2. 关键设计决定

### 2.1 为什么两套 Profile 各自独立实现 framing（S1）

计划 §6.1 原话："两种模式使用独立测试侧 framing；关键 golden 不全部复用生产
decoder"。若 Slave 直接调用 `TcpStreamDecoder`/`EncodeEthernetFrameAligned`，
那么"Master 与 Slave 互通"只能证明本库**自洽**——同一个误解在收发两侧对称出现
时测试照样绿。因此 `TcpTestSlave::EncodeFrame` 与 `ReadFrame` 均为独立实现：

- `AsamV14`：`[LEN=真实Packet长][CTR][Packet[LEN]]`，**绝不读写 LEN 外 Tail**；
- `AsamV15`：上者基础上，按 `TailBytesForAlignment` 在 LEN **之外**追加 FILL。

### 2.2 AsamV14 语义的源级取证（更正先前的错误结论）

**先前在 B-T2 记录中写下的结论是错的**："XCPlite 用 DLC 语义、把对齐填充计入
LEN，导致 V15 Bit32 下字段重叠"。读全 `thirdparty/XCPliteV1.2/src/xcpEthTl.c`
的收发热路径后实测事实为：

- 发送侧 `:189` `p.dlc = size;` —— `size` 就是 **XCP Packet 语义长度**，
  与 ASAM TL V1.5 的 LEN 定义一致；
- 接收侧 `:332-335` 先读 4 字节头（`XCPTL_TRANSPORT_LAYER_HEADER_SIZE 4`，
  `xcptl_cfg.h:50`），再按 `msgBuf.dlc` **精确**读取，**不做**对齐跳过；
- `XCPTL_PACKET_ALIGNMENT 4`（`xcptl_cfg.h:46`）仅出现在宏定义与注释中，
  TCP 收发热路径**从未引用**它。

⇒ XCPlite 的 TCP framing **等同于 `AsamV14`**，**不存在字段重叠**。

**教训**：先前只读了 `xcptl_cfg.h` 的宏定义就外推装配语义，未读实际收发代码。
这与 §4 表格中"凭记忆编码"属同一类错误——**结论必须有代码级证据**。
B-T2 实施记录中的对应条目已同步更正。

### 2.3 验证性质的分层（计划 §6.3 明确要求）

| 层次 | 可声称的结论 |
|---|---|
| AsamV14 用例 | framing 逐字节复刻 XCPlite 收发路径，**可**声称与 XCPlite 语义一致 |
| AsamV15 用例 | LEN 外 Tail **无第三方实现可对照**，属**自有闭环验证**，**不得**宣称"已与独立实现互操作" |

### 2.4 持续 accept 而非常规"accept 一次"

计划 §6.1 要求"第二连接需 accept 后显式拒绝/关闭"。生产 `TcpTransport`
（`src/tcp_transport.cpp:435-455`）accept 一次后即停止监听；本 Test Slave
必须**持续** accept，否则第二条连接会滞留在内核 backlog 中——客户端
`connect()` 因内核完成三次握手而"看似成功"，却永远收不到响应，表现为
**静默挂起**。实现方式：监听 Socket 置非阻塞 + 每轮循环 `select` 同时观察
"新连接待 accept"与"活动连接可读"。

### 2.5 Dynamic DAQ 验收的删除

计划 §6.1 写明"…和 DTO 推送；**否则删除 Dynamic DAQ 验收**"。本批次据此
**显式删除**，而非留下未实现命令返回假成功。落实为两点：

1. `TcpTestSlave` 不实现任何 DAQ 命令（落到 `default` → `ERR_CMD_UNKNOWN`）；
2. CONNECT 响应的 RESOURCE 宣告 `0x11`（CAL/PAG + PGM），**不含** DAQ 位，
   保证"声明的能力"与"实际实现"一致；
3. 用例 `DynamicDaqIsExplicitlyOutOfScope` 以可执行方式固定该范围决定。

---

## 3. 实现中发现并修复的真实缺陷

> 以下四项均为**工具实测发现**，非推理。前三项在测试设施内，第四项属先前
> 文档错误结论的更正。

### 3.1 `ReadFrame(Bytes out_packet)` 按值传参导致帧内容丢失

**症状**：Slave 打印出"收到帧 `FF 00`"，但 `DispatchCommand` 收到的
`xcp_packet.size() == 0`，所有命令都走空包分支，响应永不发出，
Master 侧 CONNECT 超时。

**根因**：`int ReadFrame(Bytes out_packet);` 参数按值传递，函数填充的是
一份私有副本，调用方容器恒为空。

**修复**：改为 `int ReadFrame(Bytes& packet_out);`（`tcp_test_slave.hpp:271-278`）。

**教训**：这类"函数看起来填了数据、调用方拿到空"的缺陷在单看任一函数时
都不显眼，必须靠端到端 trace 才能定位。**定位手段**：在 `ReadFrame` 出口与
`DispatchCommand` 入口同时打印 size，两者不一致即刻锁定传参方式问题。

### 3.2 命令请求字段偏移全错

**症状**：CONNECT 成功后再无命令可用，报
`"命令 244 响应数据长度 0 不等于期望的 elements*AG = 4"`。

**根因**：实现时凭印象写了偏移。实际规范布局（源级核证）：

| 命令 | 真实布局 | 依据 |
|---|---|---|
| SET_MTA | `[F6][MODE][reserved][Extension][Address(4)]` = 8 字节 | `src/command_codec.cpp:132-136` |
| SHORT_UPLOAD | `[F4][NumberOfElements][reserved][Extension][Address(4)]` = 8 字节 | `src/command_codec.cpp:166-170` |
| UPLOAD | `[F5][NumberOfElements]` | `src/command_codec.cpp:150-151` |

**关键**：`NumberOfElements` 在 **byte1**（紧邻 PID），地址从 **byte4** 起；
旧实现把 count 放在 byte2、地址放在 byte3，整体错位一字节。

**修复**：按上表重写三处偏移。

### 3.3 模拟内存"块存储、按字节读"粒度混用

**症状**：`SetMemory(0x70012340, {CA,FE,BA,BE})` 后 `UPLOAD 4` 只返回
`CA 00 00 00`。

**根因**：`m_memory_` 是 `std::map<Address, Bytes>`（以起始地址为键存整块），
但读取侧只取 `it->second[0]`——即"每遇到一个地址就取该地址块的首字节"，
于是 4 个地址里只有首地址命中，其余落空。

**修复**：`m_memory_` 改为 `std::map<Address, std::uint8_t>`（**逐字节粒度**），
`SetMemory` 展开存储，读取侧直接查表。这样任意地址都能独立解析，
读取侧无需知道"某块从哪开始、有多长"。

### 3.4 故障注入的响应序号必须计入 Connect 自身消耗

**症状**：`close_after_response_n = 1` 从未触发，用例超时失败。

**根因**：响应序号是**全局递增**的，而 `XcpMaster::Connect()` 自身会消耗
4 个响应：CONNECT(1)、GET_VERSION(2)、GET_STATUS(3)、PAG 探测(4)。
若在 `Connect()` **之后**才 `SetFaultInjection`，序号早已越过 1。

**修复**：故障必须在 `Connect()` **之前**装好，且目标序号为 5。
该约束已写入用例注释，避免后续维护者重犯。

---

## 4. 测试结果

### 4.1 B-T3 新增用例（19 项，全绿）

```
[==========] 19 tests from 2 test suites ran.
[  PASSED  ] 19 tests.
```

| 套件 | 用例 | 覆盖 |
|---|---|---|
| `TcpLoopbackProfileTest` | `FullSessionConnectReadDisconnect/{AsamV14Bit8, AsamV15Bit8, AsamV15Bit16, AsamV15Bit32}` | §6.2-1/3：Connect + SET_MTA/UPLOAD + Disconnect |
| | `ShortUploadMatchesUpload/{四档}` | §6.2-3：SHORT_UPLOAD 与 UPLOAD 一致 |
| | `CtrAdvancesMonotonically/{四档}` | §6.2-6：RX CTR 随帧推进 |
| `TcpLoopbackTest` | `SlaveRejectsSecondConnection` | §6.1：第二条连接被显式拒绝，且不影响首条会话 |
| | `ByteWiseSplitFramesAreReassembled` | §6.1 分片注入：逐字节写出仍能重组 |
| | `FrameHeaderSplitAcrossWrites` | §6.1 分片注入：帧头跨 write 边界 |
| | `PeerCloseAfterResponseIsObserved` | §6.2-8：peer FIN 后会话不可复用，报 `Cancelled` |
| | `NoAutomaticReconnectAfterPeerClose` | §6.2-9：绝不静默重连 |
| | `ProfileMismatchIsNotAutodetected` | §6.1：Profile 不匹配必须显式失败 |
| | `DynamicDaqIsExplicitlyOutOfScope` | §6.1：DAQ 验收已删除的范围决定 |

### 4.2 全量回归

```
[==========] 740 tests from 47 test suites ran.
[  PASSED  ] 740 tests.
```

B-T2 结束时为 722 项，本次新增 19 项（部分套件内计数合并），**无回归**。

### 4.3 无外部依赖（§6.3 Gate 第 3 条）

- 全部绑定 `127.0.0.1:0`（OS 分配临时端口），**无固定端口**；
- 无公网访问；
- 不依赖 backlog 深度（第二条连接被主动 accept 后立即关闭）。

---

## 5. 与计划的偏差（显式记录）

| 计划条目（§6.1 故障注入） | 实际 | 说明 |
|---|---|---|
| Header/Payload/Tail 每个边界分片 | **部分**：已实现逐字节（`split_bytes_n`）与固定块（`chunk_size_n`） | 这两者可构造任意边界切分，但未为每个边界写独立用例 |
| 多帧单 write | **未测** | 需在一次 write 中拼接多个 Frame；当前 Slave 一次响应只写一帧 |
| AsamV15 末帧无/完整/部分 Tail | **未测** | 需构造畸形 Tail；当前只测合法 Tail 路径 |
| AsamV14 LEN 内扩长 | **未测** | XCPlite 的"异步 CRM/DTO LEN 内 4 字节扩长"未构造 |
| RST（而非 FIN） | **未测** | 仅关闭连接产生 FIN，未用 `SO_LINGER=0` 构造 RST |
| duplicate/jump/backward CTR | **已实现未测** | `TcpFaultInjection` 三项字段已实现，未写对应用例 |
| corrupt LEN | **已实现未测** | 同上 |
| 延迟读取触发 partial send/timeout | **未测** | 另见 B-T2 记录同类偏差 |
| §6.2-2 PL1.2 unknown-command 路径 | **部分**：GET_VERSION 回 `ERR_CMD_UNKNOWN` 后连接**不失败**已隐含验证（Slave 未实现 `FB`，Connect 仍成功） | 未写显式断言用例 |
| §6.2-4 标定写入并回读 | **未测** | Slave 已实现 `DOWNLOAD`（最小写回），未写用例 |
| §6.2-5 Dynamic DAQ | **按计划删除** | 见 §2.5 |
| §6.2-7 Disconnect 先 XCP 后 TCP | **未测** | 未断言 DISCONNECT 先于 close |

> **诚实声明**：上表"未测"项**不是"已通过"**。B-T3 的绿灯证明的是
> "双 Profile 主路径 + 分片重组 + 断链契约 + Profile 不匹配防护"，
> **不等于** §6.1 列举的全部故障注入面已覆盖。

---

## 6. B-T3 Gate 自评

| Gate 条件（§6.3） | 状态 |
|---|---|
| 双 Profile 所有单元和 loopback 集成测试通过 | ✅ 19/19 |
| 全量 `libxcp_tests` 通过 | ✅ 740/740 |
| 不依赖公网、固定端口或 backlog | ✅ |
| 实施记录区分"自有闭环"与"独立实现互操作" | ✅ 见 §2.3 |

**未完全满足项**：§6.1 故障注入清单中多项标记为"未测"（见 §5）。
这些不阻断主路径，但**不应**被当作已验证。若需收口，建议后续补：

1. 多帧单 write 与 AsamV15 三种 Tail 形态（`SendRaw` 已备好注入入口）；
2. duplicate/jump/backward CTR 与 corrupt LEN 的用例（字段已实现，仅缺用例）；
3. §6.2-4 标定写入回读、§6.2-7 Disconnect 顺序断言。

---

## 7. 后续批次

- **B-T4**：`thirdparty/a2l-sdk` endpoint 语义（`endpoint_*` 字段、ABI v8→v9）、
  Profile/版本来源、endpoint 选择。**需再次获得 thirdparty 修改批准**。
- **B-T5**：CLI 双 Profile 连接与互操作验收。