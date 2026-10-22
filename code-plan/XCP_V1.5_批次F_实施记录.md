# XCP V1.5 批次 F 实施记录（F-03 + F-11）

- 批次：F（方案 §5 批次划分；F-03 与 F-11 同批，因两者均为低风险收口且 F-11 涉及执行器恢复核心）
- 实施日期：2026-10-21
- 规范基准：`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.pdf`（**唯一权威来源**）
  - ⚠ 用户裁决 m01451：不再参考 Markdown 版 `Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.md`
- 前置批次：C / D / E1 / E2 均已交付

---

## 1. 规范依据

### 1.1 F-03（GET_ID）

**Table 60「GET ID positive response structure」（原始 PDF 第 123 页）逐字**：

```
Position     Type    Description
0            BYTE    Packet ID: 0xFF
1            BYTE    Mode
2            WORD    Reserved
4            DWORD   Length [BYTE]
8            BYTE 1  first byte of Identification (if mode = 1)
8+Length-1   BYTE n  nth byte of identification
```

`res_data` 剥离首字节 `0xFF` 后索引全体 −1：`mode@0`、`reserved@1..2`（WORD）、
`length@3..6`（DWORD）、`data@7..`。

**Table 60 下方原文**：`The following rule applies: Length mod AG = 0`

**Table 61「GET ID Mode 位定义」（原始 PDF 第 124 页）**：

| 位 | 语义 |
|---|---|
| Bit 7..2 | `x`（保留） |
| Bit 1 | `COMPRESSED_ENCRYPTED` |
| Bit 0 | `TRANSFER_MODE` |

`TRANSFER_MODE=1` → 标识符内联于响应剩余字节；`=0` → Slave 设置 MTA，
Master 用 **UPLOAD** 读取。

**Table 61 原文（UPLOAD 元素数换算）**：`Number of Data Elements UPLOAD [AG] = Length / AG`

**对照 Table 45（GET_VERSION 响应，原始 PDF 第 114 页）**：
`[Packet ID][Reserved][Major PL][Minor PL][Major TL][Minor TL]` —— **固定 6 字节、
根本不含 LENGTH 字段**。

### 1.2 F-11（错误恢复增强）

**§7.6.1.2 PRE-ACTION 列表（原始 PDF 第 239 页）**：`Wait t7`、`SYNCH`、
`GET_SEED/UNLOCK`、`SET_MTA`、`SET_DAQ_PTR`、`START_STOP_x`、`Reinitialise DAQ`

**§7.6.1.3 ACTION 列表**：`Display error`、`Retry other syntax`、
`Retry other parameter`、`Use ASAM MCD-2 MC Description File`、`Use alternative`、
`Repeat 2 times`、`Repeat ∞ times`、`Restart session`、`Terminate session`

**恢复矩阵原文样例（原始 PDF 第 246 页）**：

```
GET_STATUS  timeout t1          SYNCH        repeat 2 times
SET_REQUEST timeout t1          SYNCH        repeat 2 times
            ERR_CMD_BUSY        wait t7      repeat ∞ times
            ERR_PGM_ACTIVE      wait t7      repeat ∞ times
            ERR_CMD_SYNTAX      -            retry other syntax
            ERR_OUT_OF_RANGE    -            retry other parameter
GET_SEED    timeout t1          SYNCH        repeat 2 times
            ERR_CMD_BUSY        wait t7      repeat ∞ times
UNLOCK      timeout t1          SYNCH        repeat 2 times
            ERR_CMD_BUSY        wait t7      repeat ∞ times
```

`ERR_CMD_BUSY → wait t7 → repeat ∞ times` **几乎出现在每个命令的矩阵行中**。

---

## 2. 裁决文档对 F-03 的更正（本批次落实）

方案 §2A 已裁定：**审计对 F-03 的核心指控不成立**。

| 审计原指控 | 独立复核裁定 |
|---|---|
| GET_ID 固化 XCPlite 方言、偏离标准布局 | **不成立**——原实现逐字段符合 Table 60 |
| 严重度【高】 | **下调为【中】**（无 wire-level 偏差，不影响与标准 Slave 互操作） |

**否决的修复方向**：按「标准布局」重写解析器（现实现本就是标准布局）。

**审计误判根源**：`include/libxcp/response_parser.hpp` 的 Doxygen 曾声称
「规范 §7.5.1.6 的 LENGTH 为 **WORD** 且偏移不同——实然优先」，
**该陈述与官方规范冲突且属臆造**。该错误注释已实际导致审计做出错误的
高风险判断，故本批次作为 F-03a 一并修正。

> **教训（值得记入方法论）**：错误的注释比没有注释更危险——它会被下游
> 审计/复核当作证据采纳。注释中的规范引用必须可回溯到具体 PDF 页码与表号。

---

## 3. 修改范围

### 3.1 F-03a：注释与头文件更正

| 文件 | 改动 |
|---|---|
| `include/libxcp/response_parser.hpp` | 重写 `GetIdResponse` 的 Doxygen：以 `@verbatim` 嵌入 Table 60 逐字布局；删除「规范 LENGTH 为 WORD / 偏移不同 / 实然优先」三处不实陈述，改以 `@warning` 显式记录该错误及其后果与行号更正（GET_ID 在 PDF 第 123–124 页） |

### 3.2 F-03b：Mode 位域拆分与压缩/加密拒绝

| 文件 | 改动 |
|---|---|
| `include/libxcp/response_parser.hpp` | `GetIdResponse` 新增 4 个字段：`raw_mode`（保留原始字节供诊断）、`compressed_encrypted`、`reserved_bits_clear`、`length_aligned_to_ag` |
| `src/response_parser.cpp` | `ParseGetId` 按 Table 61 拆位：`transfer_mode = raw_mode & 0x01`、`compressed_encrypted = (raw_mode & 0x02) != 0`、`reserved_bits_clear = (raw_mode & 0xFC) == 0` |
| `src/xcp_master.cpp` | `FetchA2lViaUpload` 在编排 UPLOAD 前**先拒绝** `compressed_encrypted`（抛出 `UnsupportedFeature`，因库未实现外部解压/解密算法），再拒绝保留位非零（`MalformedPacket`） |

**缺陷实体（修正前）**：`resp.transfer_mode = res_data[0];` 把整个 Mode 字节存入
`transfer_mode`，按**整字节相等**语义使用。后果：Slave 置 bit 1 时 Mode 为
`0x02`/`0x03`，整字节比较判定失败；且 `COMPRESSED_ENCRYPTED` 信息完全丢失，
**密文会被当作 A2L 文本交给解析器**。

### 3.3 F-03c：Length mod AG 校验

| 文件 | 改动 |
|---|---|
| `include/libxcp/response_parser.hpp` | `ResponseParser` 构造函数新增 AG 形参：`explicit ResponseParser(ByteOrder byte_order, AddressGranularity ag = AddressGranularity::Byte) noexcept;`；新增 `GetAddressGranularity()` |
| `src/response_parser.cpp` | 解析后判定 `length % AgToBytes(ag) == 0`，结果存入 `length_aligned_to_ag`（**解析层只记录，不抛错**，由调用方按上下文决定拒绝方式） |
| `src/command_executor.cpp` | `EnsureCodec` 构造解析器时传入会话 AG |
| `src/xcp_master.cpp` | `FetchA2lViaUpload` 在 `length_aligned_to_ag` 为假时抛 `MalformedPacket`，并给出 AG 字节数与 PDF 依据 |

**不修则后果**：后续 UPLOAD 元素数换算 `Length / AG`（Table 61 原文）会因整除
截断而**少读尾部字节**，且 Slave 本身违反规范。

### 3.4 F-11：`ERR_CMD_BUSY` 重试与可取消总体 deadline

**新增冻结参数结构**（`include/libxcp/command_executor.hpp`）：

```cpp
struct RecoveryPolicy {
    std::chrono::milliseconds busy_retry_delay{100};      // t7 默认
    std::chrono::milliseconds busy_total_timeout{10000};  // 顶层总体 deadline
    bool busy_retry_enabled{true};
    [[nodiscard]] std::string Validate() const;
};
```

> **重要说明**：上述数值是**项目工程默认值**，`100 ms` / `10 s` **不是** XCP V1.5
> 的固定常量（方案 §4.6 第 8 条）。方案 §4.6 规定的冻结值中
> `timeout_retry_limit = 2` 与 `seed_key_transaction_restart_limit = 1` 已分别由
> 既有 `CommandTimeouts::max_retries{2}` 与批次 D 的 `kMaxUnlockSequenceRestarts = 1`
> 承载，本批次不重复引入。有可信 A2L/调用方 t7 时必须覆盖默认值并保留来源。

| 文件 | 改动 |
|---|---|
| `include/libxcp/xcp_error.hpp` / `src/xcp_error.cpp` | 新增 `ErrorCategory::Cancelled` 与 `detail::MakeCancelled(msg, cmd)` |
| `include/libxcp/command_executor.hpp` | 新增 `RecoveryPolicy`；构造函数新增第 5 形参；新增 public `CancelPendingOperation()`、`GetRecoveryPolicy()`；私有 `IsCancelled()`、`IsBusyResponse()`、`WaitBusyDelay()`；成员 `m_recovery_policy_`、`m_cancelled_` |
| `src/command_executor.cpp` | `RunCommand` 循环加入 BUSY 分支（`IsBusyResponse` → `WaitBusyDelay(t7)` → 重发，deadline 自**首次发送前**计时且重试后不重新计时）；`WaitForResponse` / SYNCH 等待循环加入取消检查；`CancelPendingOperation()` 置位并 `notify_all`；Transport 关闭、Disconnect、析构同样置位取消 |
| `include/libxcp/xcp_master.hpp` / `src/xcp_master.cpp` | 新增公开 API `XcpMaster::CancelPendingOperation()`（转发给执行器） |

**设计要点**：

1. **取消必须可中断 `repeat ∞ times`**：规范语义上 BUSY 表示持续等待资源，
   但库实现**不得**形成不可中断的同步调用（方案 §4.6 第 6 条），故提供
   `CancelPendingOperation()`。等待使用条件变量而非 `sleep`，使取消、断连、
   Transport 关闭与析构都能**立即**唤醒。
2. **取消语义独立于超时**：取消抛 `ErrorCategory::Cancelled`，**不得**复用
   `Timeout`/`ProtocolError`——否则调用方会把主动放弃误判为对端故障并触发
   不必要的恢复。
3. **deadline 不因重试而重置**：与 `EV_CMD_PENDING` 重启单轮响应计时不同，
   BUSY 的总体 deadline 自顶层调用首次发送前开始，重试后**不重新计时**，
   防止 `repeat ∞` 因每次重试刷新而实际永不收敛。
4. **可选关闭**：`busy_retry_enabled = false` 时退回旧行为（BUSY 立即作为
   `ProtocolError` 上抛），供已知对端不实现 t7 等待语义的场合使用。

---

## 4. 测试更新与新增

### 4.1 必须更新的固化断言（旧行为回归锁）

| 文件 | 原断言 | 处理 |
|---|---|---|
| `tests/recovery_test.cpp:540-550` | `ErrorDispatch.CmdBusyReportedAsProtocolErrorWithoutRetry` —— 断言 BUSY **不重试**、立即返回 `ProtocolError` | 重写为 `RecoveryBusy.DisabledBusyRetryKeepsImmediateProtocolError`：显式设 `busy_retry_enabled=false` 后仍应立即上抛（保留该路径的回归保护） |

方案 §5.3 明确要求**更新而非保留**该固化用例。修改前实测该用例失败于
`tests/recovery_test.cpp:547`（期望 1 次调用、实测 98 次），失败形态本身即
证明 BUSY 重试已生效。

### 4.2 新增用例

**`tests/response_parser_test.cpp`（F-03，7 个）**：

| 用例 | 验证点 |
|---|---|
| `GetIdLayout.MatchesTable60FieldForField` | 逐字段按 Table 60 偏移解码 |
| `GetIdMode.SplitsTransferModeAndCompressedEncryptedBits` | `0x00`/`0x01`/`0x02`/`0x03` 拆位正确 |
| `GetIdMode.FlagsNonZeroReservedBits` | bit 7..2 非零被标记 |
| `GetIdMode.InlineDataOnlyWhenTransferModeSet` | 内联数据仅在 bit 0 置位时带上 |
| `GetIdLength.ChecksModuloAddressGranularity` | AG=1 全通过；AG=4 时非 4 倍长度被标记 |
| `GetIdLength.RejectsShortFrames` | < 7 字节返回 `nullopt` |
| `GetIdLength.TruncatesToAvailableDataWithoutOverread` | 上报长度大于可用字节时**不越界读**，截断到可用量 |

**`tests/recovery_test.cpp`（F-11，8 个）**：

| 用例 | 验证点 |
|---|---|
| `RecoveryBusy.BusyIsRetriedUntilTotalDeadlineExhausted` | 持续 BUSY → 重试至总体 deadline 后收敛（实测 98 次 @100 ms ≈ 10 s） |
| `RecoveryBusy.DisabledBusyRetryKeepsImmediateProtocolError` | 关闭重试后立即 `ProtocolError`（旧行为保留） |
| `RecoveryBusy.BusyThenSuccessReturnsWithoutWaitingDeadline` | 前 3 次 BUSY 后成功 → **立即**收敛，不白等 deadline |
| `RecoveryBusy.PolicyValidationRejectsIllegalValues` | `Validate()` 拒绝非正数与 `total < delay` |
| `RecoveryCancel.CancelDuringWaitReturnsCancelledWithoutRecovery` | 等待期间取消 → `Cancelled` 且**不触发** SYNCH 恢复 |
| `RecoveryCancel.CancelIsIdempotentAndSafeWhenIdle` | 空闲时重复取消幂等、不抛错 |
| `RecoveryCancel.TransportCloseWakesWaiterAsCancelled` | Transport 关闭唤醒等待者并归一为 `Cancelled` |
| `RecoveryCancel.CancelDuringBusyWaitIsHonoured` | **BUSY 等待期间**取消被响应（不被 `repeat ∞` 吞掉） |

**测试夹具新增能力**（`tests/recovery_test.cpp` 的 `ScriptedSlave`）：
`SetBusyFirstCalls(CommandCode cmd, std::size_t n)` —— 前 n 次调用返回
`ERR_CMD_BUSY`，用于验证「短暂 BUSY 后收敛」与「持续 BUSY 打满 deadline」
两种形态。

---

## 5. 验证结果

### 5.1 全量回归

| 套件 | 结果 |
|---|---|
| `libxcp_tests` | **527 tests / 526 passed / 1 skipped / 0 failed**（本轮一次全绿） |
| `MeasurementAdapterTest` | 16/16 |
| `A2lE2E` | 10/10 |
| `XcpMasterSymbolPolicyTest` | 74/74 |
| `A2lGolden` | 40 passed / 2 skipped（均既有跳过） |
| `XcpliteIntegration`（跨进程真 Slave） | 46 passed / 1 skipped（既有）/ 0 failed；**无孤儿进程残留** |

`libxcp_tests` 用例数由 E2 的 513 增至 **527**（+14：F-03 七例 + F-11 八例 −
被重写的旧例 1 个净增 14）。

> **既有缺陷说明**：`UdpTransportClose.CloseWhileBlockedReturnsPromptly`
> （`tests/udp_transport_test.cpp:868-878`，断言 `< 1000ms` 而轮询周期恰为
> `1000ms`）为**间歇性既有缺陷**，本轮两次全量中一次通过、一次失败；
> `git diff` 确认该文件与依赖均未被本批次触及，按 AGENTS.md 不擅自重构，仅记录。

### 5.2 变异测试（三组，证明新断言有效）

备份：`src/response_parser.cpp`、`src/command_executor.cpp`。
注入标记统一为注释 `// 变异测试临时改动`。

| 变异 | 内容 | 失败位置 |
|---|---|---|
| 1 | Mode 退回整字节（去掉位拆分） | `tests/response_parser_test.cpp:667,669`（`SplitsTransferModeAndCompressedEncryptedBits`）、`:681,683`（`FlagsNonZeroReservedBits`） |
| 2 | `IsBusyResponse` 恒返回 false（禁用 BUSY 识别） | `tests/recovery_test.cpp:589,597,606`（`BusyIsRetriedUntilTotalDeadlineExhausted`）、`:650,655`（`BusyThenSuccessReturnsWithoutWaitingDeadline`） |
| 3 | `IsCancelled` 恒返回 false + BUSY 等待循环去掉取消条件 | `tests/recovery_test.cpp:733`（`CancelDuringWaitReturnsCancelledWithoutRecovery`）、`:782`（`TransportCloseWakesWaiterAsCancelled`）、`:819`（`CancelDuringBusyWaitIsHonoured`） |

三组变异均**精确失败在预期断言行**，且未产生连带失败。恢复后
`grep "变异测试临时改动" src/ include/ tests/` 无输出、`diff` 与备份一致、
全量重建 exit 0。

> 变异 2 中 `DisabledBusyRetryKeepsImmediateProtocolError` 仍通过——这是**正确**
> 的：该用例专测「显式关闭重试」路径，与 `IsBusyResponse` 是否识别无关，
> 说明新旧两条路径各有独立保护。

---

## 6. 未覆盖项与遗留

1. **`ERR_PGM_ACTIVE` 与 `ERR_RES_TEMP_NOT_ACCESSIBLE` 未纳入**：恢复矩阵对
   `ERR_PGM_ACTIVE` 亦规定 `wait t7 → repeat ∞ times`，本批次只实现
   `ERR_CMD_BUSY`。方案 §2C.3 将恢复矩阵增强整体定性为「允许的恢复手段
   清单、未强制 Master 自动执行全部」，故属**可选增强**范围。
2. **`Retry other syntax` / `Retry other parameter` / `Restart session` 未实现**：
   同上，未纳入本轮。
3. **`ERR_CMD_BUSY` 的可取消性未经真 Slave 验证**：XCPlite 在正常路径下不
   返回 `ERR_CMD_BUSY`，当前验证依赖 ScriptedSlave 模拟。
4. **A2L 派生 t7 未接入**：`RecoveryPolicy::busy_retry_delay` 目前只能由调用方
   显式设置，尚未从 A2L 的 `TRANSFER`/事件周期等来源派生。
5. **`CancelPendingOperation()` 与 Transport 关闭的竞态**：已在实现中以
   `m_cancelled_ || m_transport_closed_` 统一归一为 `Cancelled`，但未做
   并发压力测试。
6. **F-03 的 `Range` 与 `Identification type` 组合校验未做**：Table 61 对
   `COMPRESSED_ENCRYPTED` 有「仅 identification type 4 合法」的前提，
   当前实现对其他 IDT 类型的 GET_ID 调用未做该交叉校验。

---

## 7. 批次状态

| 批次 | 内容 | 状态 |
|---|---|---|
| A | 规范裁决与测试 Oracle | 已完成（裁决文档 `code-plan/XCP_V1.5_规范符合性裁决与修复方案.md`） |
| C | F-01 + F-02 + F-04 | 已交付 |
| D | F-09 + F-10 | 已交付 |
| E1 | F-08 → F-06 → F-07 | 已交付 |
| E2 | F-05 + F-12 | 已交付 |
| **F** | **F-03 + F-11** | **本批次已交付** |
| B | GET_VERSION + 版本发现 + V1.3/V1.4 降级回退 | 未实施 |
| G | 文档收口与三平台验收 | 未实施 |

至此 **F-01～F-12 全部十二项已实施完毕**，仅剩批次 B（版本发现，需新增
`GET_VERSION` 命令族）与批次 G（三平台验收收口）。