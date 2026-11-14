# P-E：容量标定接入阶段分解 实施记录

> 任务：`perf-calibration-integration`（性能统计方案 P-E 期）
> 关联计划：`code-plan/性能统计_滑动窗口与分阶段耗时_实施计划.md`
> 日期：2026-11-14

## 1. 问题与范围

`tests/poll_capacity_calibration.cpp` 此前只报"多少个变量还能跑得动"，不报"为什么跑不动"。
容量上限被判定为 FAIL 时，使用者只能看到 `skip` 次数上升，无法判断瓶颈是：

- 每变量一次往返（协议/链路开销）
- A2L 换算层（`ToPhysical` / `ToPhysicalElement`）
- 合并读组是否生效

本次把 P-B 已经做好的 poll 阶段分解接进容量报告，让每一条 `PrintRun()` 输出后面紧跟一行阶段分解。

**不在本次范围**：改变容量判定阈值、改变探针布局、把标定结果写入文档表格。

## 2. 改动内容

文件：`tests/poll_capacity_calibration.cpp`（唯一改动文件）

| # | 位置 | 改动 |
| --- | --- | --- |
| 1 | 文件头 | `#include "libxcp/detail/perf_window.hpp"` |
| 2 | `struct CapacityRun` | 增加 `PollPerfSnapshot perf{};`（本轮的性能窗口快照） |
| 3 | `RunOne()` | 构造 `PollSession` 后设置 `PerfConfig`：`enabled = true`、`window = 运行窗口 + 60 s`、`log_to_logger = false` |
| 4 | `RunOne()` | `session.Stop()` 之后取 `run.perf = session.PerfSnapshot();` |
| 5 | 新增 `PrintRunPerf()` | 单行打印阶段分解（含占比、合并率、字节量） |
| 6 | `PrintRun()` 末尾 | 调用 `PrintRunPerf(run)` |

另外修正文件头注释里过期的示例命令：`--gtest_filter='XcplitePollCapacityTest.*'` → `--gtest_filter='*PollCapacity*'`。真实套件名是 `XcpliteSlaveTest` 与 `PollCapacityTcpTest`，旧 filter 一个都匹配不到。

## 3. 设计决策

**D1：把窗口设成「运行窗口 + 60 s」，而不是默认 5 s。**
标定是"跑 N 秒然后看总账"，中途刷窗口日志会污染 stdout 且把一次运行拆成多段。
窗口远大于运行时长，等于"这一轮只出一个快照"，且不会在 `Stop()` 之后还残留未结算的窗口。

**D2：`log_to_logger = false`。**
标定输出走 `std::cout` 的表格，性能日志走 glog；在标定场景里把两者混在一起没有价值，日志一条也不要。

**D3：快照直接按 `window.stages[size_t(PerfStage::X)]` 取，不调 `Stage()`。**
`PerfWindowSnapshot::Stage()` 是非 const 的查找辅助，且这里需要按名字逐个取，用下标更直观、也避免依赖签名细节。

**D4：占比以 `TickTotal.sum_ms` 为分母；`merged` 用 `1 - groups/variables`。**
与 poll 窗口日志（`FlushPerfWindow()`）保持同一口径，避免两条路径的数字对不上。

## 4. 实测数据

环境：Windows + XCPlite 本机 TCP 回环，`LIBXCP_POLL_CALIBRATION_WINDOW_MS=1200`，
`--gtest_filter='*PollCapacity*'` → **2 tests PASSED，34810 ms**。

### 4.1 阶段分解（TCP，倍增段）

| N | 结果 | tick mean | p95 | max | group_read | convert | merged | bytes/tick |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | PASS | 0.547 ms | 0.954 ms | 0.942 ms | 0.536 ms (98%) | 0.001 ms (0%) | 0.0% | 1 B |
| 64 | PASS | 10.766 ms | 19.231 ms | 16.106 ms | 10.712 ms (99%) | 0.022 ms (0%) | 0.0% | 64 B |
| 92 | PASS | 16.922 ms | 35.000 ms | 22.378 ms | 16.844 ms (100%) | — | 0.0% | 92 B |

`assemble` 在 N=1 时 0.004 ms（1%），随 N 增长被 `group_read` 淹没；`single_read` 全程 0（没有合并组失败回退）。

### 4.2 容量边界（20 ms 周期）

| N | skip | 超期率 | 判定 |
| --- | --- | --- | --- |
| 92 | 0 | 0.00% | PASS |
| 93 | 1 | 1.64% | FAIL |
| 94 | 1 | 1.67% | FAIL |
| 128 | 9 | 15.00% | FAIL |

结论：本机 20 ms 周期下 **92 个互不相邻变量**是上限，与前一批 P4 标定（TCP 92）一致。

## 5. 结论：瓶颈在往返次数，不在换算层

1. `group_read` 稳定占 **98%–100%** 的 tick 耗时，`convert` 始终 **≤ 0.05 ms**（在 1% 以内）。
   A2L 换算层不是瓶颈，优化它不会提高容量上限。
2. `merged = 0.0%` 全程成立——探针按 `kProbeStride = 4` 取地址，**故意让地址合并完全失效**，
   所以这份数据描述的是"最坏情况：每变量一次往返"。
3. 因此容量上限 ≈ `周期 / 单次往返耗时`：N=92 时 tick 16.9 ms 已经贴近 20 ms 周期，
   再加一个变量就会超期。要提高上限，只有两条路：减少往返次数（真实地址相邻时自动生效）
   或提高链路单次往返速度。

这三条正是引入阶段分解的目的：**把"跑不动"从现象变成可归因的数字**。

## 6. 已知限制

1. 只有 poll 侧被接入；DAQ 侧（P-C 的六阶段）没有对应标定脚手架。
2. 阶段分解是**整个运行窗口的聚合**，看不到"后半段变慢"这类时间演化——需要改成周期性取快照才能观察。
3. `merged` 在标定场景恒为 0（探针布局决定），不能用来评估合并优化效果；评估合并需要另写连续地址探针。
4. 标定不进 `ctest` 常规套件（耗时约 35 s 且依赖本机 XCPlite 端口），仍需手动执行。
5. 本机为 Windows 非实时环境，数字只能横向比较，不能当作绝对性能承诺。

## 7. 复现命令

```bash
# 构建
cmake --build cmake-build-release --target XcpliteIntegration -j 4

# 运行（缩短窗口以加快迭代；不设则用缺省 10000 ms）
LIBXCP_POLL_CALIBRATION_WINDOW_MS=1200 \
  ./cmake-build-release/tests/XcpliteIntegration.exe \
  --gtest_filter='*PollCapacity*'
```

预期：`PrintRun()` 行之后紧跟一行 `perf: tick mean=... | group=... | groups/tick=... merged=... bytes/tick=...`。

## 8. 改动文件

| 文件 | 说明 |
| --- | --- |
| `tests/poll_capacity_calibration.cpp` | 接入 `PollPerfSnapshot` 并打印阶段分解；修正过期 gtest filter 注释 |

依赖的既有能力（本批不改）：`include/libxcp/detail/perf_window.hpp`、`PollSession::PerfSnapshot()`。
