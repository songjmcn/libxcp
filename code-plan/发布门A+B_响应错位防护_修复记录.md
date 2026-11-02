# 发布门补充：无主响应污染（响应错位）修复记录

> 对应问题：`XcpSlaveDemoIntegrationTest.CrossProcessCalibrationChangesDaqOutput` 在 Windows 上约 25% 概率失败，
> 报 `START_STOP_DAQ_LIST 响应长度不足（RES 数据 0 字节，期望 >= 1）`。
> 该缺陷**不在** `docs/review/` 的发布门清单内，是在批次 3 交付时发现的既有偶发（`git diff master...HEAD -- src/command_executor.cpp src/session.cpp` 为空，非本分支引入）。
> 修复方案经用户确认：「确认按照A+B实施」（C 选项放宽命令超时不纳入本次）。

## 1. 缺陷回顾

### 1.1 现场证据

单独运行该用例 20 次，失败 5 次（25%），错误恒定在 `StartDaqSync()`：

```
tests/xcp_slave_demo_integration_test.cpp(1793): error: Expected: master->StartDaqSync() doesn't throw an exception.
  Actual: it throws calmcar::xcp::XcpException with description
  "START_STOP_DAQ_LIST 响应长度不足（RES 数据 0 字节，期望 >= 1）"
```

命中失败用例的 glog 显示**整条链路错位一格**：

```
XCP response wait started timeout_ms=300
W XCP response wait expired timeout_ms=300
W XCP starting recovery cmd=0xDD retry=1          ← START_STOP_SYNCH 超时
  XCP recovery initialized: response slot cleared
  XCP RX RES pid=0xFF bytes=1 command=unknown     ← 迟到的那条 SYNCH 响应落进刚清空的槽
  XCP RX ERR pid=0xFE bytes=1 command=0xFC
  XCP recovery confirmed by ERR_CMD_SYNCH
```

此后每条命令都是 `response wait started` → 立即 `response slot consumed`，并持续出现
`XCP RX RES ... command=unknown`；到 `StartStopDaqList(0xDE)`（需要 ≥1 字节 FIRST_PID）时，
取到的是上一条命令的 1 字节空 RES，解析出 0 字节数据。

统计相关性：20 次里失败 5 次**全部** `command=unknown` 计数 ≥1；通过的 15 次多为 0。

### 1.2 根因

XCP 的 RES/ERR 不携带事务关联 ID，主站靠"单槽 + 待响应命令"配对，因此任何一条无主响应进入槽位都会污染后续所有命令。

1. **触发**：`START_STOP_SYNCH` 偶发超过 300 ms 无响应（从站确实回了，只是晚了；UDP 无重传，从站线程调度抖动即可触发）。
2. **放大**：`SendSynch()` 在**发送前**清槽，但 `PerformRecovery()` 确认成功后**不再清槽**。恢复窗口内到达的那条迟到 RES 被原样留在槽里，紧随其后的重放命令立刻把它当成自己的应答。
3. **表现**：`CommandExecutor::OnPacketReceived()` 只按 PID 分流 RES/ERR，**不校验"当前是否还有待响应命令"**（日志里的 `command=unknown` 正是此状态），于是无主响应畅通无阻地入槽。

## 2. 修改内容

### 2.1 A：无待响应命令的 RES/ERR 一律丢弃

`src/command_executor.cpp` 的 `OnPacketReceived()`（RES 分支）：

```cpp
if (!m_session_.HasPendingCommand()) {
    // 迟到的响应（当前没有任何待响应命令）必须丢弃：XCP 的 RES/ERR
    // 不带事务关联 ID，一旦把无主响应存进唯一槽位，之后每条命令都会
    // 取到上一条命令的应答，整条链路错位一格，最终表现为某条命令
    // "收到 0 字节数据"之类的畸形包错误。
    control_route = "stale RES discarded; no pending command";
} else if (m_response_ready_) {
    // 槽位已被占用说明协议失步（原逻辑保持不变）
}
```

ERR 分支（在 `ERR_CMD_SYNCH` 判定之后）同样处理，避免被放弃命令的 ERR 污染下一条命令。

`Session::HasPendingCommand()` 内部只取 Session 自己的互斥量，而 Session 从不回调
`CommandExecutor`，锁序单向（`m_mutex_` → Session 锁），不会引入死锁。

### 2.2 B：恢复收尾再清一次槽

`src/command_executor.cpp` 的 `PerformRecovery()`，在 `SendSynch()` 成功后追加：

```cpp
// SYNCH 确认后再清一次响应槽：恢复窗口内到达的 RES/ERR 属于那条已被
// 放弃的命令，若留在槽位里，紧随其后的重放命令会把它当成自己的应答。
{
    const std::lock_guard<std::mutex> lock(m_mutex_);
    m_pending_response_.reset();
    m_response_ready_ = false;
}
LIBXCP_LOG_INFO("XCP recovery finished: stale responses dropped");
```

A 与 B 互补：A 挡住"完全没有待响应命令"的窗口，B 挡住"恢复事务进行中仍有待响应命令（SYNCH 自己）"的窗口——
后者正是本次日志里迟到包落槽的路径，单靠 A 挡不住。

## 3. 新增用例

| 用例 | 文件 | 验证点 |
| --- | --- | --- |
| `StaleResponse.ResponseWithoutPendingCommandIsDiscarded` | `tests/recovery_test.cpp` | 无待响应命令时注入 1 字节 RES 后，下一条 UPLOAD 仍拿到自己的 2 字节数据（只靠 A） |
| `StaleResponse.SyncRecoveryDropsResponseArrivingDuringSynch` | `tests/recovery_test.cpp` | UPLOAD 丢一次响应触发 SYNCH 恢复，在 SYNCH 窗口注入迟到 RES；重放的 UPLOAD 仍拿到 1 字节数据，且 `Count(Synch)==1`、`Count(Upload)==2`（需要 A+B） |

两个用例都用真实 `CommandExecutor` + 脚本化从站走完整命令链路，没有 mock 断言。

## 4. 变异验证

脚本 `.tmp/mutate_stale.py`：分别回退 A / B，重新构建并运行对应用例，结束后一定还原源码。

| 变异 | 用例结果 | 实际报错 |
| --- | --- | --- |
| 回退 A（去掉 `HasPendingCommand` 守卫） | **红（预期）** | `命令 245 响应数据长度 0 不等于期望的 elements*AG = 2` |
| 回退 B（删掉恢复后清槽） | **红（预期）** | `命令 245 响应数据长度 0 不等于期望的 elements*AG = 1` |

还原后重建，两用例均通过。注意两次红分别对应 0xAB/0xCD 与 0x5A 两个场景，说明两条用例各自只依赖自己那处修复。

## 5. 验证结果

### 5.1 原失败用例连跑

| 轮次 | 结果 |
| --- | --- |
| 修复前（基线） | 20 次中失败 **5 次**（25%） |
| 修复后 · 第 1 轮 | 20 次 **0 失败** |
| 修复后 · 第 2 轮 | 20 次 **0 失败** |

命令（Git Bash，`TEMP`/`TMP` 必须用反斜杠，否则 `GetTempPath` 回退到用户目录）：

```bash
cd cmake-build-vs/tests/Release
export TEMP='D:\project\libxcp\.tmp\ci_temp' TMP='D:\project\libxcp\.tmp\ci_temp'
./XcpliteIntegration.exe --gtest_filter='XcpSlaveDemoIntegrationTest.CrossProcessCalibrationChangesDaqOutput'
```

### 5.2 Windows（VS2022 Release，`cmake-build-vs`）

| 项目 | 结果 |
| --- | --- |
| 构建 | exit 0 |
| `libxcp_tests` | 826 tests：825 passed + 1 skipped（既有 `Ag1_Cto8`）、46.2 s |
| `XcpliteIntegration` | 61/61 PASSED、35.2 s |
| `XcpliteV12InteropTest` | 7/7 PASSED、4.5 s |

### 5.3 Ubuntu 24.04（WSL2，GCC 13.3 + Ninja）

| 项目 | 结果 |
| --- | --- |
| 构建 | 通过 |
| `libxcp_tests` | 825 tests：824 passed + 1 skipped、41.1 s |
| `XcpliteIntegration` | 61/61 PASSED、31.6 s |
| `XcpliteV12InteropTest` | 7/7 PASSED、3.7 s |
| ThreadSanitizer 全量 | 825 tests 全跑完、**0 条竞争警告**、41.3 s |

> Linux 比 Windows 少 1 条：一条平台条件编译用例，与本次改动无关。

## 6. 已知遗留

- **触发原因未消除**：`START_STOP_SYNCH` 偶发超过 300 ms 仍是既有现象（用户确认的 C 选项：把测试侧命令超时从 300 ms 放宽到 1500 ms，本次不做）。
  本次修复的是"一次迟到就把后续所有命令带偏"的放大机制；修复后偶发超时只会表现为一次恢复 + 重放，不再导致用例失败。
- 若后续观察到 `XCP RX RES ... command=unknown` 在日志中**依然**高频出现，说明还有别处会清槽或存在真正的重复投递，需要再查。

## 7. 改动文件

| 文件 | 改动 |
| --- | --- |
| `src/command_executor.cpp` | `OnPacketReceived()` 的 RES/ERR 分支加"必须有待响应命令"守卫；`PerformRecovery()` 确认后清槽并记录日志 |
| `tests/recovery_test.cpp` | 新增 `StaleResponse.ResponseWithoutPendingCommandIsDiscarded`、`StaleResponse.SyncRecoveryDropsResponseArrivingDuringSynch` |

## 8. 后续

批次 4：C-01/C-02（ACL 信任判定）、M-01（快照负向用例自 SKIP 导致变异零检出）、M-02（静默对端被判"全部被拒"）。
