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
