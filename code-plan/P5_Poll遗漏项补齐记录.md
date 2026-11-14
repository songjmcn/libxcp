# P5：Poll 计划遗漏项补齐实施记录

- 日期：2026-11-14
- 依据：`code-plan/Poll模式多Event架构实施计划.md` §3.4、§3.5、§8.2、§8.3、§8.4、§11、§11.1
- 触发：用户"检查是否还有遗漏项？"后逐条比对，确认 8 处缺口/不一致；用户选择"全部补齐"
- 性质：**补齐与对齐**，不新增功能面

---

## 1. 范围

| # | 缺口 | 依据 | 处理 |
|---|------|------|------|
| 1 | 计划 §8.2 的 4 个用例缺失 | §8.2 | 补 4 例（另把 `ReadFailure...` 升级为 `SingleVariableFailureDoesNotAbortTick`） |
| 2 | `PollSample` 无 `t_end` | §3.4 "t_begin / t_end 都保留" | 新增 `steady_time_end` |
| 3 | 未做 clean 全量构建 | §8.4、`AGENTS.md` | `cmake --build cmake-build-release --target clean` 后完整重建 |
| 4 | 只标定 UDP，未测 TCP | §8.3 "同时覆盖 UDP 与 TCP" | 新增 TCP 标定用例 + 结果 |
| 5 | DAQ 互斥判据用 `DaqLedger()` 而非计划写的 `DaqConfigGeneration()` | §3.5 | 保留实现，计划 §3.5 加实施说明 |
| 6 | `Start()` 未调用 `Validate()` | §11 决策 4 | 保留实现，计划决策 4 改为准确表述 |
| 7 | 计划 §11.1 仍写 `skipped_overruns == 0` | §11.1 | 计划改为比例判据并写明修正理由 |
| 8 | 错误隔离用例只有单个符号 | §8.2 | 见 #1（并入 `SingleVariableFailureDoesNotAbortTick`） |

---

## 2. 代码变更

### 2.1 `include/libxcp/poll/poll_types.hpp`

`PollSample` 新增 `std::chrono::steady_clock::time_point steady_time_end;`，
紧跟 `steady_time`。语义：一次 tick 先记 `t_begin`（`steady_time`），读完所有
分组后再记 `t_end`（`steady_time_end`），真实采样时刻落在两者之间。

### 2.2 `src/poll/poll_session.cpp`

`RunTick()` 在回调**之前**追加：

```cpp
sample.samples = std::move(results);
// 单次 tick 的真实采样时刻落在 [steady_time, steady_time_end] 区间内，
// 只保留起点是保守但不精确的做法，故两者都记录（实施计划 §3.4）。
sample.steady_time_end = std::chrono::steady_clock::now();
```

### 2.3 `tests/poll_session_test.cpp`

从机 `PollSlave` 增加两个可控钩子：

- `std::atomic<std::uint8_t> session_status{0x00}`：`GET_STATUS` 的
  Current Session Status 字节，用于构造 `DAQ_RUNNING`（bit6=0x40）
- `std::atomic<Address> fail_address{0xFFFFFFFFU}`：`SHORT_UPLOAD` 范围覆盖该
  地址时回 `ERR_OUT_OF_RANGE`，其余照常返回——用于验证"只坏一个变量"

新增/改写 5 个用例：

| 用例 | 断言 |
|------|------|
| `SingleVariableFailureDoesNotAbortTick`（原名 `ReadFailureMarksVariableInvalidWithoutStopping`） | 两符号；`samples[0].valid==false` 且 error 非空，`samples[1].valid==true` 且值 == 0x20；`read_errors>=1` |
| `MultipleEventsKeepIndependentPeriods` | 20 ms + 50 ms 双 event 跑 800 ms：`fast >= 10`、`slow >= 4`、`fast >= slow*2`；slow 相邻间隔落在 [40, 75] ms |
| `OverrunIsSkippedNotCatchUp` | 回调内睡 60 ms：`skipped_overruns > 0`，且 400 ms 内 `ticks` 落在 [3, 10]（若补读会逼近 20） |
| `StartRejectedWhenDaqRunning` | `session_status=0x40` 时 `Start` 抛 `XcpException`，`IsRunning()==false` |
| `TimestampIsMonotonicPerEvent` | 同 event `steady_time` 严格递增；每帧 `steady_time_end >= steady_time` |

### 2.4 `tests/poll_capacity_calibration.cpp`

- 抽出 `struct CalibrationResult{max_variables, saturated}` 与
  `CalibrationResult CalibrateCapacity(XcpMaster&, const IMeasurementDatabase&,
  std::chrono::milliseconds)`——倍增找上界 + 区间二分的公共流程，UDP/TCP 共用
  同一判据
- UDP 用例（`XcpliteSlaveTest.PollCapacityAtTwentyMillisecondPeriod`）改调该函数，
  断言由 `EXPECT_GT(last_pass, 1U)` 收紧为 `ASSERT_GT(max_variables, 1U)`
- 新增夹具 `PollCapacityTcpTest`：`XcpliteSlaveProcess{std::nullopt, 0, true}` +
  `TcpTransport`（`TcpWireProfile::AsamV14`、`PacketAlignment::Bit8`、超时 3 s）
  与用例 `PollCapacityAtTwentyMillisecondPeriodOverTcp`，
  `RecordProperty("max_variables_at_20ms_tcp")`

### 2.5 计划文档同步（`code-plan/Poll模式多Event架构实施计划.md`）

- §3.5：判据改为 `DaqLedger()` + `GET_STATUS.DAQ_RUNNING`，并写明为何不用
  `DaqConfigGeneration()`（只增不减，"配置过又 CLEAR"时会误杀）
- §10 边界 4：补"采样时刻是区间不是点，`steady_time` / `steady_time_end`"
- §11 决策 4：改为"`Start()` 内部逐 event 调 `ResolveEvent`（与 `Validate()` 同一
  解析路径）做兜底"
- §11.1 判据：改为超期比例 ≤1%，并写入修正理由
- §8.3：补 P4 落地说明（两个标定用例、共用流程）

---

## 3. 设计决策

1. **`steady_time_end` 而非 `t_begin`/`t_end` 命名**：与既有 `steady_time`
   （墙钟对应 `system_time`）保持同族命名，避免出现两套时间概念。
   `system_time` 不再补 `system_time_end`——墙钟只用于与外部日志对齐，
   区间精度需求由单调时钟承担，多一个字段只会让调用方选错。
2. **错误隔离用例用"地址注入失败"而非"全局失败开关"**：相邻两符号会被
   P3 合并成一次读，全局开关会让整组失败、两变量都 invalid，反倒验不出隔离。
   按地址注入才能构造"组读失败 → 回退逐变量读 → 只有一个坏"的真实路径。
3. **上界改 75 ms 而不是继续收紧下限**：clean 后首次全量出现
   `actual: 61 vs 60`（3 次），与本记录 §9 的 P4 数据同源——非实时 Windows 上
   单次 tick 抖动可达 +12 ms（20 ms 周期最坏间隔 32 ms）。断言应表达
   "周期独立"而不是"OS 是实时的"，故放宽上界并注明理由。
4. **TCP 标定独立夹具而非扩展 `XcpliteSlaveTest`**：后者的 `MakeMaster()` 固定
   构造 `UdpTransport`；改它会波及既有 60+ 个互操作用例。新建夹具只多十几行，
   却把"传输方式"这个变量隔离在用例内部。
5. **TCP 结论只记录不推广**：92 vs 65 的差异有两种互斥解释（UDP 用户态轮询开销 /
   本轮系统负载更低），单次标定无法分离，按 §11.1 "不写死数字"的口径只登记
   当次事实。

---

## 4. 实测（clean 全量构建）

构建：`cmake --build cmake-build-release --target clean` 后 `-j 4` 完整重建，
`EXIT=0`，本仓库源码 0 告警。

| 套件 | 结果 |
|------|------|
| `libxcp_tests` | **1001 例 / 1000 通过 / 1 跳过 / 0 失败 / 47093 ms** |
| `PollSessionSkeleton` | 19/19（1438 ms） |
| `PollSessionAddressMerge` | 4/4（4 ms） |
| `XcpliteIntegration`（不含标定） | **61/61 通过 / 36054 ms** |
| UDP 标定 | 通过，结论 **65**（140412 ms） |
| TCP 标定 | 通过，结论 **92**（140583 ms） |

跳过的仍是 `AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`，
B0 之前即存在。

---

## 5. 边界与已知限制

1. TCP 标定同样只覆盖单 event、20 ms、两两不相邻的最坏情况，多 event 叠加时的
   总预算未标定（计划 §10 未要求，但实际部署应留意：20 + 50 ms 两 event 的
   合计变量数不等于各自容量之和）
2. `OverrunIsSkippedNotCatchUp` 用回调 sleep 制造超期，钉的是"跳过不补读"
   这一行为；若未来引入补偿策略，该用例会失败——这是预期的护栏
3. 时间戳仍是主站本地时钟，`steady_time_end` 只缩小不确定性区间，不等于 ECU 采样时刻
4. 两个标定用例合计约 281 s，仍只在 `XcpliteIntegration` 手动运行，不进入常规回归
5. `%TEMP%` 权限问题依旧存在：本机直接跑 `XcpliteIntegration` 时 11 个
   `XcpSlaveDemoIntegrationTest` 会因 `create_directories: Access is denied` 失败，
   必须把 `TEMP`/`TMP` 指向工作区目录（见 P4 记录）

---

## 6. 自审

- [x] 8 项逐条有落点，无"记在脑子里"的
- [x] `PollSample` 新字段有中文 doxygen 注释
- [x] 代码段（`RunTick` 追加处）有中文注释说明动机
- [x] 新用例名与计划 §8.2 的表述一一对应，便于审计
- [x] 判据偏离（比例而非零超期）在计划与记录**两处**都有说明
- [x] clean 构建 + 全量回归的真实数字已记录，未引用旧数字
- [x] 临时文件（`.tmp-build-tests.bat`、`p5_*.log`、`.tmp_probe`）已清理