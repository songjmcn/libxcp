# DAQ 帧率门禁与诊断增强 — 实施记录

- 日期：2026-10-30
- 分支：`feat/tcp-a2l-wire-evidence`
- 前置：`code-plan/DAQ帧率遗留问题_分析与macOS复测方案.md`、
  `code-plan/DAQ帧率遗留问题_macOS真机复测记录.md`
- 用户批准（2026-10-30）：同意复测记录 §7 提出的三条建议

---

## 0. 结论速览

| 建议 | 实施结果 |
| --- | --- |
| ① 给 DAQ 用例补帧率下限断言 | ✅ 已加，阈值取理论值的 50%（3 s ≥150 帧 / 1 s ≥5 帧） |
| ② 从站心跳改走 stdout | ✅ 已加，`LIBXCP_ENABLE_LOG=OFF` 时 CI 工件仍能看到事件计数 |
| ③ `Diagnostics()` 增加时间戳差值分布 | ✅ 已加，且按 32 位模减法处理回绕 |

改动只涉及 2 个文件、不涉及任何库代码与 ABI：

| 文件 | 变化 |
| --- | --- |
| `tests/xcp_slave_demo_integration_test.cpp` | +`CountFramesOver()`、+`AppendTimestampDeltaSummary()`、两条用例加帧率断言、2 条诊断文本锁定断言 |
| `examples/xcp_slave_demo/main.cpp` | 心跳额外写一行到 stdout |

## 1. ① 帧率下限断言

### 1.1 为什么原来抓不到退化

`CrossProcessCalibrationChangesDaqOutput` 此前只断言「等到了期望值 42.0」。
复测已确认：macOS CI 上曾出现 3 s 只收 89 帧，但因期望值对丢帧鲁棒，
用例依旧算通过——**帧率退化不会让构建变红**。

### 1.2 新增的计量原语

`DemoDtoCollector::CountFramesOver(std::chrono::milliseconds window)`
（`tests/xcp_slave_demo_integration_test.cpp:908`）：在固定窗口内统计新到达的
DTO 帧数，阻塞到窗口结束。之所以不直接用「先记帧数、sleep、再记帧数」，是为了
让收集器在等待期间继续被 `condition_variable` 唤醒并保持缓存一致性。

### 1.3 断言与阈值

| 位置 | 事件周期 | 窗口 | 理论上限 | 阈值（50%） |
| --- | --- | --- | --- | --- |
| `:1694` `kMinFastDaqFrames` | 10 ms | 3 s | 300 帧 | **150 帧** |
| `:1723` `kMinSlowDaqFrames` | 100 ms | 1 s | 10 帧 | **5 帧** |

阈值取 50% 的理由：复测实测本机 98.3~99.4 Hz（接近理论上限），而 CI 曾退化到
29.7%；50% 阈值与两者都有足够距离，能抓住数量级退化又不会因调度抖动误报。
后续若拿到 CI 的真实分布，可再收紧（见 §5）。

快速段改动后顺序为：

1. `StartDaqSync()`
2. `CountFramesOver(3 s)` → 断言 ≥150 帧
3. `WaitForDouble(42.0, 500 ms)` → 数值仍要正确（帧已缓存，命中很快）

慢速段同理：`CountFramesOver(1 s)` → 断言 ≥5 帧，再 `WaitForNonzeroUint32(2 s)`。

### 1.4 反向验证（阈值故意调大）

把 `kMinFastDaqFrames` 临时改成 `1000` 后重跑，断言如期失败并输出：

```
3 s 窗口只收到 299 帧（10 ms 事件理论上限 300，阈值 1000），DAQ 帧率退化；
事件通道 0，已收 DTO 299 帧，等待期解码成功 0 次，解码失败 0 次，
首帧长度 16 字节，内容 00 aa 00 00 56 11 be 56 ...，
时间戳差值 min=7634417 中位=9992625 均值=9998129 max=12375916（296 帧样本）
```

验证完成已恢复为 150。

## 2. ② 从站心跳写 stdout

`examples/xcp_slave_demo/main.cpp:928`（日志调用仍在 `:926` 保留，
既有用例 `ServiceEmitsPeriodicHeartbeatLog` 断言的是日志文本，不能删）：

```
[从站心跳] 累计快事件次数=501 已连接=0 DAQ运行中=0
```

要点：

- 用 `'\n' << std::flush` 而非 `std::endl`：从站在 `kill -INT` 后要立刻可见，
  但不能引入无谓的二次 flush（心跳 5 s 一次，可接受）。
- 保留 `LIBXCP_LOG_INFO` 是刻意的：日志带 glog 时间戳与线程号，便于事后对齐；
  stdout 则在 `LIBXCP_ENABLE_LOG=OFF`（CI 现场）时也能被 CTest 收进工件。
- 已验证：默认构建（无 `LIBXCP_ENABLE_LOG`）启动从站 7 s，stdout 出现
  `[从站心跳] 累计快事件次数=501`。

有了它，CI 上「帧数偏低」可以直接对照：心跳里的事件计数增量 ≈ 收帧数 →
**从站没降频**；心跳增量也低 → **从站被饿死**（结论 A）。

## 3. ③ 时间戳差值分布

`DemoDtoCollector::AppendTimestampDeltaSummary()`
（`tests/xcp_slave_demo_integration_test.cpp:988`），由 `Diagnostics()`（`:971`）调用，
输出形如：

```
时间戳差值 min=7634417 中位=9992625 均值=9998129 max=12375916（296 帧样本）
```

判读约定（写进代码注释）：

- 差值**恒为事件周期整数倍**（这里 10 ms ≈ 1e7 个计数）→ 帧是"被丢弃"的（结论 B）；
- 差值**抖动且多数不是整数倍**（同时最小间隔明显大于周期）→ 帧**根本没产生**
  （结论 A，复测里后台 QoS 场景即为此形态）。

### 3.1 实现中修掉的一个真实缺陷

首版按「后一帧 ≥ 前一帧则相减，否则记 0」处理回绕。反向验证暴露出问题：
32 位 ns 计数约 4.29 s 回绕一次，输出变成 `min=0 中位=9996000`，最小值 0 会被
误读成"存在零间隔＝重复帧"。

修法：按布局已知位宽做模减法（`:1008-1021`）：

```cpp
const std::uint64_t delta =
    layout_.timestamp_size_bits == 32U
        ? static_cast<std::uint64_t>(static_cast<std::uint32_t>(stamps[index]) -
                                     static_cast<std::uint32_t>(stamps[index - 1U]))
        : (stamps[index] >= stamps[index - 1U] ? stamps[index] - stamps[index - 1U] : 0U);
```

修后同一场景输出 `min=7634417 中位=9992625`，回绕不再污染统计。

### 3.2 措辞修正

`Diagnostics()` 里的 `解码成功 N 次` 改为 `等待期解码成功 N 次`：该计数只统计
`WaitFor*` 内部的解码尝试，而 `CountFramesOver` 期间不解码，直接看"解码成功 0 次"
容易被误读成"全部解码失败"。时间戳统计的帧数另行以"（N 帧样本）"给出。

## 4. 新增/修改的测试断言

`DemoDtoCollectorDiagnosticsCounters`（`:1054` 起）新增 3 条锁定断言：

| 断言 | 作用 |
| --- | --- |
| 两帧时间戳相差 10000 → 文本含 `时间戳差值 min=10000` | 锁定差值统计真的在算 |
| 同上 → 文本含 `（2 帧样本）` | 锁定样本数披露 |
| 截断帧场景 → 文本含 `时间戳差值：样本不足` | 避免"没样本"被误读成"差值 0 = 不丢帧" |

## 5. 验证结果

| 项 | 结果 |
| --- | --- |
| `cmake --build cmake-build-release -j 8` | 成功（无 error） |
| `XcpliteIntegration --gtest_filter='XcpSlaveDemoIntegrationTest.*'` | **12/12 通过**（15.77 s） |
| 目标用例 `CrossProcessCalibrationChangesDaqOutput` 单独 | 通过，4.44 s（原 3 s 纯等待 → 现 4 s 采集窗口） |
| `DemoDtoCollectorDiagnosticsCounters` | 通过（含 3 条新断言） |
| 阈值反向验证 | 如期失败并输出完整诊断（§1.4） |
| 全量 `ctest` | 见 §5.1 |

### 5.1 全量回归

```bash
ctest --test-dir cmake-build-release --timeout 600
```

| 指标 | 结果 |
| --- | --- |
| 用例总数 | 1159（含 1 项 `Disabled`） |
| 通过 | **1158 / 1158（100%）** |
| 失败 | **0** |
| 总耗时 | **341.82 s**（改动前 336.79 s，增加约 5 s：新增 3 s + 1 s 采集窗口） |
| 未运行 | 4 项，均预期（1 Disabled + 3 Skipped） |

格式检查：`clang-format --dry-run` 对两个改动文件均报 **0 处漂移**。

### 5.2 一次环境性假失败（记录以防误判）

首次跑全量时有 5 个 Example 用例报失败（`ExampleXcpMasterUdp`、`ExampleMeasurementDemo`、
`ExampleCppDemoMeasurement`、`ExampleCppDemoCalibration`、`ExampleXcpIntegrationDemoCppDemo`），
日志显示：

```
[env] 端口 5555 已被占用（疑似已有 XCP slave 在跑），请先停止
[env] 端口 5555 已有 XCP 应答（残留 Slave？）——拒绝在脏端口上取证
```

根因是复测阶段启动的 `xcp_slave_demo --bind 127.0.0.1 --port 5555` 残留（`kill -INT`
后进程未及时退出）。清理后 `ctest -R "^Example"` → **14/14 通过**，全量随即全绿。

**教训**：跑 Examples / 全量前必须确认 5555 端口无残留从站；
`pgrep -fl "xcp_slave_demo|xcp_test_slave"` 应无输出。

## 6. 未做 / 后续

1. **阈值尚未用 CI 实测校准**：150 帧/5 帧是按"理论上限的 50%"取的。等三平台 CI
   跑出真实分布（尤其 macOS 3~4 核 runner）后应收紧到"实测下界 × 0.7"量级。
2. **未做 `SO_RCVBUF` / `SO_RXQ_OVFL`**：只在真出现结论 B 时才需要（分析文档 §10）。
3. **未把心跳纳入断言**：心跳目前只是证据输出，没有"心跳增量必须 ≥N"的断言。
   若将来要防"从站被饿死"回归，应在 CI 侧解析 stdout 做趋势检查，而非在单测里断言。

## 7. 回滚

```bash
git checkout -- tests/xcp_slave_demo_integration_test.cpp examples/xcp_slave_demo/main.cpp
```

回滚后帧率退化将重新变为"不可见"（用例仍绿），请仅在确认误报时回滚。

---

# 第二轮（2026-10-31）：CI 实测定性与门禁调整

## 8. 触发

提交 `0c53de8` 推送后，macOS job 失败：

```
879 - XcpSlaveDemoIntegrationTest.CrossProcessCalibrationChangesDaqOutput (Failed)
Expected: (fast_frames) >= (kMinFastDaqFrames), actual: 99 vs 150
```

全量 1150 项中**只有这 1 项失败**（其余含慢速段、`WaitForDouble(42.0)` 全部通过）。

## 9. 日志实测数据

窗口 3 s（日志取自用户导出的 `logs_111352118587.zip`）：

| 指标 | CI 实测 | 折算成"事件周期倍数" | 本机零丢帧基线（299 帧） |
| --- | --- | --- | --- |
| 收帧数 | 99（33.0 Hz） | — | 299（99.7 Hz） |
| 差值 min | 1,000 ns（1 µs） | ×0.00 | 7.63 ms（×0.76） |
| 差值中位 | 19,625,416 ns | **×1.96** | 9.99 ms（×1.00） |
| 差值均值 | 29,736,811 ns | **×2.98** | 10.00 ms（×1.00） |
| 差值 max | 104,802,834 ns | **×10.49** | 12.38 ms（×1.24） |

从站时钟：`CLOCK_TICKS_PER_S = 1000000000`、`resolution = 42ns`，故时间戳单位是 1 ns。

### 9.1 判读

1. **均值 ×2.98 ≈ 3**：与"每 3 个事件收到 1 个"（成功 1/3）高度一致；
   中位 ×1.96 ≈ 2 说明最常见的是"丢 1 帧"；max ×10.49 说明出现过最长 10 个周期
   的空档。三者构成典型几何分布形状。
2. **慢速段（100 ms 事件）通过**：1 s 窗口 ≥5 帧。若链路均匀丢 2/3，慢速段只会有
   ~3.3 帧而必然失败。故**丢帧与事件频率相关**（高频积压、低频不积压），
   不是与频率无关的链路丢包。
3. **min = 1 µs 是本轮唯一无法用"丢帧模型"解释的点**：两帧时间戳几乎相同，意味着
   它们在同一时刻被读取/发出（怀疑从站挂起后恢复时批量 flush），本机基线的
   min 是 7.63 ms，不存在此现象。

结论：主因是**从站侧限流/发送队列溢出**（A 的变体：生产者不慢，但"生产者→队列"
这一段在 CI 的 3 核配额下撑不住 100 Hz），**不是网络链路丢包（B）**。

## 10. 本轮改动（用户裁决：阈值 60 + 直方图 + 心跳 1 s 自证）

### 10.1 阈值调整

`tests/xcp_slave_demo_integration_test.cpp:1721` `kMinFastDaqFrames`：150 → **60**（20 Hz）。
理由：macOS 3 核 runner 上历史两次实测分别是 89 帧（29.7 Hz）与 99 帧（33.0 Hz），
150 帧（50 Hz）在该环境长期误报；60 帧只拦"灾难级塌陷"，帧率正常性由自证通道解释。

### 10.2 从站自证通道（新增，比读 stdout 更硬）

在帧率窗口前后各 POLL 一次 `meas_slow.statistics.fast_cycle_count`
（= 从站快循环序号 `tick`，`examples/xcp_slave_demo/slave_model.cpp:127` 写入），
得到从站真实事件速率 `slave_hz`，与主站收帧率 `master_hz` 一起写进断言消息：

```cpp
EXPECT_GE(fast_frames, kMinFastDaqFrames)
    << "…从站快循环速率 " << slave_hz << " Hz，主站收帧率 " << master_hz
    << " Hz → " << verdict << "；" << collector.Diagnostics();
```

`verdict` 三分支：

| 条件 | 判读 |
| --- | --- |
| `slave_hz < 60` | 从站自身被限流（结论 A） |
| `slave_hz ≥ 60` 且 `master_hz < slave_hz × 0.8` | 从站正常、帧被丢弃（结论 B） |
| 其余 | 主站收帧率与从站速率相当，未丢帧（失败来自阈值与环境不匹配） |

它跨平台一致（走 POLL，不依赖 stdout 捕获），也不受管道缓冲/Windows 重定向影响。

### 10.3 差值直方图与前 10 个差值

`AppendTimestampDeltaSummary()`（`:1016` 起）在原有 min/中位/均值/max 之后追加：

```
差值直方图(<5ms/5-15/15-25/25-45/45-85/≥85)=0/314/0/0/0/0
前 10 个差值(ns)=9268458,10992834,9749375,9413833,10806083,9790542,10050208,9986959,9421833,10732125
```

分桶用于区分「单峰集中在 1 周期桶」（偶发丢弃）与「跨多桶且各桶显著」（间隔被随机
拉长）；"前 10 个原始差值"用于暴露均值/中位掩盖的异常点（例如本轮 CI 上的 1 µs）。

### 10.4 心跳间隔 5 s → 1 s

`examples/xcp_slave_demo/main.cpp:58` `kHeartbeatInterval{1}`。集成用例的子进程通常
只活 4~5 s，5 s 间隔下 CI 日志里往往一条心跳都没有；1 s 间隔能在同一窗口留下 3~4 条
`[从站心跳] 累计快事件次数=N`，用增量即可复核从站速率（与 §10.2 的 POLL 读数互相印证）。

## 11. 验证

### 11.1 本机反向验证（阈值临时改 1000）

```
3 s 窗口只收到 298 帧（10 ms 事件理论上限 300，阈值 1000），DAQ 帧率退化；事件通道 0；
从站快循环速率 103.20415324513856 Hz，主站收帧率 99.209153764681574 Hz
→ 主站收帧率与从站速率相当，未丢帧（失败来自阈值与环境不匹配）；
…时间戳差值 min=7882333 中位=9987416 均值=9999969 max=12452917（315 帧样本），
差值直方图(<5ms/5-15/15-25/25-45/45-85/≥85)=0/314/0/0/0/0，
前 10 个差值(ns)=9268458,10992834,9749375,…
```

三条新能力（自证速率、直方图、前 10 差值）全部如期输出，且第三分支判读正确
（本机未丢帧，故不误判为 A 或 B）。验证后阈值恢复 60。

### 11.2 心跳 1 s 生效

```
[从站心跳] 累计快事件次数=101 已连接=1 DAQ运行中=1
[从站心跳] 累计快事件次数=201 已连接=1 DAQ运行中=1
```

每次 +100，与 100 Hz 快循环一致；用例结束前能留下 4 条。

### 11.3 回归

| 项 | 结果 |
| --- | --- |
| `XcpSlaveDemoIntegrationTest.*` | 12/12 通过（15.94 s） |
| 全量 `ctest --test-dir cmake-build-release --timeout 600` | **1158/1158 通过（100%），0 失败，338.97 s**；总数 1159（含 1 项 `Disabled`），另 2 项 `Skipped` 为既有预期 |
| 格式检查 `clang-format --dry-run` | 两个改动文件均 **0 处漂移** |

## 12. 遗留

1. **1 µs 异常仍未定性**：下一轮若 CI 再失败，直方图 + 前 10 差值可确认它是否来自
   "挂起后批量 flush"。若确认，则说明 CI 上从站会被整段挂起（而非匀速降频），
   阈值 60 是否仍合理需要再评估。
2. **未做从站队列丢帧计数上报**：XCPlite 的 `lost` 计数在 Release 下不可见；
   若要根治，应让从站在心跳里附带"队列溢出丢弃数"，这是定位"队列溢出 vs 网络丢包"
   的最终证据（需修改 XCPlite 或改用其 debug 打印）。
