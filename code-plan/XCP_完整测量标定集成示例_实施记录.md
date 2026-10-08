# 完整测量与标定集成示例 — 实施记录

## 目标和设计

用户要求提供后续集成人员可参考的完整测量/标定示例，并选择“通用集成示例 + cpp_demo 可运行配置”（m08932）、批准设计（m08944）。本示例呈现仓库 `libxcp` 现有接口和安全使用方式，不声称完整 XCP 1.3.0，也不把本地 XCPlite 验证当作真实 ECU 互操作证据。

设计记录：`code-plan/XCP_完整测量标定集成示例_实施计划.md`。

## 交付内容

- `examples/xcp_integration_demo/xcp_integration_demo.cpp`：UDP/XCP 会话与能力查询；本地 A2L 或 GET_ID+UPLOAD；显式 symbol→event 绑定；通用 DTO/time profile 或 XCPlite runtime profile；DAQ 启停与 raw/physical/timestamp/统计输出；CAL/PAG 查询、临时 SET_CAL_PAGE 与 segment freeze；临时原始写/读回/恢复；MODIFY_BITS；COPY_CAL_PAGE；Seed&Key 回调接入点。
- `examples/xcp_integration_demo/README.md`：Windows 构建、cpp_demo 一键运行、通用 ECU 参数说明、A2L 上传、标定命令、安全前置条件及能力边界。
- `examples/xcp_integration_demo/CMakeLists.txt` 与 `examples/CMakeLists.txt`：添加目标、依赖、liba2l DLL 部署和 CTest。

## 安全行为

- 通用测量必须显式提供 DTO envelope 与 symbol→event/event metadata，不推断未知 Slave 方言；通道 0 合法。
- 写操作需要显式 opt-in。DOWNLOAD 和 MODIFY_BITS 还必须提供匹配长度的 `--expected-original`，按现场已知原始字节做 compare-before-write；每次运行最多一个状态变更操作。
- 临时 DOWNLOAD 执行前备份原字节，成功后读回、恢复并复读。超时后先查询：仍为原值不重试；已观察到目标值才尝试一次恢复；其他状态报告未知，不盲目重放。
- SET_CAL_PAGE、segment freeze 先查询原状态，临时变更后恢复；COPY_CAL_PAGE 不自动回滚，要求操作者提供外部备份/恢复策略。
- Seed&Key 不含占位密钥/算法，也不会因 `ERR_ACCESS_LOCKED` 自动尝试解锁；集成者应在获授权后注入厂商实现。
- cpp_demo profile 保留运行目录已有文件，建议使用隔离的 `--run-dir`；本地示例只验证 cpp_demo 支持的行为。STIM、Flash Programming、NVM 持久化不在范围内。

## 构建与测试

- Windows VS2022 Release，`build-v09`，A2L + XCPLite + examples + tests ON：目标构建成功。
- `ctest --test-dir build-v09 -C Release -R ExampleXcpIntegrationDemo --output-on-failure`：9/9 passed。覆盖 help、cpp_demo 本地 profile、缺少写确认、缺少 expected-original，以及 SET_CAL_PAGE/freeze/COPY_CAL_PAGE/MODIFY_BITS 缺少确认时 fail-closed。
- 全量 `cmake --build build-v09 --config Release --parallel 1` 和 CTest：524 项，523 passed，1 known skip，0 failures，73.72s。唯一 skip：`AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`。
- `ExampleXcpIntegrationDemoCppDemo` 验证本例上传 A2L、配置/运行多事件 DAQ、`counter_max` 原始写/读/恢复。`ExampleCppDemoCalibration` 另行验证在线校准造成 DAQ 可观察变化再恢复。
- `git diff --check` 未报告 whitespace error；有 Git 的 LF→CRLF 提示。

## 未验证与限制

本次只在 Windows + UDP + vendored XCPlite/cpp_demo 环境验证。Linux/macOS、真实非 XCPlite ECU、目标 ECU 的 CAL/PAG 页面/安全行为、厂商 Seed&Key 算法、商业工具互操作未验证。所有通用 ECU 地址、事件、时间单位、资源权限、页面语义和恢复流程必须由目标 A2L、Slave capability/runtime query、设备文档及台架验证确定。