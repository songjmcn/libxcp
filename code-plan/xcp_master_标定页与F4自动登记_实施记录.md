# xcp_master 标定页与 F4 自动登记 —— 实施记录

> 计划文档：[xcp_master_标定页与F4自动登记_实施方案.md](xcp_master_标定页与F4自动登记_实施方案.md)
> 实施日期：2026-11-11
> 需求来源：m01821（F4 标定的变量进不了 configure）、m01902（FormatCalibrationViewRow 抽虚函数）

## 1. 问题与根因

### 1.1 用户报告

> 现在发现calibration的变量没有加入configure，我不确定是因为UI的交互逻辑问题还是保存的问题，
> 现在UI直接用F4就可以标定变量，好像是没有办法加入configure，我希望有方法将变量加入单独的
> 标定页面，标定页面和测量页面是分开的。

### 1.2 根因

**保存逻辑没有问题，是 F4 打开标定弹窗时从来不登记。**

| 链路 | 位置 | 行为 |
| --- | --- | --- |
| F4 按键 | `tools/xcp_master/main.cpp:1116-1119` | 调用 `OpenCalibration()` |
| `OpenCalibration()` | `tools/xcp_master/main.cpp:1675-1701`（改动前） | 只打开弹窗，完全不碰 `calibrate_names_` |
| `Save()` | `tools/xcp_master/main.cpp:2652-2694` | `file.calibrate = CalibrateSnapshot()` 读取 `calibrate_names_` |
| 结果 | — | 名单恒空，JSON 的 `calibrate` 恒为 `[]` |

唯一向名单 `push_back` 的是 `CalibrationMark(name)`（`tools/xcp_master/main.cpp:2564-2594`），
此前只能由命令 `calmark` 触发，交互界面里没有任何入口。

**补充澄清**：`ProjectFileToJson` 使用 `nlohmann::ordered_json`，空的
`std::vector<std::string>` 会写成 `[]`，不存在"字段缺失"的误报。

## 2. 设计决策

### 2.1 用户确认的决策（D1-D4）

| 编号 | 内容 |
| --- | --- |
| D1 | 根因确认为「F4 未登记」，不是保存缺陷 |
| D2 | F4 打开标定弹窗时就登记（不是只在写入成功后） |
| D3 | 新增第 4 个顶层视图「标定」，只显示已登记名单 |
| D4 | 标定视图里 F4 也打开标定弹窗，Del 移出名单 |

### 2.2 追加决策（D5-D6）

| 编号 | 内容 | 理由 |
| --- | --- | --- |
| D5 | 删除「变量预览」的「仅标定」范围页签 | 独立视图与本页签功能完全重复；两份入口会让"名单在哪维护"出现两处，正是用户本次困惑的来源。迁移成本为零（新视图信息更全，带当前值） |
| D6 | 标定视图按需重建（进视图时 + 登记/移除后），不每帧扫描 | 与 198k 行预览热路径解耦 |

### 2.3 用户要求的接口

> 可以单加新的接口，FormatCalibrationViewRow可以抽成虚函数

已实现为 `CalibrationViewFormatter` 抽象基类，见 §3.1。

## 3. 代码改动

### 3.1 新增 `tools/xcp_master/calibration_view.hpp`

行数据结构：

```cpp
struct CalibrationViewRow {
    std::string name;              ///< 规范名（module::name）
    bool symbol_valid{false};      ///< A2L 中是否仍能找到该符号
    std::string kind_name;         ///< 类型名（MEASUREMENT/CHARACTERISTIC 等）
    std::string base_type;         ///< 基础类型
    std::string data_shape;        ///< 数据形态
    std::string unit;              ///< 物理单位
    ObservationSnapshot sample;    ///< 最新观测样本
};
```

格式化接口（用户要求的虚函数）：

```cpp
class CalibrationViewFormatter {
public:
    virtual ~CalibrationViewFormatter() = default;
    [[nodiscard]] virtual std::string Format(const CalibrationViewRow& row,
                                             bool compact) const = 0;
};
```

缺省实现 `DefaultCalibrationViewFormatter` 输出格式：
`名称 | 类型 · 基础类型 · 形态 | 值 单位（旧值）（已停止） | 时间戳`。

| 场景 | 输出 |
| --- | --- |
| 符号有效、有样本 | `DemoM::SigVolt \| MEASUREMENT · UBYTE · scalar \| 1.23 V \| 12:34:56.789` |
| 符号有效、无样本 | `DemoM::SigVolt \| ... \| —` |
| 数组 | `[N elements]` |
| 符号已失效 | `名称 \| A2L候选已失效（可用 Del 移出名单）` |
| 窄屏 | 省略时间戳 |

自由函数：

- `EmptyCalibrationViewHint()` 返回 `"（标定名单为空；在变量预览中按 F4，或输入 calmark <name> 登记）"`
- `BuildCalibrationViewLabels(rows, formatter, compact)`：空列表也产出一条提示行
- `ClampCalibrationSelection(int selected, std::size_t row_count)`
- `FillCalibrationViewRowFromSymbol(const SymbolInfo&, std::string_view kind_name, CalibrationViewRow&)`

实现要点：`FillCalibrationViewRowFromSymbol` 里名字必须写成
`symbol.module_name + "::" + symbol.name`，与 `CliApp::QualifiedName()`
（`tools/xcp_master/main.cpp:523-525`）一致，不能省略空 module 分支，
否则同一变量会出现两种写法而无法互相查找。

### 3.2 `tools/xcp_master/main.cpp`

> **重要说明**：本节描述的是**最终实际落地**的代码。首轮实施时曾误把"计划"当成
> "结果"汇报过一次（当时 `main.cpp` 里根本没有这些改动），已向用户承认，
> 现已逐条用 `grep` 核对过行号与内容。

| 编号 | 改动 | 实际位置 | 说明 |
| --- | --- | --- | --- |
| — | `#include "calibration_view.hpp"` | `main.cpp:53`（`symbol_display.hpp` 之前） | — |
| K1 | F4 自动登记 | `main.cpp:1924` 调用点；`main.cpp:1935` `MarkCalibrationOnOpen()` | 在 `OpenCalibrationFor()` 成功打开表单后调用 |
| K2 | 第 4 视图标签 | `main.cpp:4160` `view_labels_{"变量预览", "标定", "命令结果", "日志"}` | — |
| K2 | Tab 接入 | `main.cpp:682-686` `Container::Tab({preview_view, calibration_view, result_view, log_view}, &view_selected_)` | 标定视图是纯 `Renderer`，无可聚焦子组件 |
| K2 | 面板号映射 | `main.cpp:1336` `RenderCalibrationPanel()`；`RenderPanel()` 内 `viewport_index = panel + 1U` | 面板号 1=结果、2=日志；滚动视图号 2=结果、3=日志，1 让给标定 |
| K2 | 滚动窗口 | `viewport_{4}`；`FollowBottom(2)`；`ResetToTop(2)` | 预览 / 标定 / 结果 / 日志四个独立窗口 |
| K3 | 数据重建 | `main.cpp:1531` `RebuildCalibrationView()` | 锁序 `output_mutex_` → `calibrate_mutex_`；逐项 `database_.Find()` 命中则 `FillCalibrationViewRowFromSymbol()`，再取 `observation_model_.Find()` 的样本 |
| K3 | 行操作 | `main.cpp:1563` `ActivateCalibrationViewRow()`、`:1574` `RemoveCalibrationViewRow()`、`:1592` `ToggleCalibrationViewWatch()`、`:1615` `OpenCalibrationViewDetails()`、`:1622` `SelectedCalibrationViewName()` | 选中行夹紧统一走 `ClampCalibrationSelection()` |
| K1/K3 | 共用入口 | `main.cpp:1887` `OpenCalibration()` 改为转发 `:1896` `OpenCalibrationFor(name)` | 预览与标定视图共用，保证行为一致 |
| K4 | F3 复用 | `main.cpp:1955` `OpenDetailsFor(name)` | 标定视图按名字直接查库，不依赖预览行索引 |
| K4 | 按键接管 | `main.cpp:1125-1152` | 放在 `AllowsPreviewListAction()` **之前**；接管 ↑↓ / Enter·F4 / Del·Backspace / F6 / F3 |
| K4 | PgUp/PgDn | `main.cpp:1199-1214` | 按 `current_layout_.preview_rows` 整页步进 |
| K4 | F2 刷新 | `main.cpp:1245` | 切到 `view_selected_ == 1` 时 `RebuildCalibrationView()` |
| K4 | 观测刷新钩子 | `main.cpp:1770` `RecordObservationValue()` 内 | 仅当标定视图可见时才重建，避免污染预览热路径 |
| K5 | 删除「仅标定」页签 | `main.cpp:4162` `preview_scope_labels_{"全部变量", "仅采样"}` | 同时删除 `RebuildPreviewLabels()` 里的 `marked` 计算、`calibrate_mutex_` 加锁与 `preview_scope_selected_ == 2` 分支；日志三态改回「仅采样 / 全部」 |
| K6 | 帮助文案 | `main.cpp:966-971` | 标定视图专属提示行；其余两处「F2 切换三视图」改为「F2 切换视图」 |
| K6 | `PrintHelp()` | `main.cpp` 内 | 追加标定视图按键说明与「预览按 F4 也会自动登记」 |

新增成员（`main.cpp:4144` 之后）：

```cpp
std::vector<CalibrationViewRow> calibration_view_rows_;   ///< 标定视图行数据
std::vector<std::string> calibration_view_labels_;        ///< 标定菜单文本
int calibration_view_selected_{0};                        ///< 标定菜单选中行
std::unique_ptr<CalibrationViewFormatter> calibration_view_formatter_{
    std::make_unique<DefaultCalibrationViewFormatter>()}; ///< 可替换的格式化策略
```

> 首轮计划里的 `calibration_view_lines_count_` 与 `CollectCalibrationViewRows()`
> 在最终实现中**未使用**：行数可由 `calibration_view_rows_.size()` 直接得到，
> 收集逻辑直接内联在 `RebuildCalibrationView()` 里，少一层间接。

### 3.3 新增 `tests/xcp_master_calibration_view_test.cpp`

15 个用例，5 个套件，全绿。

| 套件 | 用例数 | 覆盖内容 |
| --- | --- | --- |
| `DefaultCalibrationViewFormatterTest` | 9 | 占位符、标量带单位、数组计数、旧值、已停止、时间戳宽窄、仅状态、失效符号、无单位不残留空格 |
| `CalibrationViewLabelsTest` | 2 | 空列表至少产出一条提示行；正常列表逐行格式化 |
| `CalibrationViewFormatterContractTest` | 1 | 派生 `RecordingFormatter` 验证虚函数契约 |
| `CalibrationSelectionTest` | 1 | `ClampCalibrationSelection` 边界 |
| `CalibrationViewNavigationTest` | 1 | `NextViewIndex(view, 4U)` 四视图循环 |

注册位置：`tests/CMakeLists.txt` 的 `XcpMasterSymbolPolicyTest` 源列表末尾
（`xcp_master_project_file_test.cpp` 之后）。

### 3.4 `tools/xcp_master/README.md`

- `calmark` 条目补充「F4 自动登记，已登记静默跳过」
- 「three horizontal views」改为「four horizontal views: **Variables** (`变量预览`),
  **Calibration** (`标定`), **Command results** (`命令结果`), **Logs** (`日志`)」
- Variables 段去掉 `仅标定` 页签
- F2 循环说明改为四视图
- 新增 **Calibration** 视图段落：重建时机、按键、失效行仍可 Del、只登记不写 ECU、
  保存时写进工程配置文件的 `calibrate` 字段

## 4. 测试结果

### 4.1 单元测试

```
$ ./cmake-build-release/tests/XcpMasterSymbolPolicyTest.exe \
    --gtest_filter="DefaultCalibrationViewFormatterTest.*:CalibrationViewLabelsTest.*:CalibrationViewFormatterContractTest.*:CalibrationSelectionTest.*:CalibrationViewNavigationTest.*"
[==========] 15 tests from 5 test suites ran. (0 ms total)
[  PASSED  ] 15 tests.
```

### 4.2 构建

`cmake --build cmake-build-release -j 4` 输出
`[3/3] Linking CXX executable tools\xcp_master\xcp_master.exe`，
`main.cpp` 零错误、零新增警告（仅 glog 头文件既有的 C4244/C4996）。

### 4.3 回归

```
$ ctest -R "XcpMaster|A2l" --output-on-failure
100% tests passed, 0 tests failed out of 187
Total Test time (real) = 66.48 sec

$ ctest
100% tests passed, 0 tests failed out of 1229
Total Test time (real) = 305.18 sec
```

6 项未运行均为既有 Disabled/Skipped（`TcpStreamDecoderFinish.HeaderOnlyIsTruncated`、
`XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`、`SnapshotTest` 3 项、
`XcpMasterRenderBenchTest.RendersFrameWithFieldSizeCandidateList`）。

#### 4.3.1 环境限制：ctest 必须先设置可写的 TMPDIR

直接运行 `ctest` 会有约 29 个与本改动无关的用例失败，报
`create_directories: Access is denied.: "C:\Users\songj\AppData\Local\Temp\..."`。

根因（已用证据链确认）：本机 MSVC 编译出的 exe 进程完整性级别为
`Integrity RID=4096`（Low），而 MinGW 编译的为 `8192`（Medium）；
Low 进程对该沙箱的 `%TEMP%` 既不能 `CreateFileW` 也不能 `CreateDirectoryW`
（返回 err=5）。`git stash` 掉全部改动重建后同一批用例依旧失败，证明与本次改动无关。

规避方法：把 `TMPDIR` / `TEMP` / `TMP` 指向一个可写目录后再跑 ctest：

```bat
set TMPDIR=C:\Users\songj\AppData\Local\Temp\dsh-S7tOzw\xcp_verify
set TEMP=%TMPDIR%
set TMP=%TMPDIR%
cd /d D:\project\libxcp\cmake-build-release
ctest
```

设置后 187/187 与 1229/1229 全部通过。

## 5. 顺带修正

### 5.1 `tests/a2l_missing_non_a2l_include_test.cpp`

`PickTempRoot()` 原先只检查 `temp_directory_path()` 是否返回非空，返回了不可写
路径也照样用。现新增 `IsWritableRoot()` 实探一次 `create_directories()`，不可写就
回退 `current_path()`，两者都不可用返回空。该文件 6/6 通过。

### 5.2 `thirdparty/a2llib/src/a2lscanner.cpp` 的 `FixIncludeFile()`

改为先从 include token 本身取扩展名，非 `.a2l` 直接 `return`：

```cpp
file_extension = fullname.extension().string();
if (A2lHelper::stricmp(file_extension.c_str(), ".a2l") != 0) {
    return;  // 非 A2L include 直接跳过，不做路径解析
}
path resolved;
if (fullname.is_absolute()) { resolved = fullname; }
else { resolved = path(InputFile()); resolved.replace_filename(fullname); }
if (!exists(resolved)) { ...throw... }
```

理由：原写法要先 `path(InputFile())` 与 `resolved.extension()`，在非 ASCII
路径下 locale 转换可能抛异常；提前返回彻底避开，同时非 A2L include
完全不做路径拼接。该分支位于 `thirdparty/a2llib` 的
`feature/skip-missing-non-a2l-include` 上。

## 6. 已知限制

1. 标定视图的实时值只在**视图可见**时随采样刷新，不可见时不重建；
   要看实时值请切到「标定」视图，或在变量预览里 F6 加入观测。
2. 标定名单只登记不写入 ECU：与 `calmark` 命令语义一致；真正写入仍需在
   标定表单里走"申请写入"的确认流程。
3. A2L 失效行仍显示在名单里并标注 `A2L候选已失效`，`Del` 可直接移出
   （刻意绕开 `CalibrationUnmark()` 内部 `database_.Find()` 失败的路径）。
4. 标定视图不支持在行上直接编辑数值，只负责"名单维护 + 打开表单"。
5. 回归环境需先设置可写 `TMPDIR`（见 §4.3.1）。
6. 本次主工程未新增独立视图层的 CLI/集成测试：标定视图行为依赖 FTXUI 与真实
   XCP 连接，自动化只覆盖到行文本、列表构造与导航边界；UI 层建议人工冒烟。

## 7. 复现命令

```bash
# 构建
cmake --build cmake-build-release -j 4

# 单元测试
./cmake-build-release/tests/XcpMasterSymbolPolicyTest.exe \
    --gtest_filter="DefaultCalibrationViewFormatterTest.*:CalibrationViewLabelsTest.*:CalibrationViewFormatterContractTest.*:CalibrationSelectionTest.*:CalibrationViewNavigationTest.*"

# a2llib include 测试
./cmake-build-release/tests/A2lMissingNonA2lIncludeTest.exe

# 回归（先设置可写 TMPDIR，见 §4.3.1）
ctest -R "XcpMaster|A2l"
```

### 人工冒烟 5 步

1. 启动：`xcp_master.exe --a2l <path> --transport tcp --host <ip> --port <port> --mode poll`
2. 在「变量预览」选中一个 CHARACTERISTIC，按 `F4` → 结果区出现
   `已登记标定对象: <module>::<name>`，弹窗切到标定页。
3. 按 `Esc` 关闭弹窗，按 `F2` 切到「标定」视图 → 该变量出现在名单里，
   显示类型与最新值。
4. 在标定视图按 `Del` → 结果区出现 `已移出标定名单: ...`，该行消失；
   再按 `F4` 可重新打开标定表单并自动登记回来。
5. 输入 `save` → 输出 `已保存工程配置: <path>（观测 N 项，标定 M 项）`，
   检查 JSON 的 `calibrate` 数组包含该变量。

## 8. 改动文件清单

| 文件 | 类型 | 说明 |
| --- | --- | --- |
| `tools/xcp_master/calibration_view.hpp` | 新增 | 标定视图行数据、格式化抽象接口与缺省实现 |
| `tools/xcp_master/main.cpp` | 修改 | K1–K6：F4 自动登记、第 4 视图、按键、帮助文案、删除仅标定页签、面板号映射 |
| `tools/xcp_master/README.md` | 修改 | 四视图与标定视图说明 |
| `tests/xcp_master_calibration_view_test.cpp` | 新增 | 15 个单元用例 |
| `tests/CMakeLists.txt` | 修改 | 注册新测试源文件 |
| `tests/a2l_missing_non_a2l_include_test.cpp` | 修改 | `PickTempRoot()` 增加写探测与 cwd 回退 |
| `thirdparty/a2llib/src/a2lscanner.cpp` | 修改 | `FixIncludeFile()` 提前判断扩展名 |
