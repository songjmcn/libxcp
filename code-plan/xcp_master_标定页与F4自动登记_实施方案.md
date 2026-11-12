# xcp_master 标定页与 F4 自动登记 实施方案

## 1. 问题现象

用户在 `xcp_master` 中用 **F4** 打开标定表单、读回并写入标定值，随后执行 `save` 保存工程配置文件，
发现 JSON 里的 `calibrate` 数组是空的。用户不确定是 UI 交互问题还是保存逻辑问题。

## 2. 根因分析（已读代码确认）

结论：**保存逻辑没有问题，是 UI 交互少了一步登记。**

| 环节 | 代码位置 | 实际行为 |
| --- | --- | --- |
| F4 按键 | `tools/xcp_master/main.cpp:1116-1119` | 调用 `OpenCalibration()` |
| `OpenCalibration()` | `tools/xcp_master/main.cpp:1675-1701` | 只设置 `modal_symbol_info_`、`calibration_form_.Select()`、`variable_modal_.OpenCalibration()`、`modal_page_selected_ = 1`；**完全不触碰 `calibrate_names_`** |
| `variable_modal_.OpenCalibration` | `tools/xcp_master/tui_model.hpp:700-709` | 只把弹窗内部页切到 `VariableModalPage::Calibration` |
| `CalibrationMark()` | `tools/xcp_master/main.cpp:2564-2594` | 唯一往 `calibrate_names_` push 的入口；此前**只能**由命令 `calmark`（`main.cpp:2263-2267`）触发 |
| `Save()` | `tools/xcp_master/main.cpp:2652-2694` | `file.calibrate = CalibrateSnapshot();`（`main.cpp:2666-2667`） |
| `CalibrateSnapshot()` | `tools/xcp_master/main.cpp:2710-2714` | 读 `calibrate_names_` |

即：**F4 → 标定 → save 这条最自然的路径根本没往名单里写东西**，所以 `calibrate` 必然是 `[]`。

### 2.1 为什么用户会觉得"没有办法加入配置"

- 命令 `calmark <name>` 确实存在（`main.cpp:2564`），但它是隐藏命令，`PrintHelp()` 里只有一行说明，
  用户很难联想到"用 F4 标定完之后还要再单独敲一条命令"。
- 现有「仅标定」页签（`preview_scope_labels_` 第 3 项，`main.cpp:3869-3871`）虽然已经按
  `calibrate_names_` 过滤，但名单恒空，页签恒空，看不出来这件事存在。

## 3. 用户已确认的设计决策

| 编号 | 问题 | 用户选择 |
| --- | --- | --- |
| D1 | 根因判断 | 认可「F4 未登记」，按此方案设计 |
| D2 | 登记时机 | **F4 打开标定弹窗时就自动登记**（不是只在写入成功后） |
| D3 | 独立标定页形态 | **新增第 4 个顶层视图「标定」**，只显示已登记标定名单 |
| D4 | 按键分配 | 标定视图里 **F4 仍为"打开标定弹窗"**，**Del 移出名单** |

补充决策（本次设计新增，实施前说明理由）：

- **D5 删除「变量预览」里的「仅标定」页签**：D3 一旦落地，顶层「标定」视图与本页签功能完全重复；
  保留两份会让"名单在哪里维护"出现两个入口，反而容易再次困惑。范围页签回到
  `{"全部变量", "仅采样"}` 两态，语义 0=全部、1=仅采样。
  **从「仅标定」页签切到「标定」视图的迁移成本为零**，因为后者信息更全（带当前值）。
- **D6 标定视图按需重建**：标定名单通常只有几十项，但仍采用"进视图时重建"而非每帧扫描，
  与预览列表的 198k 行场景解耦，避免拖动时误伤帧率。

## 4. 详细设计

### 4.1 F4 自动登记

改动点：`CliApp::OpenCalibration()`

在现有流程之后追加一次登记调用，**复用 `CalibrationMark()`**，保证与 `calmark` 命令的准入规则
（`SymbolWriteUnsupportedReason()`）完全一致，不引入第二套判定。

- 已登记：`CalibrationMark()` 自身会输出「该变量已在标定名单: ...」，为避免每次 F4 都刷屏，
  `OpenCalibration()` 先查一次名单，只在**未登记**时才调用。
- 不可写符号：`CalibrationMark()` 输出「不能作为标定对象: <原因>」，并**不阻止**弹窗打开
  （用户仍可读回当前值，只是无法写入）。
- 登记成功后调用 `RebuildCalibrationView()`，让「标定」视图即时刷新。

### 4.2 第 4 个顶层视图「标定」

| 项 | 设计 |
| --- | --- |
| 标签 | `view_labels_ = {"变量预览", "标定", "命令结果", "日志"}` |
| 索引语义 | 0=变量预览，**1=标定**，2=命令结果，3=日志 |
| F2 循环 | 继续用 `NextViewIndex(view_selected_, view_labels_.size())`，随标签数自动变成 4 态 |
| 行来源 | `calibration_view_labels_`，由 `RebuildCalibrationView()` 从 `calibrate_names_` + `observation_model_` 生成 |
| 行文本 | `<规范名> \| <类型> \| <当前值> <单位> \| <状态时间戳>`；无样本时显示「—」，有观测且过期时标「（旧值）」 |
| 选择 | 复用 `calibration_view_selected_`（`int`） |
| 滚动 | `PanelViewport` 视图数从 3 改为 4，索引 1 = 标定视图，与 `view_selected_` 对齐 |
| 翻页 | `PgUp/PgDn` 分支对 `view_selected_ == 1` 使用标定视图行数 |
| 空名单 | 单行提示「标定名单为空；在变量预览中按 F4 或输入 calmark <name> 登记」 |

**索引对齐技巧**：`view_content = Container::Tab({preview_view, calibration_view, result_view, log_view}, &view_selected_)`，
且 `PanelViewport` 视图数同步改成 4，这样 `view_selected_` 既是 Tab 索引也是视口索引，**不需要映射表**。

### 4.3 标定视图的按键

| 按键 | 行为 |
| --- | --- |
| `↑/↓` | 移动 `calibration_view_selected_` |
| `F4` | 对当前行调用 `OpenCalibration()`（打开标定弹窗，写入走既有确认/回读流程） |
| `Del` | 对当前行调用 `CalibrationUnmark()`，随后重建视图并把选中行夹紧 |
| `F3` | 打开只读详情（与预览一致） |
| `F6` | 切换该变量的观测成员（与预览一致，便于"标定 + 对照采样"） |
| `PgUp/PgDn` | 翻页 |
| `F2` | 切换视图（全局） |

按键注册位置：`main.cpp` 的 `CatchEvent` 链中，位于 `F4` 预览分支**之前**，用
`view_selected_ == 1` 判定；`Del` 使用 `Event::Delete`。

### 4.4 受影响的帮助文案

- `"F2 切换三视图"` → `"F2 切换视图"`（出现于 `main.cpp:960-962`、`978-980`），
  因为 4 个按钮已经不会让人数不清，也更耐将来再加视图。
- 标定视图聚焦时新增一条：`"↑↓ 选择 | Enter/F4 标定 | Del 移出名单 | F6 观测 | F3 详情 | F2 视图"`。

### 4.5 `save` 与 `-c` 的行为（不变）

- `save` 仍序列化 `CalibrateSnapshot()`，只是现在名单里有内容了。
- `-c` 加载时 `PreloadFromProjectFile()` 仍**只登记不自动 watch**，与既有契约一致。
- **兼容性**：旧的 `.xcp.json`（`calibrate: []`）照常可读；新保存的文件会让「标定」视图在启动后
  立即有内容。

## 5. 实施步骤

| 步骤 | 内容 | 文件 |
| --- | --- | --- |
| K1 | `OpenCalibration()` 增自动登记 | `tools/xcp_master/main.cpp` |
| K2 | 新增 `RebuildCalibrationView()` / `CalibrationViewRows()` / `ActivateCalibrationViewRow()` | `tools/xcp_master/main.cpp` |
| K3 | `view_labels_` 扩为 4 项、`view_content` 插 `calibration_view`、`PanelViewport(4)` | `tools/xcp_master/main.cpp` |
| K4 | 按键：标定视图 `↑↓`/`F4`/`Del`/`F3`/`F6`；`PgUp/PgDn` 分支补标定视图 | `tools/xcp_master/main.cpp` |
| K5 | 删除「仅标定」范围页签（`preview_scope_labels_` 回到 2 项、`RebuildPreviewLabels()` 去掉分支 2） | `tools/xcp_master/main.cpp` |
| K6 | 帮助文案与 `PrintHelp()` 更新 | `tools/xcp_master/main.cpp` |
| K7 | 单元测试：标定视图行文本格式化函数（抽成可测的纯函数） | `tools/xcp_master/*.hpp` + `tests/xcp_master_*_test.cpp` |
| K8 | 回归 + 文档 | `tools/xcp_master/README.md`、`code-plan/` |

**K7 说明**：`RebuildCalibrationView()` 依赖 `CliApp` 内部状态无法直接单测，因此把"一行标定视图文本"
抽成头文件里的纯函数 `FormatCalibrationViewRow(...)`（放 `tools/xcp_master/symbol_display.hpp` 或
新文件 `calibration_view.hpp`），单测覆盖：无样本、有样本、数组、带单位、旧值标记、长名称。

## 6. 测试计划

### 6.1 单元测试

新增 `tests/xcp_master_calibration_view_test.cpp`（编入 `XcpMasterSymbolPolicyTest`）：

1. 无样本 → `<name> | <type> | —`
2. 有标量样本 + 单位 → 含值与单位
3. 数组样本 → `[N elements]`
4. `stale` 样本 → 含「（旧值）」
5. `stopped` 样本 → 含「（已停止）」
6. 带时间戳 → 含时间戳
7. 空名单 → 提示行文本
8. `NextViewIndex` 在 4 视图下循环：0→1→2→3→0

### 6.2 回归

- `ctest -R "XcpMaster|A2l"` 全绿。
- 全量 `ctest` 全绿。

### 6.3 人工冒烟

1. 启动 → 变量预览选中一个 CHARACTERISTIC → F4 → Esc 关闭
2. F2 切到「标定」→ 应看到该变量
3. 输入 `save` → JSON 的 `calibrate` 应含该变量
4. 重启 `-c` 该 JSON → 「标定」视图应直接显示该变量
5. 标定视图按 Del → 变量消失 → `callist` 确认

## 7. 影响面与已知限制

- `calmark`/`calunmark`/`callist`/`save` 命令语法**不变**。
- 「仅标定」范围页签被删除（D5），依赖它的自动化测试若存在需同步更新。
- 标定视图只显示**已登记**变量，不做全量 CHARACTERISTIC 浏览；全量浏览仍在「变量预览」。
- 登记不写入 ECU，F4 打开即登记意味着「看一眼也算登记」，这是 D2 的取舍；用 Del 或 `calunmark` 撤销。
