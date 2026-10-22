# XCP Master DAQ 实例事件继承修复实施记录

## 目的与范围

针对生成的 `xcp_slave_demo.a2l` 中 `meas_fast`/`meas_slow` 的 `FIXED_EVENT_LIST` 声明在 INSTANCE 上、展开后的标量 MEASUREMENT 叶子缺失固定事件、导致 F6 与 `watch` 静态拒绝的缺陷。按已批准的 [修复计划](./xcp_master_DAQ实例事件继承修复计划.md) 修改本仓库 `thirdparty/a2l-sdk` 的 DTO 与桥接层；不改 `thirdparty/a2llib`、XCP 协议实现，也不让数组或标定叶子进入 DAQ。

## 修改内容

1. `thirdparty/a2l-sdk/include/liba2l/liba2l_api.hpp`：`StructInfoDto` 保存 INSTANCE 的固定 DAQ 事件列表。`thirdparty/a2l-sdk/src/liba2l_export.cpp`：按既有 IF_DATA 选择规则导出所有固定事件，包括事件号 0，不将缺失或多个事件猜成一个有效事件。
2. `thirdparty/a2l-sdk/a2lbridge/src/structure_layout.cpp`：展开结构、嵌套结构和结构数组时，仅让 INSTANCE 下的 MEASUREMENT 标量叶子继承实例事件；保留直接 MEASUREMENT 的既有规则；标定叶子保持不可观测。`tools/xcp_master/symbol_policy.hpp` 原有的 MEASUREMENT 标量、唯一固定事件、EVENT_CHANNEL 校验继续作为 F6/预览/`watch` 的统一入口。
3. `tools/xcp_master/main.cpp`：当用户显式使用默认 `generic` profile 且运行期布局检查失败时，仅给出“若确认是 XCPlite，请显式传 `--daq-profile xcplite`”的提示，不自动猜 profile、绕过校验或配置 DAQ。`tools/xcp_master/README.md` 补充本 Slave 的完整启动示例和保留的 DAQ 安全确认。
4. `tests/xcp_slave_demo_integration_test.cpp`：真实生成 A2L 并通过 SDK/桥接断言 `meas_fast.input`→事件 0、`meas_slow.basic.real_32`→事件 1、嵌套叶子继承、标定叶子不继承；还用生成文件的合法结构数组变体核验 `[0]`/`[1]` 嵌套标量继承；实际调用 `SymbolDaqObservationUnsupportedReason` 核验 Master 策略。生成 A2L 的合法变体覆盖缺失/DEFAULT、重复同一事件及多种不同事件时的准入/拒绝；EVENT_CHANNEL 缺失的拒绝由已有 `tests/xcp_master_symbol_policy_test.cpp` 覆盖。从全库搜索结果按规范名建立既有 `EventBoundMeasurementDatabase` 事件绑定，并由 `MeasurementPlanner` 核验快慢叶子被规划到事件 0/1；内层 `A2lMeasurementDatabase` 的默认事件 0 不影响真实工具使用的包装层，无须修改适配器。跨进程 Slave 测试还直接从桥接的 `SymbolInfo` 叶子取得地址、地址扩展、字节宽度和固定事件，并以此配置 Master DAQ，验证快速/慢速 DTO 确实送达；新增独立跨进程测试使用真实 A2L、事件绑定、`xcplite` 运行期取证、`MeasurementSession::Prepare/Start`，核验快慢叶子均能产生有效解码样本。上游 A2L 语法不允许 TYPEDEF_MEASUREMENT 携带 IF_DATA，因此没有通过修改第三方解析器制造无法解析的冲突语料。
5. `tests/measurement_adapter_test.cpp`：补齐测试 Fake 对已有 `IA2lDatabase` 元素换算接口的实现，恢复相关回归套件构建；不改变真实适配器实现。

## 验证

- AppleClang Release：`cmake --build cmake-build-logging-release --target XcpliteIntegration xcp_master -j 4` 通过；`A2lGolden`、`A2lE2E`、`MeasurementAdapterTest`、`XcpMasterSymbolPolicyTest` 目标亦已通过增量构建；另外已重新构建用户原有的 `cmake-build-release` 中 `xcp_master` 目标以载入 SDK DTO/桥接修改。仅目标增量构建，非全仓 clean build。构建过程存在已知 `u8path` 弃用、GoogleTest `char8_t` 与重复静态库警告，不影响通过结果。
- `XcpSlaveDemoIntegrationTest.GeneratedA2lPropagatesFixedEventsToMeasurementLeaves`、`GeneratedA2lRejectsAmbiguousOrMissingFixedEvents` 及 `GeneratedA2lFeedsLiveMeasurementSession` 通过；最后一项从真实生成 A2L 经项目既有事件绑定、运行期 XCPlite profile、双事件 DAQ 规划和 MeasurementSession 解码，收到了快慢标量的有效样本。
- 修改的 C++ 文件执行 `clang-format --dry-run --Werror`；`git diff --check` 通过。
- 聚焦 CTest：`XcpSlaveDemoIntegrationTest`、`A2lMeasurementDatabase`、`EventBoundMeasurementDatabase`、`GenericMeasurementRuntimeProfile`、`XcpliteMeasurementRuntimeProfile`、`XcpMasterSymbolPolicyTest`、`A2lSmoke`、`A2lGolden`、`A2lE2E` 与 A2L 语料生成合计 **35/35 通过**。既有跨进程测试直接配置 DAQ；新增跨进程测试通过与 Master 相同的绑定包装层、XCPlite profile 和 `MeasurementSession::Prepare/Start`，但尚未驱动 TUI 的 F6/`watch` 与界面显示。
- 曾尝试包含端口争用夹具 `XcpliteSlaveProcessTest.OccupiedPortDoesNotSatisfyReadinessAndRetriesNextCandidate` 的大范围 CTest：卡在该用例，主动终止；此前完成的 28 项中 27 通过、1 项按 AG=1 预期跳过。随后排除该夹具的上述 30 项聚焦回归均通过，不将那次中断计为全部通过。

## 未验证与局限

- **没有宣称 Master TUI 已真实显示 DTO 数值。**试图以本地 `xcp_slave_demo` 和伪终端启动 `xcp_master` 进行交互联调时，当前环境 `pty.openpty()` 报 `OSError: out of pty devices`；尚无法验证 F6→`watch`→确认→DTO→UI 更新的整链路。用于该尝试生成的 A2L 在构建树 `cmake-build-logging-release/xcp_master_daq_probe_bd4d0a6e/`，端口 62414；Slave 在 PTY 分配失败前已启动，进程清单查询被当前沙箱拒绝，因此**不能确定它是否仍在监听**。若占用，请由用户在有权限的终端核查并停止该演示进程。
- 未验证 Windows/Linux、全仓干净构建或其他 ECU。SDK DTO 增加字段，依赖项目内 SDK 的二进制需重编译；不要混用新旧 ABI。
