# XCP Master UI 美化实施记录

## 需求与批准

用户批准 `code-plan/xcp_master_UI美化计划.md` 并要求实施（m05671）；变量详情采用按 Enter 打开的模态弹窗方案已由用户确认（m04574）。本次目标为三视图、四层固定布局、统一变量预览和弹窗编辑，同时保持现有命令入口及写入/DAQ 安全边界。

## 实施内容

- `tools/xcp_master/main.cpp`：主面板改为变量预览、命令结果、日志三个横向视图；顶部状态、弹性主面板、命令输入和上下文提示四层固定。移除底部 watch/stop/quit 按钮，保留 `watch`、`stop`、`quit` 命令和 F5 停止。
- 变量预览通过 A2L 元数据搜索显示 MEASUREMENT 与标量 `CHARACTERISTIC/VALUE`，支持全部候选/采样集合范围、搜索、稳定选择和不支持原因；搜索不自动访问 ECU。最新值、单位、时间、状态和 stale 信息在预览/详情中显示。
- Enter 打开模态详情，不自动读写；弹窗提供资格允许的读取、采样或标量写入操作。Esc 关闭并丢弃编辑草稿，保留列表筛选、选择及滚动位置。FTXUI Modal 隔离背景事件。DAQ 活跃时详情显式显示限制并禁用冲突操作。
- 写入及 FREE_DAQ 确认改在模态步骤中显示，复用现有 `ConfirmationGate` ticket 与 `SendIfConfirmed`：只有 Y/y 接受，N/n/Esc 取消；Dispatch 前可撤销已接受许可。resize 使用原子 `CancelBeforeDispatch()`，只在取消先于 Dispatch 生效时报告“未发送”；Dispatch 先消费许可则不阻塞渲染，并明确在途同步XCP请求不可撤回。F5 先取消再停止，Enter 不接受，其它输入被隔离；窗口低于 80×24 或确认期间缩至最小尺寸以下时取消危险确认。
- 独立复核后修复交互竞态：普通命令因队列忙被拒绝时保留输入草稿/不记历史；异步 A2L 搜索在详情/通用确认模态期间暂存结果，锁住背景候选列表，关闭后再应用；搜索框和命令行 `find` 都携带输入代际，过期结果被丢弃，不覆盖新草稿或当前列表；通用确认关闭刷新暂存结果但不撤销已接受票据。
- 最新样本模型接入手动读、POLL、DAQ、写入预读/回读：成功更新对应符号，失败保留旧值并标 stale；仅实际运行后停止的采样集合会标为 stopped。DAQ 帧不再作为重复日志累积。
- `tools/xcp_master/tui_model.hpp`：三视图分页状态按实际终端高度动态设置，`list all` 仍使用完整结果快照；布局计算覆盖 120×40、100×30、80×24 与更小尺寸。
- `tests/xcp_master_tui_model_test.cpp`：补充/更新三视图、动态页高、窗口尺寸、模态打开期间锁定背景列表、最新值/stale、搜索代际、命令草稿提交和确认门测试（覆盖 Wait 返回后 N/Esc 撤销、resize 原子 CancelBeforeDispatch、Dispatch先消费许可后的非阻塞取消）。
- `tests/CMakeLists.txt`：让本次 Fetched GoogleTest 头文件先于 `/usr/local/include/gtest` 中旧安装版本，避免测试对象与 `libgtest.a` ABI 不一致；修复后本地头/库一致。
- `tools/xcp_master/README.md`、本计划和 `code-plan/xcp_master_UI美化_界面样例.svg` 已同步三视图、模态交互、安全确认和已实施状态；示意图的确认步骤已移除类似可点击按钮的表示。

## 构建与测试

- 使用 AppleClang Release 配置 `cmake-build-release-clang`，Boost 1.86 static、FTXUI v7.0.3 和 GoogleTest 本地 FetchContent 源。
- `xcp_master` 与 `XcpMasterSymbolPolicyTest` 构建成功。
- 聚焦 CTest（XCP Master CLI、符号/标定策略、TUI 模型/确认/分页）：**42/42 通过，0 失败，0 跳过**。包含命令提交拒绝时保留草稿、搜索代际防覆盖、布局缩放和确认状态回归。
- 构建期间发现系统 `/usr/local/include/gtest` 的旧 GoogleTest 头与本地 archive 不匹配；`tests/CMakeLists.txt` 调整 `gtest`/`gtest_main` 的 system include 分类后，Clang 使用与 archive 同源的头文件，修复链接错误。

## 尚未验证

- 本轮没有执行全仓库 Release 构建或全仓库 CTest；42/42 仅代表上列聚焦测试。
- 尚无真实终端键盘/焦点/resize 人工走查、实际截图或 Slave/ECU 联测；没有执行真实标定写入。
- Windows/Linux 跨平台验收遵循此前暂缓决定，本轮只在当前 AppleClang 环境验证。
- 当前自动化主要覆盖纯模型、安全门和 CLI smoke；FTXUI 实际事件循环的端到端焦点/鼠标事件尚无专门集成测试。
