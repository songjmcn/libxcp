# 完整测量与标定集成示例 — 实施计划

## 状态

设计已由用户于 m08932 选择“通用集成示例 + cpp_demo 可运行配置”，并于 m08944 批准。代码、README、参数安全门及 Windows Release 验证已完成；执行记录见 `code-plan/XCP_完整测量标定集成示例_实施记录.md`。

## 目标与边界

为后续集成人员提供可编译、可复制改造的统一 XCP 测量/标定示例，覆盖 libxcp 当前已公开的测量与标定 API；示例必须区分协议能力、目标 ECU 能力和本地 cpp_demo 已验证行为。此工作不是宣称支持完整 XCP 1.3 标准。

明确不覆盖：STIM、Flash Programming、NVM 持久化、未实现的 Slave 方言以及 CANape/INCA 等商业工具认证。Seed&Key 算法由集成人员以回调注入，不提供虚假/厂商密钥。

## 代码设计

新增 `examples/xcp_integration_demo/`：一个 C++20 可执行程序和 README。示例包括：

1. UDP 连接、CONNECT/状态和可选 DAQ/CAL-PAG 能力查询；
2. 本地 A2L 加载或 GET_ID+UPLOAD 拉取 A2L，显式输入符号—事件绑定与 DTO/timestamp profile，配置 `MeasurementSession`，输出 raw/physical/timestamp 及丢包/解码统计；不从未知 ECU 方言推断 envelope 或 event 绑定；
3. 显式 allowlist 下的字节读写、读回与恢复；可选展示 CAL_PAGE、segment/page 查询、segment freeze、MODIFY_BITS、COPY_CAL_PAGE 与 Seed&Key 接入。会影响目标状态的操作默认关闭，需显式命令/参数确认；Seed&Key 未注入时明确跳过；
4. cpp_demo 配置在 CTest 中复用已验证 measurement_demo 流程，验证 cpp_demo 真正支持的 A2L 上传、DAQ 与 `counter_max` 在线效果/恢复，不把不支持的页面/安全命令算作 cpp_demo 证据。

不修改 `thirdparty/`，不增加第三方依赖。代码与文档作为同一实现批次记录。

## 验证结果

- `xcp_integration_demo` Windows Release 目标编译通过；该示例专项 CTest 9/9 通过，覆盖 `--help`、cpp_demo profile 测量/临时写恢复、缺少确认/expected-original 的 fail-closed 参数校验。
- Windows VS2022 Release ON 全量构建与 CTest：524 项，523 passed、1 个已知 skip、0 failures，73.72s。skip 为 `AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`。
- `ExampleXcpIntegrationDemoCppDemo` 验证本例上传 A2L、XCPlite 多事件 DAQ 与 `counter_max` 原始写/读/恢复；在线标定对 DAQ 行为的变化由既有 `ExampleCppDemoCalibration` 独立验证。
- Linux/macOS、真实非 XCPlite ECU、Seed&Key 厂商算法和商业工具互操作均未实际运行，维持 NOT VERIFIED。

## 当前限制和待核实

通用 ECU 的 event metadata、DAQ DTO envelope、timestamp 单位、校准地址/页、访问权限和 Seed&Key 算法都由 A2L、设备文档和实测共同确定；示例不提供通用默认值。首次使用写/改页/改位/复制页前必须在台架目标、经批准 allowlist 和备份策略下评审。