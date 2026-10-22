# XCP Master 交互职责分离实施记录

## 需求与边界

按用户授权（m14879）实现变量预览交互职责分离：移除 CLI `add` 入口；预览快捷键管理观测集合；Enter 原位展开；F3 详情只展示 A2L 元数据/静态能力；Ctrl/Command+S 进入独立标定流程。保留 `remove`、`watch`、`stop`、F5 等既有入口。未修改 SDK、协议公共 API、submodule、第三方源码或全局终端配置。DAQ 数组继续拒绝，因 ODT/DTO 映射尚未验证。

## 实施内容

- `tools/xcp_master/main.cpp`：变量列表快捷键 Ctrl/Command+A 与 F6/F4 备用键、Ctrl/Command+S 与 F3；Enter 根据 `PreviewRowAction` 展开/折叠、触发一次性读取或忽略子行。添加 Kitty CSI-u Super/Command 序列识别，并只在变量预览取得焦点时处理这些快捷键。
- 删除用户命令 `add` 的解析/帮助入口；UI 直接调用观测集合的 Add/Remove 业务逻辑，保留显式 `remove` 命令。
- `tools/xcp_master/tui_model.hpp`：`LatestObservationModel` 原子保存完整数组批次；失败保留上一批并标记 stale。`PreviewRow`/`BuildPreviewRows` 建立父行、元素行、单次读取操作行和未读占位行的明确映射；选择恢复按符号及行类型/索引匹配。
- `tools/xcp_master/main.cpp`：POLL 与一次性预览读取复用数组读取管线，按最多 8 个元素分块，保留 XCP 地址扩展并逐元素转换；只有完整向量成功后才发布，单次读取不要求变量已在观测集合，POLL 仍在提交前复核采样状态。数组样本变化会刷新展开行。
- F3 Details 改为静态元数据和只读能力信息；不触发 ECU 请求，不读取/清空 `CalibrationFormModel`。独立 Calibration 页面负责草稿、预读、写入确认和回读；运行时页权限检查只在 Calibration 流程执行。观测更新不再写入标定表单。
- `tools/xcp_master/symbol_policy.hpp`：新增仅用 A2L 静态字段计算的标定资格说明，避免 Details 为展示资格而查询 ECU。`tests/xcp_master_symbol_policy_test.cpp` 与 `tests/xcp_master_tui_model_test.cpp` 增加静态写策略、数组快照、失败原子性、预览行类型/读取动作和模态隔离回归。

## 构建与测试

- 格式检查通过：
  `clang-format --dry-run --Werror tools/xcp_master/main.cpp tools/xcp_master/tui_model.hpp tools/xcp_master/symbol_policy.hpp tests/xcp_master_tui_model_test.cpp tests/xcp_master_symbol_policy_test.cpp`
- AppleClang Release 聚焦目标构建通过：
  `cmake --build cmake-build-release-a2l --target xcp_master XcpMasterSymbolPolicyTest -j4`
- 聚焦 CTest 60/60 通过：
  `ctest --test-dir cmake-build-release-a2l -R 'XcpMaster|SymbolWritePolicy|StaticWritePolicy|LatestObservationModel|CommandHistory|BoundedLineBuffer|PreviewRows|CalibrationFormModel|VariableModalState|ConfirmationGate|A2lGolden|a2l_gen' --output-on-failure`
  覆盖 CLI smoke、A2L/标定策略、数组索引与边界、样本批次原子性、预览行操作、模态隔离及 A2L Golden/generator 测试。

## 未验证与风险

- 未运行全仓 clean Release 构建或全仓 CTest；本记录是指定目标和聚焦 CTest 的验证结果。
- 未在真实交互终端人工验证 Command/CSI-u、窗口缩放、焦点/鼠标事件；Ctrl/Alt 与 F6/F4 备用键已保留。
- 未连接真实 ECU 验证动态地址扩展、数组读性能、运行时页权限或标定写入；没有进行任何真实写入。
- Windows/Linux 构建未执行。DAQ 数组映射未验证，因此继续明确拒绝。
