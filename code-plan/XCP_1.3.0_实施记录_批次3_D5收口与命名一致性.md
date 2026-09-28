# XCP 1.3.0 实施记录 — 批次 3（D5 收口 + 命名一致性 + 关闭顺序缺陷修复）

> 依据文档：`code-plan/XCP_1.3.0_详细设计_类接口与头文件.md`（§2.2.1、§8.1、§16.4、§17.6、§18.1~§18.3、§15.2）、
> `code-plan/XCP_1.3.0_最小协议核心实现计划.md`（§4.7、§6.2）。
>
> 承接：批次 1（阶段 0~2，commit `155d8ae`）、批次 2（阶段 3~7，commit `92e8b5c`）。
>
> **本批次范围的用户裁决**：选择"以代码现状为准，改设计文档"处理命名遗留项，
> 并选择"补齐 D5 歧义 CTR 测试 + 更新设计文档状态"作为下一步方向。
> 未进入 A2L / CAN / 跨平台 CI（那三项需另行立项与澄清）。

---

## 1. 本次范围与动机

批次 2 收尾时，实施记录 §9「已知限制」与设计文档 §18 留下了若干**文档内可闭环**的尾巴。
本批次不做新功能，只做三件事：

| # | 任务 | 来源 |
|---|---|---|
| A | 补齐 D5（CTR 与期望值恰差 `0x8000` → 歧义丢弃）的**精确构造**用例 | 批次 2 记录 §9 限制 3；设计 §8.1 第 6 条 / §18.2 D5 |
| B | 按设计 §2.2.1 统一代码命名，关闭批次 1/2 反复标记的"待确认"项 | 批次 1 记录 §2 第 1 项；批次 2 记录 §9 限制 6 |
| C | 把设计文档中已落地/已校核的章节状态写回文档（避免文档继续自称"未经编译验证"） | 设计 §18.1 六个开放问题、§18.3 三项"预留" |

过程中额外发现并修复了 **1 个真实产品缺陷**（关闭顺序导致的句柄复用串扰），见 §4。

> **完成度更正（自查后追加）**：任务 A、C 已完成并验证；任务 B **只完成了函数改名部分**。
> 自查发现成员变量命名仍有 2 个产品结构体（7 字段）+ 若干测试设施偏差，
> 且其中 `CommandTimeouts` 与公开头文件其余 7 个结构体不自洽、与设计 §12 直接矛盾。
> 清单见 §3.4。**裁决结果**：沿用"以代码现状为准、不做批量重命名"的既有指示，
> 零代码改动，偏差按三类登记为 §2.2.1 显式豁免，并把 §12 字段名改为与代码一致。
> 遗留代价（公开 API 内部一处不一致）已如实记录于 §2.2.1.2 与 §9 限制 3。

---

## 2. 任务 A：D5 歧义 CTR 测试收口

### 2.1 为什么原来测不了

`FaultInjection` 原有两种 CTR 注入：

- `m_jump_ctr_n_`：固定 `ctr + 5` → 只能制造**前向跳号**；
- `m_duplicate_ctr_n_`：固定 `ctr - 1` → 只能制造**重复/后向**。

而 D5 要求判定"与期望值**恰好**相差 `0x8000`"的分支。这个差值既不是 +5 也不是 −1，
用旧夹具无法命中，只能覆盖其相邻区间（批次 2 记录已如实标注该限制）。

### 2.2 夹具改动（`tests/udp_test_slave.hpp/.cpp`）

新增按模 65536 的**有符号偏移**注入：

```cpp
/// @brief 对第 N 个响应的 CTR 施加指定的有符号偏移（模 65536）
std::optional<std::pair<std::size_t, int>> m_ctr_offset_n_;
```

施加逻辑（`SendResponseTo()`）：

```cpp
if (ctr_offset != 0) {
    // 先加 65536 保证中间值为正，再取模，使负偏移也得到正确的回绕结果
    ctr = static_cast<DatagramCtr>(
        (static_cast<long>(ctr) + 65536L + ctr_offset) % 65536L);
}
```

同时新增只读诊断接口 `NextSendCtr()`，用于断言"注入只改写实际发出的 Header，
不改变 Slave 自身计数器的推进规律"——这是 Master 侧边界用例成立的前提。

### 2.3 关键推导：如何精确命中 `0x8000`

判定依据（`UdpTransport::HandleFrame()`）：

```
expected = 最近收到的 CTR + 1
delta    = (实收 CTR - expected) mod 65536      // 前向距离
delta == 0x8000  →  方向歧义，丢弃
```

构造：CONNECT 响应使用 Slave 的 CTR=0，Master 以此建立基线 → 期望值 `expected = 1`。
对下一个响应施加偏移 `+0x8000`：

```
实收 = (1 + 32768) mod 65536 = 32769 = 0x8001
delta = (0x8001 - 1) mod 65536 = 0x8000   ← 命中歧义分支
```

> 初版实现曾误用偏移 `+0x7FFF`（得到实收 `0x8000`，但 delta = `0x7FFF`），
> 导致用例失败。该错误由测试本身暴露，已按上式修正——**这类差值推导必须落在
> 期望值上算，而不能直接把 CTR 值当成差值**。

### 2.4 新增用例

| 用例 | 断言 |
|---|---|
| `UdpTransportCtrSemantics.AmbiguousCtrOffsetIsDropped` | 差值 `0x8000` → 不交付、接收基线不推进、产生含 `0x8000` 的诊断 |
| `UdpTransportCtrSemantics.JustBelowAmbiguousBoundaryIsAccepted` | 差值 `0x7FFF` → 按前向跳号**接收**、报告缺口、推进到 32767 |
| `UdpTransportCtrSemantics.JustAboveAmbiguousBoundaryIsDropped` | 差值 `0x8001` → 按后向乱序**丢弃** |
| `UdpTestSlaveFault.CtrOffsetProducesExactHeaderCounter` | 夹具语义：偏移后 Header CTR 精确等于 `0x8001`，且 `NextSendCtr()` 仍按正常序列推进 |
| `UdpLoopback.AmbiguousCtrFrameDroppedAndSessionContinues` | 真实 UDP 端到端：歧义帧丢弃 + 诊断；清空注入后**同一通道继续正常收发**（证明策略只丢单帧、不禁用会话） |

三个相邻取值 `0x7FFF / 0x8000 / 0x8001` 现在全部有正向用例，边界归属被逐值锁定。

### 2.5 附带改进

`UdpTransport` 的 CTR 诊断文本由十进制改为 `0xNNNN` 十六进制（新增匿名命名空间辅助
`ctrToHex()`），使诊断输出能与设计 §8.1 条款、测试断言逐字对应，无需人工换算。

---

## 3. 任务 B：命名一致性收口

### 3.1 核对结论

设计 §2.2.1 规定「函数/成员函数/全局函数用大驼峰」，并规定成员变量用 `m_<snake>_`。

> ⚠️ **本节结论更正**：本节初稿曾写"全量扫描 `include/libxcp/*.hpp`、`src/*.cpp`
> 未发现非 `m_` 前缀的成员变量，实测代码已全面符合"。**该结论是错的**——当时的扫描
> 正则要求行内含 `(`，实际只匹配到函数声明，成员变量从头到尾没有被检查过。
> 用户追问"上次任务是否都完成了"后自查发现此漏洞，已用基于括号栈的作用域扫描重做，
> 结果见 §3.4。**函数改名部分（下表）经复核仍然成立；成员变量结论已推翻并更正。**

本小节先记录**已完成的函数改名**。已修正清单：

| 类别 | 原名称 | 现名称 | 涉及文件 |
|---|---|---|---|
| `XcpMaster` 公开 API | `connect` / `disconnect` / `isConnected` / `queryStatus` | `Connect` / `Disconnect` / `IsConnected` / `QueryStatus` | `xcp_master.hpp/.cpp` + 2 个测试 |
| `XcpMaster` 读取 API | `readMemory`（字节）/ `readMemoryElements`（元素） | `ReadMemoryBytes`（字节）/ `ReadMemory`（元素） | 同上 |
| `XcpMaster` 状态 API | `sessionParameters` / `sessionState` | `GetSessionParameters` / `GetSessionState` | 同上 |
| `IEventListener` | `onEvent` / `onService` / `onDto` | `OnEvent` / `OnService` / `OnDto` | `command_executor.hpp/.cpp` + 3 个测试 |
| `XcpAddress40` | `advance` | `Advance` | `protocol_types.hpp/.cpp` + 2 处调用 |
| UDP Header 自由函数 | `encodeUdpFrame` / `decodeUdpDatagram` | `EncodeUdpFrame` / `DecodeUdpDatagram` | `udp_header_codec.hpp/.cpp` + 3 个测试 |
| 协议类型自由函数 | `classifyPacket`、`errorCodeName`、`toErrorCode`、`eventCodeName`、`toEventCode`、`agToBytes`、`commModeBasicToAg`、`agToCommModeBasicField`、`hasResource`、`sessionStateName` | 同名大驼峰（`ClassifyPacket`、`ErrorCodeName`、`ToErrorCode`、…） | `protocol_types.hpp/.cpp` + 测试 |
| 异常自由函数 | `errorCategoryName` | `ErrorCategoryName` | `xcp_error.hpp/.cpp` + 测试 |
| `UdpTestSlave` 私有辅助 | `sendResponse`、`sendResponseTo`、`makeRes`、`makeErr`、`readAtMta`、`advanceMta` | 同名大驼峰 | `tests/udp_test_slave.hpp/.cpp` |
| 注释措辞 | `open()` / `close()` / `onPacketReceived()`（注释引用旧名） | `Open()` / `Close()` / `OnPacketReceived()` | `udp_transport.hpp/.cpp`、`command_executor.hpp/.cpp` |
| 死声明 | `memory_access.hpp` 同时声明 `ValidateRead` 与 `validateRead`（同签名、后者无定义无引用） | 删除 `validateRead` | `memory_access.hpp` |

**保留不变**：`src/*.cpp` 匿名命名空间内的实现细节函数（`parseIpv4()`、`lastSocketError()`、
`isTimeoutError()`、`ctrForwardDistance()`、`toHex()`、`protocolMessage()` 等）继续用 snake_case。
理由是它们具备内部链接、不对外承诺，保留小写可与"公开 API 大驼峰"形成视觉区分。
如需一并统一属于独立机械改动，已在设计文档 §2.2.1.1 显式记录为**未做项**，不做静默处理。

### 3.2 命名选择的说明：为什么不是两个 `ReadMemory` 重载

设计 §14 原文给出两个同名 `ReadMemory` 重载（一个收 `ByteCount`、一个收 `ElementCount`）。
实际不可行：`ByteCount` 与 `ElementCount` 都是 `std::uint32_t` 的别名，
`master.ReadMemory(addr, 0x00, 4)` 这样的字面量调用会产生**重载歧义**，无法编译。
因此按实现拆为 `ReadMemoryBytes` / `ReadMemory`，并把该理由写回设计 §14 与附录 B.1。

### 3.3 改名的安全性论证

- 全部为**纯机械重命名**，未改变任何签名语义、调用顺序或行为。
- 改名范围经脚本枚举确认：`(\.|->)` 形式的调用点仅存在于
  `xcp_master_integration_test.cpp`、`xcp_udp_loopback_test.cpp` 与实现文件本身；
  `f.session.IsConnected()` / `slave.IsConnected()` 等**同名但不同类**的调用点
  经逐条核对后未被误改（`Session::IsConnected`、`UdpTestSlave::IsConnected` 本已是正确拼写）。
- 改名后 `EncodeUdpFrame` / `DecodeUdpDatagram` 变长导致部分测试行超列宽，
  clang-format 自动重排，属预期 churn。

### 3.4 成员变量：自查发现的偏差与裁决（本批次以"登记豁免"收口，未改代码）

用基于括号栈的作用域扫描（只在"栈顶为 class 作用域"的行上匹配声明，
从而排除函数体内局部变量与多行参数声明续行）重做核查，得到如下**准确**清单。
设计文档已同步登记为 §2.2.1.2。

**产品代码：2 个结构体、7 个字段仍为裸 snake_case**

| 位置 | 结构体 | 字段 | 可见性 |
|---|---|---|---|
| `include/libxcp/command_executor.hpp:35/38/41` | `CommandTimeouts` | `command_timeout`、`synch_timeout`、`max_retries` | **公开头文件** |
| `src/udp_transport.cpp:157/159/160/161` | `UdpTransport::SocketImpl` | `winsock`、`handle`、`bound`、`remote` | 私有 PIMPL（`.cpp` 内） |

`CommandTimeouts` 是其中唯一影响**公开 API 自洽性**的：公开头文件其余 7 个结构体
（`ConnectResponse`、`GetStatusResponse`、`GetCommModeInfoResponse`、`SessionParameters`、
`XcpAddress40`、`PositiveResponse`/`NegativeResponse`/`EventPacket`/`ServicePacket`/`DtoPacket`、
`UdpFrame`/`UdpHeader`、`UdpTransportConfig`）字段全部是 `m_<snake>_`，只有它是裸名。
改名成本极小（每字段各 2 处引用）。设计 §12 原本把这三个字段写成 `m_*_` 形式，
即**代码与其对应设计章节直接矛盾**——该矛盾已按"以代码为准"消除（见下方裁决结果）。

**测试设施（§2.2.1 明确将"测试设施"纳入规则范围）**

| 位置 | 类别 | 字段 |
|---|---|---|
| `tests/udp_test_slave_test.cpp:314-317` | 夹具成员 | `slave_`、`ep_`、`ctr_`、`res_` |
| `tests/xcp_master_integration_test.cpp:330-333` | `Rig` 成员 | `slave`、`transport_ptr`、`master` |
| `tests/memory_access_test.cpp:275-276`、`tests/recovery_test.cpp:232-234` | 夹具成员 | `session`、`executor` |
| `tests/udp_test_slave.cpp:88-90` | 私有 PIMPL | `winsock`、`handle` |
| `AgCase` / `Case` / `AgIntegrationCase`（3 处） | **参数化测试 POD 数据载体** | `ag`、`max_cto`、`max_dto`、`short_upload_max`、`upload_max` |

**已确认为合规、非偏差**（初版扫描曾误报）：`mock_transport.hpp:65` 的 `responder`、
`recovery_test.cpp:391` 的 `upload_sends`、`session_test.cpp:406` 的 `workers`、
`udp_test_slave.cpp:436` 的 `ip` 等——均为**函数体内局部变量**，按 §2.2.1
"参数与局部变量用 snake_case"本就正确。

**裁决结果（本批次已执行）**：沿用用户对命名遗留项的既有指示——"以代码现状为准，
改设计文档…**不做批量重命名**"，选择**零代码改动、全部登记为显式豁免**：

- 设计 §2.2.1 规则表新增"**显式豁免**"三类（纯值聚合参数对象 / `.cpp` 内私有 PIMPL /
  GoogleTest 夹具成员 / 参数化测试 POD 数据载体），今后按"规则 + 豁免表"判定；
- 设计 §12 的 `CommandTimeouts` 字段名**改为与代码一致**（`command_timeout` /
  `synch_timeout` / `max_retries`），§12.1 流程与 §18.2 D2 的引用同步改为
  `m_timeouts_.xxx` 实际访问式 —— 即代码与设计章节的直接矛盾已消除；
- 设计 §2.2.1.2 完整登记偏差清单与裁决，并**如实写明代价**：`CommandTimeouts` 成为
  公开头文件中唯一不遵循 `m_<snake>_` 的结构体（其余 7 个均遵循），
  公开 API 内部留下一处已登记的命名不一致。

> 我此前给出的推荐是"只改 `CommandTimeouts`"（成本 3 字段 × 2 处引用即可消除上述不一致），
> 但采纳的是**零改动**这一指示延续项。该不一致因此长期存在，已记录在案，
> 日后若要消除只需回收这一条豁免。

**方法论教训**：扫描脚本的匹配条件本身就是一项断言，必须验证它真的覆盖目标。
本例中"要求含 `(`"这一条件把检查对象从"成员变量"悄悄换成了"函数声明"，
却输出了"未发现成员变量偏差"的结论——脚本"跑通且无输出"被误当成"检查通过"。
零命中必须先证明检测器能命中已知正例，否则等于没测。

---

## 4. 任务 C 过程中发现的真实缺陷：关闭顺序导致句柄复用串扰

### 4.1 现象

全量 `ctest` 运行后，`UdpTestSlaveFault.ResponseCounterStartsFreshAfterEachInjection`
**间歇性失败**：单独运行通过、轻负载复跑 60 次通过、满负载全量运行偶发失败。

### 4.2 根因

`UdpTransport::Close()` 与 `UdpTestSlave::Stop()` 原实现均为**先关 Socket，再 join 接收线程**：

```cpp
m_running_.store(false);
closeSocket(m_socket_->handle);   // ← 先关
m_socket_->handle = kInvalidSocket;
m_receive_thread_.join();         // ← 后 join
```

原注释给出的理由是"关闭 Socket 可立即唤醒阻塞的 `recvfrom`"。功能上确实能唤醒，
但打开了一个竞态窗口：

1. Socket 一旦关闭，**句柄号立即被 OS 回收**，并可能分配给进程中其他新建的 Socket
   （测试进程内 Socket 创建/销毁极其频繁，复用概率高）；
2. 此时接收线程可能正处于"已从内核取出报文、正在处理并回调监听器"的中间状态；
3. 迟到的操作便可能命中**被复用的句柄**，把数据投递到无关端点上。

这正是"仅在 Socket 高频创建/销毁时暴露"的典型竞态，因此表现为满负载偶发。

### 4.3 修复

改为**先 join 再关 Socket**（`src/udp_transport.cpp`、`tests/udp_test_slave.cpp`）：

```cpp
m_running_.store(false);
if (m_receive_thread_.joinable()) {
    m_receive_thread_.join();     // ← 线程已停，此后不再有并发操作
}
closeSocket(m_socket_->handle);   // ← 再关，无竞态
m_socket_->handle = kInvalidSocket;
```

唤醒不再依赖"关 Socket"，而是依赖 `Open()` 已设置的 `SO_RCVTIMEO` 轮询
（`UdpTransportConfig::m_receive_poll_interval_ms_`）。

### 4.4 代价与回归保护

`Close()` 的返回延迟上限变为一个轮询周期。既有用例
`UdpTransportClose.CloseWhileBlockedReturnsPromptly` 特意把轮询周期放大到 1000 ms
并断言 `Close()` 在 1000 ms 内返回，正好锁定该上限，**修复后仍通过**。
若后续需要更高关闭实时性，应改用自管道/事件对象唤醒，而**不应回退到"先关 Socket"**。
该结论已写入设计文档 §16.4。

### 4.5 验证

| 验证方式 | 修复前 | 修复后 |
|---|---|---|
| 单项复跑 | 通过（不复现） | 通过 |
| 全量 `ctest` 连续复跑 | 偶发 1 次失败 | **15 次全部通过** |
| 时序敏感子集（Udp/Recovery/CmdPending/Loopback/Slave，93 项）连续复跑 | — | **6 次全部通过** |
| `UdpTestSlaveFault` / `UdpTestSlaveSession` 单进程循环 | — | **40 次全部通过** |

---

## 5. 设计文档同步更新清单

按用户裁决"以代码现状为准，改设计文档"，逐处就地标注"批次 3"说明：

| 章节 | 更新内容 |
|---|---|
| §2.2.1.1（**新增**） | 命名规则落地状态核对表：逐项给出 ✅/⚠️ 与理由；明确"匿名命名空间实现函数保留 snake_case"是**未做项**而非遗漏 |
| §10 | `ReadU16()` 改为返回 `std::optional`（越界安全）；补充 `ReadU8()`；`ReadU32()` 标注**未实现**及原因（最小子集无 32 位字段），并给出将来补充时应遵循的模式 |
| §12 | `CommandResult` 标注**未实现**及理由；私有区补齐 `RunCommand`（原名 `ExecuteCommand`）、`PerformAttempt`、`DispatchResponse`、`RestoreUploadMta`、`EnsureCodec`、`CheckResLength`；同步补齐 `m_synch_cv_`、`m_response_ready_`、`m_in_recovery_`、`m_last_mta_` 等字段 |
| §12.1 | 标题与流程改为 `RunCommand`；按实现补全 `PerformAttempt` 两步结构、槽位占用保护、EV_CMD_PENDING"重启计时且仍上报"、UPLOAD 重试前 `RestoreUploadMta`、"回调在释放锁之后" |
| §14 | `ReadMemory` 字节版改名 `ReadMemoryBytes` 并写明不可用同名重载的原因；`GetSessionParameters` / `GetSessionState` / `QueryStatus` |
| §15.2 | `FaultInjection` 补 `m_ctr_offset_n_`；公开区补 `NextSendCtr()` / `IsConnected()` / `SendRawPayload()` / `SendRawPayloadTo()`；私有区补全 `SendResponseTo` / `MakeRes` / `MakeErr` / `ReadAtMta` / `AdvanceMta`；成员补 `m_response_count_` |
| §16.4 | 关闭语义改为 4 步有序列表；**新增"为何不能先关 Socket"**专段，记录真实缺陷的形成机理、触发条件、修复与代价（对应 §4） |
| §17.6 | 标题由"待官方规范确认"改为"已校核"；补 bit3-5 与 `11=保留`；**补 4 条校核证据**（本地文档行号、第三方实现交叉验证、代码落点） |
| §18.1 | Q1~Q6 全部标记 ✅ 关闭，逐条给出结论与证据；末尾区分"设计开放问题已关闭"与"外部互操作验证仍未做" |
| §18.2 | D1~D8 表格增加"实现落点"与"验证用例"两列；D5 注明批次 3 已补齐精确构造用例及三个相邻取值的覆盖 |
| §18.3 | 三项"预留"逐条标注已实现并有用例（`Fail()` 另有 `FailReason()` 访问器） |
| 附录 B.1 | 明确 `ReadMemory()` 第三参数是**元素数**；需要按字节读取用 `ReadMemoryBytes()`；补充单位说明 |
| 文末 | 删除"未经编译验证/开始编码前需解决第 18 节"的过期声明，改为"已落地并通过 253 项测试"；明确"与文档存在差异处以代码为准" |

---

## 6. 构建环境与命令

```
CMake 4.0.2 / MSVC 19.44.35228 (VS 2022 Community, x64) / Windows SDK 10.0.26100
生成器：Visual Studio 17 2022
GoogleTest v1.15.2（离线源 .deps-cache/googletest）
clang-format 22.1.8
```

```bash
cmake -B cmake-build-release -S . && cmake --build cmake-build-release --config Release
ctest --test-dir cmake-build-release -C Release --output-on-failure

cmake -B cmake-build-debug -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build-debug --config Debug
ctest --test-dir cmake-build-debug -C Debug --output-on-failure
```

> 沿用批次 1 记录 §5 的环境限制：本沙箱内 `cmake --build -j N` 在 MSBuild 并行解析
> 项目引用阶段会失败，须串行构建（不带 `-j`）。

---

## 7. 测试结果

### 7.1 汇总

| 配置 | 构建 | 项目源码警告 | ctest |
|---|---|---|---|
| Release | 成功 | **0**（`/W4`） | **253 项：252 通过 + 1 跳过，0 失败** |
| Debug | 成功 | **0** | **253 项：252 通过 + 1 跳过，0 失败** |

较批次 2 的 248 项增加 **5 项**（本批次新增，见 §2.4）；
唯一跳过项与批次 2 相同且为按设计跳过：
`AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`
（AG=BYTE 时任意字节数都合法，无"非整除"场景可测）。

### 7.2 稳定性

| 复跑对象 | 次数 | 结果 |
|---|---|---|
| 全量 `ctest` | 15 | 全部通过，0 次偶发失败 |
| 时序敏感子集（Udp/Recovery/CmdPending/Loopback/Slave，93 项） | 6 | 全部通过 |
| `UdpTestSlaveFault` / `UdpTestSlaveSession` | 40 | 全部通过 |
| 新增 D5 五项用例 | 每次全量均执行 | 全部通过 |

> 对比：修复 §4 缺陷前，全量运行曾出现 1 次偶发失败；修复后连续 15 次全量运行零失败。

### 7.3 格式检查

```
clang-format --dry-run --Werror -style=file <全部 .hpp/.cpp>   → exit 0（无差异）
```

### 7.4 编码检查

改动文件均保持 **UTF-8 无 BOM**，中文 Doxygen 注释完整（已按文件抽样校验字节序列）。

---

## 8. 验收对照（计划文档 §10）

批次 2 已使 §10 的 15 条验收标准全部达标。本批次对其中的关键条目做了**加固**，
其余条目维持批次 2 的证据，此处仅列受本批次影响者：

| # | 验收标准 | 本批次影响 |
|---|---|---|
| 6 | Timeout 执行 SYNCH，UPLOAD 重试前恢复 MTA | 不变（证据见批次 2）；`RunCommand` 流程已按实现写回设计 §12.1 |
| 9 | Release 单元 + Mock 集成 + UDP Loopback 全部通过 | 加固：双配置 253 项 + 全量 15 次复跑 + 子集 6 次复跑 |
| 11 | 不把 CTR 当事务 ID，能诊断丢包/重复/乱序 | **加固**：补齐 D5 歧义点精确用例，形成 `0x7FFF/0x8000/0x8001` 三值边界闭环 |
| 12 | UDP 测试不依赖固定端口、外网、厂商库，能可靠取消和释放资源 | **加固**：修复关闭顺序竞态，消除偶发失败与跨用例串扰 |
| 14 | 未实现功能返回明确错误 | 不变；`CommandResult` / `ReadU32` 的未实现状态已显式记录于设计文档，非静默缺失 |
| 15 | 不把 Loopback/Mock 通过表述为真实 ECU/CANape 互操作通过 | 强化：设计文档文末与 §18.1 均重申外部互操作验证**未做** |

---

## 9. 已知限制与后续工作

1. **未与真实 ECU / CANape 互操作验证**（延续批次 2 限制 1）：所有端到端测试均为
   自有 Master ↔ 自有 UdpTestSlave，两端可能共享同一误解（计划 §11.4）。
   本批次**未改变**这一事实，且在文档中再次显式声明。
2. **仅 Windows 实测**（延续批次 2 限制 2）：跨平台代码已隔离在
   `udp_transport.cpp` 顶部平台适配层；Linux/macOS 未在本环境编译运行。
   ⚠️ 本批次的关闭顺序修复在 POSIX 下语义等同（`recvfrom` 同样受 `SO_RCVTIMEO`
   约束），但**仍需三平台 CI 实测确认**。
3. **公开 API 内部留有一处已登记的命名不一致**（本批次按"零改动 + 登记豁免"裁决的代价）：
   `CommandTimeouts`（`include/libxcp/command_executor.hpp` 的 3 个公开字段
   `command_timeout`/`synch_timeout`/`max_retries`）是公开头文件中唯一不遵循
   `m_<snake>_` 的结构体，其余 7 个均遵循。私有 `UdpTransport::SocketImpl`（4 字段）
   与测试夹具/POD 参数载体同批豁免。清单与理由见 §3.4 / 设计文档 §2.2.1 豁免表。
   消除成本极小（3 字段 × 各 2 处引用），日后若要统一，回收该条豁免即可。
4. **匿名命名空间函数命名未统一**：`parseIpv4()` 等内部实现函数仍为 snake_case，
   属设计 §2.2.1 的适用范围边缘，已在 §2.2.1.1 显式登记为未做项。
5. **`CommandResult` 与 `ReadU32()` 未实现**：已在设计文档就地标注理由，
   非静默缺失；将来若需要 32 位字段解析或统一结果包装，按文档给出的模式补充。
6. **`m_ctr_offset_n_` 的偏移语义**：负偏移依赖 `(ctr + 65536 + offset) % 65536`
   的中间值为正；`offset` 超出 `[-65536, 65535]` 的用法未定义，测试只使用窄范围值。
7. **Block/Interleaved Mode、DTO 解析、A2L、CAN/CAN FD、跨平台 CI** 仍在本计划外
   （计划 §2.2、§12），需另行立项。

---

## 10. 下一步（供决策）

本批次已把设计文档范围内**所有可闭环项**关闭。剩余方向均需新的范围确认：

1. **进入计划 §12 里程碑**：最小 A2L（MEASUREMENT / 地址 / 数据类型 / Byte Order /
   COMPU_METHOD）。属于全新模块，按 AGENTS.md 要求需先做需求澄清与分节设计并经批准。
2. **XCP on CAN/CAN FD Transport**：需先确认厂商库可得性与依赖策略
   （Vector/PEAK 通常涉及授权与安装）。
3. **跨平台 CI 与三平台实测**：需容器/CI 环境配合，Windows 沙箱内无法真实三平台验证。
4. **与第三方真实 ECU/CANape 的 UDP 互操作抓包对照**：可关闭验收标准 15 的最后缺口，
   但依赖硬件与第三方工具。
