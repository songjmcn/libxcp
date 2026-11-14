# xcp_master 工具侧 poll 性能统计（P-D）实施记录

## 1. 目标与范围

`xcp_master` 的 poll 线程是工具自己实现的（不复用 `PollSession`），因此在库侧完成 P-A/P-B/P-C 之后，工具侧仍看不到任何耗时数字。本批补上两个量：

| 指标 | 含义 | 为什么需要 |
| --- | --- | --- |
| `cycle_actual` | 相邻两轮 `cycle_start` 的间隔 | 目标周期是 `--interval`，但真实周期受读取耗时与条件变量抖动影响；只看目标值无法判断"是否跟得上" |
| `tick_total` | 一轮"取快照 + 逐个变量读取 + 记录值"的耗时 | 反映读取负载；与 `cycle_actual` 对比即可看出余量 |

按方案约定，工具侧**只统计这两个量**，不做阶段拆分：它走的是 `database_.Find()` + `ReadPhysical` 的逐变量路径，拆出来的阶段也无法用于优化库内实现。

## 2. 改动

### 2.1 `include/libxcp/detail/perf_window.hpp`

`PerfStage` 增加 `CycleActual`（放在 `Count` 之前），`PerfStageName()` 增加对应分支返回 `"cycle_actual"`。

位置选在枚举末尾而不是 poll 阶段中间，是为了让已有阶段的下标保持不变——`kPerfStageCount` 只增不减，旧的下标语义不受影响。

### 2.2 `tools/xcp_master/main.cpp`

| 位置 | 改动 |
| --- | --- |
| include 区 | 增加 `<cstdio>` 与 `"libxcp/detail/perf_window.hpp"` |
| 轮询线程 | 每轮读取结束、进入条件变量等待之前调用 `RecordPollCyclePerf(cycle_start)` |
| 成员区 | `poll_perf_`（`WindowAggregator`）、`poll_perf_mutex_`、`poll_last_cycle_`、`poll_has_last_cycle_`、`poll_status_*` 四字段 |
| 方法区 | `RecordPollCyclePerf()`、`FormatPollPerfLog()`、`PollPerfStatusText()` |
| 状态栏 | 第二行末尾追加 `PollPerfStatusText()` |

三条实现约束：

1. **周期用相邻 `cycle_start` 之差**，而不是"读取耗时 + 等待耗时"。后者会把条件变量的唤醒抖动算进周期，测出来的数字比真实调度周期更难看，且无法区分"读得慢"和"睡得久"。
2. **聚合器与状态字段由同一把互斥量保护**。聚合器本身不是线程安全的（`Record` 设计为无锁快路径），而这里写方是 watch 线程、读方是 FTXUI 渲染线程，必须自行加锁。
3. **只在窗口到点时才格式化日志**。状态行摘要每轮更新一次（一次 `snprintf` 到栈缓冲），日志每 5 s 一条。

### 2.3 日志与状态行

窗口到点时输出一行：

```
[perf][poll] window=5.000s ticks=10 cycle=500.12ms read mean=1.823ms p95=2.400ms max=6.100ms target=500ms
```

状态栏第二行末尾：

```
  |  周期 500.1ms / 读取 1.82ms
```

窗口未到点（还没有快照）时状态段为空串，不占位。

## 3. 验证

| 命令 | 结果 |
| --- | --- |
| `cmake --build cmake-build-release --target xcp_master -j 4` | `[7/7] Linking CXX executable tools\xcp_master\xcp_master.exe`，零错误 |
| `cmake --build cmake-build-release --target libxcp_tests -j 4` | `[7/7] Linking CXX executable tests\libxcp_tests.exe`，零错误 |
| `libxcp_tests.exe --gtest_filter="PerfWindow*:StageWindow*:PerfConfig*:PollSessionPerf*:MeasurementSessionPerf*"` | **28/28 通过**（241 ms） |

`grep` 复核行号：`main.cpp:1071`（状态栏）、`main.cpp:3761-3790`（`RecordPollCyclePerf`）、`main.cpp:3826`（`PollPerfStatusText`）、`main.cpp:3912`（埋点调用）、`main.cpp:4290-4293`（成员）。

## 4. 已知限制

1. **只统计两个量**：没有最慢变量、没有按变量拆分。工具侧逐变量读取的耗时分散在 `ReadPhysical` 调用中，若要证明"哪个变量慢"，应改用库侧的 `PollSession`（P-B 已实现 slowest）。
2. **不统计超期**：工具侧没有"整周期跳过"的概念，跟不上只会表现为 `cycle_actual` 大于目标。
3. **首轮不记周期**：没有"上一轮"可比，`CycleActual.count` 比 `TickTotal.count` 少 1。
4. **状态行每次重绘都会取一次锁**：FTXUI 每帧渲染都会调用 `PollPerfStatusText()`，锁争用极低但非零；这是为了让状态数字实时，代价可接受。
5. **日志走 `Say()`**：进入界面的消息栏，不写 glog。若需要落盘分析，可后续改为 `LIBXCP_LOG_INFO` 双写。

## 5. 复现命令

```bash
# 构建
cmd.exe /c build.bat            # vcvars64 + cmake --build cmake-build-release --target xcp_master -j 4

# 单测
./cmake-build-release/tests/libxcp_tests.exe \
  --gtest_filter="PerfWindow*:StageWindow*:PerfConfig*:PollSessionPerf*:MeasurementSessionPerf*"

# 人工验收（需要交互式终端）
LIBXCP_PERF_WINDOW_MS=1000 ./cmake-build-release/tools/xcp_master/xcp_master.exe \
  --a2l <A2L 路径> --host 127.0.0.1 --port 5555 --interval-ms 100
# 在界面里 watch 若干变量，观察：
#   1) 状态栏出现「周期 xxx ms / 读取 x.xx ms」
#   2) 每秒输出一条 [perf][poll] 汇总
```

## 6. 改动文件

- `include/libxcp/detail/perf_window.hpp`（枚举 + 阶段名）
- `tools/xcp_master/main.cpp`（include、埋点、成员、三个方法、状态栏）
