# XCP V1.5 批次 D 实施记录（F-09 / F-10）

- 关联方案：`code-plan/XCP_V1.5_规范符合性裁决与修复方案.md`（§2C F-10/F-11 裁决、§5 批次 D）
- 裁决依据：原始规范 `docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.pdf`
  （**不使用** Markdown 转换版，用户裁决 m01451）
- 实施日期：2026-10-21

---

## 1. 规范依据（原始 PDF）

| 项 | 规范原文位置 | 关键内容 |
|---|---|---|
| SHORT_UPLOAD 后 MTA | PDF 第 124 页附近 UPLOAD 小节 | 上传成功后 Slave 将 MTA 设为「上传数据块之后的第一个数据字节」 |
| 分段 GET_SEED | PDF 第 128 页 §7.5.1.9 | `Mode=0` 首段/`Mode=1` 剩余段；无前序 `Mode=0` 直接发 `Mode=1` → `ERR_SEQUENCE` |
| 分段 UNLOCK | PDF 第 130 页 §7.5.1.10 | 首次 UNLOCK 的 Length 必须是 key **总长度**；**Key 在序列完成后才校验** |
| 超时恢复矩阵 | PDF 第 246 页 | GET_SEED / UNLOCK 超时 → `SYNCH` + `repeat 2 times` |
| Pre-Action 列表 | PDF 第 239 页 §7.6.1.2 | 含 `GET_SEED/UNLOCK`、`SET_MTA`、`SET_DAQ_PTR` |

**F-10 的规范空白（本批次的核心判断）**：恢复矩阵对 GET_SEED/UNLOCK **单条命令**超时规定了
`SYNCH + repeat 2 times`，但对「**多段序列中途**某一段超时」留了空白。通用路径
「SYNCH + 重放本端认为的当前段」在该空白下会失步——超时段的 Slave 侧段偏移可能已推进
或复位，重放会错位；而「Key 在序列完成后才校验」意味着**错位不会立即报错**，属静默失步。
规范期望的恢复方式是**重启整个 Seed&Key 序列**（Pre-Action 列表含 `GET_SEED/UNLOCK`）。

---

## 2. 修改范围

### 2.1 F-09 —— SHORT_UPLOAD 后同步本地隐含 MTA

**文件**：`src/command_executor.cpp`（`ExecuteShortUpload`）

命令返回后按 AG 推进地址并写入 `m_last_mta_`，使后续 UPLOAD 超时恢复
（`RestoreUploadMta()`）重放的是**推进后**的地址：

```cpp
const auto advanced = XcpAddress40{address, extension}.Advance(
    number_of_elements, m_session_.GetAddressGranularity());
const std::lock_guard<std::mutex> lock(m_mutex_);
if (advanced.has_value()) {
    m_last_mta_ = *advanced;
} else {
    // 地址推进溢出 32 位：作废本端记录而不是保留陈旧值
    m_last_mta_.reset();
}
```

**修复前**：`m_last_mta_` 只由 `ExecuteSetMta` 更新，SHORT_UPLOAD 不更新 → 若 SHORT_UPLOAD
之后发生 UPLOAD 超时，恢复会把 MTA 拉回 SHORT_UPLOAD 的**起始**地址，链式 UPLOAD 会重复
读取同一块。

### 2.2 F-10 —— 分段 Seed&Key 超时的事务级恢复

**新增私有枚举**（`include/libxcp/command_executor.hpp`）：

```cpp
enum class SeedKeySegment {
    NotSegmented,  ///< 非分段 Seed&Key 命令（GET_SEED Mode=0 属此列）
    NonFinal,      ///< 多段序列的中间段：重放不安全，须完整重启事务
    Final          ///< 末段：结果未知，禁止自动重放
};
```

**新增成员与方法**：
- `m_seed_key_segment_`（段位置）、`m_seed_key_restarts_`（事务级重启计数）
- `SetSeedKeySegment(SeedKeySegment)`、`ClearSeedKeySegment()`（private）
- `ResetSeedKeyTransaction()`（public，事务层收尾用）
- `RunSegmentedRecovery(CommandCode, SeedKeySegment)`（private）

**`RunCommand` 超时分支**：分段命令不再走通用重放路径，改为按段位分派：

```cpp
if (segment != SeedKeySegment::NotSegmented) {
    RunSegmentedRecovery(cmd, segment);
    // 正常返回表示事务已重启，本段由事务层重新编排；不得重放原 encoded_packet
    throw XcpException(ErrorCategory::RecoveryFailed,
                       "分段 Seed&Key 事务已重启，原分段不再重放", cmd,
                       std::nullopt, retries);
}
```

**恢复策略分派**：

| 场景 | 策略 |
|---|---|
| `Final`（末段，GET_SEED/UNLOCK 共用） | 立即抛 `OperationOutcomeUnknown`，**禁止重放**；不发 SYNCH、不重连 |
| `NonFinal` + UNLOCK | 抛 `OperationOutcomeUnknown`；Slave 可能已缓存部分 Key，**须先断开重连**再从 GET_SEED 完整重启 |
| `NonFinal` + GET_SEED | 清段位置（**保留重启计数**）+ `SYNCH`；由事务层从 `Mode=0` 完整重启 |
| 重启预算用尽（>1 次） | 抛 `RecoveryFailed`，不再无限重启 |

**签名变更**：`ExecuteUnlock(length_field, key_segment)` →
`ExecuteUnlock(length_field, key_segment, bool is_final_segment)`，由编排层告知段位置。

**`XcpMaster::Unlock` 重构**（`src/xcp_master.cpp`）：把**整个 Seed&Key 事务**
（2a 首段 GET_SEED → 2b 续段收集 → 2c 算 Key → 2d 分段 UNLOCK）放进重试循环，
使 GET_SEED 段超时也能被捕获并重启：

```cpp
constexpr int kMaxUnlockSequenceRestarts = 1;
for (int sequence_restart = 0;; ++sequence_restart) {
    try {
        /* 2a ~ 2d */
        m_executor_->ResetSeedKeyTransaction();
        return UnlockResult{false, last.resource_protection};
    } catch (const XcpException& e) {
        const bool is_get_seed_timeout =
            e.Category() == ErrorCategory::RecoveryFailed &&
            e.GetCommandCode() == CommandCode::GetSeed;
        if (!is_get_seed_timeout ||
            sequence_restart >= kMaxUnlockSequenceRestarts) {
            m_executor_->ResetSeedKeyTransaction();
            throw;
        }
        // 回到 2a：从 GET_SEED(Mode=0) 完整重启并重算 Key
    }
}
```

> **重构中发现的原实现缺陷**：初版只把 UNLOCK 段放进重试循环，GET_SEED 段的收集
> （原步骤 2–3）在循环**之外**，因此 GET_SEED 段超时的重启永远无法被捕获
> （实测抛 `"分段 Seed&Key 事务已重启，原分段不再重放"`）。本轮把整个事务纳入循环后修复。

### 2.3 重启计数保留（实施中发现的缺陷）

`RunSegmentedRecovery` 初版沿用 `ResetSeedKeyTransaction()`，该函数**同时清零段位置与
重启计数**，导致每次超时都把预算重置为 0，**重启上限形同虚设**（实测用例
`RestartBudgetExhaustedStopsRetrying` 失败：超时后 GetSeed 只调用 2 次、抛出
`MalformedPacket` 而非 `RecoveryFailed`）。

**修正**：拆出只清段位置的 `ClearSeedKeySegment()`，恢复路径改用它，重启计数仅在
事务层收尾（`ResetSeedKeyTransaction`）时清零。

---

## 3. 测试

### 3.1 新增用例

**`tests/seed_key_test.cpp`** —— 新增 `SeedKeyTimeoutRecovery` 套件（5 个用例）：

| 用例 | 验证点 |
|---|---|
| `GetSeedRemainderTimeoutRestartsFromScratch` | 续段超时 → SYNCH 后**必须从 `Mode=0` 重新起手**（断言 SYNCH 后第一条 GET_SEED 为 `{0xF8,0x00,0x01}`），并重算 Key |
| `UnlockFinalSegmentTimeoutIsOutcomeUnknown` | 末段超时 → `OperationOutcomeUnknown`；UNLOCK 仅发送 1 次（**不重放**）；SYNCH 计数为 0 |
| `UnlockMidSegmentTimeoutRequiresReconnect` | 非末段超时 → `OperationOutcomeUnknown`；UNLOCK 仅 2 次（首帧+超时帧）；执行器不发 SYNCH |
| `RestartBudgetExhaustedStopsRetrying` | 连续两次超时 → 第二次抛 `RecoveryFailed`；GET_SEED 恰 4 次、SYNCH ≤2 次、UNLOCK 0 次 |
| `SegmentStateDoesNotLeakAcrossCalls` | 两次成功解锁均以 `Mode=0` 起手（分段状态不跨调用泄漏） |

**测试脚手架扩展**（`QueuedSlave`）：
- `DropNthCall(cmd, n)`：第 n 次调用不产生响应（模拟单次超时）
- `DropCallsFrom(cmd, from)`：自第 from 次起全部不回响应（模拟连续超时）
  —— 单条目 `map` 会互相覆盖，故需两个独立入口

**`tests/recovery_test.cpp`** —— 新增 `ShortUploadMtaSync` 套件（2 个用例）：

| 用例 | 验证点 |
|---|---|
| `AdvanceIsReplayedByUploadRecovery` | SHORT_UPLOAD 读 2 元素（地址 0x1000）后 UPLOAD 超时 → 恢复重放的 SET_MTA 地址为 **0x1002**（推进后），且绝不出现 0x1000 |
| `NoShortUploadLeavesRecoveryMtaUnset` | 对照：经 `ExecuteSetMta(0x2000)` 后恢复重放 0x2000（不凭空造地址） |

### 3.2 变异测试（证明断言有效）

**F-09**：注入 `if (false && advanced.has_value())` 禁用 MTA 同步 →
`ShortUploadMtaSync.AdvanceIsReplayedByUploadRecovery` **精确失败**于
`tests/recovery_test.cpp:855`：`未观察到携带推进后地址 0x1002 的 SET_MTA`。
恢复后重新通过。**断言有效。**

**F-10**：`RestartBudgetExhaustedStopsRetrying` 在修复重启计数保留缺陷前**实际失败**
（抛出 `MalformedPacket`、GetSeed 仅 2 次），修复后通过 —— 属真实缺陷驱动，非事后补测。

### 3.3 全量回归（`cmake-build-logging-release`）

```
[==========] 485 tests from 85 test suites ran. (13552 ms total)
[  PASSED  ] 483 tests.
[  SKIPPED ] 1 test （AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8）
[  FAILED  ] 1 test  （UdpTransportClose.CloseWhileBlockedReturnsPromptly）
```

**唯一失败项为既有缺陷，非本批次引入**：`tests/udp_transport_test.cpp:868-878` 用例自身
边界疏漏 —— 配置 `receive_poll_interval_ms = 1000` 且注释写「最迟在一个轮询周期内退出」，
但断言为 `EXPECT_LT(elapsed.count(), 1000)` 严格小于。连跑 5 次实测 **4 失败 1 通过**
（唯一通过那次耗时 1016 ms，反而超过阈值），计时方式本身不稳定。`git diff --name-only`
确认该文件与依赖均未被本批次触及。

### 3.4 构建

`cmake --build cmake-build-logging-release -j 4` 全量构建 **exit 0**，无 error。

---

## 4. 未覆盖 / 待确认项

1. **F-10 的真实对端验证**：本批次仅用脚本 Slave（`QueuedSlave`）验证命令时序与安全属性，
   **未在 XCPlite 真实对端上制造分段超时**。XCPlite 的 Seed&Key 分段行为（是否支持多段
   key、超时后段偏移如何复位）未实测。
2. **F-10 与 F-11 的边界**：本批次只做「拒绝盲重放」这一安全属性，`ERR_CMD_BUSY → wait t7
   → repeat ∞` 等恢复矩阵条目属**批次 F（F-11 选 B）**，未在本批次实施。
3. **`ExecuteUnlock` 签名变更影响面**：已同步更新 `tests/logging_probe.cpp:109`、
   `tests/seed_key_test.cpp:213,231` 三处调用点；确认无其它调用者（`grep` 全仓核实）。
4. **F-09 的 MTA 溢出分支**（`m_last_mta_.reset()`）未被用例覆盖 —— 需 32 位地址 +
   大元素数推进溢出，构造成本较高，本轮以代码注释说明语义。
5. **XCPlite 集成套件（`XcpliteIntegration`）**：本批次改动集中在 Seed&Key 与 MTA 同步，
   未跑该套件；批次 C 已建立其基础设施修复，建议在批次 E1 一并回归。

---

## 5. 与方案 §5 批次划分的一致性

方案 §5.1 建议「F-10 与 F-09 合批（同在 `src/command_executor.cpp`）」，本批次**已按此执行**。
批次划分现状：

| 批次 | 内容 | 状态 |
|---|---|---|
| C | F-01 + F-02 + F-04 | 已完成 |
| **D** | **F-09 + F-10** | **本批次已完成** |
| E1 | F-08 → F-06 → F-07（顺序不可颠倒） | 未开始 |
| E2 | F-05 + F-12 | 未开始 |
| F | F-03 + F-11（选 B：实现 `ERR_CMD_BUSY` 重试） | 未开始 |
| B | GET_VERSION + 版本解析 + 降级回退 | 未开始 |