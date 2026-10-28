# XCP Master DAQ 事件来源解析实施记录

## 目的和范围

针对符号仅声明 DEFAULT_EVENT_LIST 或 CONSISTENCY_EVENT_LIST 时被固定事件准入策略拒绝的问题，按批准方案恢复事件事实的端到端透传及统一选择。未修改 `thirdparty/a2llib`，也未改变 ECU 运行时 DAQ 能力检查。

## 实施内容

- SDK 与桥接层透传 FIXED、DEFAULT、AVAILABLE 和 CONSISTENCY 四张事件表；INSTANCE 的嵌套测量叶子继承其事件事实。ABI 由 10 提升至 11，SDK 与所有消费者必须一起重建，不可混用旧 DLL。
- 主站按 FIXED → DEFAULT → CONSISTENCY 的顺序选择唯一事件；重复同号去重，事件号 0 有效。AVAILABLE 非空时检查 DEFAULT 和 CONSISTENCY 的子集一致性；多候选、矛盾和缺失 EVENT_CHANNEL 均拒绝。
- F6、watch 和事件绑定使用统一策略；界面说明事件来源。保留原有测量类型、运行时能力和安全检查。
- 新增或恢复事件表导出、事件选择、实例叶子继承及示例从站实际 DAQ 测试。

## 验证状态

Windows MSVC Release：启用 `LIBXCP_BUILD_TESTS=ON` 后清理并完整重建，370/370 构建步骤成功。将 TEMP/TMP 指向工作区后运行 `ctest --test-dir cmake-build-release -R 'DaqEvent|SymbolPolicy|XcpSlaveDemoIntegration|MeasurementPlanner' --output-on-failure`，67/67 通过，用时 17.93 秒。该结果仅覆盖所筛选测试，不代表全量测试通过。客户 A2L、实际 ECU、TUI 全链路、Linux/macOS 尚未验证；本次示例 DAQ 使用的是 XCPlite V2.2，不能作为 V1.2 DAQ 已验证的证据。示例 live DAQ 测试直接构造 DAQ 配置并调用底层接口，不经过主站 F6、watch、事件绑定或 MeasurementPlanner，不能作为主站交互全链路通过的证据。

## 已知依赖边界

当前 `a2llib` 已提供 DEFAULT 和 CONSISTENCY 的语法与 getter，不需要更新子模块。其 module-information-only 快速解析在某些布局下可能于 MOD_PAR 提前结束，此问题与本次全量解析测试分开，不能把本次结果外推到所有 A2L 布局。

解析器允许空事件列表；当前 DTO 仅保存向量，无法区分显式空表与未声明。空表的规范语义及此差异的影响尚未核实，不能声称覆盖该边界。
