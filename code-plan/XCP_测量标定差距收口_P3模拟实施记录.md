# XCP 测量与标定差距收口 P3 模拟实施记录

## 范围与批准

本记录补充并扩展此前 MODIFY_BITS 超时策略。用户在 m02285 确认将同一保守策略覆盖 DOWNLOAD、SHORT_DOWNLOAD、SET_CAL_PAGE、SET_SEGMENT_MODE、COPY_CAL_PAGE。理由是超时只表示响应未确认，不能证明对端未执行；即使设置类命令通常看似赋值操作，也可能覆盖并发期间的状态。六类命令在超时后的最终结果统一视为未知。

仅处理 Master 本地超时重试安全和 Mock 故障注入；未修改 `thirdparty/`，未验证真实 Slave/ECU 的分页标定互操作，也未将模拟结果宣称为真实设备支持。

## 修改

- `include/libxcp/xcp_error.hpp`：在已有错误分类末尾追加 `OperationOutcomeUnknown`，保留此前隐式枚举值顺序；这是此前已批准的公共 API 扩展。
- `src/xcp_error.cpp`：新分类名称映射为 `OperationOutcomeUnknown`。
- `src/command_executor.cpp`：新增超时分类，涵盖 MODIFY_BITS、DOWNLOAD、SHORT_DOWNLOAD、SET_CAL_PAGE、SET_SEGMENT_MODE、COPY_CAL_PAGE。恢复流程可发送 SYNCH，但不会重发上述原命令；恢复成功、失败或超过恢复次数时均报告 `OperationOutcomeUnknown`，保留命令/恢复诊断。DOWNLOAD 不再通过重新 SET_MTA 后重发写命令。
- `tests/xcp_master_integration_test.cpp`：Mock 支持命令已生效但响应丢失；对六类命令验证结果未知、原命令只执行一次、可观察副作用只发生一次，并检查 SYNCH/读回或模拟状态。覆盖用例：`XcpCalibration.ModifyBitsAppliedResponseLostIsNotReplayed`、`DownloadAppliedResponseLostIsNotReplayed`、`ShortDownloadAppliedResponseLostIsNotReplayed`、`SetCalPageAppliedResponseLostIsNotReplayed`、`SetSegmentModeAppliedResponseLostIsNotReplayed`、`CopyCalPageAppliedResponseLostIsNotReplayed`。
- `tests/xcp_daq_test.cpp`：将旧的 Download 超时重试预期更新为 `DownloadTimeoutReportsUnknownWithoutRetry`；即使故障注入在执行前丢响应，也断言 DOWNLOAD 不重发、结果未知。

## 验证（Windows，build-v09，Release）

- `cmake --build build-v09 --config Release --target libxcp_tests -j 1`：通过。
- 六个响应丢失后置执行用例 + Download 超时回归：7/7 通过。
- 更正恢复路径中关于 DOWNLOAD/标定命令的过期注释，并将不可达的 DOWNLOAD MTA 重建分支收窄为 UPLOAD；重新构建 `libxcp_tests` 后，`XcpCalibration|XcpDaqRecovery` 聚焦 CTest 19/19 通过。
- 最终全量 `ctest --test-dir build-v09 -C Release --output-on-failure -j 1`：最近一次在上述修订后运行，508 项中 507 通过、1 项既有 Skip、0 失败，用时 48.21s；Skip 为 `AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`。
- 更新旧 Download 预期前，第一次全量测试另外出现 A2L 性能阈值用例波动：`A2lGoldenTest.ParseLargeFileUnderBudget` 为 901ms（限值 792ms），`ParseLargeFileUnderBudget` 为 845ms（限值 792ms）；更新预期后的最终全量重跑两者通过（全程 51.61s）。这两次首跑超限是观测到的性能波动，不作为本次代码失败隐去。

## 未验证/边界

- Mock 验证的是 Master 的超时策略，不等价于真实 ECU/独立 Slave 的 MODIFY_BITS、DOWNLOAD、COPY_CAL_PAGE 或 Page Switching 互操作；该设备门禁仍开放。
- 该策略避免响应不确定时的静默二次执行；调用方仍需通过读回、重新查询或应用层对账决定后续动作。错误不表示“命令未生效”。
- 本批仅 Windows Release 验证；Linux/macOS 和非自有对手端尚未验证，P3/P4 整体仍未完成。
