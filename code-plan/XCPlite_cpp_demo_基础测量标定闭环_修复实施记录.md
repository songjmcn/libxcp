# XCPlite cpp_demo 基础测量标定闭环——修复实施记录

## 1. 结论

本轮完成 R0–R4：修复标准 A2L `COEFFS_LINEAR` 物理换算；以原版 vendored XCPlite `cpp_demo` 验证 temperature 同帧 raw/physical 对应；在 DAQ 持续运行中写入 `counter_max`，观察算法效果改变，再恢复原值并验证恢复。Windows Release ON/OFF 构建与全量 CTest 通过。

证据边界：这是本机 Windows、UDP Loopback、仓库自带 XCPlite `cpp_demo` 的定向证据，不是独立 ECU/Slave、非 XCPlite、Linux/macOS、CANape 或全量 XCP 互操作认证。

## 2. 批准与规范结论

用户批准了 R1 设计所列 `thirdparty/a2l-sdk` 修改；R3 方案采用现有 `kParameters.counter_max`、安全测试阈值 20、同一 DAQ 会话中的写入/效果/恢复闭环。

R0 核证：标准 LINEAR 为 `PHYS = a * INT + b`，A2L `COEFFS_LINEAR a b` 依次为 factor、offset。cpp_demo 用 `(1,-50)` 输出 `COEFFS_LINEAR 1 -50`，raw 50 应为 0°C。原 SDK 将值错映射并把 offset 加进斜率，算出 -2450。规范交叉来源和逐层数据流见实施计划 §4 R0（其中注明未能直接抓取官方 ASAM 下载正文）。非标准旧 `COEFFS` fallback 保持原实现映射，未宣称它与标准化语义一致。

## 3. 实施内容

### R1：LINEAR 修复及独立 oracle

- `thirdparty/a2l-sdk/src/liba2l_export.cpp`：标准 `COEFFS_LINEAR` 首值映射到比例 C，次值映射到加法偏移 O，F=0；保留旧 `COEFFS` fallback 行为。
- `thirdparty/a2l-sdk/a2lbridge/src/compu_method_eval.cpp`：正算为 `F + C*raw + O`，逆算为 `(physical-F-O)/C`；零比例不允许逆算。
- 修正 `thirdparty/a2l-sdk/include/liba2l/liba2l_api.hpp`、`thirdparty/a2l-sdk/a2lbridge/include/libxcp/a2l/a2l_types.hpp` 的 LINEAR 语义注释。
- `tests/a2l_gen/gen_a2l.py`、`tests/a2l_gen/golden_spec_basic.json`、`tests/a2l_gen/golden_spec_convert.json`：按 factor/offset 生成 A2L 及预期值；加入独立常量样例。`tests/a2l_golden_test.cpp` 断言 factor/offset、温度 raw 0/50/150→-50/0/100、factor 2、小数比例、负比例及零比例不可逆，并将增加后的 fixture 符号数更新为 15。

### R2：原版 cpp_demo 测量正确性

- `examples/measurement_demo/measurement_demo.cpp`：新增同一个 `MeasurementSample` 中 raw 字节和物理值配对检查，按独立公式断言 temperature `PHYS=raw-50`，避免跨 DTO 时刻误比较。
- `examples/measurement_demo/CMakeLists.txt`：加入串行 CTest `ExampleCppDemoMeasurement`，依赖并启动原版 `cpp_demo`，不以 `xcp_test_slave` 代替；校验 temperature、speed、counter、sum、两路信号样本及配置的值域/启停路径。
- `ExampleCppDemoMeasurement` 专项通过；完整 ON CTest 最终运行中该项通过（9.84s）。

### R3：在线标定→算法效果→DAQ→恢复

- 同一 demo 增加 `--calibration-probe` 专项模式。测试从本次上传 A2L 解析并验证 `cpp_demo::kParameters.counter_max` 是标量 U16，读取并保存原始两字节，先观察 counter 大于 20；DAQ 不停流时仅写一次 20，读回并观察 counter 在 0..20 内至少两次回绕；恢复原始 bytes、读回，并在 DAQ 中再次证明 counter 大于 20。
- 使用隔离运行目录；仅删除该测试目录的 `cpp_demo_V201.bin` 以确保默认初值。恢复异常尽力处理；写入结果不确定时查询状态而不重放 mutator。验证包含连续帧/回绕条件，不以固定 sleep 作为唯一通过判据。
- 新增串行 CTest `ExampleCppDemoCalibration`。专项通过；完整 ON CTest 最终运行中该项通过（9.72s）。专项日志：原始 `counter_max=1000`、下调后读回 20、至少两次回绕、恢复读回原 bytes 且 counter 再次超过 20；采集 `received=3896 dropped=0 decode_err=0`。这是本地 `cpp_demo` 运行效果证据。

## 4. Release 构建与测试

工具链：Windows / Visual Studio 17 2022（MSBuild 17.14.60+43b635718），Release，串行构建。

### SDK 依赖

- 清理 SDK Release 输出后运行 `pwsh -File thirdparty/a2l-sdk/build-sdk.ps1 -BoostRoot C:\boost\lib -Config Release -OutRoot build/liba2l-prepared`。
- Boost 1.86.0；a2llib revision `c31057498555cbb29ab48e718329ca3163c10221`。
- 准备根 `build/liba2l-prepared/msvc-x64-release`；运行时 DLL 与构建使用的 Release 依赖一致。

### ON

- 配置：`build-v09`，A2L、XCPLITE、examples、tests 均 ON。
- 执行 clean 后完整 Release build 成功：`cmake --build build-v09 --config Release -j 1`。
- 最终完整 CTest：515 项，514 passed、1 skipped、0 failed，65.86s。唯一 skip：`AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`。
- 透明记录：SDK 最终重建后首次全量 CTest 中，未修改的 `UdpTestSlaveFault.ResponseCounterStartsFreshAfterEachInjection` 曾在 `tests/udp_test_slave_test.cpp:469` 超时，原文为 `WaitUntil([&] { return ep.recvPacket(resp, ip, port); })` 实际 false，提示“取消注入后应恢复应答”。单项复跑 1/1 通过，随后完整复跑 515 项全部通过。该事件未通过删测试或改测试规避。

### OFF

- 配置：独立 `build-p0-off`，A2L、XCPLITE、examples OFF，tests ON。
- clean 后完整 Release build 成功；CTest 460 项，459 passed、1 skipped、0 failed，30.36s。唯一 skip 为相同的 AG 参数化项。

构建输出还报告若干 MSVC C4244/C4267/C4100 警告（A2lE2E、XCPlite A2L read、Golden 和 cpp_demo 源）；本轮未以关闭警告掩盖它们。完整回归的精简结论与瞬时失败首跑/复跑结果以上均保留。

## 5. 历史记录处置与能力边界

- `code-plan/libxcp_测量子系统_L4冒烟_cpp_demo_记录.md`、`code-plan/libxcp_测量子系统_L4冒烟_cpp_demo_进度存档.md` 中关于 `COEFFS_LINEAR` offset-first 的陈述是当时历史结论，不改写成“当时已知正确”；本轮计划 §7 标记由 R0 证据取代。
- 本轮验证了标准 LINEAR 小样本、cpp_demo 的 temperature/counter 基本测量和单参数在线标定闭环。未覆盖全结构编辑、SignalGenerator 全部参数、lookup 曲线/轴、分页持久化、STIM、Seed&Key 的独立 Slave 行为、异构信封、Linux/macOS 或商业工具。
- 详细实施计划及标准来源见 `code-plan/XCPlite_cpp_demo_基础测量标定闭环_迭代修复计划.md`。