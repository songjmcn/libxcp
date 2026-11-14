# P-C：DAQ 六阶段性能统计 实施记录

## 1. 交付概览

在 `MeasurementSession` 上补齐与 poll 侧对称的滑动窗口性能统计：六阶段耗时
（`rx_enqueue` / `queue_wait` / `envelope_decode` / `payload_decode` / `callback` / `e2e`）、
帧数与切片数计数、`PerfConfig` 运行期配置 API、一窗口一次的汇总日志。

| 项 | 值 |
| --- | --- |
| 阶段数 | 6（含端到端延迟 `e2e`） |
| 日志频率 | 默认 5 s 一窗口（`PerfConfig::window`），不是每帧一行 |
| 采样频率 | 默认每帧都测（`PerfConfig::sample_every = 1`） |
| 新增单测 | 3 例（`MeasurementSessionPerf`） |
| 单元测试总计 | 1031 例：1030 通过 / 1 skipped（既有） |

## 2. 改动文件

| 文件 | 改动 |
| --- | --- |
| `include/libxcp/detail/perf_window.hpp` | 新增 `WindowAggregator::Config()`：返回"即将生效"的配置（pending 优先），供 `PerfConfigValue()` 使用 |
| `include/libxcp/measurement/measurement_session.hpp` | 新增 `#include "libxcp/detail/perf_window.hpp"`、`MeasurementPerfSnapshot`、`QueuedDto`、`DaqFrameTiming`；构造增加 `PerfConfig` 末位默认参数；新增 `SetPerfConfig` / `PerfConfigValue` / `PerfSnapshot`；私有 `RecordFramePerf` / `FlushPerfWindow` 与统计成员 |
| `src/measurement/measurement_session.cpp` | 构造初始化 `m_perf_`；`OnDto` 记录到达时刻与入队耗时；worker 取帧算 `queue_wait`；信封解码、载荷解码、回调分别计时；每帧 `RecordFramePerf` |
| `tests/measurement_session_test.cpp` | 新增 `MeasurementSessionPerf` 三个用例（721 → 809 行） |
| `src/poll/poll_session.cpp` | 仅 `PerfConfigValue()` 改为 `m_perf_.Config()`（与 DAQ 侧口径一致） |

## 3. 关键决策

### D1：队列元素携带到达时刻（`std::deque<QueuedDto>`）

`e2e` 无法事后推导，必须在帧被接收的那一刻记时。`QueuedDto` 只多两个字段
（`arrival`、`enqueue_ms`），不改变队列容量语义（仍是 `kMaxQueue = 256`，溢出仍丢最旧帧）。

### D2：`e2e` 定义为「RX 入队时刻 → 回调开始」

不含用户回调自身耗时（那由 `callback` 阶段单独统计）。这样两个数字相加即"从帧到达到回调返回"。
取不到（未配置回调、或帧未走到回调）时保持 `-1`，`RecordFramePerf` 跳过该阶段，
**不把缺失当成 0**，否则窗口均值会被虚假拉低。

### D3：解码失败的帧也计入窗口

信封解码失败会 `continue`，但它在现实中确实消耗了时间、也确实是一帧。
因此在 `continue` 之前同样 `RecordFramePerf`（`payload_decode`/`callback`/`e2e` 保持 `-1`）。
否则"解码错误率高的场景"窗口里只剩成功帧，耗时会显得异常漂亮。

### D4：`snapshot.window.samples` 是「阶段样本数」，不是帧数

`WindowAggregator::samples_` 统计 `Record()` 调用次数。DAQ 每帧记 6 个阶段，
所以 3 帧 = 18 个样本。帧数用 `MeasurementPerfSnapshot::frames`。
测试曾把两者混淆，已在断言处加注释固化语义。

### D5：`PerfConfigValue()` 返回"即将生效"的配置

初版 `SetPerfConfig()` 只把新配置放进 pending，而 `PerfConfigValue()` 读 `ActiveConfig()`，
于是「设完立刻读」拿到旧值——这是 API 缺陷，不是测试写法问题。
新增 `WindowAggregator::Config()`（`has_pending_ ? pending_ : config_`）修正；
`ActiveConfig()` 仍供 `FlushPerfWindow()` 这类"当前窗口按哪份配置记录"的内部路径使用。

### D6：`FlushPerfWindow()` 要求调用方持锁

日志格式化与慢路径聚合在 `m_perf_mutex_` 下完成，不引入第二把锁。
窗口输出失败（例如 logger 抛异常）不得影响采集，故调用点在 `RecordFramePerf` 内部且用 `try` 包裹。

## 4. 测试

`tests/measurement_session_test.cpp` 追加 `MeasurementSessionPerf`：

| 用例 | 断言要点 |
| --- | --- |
| `SnapshotReportsFrameAndStageTotals` | 3 帧 → `frames == 3`、`payload_slices == 3`、6 个阶段 `count == 3`、`window.samples == 18`、`E2e.max_ms >= EnvelopeDecode.max_ms` |
| `DisabledConfigCollectsNoStages` | `enabled = false` → 帧计数照常（`frames == 1`）、阶段样本为 0、`PayloadDecode.count == 0` |
| `SnapshotResetsWindowCounters` | 第二次 `PerfSnapshot()` 的 `frames == 0` 且 `Empty()` |

## 5. 验证结果

```
cmake --build cmake-build-release --target libxcp_tests -j 4      # [10/10] Linking，零错误
./cmake-build-release/tests/libxcp_tests.exe --gtest_filter="MeasurementSessionPerf*"
# [==========] 3 tests from 1 test suite ran. (9 ms total)
# [  PASSED  ] 3 tests.
./cmake-build-release/tests/libxcp_tests.exe
# [==========] 1031 tests from 173 test suites ran. (47962 ms total)
# [  PASSED  ] 1030 tests.
# [  SKIPPED ] 1 test (AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8)
```

## 6. 已知限制

1. `e2e` 只在"帧确实走到回调"时才有值；解码失败帧没有端到端延迟。
2. 队列容量仍固定 256；本改造没有暴露队列深度指标（`queue_wait` 的均值可间接反映积压）。
3. 阶段耗时含 `steady_clock::now()` 本身的调用成本（Windows QPC 约 20–30 ns/次，7 次/帧在 1 kHz 下约 210 µs/s）。
4. `sample_every > 1` 时 `samples` 按抽稀后的次数统计，`frames` 仍按全部帧统计——两者不可直接相除得"每帧样本数"。
5. 未对 `MeasurementSession` 加编译期开关（只有运行期 `PerfConfig::enabled` 与 `LIBXCP_PERF_ENABLED`）。

## 7. 复现命令

```bash
cmake --build cmake-build-release --target libxcp_tests -j 4
./cmake-build-release/tests/libxcp_tests.exe --gtest_filter="MeasurementSessionPerf*"
./cmake-build-release/tests/libxcp_tests.exe
```

## 8. 改动文件清单

- `include/libxcp/detail/perf_window.hpp`
- `include/libxcp/measurement/measurement_session.hpp`
- `src/measurement/measurement_session.cpp`
- `src/poll/poll_session.cpp`（仅 `PerfConfigValue()`）
- `tests/measurement_session_test.cpp`
