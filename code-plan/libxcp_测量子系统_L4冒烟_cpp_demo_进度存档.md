# libxcp 测量子系统 · cpp_demo L4 冒烟进度存档（2026-09-29 暂停点）

> 用途：会话中断后的接续锚点。下次恢复时读本文档即可继续，无需重查历史。

## 0. 关联 goal 与恢复方式

- Goal id：`goal-0fa307de-5cdd-432c-814a-d089fe6ae726`（objective：measurement_demo 参数化改造 + cpp_demo ≥10min 九步冒烟 + 证据 MD；AGENTS.md 禁 git commit）。
- 恢复：向助手说"继续 L4 冒烟收尾"→ `get_goal` 取当前 revision → `update_goal action resume`。

## 1. 任务完成度（截至存档时）

| 子项 | 状态 |
|---|---|
| measurement_demo 参数化（project/symbols/event/a2l-name/mode/expect/range 全进 CLI） | ✅ 已落盘（947 行版） |
| 默认参数零回归（xcp_test_slave 旧行为） | ✅ ctest -R ExampleMeasurementDemo Passed 5.27s |
| cpp_demo 30s 快验 | ✅ 30/30 EXIT=0（build-v09\l4quick30.txt，311 行） |
| **cpp_demo 600s 正式冒烟** | ✅ **30/30 EXIT=0**（build-v09\l4smoke.txt，311 行） |
| 缺陷②（Stop→复 Start 控制流）修复 | ✅ 已落盘并实证（复 Start 3s 连续收流/回绕） |
| 缺陷①（temperature 值域异常）定性 | ✅ 定性完成（根因在上游 a2l-sdk/XCPlite，见 §4） |
| **证据 MD（code-plan/ 正式记录）** | ⛔ 未写 —— 剩余工作 1 |
| **全量 ctest 回归复跑（可选加固）** | ⛔ 未做 —— 剩余工作 2 |
| goal complete | ⛔ 待上述两项 |

## 2. 600s 正式冒烟关键数字（证据源 build-v09\l4smoke.txt）

- 复现命令（cwd=仓库根，PATH 前置 build-v09\examples\measurement_demo\Release 与 build-v09\Release 以解析 liba2l.dll）：
  `measurement_demo.exe --slave build-v09/examples/xcp_master_udp/Release/cpp_demo.exe --port 5555 --project cpp_demo --mode dynamic --symbols temperature,speed,counter,sum,SigGen1.value_,SigGen2.value_ --range speed=0:250 --range counter=0:65535 --range sum=-100:100 --range SigGen1.value_=-100:100 --range SigGen2.value_=-100:100 --seconds 600 --run-dir build-v09/l4smoke`
- 事件通道逐符号取证：temperature/speed/counter/sum=2（mainloop）、SigGen1.value_=0、SigGen2.value_=1 ⇒ 3 条独立流（daq_list 0/1/2）。
- 统计：received=1705430（≈2842 DTO/s）dropped=0 decode_err=0 wraps=420。
- 回绕自洽：wraps=420 ∈ [167,676]（3 流×600s÷4.295s≈419 圈预测，实测吻合）——L4 第 7 步核心证据。
- Stop→复 Start 3s：received 1705430→1713543、wraps 420→423 连续不清零（第 8 步）；Disconnect 完成（第 9 步）。
- 动态值变化证据（[raw] 行）：speed 观测 [3.82,237.67]（指数逼近 250 形态）、counter 0..1000 循环、SigGen1 ±12.5（TRIANGLE）、SigGen2 ±80（SINE）、sum=SigGen1+SigGen2 叠加区间自洽；每符号 keys=8 个不同原始值。
- temperature：值域断言按计划去除（缺陷①），保留覆盖+变化性：raw keys 0x32..0x39 单字节正确、seen=valid、多值变化 OK。
- 九步对照：Connect/GetDaqProcessorInfo/GetDaqResolutionInfo/GetDaqEventInfo 回退路径/信封 RelativeByte/Prepare 三流规划/600s 零错+回绕自洽/复 Start 连续/Disconnect——全部过。

## 3. measurement_demo 参数化落地清单（examples/measurement_demo/）

- measurement_demo.cpp（947 行）：Options 新字段 project="xcp_test_slave"、symbolsSpec="g_basic_u32,g_basic_f32"、event="testev"、a2lName、dynamicMode、expect=0xDEADBEEF、ranges map、port 5556——零参数=v0.9 旧行为；新 CLI --project/--symbols/--event/--a2l-name/--mode fixed|dynamic/--expect 0xhex/--range NAME=min:max（可重复）。
- ResolveSymbolEventChannel（:416-444）：按上传 A2L 的 `/begin MEASUREMENT <name>` 行内 EVENT 0x 逐符号取证，回退 --event 命名事件，再无则 exit 2。
- FrameSink（:223-352）：有界聚合（kKeepFrames=2000、kMaxKeys=8）+ Lists() 流数 + SawU64 + DumpRawKeys(:360)；长窗（≥30s）断言 dropped==0、wraps>0、dynamic 回绕自洽 [0.4×,1.6×+2]×(流数×s/4.294967296)。
- 控制流修复（缺陷②）：session.Stop()→Running 断言→dynamic 复 Start 3s（slave 存活期）→master->Disconnect()+!IsConnected 断言→slave.Stop()（:908-937）；此前 Disconnect 二次调用抛 src/session.cpp:170 InvalidState。
- 端口脏检（:619-641）：目标端口必须无人应答才开跑。
- CMakeLists.txt（62 行）：新增 XCP_104_AML_FILE 注入（:37-40，照抄 xcp_master_udp/CMakeLists.txt:18,41 先例）——cpp_demo 的 A2L 含 `/include "XCP_104.aml"`，demo 在 run-dir 预置副本保证桥接可解析。
- git 工作区：仅这两文件 M（numstat 6/1 与 561/72）；核心库/测试零改动 → 参数化不动冻结面。

## 4. 缺陷①终定性（不改 thirdparty，口径已定）

- 现象：DAQ 路径 temperature 物理值 = raw×(−49)（raw50→−2450）。
- 根因两层（均在上游）：
  1. `thirdparty\a2l-sdk\a2lbridge\src\compu_method_eval.cpp:355` 正向 `c.f + raw*c.c + raw*c.o` ⇒ offset 被乘进 raw，违反 ASAM F(x)=factor·x+offset；
  2. `thirdparty\a2l-sdk\src\liba2l_export.cpp:589-592` 按 ASAM spec 序映射 COEFFS_LINEAR（lin[0]=offset、lin[1]=factor），而 XCPlite 生成端 `thirdparty\XCPlite\src\a2l.c:931` 实际 factor 在前（铁证：a2l_test_expected.a2l:235 注释 "uint8*2-50" ↔ "COEFFS_LINEAR 2 -50"）。cpp_demo 写 "1 -50" ⇒ 桥接读成 C=−50、O=1 ⇒ raw×(1−50)。
- libxcp 侧无 bug：raw 字节正确（1B、0x32..0x39）、定标计算忠实于桥接输出。建议上游整改（a2l-sdk 求值式 + XCPlite 系数序），本仓库不动。
- 注：xcp_master_udp 直址读路径不经 COMPU_METHOD 乘式，两处口径不冲突。

## 5. 剩余工作（下次会话按序执行）

1. **写证据 MD**：`code-plan/libxcp_测量子系统_L4冒烟_cpp_demo_记录.md` —— 内容=本文 §2 数字 + 九步对照表 + 缺陷①②记录 + 参数化说明（可从本文扩写，证据文件 l4smoke.txt/l4quick30.txt 在盘）。
2. （可选加固）build-v09 全量 ctest 复跑确认 466/466（参数化只动 examples 目录，理论无影响；上轮全量为 466/466 是参数化前）。
3. `get_goal` → `update_goal action complete` 收尾。

## 6. 环境备忘（避坑）

- 构建：`cmake --build build-v09 --config Release` 串行；pwsh 偶发空 stdout（历史多轮踩坑）——优先 `write` 工具落文件、证据尽早收齐。
- 手工跑 demo 必须 PATH 前置 `build-v09\examples\measurement_demo\Release;build-v09\Release`（liba2l.dll），run-dir 用显式可写路径。
- 5555 端口若被残留 cpp_demo 占用，demo 预检会拒跑（先杀进程）。
- 时间戳方言：XCPlite 族 4B tick（mode 0x0C）⇒ ≈4.295s 一回绕；10min≈140×流数 圈为预期，wraps>0 是正确实然。
