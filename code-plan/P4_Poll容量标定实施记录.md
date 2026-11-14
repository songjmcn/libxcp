# P4：20 ms 周期下的 Poll 容量标定实施记录

> 对应计划：`code-plan/Poll模式多Event架构实施计划.md` §9 批次 P4、§11.1 验收口径。
> 本记录只写真实运行数字。标定日期：2026-11-14。

---

## 1. 标定目标与方法

**要回答的问题**：单 event、周期 20 ms 时，poll 最多能稳定轮询多少个变量。

**方法**（对应 §11.1）：

| 口径要求 | 本实现 |
|----------|--------|
| 变量地址两两不相邻 | 取 Slave 的 `g_blob`（A2L 中 600 字节 U8 块，基址 `0x20410`），每 4 字节取一个 1 字节探针 → 150 个真实地址，两两间隔 4 字节 > 元素宽度，P3 的地址合并必然失效 |
| 单 event、20 ms | `PollEventSpec{period = PollSession::kMinPeriod}`，只注册一个 event |
| 运行 ≥ 10 s | 默认窗口 10 000 ms；`LIBXCP_POLL_CALIBRATION_WINDOW_MS` 仅用于快速自检 |
| 二分找最大 N | 倍增 1→2→4→…→128 找上界，再在 `(last_pass, first_fail)` 内二分收敛 |
| 记录环境 | 本机 Windows + MSVC 19.44 Release + loopback UDP + XCPlite Slave 子进程 |

**为什么用 blob 探针而不是 Slave 的真实变量**：Slave 只注册了 5 个标量 + 若干结构体/数组，地址数量不足以二分；`g_blob` 提供 600 字节可读区，能在**不新增 Slave 变量**的前提下构造足够多真实存在、互不相邻的地址。探针地址由 A2L 运行期解析得到，不硬编码。

---

## 2. 判据修正（重要发现）

计划 §11.1 原写判据为 `skipped_overruns == 0`。**实测该判据在 Windows 上不可达**：

首轮正式标定（10 s 窗口）结果：

```
N=1    ticks=501  skipped=0  mean=20.010 ms worst=32.175 ms  PASS
N=2    ticks=500  skipped=1  mean=20.055 ms worst=32.098 ms  FAIL   ← 因 1 次超期直接判失败
结论：最多 1 个变量（显然荒谬）
```

关键数据是 `worst`：**N=1 时相邻 tick 的最大间隔也达 32 ms**（比 20 ms 周期多约 12 ms），10 s 内 500 个 tick，这种抖动必然偶尔命中一次。这不是 poll 实现的缺陷，是 Windows 非实时 OS 的线程调度/定时器精度限制。

因此判据改为**超期比例** ≤ 1%（`skipped / (ticks + skipped)`），`kMaxOverrunRatio` 常量处已注释理由。该指标对 N 仍有单调区分力（见 §3 表格：0.2% → 1.20% → 5.80% → 30.2%），且比"绝对 0 次"更贴近"能否稳定按 20 ms 采样"的工程含义。

**这一修正必须写进结论**：任何在非实时 OS 上复现该死循环的判据，都不应要求"零超期"。

---

## 3. 实测结果（正式标定，10 s 窗口）

| N | ticks | skipped | 超期率 | mean 间隔 | worst 间隔 | 判定 |
|---|-------|---------|--------|-----------|-----------|------|
| 1 | 500 | 1 | 0.20% | 20.046 ms | 41.807 ms | PASS |
| 2 | 501 | 0 | 0.00% | 20.024 ms | 32.079 ms | PASS |
| 4 | 500 | 0 | 0.00% | 20.017 ms | 32.805 ms | PASS |
| 8 | 501 | 0 | 0.00% | 20.025 ms | 32.080 ms | PASS |
| 16 | 501 | 0 | 0.00% | 20.013 ms | 32.178 ms | PASS |
| 32 | 501 | 0 | 0.00% | 20.017 ms | 32.206 ms | PASS |
| 64 | 501 | 0 | 0.00% | 20.011 ms | 35.302 ms | PASS |
| 68 | 498 | 2 | 0.40% | 20.101 ms | 35.202 ms | PASS |
| 70 | 498 | 2 | 0.40% | 20.101 ms | 35.307 ms | PASS |
| **71** | 499 | 1 | **0.20%** | **20.068 ms** | 35.683 ms | **PASS（最大）** |
| 72 | 494 | 6 | 1.20% | 20.267 ms | 35.319 ms | FAIL |
| 80 | 471 | 29 | 5.80% | 21.270 ms | 34.318 ms | FAIL |
| 96 | 428 | 72 | 14.40% | 23.373 ms | 39.520 ms | FAIL |
| 128 | 349 | 151 | 30.20% | 28.718 ms | 48.273 ms | FAIL |

**本次运行结论：本机环境（loopback UDP + XCPlite Slave）20 ms 周期下最多 71 个互不相邻变量。**

用量 140 428 ms（14 轮 × 10 s + 启动）。

### 3.1 重复性与方差（重要）

同一用例在同一台机器上又跑了一次（这次与其余 61 个 XCPlite 集成用例**连续**执行，系统负载更高），结果为 **63**：

```
N=1    ticks=501  skip=0   (0.00%) mean=20.009 ms worst=32.149 ms  PASS
N=64   ticks=490  skip=10  (2.00%) mean=20.421 ms worst=35.453 ms  FAIL
结论：20 ms 周期下最多 63 个互不相邻变量
```

两次运行的差异（71 vs 63，约 −11%）说明这个数字是**受系统负载影响的经验值**，不是设备常数：

- 单独运行、机器空闲 → 71；
- 与 61 个真实 UDP/TCP 集成用例连跑、后台负载更高 → 63。

因此报告口径必须是"**本机约 60–70 个量级（两次实测 63 与 71）**"，不能引用单次数字当作硬指标。要得到可发布的容量，需要多次重复取中位数并固定负载条件——本批未做（见 §6-7）。

**单次读取往返的推算**：N=128 时 mean 比 20 ms 多出 8.718 ms，128 次读取平摊 ⇒ 约 **68 µs/次**。这与 loopback UDP 的实际往返量级一致。据此，纯计算上 20 ms 可容纳约 280 次往返，但**最坏间隔抖动 ~12 ms 吃掉了一半预算**，所以稳定容量只有 71。

**这个数字的含义**：它量化的是"20 ms 周期 + Windows 调度抖动 + loopback UDP"的组合，**不是** poll 架构的上限。换实时 OS、增大周期或换更快的链路都会改变它。报告时必须带上环境，不能单独引用"71"。

---

## 4. 代码变更

| 文件 | 变更 |
|------|------|
| `tests/poll_capacity_calibration.cpp`（新增，约 350 行） | `BlobProbeDatabase`（把 `probe_<i>` 映射到 blob 探针地址）、`FindAddressByName`（从 A2L 取 `g_blob` 基址，不硬编码）、`RunOne`（一轮标定 + 判据）、倍增 + 二分主用例 |
| `tests/CMakeLists.txt` | `if(LIBXCP_BUILD_XCPLITE_SLAVE)` 块的 `XcpliteIntegration` 源列表新增 `poll_capacity_calibration.cpp` |

**刻意不放进 `libxcp_tests`**：一轮标定约 140 s，会把它从 46 s 拖到 3 分钟以上。挂到 `XcpliteIntegration`（手动运行的独立 exe）既保留可复现性，又不拖慢常规回归。

---

## 5. 设计决策

1. **探针地址从 A2L 动态解析**：`g_blob` 基址随编译变化（本例 `0x20410`），硬编码会在下次构建后失效；用 `FindAddressByName(a2l, "g_blob")` 取 A2L 里的第一个 `0x` 字面量。
2. **探针步长 4 字节**：既能保证两两不相邻（合并失效），又能在 600 字节 blob 内得到 150 个地址，覆盖到 128 这一档。
3. **判据用比率而非绝对次数**：见 §2。
4. **每轮新建 `PollSession`**：`AddEvent` 不允许在运行中调用，且每轮需要干净的统计。
5. **窗口可被环境变量覆盖**：让标定逻辑本身能在秒级自检，正式数字用默认 10 s（记录中明确标注哪个数字来自哪个窗口）。
6. **主用例断言 `last_pass > 1`**：如果连 2 个变量都跑不稳，说明环境异常（不是容量问题），应当失败而不是输出一个荒谬的"容量 1"。

---

## 6. 边界与遗留

1. **只测了 UDP loopback**：计划 §11.1 要求 UDP/TCP 都测，TCP 未做（`XcpliteSlaveProcess` 支持 `use_tcp = true`，只需在用例里换夹具参数）。
2. **只测 1 字节标量**：`element_size_bytes = 1`。更大的元素（4/8 字节）在 `SHORT_UPLOAD` 下仍是 1 次往返，但 DTO 更长；若超过单包上限会退化为分块 UPLOAD（每块 2 往返），容量会显著下降。未测。
3. **只测单 event**：多 event 共享一个调度线程，读取串行，总变量数才是约束；未测多 event 混合周期的公平性。
4. **未测真实 ECU/以太网**：loopback 的往返时间不代表真实链路。
5. **抖动来源未分离**：`worst ≈ 32 ms` 恒定，与 N 无关，推测为 OS 定时器/调度；未做 CPU 亲和性或 `timeBeginPeriod` 对比实验。
6. **含 1 次超期的 N=1 也判 PASS**：判据是比率，这是设计选择；若要更严格，可要求 `ticks >= 0.95 × 期望 tick 数`。
7. **未做重复性标定**：两次运行得到 71 与 63（§3.1），方差约 11%。正式发布容量应做 N 次重复取中位数并记录负载条件，本批未做。
8. **集成回归证据**：`XcpliteIntegration.exe` 全量 **62 tests / 62 passed / 155 570 ms**（含本标定用例，第二次运行的 63 即来自这一次）。注意必须把 `TEMP`/`TMP` 指向工作区内目录运行——否则 `XcpSlaveDemoIntegrationTest` 的 11 个用例会因 `%TEMP%` 下 `create_directories` 被拒（`Access is denied`）而失败，这是本机环境限制，与代码无关（换 `TEMP` 后全部通过，已实测）。

---

## 7. 复现方式

```bash
# 构建（需要 LIBXCP_BUILD_XCPLITE_SLAVE=ON，本仓库默认开启）
cmake --build cmake-build-release -j 4 --target XcpliteIntegration

# 正式标定（约 140 s）
./cmake-build-release/tests/XcpliteIntegration.exe \
    --gtest_filter='XcpliteSlaveTest.PollCapacityAtTwentyMillisecondPeriod'

# 快速自检（1.5 s 窗口，数字不可作为结论）
LIBXCP_POLL_CALIBRATION_WINDOW_MS=1500 \
  ./cmake-build-release/tests/XcpliteIntegration.exe \
    --gtest_filter='XcpliteSlaveTest.PollCapacityAtTwentyMillisecondPeriod'
```

---

## 8. 自审

| 检查项 | 结论 |
|--------|------|
| 是否有编造数字 | 无，全部来自 `p4_calibration.log` 实跑 |
| 是否解释了判据为何偏离计划 | 是（§2，含原始数据） |
| 是否声明了环境依赖 | 是（§1、§3 末段、§6） |
| 是否硬编码地址 | 否，从 A2L 解析 |
| 是否拖慢常规回归 | 否，挂在 `XcpliteIntegration` |
| 中文 Doxygen | 新增类型/函数均有 |
| 是否改 thirdparty | 否 |
| 是否有未处理告警 | 无（本文件 0 告警；首次构建的 C4996 已用文件级 pragma 消除并注释理由） |

---

## 9. 补齐复测（2026-11-14，clean 全量构建 + TCP 标定）

用户要求"检查遗漏项"后确认原 P4 只标定了 UDP，且全程未做 clean 构建。
本轮补齐：`clean` 后完整 Release 构建（含 `XcpliteIntegration`）、TCP 标定用例、
以及 `PollSample::steady_time_end`（见 §10）。

### 9.1 实测（clean 构建，2026-11-14 16:2x）

| 传输 | 窗口 | N=1 baseline | 结论 N | 失败档 |
|------|------|--------------|--------|--------|
| UDP | 10 s | mean 20.008 ms / skip 0 | **65** | N=66 (1.60 %) |
| TCP | 10 s | mean 20.010 ms / skip 0 | **92** | N=93 (2.40 %) |

完整表见当次日志；关键点：

- **UDP 65** 与本记录 §3 第一次的 71、第二次连续跑的 63 同属一个量级（60–70），
  三轮实测 63 / 65 / 71 的散布约 −11 %~+10 %，坐实了"同环境重复标定需取量级
  而非精确值"的结论。
- **TCP 92 明显高于 UDP**：同一 blob、同一从机进程、同一判据下 TCP 反而能多带
  ~40 % 的变量。合理解释是 TCP 无 Nagle 之外的额外开销且回环走内核直通，
  而 UDP 路径每帧都要过一次 datagram 收发的用户态轮询
  （`UdpTransportConfig::receive_poll_interval_ms`）；也可能是本轮系统负载更低。
  两种解释都无法从本次数据中分离，故**只记录事实，不推广结论**。
- `read_err` 与 `cb_err` 在所有档位均为 0，说明失败提现为超期而非读错误。

### 9.2 TCP 标定实现

`tests/poll_capacity_calibration.cpp` 抽出 `CalibrateCapacity(master, database, window)`
（倍增 + 二分，与 UDP 共用同一判据），新增夹具 `PollCapacityTcpTest`
（`XcpliteSlaveProcess{std::nullopt, 0, /*use_tcp=*/true}` +
`TcpTransport`（`TcpWireProfile::AsamV14` + `PacketAlignment::Bit8`））与用例
`PollCapacityAtTwentyMillisecondPeriodOverTcp`，断言与 UDP 版本一致
（`max_variables > 1`），并额外 `RecordProperty("max_variables_at_20ms_tcp")`。

### 9.3 回归

- `libxcp_tests`：**1001 例 / 1000 通过 / 1 跳过 / 0 失败 / 47093 ms**
- `XcpliteIntegration`（不含标定）：**61 例 / 61 通过 / 36054 ms**
- 两个标定用例：**2/2 通过**（140412 ms + 140583 ms）
- clean 后完整构建 `EXIT=0`，本仓库源码 0 告警
