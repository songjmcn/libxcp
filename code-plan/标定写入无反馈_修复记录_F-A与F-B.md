# 标定写入「点了没反应」修复记录（F-A + F-B）

- 日期：2026-10-23
- 范围：`tools/xcp_master`（交互界面 + 标定策略纯函数层）、`tests/xcp_master_symbol_policy_test.cpp`
- 用户批准：m03996「是的，写入 5 是 OK 的，按照建议修复测试出来的问题。」（批准 F-A 与 F-B；未批准 F-C「调整从站示例 A2L 限值」）

## 0. 报障现象（m03784）

在「独立标定流程」页面对 `xcp_slave_demo_v12::cal.gain`（当前值 1）填写目标值 20 后选择「申请写入」：

- 值没有任何变化；
- 没有出现写入确认页；
- 界面上也没有任何提示。

用户在 TCP（AsamV14）下、DAQ 与 POLL 两种模式都复现，因此现象与传输无关。

## 1. 根因（三层，均工具取证）

### 1.1 业务层：目标值 20 越出 A2L 声明限值 0…10（错误码 B-15）

从站示例在注册 `gain` 时声明了限值 0…10，A2L 中表现为：

```
TYPEDEF_CHARACTERISTIC C_gain_19 "波形增益" VALUE F32 0 NO_COMPU_METHOD 0 10
```

`xcp_master` 在发送前会经 A2L 换算层校验限值，判定代码在
`thirdparty/a2l-sdk/a2lbridge/src/compu_method_eval.cpp:609-616`：

```cpp
if (symbol.have_limit) {
    double p = 0.0;
    if (AsDouble(value, &p)) {
        if (p < symbol.lower_limit || p > symbol.upper_limit) {
            return ConvError(ErrorCode::ConversionOutOfRange, symbol,
                             "物理值超出 A2L 符号限值（B-15）");
        }
    }
}
```

探针 `.tmp/probe/preflight.cpp` 复刻写入守卫后实测：

```
[limits] have_limit=1 lower=0 upper=10
[FromPhysical] 目标 5/10 → ok bytes=4 ；目标 20 → 失败: 物理值超出 A2L 符号限值（B-15）
```

因此「值不变」是正确行为——写入在**本地编码阶段**就被拒绝，从未上 wire。用户日志
`cmake-build-release/tools/xcp_master/logs/tools/xcp_master/xcp_master.INFO.20261023-181914.21736.glog`
中也确实没有 `0xF0(DOWNLOAD)`、`0xED(SHORT_DOWNLOAD)`、`0xF6(SET_MTA)`、`0xEC(GET_CAL_PAGE)`。

### 1.2 反馈层：拒绝原因写进了「看不见的地方」

`Write()` 的每个失败分支都调用了
`UpdateCalibrationState(selection, status)`（`tools/xcp_master/main.cpp`），把结论写进
`calibration_form_.status`（`tools/xcp_master/tui_model.hpp`），但**标定模态从未渲染该字段**：
原界面渲染的是 `ModalReadUnsupportedReason()` —— 一个恒为空的静态策略字符串，
显示成「读取状态：」。

同时 `Say()` 只把文本追加到 `result_lines_` / `log_lines_`（主界面面板），而模态窗口正遮住主界面，
于是「拒绝写入：物理值超出 A2L 符号限值（B-15）」这句提示对用户完全不可见。

即：`calibration_form_.status` 是**死渲染字段**（全仓 `grep "form.status"` 无渲染引用）。

### 1.3 信息层：状态文本被截断成「无法换算或编码」

编码失败分支把 `encoded.ErrorInfo().message` 丢掉，只写「目标值无法换算或编码」，
即使状态行被渲染出来，用户也无法知道到底为什么。

### 1.4 交互层：两个静默分支

`SubmitCalibrationWrite()` 的两处提前返回只调 `Say()`：

- 未选中变量：`if (!modal_symbol_info_.has_value()) return;` —— 完全静默；
- 未选 VALUE/曲线点或目标值为空：「请选择有效标量VALUE或曲线点…」同样只进主界面面板。

## 2. 改动清单

### 2.1 `tools/xcp_master/calibration_policy.hpp`

新增两个纯函数（可单测，无 UI 依赖）：

| 函数 | 行 | 作用 |
| --- | --- | --- |
| `CalibrationLimitText(const a2l::SymbolInfo&)` | :162 | 渲染 A2L 符号限值，形如 `0 … 10`；未声明限值时返回「A2L 未声明限值」，避免界面把「未声明」读成 `0` |
| `CalibrationTargetLimitHint(const a2l::SymbolInfo&, std::string_view)` | :179 | 目标文本越出限值时返回提示（含 B-15），与换算层判定口径一致：只在 `have_limit` 且文本可解析为有限数值时比较，文本型/非法输入不提示 |

新增包含：`<cmath>`、`<cstdlib>`、`<iomanip>`、`<sstream>`。

### 2.2 `tools/xcp_master/main.cpp`

1. **标定模态渲染**（:576-645）：
   - 新增「允许范围：」行（`CalibrationLimitText`）；
   - 新增「状态：」行渲染 `calibration_form_.status`（青色）——这是本次修复的核心；
   - 目标值越界时插入红色提示行（`CalibrationTargetLimitHint`），随输入即时刷新；
   - 原静态「读取状态：」改为「读取策略：」并在空串时显示「可用」，避免展示空白；
   - 渲染改为先构建 `std::vector<Element> lines` 再 `vbox(std::move(lines))`，
     使越界提示行只在需要时出现（不插入空元素，避免容器出现空子节点）；
   - 渲染前在 `output_mutex_` 保护下取 `current`/`status`/`target` 快照：
     这三个字段由工作线程改写（`UpdateCalibrationState`），此前渲染器直接读取属于数据竞争。
2. **`Write()` 编码失败分支**（:2500-2510）：状态文本由「目标值无法换算或编码」改为
   `"目标值不可编码：" + encoded.ErrorInfo().message`，把 B-15 等真实原因带到界面上。
3. **`SubmitCalibrationWrite()`**（:1736-1772）：两个静默分支补 `SetCalibrationFormStatus(...)`
   （「尚未选择变量；请先在列表中选中对象」「请选择有效标量VALUE或曲线点，并填写目标物理值」），
   保留原有 `Say()`。
4. **新增 `SetCalibrationFormStatus(const std::string&)`**（:1653-1663）：界面线程的本地反馈入口，
   在 `output_mutex_` 保护下改写 `calibration_form_.status`，随后 `screen_->PostEvent(Custom)` 触发重绘
   （锁外调用，避免持锁重绘）。

### 2.3 `tests/xcp_master_symbol_policy_test.cpp`

新增 2 个用例：

- `RendersDeclaredLimitRangeText`：`have_limit=true, [0,10]` → `"0 … 10"`；`have_limit=false` → `"A2L 未声明限值"`；
- `HintsTargetValueBeyondDeclaredLimit`：`20` 与 `-1` 均提示且含「超出 A2L 允许范围」「B-15」；
  `5`/`0`/`10` 不提示；`abc`/`5x`/空串不提示；`have_limit=false` 时不提示。

### 2.4 `examples/xcp_slave_demo_v12/README.md`（文档）

在「变量模型」后新增「标定段写入的 A2L 声明限值」表（`gain 0…10`、`offset -100…100`、
`limit 0…1000`、`run 0…1`、`corrections -5…5`，均以
`examples/xcp_slave_demo_v12/slave_registry.cpp:88-94` 的注册参数为准），
并说明「把 `cal.gain` 写成 20 会被主站本地拒收（限值约束，不是链路故障）」，
避免同类误判再次发生。

## 3. 测试结果

| 项 | 命令 | 结果 |
| --- | --- | --- |
| 目标构建 | `cmake --build cmake-build-release --target xcp_master XcpMasterSymbolPolicyTest -j 4` | 成功（仅既有 ld/gtest 警告） |
| 新增用例 | `./tests/XcpMasterSymbolPolicyTest --gtest_filter='XcpMasterSymbolPolicyTest.Renders*:XcpMasterSymbolPolicyTest.Hints*'` | 2 passed |
| 策略测试目标 | `./tests/XcpMasterSymbolPolicyTest` | **102 passed**（原 100 + 新增 2） |
| 全量回归 | `ctest --test-dir cmake-build-release -j 4` | **968 项 / 3 failed**（54.79 s），3 项与本次改动无关：`a2l_perf_gen`（需 Python 3.10+）、`ParseLargeFileUnderBudget`（依赖上者 fixture，Not Run）、`ExampleXcpIntegrationDemoCppDemo` |

### 3.1 未能自动验证的部分

`xcp_master` 的交互界面要求真实 TTY（`IsInteractiveTerminal()`），本环境禁止 `openpty`
（`script: openpty: Operation not permitted`），无法构造伪终端，因此**模态渲染未做端到端自动化验证**。
纯逻辑已由上述单测覆盖，渲染层仅做编译验证。

建议用户在带终端的环境按以下步骤复核：

1. 重建：`cmake --build cmake-build-release --target xcp_master -j 4`；
2. 启动从站：`./cmake-build-release/examples/xcp_slave_demo_v12/xcp_slave_demo_v12 --transport tcp --port 5559`；
3. 启动主站：`./cmake-build-release/tools/xcp_master/xcp_master --a2l <生成的 a2l> --host 127.0.0.1 --port 5559 --mode poll --transport tcp --tcp-profile asam-v1.4`；
4. 进入标定页后应看到「允许范围：0 … 10」；
5. 输入 `20` → 立即出现红色「目标值 20 超出 A2L 允许范围（0 … 10），写入会被拒绝（B-15）」，按「申请写入」后「状态：」显示同样的拒绝原因；
6. 输入 `5` → 无红色提示，按「申请写入」后进入确认页，按 `Y` 后「状态：写入命令成功，回读已完成」且「当前值」更新为 5。

## 4. 未做项与残留

| 项 | 说明 |
| --- | --- |
| F-C：调整从站示例 A2L 限值 | 未批准，未做。`gain` 仍声明 0…10；如需写入更大值应先改从站注册参数 |
| TUI 端到端测试 | 无伪终端支持，未新增；渲染正确性只能人工复核 |
| 「未选中变量」分支的状态文本 | 该分支 `calibration_form_.Snapshot().name` 为空，状态行会显示但不出现在有效表单上，属边界提示 |
| 只读符号的「写入资格」文案 | 仍为静态策略结论，未与限值联动（限值属动态输入，由独立提示行承担） |

## 5. 回滚

三处改动相互独立，按文件回滚即可：

```bash
git checkout -- tools/xcp_master/calibration_policy.hpp \
                tools/xcp_master/main.cpp \
                tests/xcp_master_symbol_policy_test.cpp
```

回滚后行为回到：越界目标值静默无反馈，`calibration_form_.status` 不被渲染。