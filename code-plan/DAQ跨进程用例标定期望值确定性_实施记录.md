# DAQ 跨进程用例标定期望值确定性 实施记录

## 1. 背景

`tests/xcp_slave_demo_integration_test.cpp` 的
`XcpSlaveDemoIntegrationTest.CrossProcessCalibrationChangesDaqOutput`
在 macOS CI 上连续三次红灯（PR #17 的三轮），而 Windows / Linux 一直绿。
`2613c78` 给该用例加了失败现场诊断（帧数 / 解码次数 / 首帧十六进制），
本轮从 CI 日志包中取到了真实现场，根因已可确定，不再是"无法复现的抖动"。

## 2. 现场证据

macOS 失败日志（`logs_110766492525.zip` → `build-macos/6_Test.txt`）：

```text
[  FAILED  ] XcpSlaveDemoIntegrationTest.CrossProcessCalibrationChangesDaqOutput
10 ms DAQ DTO 未观察到标定偏置 42.0；事件通道 0，已收 DTO 89 帧，
解码成功 3483 次，解码失败 0 次，首帧长度 16 字节，
内容 00 aa 00 00 22 d3 0a d3 00 00 00 33 33 73 45 40；
可能是相对基址、CalSeg 快照未生效或 DAQ 未启动
```

三点结论可以直接从这行日志读出：

1. **帧是收到的**（89 帧），且**解码全部成功**（解码失败 0 次），
   说明 UDP 通路、信封布局（4 字节头 + 32 位时间戳 + 8 字节 payload）都正确；
2. payload 8 字节 `00 00 00 33 33 73 45 40` 按小端 double 解出
   **42.89999997615814**，恰好等于 `42.0 + 0.89999997615814208984375`，
   后者是 `float(0.9)` 精确值 —— 也就是"标定偏置 42.0 已经生效，只是本帧
   输入不为 0"；
3. 断言要求 `std::abs(observed - 42.0) < 1e-9`，只有输入恰为 0 的帧才满足。

因此这不是标定未生效、也不是布局错误，而是**断言依赖了一个极稀疏的取值**。

## 3. 根因

从站示例的算法与输入（`examples/xcp_slave_demo/slave_model.cpp`）：

```cpp
// :83  输入在 0.0 … 9.9 之间循环，步长 0.1
measurements.input = static_cast<float>(tick % 100U) / 10.0F;
// :96-108  output = clamp(gain × (input + corrections[0]) + offset)
```

默认 `gain = 1.0`、`corrections[0] = 0`，用例只写了 `model.offset = 42.0`，
于是 `output = 42.0 + (tick % 100) / 10.0`，取值集合是
`{42.0, 42.1, …, 51.9}`，**只有 `tick % 100 == 0` 时输出正好是 42.0**，
即 100 Hz 下每秒出现 1 次、占全部帧的 1%。

macOS 那一轮 3 秒窗口只收到 **89 帧**（期望 100 Hz × 3 s = 300 帧），
恰好错过了全部 3 个"零输入"样本，用例因此超时失败。Windows / Linux
上帧几乎不丢（300/300），必然包含那 3 帧，所以一直通过。

帧率为什么只有约 30 Hz：从站示例的触发循环
（`examples/xcp_slave_demo/main.cpp:885-932`）每轮要做标定段快照、更新测量、
触发 DAQ、观察协议状态，然后在 `:931` `sleep_until(min(next_fast, next_slow))`；
一旦单轮耗时超过 10 ms，`:902-904` 的追平逻辑会把周期重新锚定，
实际触发率就下降。CI 上 `ctest -j 4` 跑在核数不多的 runner 上，CPU 争抢
足以触发这种降频。**这是示例夹具的时序特性，不是 libxcp 的产品缺陷**；
该用例也从未断言帧率，所以帧率下降本身不会失败，只是让"1% 取值"策略失效。

## 4. 改动

只改 `tests/xcp_slave_demo_integration_test.cpp`，不碰产品代码：

| 位置 | 内容 |
| --- | --- |
| `:1516-1537` | 在写 `model.offset = 42.0` 之前，先把 `model.gain` 写成 `0.0`，并按既有的 `WriteMemoryBytes` + `ReadMemoryBytes` + `EXPECT_DOUBLE_EQ` 模式回读校验 |
| `:1595-1601` | 失败消息补充"增益已清零，任意采样应恒为 42.0"的说明 |

选择 `gain = 0.0` 的理由：

- `ValidateCalibrationParameters`（`examples/xcp_slave_demo/slave_model.cpp:52-56`）
  允许增益处于 `[0, 10]`，`0.0` 合法，从站不会因此进入诊断码非零分支；
- 清零后 `output = clamp(0 × (input + corr0) + 42.0) = 42.0` 对**任意** tick 成立，
  任一帧都能证明"标定写入经 CalSeg 快照进入了测量计算"，对丢帧完全鲁棒；
- 证据力没有下降：若 CalSeg 快照未生效，输出仍会是未标定的 `0.0 … 9.9`，
  断言照样失败；
- 增益自身的语义不会因此失去覆盖：`tests/xcp_slave_demo_model_test.cpp:29`
  已有 `gain = 2.0` 的模型层用例，`lower_limit >= upper_limit` 等校验也在
  同文件 `:71` 附近覆盖。

## 5. 验证

环境：Windows 10 + MSVC 2022（`cmake-build-vs`，`Release`）。

### 5.1 构建

```bash
cmake --build cmake-build-vs --config Release --target XcpliteIntegration
```

成功。仅剩既有的 glog 警告与 `:1564` 的 `C4267`（该行是既有代码
`basic_signed_8_address … signed_8_value.size()`，非本次改动引入）。

### 5.2 定向重复执行

```bash
XcpliteIntegration.exe \
  --gtest_filter='XcpSlaveDemoIntegrationTest.CrossProcessCalibrationChangesDaqOutput' \
  --gtest_repeat=10 --gtest_color=no
```

结果：**10/10 通过，0 失败**；单次耗时从原先约 1.4 s 降到约 0.6 s
（原先要等到那个稀疏的 42.0 样本才返回）。

### 5.3 全量集成用例

```bash
XcpliteIntegration.exe --gtest_color=no
```

结果：**61/61 通过**。

### 5.4 全量回归

见 `ctest --test-dir cmake-build-vs -C Release -j 4` 结果（本记录第 6 节补充）；
仅测试文件改动，未触及 `src/`、`include/`、`tools/`。

## 6. 全量回归结果

```bash
ctest --test-dir cmake-build-vs -C Release -j 4
```

结果：**1158 项中 99% 通过（1156 通过），2 项失败**，均为
`XcpMasterRenderBenchTest` 的性能基准，与本次改动无关（本次只改
`tests/xcp_slave_demo_integration_test.cpp`，两者不在同一可执行文件）：

| 用例 | 失败原因 |
| --- | --- |
| `XcpMasterRenderBenchTest.RendersFrameWithFieldSizeCandidateList` | 单帧渲染 1537.6 ms，超过绝对上限 1500 ms；日志同时给出"本机每行 16.86 µs、参考每行 0.86 µs、能力系数 19.69" |
| `XcpMasterRenderBenchTest.SearchesAndFiltersLargeA2lWithinBudget` | 全库检索 331.7 ms，超过 `kSearchReferenceMs × 3.0 = 270 ms` |

这两项是**本机算力问题，不是回归**：

- 该用例的判定顺序在 `tests/xcp_master_render_bench_test.cpp:205-239` 中写明
  "突破 `kFrameRenderHardLimitMs` 一律失败，慢机不豁免"，而本机能力系数
  11–20 倍于参考机，必然超限；单独复跑两次分别为 1522.0 ms 与 1719.9 ms，
  能力系数 13.61 / 11.20，同样失败；
- 检索基准同样只按参考机的绝对毫秒判定，单独复跑两次为 310.8 ms（失败）
  与 194.6 ms（通过），说明它是随机器负载波动的量级判定；
- 两者都不涉及本次改动的任何代码路径，且此前多轮全量回归中都出现过同类
  单点失败（例如本分支 `2613c78` 那轮只失败 `RendersFrameWithFieldSizeCandidateList`）。

## 7. 遗留与限制

1. **macOS 侧仍需由 CI 复验**。本机无法复现 macOS 的 30 Hz 现象，
   因此"新断言在丢帧 70% 时仍然通过"是由推理与本地重复执行保证的，
   最终证据是 PR #17 的 macOS 作业转绿。
2. **帧率下降本身未做断言**。若将来希望把"DAQ 触发率"也纳入门禁，
   需要另立用例（例如统计 3 秒内 DTO 数并设下限），这属于新需求，
   未包含在本次改动中。
3. macOS 那轮的 89/300 帧率只来自单次日志，未做跨平台系统性测量；
   如需区分"示例循环降频"与"XCPlite 发送节流"，需要在两侧加时间戳采样。
