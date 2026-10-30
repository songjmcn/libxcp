# DAQ 帧率遗留问题 — macOS 真机复测记录

- 日期：2026-10-29
- 分支：`feat/tcp-a2l-wire-evidence`（HEAD `db00031`）
- 对应分析文档：`code-plan/DAQ帧率遗留问题_分析与macOS复测方案.md`
- 性质：**只读复测**。未修改任何产品代码、测试代码与第三方库；
  唯一的临时产物是探针 `.tmp/probe/daq_rate_probe.cpp`（未入库）。

---

## 1. 复测环境（文档步骤 0）

| 项 | 值 |
| --- | --- |
| 系统 | macOS 27.0.1（Build 26A434） |
| 机器 | Apple Silicon（主机名 `Alexs-Macbook-M4.local`） |
| 核心 | `hw.ncpu = 14`、`hw.physicalcpu = 14` |
| 内存 | 51,539,607,552 B（48 GiB） |
| 运行时长 | up 11 days |
| 复测开始时的负载 | load average 5.65 / 5.78 / 5.43（非空载机器，仍有其它任务） |
| 构建配置 | `cmake-build-release`：Release + `LIBXCP_BUILD_TESTS=ON` + `LIBXCP_BUILD_EXAMPLES=ON` + `LIBXCP_BUILD_TOOLS=ON` + **`LIBXCP_ENABLE_LOG=ON`** |

与 CI 的差别：CI 是 3~4 核共享 runner，本机是 14 核 M4。这一差别本身是本记录
结论的一部分——本机很难人为造出与 CI 等价的资源争用。

## 2. 全量测试（文档步骤 1/4 的前置）

构建：`cmake --build cmake-build-release -j 8` → 成功（151 个目标，无 error）。

全量用例（**不排除任何用例**，包含渲染基准与 A2L 规模/耗时断言，串行执行）：

```bash
ctest --test-dir cmake-build-release --output-on-failure --timeout 900
```

| 指标 | 结果 |
| --- | --- |
| 用例总数 | **1158** |
| 通过 | **1158（100%）** |
| 失败 | **0** |
| 总耗时 | **336.79 s** |
| 未运行 | 3 项，均为预期：`TcpStreamDecoderFinish.HeaderOnlyIsTruncated`（Disabled）、`AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/2-byte`（Skipped）、`SnapshotTest.RejectsRootWritableByOthers`（Skipped） |

其中最慢 5 项均为示例程序用例：`ExampleMeasurementDemo` 10.12 s、
`ExampleCppDemoMeasurement` 9.88 s、`ExampleCppDemoCalibration` 9.80 s、
`ExampleXcpMasterUdp` 5.40 s、`ExampleXcpIntegrationDemoCppDemo` 5.33 s。

`XcpSlaveDemoIntegrationTest.*` 共 **24** 项全部通过，含目标用例
`CrossProcessCalibrationChangesDaqOutput`（#879）。

## 3. 目标用例重复运行（文档步骤 4）

```bash
cd /tmp/xcp_daq_repeat
/path/to/cmake-build-release/tests/XcpliteIntegration \
  --gtest_filter='XcpSlaveDemoIntegrationTest.CrossProcessCalibrationChangesDaqOutput' \
  --gtest_repeat=20 --gtest_color=no
```

结果：**20/20 全绿**，`exit=0`，无一次失败、无诊断行输出。

> 注意：该用例已按 `tests/xcp_slave_demo_integration_test.cpp:1526-1537` 的说明
> 把增益清零、期望值恒为 42.0，对丢帧鲁棒。**通过 20 轮只能证明稳定性，
> 不能证明帧率达标**——这正是分析文档 §2.4 记录的"门禁缺口"。

## 4. 从站循环速率（文档步骤 2/3，心跳法）

从站：`xcp_slave_demo --bind 127.0.0.1 --port 5555`（`LIBXCP_ENABLE_LOG=ON` 构建），
运行约 24 s 后 `SIGINT`，读 `logs/examples/xcp_slave_demo/*.glog`。

| 场景 | 心跳片段（`累计快事件次数`） | 5 s 增量 | 循环速率 |
| --- | --- | --- | --- |
| 空载 | 501 → 1001 → 1501 → 2001 → 2501 | **+500** | **100 Hz（健康）** |
| 叠加 4 个忙循环 | 501 → 1001 → 1501 → 2001（另一进程 5501） | **+500** | **100 Hz（健康）** |

判读：即便机器已有 load ≈ 6，从站的 10 ms 事件循环仍稳定在 100 Hz，
**未出现文档 §2.2 里“9 秒只推进 265 次事件”（≈29.4 Hz）的现象**。

## 5. 主站收帧率（关键实验，靠临时探针）

心跳只能证明"生产"，不能证明"链路"。为此新增临时探针
`.tmp/probe/daq_rate_probe.cpp`（不入库，编译进 `.tmp/probe/`）：加载示例生成的
A2L → 连接从站 → 对 `meas_fast_10ms` 事件配 1 条 ODT（带时间戳）→
`StartDaqSync` → 统计窗口内收到的 DTO 帧数、帧率达到间隔分布、前 3 帧首 8 字节。

| 场景 | 窗口 | 收到帧数 | 实测帧率 | 到达间隔 中位 / 均值 / 最大 |
| --- | --- | --- | --- | --- |
| 空载 | 3.05 s | 300 | **98.3 Hz** | 2.79 / 10.17 / 51.4 ms |
| 空载（清理后复测） | 3.05 s | 301 | **98.8 Hz** | 2.75 / 10.13 / 51.3 ms |
| 4 个忙循环 | 3.02 s | 300 | **99.2 Hz** | 3.21 / 10.01 / 51.5 ms |
| 16 个忙循环 | 3.03 s | 300 | **99.0 Hz** | 5.13 / 10.06 / 54.4 ms |
| 28 个忙循环（2× 核数） | 3.03 s | 300 / 301 | **99.1 / 99.4 Hz** | 5.6 / 10.0 / 53.3 ms |
| 14 个忙循环 + 从站降为后台 QoS（`taskpolicy -b`） | 4.35 s | **5** | **1.1 Hz** | 763 / 520 / 833 ms |

### 5.1 关键判读

1. 正常与 2× 核数争用下，**3 s 窗口稳定收满 300 帧**（理论值 300 = 100 Hz × 3 s），
   **零丢帧**；帧到达间隔均值 ≈10 ms、与 10 ms 事件周期一致。
   → 分析文档的**结论 B（链路/主站丢 70%）在本机不成立**。
2. 只有当从站被压到"几乎得不到 CPU"（后台 QoS + 忙循环）时，收帧率才塌到 1.1 Hz；
   该场景的帧到达间隔**最小值也有 179 ms**，不存在任何 10 ms 级正常间隔，
   且此时主站连 CONNECT 都开始超时失败。
   → 与**结论 A（生产者循环被拉长，帧根本没产生）**一致。
3. 该极端场景无法与 CI 的 89/300（≈29.7 Hz）直接对齐：本机是 14 核，
   CI 是 3~4 核共享，**没有等价的中间档可复现**。

## 6. 结论

| 问题 | 结论 |
| --- | --- |
| 本机能否复现"3 s 只收 89 帧" | **不能**。5 组不同负载下均为 98.3~99.4 Hz、300 帧 |
| 是否链路丢包（结论 B） | **本机无证据**。零丢帧；到达间隔与 10 ms 周期一致 |
| 是否生产降频（结论 A） | **本机仅在人造饥饿下成立**（1.1 Hz，间隔全 ≥179 ms）；正常与重负载下不成立 |
| CI 上 89/300 的定因 | **仍未定因**（与本机结果不冲突：CI runner 核数少、且有并发任务） |
| "帧率退化不再会失败"的门禁缺口 | **仍然存在**：全仓没有任何用例断言 DAQ 帧数/帧率下限（本次已复核） |

因此，"问题是否仍然存在"的准确表述是：

- **产品侧（链路丢包）**：在本机未复现，无证据支持；
- **测试门禁侧**：缺口仍在——`CrossProcessCalibrationChangesDaqOutput` 因为增益清零
  而对丢帧鲁棒，若 CI 上再次退化到 30%，构建依旧为绿；
- **可观测性侧**：本次仍只能靠"心跳日志 + 自建探针"取得帧率，CI 工件内
  依旧无法直接读出"循环速率"与"收帧数"（分析文档 §3 的结论未变）。

## 7. 后续建议（需另行裁决，本次未实施）

1. **补帧率下限断言**（对齐分析文档 §10 的"若为 A"分支）：在现有 DAQ 用例里加
   `EXPECT_GE(received, 某下限)`；阈值要用 CI 实测校准（建议先取理论值的 50%，
   即 3 s ≥150 帧），并在失败信息里输出 `Diagnostics()` 已有的帧数/间隔分布。
2. **让 CI 能自证降频**：把从站心跳从 `LIBXCP_LOG_INFO` 改为始终写 stdout
   （stdout 会被 CTest 捕获并进 CI 日志），这样失败时可直接对比"事件数 vs 收帧数"。
3. **失败诊断增强**：`Diagnostics()` 增加"相邻 DTO 时间戳差值分布"
   （现在只有总帧数与首帧十六进制），失败即可区分 A / B，不必再上真机。
4. 若将来在 CI 上确实出现 B：再考虑 `UdpTransport` 暴露 `SO_RCVBUF` 配置与
   Linux `SO_RXQ_OVFL` 丢包计数（分析文档 §10 的 B 分支）。

## 8. 附：本次采集物位置

| 采集物 | 位置 |
| --- | --- |
| 全量 ctest 输出 | `/tmp/daq_ctest_full.log` |
| 目标用例 20 轮日志 | `/tmp/xcp_daq_repeat/repeat20.log`（含主站 INFO 日志 `logs/tests/XcpliteIntegration/`） |
| 从站心跳日志（空载/4 负载） | `/tmp/xcp_daq_probe/logs/examples/xcp_slave_demo/` |
| 收帧率探针 | `/Users/alex/workspace/project/libxcp/.tmp/probe/daq_rate_probe.cpp`（未入库） |
| 各场景 A2L 与从站进程目录 | `/tmp/daqprobe/{a2l,service,service2..service5}` |
