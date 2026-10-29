# DAQ 帧率遗留问题分析与 macOS 手工复测方案

- 日期：2026-10-29
- 分支：`feat/tcp-a2l-wire-evidence`（PR #17）
- 性质：**只读分析 + 复测方案**，本次未修改任何产品代码、测试代码与第三方库
- 目标读者：在**真实 Mac 真机（非 CI/workflow 机器）**上执行复测的开发者

---

## 1. 问题定义

macOS CI 上，用例 `XcpSlaveDemoIntegrationTest.CrossProcessCalibrationChangesDaqOutput`
（`tests/xcp_slave_demo_integration_test.cpp:1457`）曾失败，失败信息给出的观测值为：

| 指标 | 观测值 | 理论期望 |
| --- | --- | --- |
| 3 秒窗口内收到的 DTO 帧数 | 89 | 300（10 ms 事件 → 100 Hz × 3 s） |
| 解码尝试次数 | 3483 | — |
| 解码失败次数 | 0 | 0 |
| 首帧内容 | `00 aa 00 00 22 d3 0a d3` | `[relODT][0xAA][daq16][ts32]` |

即：**接收率约 30%**，且**解码全部成功**（说明帧本身完好，不是协议解析问题）。

该用例现已通过“把 `gain` 清零、期望输出恒为 42.0”的方式对丢帧鲁棒
（`tests/xcp_slave_demo_integration_test.cpp:1526-1537` 的注释记录了原因）。
**副作用是：帧率退化不再会失败。** 这就是“遗留点”。

## 2. 结论摘要

1. **现象未定因**，存在两种互斥解释：
   - **A：从站生产者只跑了约 30 Hz**（10 ms 循环被拉长到 ~33 ms）；
   - **B：生产者 100 Hz，链路丢了约 70%。**
2. **代码内已有强指向 A 的证据**：`examples/xcp_slave_demo/main.cpp:49-53` 的心跳注释记录了历史实测：
   > 「早期按 tick 计数（10 ms × 500 = 5 秒），但慢机上事件循环被调度拖慢时逻辑时间跟不上墙钟：**CI 实测 9 秒墙钟只推进 265 次事件**」

   265 次 / 9 s ≈ **29.4 Hz**，与 89 帧 / 3 s ≈ **29.7 Hz** 几乎一致。这强烈提示：**循环本身就在 ~30 Hz 运行，帧并没有在链路上被丢**。
3. **但仍不能判定**，因为链路三层都没有任何能进入 CI 工件的计数：
   从站心跳走 glog 且默认编译关闭；XCPlite 两个丢弃点只有调试打印；主站从不查询内核丢包。
4. **门禁缺口**：没有任何用例断言“DAQ 吞吐下限”，所以 100 Hz 退化成 5 Hz 也是绿的。

## 3. 可观测性盘点（为什么 CI 上定不了因）

| 层级 | 现状 | 证据 |
| --- | --- | --- |
| 从站心跳（循环速率） | 走 `LIBXCP_LOG_INFO`，`LIBXCP_ENABLE_LOG` 默认 **OFF**，关闭时宏为空操作 → 无输出 | `examples/xcp_slave_demo/main.cpp:922-924`、`CMakeLists.txt:35`、`include/libxcp/log.hpp:78-92` |
| 从站 stdout | **会被 CTest 捕获**（CI 日志里能看到“正在监听”那一行），但 stdout 只有启动横幅，无运行期计数 | `examples/xcp_slave_demo/main.cpp:874-877`；`build-macos/6_Test.txt` 中 `正在监听` 命中 1 次 |
| XCPlite DAQ 队列溢出 | 仅当 `XCP_ENABLE_OVERRUN_INDICATION_PID` 才自增 `daq_overflow_count`，否则只有 `DBG_PRINTF6` | `thirdparty/XCPlite/src/xcplite.c:516-526` |
| XCPlite 发送队列溢出 | 只有 `DBG_PRINTF_WARNING("Transmit queue overflow: lost %u packets…")`，无查询接口 | `thirdparty/XCPlite/src/xcpEthTl.c:720-727` |
| 主站内核接收缓冲/丢包 | 全仓 `setsockopt` 只有 `SO_RCVTIMEO`；无 `SO_RCVBUF`、无 `SO_RXQ_OVFL` 计数 | `src/detail/socket/socket_ops_posix.cpp:183`、`socket_ops_win.cpp:212` |
| 测试断言 | 只断言“看到过 42.0”，无帧数/帧率下限 | `tests/xcp_slave_demo_integration_test.cpp:1596-1601` |

补充：Release 构建下 XCPlite 的 `DBG_PRINT*` 亦被剥掉，所以上述调试打印在 CI 里也不会有。

## 4. 为什么“队列溢出”和“主站不消费”基本可以排除

- 测量队列容量 `kMeasurementQueueSize = 32768` 字节（`examples/xcp_slave_demo/main.cpp:48`），
  按每帧 16~20 字节计 ≈ 1600 帧；300 帧仅约 5 KB。要丢 70% 需发送线程停摆十几秒。
- 发送线程在 macOS/Linux 分支是“100 次外循环 × 1 ms 睡眠、每次最多突发 1000 帧”
  （`thirdparty/XCPlite/src/xcpEthTl.c:706-711,716-756`），排空 100 Hz 毫无压力。
- 主站接收是 `poll()` 事件驱动（`src/detail/socket/event_poller_posix.cpp:61`），
  每 datagram 同步分发，16 字节报文在默认内核缓冲下也足够。

## 5. 判定原理（本方案的核心）

从站 `tick` 在**每次快速循环迭代**时自增，且**与是否连接主站、DAQ 是否运行无关**：

- `examples/xcp_slave_demo/main.cpp:887-905`：`if (now >= next_fast) { … ++tick; }`
- `examples/xcp_slave_demo/main.cpp:921-925`：每 5 秒（`kHeartbeatInterval`，`:54`）打印一次 `tick`

因此，**心跳里 `累计快事件次数` 的增量 ÷ 时间 = 循环真实速率**：

- 增量 ≈ 500 / 5 s → 循环 100 Hz → 若主站仍只收到 ~30 帧/秒，则问题在**链路或主站**（结论 B 方向）
- 增量 ≈ 150 / 5 s → 循环 30 Hz → **直接证实 A**，帧没有丢

**这个实验不需要主站、不需要写任何代码**，只需要打开日志编译开关。

---

## 6. macOS 真机复测步骤

> 目的：在非 CI 的真机上取得“循环速率”与“主站收帧数”两个独立观测值，判定 A / B。

### 步骤 0：记录环境（1 分钟）

```bash
sw_vers
sysctl -n hw.ncpu hw.physicalcpu hw.memsize
sysctl -n kern.boottime
uptime
```

把输出贴进结论；真机与 CI runner（通常 3~4 核共享）的差异本身就是结论的一部分。

### 步骤 1：构建（开启日志与测试/示例）

需要 glog 0.7.1+：

```bash
brew install glog
cmake -B build-macos -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DLIBXCP_BUILD_TESTS=ON \
  -DLIBXCP_BUILD_EXAMPLES=ON \
  -DLIBXCP_ENABLE_LOG=ON
cmake --build build-macos -j 8
```

> 两点必须注意：
> 1. `LIBXCP_BUILD_EXAMPLES` 默认 **OFF**（`CMakeLists.txt:350-352`），而集成用例只有在 examples 打开时
>    才会把 `xcp_slave_demo_integration_test.cpp` 挂到 `XcpliteIntegration`（`tests/CMakeLists.txt:182-186`）。
>    不开这个开关，`ctest -R XcpSlaveDemoIntegrationTest` 会找不到任何用例。
> 2. 若 glog 无法安装：可先跳过“开日志”，改用步骤 4 的降级方案（只能测稳定性，不能定因）。
>    `LIBXCP_ENABLE_LOG=ON` 需要支持 C++20 `<format>` 的编译器（`CMakeLists.txt:71-83`）。

### 步骤 2：先做关键实验——单一从站的心跳速率（约 3 分钟）

**这是最短路径，先做这一步。**

```bash
mkdir -p /tmp/xcp_daq_probe && cd /tmp/xcp_daq_probe
/path/to/build-macos/examples/xcp_slave_demo/xcp_slave_demo --bind 127.0.0.1 --port 5555
# 保持运行 ≥ 20 秒；然后 Ctrl+C 退出（glog 通常在进程退出时才落盘）
grep "从站心跳" logs/examples/xcp_slave_demo/*
```

日志目录形如 `<当前工作目录>/logs/examples/xcp_slave_demo/`
（判据来自测试侧读取逻辑 `tests/xcp_slave_demo_integration_test.cpp:1987-1993`）。

判读：相邻两条心跳的 `累计快事件次数` 之差：

| 5 秒增量 | 结论 |
| --- | --- |
| ≈ 500 | 循环 100 Hz，健康 |
| ≈ 150 | 循环 ~30 Hz → **现象 A 成立**：帧不是丢的，是根本没产生 |
| < 100 | 循环被严重饿死 |

### 步骤 3：叠加负载再测（复现 CI 条件，可选但推荐）

在步骤 2 的运行过程中，另开终端制造 CPU 竞争（模拟 `ctest -j4`）：

```bash
# 终端 B：制造与 CI 相当的并行负载
for i in 1 2 3 4; do yes > /dev/null & done
sleep 20
kill %1 %2 %3 %4
```

再次比对心跳增量。若增量随负载从 ~500 掉到 ~150，即可确认**降频由调度竞争引起**，
并说明 CI 上的 89 帧与产品无关。

### 步骤 4：跑失败用例本身（验证噪声区间）

```bash
cd /path/to/libxcp
ctest --test-dir build-macos -R "XcpSlaveDemoIntegrationTest.CrossProcessCalibrationChangesDaqOutput" \
      --output-on-failure -V --repeat until-fail:20
```

或直接跑可执行文件（更细的输出控制）：

```bash
./build-macos/tests/XcpliteIntegration \
  --gtest_filter='XcpSlaveDemoIntegrationTest.CrossProcessCalibrationChangesDaqOutput' \
  --gtest_repeat=20 --gtest_color=no
```

记录：**20 轮是否全绿**；若失败，贴出完整失败信息里的
`已收 DTO … 帧，解码成功 … 次，解码失败 … 次，首帧 …`。

### 步骤 5：若步骤 2 显示循环 ≈100 Hz 而收帧仍 ≈30（即结论 B）

此时问题在主站或链路，按顺序采集：

```bash
# 1) 确认主站线程是否被日志拖慢（开日志后不要同时开 -V 全量断言输出）
# 2) 用 DTO 自带时间戳判断连续性：帧头 [relODT][0xAA][daq16][ts32]
#    相邻帧 ts 差值 ≈10 ms → 生产正常但中间有帧缺失（丢包）
#    相邻帧 ts 差值 ≈33 ms → 出现在从站侧，回步骤 2 复核
# 3) 抓包（若可用 tcpdump）：
sudo tcpdump -i lo0 -n 'udp port 5555' -c 400 -w /tmp/xcp_daq.pcap
```

> 提示：DTO 时间戳布局已由源码确认（`thirdparty/XCPlite/src/xcplite.c:528-551`），
> CI 首帧 `00 aa 00 00 22 d3 0a d3` 中 `22 d3 0a d3` 即 32 位时间戳，
> **这是目前唯一能零改码区分 A / B 的线上证据**，建议失败时一并贴出多帧首字节。

## 7. 判读矩阵

| 循环速率（心跳增量/5 s） | 主站收帧率 | 结论 | 后续动作 |
| --- | --- | --- | --- |
| ≈500 | ≈100 Hz | 健康 | 记录真机基线，关闭该遗留点 |
| ≈500 | ≈30 Hz | 链路/主站丢包（B） | 查主站接收路径与内核缓冲 |
| ≈150 | ≈30 Hz | 生产降频（A） | 夹具/环境问题：需要调整测试对帧率的假设与门禁 |
| ≈150 | ≈100 Hz | 矛盾（不可能） | 采集有误，重测 |

## 8. 需要回传的采集物

1. 步骤 0 的环境输出；
2. 步骤 2/3 的 `从站心跳` 日志片段（至少 3 条连续心跳，注明是否有人造负载）；
3. 步骤 4 的 20 轮汇总（全绿/失败次数 + 失败时的完整诊断行）；
4. 若走到步骤 5：pcap 或至少 5 帧的首 8 字节十六进制 + 相邻时间戳差值。

## 9. 注意事项（真机复测容易踩的坑）

1. **临时目录会被删除**：用例内的 `TemporaryDirectory` 析构时 `remove_all`
   （`tests/xcp_slave_demo_integration_test.cpp:372-376`），所以**用例失败后拿不到从站日志**。
   要保留证据，必须用步骤 2 手工起从站到你自己控制的目录。
2. **glog 落盘时机**：测试注释明确“日志通常在进程退出时才落盘”
   （`tests/xcp_slave_demo_integration_test.cpp:2029-2031`）→ 必须 Ctrl+C 后再 `grep`。
3. **`LIBXCP_ENABLE_LOG=OFF` 时心跳不存在**，`ServiceEmitsPeriodicHeartbeatLog`
   在该配置下只断言“日志为空且进程可正常启停”（同文件 `:2048-2051`）。
4. **Release 下 XCPlite 的 `DBG_PRINT*` 也会被关掉**，不要指望从 CI 日志里找“队列溢出”。
5. **不要为了看日志同时开 `-V` 全量输出**：大量管道写入本身会改变接收线程时序。
6. **端口冲突**：`--port 5555` 若被占用，换端口重试；TCP 与 UDP 端口空间独立。
7. 真机优先用**空载**先取一次基线，再叠加负载；两次都用同一个从站二进制。

## 10. 复测之后的动作（按结论）

- **若为 A（生产降频）**：问题落在测试夹具与门禁设计上，需要（设计后再动手）：
  - 给用例加“最小帧率/最小帧数”断言，让退化变成显式失败；
  - 或改用不受调度影响的触发源（例如从站侧按墙钟补偿而非 `sleep_until`）；
  - 失败信息里输出从站 tick 与帧时间戳差值分布（现在只输出总帧数与首帧十六进制）。
- **若为 B（链路丢包）**：这是产品级问题，需在 `UdpTransport` 侧增加
  接收缓冲可配置（`SO_RCVBUF`）与丢包可见性（`SO_RXQ_OVFL`，Linux）并补用例。

---

## 附：本报告涉及的源码坐标

| 主题 | 位置 |
| --- | --- |
| 从站心跳与 tick 自增 | `examples/xcp_slave_demo/main.cpp:49-54, 887-905, 921-925` |
| 从站启动横幅（stdout） | `examples/xcp_slave_demo/main.cpp:874-877` |
| 队列容量 | `examples/xcp_slave_demo/main.cpp:48` |
| 失败用例与诊断输出 | `tests/xcp_slave_demo_integration_test.cpp:1457, 1526-1537, 1596-1601` |
| 临时目录清理 | `tests/xcp_slave_demo_integration_test.cpp:372-376` |
| 从站日志读取路径 | `tests/xcp_slave_demo_integration_test.cpp:1987-1993` |
| 日志编译开关 | `CMakeLists.txt:35, 71-83`；`include/libxcp/log.hpp:78-92` |
| DAQ 队列溢出 | `thirdparty/XCPlite/src/xcplite.c:516-526` |
| 发送队列溢出与线程节奏 | `thirdparty/XCPlite/src/xcpEthTl.c:706-711, 720-727, 716-756` |
| DTO 帧头与时间戳布局 | `thirdparty/XCPlite/src/xcplite.c:528-551` |
| 主站 setsockopt 现状 | `src/detail/socket/socket_ops_posix.cpp:183`；`socket_ops_win.cpp:212` |
| 主站 poll 事件驱动 | `src/detail/socket/event_poller_posix.cpp:61` |
