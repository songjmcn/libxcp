# XCP Master 功能键预览/命令模式实施记录

## 目标与范围

按[交互职责分离计划第 8 节](<xcp_master_交互职责分离计划.md>)落实经确认的后续键位：默认变量预览列表；Tab/Shift+Tab 仅循环搜索框、范围菜单、列表；F7 显式进入命令输入；Esc 回预览列表并保留未提交草稿；F2 浏览视图不改变输入模式；F3 只读详情、F4 独立标定、F5 停止、F6 观测成员。取消 Ctrl/Command/Alt+A/S 及普通 `:`、`m`、`c` 操作入口；F6 不自动开始 `watch`。仅修改工具 UI、私有模型、相关测试和文档；不更改 SDK、公共 API、第三方源码或终端全局配置。

## 实现

- `tools/xcp_master/tui_model.hpp`：`TuiInputModeState` 管理预览/命令选择器与列表操作准入，`PreviewFocusTarget`/`NextPreviewFocusTarget` 计算预览区双向 Tab 焦点循环。
- `tools/xcp_master/main.cpp`：FTXUI 根容器改为按模式选择的两个事件子树；启动聚焦变量列表，空候选结果使用不可执行占位行。F7 从任意非模态视图聚焦命令框，Esc 退出命令模式并切回变量列表；成功提交保持命令模式，忙时保留草稿。预览模式的 Tab 只在三个预览控件中循环。F2 不改模式；F3/F4/F6 只在预览视图列表有焦点时处理，F5 保持停止优先级；弹窗遮断背景功能键。旧 Ctrl/Command/Alt+A/S 字符序列分派已移除。
- `tools/xcp_master/README.md` 和工具的上下文帮助、空观测集合提示同步功能键及模式语义。历史计划第 3–7 节仍记录旧方案；本轮实施与限制追加于第 8.6 节，四张旧版 SVG 未重绘，不能当作本轮实机截图。

## 测试与验证

- `tests/xcp_master_tui_model_test.cpp` 新增 3 项：预览三控件 Tab/Shift+Tab 环路、显式 F7/Esc 模式及草稿保留、F2 跨视图时列表操作准入。与既有命令历史、确认撤销、模态隔离、XCP Master 测试一同执行。
- AppleClang Release 现有构建目录聚焦目标：`cmake --build cmake-build-logging-release --target xcp_master XcpMasterSymbolPolicyTest -j 4`，成功。构建报告有第三方 gtest 的 `char8_t` 转换警告和重复静态库链接警告，目标均构建成功。
- `ctest --test-dir cmake-build-logging-release -C Release --output-on-failure -R 'XcpMaster|TuiInputModeStateTest|CommandHistoryTest|VariableModalStateTest|ConfirmationGateTest'`：73 项中 72 通过、1 项既有 AG=1 字节测试按其条件跳过，0 失败；包含 `ExampleXcpMasterUdp` 示例测试。
- `clang-format --dry-run --Werror tools/xcp_master/main.cpp tools/xcp_master/tui_model.hpp tests/xcp_master_tui_model_test.cpp` 与 `git diff --check` 通过。

## 未验证与人工验收建议

模型测试不是 FTXUI 真实终端键盘注入，不覆盖各 OS/终端对 F2–F7、Fn、Shift+Tab 的实际传递，也不证明重绘后每个焦点的真实行为。需分别在目标 macOS/Windows/Linux 终端人工检查：启动列表焦点、Tab 环路、从三视图按 F7、Esc 保留草稿、F2 不偷走命令焦点、搜索框可输入 `module::symbol`、空筛选焦点、模态键屏蔽、F5 取消；F6 只修改成员且 `watch` 需显式执行。未做真实 ECU 联机、全仓 clean Release 构建及全仓 CTest。若某终端截获功能键，应先核查终端/Fn 配置；不应假定 FTXUI 能越过系统级键处理。
