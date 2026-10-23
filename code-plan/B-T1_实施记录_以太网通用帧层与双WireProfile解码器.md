# B-T1 实施记录：以太网通用帧层与双 Wire Profile 流解码器

> 对应计划：`code-plan/TCP传输_实施计划_B-T1至B-T5.md` 第 4 章（B-T1）
> 批次：B-T1
> 裁决依据：m00560（冲突 1 用方案 B「编码即拒绝」；冲突 2 用方案 A「TcpWireProfile 定义在 B-T1」）

---

## 1. 本批次目标

把原本只服务 UDP 的「LEN + CTR」帧层，抽象为**传输无关的以太网通用帧层**，并在其上
按 ASAM XCP on Ethernet Transport Layer V1.5.0 实现 **两种 wire profile** 的流式解码器：

| Profile | 规范来源 | 帧格式 | 适用场景 |
|---|---|---|---|
| `AsamV15` | ASAM-AE-MCD-1-XCP-AS-Ethernet-Transport-Layer-V1-5-0 §5.3 | `LEN(u16le) + CTR(u16le) + Packet[LEN] + Tail[0..3]` | TCP / 通用以太网 |
| `AsamV14` | ASAM XCP on Ethernet V1.4 | `LEN(u16le) + CTR(u16le) + Packet[LEN]` | UDP 兼容路径 |

两种 profile 的区别**仅**在于 LEN 之外是否存在对齐补零的 Tail，且该 Tail 只有在
`alignment = 32-bit` 时才会出现。

---

## 2. 交付物清单

### 2.1 新增文件

| 文件 | 行数 | 职责 |
|---|---|---|
| `include/libxcp/ethernet_header_codec.hpp` | — | 通用帧层对外契约：`kEthernetHeaderSize`、`kEthernetMaxXcpPacket`(65535)、`kEthernetMaxFrameSize`、`EthernetFrame`、`EncodeEthernetFrameAligned`、`DecodeEthernetDatagramAligned` |
| `src/ethernet_header_codec.cpp` | — | 通用帧层的编解码实现（原 `udp_header_codec` 的通用部分上移） |
| `include/libxcp/tcp_wire_profile.hpp` | — | `TcpWireProfile{Unknown, AsamV15, AsamV14}`、`IsTcpWireProfileResolved`、`TcpWireProfileName` |
| `src/tcp_stream_decoder.cpp` | — | TCP 字节流状态机：`Push` / `Finish` / `Reset` |
| `include/libxcp/tcp_stream_decoder.hpp` | — | `TcpStreamDecoder` 对外契约与 `DecodeResult` / `DecodedFrame` |

### 2.2 新增测试

| 文件 | 行数 | 用例数 |
|---|---|---|
| `tests/ethernet_header_codec_test.cpp` | 410 | 通用帧层 + §4.4 Gate（LEN 边界、版本基线） |
| `tests/tcp_stream_decoder_test.cpp` | 587 | 双 profile 状态机全套 |

两者均已加入 `tests/CMakeLists.txt`（`:54`、`:55`）。

### 2.3 修改文件

| 文件 | 改动 |
|---|---|
| `include/libxcp/udp_header_codec.hpp` | 保留 UDP 专属契约（`kUdpMaxXcpPacket` = 65503 等），通用部分下沉到 `ethernet_header_codec` |
| `src/udp_header_codec.cpp` | 改为对通用帧层的**薄别名**，保持既有调用方不变（`-128` 行） |
| `include/libxcp/protocol_version.hpp` | 新增 `kProtocolVersion12{1,2}`（下限自 1.3 下调至 1.2） |
| `src/protocol_version.cpp` | 支持下限与校验逻辑同步 |
| `src/version_resolver.cpp` | 版本裁决随之调整 |
| `CMakeLists.txt` | 登记 2 个新源文件 |
| `tests/CMakeLists.txt` | 登记 2 个新测试文件 |
| `tests/udp_header_codec_test.cpp` | 适配「空 Packet 即拒绝」新契约 |
| `tests/version_resolver_test.cpp` | 适配 PL 1.2 新基线 |
| `tests/xcp_master_integration_test.cpp` | 适配空 Packet 契约 |

---

## 3. 关键设计决策

### 3.1 Profile 必须显式指定，禁止自动推断

**决策**：`TcpStreamDecoder` 构造时强制传入 `TcpWireProfile` 与 `PacketAlignment`，
`Unknown` 直接抛异常（fail closed）。

**理由**：当 `alignment = 8-bit` 时，`AsamV15` 与 `AsamV14` 的帧字节形式**数值完全等价**
（Tail 长度恒为 0）。也就是说，**首帧内容无法区分两种 profile**。任何"先猜再纠正"的
启发式都会在混合/演进场景下静默出错。因此 profile 是**连接级不可变属性**，由调用方
（B-T3 的 server 配置 / B-T5 的 CLI 参数）提供。

对应测试：`TcpStreamDecoderCtor.RejectsUnknownProfile`、
`TcpStreamDecoderCrossProfile.V14StreamIsAlsoValidV15WhenAlignedBit8`。

### 3.2 编码器「编码即拒绝」空 Packet（冲突 1 方案 B）

**决策**：`CheckEthernetPacketLen()` 对 `len == 0` 抛 `InvalidArgument`。

**理由**：每个 Frame 至少携带 1 字节 PID。此前"编码宽进、解码严出"的契约会让
非法帧在本进程内被正常构造出来，再在对端解码时才失败——故障位置与故障原因被
拉开，定位成本高。改为编码即拒绝后，非法输入在本进程立刻暴露。

**影响**：推翻迁移前的旧契约，已同步修改 `UdpHeaderEncode.EmptyPacketProducesValidFrame`
等既有用例（见 §5 失败记录）。

对应测试：`TcpStreamDecoderMalformed.ZeroLenFrameFailsClosed`。

### 3.3 V14 的 LEN 内尾部零是**真实数据**，禁止裁剪

**决策**：decoder 对 `AsamV14` 完全按 `LEN` 上交字节，不做任何尾部零裁剪。

**理由**：V14 的 DTO / 异步 CRM 允许把 message 总长按 4 字节扩长，且该扩长**计入 LEN**。
因此 LEN 内的尾部零是协议数据而非填充，裁剪会破坏上层的 DTO 解析。

对应测试：`TcpStreamDecoderCoalescing.V14PreservesLenInternalPadding`、
`TcpStreamDecoderGate.V14LenInternalPaddingSurvivesByteAtATime`。

### 3.4 AsamV15 单独一帧可省略末尾 Tail

**决策**：`Finish()` 时若 payload 已完整交付、仅缺 Tail，判为**合法**而非截断。

**理由**：规范允许最后一帧不带 Tail（发送方无需为末尾补零）。但若 Tail **部分到达**
（1..N-1 字节），仍必须判为截断——因为这说明字节流在中途被破坏。

对应测试：`TcpStreamDecoderFinish.OmittedTrailingTailIsValid`、
`TcpStreamDecoderFinish.PartialTailIsTruncated`。

### 3.5 畸形后停机，禁止启发式恢复

**决策**：一旦进入畸形态（`IsMalformed()`），后续 `Push` 一律返回畸形且不产出帧，
直到显式 `Reset()`。

**理由**：TCP 是字节流，失去帧边界后**无法**可靠地重新同步。任何"读到下一个零再当
Header"的启发式都可能把攻击者构造的数据当成合法帧。fail closed 是唯一安全选择。

对应测试：`TcpStreamDecoderMalformed.StaysFailedAndIgnoresFurtherData`。

---

## 4. §4.4 Gate 验证结果

计划 §4.4 要求的 Gate 项及实测情况（全部通过）：

| Gate 项 | 测试 | 结果 |
|---|---|---|
| LEN = 1（最小合法） | `EthernetGateLenBoundaries.LenOneIsAccepted` | ✅ |
| LEN = 65503（UDP 上界） | `EthernetGateLenBoundaries.UdpLimitBoundary65503AcceptedByBothLayers` | ✅ |
| LEN = 65504（越 UDP 界、合法通用界） | `EthernetGateLenBoundaries.Len65504AcceptedByGenericButRejectedByUdp` | ✅ |
| LEN = 65535（通用上界） | `EthernetGateLenBoundaries.Len65535IsMaxAccepted` | ✅ |
| LEN = 65536（越界拒绝） | `EthernetGateLenBoundaries.Len65536IsRejected` | ✅ |
| 65535 + 32-bit 对齐总长不越界 | `EthernetGateLenBoundaries.MaxPacketWithBit32AlignmentStaysInBounds`、`TcpStreamDecoderGate.MaxLenFrameWithBit32AlignmentDecodes` | ✅ |
| decoder LEN == 配置上限 | `TcpStreamDecoderGate.LenExactlyAtConfiguredMaxIsAccepted` | ✅ |
| decoder LEN == 上限+1 | `TcpStreamDecoderGate.LenOneAboveConfiguredMaxIsRejected` | ✅ |
| PL 1.2 / 1.3 / 1.4 / 1.5 基线 | `ProtocolVersionBaseline.*`（3 例） | ✅ |
| 逐字节分片 | `TcpStreamDecoderFragmentation.SingleByteAtATimeDecodesAll` 等 4 例 | ✅ |
| Tail 边界 | `TcpStreamDecoderFragmentation.TailBoundarySplitForV15` | ✅ |
| 交叉 Profile 负向 | `TcpStreamDecoderCrossProfile.*`（2 例） | ✅ |

**实测数据**：`65535 + Bit32` → Header 4 + Payload 65535 = 65539，`65539 % 4 = 3`，
故 `tail_bytes = 1`，总长 65540，未超 `kEthernetMaxFrameSize`。该预测已由测试断言证实。

---

## 5. 测试记录

### 5.1 清理构建

```
cmake --build cmake-build-release --target clean   → exit 0
cmake -B cmake-build-release -S . -DCMAKE_BUILD_TYPE=Release -DLIBXCP_BUILD_TESTS=ON
cmake --build cmake-build-release -j 4             → exit 0
```

新旧文件（`tcp_stream_decoder`、`ethernet_header_codec`）**零警告**。

### 5.2 全量测试

```
./cmake-build-release/tests/libxcp_tests
[  PASSED  ] 696 tests.
```

### 5.3 本批次发现并修复的**真实缺陷**

#### 缺陷 1：`CompactBuffer()` 清空缓冲导致静默丢字节

**现象**：用例 `TcpStreamDecoderMalformed.IncorrectlyAddedTailFailsWithoutHeuristicRecovery`
失败——预期判为畸形，实际 `malformed = false` 且**不报错**。

**根因定位过程**：先用独立追踪程序复现，打印每一步状态：

```
r1 frames=1 malformed=0          # 第一帧交付正常
r2 frames=0 malformed=0          # 投喂后续字节后既不报错也无帧
```

逐步打印内部状态后定位到 `CompactBuffer()`：

```cpp
if (delivered_begin_ >= cursor_) {
    buffer_.clear();   // ← BUG：把 cursor_ 之后未消费的字节一并丢弃
    ...
}
```

`delivered_begin_` 只标记"已交付数据的结束"，而 `cursor_` **之后**可能仍有属于后续帧、
尚未被消费的字节（AsamV15 待消费的 Tail、或已到达但不足以组成下一 Header 的字节）。
原实现直接 `clear()` 使这些字节永久丢失——**这是本类最严重的缺陷类型：静默失步**。
它不会报错，只会让后续所有帧解析错位。

**修复**：改为始终把 `[cursor_, end)` 前移到缓冲原点，不再走 `clear()` 快捷路径。

```cpp
const auto remaining_begin = buffer_.begin() + static_cast<std::ptrdiff_t>(cursor_);
buffer_.erase(buffer_.begin(), remaining_begin);
cursor_ -= delivered_begin_;
delivered_begin_ = 0;
```

**修复后**：同一追踪程序输出

```
r2 frames=0 malformed=1 reason=TCP 流中出现 LEN=0 的 Frame：每个 Frame 至少携带 1 字节 PID
```

即畸形被正确检出并给出可诊断原因。

**意义**：该缺陷**由测试首先发现**，且若不修复，B-T3 的 slave 与 B-T5 的互操作会在
真实 TCP 连接上出现难以复现的错位。修复后 696 项测试全绿。

#### 缺陷 2（编译期）：`EXPECT_LT` 宏实参含花括号

`EXPECT_LT(kProtocolVersion13, XcpProtocolVersion{1, 4})` 中，预处理器把花括号内的
逗号当作宏实参分隔符，报 `too many arguments provided to function-like macro invocation`。
**修复**：先落成具名变量 `const XcpProtocolVersion v14{1, 4};` 再比较。

#### 缺陷 3（编译期）：使用了未定义的 `operator<=`

`XcpProtocolVersion` 仅定义 `operator<` 与 `operator==`，不支持 `EXPECT_LE`。
**修复**：改用 `EXPECT_LT` 表达等价语义。

#### 缺陷 4（流程）：陈旧测试二进制导致假绿

首次跑测试得到"全绿"，实为 `libxcp_tests` 仍是 10 月 18 日的**陈旧产物**，未编入本次改动。
必须显式 `-DLIBXCP_BUILD_TESTS=ON` 重新配置再编译，才会真正编译测试目标。
**教训**：验证前必须确认被测二进制确由本次改动编译（看 mtime / 构建目标），
否则"全绿"毫无意义。

---

## 6. 与计划的偏差说明

| 项 | 计划 | 实际 | 说明 |
|---|---|---|---|
| PL 下限 | §4.3 提到下调至 1.2 | 已完成 | 依据 `thirdparty/XCPliteV1.2` 上报 PL `0x0102` / TL `0x0104` |
| 空 Packet | 计划未明确 | 按裁决改为拒绝 | 见 §3.2 |
| `TcpWireProfile` 位置 | 计划待在 B-T3 落地 | 按裁决提前至 B-T1 | 见 §3.1；decoder 同批完成，B-T1 达到 §4.4 完整 Gate |

除上述三点（均由用户裁决 m00560 明确授权）外，实现与计划 §4 一致。

---

## 7. 遗留事项

- **B-T2**：`TcpTransport`（`IXcpTransport` 的 TCP 实现，含连接建立、`close_socket_at_response_n` 注入点）。
- **B-T3**：双 Profile Test Slave 与集成测试（本批 decoder 已就绪）。
- **B-T4**：`a2l-sdk` 导出适配（**需先获 `thirdparty` 修改批准**）。
- **B-T5**：CLI 与互操作。
- 本轮未 commit（`AGENTS.md` 规定无明确指令仅允许 `git diff` / `git branch`）。

---

## 8. 文件路径索引

- 通用帧层契约：`include/libxcp/ethernet_header_codec.hpp`
- 通用帧层实现：`src/ethernet_header_codec.cpp`
- Wire Profile 定义：`include/libxcp/tcp_wire_profile.hpp`
- 流解码器契约：`include/libxcp/tcp_stream_decoder.hpp`
- 流解码器实现：`src/tcp_stream_decoder.cpp`
- 通用帧层测试：`tests/ethernet_header_codec_test.cpp`
- 流解码器测试：`tests/tcp_stream_decoder_test.cpp`
- 实施计划：`code-plan/TCP传输_实施计划_B-T1至B-T5.md`