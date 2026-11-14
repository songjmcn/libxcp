# P-A 性能窗口聚合器 实施记录

> 对应计划：`code-plan/性能统计_滑动窗口与分阶段耗时_实施计划.md` §3、§4
> 任务：`perf-window-core`
> 范围：只做"聚合器 + 单测"，不改任何 Session（poll / DAQ 的埋点在 P-B、P-C）。

## 1. 交付物

| 文件 | 说明 |
| --- | --- |
| `include/libxcp/detail/perf_window.hpp` | 新增，header-only，约 690 行，纯 LF |
| `tests/perf_window_test.cpp` | 新增，20 个用例，纯 LF |
| `tests/CMakeLists.txt` | `libxcp_tests` 源列表追加 `perf_window_test.cpp` |

## 2. 头文件结构

| 符号 | 作用 |
| --- | --- |
| `PerfStage` | 12 个阶段枚举（poll 7 + DAQ 5）+ `Count` 哨兵 |
| `kPerfBucketEdgesMs` | 16 个固定桶边界（0.1 ms … 5000 ms，末桶 +∞） |
| `StageWindow` | 单阶段 `count / sum_ms / max_ms / buckets` + `MeanMs()` / `PercentileMs()` |
| `PerfConfig` | `enabled` / `window` / `sample_every` / `slowest_capacity` / `log_to_logger` / `verbose_stages` |
| `PerfConfigFromEnvironment()` | 读 `LIBXCP_PERF_*` 五个环境变量，非法值回落并记 WARNING |
| `SlowSpot` | 「阶段 × 变量」槽：`symbol` / `count` / `sum_ms` / `max_ms` / `group_size` / `group_bytes` |
| `PerfWindowSnapshot` | 一次窗口的只读快照（时间跨度、事件数、各阶段、slowest 列表） |
| `WindowAggregator` | `Record` / `Due` / `TakeSnapshot` / `SetConfig` / `Reset` |

## 3. 关键决策

- **D1 header-only**：聚合器没有平台依赖，放头文件可让 `PollSession`、`MeasurementSession`、`xcp_master` 三处零链接成本复用；与 `transport_sniff.hpp`、`project_file.hpp` 的风格一致。
- **D2 用固定桶而非保存样本**：保存原始样本会让内存随采样数增长，实时线程里也必然引入分配；16 桶 + 桶内线性插值的内存固定约 1.5 KB/会话，p50/p95/p99 与精确分位数误差在桶宽以内，足够定位热点。
- **D3 时钟可注入（`NowFn`）**：否则窗口边界与 `Due()` 只能靠 sleep 测，既慢又不稳定；注入后 20 个用例 3 ms 跑完。
- **D4 `sample_every` 只抽稀采样、不抽稀计数**：`events_` always 递增，`samples_` 才决定是否 `Record`，否则"每 N 帧测一次"会把超期率也算错。
- **D5 slowest 按窗口累计 `sum_ms` 排序**：每 tick 0.5 ms 的变量比偶发 5 ms 的变量更值得优化；`max_ms` 只作次键。
- **D6 环境变量读取用平台助手**：MSVC 把 `std::getenv` 标为不安全（C4996），`detail::PerfEnvironmentValue()` 在 `_MSC_VER` 下走 `_dupenv_s` + `std::free`，其它平台走 `std::getenv`，语义一致。**本仓此前无任何 `getenv` 调用**（`src/` 全扫 0 命中），所以这是一处新引入的用法，必须把警告清零而不是接受。

## 4. 测试结果

```
cmake --build cmake-build-release --target libxcp_tests -j 4     # 通过，零错误
libxcp_tests.exe --gtest_filter="PerfWindow*:StageWindow*:PerfConfig*"
[==========] 20 tests from 3 test suites ran. (3 ms total)
[  PASSED  ] 20 tests.
```

| 套件 | 用例数 | 覆盖 |
| --- | --- | --- |
| `PerfWindow` | 15 | 累加与桶、负值钳零、窗口边界、快照归零、采样抽稀、禁用、配置延迟生效、slowest 排序/累加/分阶段/容量 0、合并组 `group_size`/`group_bytes`、桶索引单调、阶段名稳定 |
| `StageWindow` | 2 | 分位数跟随分布、空窗口返回 0 |
| `PerfConfig` | 3 | 环境变量覆盖、非法值回落、逐字段相等比较 |

构建产物侧：链接 `libxcp_tests.exe` 后重新跑两条 getenv 警告已消失，仅剩 glog 自带的 `C4244`/`C4996`。

## 5. 已知限制

1. `PercentileMs()` 是桶内线性插值，不是精确分位数；误差上限为所在桶宽度。
2. 桶上限 5000 ms，超过的样本落末桶，`PercentileMs` 用 `max_ms` 兜底（不会低报）。
3. `SlowSpot::symbol` 是固定 64 字节缓冲区，超长符号被截断（A2L 符号名远短于此）。
4. `Record()` 无锁，但要求同一实例只被一个线程调用（poll 调度线程 / DAQ worker 各自独占）。
5. 本阶段只提供聚合能力，日志格式化由 P-B（poll）/ P-C（DAQ）负责。

## 6. 复现命令

```bash
# 构建
printf '@echo off\r\ncall "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul 2>&1\r\ncd /d D:\\project\\libxcp\r\ncmake --build cmake-build-release --target libxcp_tests -j 4\r\n' > /tmp/pt.bat
cmd.exe /c "$(cygpath -w /tmp/pt.bat)"
# 跑用例
./cmake-build-release/tests/libxcp_tests.exe --gtest_filter="PerfWindow*:StageWindow*:PerfConfig*"
```

## 7. 下一步

`perf-poll-stages`：在 `PollSession::RunTick` 里按计划 §5 埋 7 个阶段，并把窗口到点时组装一次日志（含 `bytes/tick`、`group_mean`、`merged`、slowest 的前若干行）。
