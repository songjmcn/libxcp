# libxcp 测量子系统 · L4 冒烟（cpp_demo 现成从站）记录

日期：2026-09-29 ｜ 里程碑：参数化改造 + cpp_demo ≥10min 九步冒烟 ｜ 约束：不改 thirdparty、不 git commit

## 1. 背景与目标

v1.0 封板后，L4 真 ECU 冒烟清单（v0.9 记录 MD §6 九步）在 CI 之外留有人工验证缺口。经分析（对照取证见进度存档），`thirdparty/XCPlite/examples/cpp_demo` 与 `xcp_test_slave` 同属 XCPlite 协议方言族，且在三方面强于测试从站：

- 多事件、非零事件通道（SigGen1/SigGen2/mainloop 三条独立流）；
- 动态物理值（正弦 ±80、三角 ±12.5、temperature/speed/counter/sum 主循环 1ms 演化）；
- 多 ODT 拆分场景（loop_histogram 512B，本轮冒烟符号集有意避开，见 §6 局限）。

据此将 `examples/measurement_demo` 参数化，使其可直接指向 cpp_demo 执行 L4 九步冒烟。

## 2. 参数化改造（examples/measurement_demo/）

git 工作区仅两文件变更，核心库/测试零改动 → 不动 v1.0 冻结面：

- `measurement_demo.cpp`（947 行，+561/−72）：
  - Options 新字段：`project="xcp_test_slave"`、`symbolsSpec="g_basic_u32,g_basic_f32"`、`event="testev"`、`a2lName`、`dynamicMode`、`expect=0xDEADBEEF`、`ranges` map、`port=5556`——**零参数 = v0.9 旧行为**。
  - 新 CLI：`--project` / `--symbols`（逗号分隔）/ `--event` / `--a2l-name` / `--mode fixed|dynamic` / `--expect 0xhex` / `--range NAME=min:max`（可重复）。
  - `ResolveSymbolEventChannel`（:416-444）：按 UPLOAD 拉回 A2L 的 `/begin MEASUREMENT <name>` 行内 `EVENT 0x` **逐符号取证**，取不到回退 `--event` 命名事件，再无则 exit 2。
  - `FrameSink`（:223-352）：有界聚合（kKeepFrames=2000、kMaxKeys=8）+ 逐符号 seen/valid/原始值集/物理 min/max + `Lists()` 流数 + `DumpRawKeys`（:360，取证线宽形态用）。
  - 长窗断言（≥30s）：`dropped==0`、`wraps>0`、dynamic 模式回绕自洽 `wraps ∈ [0.4×, 1.6×+2] × (流数×秒 ÷ 4.294967296)`。
  - 控制流修复（缺陷②，见 §5）：`session.Stop()` → dynamic 复 Start 3s（slave 存活期，L4 第 8 步）→ `master->Disconnect()`（第 9 步）→ `slave.Stop()`（:908-937）。
  - 端口脏检（:619-641）：目标端口必须无人应答才开跑，防残留 slave 假通过。
- `CMakeLists.txt`（62 行，+6/−1）：注入 `XCP_104_AML_FILE`（:37-40，照抄 xcp_master_udp/CMakeLists.txt:18,41 先例）——cpp_demo 生成 A2L 含 `/include "XCP_104.aml"`，demo 在 run-dir 预置副本保证桥接 include 链可解析。

**零回归**：`ctest -R ExampleMeasurementDemo`（默认参数 = xcp_test_slave）Passed 5.27s。

## 3. 600s 正式冒烟（L4 九步，EXIT=0）

复现命令（cwd=仓库根，PATH 前置 `build-v09\examples\measurement_demo\Release;build-v09\Release` 解析 liba2l.dll）：

```
measurement_demo.exe --slave build-v09/examples/xcp_master_udp/Release/cpp_demo.exe
  --port 5555 --project cpp_demo --mode dynamic
  --symbols temperature,speed,counter,sum,SigGen1.value_,SigGen2.value_
  --range speed=0:250 --range counter=0:65535 --range sum=-100:100
  --range SigGen1.value_=-100:100 --range SigGen2.value_=-100:100
  --seconds 600 --run-dir build-v09/l4smoke
```

证据文件：`build-v09/l4smoke.txt`（311 行）。结果 **30 项断言 0 失败，SMOKE_EXIT=0**。

| 九步（v0.9 MD §6） | cpp_demo 实然证据 |
|---|---|
| 1 Connect→SessionParameters | Slave pid=42424，CONNECT OK，UPLOAD A2L 15269 字节 |
| 2 GetDaqProcessorInfo | DYNAMIC+TIMESTAMP 位生效（Prepare 走动态落表路径） |
| 3 GetDaqResolutionInfo→SetTimestampUnit | 4B tick（mode 0x0C，unit=1ns 注入） |
| 4 GetDaqEventInfo 回退路径 | nullopt ⇒ 逐符号 EVENT 0x 取证：temperature/speed/counter/sum=**2**（mainloop）、SigGen1.value_=**1**、SigGen2.value_=**0**（通道号随事件注册次序浮动，本轮与 30s 快验相反——取证必须运行时做，不能写死） |
| 5 SetEnvelopeMode 按实然 | RelativeByte + FirstOdtOnly=true，decode_err=0 反证识别字段正确 |
| 6 Prepare 规划核对 | 三流 daq_list 0/1/2（ALLOC_DAQ=3 与 slave 日志吻合），首 ODT 帧带有效时间戳 PASS |
| 7 Start ≥10min | **received=1705430（≈2842 DTO/s）、dropped=0、decode_err=0、wraps=420**；回绕自洽 420 ∈ [167,676]（3 流×600s÷4.295s≈419 圈预测，实测吻合） |
| 8 Stop→复 Start 连续性 | received 1705430→1713543、wraps 420→423（跨 Stop 不清零，L4 第 8 步） |
| 9 Disconnect | 断连 OK，无二次 Disconnect 异常 |

动态值变化证据（[raw] 行，每符号 keys=8 个不同原始值）：

| 符号 | 观测物理值域 | 期望形态核对 |
|---|---|---|
| speed | [0, 245] | 指数逼近 250、>245 归 0 ✓（值域断言 PASS） |
| counter | [0, 1000] | 0..1000 回绕循环 ✓ |
| sum | [-92.4997, 88.5953] | =SigGen1+SigGen2 叠加，理论 ±92.5 ✓ |
| SigGen1.value_ | [-12.5, 12.5] | TRIANGLE ampl 12.5 ✓ |
| SigGen2.value_ | [-80, 80] | SINE ampl 80 ✓ |
| temperature | raw keys 0x32..0x39（**单字节 1B，线宽正确**），phys [-7350, 0] | 覆盖+变化性 PASS；值域断言按计划去除（缺陷①，§5）；-7350=150×(−49) 与定性公式精确吻合 |

## 4. 30s 快验（前置轮次）

`build-v09/l4quick30.txt`（311 行）：同参数 `--seconds 30`，30/30 EXIT=0（received=88084、wraps=21 ∈ [8,39]）。快验发现并修复缺陷②后，正式 600s 一轮全绿。

## 5. 缺陷记录与定性

- **缺陷②（demo 控制流 bug，已修）**：主路径 `slave.Stop()` 在 dynamic 复 Start 块之前执行 ⇒ 复 Start 无流且二次 `Disconnect()` 抛 `src/session.cpp:170` InvalidState。修复：调整 :908-937 顺序（复 Start 期间 slave 存活，Disconnect 在 slave.Stop 之前）。600s 冒烟实证生效。
- **缺陷①（temperature 物理值 = raw×(−49)，定性=上游问题，不改 thirdparty）**：
  - libxcp 侧无 bug：raw 字节解码正确（1B、0x32..0x39 = uint8 温度真值），定标忠实执行桥接输出。
  - 根因两层（均在 SDK 上游）：
    1. `thirdparty\a2l-sdk\a2lbridge\src\compu_method_eval.cpp:355` 正向 `c.f + raw*c.c + raw*c.o` ⇒ offset 被乘进 raw，违反 ASAM F(x)=factor·x+offset（:399 逆解同口径）；
    2. `thirdparty\a2l-sdk\src\liba2l_export.cpp:589-592` 按 ASAM spec 序映射 COEFFS_LINEAR（lin[0]=offset、lin[1]=factor），而 XCPlite 生成端 `thirdparty\XCPlite\src\a2l.c:931` 实际 **factor 在前**（铁证：`thirdparty\XCPlite\test\a2l_test\a2l_test_expected.a2l:235` 注释 "Temperature as uint8*2-50" ↔ `COEFFS_LINEAR 2 -50`）。cpp_demo 写 `1 -50` ⇒ 桥接读成 C=−50、O=1 ⇒ raw×(1−50)。
  - 处置：冒烟保留 temperature 覆盖/变化性断言、去除值域断言；建议上游整改（a2l-sdk 求值式 + XCPlite 系数序两处，任改其一即自洽）。
  - 注：`examples/xcp_master_udp` 直址读路径（:550 注释"物理 0..150"）不经 COMPU_METHOD 乘式，与本 DAQ 换算路径口径不冲突。

## 6. 覆盖边界（cpp_demo 不能替代真 ECU 的项）

- 异构信封方言（非 RelativeByte/非首 ODT 时间戳布局）——同族从站验证不到 decoder 的其他分支；
- 权威 tick 单位码表（R13 无表可查，unit_ns 由注入决定）；
- 总线时序/真实内存映射/厂商私有 GET_DAQ_EVENT_INFO 载荷字段；
- 多 ODT 拆分（loop_histogram 512B）本轮符号集未纳入——L2 已覆盖拆分逻辑，可下轮以 `--symbols` 加入实测。

## 7. 结论

measurement_demo 参数化完成且默认零回归；cpp_demo 现成主从组合可执行 L4 九步中除"异构方言"外的全部步骤，600s 长采回绕自洽、零丢弃、零解码错误、动态值逐符号可证。**L4 冒烟在 XCPlite 同族等价意义上闭环**；真硬件 ECU 的差异化验证仍按 §6 清单留待交付现场。

证据索引：`build-v09\l4smoke.txt`（600s 正式）、`build-v09\l4quick30.txt`（30s 快验）、`build-v09\l4smoke\cpp_demo_uploaded.a2l`（UPLOAD 落盘）、进度存档 `libxcp_测量子系统_L4冒烟_cpp_demo_进度存档.md`。

## 8. 后续规范更正（保留历史结论）

后续 R0 核证更正本记录 §5 缺陷①中的系数顺序判断：ASAM LINEAR 为 `PHYS=a*INT+b`，`COEFFS_LINEAR a b` 顺序是 factor、offset。XCPlite 写端按此顺序输出；旧 SDK 导出器把首值映射为 O、次值映射为 C，Bridge 正/逆算也错误地将 O 纳入斜率。本轮修复标准 `COEFFS_LINEAR` 映射与公式，并用原版 cpp_demo 同帧 temperature 断言及 Windows Release ON/OFF 回归验证。历史记录描述的是当时判断，不代表本轮修复前结论正确；规范证据和完整数字见 `code-plan/XCPlite_cpp_demo_基础测量标定闭环_修复实施记录.md`。
